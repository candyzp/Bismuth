/*
    Metal <-> host OpenGL sharing through IOSurface.

      macOS: IOSurface -> MTLTexture (newTextureWithDescriptor:iosurface:plane:)
                       -> GL_TEXTURE_RECTANGLE (CGLTexImageIOSurface2D)
      iOS:   CVPixelBuffer (IOSurface backed) -> CVMetalTextureCache / CVOpenGLESTextureCache

    Synchronization follows Apple's guidance for mixing Metal and OpenGL on a
    shared IOSurface: OpenGL work is flushed before Metal renders, and Metal's
    command buffer is scheduled before OpenGL samples the surface.

    This file is compiled without ARC (like bgfx's Metal backend).
*/

#include "Interop.hpp"
#include <bgfx/platform.h>

#include <TargetConditionals.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <IOSurface/IOSurfaceRef.h> // IOSurface.h is macOS-only
#import <CoreVideo/CoreVideo.h>

#if TARGET_OS_IPHONE
#   import <OpenGLES/EAGL.h>
#   import <OpenGLES/ES2/gl.h>
#   import <OpenGLES/ES2/glext.h>
#else
#   import <OpenGL/OpenGL.h>
#   import <OpenGL/CGLIOSurface.h>
#   import <OpenGL/gl.h>
#endif

#include <string>

namespace bismuth::gfx {

// BGFX's Metal backend insists on a CAMetalLayer even when Bismuth renders
// exclusively into offscreen textures shared with Geometry Dash's EAGL context.
// This layer is intentionally unattached: BGFX uses its pixel format and device
// for initialization, while the actual level frames use CVPixelBuffer targets.
void* createHeadlessMetalLayer() {
    CAMetalLayer* layer = [[CAMetalLayer alloc] init];
    if (!layer)
        return nullptr;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.drawableSize = CGSizeMake(16, 16);
    layer.framebufferOnly = NO;
    return (void*)layer; // +1 ownership is transferred to Device::initNative.
}

void releaseHeadlessMetalLayer(void* layer) {
    if (layer)
        [(CAMetalLayer*)layer release];
}

namespace {

struct MetalTargetData {
    id<MTLTexture> texture = nil;
#if TARGET_OS_IPHONE
    CVPixelBufferRef pixelBuffer = nullptr;
    CVMetalTextureRef metalTexture = nullptr;
    CVOpenGLESTextureRef glTexture = nullptr;
#else
    IOSurfaceRef surface = nullptr;
#endif
};

} // namespace

class InteropMetal final : public Interop {
public:
    bool init(const HostGLInfo& host, std::string& error) {
        this->host = &host;
        device = (id<MTLDevice>)bgfx::getInternalData()->context;
        if (!device) {
            error = "Metal device is unavailable";
            return false;
        }

#if TARGET_OS_IPHONE
        EAGLContext* context = [EAGLContext currentContext];
        if (!context) {
            error = "no current EAGL context";
            return false;
        }
        if (CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, device, nullptr, &metalCache) != kCVReturnSuccess
        ||  CVOpenGLESTextureCacheCreate(kCFAllocatorDefault, nullptr, context, nullptr, &glCache) != kCVReturnSuccess) {
            error = "failed to create CoreVideo texture caches";
            return false;
        }
#else
        cglContext = CGLGetCurrentContext();
        if (!cglContext) {
            error = "no current CGL context";
            return false;
        }
#endif
        return true;
    }

    ~InteropMetal() override {
        for (auto& target : targets) {
            if (target)
                destroyTarget(*target);
        }
#if TARGET_OS_IPHONE
        if (metalCache) CFRelease(metalCache);
        if (glCache)    CFRelease(glCache);
#endif
    }

