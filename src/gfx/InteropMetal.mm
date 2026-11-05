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

namespace {

struct MetalTargetData {
    id<MTLTexture> texture = nil;
    GLuint glTextureName = 0;
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
        if ((__bridge void*)context != host.nativeContext) {
            error = "current EAGL context does not match Geometry Dash's host context";
            return false;
        }
        eaglContext = [context retain];
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
        std::vector<SharedTarget*> remaining;
        for (auto* target : targets) {
            if (target)
                remaining.push_back(target);
        }
        destroyTargets(remaining);
#if TARGET_OS_IPHONE
        if (metalCache) CFRelease(metalCache);
        if (glCache)    CFRelease(glCache);
        if (eaglContext) [eaglContext release];
#endif
    }

    bool createTarget(SharedTarget& target, u32 width, u32 height) override {
#if TARGET_OS_IPHONE
        if ([EAGLContext currentContext] != eaglContext) {
            logf(LogLevel::Error, "Bismuth: refusing to create a shared target without the host EAGL context current");
            return false;
        }
#else
        if (CGLGetCurrentContext() != cglContext) {
            logf(LogLevel::Error, "Bismuth: refusing to create a shared target without the host CGL context current");
            return false;
        }
#endif
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

        // Core Video defaults kCVMetalTextureUsage to MTLTextureUsageUnknown.
        // BGFX attaches this texture as a render target, so request the required
        // usages explicitly instead of relying on driver-specific defaults.
        NSDictionary* metalTextureAttributes = @{
            (NSString*)kCVMetalTextureUsage: @(MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead),
        };
        if (CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, metalCache, data->pixelBuffer,
                (CFDictionaryRef)metalTextureAttributes, MTLPixelFormatBGRA8Unorm,
                width, height, 0, &data->metalTexture) != kCVReturnSuccess)
            return fail("CVMetalTextureCacheCreateTextureFromImage failed");
        data->texture = CVMetalTextureGetTexture(data->metalTexture);
        if (!data->texture || !(data->texture.usage & MTLTextureUsageRenderTarget))
            return fail("Core Video Metal texture is not render-target capable");
        [data->texture retain];

        {
            GLStateGuard guard(*host);
            if (CVOpenGLESTextureCacheCreateTextureFromImage(kCFAllocatorDefault, glCache, data->pixelBuffer, nullptr,
                    GL_TEXTURE_2D, GL_RGBA, (GLsizei)width, (GLsizei)height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, 0,
                    &data->glTexture) != kCVReturnSuccess)
                return fail("CVOpenGLESTextureCacheCreateTextureFromImage failed");

            target.glTarget = CVOpenGLESTextureGetTarget(data->glTexture);
            target.glTexture = CVOpenGLESTextureGetName(data->glTexture);
            data->glTextureName = target.glTexture;
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
            data->glTextureName = target.glTexture;
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
        if (!bgfx::isValid(target.texture))
            return fail("bgfx::createTexture2D failed for a shared Metal target");
        bgfx::frame();

        if (0 == bgfx::overrideInternal(target.texture, (uintptr_t)data->texture))
            return fail("bgfx::overrideInternal failed for a shared Metal texture");

        target.framebuffer = bgfx::createFrameBuffer(1, &target.texture, false);
        if (!bgfx::isValid(target.framebuffer))
            return fail("bgfx::createFrameBuffer failed for a shared Metal target");
        bgfx::frame();

        registerTarget(&target);
        return true;
    }

    void destroyTarget(SharedTarget& target) override {
        std::vector<SharedTarget*> batch { &target };
        destroyTargets(batch);
    }

