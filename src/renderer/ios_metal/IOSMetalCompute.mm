#define COREVIDEO_SILENCE_GL_DEPRECATION 1

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreVideo/CVMetalTextureCache.h>
#import <CoreVideo/CVOpenGLESTextureCache.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/gl.h>
#import <OpenGLES/ES2/glext.h>

#include "IOSMetalCompute.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

namespace BismuthIOSMetal {

constexpr NSUInteger SLOT_COUNT = 3;

namespace {
constexpr std::uint32_t COLOR_CHANNEL_BG = 1000u;
constexpr std::uint32_t COLOR_CHANNEL_LBG = 1007u;
constexpr std::uint32_t COLOR_CHANNEL_LIGHTER = 1012u;

constexpr std::uint32_t FLAG_AUDIO_SCALE = 1u << 0;
constexpr std::uint32_t FLAG_CUSTOM_AUDIO_SCALE = 1u << 1;
constexpr std::uint32_t FLAG_ORB = 1u << 2;
constexpr std::uint32_t FLAG_INVISIBLE_BLOCK = 1u << 3;
constexpr std::uint32_t FLAG_SPECIAL_GLOW = 1u << 4;
constexpr std::uint32_t FLAG_GLOW_LBG = 1u << 5;
constexpr std::uint32_t FLAG_BASE_HSV = 1u << 6;
constexpr std::uint32_t FLAG_DETAIL_HSV = 1u << 7;
constexpr std::uint32_t FLAG_STATIC_OBJECT = 1u << 8;
constexpr std::uint32_t FLAG_STATIC_GLOW = 1u << 9;

constexpr std::uint32_t PULSE_BASE_ONLY = 1u << 26;
constexpr std::uint32_t PULSE_DETAIL_ONLY = 1u << 27;
constexpr std::uint32_t PULSE_HSV = 1u << 28;
constexpr std::uint32_t PULSE_DYNAMIC_HSV = 1u << 29;
constexpr std::uint32_t PULSE_ACTIVE = 1u << 30;

NSString* computeSource() {
    return [NSString stringWithUTF8String:R"METAL(
#include <metal_stdlib>
using namespace metal;

struct SpriteWorkItem {
    float startX;
    float startY;
    uint groupCombinationIndex;
    uint spriteIndex;
};

struct InputSprite {
    uint objectIndex;
    uint spriteType;
    uint triangleStart;
    uint triangleCount;
};

struct ObjectInfo {
    float rotationSpeed;
    int flags;
    float audioScaleMin;
    float audioScaleMax;
    float fadeMargin;
    uint opacityValues;
    uint baseColorChannelId;
    uint baseHSVHueAndFlags;
    uint baseHSVSaturationAndValue;
    uint detailColorChannelId;
    uint detailHSVHueAndFlags;
    uint detailHSVSaturationAndValue;
    uint staticGlowColor;
};

struct GroupCombinationState {
    float4 positionalTransform;
    float4 localTransform;
    float2 offset;
    float opacity;
    uint pulseIndex;
};

struct PulseEntry {
    uint flags;
    uint colorOrHSV;
    uint hsvSaturationAndValue;
};

struct FrameState {
    float cameraPositionX;
    float cameraPositionY;
    float cameraRightX;
    float cameraRightY;
    float cameraUpX;
    float cameraUpY;
    float cameraWidth;
    float cameraHeight;

    float cameraViewWidth;
    float cameraViewHeight;
    float windowWidth;
    float windowHeight;

    float timer;
    float audioScale;
    float gameStateCameraPositionX;
    float gameStateCameraZoom;

    float winSizeWidth;
    float levelFlipProgress;
    float screenRight;
    float cameraUnzoomedX;

    float4 specialLightBGColor;

    uint gameStateFlags;
    uint objectCount;
    uint groupCount;
    uint pulseCount;
    uint colorChannelCount;
    uint spriteCount;
    uint reserved0;
    uint reserved1;
};

struct HSVValue {
    float hue;
    float saturation;
    float value;
    bool saturationAdd;
    bool valueAdd;
};

constant uint GAME_STATE_PLAYER_DEAD = 1u;
constant uint SPRITE_BASE = 0u;
constant uint SPRITE_DETAIL = 1u;
constant uint SPRITE_BLACK = 2u;
constant uint SPRITE_GLOW = 3u;
constant uint SPRITE_BLACK_CHILD = 4u;
constant float PI = 3.14159265358979323846;

float4 unpackRGBA(uint value) {
    return float4(
        float(value & 0xffu),
        float((value >> 8) & 0xffu),
        float((value >> 16) & 0xffu),
        float((value >> 24) & 0xffu)
    ) / 255.0;
}

float2x2 rotationMatrix(float angle) {
    float s = sin(angle);
    float c = cos(angle);
    return float2x2(float2(c, s), float2(-s, c));
}

float2x2 scaleMatrix(float2 scale) {
    return float2x2(float2(scale.x, 0.0), float2(0.0, scale.y));
}

float3 rgbToHSV(float3 c) {
    float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    float4 p = mix(float4(c.bg, K.wz), float4(c.gb, K.xy), step(c.b, c.g));
    float4 q = mix(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

float3 hsvToRGB(float3 c) {
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

float halfValue(uint bits) {
    return float(as_type<half>(ushort(bits & 0xffffu)));
}

HSVValue readHSV(uint hueFlags, uint saturationValue) {
    HSVValue hsv;
    hsv.hue = halfValue(hueFlags) / 360.0;
    hsv.saturation = halfValue(saturationValue);
    hsv.value = halfValue(saturationValue >> 16);
    hsv.saturationAdd = (hueFlags & 0x20000000u) != 0u;
    hsv.valueAdd = (hueFlags & 0x10000000u) != 0u;
    return hsv;
}

float3 applyHSV(HSVValue adjustment, float3 rgb) {
    float3 hsv = rgbToHSV(rgb);
    hsv.x = fmod(hsv.x + adjustment.hue + 1.0, 1.0);
    hsv.y = adjustment.saturationAdd
        ? hsv.y + adjustment.saturation : hsv.y * adjustment.saturation;
    hsv.z = adjustment.valueAdd
        ? hsv.z + adjustment.value : hsv.z * adjustment.value;
    hsv.y = clamp(hsv.y, 0.0, 1.0);
    hsv.z = clamp(hsv.z, 0.0, 1.0);
    return hsvToRGB(hsv);
}

HSVValue multiplyHSV(HSVValue hsv, float factor) {
    HSVValue result;
    result.hue = hsv.hue * factor;
    result.saturation = hsv.saturationAdd
        ? hsv.saturation * factor
        : hsv.saturation * factor + (1.0 - factor);
    result.value = hsv.valueAdd
        ? hsv.value * factor
        : hsv.value * factor + (1.0 - factor);
    result.saturationAdd = hsv.saturationAdd;
    result.valueAdd = hsv.valueAdd;
    return result;
}

float3 applyPulseEntries(
    float3 color,
    uint pulseIndex,
    bool isBaseColor,
    device const PulseEntry* pulses,
    device const uint* colorChannels,
    constant FrameState& frame
) {
    if (pulseIndex >= frame.pulseCount)
        return color;

    for (uint i = 0u; i < 128u && pulseIndex + i < frame.pulseCount; ++i) {
        PulseEntry entry = pulses[pulseIndex + i];
        uint flags = entry.flags;
        if (flags == 0u)
            break;
        if ((flags & PULSE_ACTIVE) == 0u)
            continue;
        if ((flags & PULSE_BASE_ONLY) != 0u && !isBaseColor)
            continue;
        if ((flags & PULSE_DETAIL_ONLY) != 0u && isBaseColor)
            continue;

        float amount = float(flags & 0xffffu) / 65535.0;
        if (amount <= 0.0)
            continue;

        float3 destination;
        if ((flags & PULSE_HSV) != 0u) {
            uint channel = (flags >> 16) & 0x3ffu;
            float3 source = color;
            if (channel != 0u && channel < frame.colorChannelCount)
                source = unpackRGBA(colorChannels[channel]).rgb;
            HSVValue hsv = readHSV(
                entry.colorOrHSV,
                entry.hsvSaturationAndValue
            );
            if ((flags & PULSE_DYNAMIC_HSV) != 0u)
                hsv = multiplyHSV(hsv, amount);
            destination = applyHSV(hsv, source);
        } else {
            destination = unpackRGBA(entry.colorOrHSV).rgb;
        }
        color = mix(color, destination, amount);
    }
    return color;
}

float2 invisibleBlockOpacity(
    float2 position,
    float fadeMargin,
    constant FrameState& frame
) {
    float centerLeft = frame.windowWidth * 0.5 - 75.0;
    float centerRight = centerLeft + 110.0;
    float screenLeft = frame.screenRight - centerRight - 90.0;

    float fadeX = position.x;
    if (fadeX <= frame.cameraUnzoomedX)
        fadeX += fadeMargin;
    else
        fadeX -= fadeMargin;

    float relative = frame.cameraViewWidth * 0.5;
    if (fadeX > relative + frame.cameraPositionX)
        relative = (relative - (fadeX - frame.cameraPositionX - relative)) * 0.014285714;
    else
        relative = (relative - (relative + frame.cameraPositionX - fadeX)) * 0.02;
    relative = clamp(relative, 0.0, 1.0);

    float width;
    if (fadeX <= centerRight + frame.cameraPositionX)
        width = (centerLeft + frame.cameraPositionX - fadeX) / max(centerLeft - 30.0, 1.0);
    else
        width = (fadeX - frame.cameraPositionX - centerRight) / max(screenLeft, 1.0);
    width = clamp(width, 0.0, 1.0);
    return float2(
        min(width * 0.95 + 0.05, relative),
        min(width * 0.85 + 0.15, relative)
    );
}

kernel void bismuthObjectCompute(
    device const SpriteWorkItem* workItems [[buffer(0)]],
    device const InputSprite* sprites [[buffer(1)]],
    device const ObjectInfo* objects [[buffer(2)]],
    device const GroupCombinationState* groups [[buffer(3)]],
    device const uint* colorChannels [[buffer(4)]],
    device const PulseEntry* pulses [[buffer(5)]],
    constant FrameState& frame [[buffer(6)]],
    texture2d<float, access::write> output [[texture(0)]],
    device atomic_uint* visibleCounter [[buffer(7)]],
    uint index [[thread_position_in_grid]]
) {
    if (index >= frame.spriteCount)
        return;

    SpriteWorkItem work = workItems[index];
    uint spriteIndex = work.spriteIndex;
    if (spriteIndex >= frame.spriteCount)
        return;

    float2 position = float2(work.startX, work.startY);
    float2x2 localTransform = float2x2(1.0);
    float4 color = float4(0.0);
    bool visible = work.groupCombinationIndex < frame.groupCount;

    GroupCombinationState group;
    if (visible) {
        group = groups[work.groupCombinationIndex];
        float2x2 groupTransform = float2x2(
            float2(group.positionalTransform.x, group.positionalTransform.y),
            float2(group.positionalTransform.z, group.positionalTransform.w)
        );
        position = groupTransform * position + group.offset;
    }

    float2 cameraOffset = position - float2(
        frame.cameraPositionX, frame.cameraPositionY
    );
    float2 screenPosition = float2(
        dot(float2(frame.cameraRightX, frame.cameraRightY), cameraOffset),
        dot(float2(frame.cameraUpX, frame.cameraUpY), cameraOffset)
    );

    constexpr float cullMargin = 100.0;
    visible = visible &&
        screenPosition.x >= -cullMargin &&
        screenPosition.x <= frame.cameraWidth + cullMargin &&
        screenPosition.y >= -cullMargin &&
        screenPosition.y <= frame.cameraHeight + cullMargin;

    if (visible) {
        InputSprite sprite = sprites[spriteIndex];
        visible = sprite.objectIndex < frame.objectCount;
        if (visible) {
            ObjectInfo object = objects[sprite.objectIndex];
            uint objectFlags = uint(object.flags);

            if ((objectFlags & FLAG_STATIC_OBJECT) == 0u) {
                localTransform = float2x2(
                    float2(group.localTransform.x, group.localTransform.y),
                    float2(group.localTransform.z, group.localTransform.w)
                );
            }

            if (object.rotationSpeed != 0.0)
                localTransform = rotationMatrix(
                    -object.rotationSpeed * frame.timer / 180.0 * PI
                ) * localTransform;

            if ((objectFlags & FLAG_AUDIO_SCALE) != 0u) {
                float scale = frame.audioScale;
                if ((objectFlags & FLAG_CUSTOM_AUDIO_SCALE) != 0u)
                    scale = (frame.audioScale - 0.1) *
                        (object.audioScaleMax - object.audioScaleMin) +
                        object.audioScaleMin;
                if ((objectFlags & FLAG_ORB) != 0u)
                    scale = min(scale + 0.3, 1.2);
                localTransform = scaleMatrix(float2(scale)) * localTransform;
            }

            if (frame.levelFlipProgress != 0.0 &&
                frame.levelFlipProgress != 1.0) {
                float zoom = max(frame.gameStateCameraZoom, 0.0001);
                float delta = frame.winSizeWidth / zoom -
                    2.0 * (position.x - frame.gameStateCameraPositionX);
                position.x += delta * frame.levelFlipProgress;
                if (frame.levelFlipProgress >= 0.5)
                    localTransform = scaleMatrix(float2(-1.0, 1.0)) * localTransform;
            }

            uint spriteType = sprite.spriteType;
            uint colorChannel = spriteType == SPRITE_DETAIL
                ? object.detailColorChannelId : object.baseColorChannelId;
            if (spriteType == SPRITE_GLOW &&
                (objectFlags & FLAG_GLOW_LBG) != 0u)
                colorChannel = COLOR_CHANNEL_LBG;
            colorChannel = min(colorChannel, max(frame.colorChannelCount, 1u) - 1u);

            if (colorChannel == COLOR_CHANNEL_LIGHTER) {
                uint baseChannel = min(
                    object.baseColorChannelId,
                    max(frame.colorChannelCount, 1u) - 1u
                );
                color = unpackRGBA(colorChannels[baseChannel]);
                HSVValue lighter = {0.0, 0.65, 0.15, false, true};
                color.rgb = applyHSV(lighter, color.rgb);
            } else {
                color = unpackRGBA(colorChannels[colorChannel]);
            }

            if (group.pulseIndex != 0u) {
                color.rgb = applyPulseEntries(
                    color.rgb,
                    group.pulseIndex,
                    spriteType != SPRITE_DETAIL,
                    pulses,
                    colorChannels,
                    frame
                );
            }

            if (spriteType == SPRITE_GLOW) {
                if ((objectFlags & FLAG_STATIC_GLOW) != 0u)
                    color = unpackRGBA(object.staticGlowColor);
                else if ((objectFlags & FLAG_SPECIAL_GLOW) != 0u)
                    color = frame.specialLightBGColor;
                color.a *= unpackRGBA(object.opacityValues).z;
            }

            if (spriteType == SPRITE_BASE &&
                (objectFlags & FLAG_BASE_HSV) != 0u) {
                color.rgb = applyHSV(
                    readHSV(object.baseHSVHueAndFlags, object.baseHSVSaturationAndValue),
                    color.rgb
                );
            } else if (spriteType == SPRITE_DETAIL &&
                (objectFlags & FLAG_DETAIL_HSV) != 0u) {
                color.rgb = applyHSV(
                    readHSV(object.detailHSVHueAndFlags, object.detailHSVSaturationAndValue),
                    color.rgb
                );
            }

            if ((objectFlags & FLAG_INVISIBLE_BLOCK) != 0u) {
                if ((frame.gameStateFlags & GAME_STATE_PLAYER_DEAD) != 0u) {
                    if (spriteType == SPRITE_GLOW)
                        color.rgb = frame.specialLightBGColor.rgb;
                } else {
                    float2 opacity = invisibleBlockOpacity(
                        position, object.fadeMargin, frame
                    );
                    if (spriteType != SPRITE_GLOW) {
                        color.a *= opacity.x;
                    } else {
                        float3 background = unpackRGBA(
                            colorChannels[COLOR_CHANNEL_BG]
                        ).rgb;
                        float3 lightBackground = unpackRGBA(
                            colorChannels[COLOR_CHANNEL_LBG]
                        ).rgb;
                        float3 base = dot(background, float3(1.0)) >= (150.0 / 255.0)
                            ? float3(1.0) : lightBackground;
                        float3 glow = opacity.x <= 0.8
                            ? frame.specialLightBGColor.rgb
                            : mix(base, frame.specialLightBGColor.rgb,
                                (1.0 - (opacity.x - 0.8) / 0.2) * 0.3 + 0.7);
                        color = float4(glow, color.a * opacity.y);
                    }
                }
            }

            if (spriteType == SPRITE_BLACK || spriteType == SPRITE_BLACK_CHILD)
                color.rgb = float3(0.0);

            float4 opacityValues = unpackRGBA(object.opacityValues);
            color.a *= group.opacity * opacityValues.x;
            color.rgb *= color.a;
            if (spriteType == SPRITE_BLACK_CHILD)
                color.a *= opacityValues.y;

            visible = color.a > 0.0;
        }
    }

    if (!visible) {
        color = float4(0.0);
        localTransform = float2x2(1.0);
    } else {
        atomic_fetch_add_explicit(visibleCounter, 1u, memory_order_relaxed);
    }

    uint texel = spriteIndex * 3u;
    uint width = output.get_width();
    uint2 p0 = uint2(texel % width, texel / width);
    uint2 p1 = uint2((texel + 1u) % width, (texel + 1u) / width);
    uint2 p2 = uint2((texel + 2u) % width, (texel + 2u) / width);

    output.write(float4(position, localTransform[0].x, localTransform[0].y), p0);
    output.write(float4(localTransform[1].x, localTransform[1].y, color.r, color.g), p1);
    output.write(float4(color.b, color.a, visible ? 1.0 : 0.0, 0.0), p2);
}
)METAL"];
}

void setError(std::string& output, const char* prefix, NSError* error = nil) {
    output = prefix;
    if (error && error.localizedDescription.UTF8String) {
        output += ": ";
        output += error.localizedDescription.UTF8String;
    }
}

bool hasExtension(const char* extensions, const char* wanted) {
    if (!extensions || !wanted || !*wanted)
        return false;
    std::string list(extensions);
    std::string value(wanted);
    std::size_t at = 0;
    while ((at = list.find(value, at)) != std::string::npos) {
        bool left = at == 0 || list[at - 1] == ' ';
        std::size_t end = at + value.size();
        bool right = end == list.size() || list[end] == ' ';
        if (left && right)
            return true;
        at = end;
    }
    return false;
}

} // namespace

struct OutputSlot {
    CVPixelBufferRef pixelBuffer = nullptr;
    CVMetalTextureRef metalTextureRef = nullptr;
    CVOpenGLESTextureRef glTextureRef = nullptr;
    __strong id<MTLTexture> metalTexture = nil;
    __strong id<MTLBuffer> groupBuffer = nil;
    __strong id<MTLBuffer> colorBuffer = nil;
    __strong id<MTLBuffer> pulseBuffer = nil;
    __strong id<MTLBuffer> visibleCounterBuffer = nil;
    NSUInteger groupCapacity = 0;
    NSUInteger colorCapacity = 0;
    NSUInteger pulseCapacity = 0;
    NSUInteger visibleCounterCapacity = 0;
    std::uint32_t visibleSpriteCount = 0;
    std::uint32_t totalSpriteCount = 0;
    std::uint32_t threadgroupCount = 0;
    std::uint32_t threadsPerThreadgroup = 0;
    double gpuTimeMilliseconds = 0.0;
    bool hasGpuTiming = false;
    std::atomic<bool> inFlight {false};
    std::atomic<bool> ready {false};
    std::atomic<bool> leased {false};
    GLsync readFence = nullptr;
    std::uint64_t sequence = 0;

    void destroy(bool canDeleteGLSync) {
        if (readFence && canDeleteGLSync)
            glDeleteSyncAPPLE(readFence);
        readFence = nullptr;
        if (glTextureRef) {
            CFRelease(glTextureRef);
            glTextureRef = nullptr;
        }
        if (metalTextureRef) {
            CFRelease(metalTextureRef);
            metalTextureRef = nullptr;
        }
        if (pixelBuffer) {
            CVPixelBufferRelease(pixelBuffer);
            pixelBuffer = nullptr;
        }
        metalTexture = nil;
        groupBuffer = nil;
        colorBuffer = nil;
        pulseBuffer = nil;
        visibleCounterBuffer = nil;
        visibleSpriteCount = 0;
        totalSpriteCount = 0;
        threadgroupCount = 0;
        threadsPerThreadgroup = 0;
        gpuTimeMilliseconds = 0.0;
        hasGpuTiming = false;
        inFlight.store(false);
        ready.store(false);
        leased.store(false);
    }
};


} // namespace BismuthIOSMetal

@interface BismuthIOSMetalOwner : NSObject {
@public
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id<MTLComputePipelineState> computePipeline;
    CVMetalTextureCacheRef metalTextureCache;
    CVOpenGLESTextureCacheRef openGLESTextureCache;
    EAGLContext* openGLESContext;

    id<MTLBuffer> workItemBuffer;
    id<MTLBuffer> inputSpriteBuffer;
    id<MTLBuffer> objectInfoBuffer;

    BismuthIOSMetal::OutputSlot slots[BismuthIOSMetal::SLOT_COUNT];
    NSUInteger textureWidth;
    NSUInteger textureHeight;
    NSUInteger spriteCount;
    NSUInteger objectCount;
    NSInteger lastPresentedSlot;
    std::uint64_t nextSequence;
    std::mutex errorMutex;
    std::string asynchronousError;
}
@end

@implementation BismuthIOSMetalOwner
- (instancetype)init {
    self = [super init];
    if (self) {
        metalTextureCache = nullptr;
        openGLESTextureCache = nullptr;
        openGLESContext = nil;
        textureWidth = 0;
        textureHeight = 0;
        spriteCount = 0;
        objectCount = 0;
        lastPresentedSlot = -1;
        nextSequence = 0;
    }
    return self;
}

- (void)dealloc {
    const bool canDeleteGL = openGLESContext &&
        [EAGLContext currentContext] == openGLESContext;
    for (auto& slot : slots)
        slot.destroy(canDeleteGL);
    if (openGLESTextureCache) {
        CFRelease(openGLESTextureCache);
        openGLESTextureCache = nullptr;
    }
    if (metalTextureCache) {
        CFRelease(metalTextureCache);
        metalTextureCache = nullptr;
    }
    workItemBuffer = nil;
    inputSpriteBuffer = nil;
    objectInfoBuffer = nil;
    computePipeline = nil;
    commandQueue = nil;
    device = nil;
}
@end
namespace BismuthIOSMetal {
namespace {


bool ensureSharedBuffer(
    id<MTLDevice> device,
    __strong id<MTLBuffer>& buffer,
    NSUInteger& capacity,
    NSUInteger required,
    std::string& error,
    const char* label
) {
    required = std::max<NSUInteger>(required, 16);
    if (buffer && capacity >= required)
        return true;
    id<MTLBuffer> replacement = [device newBufferWithLength:required
        options:MTLResourceStorageModeShared];
    if (!replacement) {
        error = std::string("Metal could not allocate ") + label;
        return false;
    }
    buffer = replacement;
    capacity = required;
    return true;
}

bool outputFenceComplete(OutputSlot& slot) {
    if (!slot.readFence)
        return true;
    const GLenum result = glClientWaitSyncAPPLE(slot.readFence, 0, 0);
    if (result == GL_ALREADY_SIGNALED_APPLE ||
        result == GL_CONDITION_SATISFIED_APPLE) {
        glDeleteSyncAPPLE(slot.readFence);
        slot.readFence = nullptr;
        return true;
    }
    return false;
}

bool createOutputSlot(
    BismuthIOSMetalOwner* owner,
    OutputSlot& slot,
    std::string& error
) {
    NSDictionary* attributes = @{
        (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey : @{},
        (__bridge NSString*)kCVPixelBufferMetalCompatibilityKey : @YES,
        (__bridge NSString*)kCVPixelBufferOpenGLESCompatibilityKey : @YES
    };

    CVReturn status = CVPixelBufferCreate(
        kCFAllocatorDefault,
        owner->textureWidth,
        owner->textureHeight,
        kCVPixelFormatType_128RGBAFloat,
        (__bridge CFDictionaryRef)attributes,
        &slot.pixelBuffer
    );
    if (status != kCVReturnSuccess || !slot.pixelBuffer) {
        error = "Core Video could not allocate an interoperable RGBA32Float pixel buffer";
        return false;
    }

    status = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault,
        owner->metalTextureCache,
        slot.pixelBuffer,
        nullptr,
        MTLPixelFormatRGBA32Float,
        owner->textureWidth,
        owner->textureHeight,
        0,
        &slot.metalTextureRef
    );
    if (status != kCVReturnSuccess || !slot.metalTextureRef) {
        error = "Core Video could not create the Metal RGBA32Float view";
        return false;
    }
    slot.metalTexture = CVMetalTextureGetTexture(slot.metalTextureRef);
    if (!slot.metalTexture) {
        error = "Core Video returned a null Metal output texture";
        return false;
    }

    status = CVOpenGLESTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault,
        owner->openGLESTextureCache,
        slot.pixelBuffer,
        nullptr,
        GL_TEXTURE_2D,
        GL_RGBA,
        owner->textureWidth,
        owner->textureHeight,
        GL_RGBA,
        GL_FLOAT,
        0,
        &slot.glTextureRef
    );
    if (status != kCVReturnSuccess || !slot.glTextureRef) {
        error = "Core Video could not create the OpenGL ES RGBA32Float view";
        return false;
    }

    const GLuint textureName = CVOpenGLESTextureGetName(slot.glTextureRef);
    if (textureName == 0 || !glIsTexture(textureName)) {
        error = "OpenGL ES rejected the shared Metal output texture";
        return false;
    }

    GLint previousBinding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousBinding);
    glBindTexture(GL_TEXTURE_2D, textureName);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLenum glError = glGetError();
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousBinding));
    if (glError != GL_NO_ERROR || glGetError() != GL_NO_ERROR) {
        error = "OpenGL ES could not configure the shared Metal output texture";
        return false;
    }
    return true;
}

} // namespace

