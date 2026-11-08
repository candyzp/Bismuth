#define COREVIDEO_SILENCE_GL_DEPRECATION 1

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreVideo/CVMetalTextureCache.h>
#import <CoreVideo/CVOpenGLESTextureCache.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/gl.h>
#import <OpenGLES/ES2/glext.h>

#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#endif

#include "IOSMetalInterop.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

@interface BismuthMetalInteropOwner : NSObject {
@public
    CVPixelBufferRef pixelBuffer;
    CVMetalTextureCacheRef metalTextureCache;
    CVMetalTextureRef metalTextureRef;
    CVOpenGLESTextureCacheRef openGLESTextureCache;
    CVOpenGLESTextureRef openGLESTextureRef;

    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id<MTLComputePipelineState> probePipeline;
    id<MTLTexture> metalTexture;
    EAGLContext* openGLESContext;

    std::uint32_t width;
    std::uint32_t height;
}
@end

@implementation BismuthMetalInteropOwner

- (instancetype)init {
    self = [super init];
    if (self) {
        pixelBuffer = nullptr;
        metalTextureCache = nullptr;
        metalTextureRef = nullptr;
        openGLESTextureCache = nullptr;
        openGLESTextureRef = nullptr;
        width = 0;
        height = 0;
    }
    return self;
}

- (void)dealloc {
    if (openGLESTextureRef) {
        CFRelease(openGLESTextureRef);
        openGLESTextureRef = nullptr;
    }
    if (metalTextureRef) {
        CFRelease(metalTextureRef);
        metalTextureRef = nullptr;
    }
    if (openGLESTextureCache) {
        CFRelease(openGLESTextureCache);
        openGLESTextureCache = nullptr;
    }
    if (metalTextureCache) {
        CFRelease(metalTextureCache);
        metalTextureCache = nullptr;
    }
    if (pixelBuffer) {
        CVPixelBufferRelease(pixelBuffer);
        pixelBuffer = nullptr;
    }
}
@end

namespace IOSUpstream {
namespace {

void assignError(std::string& output, const char* prefix, NSError* error = nil) {
    output = prefix;
    if (error && error.localizedDescription) {
        output += ": ";
        output += error.localizedDescription.UTF8String
            ? error.localizedDescription.UTF8String
            : "unknown Apple framework error";
    }
}

NSString* probeShaderSource() {
    return [NSString stringWithUTF8String:
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "kernel void bismuthInteropProbe("
        "    texture2d<float, access::write> output [[texture(0)]],"
        "    uint2 gid [[thread_position_in_grid]]) {"
        "    if (gid.x < output.get_width() && gid.y < output.get_height()) {"
        "        output.write(float4(0.25, 0.50, 0.75, 1.0), gid);"
        "    }"
        "}\n"
    ];
}

bool validateOpenGLESTexture(BismuthMetalInteropOwner* owner,
                             GLuint textureName,
                             std::string& error) {
    if (!owner || !owner->openGLESContext ||
        [EAGLContext currentContext] != owner->openGLESContext) {
        error = "the EAGL context used to create the shared texture is not current";
        return false;
    }
    if (textureName == 0) {
        error = "Core Video returned OpenGL ES texture name 0";
        return false;
    }

    GLint previousBinding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousBinding);
    glBindTexture(GL_TEXTURE_2D, textureName);
    const GLboolean isTexture = glIsTexture(textureName);
    const GLenum bindError = glGetError();
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousBinding));
    const GLenum restoreError = glGetError();

    if (bindError != GL_NO_ERROR || restoreError != GL_NO_ERROR || !isTexture) {
        error = "OpenGL ES could not bind the CVPixelBuffer-backed texture";
        return false;
    }
    return true;
}

} // namespace

IOSMetalInteropTexture::~IOSMetalInteropTexture() {
    reset();
}

void IOSMetalInteropTexture::reset() {
    if (m_impl) {
        // initialize() stores this Objective-C owner with __bridge_retained.
        // Transfer that +1 retain back to ARC instead of casting void* to CFTypeRef.
        BismuthMetalInteropOwner* owner =
            (__bridge_transfer BismuthMetalInteropOwner*)m_impl;
        m_impl = nullptr;
        (void)owner;
    }
    m_glTextureName = 0;
    m_selfTestPassed = false;
}

