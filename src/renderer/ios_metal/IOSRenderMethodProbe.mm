#define COREVIDEO_SILENCE_GL_DEPRECATION 1

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreVideo/CVMetalTextureCache.h>
#import <CoreVideo/CVOpenGLESTextureCache.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/gl.h>
#import <OpenGLES/ES2/glext.h>
#import <dispatch/dispatch.h>

#include "IOSRenderMethodProbe.hpp"
#include "IOSMetalInterop.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace IOSUpstream {
namespace {

template <typename T>
class CFScoped final {
public:
    T value = nullptr;

    CFScoped() = default;
    CFScoped(const CFScoped&) = delete;
    CFScoped& operator=(const CFScoped&) = delete;

    ~CFScoped() {
        if (value)
            CFRelease(value);
    }

    T* out() { return &value; }
    T get() const { return value; }
};

NSString* probeShaderSource() {
    return [NSString stringWithUTF8String:
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "kernel void fillTexture(texture2d<float, access::write> output [[texture(0)]], "
        "uint2 gid [[thread_position_in_grid]]) {\n"
        "  if (gid.x >= output.get_width() || gid.y >= output.get_height()) return;\n"
        "  output.write(float4(1.0, 1.0, 1.0, 1.0), gid);\n"
        "}\n"
        "kernel void fillBuffer(device uchar* output [[buffer(0)]], "
        "uint gid [[thread_position_in_grid]]) {\n"
        "  if (gid < 4) output[gid] = 255;\n"
        "}\n"
    ];
}

bool createLibrary(id<MTLDevice> device, id<MTLLibrary>& library, std::string& error) {
    NSError* metalError = nil;
    library = [device newLibraryWithSource:probeShaderSource() options:nil error:&metalError];
    if (!library) {
        error = "Metal shader compilation failed";
        if (metalError.localizedDescription.UTF8String)
            error += std::string(": ") + metalError.localizedDescription.UTF8String;
        return false;
    }
    return true;
}

bool createPipeline(id<MTLDevice> device,
                    id<MTLComputePipelineState>& pipeline,
                    std::string& error) {
    id<MTLLibrary> library = nil;
    if (!createLibrary(device, library, error))
        return false;

    id<MTLFunction> function = [library newFunctionWithName:@"fillTexture"];
    if (!function) {
        error = "Metal library did not contain fillTexture";
        return false;
    }

    NSError* metalError = nil;
    pipeline = [device newComputePipelineStateWithFunction:function error:&metalError];
    if (!pipeline) {
        error = "Metal compute pipeline creation failed";
        if (metalError.localizedDescription.UTF8String)
            error += std::string(": ") + metalError.localizedDescription.UTF8String;
        return false;
    }
    return true;
}

bool createPixelBuffer(std::uint32_t width, std::uint32_t height,
                       bool metalCompatible, bool glesCompatible,
                       CFScoped<CVPixelBufferRef>& pixelBuffer,
                       std::string& error) {
    NSMutableDictionary* attributes = [NSMutableDictionary dictionary];
    if (metalCompatible) {
        attributes[(__bridge NSString*)kCVPixelBufferMetalCompatibilityKey] = @YES;
    }
    if (glesCompatible) {
        attributes[(__bridge NSString*)kCVPixelBufferOpenGLESCompatibilityKey] = @YES;
    }

    const CVReturn result = CVPixelBufferCreate(
        kCFAllocatorDefault,
        width,
        height,
        kCVPixelFormatType_32BGRA,
        (__bridge CFDictionaryRef)attributes,
        pixelBuffer.out()
    );
    if (result != kCVReturnSuccess || !pixelBuffer.get()) {
        error = "CVPixelBufferCreate failed (result " + std::to_string(result) + ")";
        return false;
    }
    return true;
}

bool createMetalView(CVPixelBufferRef pixelBuffer,
                     id<MTLDevice> device,
                     CFScoped<CVMetalTextureCacheRef>& cache,
                     CFScoped<CVMetalTextureRef>& textureRef,
                     id<MTLTexture>& texture,
                     std::string& error) {
    CVReturn result = CVMetalTextureCacheCreate(
        kCFAllocatorDefault, nullptr, device, nullptr, cache.out()
    );
    if (result != kCVReturnSuccess || !cache.get()) {
        error = "CVMetalTextureCacheCreate failed (result " + std::to_string(result) + ")";
        return false;
    }

    result = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault,
        cache.get(),
        pixelBuffer,
        nullptr,
        MTLPixelFormatBGRA8Unorm,
        CVPixelBufferGetWidth(pixelBuffer),
        CVPixelBufferGetHeight(pixelBuffer),
        0,
        textureRef.out()
    );
    if (result != kCVReturnSuccess || !textureRef.get()) {
        error = "CVMetalTextureCacheCreateTextureFromImage failed (result " +
            std::to_string(result) + ")";
        return false;
    }