ComputeRenderer::~ComputeRenderer() {
    reset();
}

void ComputeRenderer::reset() {
    if (!m_owner)
        return;
    BismuthIOSMetalOwner* owner =
        (__bridge_transfer BismuthIOSMetalOwner*)m_owner;
    m_owner = nullptr;
    (void)owner;
}

bool ComputeRenderer::isReady() const {
    return m_owner != nullptr;
}

std::string ComputeRenderer::getDeviceName() const {
    if (!m_owner)
        return "not initialized";
    BismuthIOSMetalOwner* owner = (__bridge BismuthIOSMetalOwner*)m_owner;
    const char* name = owner->device.name.UTF8String;
    return name ? std::string(name) : "unknown Metal device";
}

std::string ComputeRenderer::takeLastError() {
    if (!m_owner)
        return {};
    BismuthIOSMetalOwner* owner = (__bridge BismuthIOSMetalOwner*)m_owner;
    std::lock_guard<std::mutex> lock(owner->errorMutex);
    std::string error = std::move(owner->asynchronousError);
    owner->asynchronousError.clear();
    return error;
}

bool ComputeRenderer::initializeLevel(
    const SpriteWorkItem* workItems, std::uint32_t workItemCount,
    const InputSprite* sprites, std::uint32_t spriteCount,
    const ObjectInfo* objects, std::uint32_t objectCount,
    std::string& error
) {
    reset();
    error.clear();

    if (!workItems || !sprites || !objects ||
        workItemCount == 0 || spriteCount == 0 || objectCount == 0 ||
        workItemCount != spriteCount) {
        error = "Static level input counts are empty or inconsistent";
        return false;
    }

    EAGLContext* currentContext = [EAGLContext currentContext];
    if (!currentContext) {
        error = "Metal/OpenGL ES initialization requires the game's current EAGLContext";
        return false;
    }

    const char* extensionList =
        reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!hasExtension(extensionList, "GL_OES_texture_float")) {
        error = "This GLES context does not support sampling RGBA32Float state textures";
        return false;
    }
    if (!hasExtension(extensionList, "GL_APPLE_sync")) {
        error = "GL_APPLE_sync is required for non-blocking output texture reuse";
        return false;
    }

    GLint maxTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    if (maxTextureSize < 64) {
        error = "The GLES maximum texture size is too small for sprite state";
        return false;
    }

    const std::uint64_t totalTexels =
        static_cast<std::uint64_t>(workItemCount) * 3u;
    const NSUInteger width = static_cast<NSUInteger>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(maxTextureSize), 2048u)
    );
    const std::uint64_t height =
        std::max<std::uint64_t>(1u, (totalTexels + width - 1u) / width);
    if (height > static_cast<std::uint64_t>(maxTextureSize)) {
        error = "The static level needs a sprite-state texture larger than GL_MAX_TEXTURE_SIZE";
        return false;
    }

    BismuthIOSMetalOwner* owner = [[BismuthIOSMetalOwner alloc] init];
    owner->openGLESContext = currentContext;
    owner->device = MTLCreateSystemDefaultDevice();
    if (!owner->device) {
        error = "Metal is not available on this device";
        return false;
    }
    owner->commandQueue = [owner->device newCommandQueue];
    if (!owner->commandQueue) {
        error = "Metal could not create a command queue";
        return false;
    }

    owner->textureWidth = width;
    owner->textureHeight = static_cast<NSUInteger>(height);
    owner->spriteCount = spriteCount;
    owner->objectCount = objectCount;

    CVReturn status = CVMetalTextureCacheCreate(
        kCFAllocatorDefault, nullptr, owner->device, nullptr,
        &owner->metalTextureCache
    );
    if (status != kCVReturnSuccess || !owner->metalTextureCache) {
        error = "Could not create the Core Video Metal texture cache";
        return false;
    }

    status = CVOpenGLESTextureCacheCreate(
        kCFAllocatorDefault, nullptr, currentContext, nullptr,
        &owner->openGLESTextureCache
    );
    if (status != kCVReturnSuccess || !owner->openGLESTextureCache) {
        error = "Could not create the Core Video OpenGL ES texture cache";
        return false;
    }

    owner->workItemBuffer = [owner->device newBufferWithBytes:workItems
        length:static_cast<NSUInteger>(workItemCount) * sizeof(SpriteWorkItem)
        options:MTLResourceStorageModeShared];
    owner->inputSpriteBuffer = [owner->device newBufferWithBytes:sprites
        length:static_cast<NSUInteger>(spriteCount) * sizeof(InputSprite)
        options:MTLResourceStorageModeShared];
    owner->objectInfoBuffer = [owner->device newBufferWithBytes:objects
        length:static_cast<NSUInteger>(objectCount) * sizeof(ObjectInfo)
        options:MTLResourceStorageModeShared];
    if (!owner->workItemBuffer || !owner->inputSpriteBuffer || !owner->objectInfoBuffer) {
        error = "Metal could not upload immutable per-level object and sprite records";
        return false;
    }

    NSError* metalError = nil;
    id<MTLLibrary> library = [owner->device newLibraryWithSource:computeSource()
        options:nil error:&metalError];
    if (!library) {
        setError(error, "Metal failed to compile the Bismuth object compute kernel", metalError);
        return false;
    }

    id<MTLFunction> function = [library newFunctionWithName:@"bismuthObjectCompute"];
    if (!function) {
        error = "Metal could not find the bismuthObjectCompute function";
        return false;
    }
    owner->computePipeline = [owner->device newComputePipelineStateWithFunction:function
        error:&metalError];
    if (!owner->computePipeline) {
        setError(error, "Metal failed to create the Bismuth object compute pipeline", metalError);
        return false;
    }

    for (auto& slot : owner->slots) {
        if (!createOutputSlot(owner, slot, error))
            return false;
    }

    m_owner = (__bridge_retained void*)owner;
    return true;
}

