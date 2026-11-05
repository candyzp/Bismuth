#pragma once

/*
    Sharing render targets between bgfx's native backend (Vulkan / Metal) and
    the host application's OpenGL context.

    Bismuth renders its layers with Vulkan/Metal into shared RGBA8 textures,
    then Geometry Dash's GL context composites them at the right z-order.

        bgfx (Vulkan/Metal)                    host OpenGL
        -------------------                    -----------
        beforeNativeFrame()  <- GL finished reading last frame
        bgfx::frame()        -> render shared targets
        afterNativeFrame()   -> publish to GL
                                               beforeHostRead()  wait for native
                                               composite targets
                                               afterHostRead()   release to native

    Implementations: InteropVulkan.cpp (Windows/Linux opaque handles, Android
    AHardwareBuffer) and InteropMetal.mm (IOSurface).
*/

#include <core/Types.hpp>
#include <bgfx/bgfx.h>
#include "HostGL.hpp"
#include <string>
#include <vector>

namespace bismuth::gfx {

struct SharedTarget {
    bgfx::TextureHandle     texture     = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;

    GLuint glTexture = 0;
    GLenum glTarget  = gl::TEXTURE_2D; ///< TEXTURE_RECTANGLE on macOS (IOSurface)
    u32    width     = 0;
    u32    height    = 0;
    bool   flipY     = false;          ///< true when the native framebuffer origin is top-left in GL texture space

    void*  platform  = nullptr;        ///< implementation data

    bool isValid() const { return bgfx::isValid(framebuffer) && glTexture != 0; }
};

class Interop {
public:
    virtual ~Interop() = default;

    /// Creates the interop for the active bgfx renderer. Must be called after
    /// bgfx::init, with the host GL context current. Returns null if unsupported.
    static UPtr<Interop> create(const HostGLInfo& host, std::string& error);

    /// Called before bgfx::init for the native backend; lets the implementation
    /// configure bgfx (e.g. pick the Vulkan device matching the GL device).
    static bool prepareInit(bgfx::RendererType::Enum type, const HostGLInfo& host, std::string& error);

    /// Creates a shared render target. May run bgfx::frame() internally.
    virtual bool createTarget(SharedTarget& target, u32 width, u32 height) = 0;
    virtual void destroyTarget(SharedTarget& target) = 0;

    /// Retires a set of targets together. Backends with asynchronous GPU work
    /// can synchronize once for the whole batch before releasing shared memory.
    virtual void destroyTargets(const std::vector<SharedTarget*>& batch) {
        for (auto* target : batch) {
            if (target)
                destroyTarget(*target);
        }
    }

    virtual void beforeNativeFrame() = 0;
    virtual void afterNativeFrame() = 0;
    virtual void beforeHostRead() = 0;
    virtual void afterHostRead() = 0;

    /// e.g. "Vulkan (GL_EXT_memory_object_win32, semaphores)"
    virtual const char* describe() const = 0;

protected:
    /// Registered textures (GL sync operations need the full list).
    // Up to 24 composite groups plus the optional overdraw target may be registered.
    SharedTarget* targets[32] = {};
    void registerTarget(SharedTarget* target);
    void unregisterTarget(SharedTarget* target);
};

/// Draws shared targets into the host's current framebuffer with premultiplied
/// "over" blending. Uses only the host GL context.
class Compositor {
public:
    ~Compositor();

    bool init(const HostGLInfo& host);
    void destroy();

    /// Draws `target` over the current viewport. Saves and restores GL state.
    void draw(const SharedTarget& target);

private:
    struct Program {
        GLuint program = 0;
        GLint  texture = -1;
        GLint  uvScale = -1;
        GLint  flipY = -1;
    };

    const HostGLInfo* host = nullptr;
    Program program2D;
    Program programRect;
    GLuint vertexBuffer = 0;
};

} // namespace bismuth::gfx
