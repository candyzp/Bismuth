#include "Device.hpp"
#include "../diagnostics/Diagnostics.hpp"

#include <chrono>

#include <bgfx/platform.h>
#include <bx/allocator.h>
#include <cstdarg>
#include <cstdio>

#if defined(__ANDROID__)
#   include <EGL/egl.h>
#endif

#if defined(__APPLE__)
#   include <TargetConditionals.h>
#endif

namespace bismuth::gfx {

UPtr<Interop> createVulkanInterop(const HostGLInfo& host, std::string& error);
bool prepareVulkanInit(const HostGLInfo& host, std::string& error);
#if defined(__APPLE__)
UPtr<Interop> createMetalInterop(const HostGLInfo& host, std::string& error);
void* createHeadlessMetalLayer();
void releaseHeadlessMetalLayer(void* layer);
#endif

UPtr<Interop> Interop::create(const HostGLInfo& host, std::string& error) {
    switch (bgfx::getRendererType()) {
#if defined(__APPLE__)
    case bgfx::RendererType::Metal:  return createMetalInterop(host, error);
#else
    case bgfx::RendererType::Vulkan: return createVulkanInterop(host, error);
#endif
    default:
        error = "no interop for this renderer";
        return nullptr;
    }
}

bool Interop::prepareInit(bgfx::RendererType::Enum type, const HostGLInfo& host, std::string& error) {
#if !defined(__APPLE__)
    if (type == bgfx::RendererType::Vulkan)
        return prepareVulkanInit(host, error);
#endif
    (void)type; (void)host; (void)error;
    return true;
}

const char* backendName(Backend backend) {
    switch (backend) {
    case Backend::Vulkan: return "Vulkan";
    case Backend::Metal:  return "Metal";
    case Backend::OpenGL: return "OpenGL";
    default:              return "none";
    }
}

//// BGFX CALLBACK ////

namespace {

/*
    bgfx's default callback aborts the process on fatal errors. Inside a game we'd
    rather log and let Bismuth fall back, so fatal errors only set a flag.
*/
class Callback final : public bgfx::CallbackI {
public:
    bool fatalRaised = false;

    void fatal(const char* filePath, uint16_t line, bgfx::Fatal::Enum code, const char* str) override {
        fatalRaised = true;
        logf(LogLevel::Error, "bgfx fatal error %d at %s:%d: %s", (int)code, filePath, line, str);
    }

    void traceVargs(const char* filePath, uint16_t line, const char* format, va_list args) override {
        (void)filePath; (void)line;
        char buffer[1024];
        std::vsnprintf(buffer, sizeof(buffer), format, args);
        size_t len = std::strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n')
            buffer[len - 1] = 0;
        logf(LogLevel::Debug, "bgfx: %s", buffer);
    }