bool ComputeRenderer::dispatchFrame(
    const GroupCombinationState* groupStates, std::uint32_t groupCount,
    const std::uint32_t* colorChannels, std::uint32_t colorChannelCount,
    const PulseEntry* pulseEntries, std::uint32_t pulseCount,
    const FrameState& frameState,
    std::string& error
) {
    error.clear();
    if (!m_owner) {
        error = "The immutable level buffers have not been initialized";
        return false;
    }

    BismuthIOSMetalOwner* owner = (__bridge BismuthIOSMetalOwner*)m_owner;
    if ([EAGLContext currentContext] != owner->openGLESContext) {
        error = "The game's original EAGLContext is not current for Metal dispatch";
        return false;
    }
    if (!groupStates || groupCount == 0 ||
        !colorChannels || colorChannelCount == 0 ||
        (!pulseEntries && pulseCount != 0)) {
        error = "Compact group, color, or pulse state is invalid";
        return false;
    }

    NSInteger selectedSlot = -1;
    for (NSUInteger i = 0; i < SLOT_COUNT; ++i) {
        if (static_cast<NSInteger>(i) == owner->lastPresentedSlot)
            continue;
        OutputSlot& slot = owner->slots[i];
        if (slot.inFlight.load() || slot.ready.load() || slot.leased.load())
            continue;
        if (!outputFenceComplete(slot))
            continue;
        selectedSlot = static_cast<NSInteger>(i);
        break;
    }
    if (selectedSlot < 0) {
        error = "No free Metal output slot; this frame is skipped without waiting";
        return false;
    }

    OutputSlot& slot = owner->slots[selectedSlot];
    const NSUInteger groupBytes =
        static_cast<NSUInteger>(groupCount) * sizeof(GroupCombinationState);
    const NSUInteger colorBytes =
        static_cast<NSUInteger>(colorChannelCount) * sizeof(std::uint32_t);
    const NSUInteger pulseBytes =
        static_cast<NSUInteger>(pulseCount) * sizeof(PulseEntry);

    if (!ensureSharedBuffer(owner->device, slot.groupBuffer,
            slot.groupCapacity, groupBytes, error, "group-combination state") ||
        !ensureSharedBuffer(owner->device, slot.colorBuffer,
            slot.colorCapacity, colorBytes, error, "color-channel state") ||
        !ensureSharedBuffer(owner->device, slot.pulseBuffer,
            slot.pulseCapacity, pulseBytes, error, "pulse-entry state") ||
        !ensureSharedBuffer(owner->device, slot.visibleCounterBuffer,
            slot.visibleCounterCapacity, sizeof(std::uint32_t), error, "GPU visibility counter"))
        return false;

    *static_cast<std::uint32_t*>(slot.visibleCounterBuffer.contents) = 0u;
    slot.visibleSpriteCount = 0;
    slot.totalSpriteCount = static_cast<std::uint32_t>(owner->spriteCount);
    slot.gpuTimeMilliseconds = 0.0;
    slot.hasGpuTiming = false;

    std::memcpy(slot.groupBuffer.contents, groupStates, groupBytes);
    std::memcpy(slot.colorBuffer.contents, colorChannels, colorBytes);
    if (pulseBytes)
        std::memcpy(slot.pulseBuffer.contents, pulseEntries, pulseBytes);

    FrameState parameters = frameState;
    parameters.spriteCount = static_cast<std::uint32_t>(owner->spriteCount);
    parameters.objectCount = static_cast<std::uint32_t>(owner->objectCount);
    parameters.groupCount = groupCount;
    parameters.colorChannelCount = std::min<std::uint32_t>(
        colorChannelCount, 1101u
    );
    parameters.pulseCount = pulseCount;

    id<MTLCommandBuffer> commandBuffer = [owner->commandQueue commandBuffer];
    if (!commandBuffer) {
        error = "Metal could not create a command buffer";
        return false;
    }
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    if (!encoder) {
        error = "Metal could not create a compute encoder";
        return false;
    }

    [encoder setComputePipelineState:owner->computePipeline];
    [encoder setBuffer:owner->workItemBuffer offset:0 atIndex:0];
    [encoder setBuffer:owner->inputSpriteBuffer offset:0 atIndex:1];
    [encoder setBuffer:owner->objectInfoBuffer offset:0 atIndex:2];
    [encoder setBuffer:slot.groupBuffer offset:0 atIndex:3];
    [encoder setBuffer:slot.colorBuffer offset:0 atIndex:4];
    [encoder setBuffer:slot.pulseBuffer offset:0 atIndex:5];
    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:6];
    [encoder setTexture:slot.metalTexture atIndex:0];
    [encoder setBuffer:slot.visibleCounterBuffer offset:0 atIndex:7];

    const NSUInteger threadWidth = std::max<NSUInteger>(
        1, owner->computePipeline.threadExecutionWidth
    );
    const NSUInteger threadgroups =
        (owner->spriteCount + threadWidth - 1) / threadWidth;
    slot.threadgroupCount = static_cast<std::uint32_t>(threadgroups);
    slot.threadsPerThreadgroup = static_cast<std::uint32_t>(threadWidth);
    [encoder dispatchThreadgroups:MTLSizeMake(threadgroups, 1, 1)
             threadsPerThreadgroup:MTLSizeMake(threadWidth, 1, 1)];
    [encoder endEncoding];

    slot.sequence = ++owner->nextSequence;
    slot.ready.store(false);
    slot.inFlight.store(true);

    const NSInteger slotIndex = selectedSlot;
    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        OutputSlot& finished = owner->slots[slotIndex];
        if (completed.status == MTLCommandBufferStatusCompleted) {
            const double gpuStart = completed.GPUStartTime;
            const double gpuEnd = completed.GPUEndTime;
            finished.hasGpuTiming = gpuEnd > gpuStart && gpuStart > 0.0;
            finished.gpuTimeMilliseconds = finished.hasGpuTiming
                ? (gpuEnd - gpuStart) * 1000.0
                : 0.0;
            finished.visibleSpriteCount =
                *static_cast<const std::uint32_t*>(finished.visibleCounterBuffer.contents);
            finished.ready.store(true);
        } else {
            std::lock_guard<std::mutex> lock(owner->errorMutex);
            owner->asynchronousError =
                completed.error.localizedDescription.UTF8String
                ? completed.error.localizedDescription.UTF8String
                : "Metal object-compute command failed";
        }
        finished.inFlight.store(false);
    }];

    [commandBuffer commit];
    return true;
}