bool IOSMetalInteropTexture::initialize(
    std::uint32_t width,
    std::uint32_t height,
    std::string& error
) {
    reset();
    error.clear();

    if (width == 0 || height == 0) {
        error = "shared texture dimensions must be nonzero";
        return false;
    }

    EAGLContext* context = [EAGLContext currentContext];
    if (!context) {
        error = "no current EAGLContext is available for OpenGL ES interop";
        return false;
    }

    BismuthMetalInteropOwner* owner = [[BismuthMetalInteropOwner alloc] init];
    owner->width = width;
    owner->height = height;
    owner->openGLESContext = context;

    owner->device = MTLCreateSystemDefaultDevice();
    if (!owner->device) {
        error = "Metal is not available on this device";
        return false;
    }

    owner->commandQueue = [owner->device newCommandQueue];
    if (!owner->commandQueue) {
        error = "could not create a Metal command queue";
        return false;
    }

    // Match Apple's documented iOS interop format table: Core Video creates
    // one BGRA pixel buffer that both Metal and OpenGL ES can view directly.
    NSDictionary* pixelBufferAttributes = @{
        (__bridge NSString*)kCVPixelBufferOpenGLESCompatibilityKey : @YES,
        (__bridge NSString*)kCVPixelBufferMetalCompatibilityKey : @YES
    };

    CVReturn cvResult = CVPixelBufferCreate(
        kCFAllocatorDefault,
        width,
        height,
        kCVPixelFormatType_32BGRA,
        (__bridge CFDictionaryRef)pixelBufferAttributes,
        &owner->pixelBuffer
    );
    if (cvResult != kCVReturnSuccess || !owner->pixelBuffer) {
        error = "CVPixelBufferCreate failed for a Metal/OpenGL ES compatible RGBA buffer";
        return false;
    }

    cvResult = CVMetalTextureCacheCreate(
        kCFAllocatorDefault,
        nullptr,
        owner->device,
        nullptr,
        &owner->metalTextureCache
    );
    if (cvResult != kCVReturnSuccess || !owner->metalTextureCache) {
        error = "CVMetalTextureCacheCreate failed";
        return false;
    }

    cvResult = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault,
        owner->metalTextureCache,
        owner->pixelBuffer,
        nullptr,
        MTLPixelFormatBGRA8Unorm,
        width,
        height,
        0,
        &owner->metalTextureRef
    );
    if (cvResult != kCVReturnSuccess || !owner->metalTextureRef) {
        error = "CVMetalTextureCache could not create an RGBA8 Metal view";
        return false;
    }

    owner->metalTexture = CVMetalTextureGetTexture(owner->metalTextureRef);
    if (!owner->metalTexture) {
        error = "CVMetalTextureGetTexture returned nil";
        return false;
    }

    cvResult = CVOpenGLESTextureCacheCreate(
        kCFAllocatorDefault,
        nullptr,
        owner->openGLESContext,
        nullptr,
        &owner->openGLESTextureCache
    );
    if (cvResult != kCVReturnSuccess || !owner->openGLESTextureCache) {
        error = "CVOpenGLESTextureCacheCreate failed";
        return false;
    }

    cvResult = CVOpenGLESTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault,
        owner->openGLESTextureCache,
        owner->pixelBuffer,
        nullptr,
        GL_TEXTURE_2D,
        GL_RGBA,
        width,
        height,
        GL_BGRA_EXT,
        GL_UNSIGNED_INT_8_8_8_8_REV,
        0,
        &owner->openGLESTextureRef
    );
    if (cvResult != kCVReturnSuccess || !owner->openGLESTextureRef) {
        error = "CVOpenGLESTextureCache could not create an RGBA OpenGL ES view";
        return false;
    }

    const GLuint textureName = CVOpenGLESTextureGetName(owner->openGLESTextureRef);
    if (!validateOpenGLESTexture(owner, textureName, error))
        return false;

    NSError* metalError = nil;
    NSString* source = probeShaderSource();
    id<MTLLibrary> library = [owner->device newLibraryWithSource:source
                                                         options:nil
                                                           error:&metalError];
    if (!library) {
        assignError(error, "Metal could not compile the interop probe kernel", metalError);
        return false;
    }

    id<MTLFunction> function = [library newFunctionWithName:@"bismuthInteropProbe"];
    if (!function) {
        error = "Metal probe function bismuthInteropProbe was not found";
        return false;
    }

    owner->probePipeline = [owner->device newComputePipelineStateWithFunction:function
                                                                        error:&metalError];
    if (!owner->probePipeline) {
        assignError(error, "Metal could not create the interop compute pipeline", metalError);
        return false;
    }

    m_impl = (__bridge_retained void*)owner;
    m_glTextureName = textureName;
    return true;
}