    void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerEnd() override {}
    uint32_t cacheReadSize(uint64_t) override { return 0; }
    bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
    void cacheWrite(uint64_t, const void*, uint32_t) override {}
    void screenShot(const char*, uint32_t, uint32_t, uint32_t, const void*, uint32_t, bool) override {}
    void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
    void captureEnd() override {}
    void captureFrame(const void*, uint32_t) override {}
};

Callback s_callback;

bgfx::Init makeInit(bgfx::RendererType::Enum type) {
    bgfx::Init init;
    init.type = type;
    init.callback = &s_callback;
    init.resolution.reset = BGFX_RESET_NONE;
    init.debug = false;
    // The CPU culling path uploads its vertices through a dynamic buffer, not
    // transient buffers, so the default transient sizes are plenty.
    return init;
}

/// bgfx::init that never leaves bgfx half-initialized on failure.
bool tryInit(const bgfx::Init& init, bgfx::RendererType::Enum type) {
    s_callback.fatalRaised = false;
    if (!bgfx::init(init)) {
        // bgfx already cleaned up after itself.
        bgfx::clearHostIntegration();
        return false;
    }
    if (s_callback.fatalRaised || bgfx::getRendererType() != type) {
        bgfx::shutdown();
        bgfx::clearHostIntegration();
        return false;
    }
    return true;
}

} // namespace

//// DEVICE ////

Device& Device::get() {
    static Device device;
    return device;
}

bool Device::init(const DeviceOptions& options, std::string& report) {
    if (isInitialized())
        return true;

    diag::checkpoint("Device::init started");
    diag::note("Attempting graphics backend initialization on Geometry Dash's current GL context.");
    report.clear();
    hostLost = false;

    diag::updatePhase("Device::init: query host OpenGL context");
    if (!queryHostGL(host)) {
        report = "Couldn't access Geometry Dash's OpenGL context.";
        diag::note(report.c_str());
        return false;
    }

    logf(LogLevel::Info, "Bismuth: host GL: %s | %s | %s", host.version.c_str(), host.renderer.c_str(), host.vendor.c_str());
    const auto hostInfo = std::string("Host GL: ") + host.version + " | " + host.renderer + " | " + host.vendor;
    diag::note(hostInfo.c_str());

    // bgfx runs on the calling (GL) thread: calling renderFrame before init
    // selects single-threaded mode.
    diag::updatePhase("Device::init: bgfx renderFrame setup");
    bgfx::renderFrame();

    bool ok = false;
    if (options.backend != BackendPreference::OpenGL) {
        diag::checkpoint("Device::init: attempting native Metal/Vulkan backend");
        ok = initNative(report);
    }
    if (!ok && options.backend != BackendPreference::Native) {
        diag::checkpoint("Device::init: native backend unavailable; attempting host OpenGL fallback");
        ok = initHostGL(report);
    }

    if (!ok) {
        backend = Backend::None;
        diag::note(report.c_str());
        diag::checkpoint("Device::init failed: no graphics backend initialized");
        return false;
    }

    chooseCullingPath(options.culling);
    logf(LogLevel::Info, "Bismuth: %s", describe().c_str());
    diag::setRendererState(backendName(backend),
        cullingPath == CullingPath::Gpu ? "GPU culling" : "CPU culling");
    diag::checkpoint("Device::init completed successfully");
    return true;
}

bool Device::initNative(std::string& report) {
#if defined(__APPLE__)
    const auto type = bgfx::RendererType::Metal;
    const Backend nativeBackend = Backend::Metal;
#else
    const auto type = bgfx::RendererType::Vulkan;
    const Backend nativeBackend = Backend::Vulkan;
#endif
    const char* name = backendName(nativeBackend);

    diag::updatePhase("Device::initNative: prepare native backend");
    std::string error;
    if (!Interop::prepareInit(type, host, error)) {
        report += std::string(name) + ": " + error + "\n";
        return false;
    }

    auto init = makeInit(type);
    // BGFX's Metal backend still requires a CAMetalLayer during initialization,
    // even when every real draw targets an offscreen shared texture. Give it an
    // unattached layer so it can create its device and its private default target
    // without taking ownership of Geometry Dash's EAGL drawable.
    init.platformData = bgfx::PlatformData();
#if defined(__APPLE__)
    void* headlessMetalLayer = createHeadlessMetalLayer();
    if (!headlessMetalLayer) {
        report += "Metal: couldn't create the headless CAMetalLayer\n";
        return false;
    }
    init.platformData.nwh = headlessMetalLayer;
#endif
    init.resolution.width = 16;
    init.resolution.height = 16;

    diag::updatePhase("Device::initNative: bgfx initialization on Metal/Vulkan");
    const bool initialized = tryInit(init, type);
#if defined(__APPLE__)
    // BGFX retains the layer when its Metal swap chain is created.
    releaseHeadlessMetalLayer(headlessMetalLayer);
#endif
    if (!initialized) {
        report += std::string(name) + ": initialization failed\n";
        return false;
    }

    const auto* caps = bgfx::getCaps();
    if (!(caps->supported & BGFX_CAPS_TEXTURE_BLIT)) {
        // Not strictly required, but a strong hint that the driver is too limited.
        logf(LogLevel::Warn, "Bismuth: %s lacks texture blit support", name);
    }

    diag::updatePhase("Device::initNative: create host interop and compositor");
    interop = Interop::create(host, error);
    if (!interop || !compositor.init(host)) {
        interop = nullptr;
        compositor.destroy();
        bgfx::shutdown();
        bgfx::clearHostIntegration();
        report += std::string(name) + ": sharing with OpenGL unavailable: " + (error.empty() ? "compositor failed" : error) + "\n";
        return false;
    }

    backend = nativeBackend;
    rendererType = type;
    return true;
}

bool Device::initHostGL(std::string& report) {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    const auto type = bgfx::RendererType::OpenGLES;
    if (!host.isES) {
        report += "OpenGL: unexpected desktop GL context\n";
        return false;
    }
#   if defined(__ANDROID__)
    if (host.major < 3) {
        report += "OpenGL: needs OpenGL ES 3.0 (this device: " + host.version + ")\n";
        return false;
    }
#   endif
#else
    const auto type = bgfx::RendererType::OpenGL;
    if (host.isES || host.isANGLE) {
        report += "OpenGL: the game is running on OpenGL ES/ANGLE (e.g. ReviANGLE), which bgfx's desktop GL backend can't use\n";
        return false;
    }
    if (!host.atLeast(2, 1)) {
        report += "OpenGL: needs OpenGL 2.1 (this device: " + host.version + ")\n";
        return false;
    }
#endif

    diag::updatePhase("Device::initHostGL: prepare Geometry Dash OpenGL context");
    GLStateGuard guard(host);

    auto init = makeInit(type);
    init.platformData.context = host.nativeContext;
    init.platformData.nwh = host.nativeWindow;   // HWND, required by bgfx's WGL code
    init.platformData.ndt = host.nativeDisplay;  // X11 display (tests)
    init.resolution.width = kHostResolution;
    init.resolution.height = kHostResolution;

    bgfx::glSetHostFramebuffer((u32)guard.snapshot().drawFramebuffer);

    diag::updatePhase("Device::initHostGL: bgfx initialization on host OpenGL");
    if (!tryInit(init, type)) {
        report += "OpenGL: initialization failed\n";
        return false;
    }

    backend = Backend::OpenGL;
    rendererType = type;
    return true;
}

void Device::chooseCullingPath(CullingPreference preference) {
    const auto* caps = bgfx::getCaps();
    const bool computeCaps = (caps->supported & BGFX_CAPS_COMPUTE) && (caps->supported & BGFX_CAPS_DRAW_INDIRECT);

    bool gpuPossible = computeCaps;
    cullingReason.clear();

    if (backend == Backend::OpenGL) {
        // bgfx decides compute support at compile time for GLES, check the real context.
        // The GPU path's shaders exist for desktop GL 4.3 only.
        if (host.isES) {
            gpuPossible = false;
            cullingReason = "no GPU culling shaders for OpenGL ES";
        } else if (!host.hasComputeShaders) {
            gpuPossible = false;
            cullingReason = "OpenGL 4.3 compute shaders unavailable";
        } else if (host.maxVertexStorageBlocks < 3) {
            gpuPossible = false;
            cullingReason = "vertex shader storage buffers unavailable";
        }
    } else if (!computeCaps) {
        cullingReason = "compute or indirect drawing unsupported";
    }

    if (preference == CullingPreference::Cpu) {
        gpuPossible = false;
        cullingReason = "forced by settings";
    } else if (preference == CullingPreference::Gpu && !gpuPossible) {
        logf(LogLevel::Warn, "Bismuth: GPU culling requested but unavailable (%s)", cullingReason.c_str());
    }

    cullingPath = gpuPossible ? CullingPath::Gpu : CullingPath::Cpu;
}

void Device::shutdown() {
    if (!isInitialized())
        return;

    if (isHostContextLost())
        hostLost = true;

    // Host mode textures are overrides of GD's (flagged shared, so bgfx never
    // deletes the GL texture); the others are bgfx's own.
    hostTextures.destroyAll();
    spriteRenderer.destroy(hostLost);
    spriteRendererFailed = false;
    generation++;

    if (hostLost) {
        // GL objects died with the old context; never delete names that may
        // now belong to the new context.
        compositor = Compositor();
    } else {
        compositor.destroy();
    }

#if defined(__ANDROID__)
    if (hostLost && backend == Backend::OpenGL) {
        // bgfx's GL backend would delete its objects in the *new* context.
        // GL calls without a current context are ignored on Android.
        EGLDisplay display = eglGetCurrentDisplay();
        EGLSurface draw = eglGetCurrentSurface(EGL_DRAW);
        EGLSurface read = eglGetCurrentSurface(EGL_READ);
        EGLContext context = eglGetCurrentContext();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        bgfx::shutdown();
        eglMakeCurrent(display, draw, read, context);
        bgfx::clearHostIntegration();
        backend = Backend::None;
        return;
    }
#endif

    if (hostLost) {
        // The interop's GL objects are gone; only release the native side.
        if (interop) interop.release(); // intentionally leaked: its destructor touches GL
    }
    interop = nullptr;

    if (backend == Backend::OpenGL && !hostLost) {
        GLStateGuard guard(host);
        bgfx::shutdown();
    } else {
        bgfx::shutdown();
    }

    bgfx::clearHostIntegration();
    backend = Backend::None;
    rendererType = bgfx::RendererType::Noop;
}

bool Device::isHostContextLost() {
    if (!isInitialized())
        return false;
    if (!hostLost && currentNativeGLContext() != host.nativeContext)
        hostLost = true;
    return hostLost;
}

void Device::setHostViewRect(bgfx::ViewId view) {
    GLint viewport[4] = {};
    glf().glGetIntegerv(gl::VIEWPORT, viewport);
    // bgfx flips Y against the back buffer height: y_gl = H - h - y_bgfx.
    const u32 y = kHostResolution - (u32)viewport[3] - (u32)viewport[1];
    bgfx::setViewRect(view, (u16)viewport[0], (u16)y, (u16)viewport[2], (u16)viewport[3]);
}

void Device::flushHost() {
    diag::updatePhase("Device::flushHost: bgfx host-frame submission");
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };

    auto t0 = Clock::now();
    Clock::time_point t1, t2;
    {
        GLStateGuard guard(host);
        t1 = Clock::now();
        bgfx::glSetHostFramebuffer((u32)guard.snapshot().drawFramebuffer);
        lastFrameNumber = bgfx::frame();
        t2 = Clock::now();
    }
    auto t3 = Clock::now();

    flushStats.count++;
    flushStats.saveMs += ms(t1 - t0);
    flushStats.bgfxMs += ms(t2 - t1);
    flushStats.restoreMs += ms(t3 - t2);

    accumulateStats();
}

void Device::submitNative() {
    diag::updatePhase("Device::submitNative: prepare shared frame");
    interop->beforeNativeFrame();
    diag::updatePhase("Device::submitNative: bgfx::frame on native backend");
    lastFrameNumber = bgfx::frame();
    diag::updatePhase("Device::submitNative: publish native frame to host GL");
    interop->afterNativeFrame();
    accumulateStats();
}

void Device::accumulateStats() {
    const auto* stats = bgfx::getStats();
    if (stats && stats->gpuTimerFreq > 0 && stats->gpuTimeEnd > stats->gpuTimeBegin)
        frameGpuTimeMs += double(stats->gpuTimeEnd - stats->gpuTimeBegin) * 1000.0 / double(stats->gpuTimerFreq);
}

