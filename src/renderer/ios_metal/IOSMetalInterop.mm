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
#include <cstring>
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
    id<MTLComputePipelineState> cullPipeline;
    id<MTLBuffer> cullInputBuffer;
    NSUInteger cullInputCapacity;
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
        cullInputCapacity = 0;
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

NSString* cullingShaderSource() {
    return [NSString stringWithUTF8String:
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct BismuthCullSprite { float2 startPosition; float2 transformColumn0; float2 transformColumn1; float2 offset; };"
        "struct BismuthCullParameters { float2 cameraPosition; float2 cameraRight; float2 cameraUp; float2 cameraSize; uint spriteCount; uint padding0; uint padding1; uint padding2; };"
        "kernel void bismuthCullSprites(device const BismuthCullSprite* sprites [[buffer(0)]], constant BismuthCullParameters& params [[buffer(1)]], texture2d<float, access::write> output [[texture(0)]], uint index [[thread_position_in_grid]]) {"
        "    if (index >= params.spriteCount) return;"
        "    const BismuthCullSprite sprite = sprites[index];"
        "    const float2 objectPosition = sprite.transformColumn0 * sprite.startPosition.x + sprite.transformColumn1 * sprite.startPosition.y + sprite.offset;"
        "    const float2 cameraOffset = objectPosition - params.cameraPosition;"
        "    const float2 screenPosition = float2(dot(params.cameraRight, cameraOffset), dot(params.cameraUp, cameraOffset));"
        "    constexpr float margin = 100.0;"
        "    const bool visible = screenPosition.x >= -margin && screenPosition.x <= params.cameraSize.x + margin && screenPosition.y >= -margin && screenPosition.y <= params.cameraSize.y + margin;"
        "    const float value = visible ? 1.0 : 0.0;"
        "    const uint2 outputPosition = uint2(index % output.get_width(), index / output.get_width());"
        "    output.write(float4(value, value, value, 1.0), outputPosition);"
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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
    m_width = 0;
    m_height = 0;
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
    NSString* source = cullingShaderSource();
    id<MTLLibrary> library = [owner->device newLibraryWithSource:source options:nil error:&metalError];
    if (!library) {
        assignError(error, "Metal could not compile the sprite-culling kernel", metalError);
        return false;
    }
    id<MTLFunction> function = [library newFunctionWithName:@"bismuthCullSprites"];
    if (!function) {
        error = "Metal function bismuthCullSprites was not found";
        return false;
    }
    owner->cullPipeline = [owner->device newComputePipelineStateWithFunction:function error:&metalError];
    if (!owner->cullPipeline) {
        assignError(error, "Metal could not create the sprite-culling pipeline", metalError);
        return false;
    }
    m_impl = (__bridge_retained void*)owner;
    m_glTextureName = textureName;
    m_width = width;
    m_height = height;
    return true;
}

bool IOSMetalInteropTexture::dispatchCulling(
    const IOSMetalCullSprite* sprites, std::uint32_t spriteCount,
    const IOSMetalCullParameters& parameters, std::string& error
) {
    error.clear();
    if (!isReady()) { error="Metal/GLES culling texture is not initialized"; return false; }
    if (spriteCount==0) return true;
    if (!sprites) { error="Metal culling input is null"; return false; }
    const std::uint64_t capacity=static_cast<std::uint64_t>(m_width)*m_height;
    if (spriteCount>capacity) { error="visibility mask exceeds shared texture capacity"; return false; }
    BismuthMetalInteropOwner* owner=(__bridge BismuthMetalInteropOwner*)m_impl;
    if ([EAGLContext currentContext]!=owner->openGLESContext) {
        error="shared EAGL context is not current during Metal culling"; return false;
    }
    const std::size_t inputBytes=static_cast<std::size_t>(spriteCount)*sizeof(IOSMetalCullSprite);
    if (owner->cullInputCapacity<inputBytes) {
        id<MTLBuffer> replacement=[owner->device newBufferWithLength:inputBytes options:MTLResourceStorageModeShared];
        if (!replacement) { error="Metal input buffer allocation failed"; return false; }
        owner->cullInputBuffer=replacement;
        owner->cullInputCapacity=inputBytes;
    }
    void* contents=owner->cullInputBuffer.contents;
    if (!contents) { error="Metal input buffer has no CPU-visible contents"; return false; }
    std::memcpy(contents,sprites,inputBytes);
    IOSMetalCullParameters params=parameters;
    params.spriteCount=spriteCount;
    id<MTLCommandBuffer> commandBuffer=[owner->commandQueue commandBuffer];
    if (!commandBuffer) { error="Metal could not create a culling command buffer"; return false; }
    id<MTLComputeCommandEncoder> encoder=[commandBuffer computeCommandEncoder];
    if (!encoder) { error="Metal could not create a culling encoder"; return false; }
    [encoder setComputePipelineState:owner->cullPipeline];
    [encoder setBuffer:owner->cullInputBuffer offset:0 atIndex:0];
    [encoder setBytes:&params length:sizeof(params) atIndex:1];
    [encoder setTexture:owner->metalTexture atIndex:0];
    const NSUInteger threads=std::max<NSUInteger>(1,owner->cullPipeline.threadExecutionWidth);
    const NSUInteger groups=(static_cast<NSUInteger>(spriteCount)+threads-1)/threads;
    [encoder dispatchThreadgroups:MTLSizeMake(groups,1,1) threadsPerThreadgroup:MTLSizeMake(threads,1,1)];
    [encoder endEncoding];
    [commandBuffer commit];
    // GLES samples this same CVPixelBuffer after dispatch, so fence Metal here.
    [commandBuffer waitUntilCompleted];
    if (commandBuffer.status!=MTLCommandBufferStatusCompleted) {
        assignError(error,"Metal sprite-culling command failed",commandBuffer.error);
        return false;
    }
    CVOpenGLESTextureCacheFlush(owner->openGLESTextureCache,0);
    return true;
}

bool IOSMetalInteropTexture::runSelfTest(std::string& error) {
    error.clear();
    if (!isReady()) { error="Metal/GLES shared texture is not initialized"; return false; }
    if (m_width<2 || m_height<1) { error="visibility texture needs at least two pixels"; return false; }
    const IOSMetalCullSprite samples[]={
        {5.f,5.f,1.f,0.f,0.f,1.f,0.f,0.f},
        {500.f,5.f,1.f,0.f,0.f,1.f,0.f,0.f}
    };
    IOSMetalCullParameters params;
    params.cameraWidth=100.f;
    params.cameraHeight=100.f;
    if (!dispatchCulling(samples,2,params,error)) return false;
    BismuthMetalInteropOwner* owner=(__bridge BismuthMetalInteropOwner*)m_impl;
    const CVReturn lock=CVPixelBufferLockBaseAddress(owner->pixelBuffer,kCVPixelBufferLock_ReadOnly);
    if (lock!=kCVReturnSuccess) { error="could not lock shared pixel buffer for visibility verification"; return false; }
    const auto* bytes=static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(owner->pixelBuffer));
    const std::size_t rowBytes=CVPixelBufferGetBytesPerRow(owner->pixelBuffer);
    const bool foreground=bytes && rowBytes>=8 && bytes[0]>=253 && bytes[1]>=253 && bytes[2]>=253 && bytes[3]>=253;
    const bool background=bytes && rowBytes>=8 && bytes[4]<=2 && bytes[5]<=2 && bytes[6]<=2 && bytes[7]>=253;
    CVPixelBufferUnlockBaseAddress(owner->pixelBuffer,kCVPixelBufferLock_ReadOnly);
    if (!foreground || !background) {
        error="Metal did not write the expected visible/offscreen mask values";
        return false;
    }
    if (!validateOpenGLESTexture(owner,m_glTextureName,error)) return false;
    m_selfTestPassed=true;
    return true;
}

} // namespace IOSUpstream