    void destroyTargets(const std::vector<SharedTarget*>& batch) override {
        // Re-enter the exact GL context that created the shared texture wrappers.
        // This also lets teardown synchronize any outstanding compositor reads.
        bool contextReady = true;
#if TARGET_OS_IPHONE
        EAGLContext* previousContext = [EAGLContext currentContext];
        const bool switchedContext = previousContext != eaglContext;
        if (switchedContext && ![EAGLContext setCurrentContext:eaglContext])
            contextReady = false;
#else
        CGLContextObj previousContext = CGLGetCurrentContext();
        const bool switchedContext = previousContext != cglContext;
        if (switchedContext && (!cglContext || CGLSetCurrentContext(cglContext) != kCGLNoError))
            contextReady = false;
#endif

        if (!contextReady) {
            logf(LogLevel::Warn, "Bismuth: host GL context unavailable while retiring Metal targets; retaining shared backing memory");
            for (auto* target : batch) {
                if (!target)
                    continue;
                unregisterTarget(target);
                *target = SharedTarget();
            }
#if TARGET_OS_IPHONE
            if (switchedContext)
                [EAGLContext setCurrentContext:previousContext];
#else
            if (switchedContext)
                CGLSetCurrentContext(previousContext);
#endif
            return; // backing objects are intentionally leaked rather than released while GL may use them
        }

        if (glReadPending) {
            // GL may still be sampling the IOSurface from the preceding game frame.
            glFinish();
            glReadPending = false;
        }

        if (!metalFrameComplete) {
            bgfx::mtlWaitForLastFrame(true);
            metalFrameComplete = true;
        }
        hostReadPrepared = false;

        std::vector<MetalTargetData*> retired;
        retired.reserve(batch.size());
        bool hasBgfxResources = false;
        for (auto* target : batch) {
            if (!target)
                continue;

            auto* data = static_cast<MetalTargetData*>(target->platform);
            unregisterTarget(target);

            if (bgfx::isValid(target->framebuffer)) {
                bgfx::destroy(target->framebuffer);
                hasBgfxResources = true;
            }
            if (bgfx::isValid(target->texture)) {
                bgfx::destroy(target->texture); // shared texture backing is owned here
                hasBgfxResources = true;
            }

            if (data)
                retired.push_back(data);
            else if (target->glTexture) {
                GLStateGuard guard(*host);
                glDeleteTextures(1, &target->glTexture);
            }
            *target = SharedTarget();
        }

        // Flush every queued BGFX destroy in one frame, then wait until Metal is
        // finished with the external textures before releasing their backing store.
        if (hasBgfxResources) {
            bgfx::frame();
            bgfx::mtlWaitForLastFrame(true);
            metalFrameComplete = true;
        }

#if TARGET_OS_IPHONE
        for (auto* data : retired) {
            if (data->glTexture)
                CFRelease(data->glTexture);
            if (data->metalTexture)
                CFRelease(data->metalTexture);
            if (data->pixelBuffer)
                CVPixelBufferRelease(data->pixelBuffer);
            if (data->texture)
                [data->texture release];
            delete data;
        }
        if (!retired.empty() && glCache)
            CVOpenGLESTextureCacheFlush(glCache, 0);
#else
        for (auto* data : retired) {
            if (data->glTextureName) {
                GLStateGuard guard(*host);
                glDeleteTextures(1, &data->glTextureName);
            }
            if (data->surface)
                CFRelease(data->surface);
            if (data->texture)
                [data->texture release];
            delete data;
        }
#endif

#if TARGET_OS_IPHONE
        if (switchedContext)
            [EAGLContext setCurrentContext:previousContext];
#else
        if (switchedContext)
            CGLSetCurrentContext(previousContext);
#endif
    }

    void beforeNativeFrame() override {
        // The GL compositor reads the same IOSurface that Metal renders into.
        // glFlush only submits GL commands; wait until those reads have finished
        // before allowing Metal to overwrite the shared backing store.
        if (glReadPending) {
            glFinish();
            glReadPending = false;
        }
        hostReadPrepared = false;
    }

    void afterNativeFrame() override {
        // Record the end-of-frame command buffer. Completion is deferred until
        // the host is actually about to sample one of the shared textures.
        bgfx::mtlWaitForLastFrame(false);
        metalFrameComplete = false;
        hostReadPrepared = false;
    }

    void beforeHostRead() override {
        if (!metalFrameComplete) {
            bgfx::mtlWaitForLastFrame(true);
            metalFrameComplete = true;
        }
        hostReadPrepared = true;
    }

    void afterHostRead() override {
        // A frame with no compositor reads must not force a glFinish next frame.
        if (hostReadPrepared)
            glReadPending = true;
        hostReadPrepared = false;
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
    bool metalFrameComplete = true;
    bool hostReadPrepared = false;
#if TARGET_OS_IPHONE
    EAGLContext* eaglContext = nil;
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