bool IOSMetalInteropTexture::runSelfTest(std::string& error) {
    error.clear();
    if (!isReady()) {
        error = "Metal/OpenGL ES shared texture has not been initialized";
        return false;
    }

    BismuthMetalInteropOwner* owner = (__bridge BismuthMetalInteropOwner*)m_impl;
    id<MTLCommandBuffer> commandBuffer = [owner->commandQueue commandBuffer];
    if (!commandBuffer) {
        error = "Metal command queue could not create a command buffer";
        return false;
    }

    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    if (!encoder) {
        error = "Metal command buffer could not create a compute encoder";
        return false;
    }

    [encoder setComputePipelineState:owner->probePipeline];
    [encoder setTexture:owner->metalTexture atIndex:0];

    const NSUInteger threadWidth = std::min<NSUInteger>(4, owner->probePipeline.threadExecutionWidth);
    const NSUInteger threadHeight = std::max<NSUInteger>(
        1,
        std::min<NSUInteger>(4, owner->probePipeline.maxTotalThreadsPerThreadgroup /
                                  std::max<NSUInteger>(1, threadWidth))
    );
    const MTLSize threadsPerGroup = MTLSizeMake(threadWidth, threadHeight, 1);
    const MTLSize threadgroups = MTLSizeMake(
        (owner->width + threadWidth - 1) / threadWidth,
        (owner->height + threadHeight - 1) / threadHeight,
        1
    );

    [encoder dispatchThreadgroups:threadgroups threadsPerThreadgroup:threadsPerGroup];
    [encoder endEncoding];
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];

    if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
        assignError(error, "Metal interop probe command failed", commandBuffer.error);
        return false;
    }

    // Verify that Metal's writes reached the same CVPixelBuffer backing before
    // exposing the GL view to the renderer. This is initialization-only work.
    CVReturn lockResult = CVPixelBufferLockBaseAddress(owner->pixelBuffer,
                                                       kCVPixelBufferLock_ReadOnly);
    if (lockResult != kCVReturnSuccess) {
        error = "could not lock the shared pixel buffer for interop verification";
        return false;
    }

    const auto* base = static_cast<const std::uint8_t*>(
        CVPixelBufferGetBaseAddress(owner->pixelBuffer)
    );
    const std::size_t bytesPerRow = CVPixelBufferGetBytesPerRow(owner->pixelBuffer);
    bool pixelMatches = false;
    if (base && bytesPerRow >= 4) {
        // BGRA8Unorm is exposed as BGRA bytes in the shared CVPixelBuffer.
        pixelMatches =
            std::abs(static_cast<int>(base[0]) - 191) <= 2 &&
            std::abs(static_cast<int>(base[1]) - 128) <= 2 &&
            std::abs(static_cast<int>(base[2]) - 64) <= 2 &&
            base[3] >= 253;
    }
    CVPixelBufferUnlockBaseAddress(owner->pixelBuffer, kCVPixelBufferLock_ReadOnly);

    if (!pixelMatches) {
        error = "Metal completed, but its expected RGBA probe value was not visible in the shared CVPixelBuffer";
        return false;
    }

    if (!validateOpenGLESTexture(owner, m_glTextureName, error))
        return false;

    m_selfTestPassed = true;
    return true;
}

} // namespace IOSUpstream