    texture = CVMetalTextureGetTexture(textureRef.get());
    if (!texture) {
        error = "CVMetalTextureGetTexture returned nil";
        return false;
    }
    return true;
}

bool createGLESView(CVPixelBufferRef pixelBuffer,
                    EAGLContext* context,
                    CFScoped<CVOpenGLESTextureCacheRef>& cache,
                    CFScoped<CVOpenGLESTextureRef>& textureRef,
                    GLuint& textureName,
                    std::string& error) {
    CVReturn result = CVOpenGLESTextureCacheCreate(
        kCFAllocatorDefault, nullptr, context, nullptr, cache.out()
    );
    if (result != kCVReturnSuccess || !cache.get()) {
        error = "CVOpenGLESTextureCacheCreate failed (result " +
            std::to_string(result) + ")";
        return false;
    }

    result = CVOpenGLESTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault,
        cache.get(),
        pixelBuffer,
        nullptr,
        GL_TEXTURE_2D,
        GL_RGBA,
        CVPixelBufferGetWidth(pixelBuffer),
        CVPixelBufferGetHeight(pixelBuffer),
        GL_BGRA_EXT,
        GL_UNSIGNED_INT_8_8_8_8_REV,
        0,
        textureRef.out()
    );
    if (result != kCVReturnSuccess || !textureRef.get()) {
        error = "CVOpenGLESTextureCacheCreateTextureFromImage failed (result " +
            std::to_string(result) + ")";
        return false;
    }

    textureName = CVOpenGLESTextureGetName(textureRef.get());
    if (textureName == 0) {
        error = "Core Video returned OpenGL ES texture name 0";
        return false;
    }
    return true;
}

bool validateGLESName(GLuint textureName, std::string& error) {
    if (!textureName) {
        error = "OpenGL ES texture name is zero";
        return false;
    }

    GLint previousBinding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousBinding);
    while (glGetError() != GL_NO_ERROR) {}

    glBindTexture(GL_TEXTURE_2D, textureName);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const GLboolean isTexture = glIsTexture(textureName);
    const GLenum bindError = glGetError();

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousBinding));
    const GLenum restoreError = glGetError();

    if (!isTexture || bindError != GL_NO_ERROR || restoreError != GL_NO_ERROR) {
        error = "OpenGL ES could not bind and validate the Core Video texture";
        return false;
    }
    return true;
}

bool verifyWhitePixel(CVPixelBufferRef pixelBuffer, std::string& error) {
    const CVReturn lockResult = CVPixelBufferLockBaseAddress(
        pixelBuffer, kCVPixelBufferLock_ReadOnly
    );
    if (lockResult != kCVReturnSuccess) {
        error = "CVPixelBufferLockBaseAddress failed (result " +
            std::to_string(lockResult) + ")";
        return false;
    }

    const auto* bytes = static_cast<const std::uint8_t*>(
        CVPixelBufferGetBaseAddress(pixelBuffer)
    );
    const std::size_t rowBytes = CVPixelBufferGetBytesPerRow(pixelBuffer);
    const bool white = bytes && rowBytes >= 4 &&
        bytes[0] >= 250 && bytes[1] >= 250 &&
        bytes[2] >= 250 && bytes[3] >= 250;

    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
    if (!white) {
        error = "shared pixel buffer did not contain the expected white result";
        return false;
    }
    return true;
}