    bool createTarget(SharedTarget& target, u32 width, u32 height) override {
        destroyTarget(target);

        auto* data = new MetalTargetData();
        target.platform = data;
        target.width = width;
        target.height = height;
        // Metal's framebuffer row 0 is the top of the image, GL's texture row 0 is the bottom.
        target.flipY = true;

        auto fail = [&](const char* message) {
            logf(LogLevel::Error, "Bismuth: %s", message);
            destroyTarget(target);
            return false;
        };

#if TARGET_OS_IPHONE
        NSDictionary* attributes = @{
            (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
            (NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
            (NSString*)kCVPixelBufferOpenGLESCompatibilityKey: @YES,
        };
        if (CVPixelBufferCreate(kCFAllocatorDefault, width, height, kCVPixelFormatType_32BGRA,
                                (CFDictionaryRef)attributes, &data->pixelBuffer) != kCVReturnSuccess)
            return fail("CVPixelBufferCreate failed");

        if (CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, metalCache, data->pixelBuffer, nullptr,
                MTLPixelFormatBGRA8Unorm, width, height, 0, &data->metalTexture) != kCVReturnSuccess)
            return fail("CVMetalTextureCacheCreateTextureFromImage failed");
        data->texture = CVMetalTextureGetTexture(data->metalTexture);
        [data->texture retain];

        {
            GLStateGuard guard(*host);
            if (CVOpenGLESTextureCacheCreateTextureFromImage(kCFAllocatorDefault, glCache, data->pixelBuffer, nullptr,
                    GL_TEXTURE_2D, GL_RGBA, (GLsizei)width, (GLsizei)height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, 0,
                    &data->glTexture) != kCVReturnSuccess)
                return fail("CVOpenGLESTextureCacheCreateTextureFromImage failed");

            target.glTarget = CVOpenGLESTextureGetTarget(data->glTexture);
            target.glTexture = CVOpenGLESTextureGetName(data->glTexture);
            glBindTexture(target.glTarget, target.glTexture);
            glTexParameteri(target.glTarget, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(target.glTarget, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(target.glTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(target.glTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
#else
        NSDictionary* properties = @{
            (NSString*)kIOSurfaceWidth: @(width),
            (NSString*)kIOSurfaceHeight: @(height),
            (NSString*)kIOSurfaceBytesPerElement: @4,
            (NSString*)kIOSurfacePixelFormat: @((uint32_t)'BGRA'),
        };
        data->surface = IOSurfaceCreate((CFDictionaryRef)properties);
        if (!data->surface)
            return fail("IOSurfaceCreate failed");

        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                       width:width
                                                                                      height:height
                                                                                   mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeManaged;
        data->texture = [device newTextureWithDescriptor:desc iosurface:data->surface plane:0];
        if (!data->texture)
            return fail("newTextureWithDescriptor:iosurface: failed");

        {
            GLStateGuard guard(*host);
            target.glTarget = gl::TEXTURE_RECTANGLE;
            glGenTextures(1, &target.glTexture);
            glBindTexture(GL_TEXTURE_RECTANGLE_ARB, target.glTexture);
            CGLError err = CGLTexImageIOSurface2D(cglContext, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA,
                (GLsizei)width, (GLsizei)height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, data->surface, 0);
            glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);
            if (err != kCGLNoError)
                return fail("CGLTexImageIOSurface2D failed");
        }
#endif

        // bgfx needs its own texture object first; then it is pointed at our MTLTexture.
        target.texture = bgfx::createTexture2D(
            (u16)width, (u16)height, false, 1, bgfx::TextureFormat::BGRA8,
            BGFX_TEXTURE_RT | BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP
        );
        bgfx::frame();

        if (0 == bgfx::overrideInternal(target.texture, (uintptr_t)data->texture))
            return fail("bgfx::overrideInternal failed for a shared Metal texture");

        target.framebuffer = bgfx::createFrameBuffer(1, &target.texture, false);
        bgfx::frame();

        registerTarget(&target);
        return true;
    }

    void destroyTarget(SharedTarget& target) override {
        auto* data = static_cast<MetalTargetData*>(target.platform);
        unregisterTarget(&target);

        if (bgfx::isValid(target.framebuffer))
            bgfx::destroy(target.framebuffer);
        if (bgfx::isValid(target.texture))
            bgfx::destroy(target.texture); // shared textures aren't released by bgfx

        if (data) {
#if TARGET_OS_IPHONE
            if (data->glTexture)    CFRelease(data->glTexture);
            if (data->metalTexture) CFRelease(data->metalTexture);
            if (data->pixelBuffer)  CVPixelBufferRelease(data->pixelBuffer);
#else
            if (target.glTexture) {
                GLStateGuard guard(*host);
                glDeleteTextures(1, &target.glTexture);
            }
            if (data->surface) CFRelease(data->surface);
#endif
            if (data->texture) [data->texture release];
            delete data;
        }

        target = SharedTarget();
    }

    void beforeNativeFrame() override {
        // Metal reuses these IOSurface-backed pixel buffers. A flush only submits
        // GLES reads; it does not guarantee they have stopped sampling the surface.
        if (glReadPending) {
            glFinish();
            glReadPending = false;
        }
    }

    void afterNativeFrame() override {
        // GLES samples the shared surface immediately after this call, so the
        // Metal command buffer must be complete, not merely scheduled.
        bgfx::mtlWaitForLastFrame(true);
#if TARGET_OS_IPHONE
        CVOpenGLESTextureCacheFlush(glCache, 0);
#endif
    }

    void beforeHostRead() override {}

    void afterHostRead() override {
        glReadPending = true;
    }

    const char* describe() const override {
#if TARGET_OS_IPHONE
        return "Metal (CVPixelBuffer / IOSurface)";
#else
        return "Metal (IOSurface)";
#endif
    }

private:
    const HostGLInfo* host = nullptr;
    id<MTLDevice> device = nil;
    bool glReadPending = false;
#if TARGET_OS_IPHONE
    CVMetalTextureCacheRef metalCache = nullptr;
    CVOpenGLESTextureCacheRef glCache = nullptr;
#else
    CGLContextObj cglContext = nullptr;
#endif
};

UPtr<Interop> createMetalInterop(const HostGLInfo& host, std::string& error) {
    auto interop = makeUnique<InteropMetal>();
    if (!interop->init(host, error))
        return nullptr;
    return interop;
}

} // namespace bismuth::gfx