bool ComputeRenderer::acquireLatestFrame(OutputFrame& output) {
    output = {};
    if (!m_owner)
        return false;

    BismuthIOSMetalOwner* owner = (__bridge BismuthIOSMetalOwner*)m_owner;
    if ([EAGLContext currentContext] != owner->openGLESContext)
        return false;

    NSInteger selectedSlot = -1;
    std::uint64_t newestSequence = 0;
    for (NSUInteger i = 0; i < SLOT_COUNT; ++i) {
        OutputSlot& slot = owner->slots[i];
        if (slot.ready.load() && !slot.inFlight.load() &&
            slot.sequence >= newestSequence) {
            selectedSlot = static_cast<NSInteger>(i);
            newestSequence = slot.sequence;
        }
    }

    if (selectedSlot >= 0) {
        for (NSUInteger i = 0; i < SLOT_COUNT; ++i) {
            if (static_cast<NSInteger>(i) != selectedSlot)
                owner->slots[i].ready.store(false);
        }
        owner->lastPresentedSlot = selectedSlot;
        owner->slots[selectedSlot].ready.store(false);
    } else {
        selectedSlot = owner->lastPresentedSlot;
    }

    if (selectedSlot < 0)
        return false;

    OutputSlot& slot = owner->slots[selectedSlot];
    if (slot.leased.load())
        return false;

    slot.leased.store(true);
    CVOpenGLESTextureCacheFlush(owner->openGLESTextureCache, 0);
    output.textureName = CVOpenGLESTextureGetName(slot.glTextureRef);
    output.slot = static_cast<std::uint32_t>(selectedSlot);
    output.sequence = slot.sequence;
    output.textureWidth = static_cast<std::uint32_t>(owner->textureWidth);
    output.textureHeight = static_cast<std::uint32_t>(owner->textureHeight);
    output.visibleSpriteCount = slot.visibleSpriteCount;
    output.totalSpriteCount = slot.totalSpriteCount;
    output.threadgroupCount = slot.threadgroupCount;
    output.threadsPerThreadgroup = slot.threadsPerThreadgroup;
    output.gpuTimeMilliseconds = slot.gpuTimeMilliseconds;
    output.hasGpuTiming = slot.hasGpuTiming;
    return output.textureName != 0;
}

bool ComputeRenderer::finishOpenGLRead(
    const OutputFrame& output,
    std::string& error
) {
    error.clear();
    if (!m_owner || output.slot >= SLOT_COUNT) {
        error = "Invalid shared Metal output frame";
        return false;
    }

    BismuthIOSMetalOwner* owner = (__bridge BismuthIOSMetalOwner*)m_owner;
    if ([EAGLContext currentContext] != owner->openGLESContext) {
        error = "The original EAGLContext is not current after the GLES draw";
        return false;
    }

    OutputSlot& slot = owner->slots[output.slot];
    if (!slot.leased.load() ||
        CVOpenGLESTextureGetName(slot.glTextureRef) != output.textureName) {
        error = "The shared output slot was not leased by this frame";
        return false;
    }

    if (slot.readFence) {
        glDeleteSyncAPPLE(slot.readFence);
        slot.readFence = nullptr;
    }
    slot.readFence = glFenceSyncAPPLE(GL_SYNC_GPU_COMMANDS_COMPLETE_APPLE, 0);
    if (!slot.readFence) {
        error = "OpenGL ES could not create its output-read fence";
        return false;
    }

    slot.leased.store(false);
    glFlush();
    return true;
}

} // namespace BismuthIOSMetal