bool dispatchTextureFill(id<MTLDevice> device,
                         id<MTLCommandQueue> queue,
                         id<MTLTexture> texture,
                         int synchronizationMode,
                         std::string& error) {
    id<MTLComputePipelineState> pipeline = nil;
    if (!createPipeline(device, pipeline, error))
        return false;

    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    if (!commandBuffer) {
        error = "Metal command queue returned no command buffer";
        return false;
    }

    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    if (!encoder) {
        error = "Metal could not create a compute encoder";
        return false;
    }

    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:texture atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake(
        (texture.width + 7) / 8,
        (texture.height + 7) / 8,
        1
    ) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [encoder endEncoding];

    if (synchronizationMode == 0) {
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
    } else if (synchronizationMode == 1) {
        dispatch_semaphore_t completion = dispatch_semaphore_create(0);
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) {
            dispatch_semaphore_signal(completion);
        }];
        [commandBuffer commit];
        const long waitResult = dispatch_semaphore_wait(
            completion, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)
        );
        if (waitResult != 0) {
            error = "Metal completion handler timed out after one second";
            return false;
        }
    } else if (synchronizationMode == 2) {
        if (@available(iOS 12.0, *)) {
            id<MTLSharedEvent> sharedEvent = [device newSharedEvent];
            if (!sharedEvent) {
                error = "MTLSharedEvent creation returned nil";
                return false;
            }
            [commandBuffer encodeSignalEvent:sharedEvent value:1];
            [commandBuffer commit];
            if (![sharedEvent waitUntilSignaledValue:1 timeoutMS:1000]) {
                error = "MTLSharedEvent did not signal within one second";
                return false;
            }
        } else {
            error = "MTLSharedEvent requires iOS 12 or newer";
            return false;
        }
    } else {
        error = "unknown Metal synchronization mode";
        return false;
    }

    if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
        error = "Metal command buffer did not complete";
        if (commandBuffer.error.localizedDescription.UTF8String)
            error += std::string(": ") + commandBuffer.error.localizedDescription.UTF8String;
        return false;
    }
    return true;
}

bool fillSharedBuffer(id<MTLDevice> device, id<MTLCommandQueue> queue,
                      id<MTLBuffer>& outputBuffer, std::string& error) {
    id<MTLLibrary> library = nil;
    if (!createLibrary(device, library, error))
        return false;

    id<MTLFunction> function = [library newFunctionWithName:@"fillBuffer"];
    if (!function) {
        error = "Metal library did not contain fillBuffer";
        return false;
    }

    NSError* metalError = nil;
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:&metalError];
    if (!pipeline) {
        error = "Metal shared-buffer pipeline creation failed";
        if (metalError.localizedDescription.UTF8String)
            error += std::string(": ") + metalError.localizedDescription.UTF8String;
        return false;
    }

    outputBuffer = [device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    if (!outputBuffer || !outputBuffer.contents) {
        error = "Metal shared-storage buffer allocation failed";
        return false;
    }

    std::memset(outputBuffer.contents, 0, 4);
    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    if (!commandBuffer || !encoder) {
        error = "Metal could not create the shared-buffer command";
        return false;
    }

    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:outputBuffer offset:0 atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(4, 1, 1)];
    [encoder endEncoding];
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];

    if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
        error = "Metal shared-buffer command did not complete";
        return false;
    }

    const auto* bytes = static_cast<const std::uint8_t*>(outputBuffer.contents);
    if (!bytes || bytes[0] != 255 || bytes[1] != 255 ||
        bytes[2] != 255 || bytes[3] != 255) {
        error = "Metal shared buffer did not contain the expected CPU-readable bytes";
        return false;
    }
    return true;
}

} // namespace