HostSpriteRenderer* Device::getHostSpriteRenderer() {
    if (!isHostMode() || cullingPath != CullingPath::Cpu || spriteRendererFailed)
        return nullptr;
    if (!spriteRenderer.isInitialized() && !spriteRenderer.init(host)) {
        logf(LogLevel::Warn, "Bismuth: direct OpenGL sprite drawing unavailable, using bgfx");
        spriteRenderer.destroy(false);
        spriteRendererFailed = true;
        return nullptr;
    }
    return &spriteRenderer;
}

void Device::setGpuTiming(bool enabled) {
    if (isHostMode())
        bgfx::glSetTimerQueries(enabled);
}

void Device::endGameFrame() {
    lastGpuTimeMs = frameGpuTimeMs;
    frameGpuTimeMs = 0.0;
    lastFlushStats = flushStats;
    flushStats = {};
}

std::string Device::describe() const {
    if (!isInitialized())
        return "Bismuth renderer inactive";

    std::string text = std::string(backendName(backend));
    if (interop)
        text = interop->describe();
    else if (backend == Backend::OpenGL)
        text += std::string(" (") + (host.isES ? "ES " : "") + std::to_string(host.major) + "." + std::to_string(host.minor) + ", in-context)";

    text += cullingPath == CullingPath::Gpu ? ", GPU culling" : ", CPU culling";
    if (cullingPath == CullingPath::Cpu && !cullingReason.empty())
        text += " (" + cullingReason + ")";
    return text;
}

} // namespace bismuth::gfx