bool runIOSRenderMethodProbe(std::uint32_t method, std::string& report) {
    report.clear();
    @autoreleasepool {
        EAGLContext* context = [EAGLContext currentContext];
        id<MTLDevice> device = nil;
        id<MTLCommandQueue> queue = nil;

        auto pass = [&](const char* message) {
            report = message;
            return true;
        };
        auto fail = [&](const std::string& message) {
            report = message;
            return false;
        };

        if (method == 1) {
            if (!context)
                return fail("No current EAGLContext; the probe ran outside Geometry Dash's GL render thread");
            const GLubyte* version = glGetString(GL_VERSION);
            if (!version)
                return fail("EAGLContext exists but glGetString(GL_VERSION) returned null");
            return pass("PASS: Geometry Dash's current EAGLContext is available");
        }

        if (method >= 2) {
            device = MTLCreateSystemDefaultDevice();
            if (!device)
                return fail("MTLCreateSystemDefaultDevice returned nil");
        }
        if (method == 2)
            return pass("PASS: Metal device creation");
        if (method >= 3) {
            queue = [device newCommandQueue];
            if (!queue)
                return fail("Metal newCommandQueue returned nil");
        }
        if (method == 3)
            return pass("PASS: Metal command queue creation");

        if (method == 4 || method == 5) {
            id<MTLLibrary> library = nil;
            std::string error;
            if (!createLibrary(device, library, error))
                return fail(error);
            if (method == 4)
                return pass("PASS: runtime Metal shader-library compilation");

            id<MTLFunction> function = [library newFunctionWithName:@"fillTexture"];
            if (!function)
                return fail("Metal function fillTexture was not found");
            NSError* metalError = nil;
            id<MTLComputePipelineState> pipeline =
                [device newComputePipelineStateWithFunction:function error:&metalError];
            if (!pipeline) {
                std::string message = "Metal compute pipeline creation failed";
                if (metalError.localizedDescription.UTF8String)
                    message += std::string(": ") + metalError.localizedDescription.UTF8String;
                return fail(message);
            }
            return pass("PASS: Metal compute-pipeline creation");
        }

        if (method >= 6 && method <= 13) {
            if (!context)
                return fail("No current EAGLContext for the Core Video/OpenGL ES probe");

            const bool wantsMetal = method != 7 && method != 10 && method != 12;
            const bool wantsGLES = method != 6 && method != 9 && method != 11;
            CFScoped<CVPixelBufferRef> pixelBuffer;
            std::string error;

            if (method == 9 || method == 10) {
                if (!createPixelBuffer(16, 16, true, true, pixelBuffer, error))
                    return fail(error);
                if (method == 9) {
                    CFScoped<CVMetalTextureCacheRef> cache;
                    const CVReturn result = CVMetalTextureCacheCreate(
                        kCFAllocatorDefault, nullptr, device, nullptr, cache.out()
                    );
                    if (result != kCVReturnSuccess || !cache.get())
                        return fail("CVMetalTextureCacheCreate failed (result " +
                            std::to_string(result) + ")");
                    return pass("PASS: CVMetalTextureCache creation");
                }
                CFScoped<CVOpenGLESTextureCacheRef> cache;
                const CVReturn result = CVOpenGLESTextureCacheCreate(
                    kCFAllocatorDefault, nullptr, context, nullptr, cache.out()
                );
                if (result != kCVReturnSuccess || !cache.get())
                    return fail("CVOpenGLESTextureCacheCreate failed (result " +
                        std::to_string(result) + ")");
                return pass("PASS: CVOpenGLESTextureCache creation");
            }

            if (!createPixelBuffer(16, 16, wantsMetal, wantsGLES, pixelBuffer, error))
                return fail(error);

            if (method == 6)
                return pass("PASS: Metal-compatible BGRA CVPixelBuffer allocation");
            if (method == 7)
                return pass("PASS: OpenGL ES-compatible BGRA CVPixelBuffer allocation");
            if (method == 8)
                return pass("PASS: Metal + OpenGL ES compatible BGRA CVPixelBuffer allocation");

            if (method == 11) {
                CFScoped<CVMetalTextureCacheRef> cache;
                CFScoped<CVMetalTextureRef> textureRef;
                id<MTLTexture> texture = nil;
                if (!createMetalView(pixelBuffer.get(), device, cache, textureRef, texture, error))
                    return fail(error);
                return pass("PASS: CVPixelBuffer to Metal texture view");
            }

            if (method == 12) {
                CFScoped<CVOpenGLESTextureCacheRef> cache;
                CFScoped<CVOpenGLESTextureRef> textureRef;
                GLuint textureName = 0;
                if (!createGLESView(pixelBuffer.get(), context, cache, textureRef, textureName, error))
                    return fail(error);
                if (!validateGLESName(textureName, error))
                    return fail(error);
                return pass("PASS: CVPixelBuffer to OpenGL ES texture view and bind");
            }

            if (method == 13) {
                CFScoped<CVMetalTextureCacheRef> metalCache;
                CFScoped<CVMetalTextureRef> metalTextureRef;
                id<MTLTexture> metalTexture = nil;
                if (!createMetalView(pixelBuffer.get(), device, metalCache, metalTextureRef,
                                     metalTexture, error))
                    return fail(error);

                CFScoped<CVOpenGLESTextureCacheRef> glCache;
                CFScoped<CVOpenGLESTextureRef> glTextureRef;
                GLuint glTextureName = 0;
                if (!createGLESView(pixelBuffer.get(), context, glCache, glTextureRef,
                                    glTextureName, error))
                    return fail(error);
                if (!validateGLESName(glTextureName, error))
                    return fail(error);
                return pass("PASS: same BGRA CVPixelBuffer produced valid Metal and GLES texture views");
            }
        }

        if (method >= 14 && method <= 16) {
            if (!context)
                return fail("No current EAGLContext for shared-texture synchronization probe");

            CFScoped<CVPixelBufferRef> pixelBuffer;
            if (!createPixelBuffer(16, 16, true, true, pixelBuffer, report))
                return false;

            CFScoped<CVMetalTextureCacheRef> metalCache;
            CFScoped<CVMetalTextureRef> metalTextureRef;
            id<MTLTexture> metalTexture = nil;
            if (!createMetalView(pixelBuffer.get(), device, metalCache, metalTextureRef,
                                 metalTexture, report))
                return false;

            const int syncMode = method == 14 ? 0 : method == 15 ? 1 : 2;
            if (!dispatchTextureFill(device, queue, metalTexture, syncMode, report))
                return false;
            if (!verifyWhitePixel(pixelBuffer.get(), report))
                return false;

            if (method == 14)
                return pass("PASS: Metal compute write + waitUntilCompleted + CPU readback");
            if (method == 15)
                return pass("PASS: Metal compute write + completion-handler synchronization + CPU readback");
            return pass("PASS: Metal compute write + MTLSharedEvent synchronization + CPU readback");
        }

        if (method == 17 || method == 18) {
            id<MTLBuffer> outputBuffer = nil;
            std::string error;
            if (!fillSharedBuffer(device, queue, outputBuffer, error))
                return fail(error);

            if (method == 17)
                return pass("PASS: Metal shared MTLBuffer GPU write and CPU readback");

            if (!context)
                return fail("No current EAGLContext for MTLBuffer-to-OpenGL ES upload");

            GLint previousBinding = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousBinding);
            GLuint textureName = 0;
            glGenTextures(1, &textureName);
            if (!textureName)
                return fail("glGenTextures returned zero for MTLBuffer upload probe");

            glBindTexture(GL_TEXTURE_2D, textureName);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, outputBuffer.contents);
            const GLenum uploadError = glGetError();
            const GLboolean isTexture = glIsTexture(textureName);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousBinding));
            glDeleteTextures(1, &textureName);
            const GLenum restoreError = glGetError();

            if (uploadError != GL_NO_ERROR || restoreError != GL_NO_ERROR || !isTexture)
                return fail("MTLBuffer-to-GLES glTexImage2D upload failed");
            return pass("PASS: shared MTLBuffer output copied into an OpenGL ES texture");
        }

        if (method == 19) {
            if (!context)
                return fail("No current EAGLContext for double-buffered texture probe");

            for (int index = 0; index < 2; ++index) {
                CFScoped<CVPixelBufferRef> pixelBuffer;
                if (!createPixelBuffer(16, 16, true, true, pixelBuffer, report))
                    return false;

                CFScoped<CVMetalTextureCacheRef> metalCache;
                CFScoped<CVMetalTextureRef> metalTextureRef;
                id<MTLTexture> metalTexture = nil;
                if (!createMetalView(pixelBuffer.get(), device, metalCache,
                                     metalTextureRef, metalTexture, report))
                    return false;

                CFScoped<CVOpenGLESTextureCacheRef> glCache;
                CFScoped<CVOpenGLESTextureRef> glTextureRef;
                GLuint glTextureName = 0;
                if (!createGLESView(pixelBuffer.get(), context, glCache,
                                    glTextureRef, glTextureName, report))
                    return false;
                if (!validateGLESName(glTextureName, report))
                    return false;
            }
            return pass("PASS: two independent CVPixelBuffers each produced Metal and GLES texture views");
        }

        if (method == 20) {
            IOSMetalInteropTexture interop;
            std::string error;
            if (!interop.initialize(16, 1, error))
                return fail("Current full interop initialization failed: " + error);
            if (!interop.runSelfTest(error))
                return fail("Current full interop self-test failed: " + error);
            return pass("PASS: current end-to-end Metal/OpenGL ES shared-texture culling self-test");
        }

        return fail("Unknown iOS render method number (valid range is 1-20)");
    }
}

} // namespace IOSUpstream
