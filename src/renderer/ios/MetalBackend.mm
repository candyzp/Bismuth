#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

#include "MetalBackend.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <vector>

@interface KyaniteMetalViewDelegate : NSObject <MTKViewDelegate>
@end

namespace {
constexpr std::uint32_t PROBE_THREAD_COUNT = 256;
constexpr std::uint32_t THREADS_PER_GROUP = 64;
constexpr std::uint32_t MAX_GROUP_ID = 9999;
constexpr float PI_F = 3.14159265358979323846f;

std::string toStdString(NSString* value) {
    if (!value)
        return {};
    const char* utf8 = [value UTF8String];
    return utf8 ? std::string(utf8) : std::string();
}

struct MetalGpuObject {
    float x = 0.0f;
    float y = 0.0f;
    float halfWidth = 1.0f;
    float halfHeight = 1.0f;
    float rotation = 0.0f;
    std::uint32_t groupCombinationIndex = 0;
    std::uint32_t flags = 0;
};

struct MetalGroupState {
    float positionM00 = 1.0f;
    float positionM01 = 0.0f;
    float positionM10 = 0.0f;
    float positionM11 = 1.0f;

    float localM00 = 1.0f;
    float localM01 = 0.0f;
    float localM10 = 0.0f;
    float localM11 = 1.0f;

    float offsetX = 0.0f;
    float offsetY = 0.0f;
    float alpha = 1.0f;
    std::uint32_t visible = 1;
};

struct MetalCameraBounds {
    float minX = -100000.0f;
    float minY = -100000.0f;
    float maxX = 100000.0f;
    float maxY = 100000.0f;
};

static_assert(sizeof(MetalSpriteVertex) == 36,
              "MetalSpriteVertex layout must match the MSL vertex layout");
static_assert(sizeof(MetalObjectInput) == 68,
              "MetalObjectInput layout must match the CPU metadata layout");
static_assert(sizeof(MetalGpuObject) == 28,
              "MetalGpuObject layout must match the MSL object layout");
static_assert(sizeof(MetalGroupState) == 48,
              "MetalGroupState layout must match the MSL group-state layout");
static_assert(sizeof(MetalCameraBounds) == 16,
              "MetalCameraBounds layout must match the MSL camera layout");

void multiplyByRotation(
    float& m00, float& m01, float& m10, float& m11,
    float cosine, float sine
) {
    const float old00 = m00;
    const float old01 = m01;
    const float old10 = m10;
    const float old11 = m11;

    // Matches Kyanite's GLM column-major transform composition.
    m00 = old00 * cosine - old01 * sine;
    m01 = old00 * sine + old01 * cosine;
    m10 = old10 * cosine - old11 * sine;
    m11 = old10 * sine + old11 * cosine;
}
} // namespace

struct MetalBackend::Impl {
    __strong id<MTLDevice> device = nil;
    __strong id<MTLCommandQueue> commandQueue = nil;
    __strong id<MTLComputePipelineState> probePipeline = nil;
    __strong id<MTLComputePipelineState> cullingPipeline = nil;
    __strong id<MTLRenderPipelineState> spriteRenderPipeline = nil;
    __strong id<MTLRenderPipelineState> additiveSpriteRenderPipeline = nil;
    __strong id<MTLSamplerState> spriteSampler = nil;
    __strong id<MTLBuffer> spriteVertexBuffer = nil;
    __strong id<MTLBuffer> visibleMaskBuffer = nil;
    __strong NSMutableArray<id<MTLTexture>>* spriteTextures = nil;
    __strong MTKView* spriteView = nil;
    __strong KyaniteMetalViewDelegate* spriteViewDelegate = nil;
    std::vector<MetalSpriteBatch> spriteBatches;
    std::atomic<std::uint32_t> renderedFrames{0};

    __strong id<MTLBuffer> probeCounter = nil;
    __strong id<MTLBuffer> objectBuffer = nil;
    __strong id<MTLBuffer> groupStateBuffer = nil;
    __strong id<MTLBuffer> cameraBuffer = nil;
    __strong id<MTLBuffer> visibleIndexBuffer = nil;
    __strong id<MTLBuffer> visibleCountBuffer = nil;

    mutable std::mutex textMutex;
    std::string deviceName;
    std::string error;

    std::vector<MetalGroupState> groupStates;
    std::vector<std::vector<std::uint32_t>> groupIdsPerCombination;
    std::vector<std::vector<std::uint32_t>> combinationsPerGroupId;
    std::vector<std::uint32_t> usedGroupIds;
    std::vector<std::uint8_t> groupToggleEnabled;
    std::vector<float> groupAlpha;
    MetalCameraBounds cameraBounds;

    std::atomic<std::uint32_t> objectCount{0};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<bool> levelDataReady{false};
    std::atomic<bool> frameInFlight{false};
    std::atomic<bool> groupStateDirty{false};

    std::atomic<bool> probeSubmitted{false};
    std::atomic<bool> probeCompleted{false};
    std::atomic<bool> probeFailed{false};
    std::atomic<std::uint32_t> completedProbeThreads{0};
    std::atomic<std::uint32_t> submittedDispatches{0};

    std::atomic<bool> cullingSubmitted{false};
    std::atomic<bool> cullingCompleted{false};
    std::atomic<bool> cullingFailed{false};
    std::atomic<std::uint32_t> visibleObjectCount{0};
    std::atomic<std::uint32_t> culledObjectCount{0};
    std::atomic<std::uint32_t> submittedCullingFrames{0};
    std::atomic<std::uint32_t> completedCullingFrames{0};
    std::atomic<std::uint32_t> skippedBusyFrames{0};
    std::atomic<std::uint32_t> invalidVisibleIndices{0};
    std::atomic<std::uint32_t> groupStateUpdates{0};

    std::atomic<bool> gpuTimeAvailable{false};
    std::atomic<double> gpuTimeMilliseconds{0.0};
    std::atomic<bool> cullingGpuTimeAvailable{false};
    std::atomic<double> cullingGpuTimeMilliseconds{0.0};

    void recomputeCombination(std::uint32_t combinationIndex) {
        if (combinationIndex >= groupStates.size() ||
            combinationIndex >= groupIdsPerCombination.size())
            return;

        bool visible = true;
        float alpha = 1.0f;

        for (const auto groupId : groupIdsPerCombination[combinationIndex]) {
            if (groupId < groupToggleEnabled.size() &&
                groupToggleEnabled[groupId] == 0)
                visible = false;

            if (groupId < groupAlpha.size())
                alpha *= groupAlpha[groupId];
        }

        groupStates[combinationIndex].visible = visible ? 1u : 0u;
        groupStates[combinationIndex].alpha = std::clamp(alpha, 0.0f, 1.0f);
    }
};

MetalBackend::MetalBackend()
  : m_impl(std::make_unique<Impl>()) {}

MetalBackend::~MetalBackend() = default;

MetalBackend& MetalBackend::get() {
    static MetalBackend backend;
    return backend;
}

bool MetalBackend::initialize() {
    @autoreleasepool {
        if (m_impl->device && m_impl->commandQueue &&
            m_impl->probePipeline && m_impl->cullingPipeline &&
            m_impl->probeCounter)
            return true;

        m_impl->device = MTLCreateSystemDefaultDevice();
        if (!m_impl->device) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "MTLCreateSystemDefaultDevice returned nil";
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->deviceName = toStdString(m_impl->device.name);
            m_impl->error.clear();
        }

        m_impl->commandQueue = [m_impl->device newCommandQueue];
        if (!m_impl->commandQueue) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal command queue creation failed";
            return false;
        }

        const char* source = R"METAL(
            #include <metal_stdlib>
            using namespace metal;

            struct KyaniteObject {
                float x;
                float y;
                float halfWidth;
                float halfHeight;
                float rotation;
                uint groupCombinationIndex;
                uint flags;
            };

            struct KyaniteGroupState {
                float positionM00;
                float positionM01;
                float positionM10;
                float positionM11;
                float localM00;
                float localM01;
                float localM10;
                float localM11;
                float offsetX;
                float offsetY;
                float alpha;
                uint visible;
            };

            struct KyaniteCameraBounds {
                float minX;
                float minY;
                float maxX;
                float maxY;
            };

            struct KyaniteSpriteVertex {
                float offsetX;
                float offsetY;
                float u;
                float v;
                uint objectIndex;
                float r;
                float g;
                float b;
                float a;
            };

            struct KyaniteSpriteVarying {
                float4 position [[position]];
                float2 uv;
                float4 color;
            };

            vertex KyaniteSpriteVarying kyanite_sprite_vertex(
                const device KyaniteSpriteVertex* vertices [[buffer(0)]],
                const device KyaniteObject* objects [[buffer(1)]],
                const device KyaniteGroupState* combinations [[buffer(2)]],
                const device uint* visibleMask [[buffer(3)]],
                constant KyaniteCameraBounds& camera [[buffer(4)]],
                uint vertexId [[vertex_id]]
            ) {
                const KyaniteSpriteVertex vertex = vertices[vertexId];
                KyaniteSpriteVarying out;
                if (visibleMask[vertex.objectIndex] == 0u) {
                    out.position = float4(3.0, 3.0, 0.0, 1.0);
                    out.uv = float2(vertex.u, vertex.v);
                    out.color = float4(0.0);
                    return out;
                }

                const KyaniteObject object = objects[vertex.objectIndex];
                const KyaniteGroupState state =
                    combinations[object.groupCombinationIndex];
                const float c = cos(object.rotation);
                const float s = sin(object.rotation);
                const float2 localOffset = float2(
                    c * vertex.offsetX - s * vertex.offsetY,
                    s * vertex.offsetX + c * vertex.offsetY
                );
                const float2 transformedOffset = float2(
                    state.localM00 * localOffset.x + state.localM01 * localOffset.y,
                    state.localM10 * localOffset.x + state.localM11 * localOffset.y
                );
                const float2 center = float2(
                    state.positionM00 * object.x +
                        state.positionM01 * object.y + state.offsetX,
                    state.positionM10 * object.x +
                        state.positionM11 * object.y + state.offsetY
                );
                const float2 world = center + transformedOffset;
                const float width = max(1.0, camera.maxX - camera.minX);
                const float height = max(1.0, camera.maxY - camera.minY);
                const float2 normalized = float2(
                    (world.x - camera.minX) / width,
                    (world.y - camera.minY) / height
                );
                out.position = float4(
                    normalized.x * 2.0 - 1.0,
                    normalized.y * 2.0 - 1.0,
                    0.0,
                    1.0
                );
                out.uv = float2(vertex.u, vertex.v);
                out.color = float4(vertex.r, vertex.g, vertex.b, vertex.a);
                return out;
            }

            fragment float4 kyanite_sprite_fragment(
                KyaniteSpriteVarying in [[stage_in]],
                texture2d<float> atlas [[texture(0)]],
                sampler atlasSampler [[sampler(0)]]
            ) {
                return atlas.sample(atlasSampler, in.uv) * in.color;
            }

            kernel void kyanite_probe(
                device atomic_uint* counter [[buffer(0)]],
                uint tid [[thread_position_in_grid]]
            ) {
                atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
            }

            kernel void kyanite_cull_objects(
                device const KyaniteObject* objects [[buffer(0)]],
                device const KyaniteGroupState* combinations [[buffer(1)]],
                constant KyaniteCameraBounds& camera [[buffer(2)]],
                device uint* visibleIndices [[buffer(3)]],
                device atomic_uint* visibleCount [[buffer(4)]],
                constant uint& objectCount [[buffer(5)]],
                device uint* visibleMask [[buffer(6)]],
                uint tid [[thread_position_in_grid]]
            ) {
                if (tid >= objectCount)
                    return;

                visibleMask[tid] = 0u;
                const KyaniteObject object = objects[tid];
                const KyaniteGroupState state =
                    combinations[object.groupCombinationIndex];

                if (state.visible == 0u || state.alpha <= 0.001f)
                    return;

                float2 center = float2(
                    state.positionM00 * object.x +
                        state.positionM01 * object.y + state.offsetX,
                    state.positionM10 * object.x +
                        state.positionM11 * object.y + state.offsetY
                );

                float c = cos(object.rotation);
                float s = sin(object.rotation);
                float2 axisX = float2(c, -s);
                float2 axisY = float2(s, c);

                axisX = float2(
                    state.localM00 * axisX.x + state.localM01 * axisX.y,
                    state.localM10 * axisX.x + state.localM11 * axisX.y
                );
                axisY = float2(
                    state.localM00 * axisY.x + state.localM01 * axisY.y,
                    state.localM10 * axisY.x + state.localM11 * axisY.y
                );

                float extentX =
                    abs(axisX.x) * object.halfWidth +
                    abs(axisY.x) * object.halfHeight;
                float extentY =
                    abs(axisX.y) * object.halfWidth +
                    abs(axisY.y) * object.halfHeight;

                bool intersects =
                    center.x + extentX >= camera.minX &&
                    center.x - extentX <= camera.maxX &&
                    center.y + extentY >= camera.minY &&
                    center.y - extentY <= camera.maxY;

                if (intersects) {
                    visibleMask[tid] = 1u;
                    uint destination = atomic_fetch_add_explicit(
                        visibleCount, 1u, memory_order_relaxed
                    );
                    if (destination < objectCount)
                        visibleIndices[destination] = tid;
                }
            }
        )METAL";

        NSError* compileError = nil;
        NSString* sourceString = [NSString stringWithUTF8String:source];
        id<MTLLibrary> library =
            [m_impl->device newLibraryWithSource:sourceString
                                         options:nil
                                           error:&compileError];
        if (!library) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = compileError
                ? toStdString(compileError.localizedDescription)
                : "Metal shader library compilation failed";
            return false;
        }

        id<MTLFunction> probeFunction = [library newFunctionWithName:@"kyanite_probe"];
        if (!probeFunction) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal compute function kyanite_probe was not found";
            return false;
        }

        NSError* pipelineError = nil;
        m_impl->probePipeline =
            [m_impl->device newComputePipelineStateWithFunction:probeFunction
                                                            error:&pipelineError];
        if (!m_impl->probePipeline) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = pipelineError
                ? toStdString(pipelineError.localizedDescription)
                : "Metal probe pipeline creation failed";
            return false;
        }

        id<MTLFunction> cullingFunction =
            [library newFunctionWithName:@"kyanite_cull_objects"];
        if (!cullingFunction) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal compute function kyanite_cull_objects was not found";
            return false;
        }

        NSError* cullingPipelineError = nil;
        m_impl->cullingPipeline =
            [m_impl->device newComputePipelineStateWithFunction:cullingFunction
                                                            error:&cullingPipelineError];
        if (!m_impl->cullingPipeline) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = cullingPipelineError
                ? toStdString(cullingPipelineError.localizedDescription)
                : "Metal object-culling pipeline creation failed";
            return false;
        }

        id<MTLFunction> spriteVertexFunction =
            [library newFunctionWithName:@"kyanite_sprite_vertex"];
        id<MTLFunction> spriteFragmentFunction =
            [library newFunctionWithName:@"kyanite_sprite_fragment"];
        if (!spriteVertexFunction || !spriteFragmentFunction) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal sprite vertex/fragment shader functions were not found";
            return false;
        }

        MTLRenderPipelineDescriptor* renderDescriptor =
            [[MTLRenderPipelineDescriptor alloc] init];
        renderDescriptor.vertexFunction = spriteVertexFunction;
        renderDescriptor.fragmentFunction = spriteFragmentFunction;
        renderDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        auto* normalBlend = renderDescriptor.colorAttachments[0];
        normalBlend.blendingEnabled = YES;
        normalBlend.rgbBlendOperation = MTLBlendOperationAdd;
        normalBlend.alphaBlendOperation = MTLBlendOperationAdd;
        normalBlend.sourceRGBBlendFactor = MTLBlendFactorOne;
        normalBlend.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        normalBlend.sourceAlphaBlendFactor = MTLBlendFactorOne;
        normalBlend.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

        NSError* renderPipelineError = nil;
        m_impl->spriteRenderPipeline =
            [m_impl->device newRenderPipelineStateWithDescriptor:renderDescriptor
                                                            error:&renderPipelineError];
        if (!m_impl->spriteRenderPipeline) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = renderPipelineError
                ? toStdString(renderPipelineError.localizedDescription)
                : "Metal sprite render pipeline creation failed";
            return false;
        }

        normalBlend.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        normalBlend.destinationRGBBlendFactor = MTLBlendFactorOne;
        normalBlend.sourceAlphaBlendFactor = MTLBlendFactorOne;
        normalBlend.destinationAlphaBlendFactor = MTLBlendFactorOne;
        NSError* additivePipelineError = nil;
        m_impl->additiveSpriteRenderPipeline =
            [m_impl->device newRenderPipelineStateWithDescriptor:renderDescriptor
                                                            error:&additivePipelineError];
        if (!m_impl->additiveSpriteRenderPipeline) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = additivePipelineError
                ? toStdString(additivePipelineError.localizedDescription)
                : "Metal additive sprite pipeline creation failed";
            return false;
        }

        MTLSamplerDescriptor* samplerDescriptor =
            [[MTLSamplerDescriptor alloc] init];
        samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
        samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
        samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
        samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
        m_impl->spriteSampler =
            [m_impl->device newSamplerStateWithDescriptor:samplerDescriptor];
        if (!m_impl->spriteSampler) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal sprite sampler creation failed";
            return false;
        }

        m_impl->probeCounter =
            [m_impl->device newBufferWithLength:sizeof(std::uint32_t)
                                        options:MTLResourceStorageModeShared];
        if (!m_impl->probeCounter) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal probe counter allocation failed";
            return false;
        }

        return true;
    }
}

void MetalBackend::submitComputeProbeOnce() {
    @autoreleasepool {
        if (m_impl->probeSubmitted.load(std::memory_order_acquire) ||
            m_impl->probeFailed.load(std::memory_order_acquire))
            return;

        if (!initialize()) {
            m_impl->probeFailed.store(true, std::memory_order_release);
            return;
        }

        auto* counter = static_cast<std::uint32_t*>(m_impl->probeCounter.contents);
        if (!counter) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal probe counter has no CPU-visible contents";
            m_impl->probeFailed.store(true, std::memory_order_release);
            return;
        }
        *counter = 0;

        id<MTLCommandBuffer> commandBuffer = [m_impl->commandQueue commandBuffer];
        if (!commandBuffer) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal probe command buffer creation failed";
            m_impl->probeFailed.store(true, std::memory_order_release);
            return;
        }

        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        if (!encoder) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal probe encoder creation failed";
            m_impl->probeFailed.store(true, std::memory_order_release);
            return;
        }

        [encoder setComputePipelineState:m_impl->probePipeline];
        [encoder setBuffer:m_impl->probeCounter offset:0 atIndex:0];
        [encoder dispatchThreadgroups:MTLSizeMake(4, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder endEncoding];

        Impl* state = m_impl.get();
        id<MTLBuffer> resultBuffer = m_impl->probeCounter;
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
            if (completedBuffer.status != MTLCommandBufferStatusCompleted) {
                std::lock_guard<std::mutex> lock(state->textMutex);
                state->error = completedBuffer.error
                    ? toStdString(completedBuffer.error.localizedDescription)
                    : "Metal probe command buffer did not complete";
                state->probeFailed.store(true, std::memory_order_release);
                return;
            }

            auto* result = static_cast<const std::uint32_t*>(resultBuffer.contents);
            state->completedProbeThreads.store(result ? *result : 0,
                                               std::memory_order_release);

            const double gpuStart = completedBuffer.GPUStartTime;
            const double gpuEnd = completedBuffer.GPUEndTime;
            if (gpuStart > 0.0 && gpuEnd >= gpuStart) {
                state->gpuTimeMilliseconds.store((gpuEnd - gpuStart) * 1000.0,
                                                 std::memory_order_release);
                state->gpuTimeAvailable.store(true, std::memory_order_release);
            }

            state->probeCompleted.store(true, std::memory_order_release);
        }];

        m_impl->probeSubmitted.store(true, std::memory_order_release);
        m_impl->submittedDispatches.fetch_add(1, std::memory_order_relaxed);
        [commandBuffer commit];
    }
}

bool MetalBackend::uploadLevelObjects(
    const std::vector<MetalObjectInput>& objects
) {
    @autoreleasepool {
        if (!initialize())
            return false;

        if (m_impl->frameInFlight.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error =
                "Level data upload deferred because a culling frame is still in flight";
            return false;
        }

        std::vector<MetalGpuObject> gpuObjects;
        gpuObjects.reserve(objects.size());

        std::map<std::vector<std::uint32_t>, std::uint32_t> combinationIndices;
        std::vector<std::vector<std::uint32_t>> groupIdsPerCombination;
        std::vector<MetalGroupState> groupStates;
        std::set<std::uint32_t> usedGroups;
        std::uint32_t maxGroupId = 0;

        const std::vector<std::uint32_t> emptyCombination;
        combinationIndices.emplace(emptyCombination, 0);
        groupIdsPerCombination.push_back(emptyCombination);
        groupStates.emplace_back();

        for (const auto& object : objects) {
            std::vector<std::uint32_t> ids;
            const std::uint32_t groupCount = std::min(object.groupCount, 10u);
            ids.reserve(groupCount);

            for (std::uint32_t i = 0; i < groupCount; ++i) {
                const auto groupId = object.groupIds[i];
                if (groupId > MAX_GROUP_ID)
                    continue;
                ids.push_back(groupId);
                usedGroups.insert(groupId);
                maxGroupId = std::max(maxGroupId, groupId);
            }

            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

            auto found = combinationIndices.find(ids);
            std::uint32_t combinationIndex = 0;
            if (found == combinationIndices.end()) {
                combinationIndex =
                    static_cast<std::uint32_t>(groupIdsPerCombination.size());
                combinationIndices.emplace(ids, combinationIndex);
                groupIdsPerCombination.push_back(ids);
                groupStates.emplace_back();
            } else {
                combinationIndex = found->second;
            }

            gpuObjects.push_back({
                object.x,
                object.y,
                object.halfWidth,
                object.halfHeight,
                object.rotationRadians,
                combinationIndex,
                object.flags
            });
        }

        std::vector<std::vector<std::uint32_t>> combinationsPerGroupId(
            static_cast<std::size_t>(maxGroupId) + 1u
        );
        for (std::uint32_t combinationIndex = 0;
             combinationIndex < groupIdsPerCombination.size();
             ++combinationIndex) {
            for (const auto groupId : groupIdsPerCombination[combinationIndex]) {
                if (groupId < combinationsPerGroupId.size())
                    combinationsPerGroupId[groupId].push_back(combinationIndex);
            }
        }

        std::vector<std::uint32_t> usedGroupIds(usedGroups.begin(), usedGroups.end());
        std::vector<std::uint8_t> groupToggleEnabled(
            static_cast<std::size_t>(maxGroupId) + 1u, 1u
        );
        std::vector<float> groupAlpha(
            static_cast<std::size_t>(maxGroupId) + 1u, 1.0f
        );

        id<MTLBuffer> newObjectBuffer = nil;
        if (!gpuObjects.empty()) {
            newObjectBuffer =
                [m_impl->device newBufferWithBytes:gpuObjects.data()
                                             length:sizeof(MetalGpuObject) * gpuObjects.size()
                                            options:MTLResourceStorageModeShared];
        } else {
            newObjectBuffer =
                [m_impl->device newBufferWithLength:sizeof(MetalGpuObject)
                                            options:MTLResourceStorageModeShared];
        }

        const std::size_t combinationCount = groupStates.size();
        id<MTLBuffer> newGroupBuffer =
            [m_impl->device newBufferWithLength:sizeof(MetalGroupState) * combinationCount
                                        options:MTLResourceStorageModeShared];
        id<MTLBuffer> newCameraBuffer =
            [m_impl->device newBufferWithLength:sizeof(MetalCameraBounds)
                                        options:MTLResourceStorageModeShared];
        id<MTLBuffer> newVisibleBuffer =
            [m_impl->device newBufferWithLength:
                sizeof(std::uint32_t) * std::max<std::size_t>(1, gpuObjects.size())
                                        options:MTLResourceStorageModeShared];
        id<MTLBuffer> newVisibleCountBuffer =
            [m_impl->device newBufferWithLength:sizeof(std::uint32_t)
                                        options:MTLResourceStorageModeShared];
        id<MTLBuffer> newVisibleMaskBuffer =
            [m_impl->device newBufferWithLength:
                sizeof(std::uint32_t) * std::max<std::size_t>(1, gpuObjects.size())
                                        options:MTLResourceStorageModeShared];

        if (!newObjectBuffer || !newGroupBuffer || !newCameraBuffer ||
            !newVisibleBuffer || !newVisibleCountBuffer || !newVisibleMaskBuffer) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Failed to allocate one or more Metal level/culling buffers";
            m_impl->levelDataReady.store(false, std::memory_order_release);
            return false;
        }

        std::memcpy(newGroupBuffer.contents, groupStates.data(),
                    sizeof(MetalGroupState) * combinationCount);
        std::memcpy(newCameraBuffer.contents, &m_impl->cameraBounds,
                    sizeof(MetalCameraBounds));
        *static_cast<std::uint32_t*>(newVisibleCountBuffer.contents) = 0;

        m_impl->objectBuffer = newObjectBuffer;
        m_impl->groupStateBuffer = newGroupBuffer;
        m_impl->cameraBuffer = newCameraBuffer;
        m_impl->visibleIndexBuffer = newVisibleBuffer;
        m_impl->visibleCountBuffer = newVisibleCountBuffer;
        m_impl->visibleMaskBuffer = newVisibleMaskBuffer;
        std::memset(newVisibleMaskBuffer.contents, 0,
                    sizeof(std::uint32_t) * std::max<std::size_t>(1, gpuObjects.size()));

        m_impl->groupStates = std::move(groupStates);
        m_impl->groupIdsPerCombination = std::move(groupIdsPerCombination);
        m_impl->combinationsPerGroupId = std::move(combinationsPerGroupId);
        m_impl->usedGroupIds = std::move(usedGroupIds);
        m_impl->groupToggleEnabled = std::move(groupToggleEnabled);
        m_impl->groupAlpha = std::move(groupAlpha);

        m_impl->objectCount.store(
            static_cast<std::uint32_t>(gpuObjects.size()),
            std::memory_order_release
        );
        m_impl->generation.fetch_add(1, std::memory_order_acq_rel);
        m_impl->visibleObjectCount.store(0, std::memory_order_release);
        m_impl->culledObjectCount.store(0, std::memory_order_release);
        m_impl->completedCullingFrames.store(0, std::memory_order_release);
        m_impl->invalidVisibleIndices.store(0, std::memory_order_release);
        m_impl->cullingSubmitted.store(false, std::memory_order_release);
        m_impl->cullingCompleted.store(false, std::memory_order_release);
        m_impl->cullingFailed.store(false, std::memory_order_release);
        m_impl->levelDataReady.store(true, std::memory_order_release);
        m_impl->groupStateDirty.store(false, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error.clear();
        }
        return true;
    }
}


bool MetalBackend::uploadSpriteGeometry(
    const std::vector<MetalSpriteVertex>& vertices,
    const std::vector<MetalSpriteBatch>& batches,
    const std::vector<MetalTextureInput>& textures
) {
    @autoreleasepool {
        if (!initialize() || vertices.empty() || batches.empty() || textures.empty())
            return false;
        if (m_impl->frameInFlight.load(std::memory_order_acquire))
            return false;

        id<MTLBuffer> newVertexBuffer =
            [m_impl->device newBufferWithBytes:vertices.data()
                                         length:vertices.size() * sizeof(MetalSpriteVertex)
                                        options:MTLResourceStorageModeShared];
        if (!newVertexBuffer) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Failed to allocate Metal sprite vertex buffer";
            return false;
        }

        NSMutableArray<id<MTLTexture>>* newTextures =
            [NSMutableArray arrayWithCapacity:textures.size()];
        for (const auto& input : textures) {
            if (input.width == 0 || input.height == 0 ||
                input.rgba.size() != static_cast<std::size_t>(input.width) * input.height * 4u) {
                std::lock_guard<std::mutex> lock(m_impl->textMutex);
                m_impl->error = "Invalid sprite atlas pixel data";
                return false;
            }

            MTLTextureDescriptor* descriptor =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                   width:input.width
                                                                  height:input.height
                                                               mipmapped:NO];
            descriptor.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> texture =
                [m_impl->device newTextureWithDescriptor:descriptor];
            if (!texture) {
                std::lock_guard<std::mutex> lock(m_impl->textMutex);
                m_impl->error = "Failed to create Metal sprite atlas texture";
                return false;
            }
            [texture replaceRegion:MTLRegionMake2D(0, 0, input.width, input.height)
                        mipmapLevel:0
                          withBytes:input.rgba.data()
                        bytesPerRow:static_cast<NSUInteger>(input.width) * 4u];
            [newTextures addObject:texture];
        }

        for (const auto& batch : batches) {
            if (batch.vertexCount == 0 ||
                static_cast<std::size_t>(batch.firstVertex) + batch.vertexCount > vertices.size() ||
                batch.textureIndex >= newTextures.count) {
                std::lock_guard<std::mutex> lock(m_impl->textMutex);
                m_impl->error = "Sprite batch references invalid vertex or texture data";
                return false;
            }
        }

        m_impl->spriteVertexBuffer = newVertexBuffer;
        m_impl->spriteTextures = newTextures;
        m_impl->spriteBatches = batches;
        {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error.clear();
        }
        return true;
    }
}

bool MetalBackend::attachSpriteView() {
    @autoreleasepool {
        if (!m_impl->device || !m_impl->spriteRenderPipeline ||
            !m_impl->spriteVertexBuffer || !m_impl->spriteTextures ||
            m_impl->spriteTextures.count == 0) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal sprite view cannot attach before sprite geometry is ready";
            return false;
        }

        UIApplication* application = [UIApplication sharedApplication];
        UIWindow* window = application.keyWindow;
        if (!window) {
            for (UIWindow* candidate in application.windows) {
                if (candidate.isKeyWindow) {
                    window = candidate;
                    break;
                }
            }
        }
        if (!window) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Unable to locate the Geometry Dash window for Metal composition";
            return false;
        }

        if (m_impl->spriteView) {
            [m_impl->spriteView removeFromSuperview];
            m_impl->spriteView.delegate = nil;
            m_impl->spriteView = nil;
        }

        UIView* hostView = window.rootViewController.view;
        if (!hostView)
            hostView = window;

        MTKView* view = [[MTKView alloc] initWithFrame:hostView.bounds
                                               device:m_impl->device];
        if (!view) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Failed to create the Metal composition view";
            return false;
        }

        m_impl->spriteViewDelegate = [[KyaniteMetalViewDelegate alloc] init];
        view.delegate = m_impl->spriteViewDelegate;
        view.frame = hostView.bounds;
        view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        view.userInteractionEnabled = NO;
        view.opaque = NO;
        view.backgroundColor = [UIColor clearColor];
        view.layer.opaque = NO;
        view.clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
        view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        view.enableSetNeedsDisplay = NO;
        view.paused = NO;
        view.preferredFramesPerSecond = 60;
        view.framebufferOnly = YES;

        [hostView addSubview:view];
        [hostView bringSubviewToFront:view];
        m_impl->spriteView = view;
        return true;
    }
}

void MetalBackend::detachSpriteView() {
    @autoreleasepool {
        if (m_impl->spriteView) {
            m_impl->spriteView.paused = YES;
            m_impl->spriteView.delegate = nil;
            [m_impl->spriteView removeFromSuperview];
            m_impl->spriteView = nil;
        }
        m_impl->spriteViewDelegate = nil;
    }
}

void MetalBackend::renderFrame(void* rawView) {
    @autoreleasepool {
        MTKView* view = (__bridge MTKView*)rawView;
        if (!view || !m_impl->levelDataReady.load(std::memory_order_acquire) ||
            !m_impl->spriteVertexBuffer || !m_impl->spriteTextures ||
            m_impl->spriteBatches.empty() || !m_impl->visibleMaskBuffer)
            return;

        id<CAMetalDrawable> drawable = view.currentDrawable;
        MTLRenderPassDescriptor* renderPass = view.currentRenderPassDescriptor;
        if (!drawable || !renderPass)
            return;

        if (m_impl->frameInFlight.exchange(true, std::memory_order_acq_rel)) {
            m_impl->skippedBusyFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const std::uint32_t count =
            m_impl->objectCount.load(std::memory_order_acquire);
        id<MTLCommandBuffer> commandBuffer = [m_impl->commandQueue commandBuffer];
        if (!commandBuffer) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal render command buffer creation failed";
            m_impl->cullingFailed.store(true, std::memory_order_release);
            m_impl->frameInFlight.store(false, std::memory_order_release);
            return;
        }

        if (m_impl->groupStateDirty.exchange(false, std::memory_order_acq_rel)) {
            std::memcpy(m_impl->groupStateBuffer.contents,
                        m_impl->groupStates.data(),
                        sizeof(MetalGroupState) * m_impl->groupStates.size());
        }
        std::memcpy(m_impl->cameraBuffer.contents,
                    &m_impl->cameraBounds,
                    sizeof(MetalCameraBounds));
        *static_cast<std::uint32_t*>(m_impl->visibleCountBuffer.contents) = 0u;

        id<MTLComputeCommandEncoder> compute =
            [commandBuffer computeCommandEncoder];
        if (!compute) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal render culling encoder creation failed";
            m_impl->cullingFailed.store(true, std::memory_order_release);
            m_impl->frameInFlight.store(false, std::memory_order_release);
            return;
        }

        if (count > 0) {
            [compute setComputePipelineState:m_impl->cullingPipeline];
            [compute setBuffer:m_impl->objectBuffer offset:0 atIndex:0];
            [compute setBuffer:m_impl->groupStateBuffer offset:0 atIndex:1];
            [compute setBuffer:m_impl->cameraBuffer offset:0 atIndex:2];
            [compute setBuffer:m_impl->visibleIndexBuffer offset:0 atIndex:3];
            [compute setBuffer:m_impl->visibleCountBuffer offset:0 atIndex:4];
            [compute setBytes:&count length:sizeof(count) atIndex:5];
            [compute setBuffer:m_impl->visibleMaskBuffer offset:0 atIndex:6];
            const std::uint32_t groups =
                (count + THREADS_PER_GROUP - 1u) / THREADS_PER_GROUP;
            [compute dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(THREADS_PER_GROUP, 1, 1)];
        }
        [compute endEncoding];

        id<MTLRenderCommandEncoder> render =
            [commandBuffer renderCommandEncoderWithDescriptor:renderPass];
        if (!render) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal sprite render encoder creation failed";
            m_impl->cullingFailed.store(true, std::memory_order_release);
            m_impl->frameInFlight.store(false, std::memory_order_release);
            return;
        }

        [render setVertexBuffer:m_impl->spriteVertexBuffer offset:0 atIndex:0];
        [render setVertexBuffer:m_impl->objectBuffer offset:0 atIndex:1];
        [render setVertexBuffer:m_impl->groupStateBuffer offset:0 atIndex:2];
        [render setVertexBuffer:m_impl->visibleMaskBuffer offset:0 atIndex:3];
        [render setVertexBuffer:m_impl->cameraBuffer offset:0 atIndex:4];
        [render setFragmentSamplerState:m_impl->spriteSampler atIndex:0];

        for (const auto& batch : m_impl->spriteBatches) {
            if (batch.textureIndex >= m_impl->spriteTextures.count)
                continue;
            id<MTLRenderPipelineState> pipeline = batch.additive
                ? m_impl->additiveSpriteRenderPipeline
                : m_impl->spriteRenderPipeline;
            [render setRenderPipelineState:pipeline];
            [render setFragmentTexture:m_impl->spriteTextures[batch.textureIndex]
                               atIndex:0];
            [render drawPrimitives:MTLPrimitiveTypeTriangle
                       vertexStart:batch.firstVertex
                       vertexCount:batch.vertexCount];
        }
        [render endEncoding];
        [commandBuffer presentDrawable:drawable];

        Impl* state = m_impl.get();
        const std::uint64_t submittedGeneration =
            m_impl->generation.load(std::memory_order_acquire);
        id<MTLBuffer> resultCountBuffer = m_impl->visibleCountBuffer;
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
            if (completedBuffer.status != MTLCommandBufferStatusCompleted) {
                if (state->generation.load(std::memory_order_acquire) == submittedGeneration) {
                    std::lock_guard<std::mutex> lock(state->textMutex);
                    state->error = completedBuffer.error
                        ? toStdString(completedBuffer.error.localizedDescription)
                        : "Metal sprite render command buffer did not complete";
                    state->cullingFailed.store(true, std::memory_order_release);
                }
                state->frameInFlight.store(false, std::memory_order_release);
                return;
            }

            const auto* countResult =
                static_cast<const std::uint32_t*>(resultCountBuffer.contents);
            const std::uint32_t visible =
                std::min(countResult ? *countResult : 0u, count);
            if (state->generation.load(std::memory_order_acquire) == submittedGeneration) {
                state->visibleObjectCount.store(visible, std::memory_order_release);
                state->culledObjectCount.store(count - visible, std::memory_order_release);
                state->completedCullingFrames.fetch_add(1, std::memory_order_relaxed);
                state->renderedFrames.fetch_add(1, std::memory_order_relaxed);

                const double gpuStart = completedBuffer.GPUStartTime;
                const double gpuEnd = completedBuffer.GPUEndTime;
                if (gpuStart > 0.0 && gpuEnd >= gpuStart) {
                    state->cullingGpuTimeMilliseconds.store(
                        (gpuEnd - gpuStart) * 1000.0,
                        std::memory_order_release
                    );
                    state->cullingGpuTimeAvailable.store(true, std::memory_order_release);
                }
                state->cullingCompleted.store(true, std::memory_order_release);
            }
            state->frameInFlight.store(false, std::memory_order_release);
        }];

        m_impl->cullingSubmitted.store(true, std::memory_order_release);
        m_impl->cullingCompleted.store(false, std::memory_order_release);
        m_impl->cullingFailed.store(false, std::memory_order_release);
        m_impl->submittedCullingFrames.fetch_add(1, std::memory_order_relaxed);
        [commandBuffer commit];
    }
}

void MetalBackend::updateCameraBounds(
    float minX, float minY, float maxX, float maxY
) {
    m_impl->cameraBounds = {
        std::min(minX, maxX),
        std::min(minY, maxY),
        std::max(minX, maxX),
        std::max(minY, maxY)
    };
}

void MetalBackend::submitCullingFrame() {
    @autoreleasepool {
        if (!m_impl->levelDataReady.load(std::memory_order_acquire) ||
            !m_impl->cullingPipeline || !m_impl->objectBuffer)
            return;

        if (m_impl->frameInFlight.exchange(true, std::memory_order_acq_rel)) {
            m_impl->skippedBusyFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const std::uint32_t count =
            m_impl->objectCount.load(std::memory_order_acquire);
        if (count == 0) {
            m_impl->visibleObjectCount.store(0, std::memory_order_release);
            m_impl->culledObjectCount.store(0, std::memory_order_release);
            m_impl->cullingSubmitted.store(true, std::memory_order_release);
            m_impl->cullingCompleted.store(true, std::memory_order_release);
            m_impl->completedCullingFrames.fetch_add(1, std::memory_order_relaxed);
            m_impl->frameInFlight.store(false, std::memory_order_release);
            return;
        }

        id<MTLCommandBuffer> commandBuffer = [m_impl->commandQueue commandBuffer];
        if (!commandBuffer) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal culling command buffer creation failed";
            m_impl->cullingFailed.store(true, std::memory_order_release);
            m_impl->frameInFlight.store(false, std::memory_order_release);
            return;
        }

        if (m_impl->groupStateDirty.exchange(false, std::memory_order_acq_rel)) {
            std::memcpy(m_impl->groupStateBuffer.contents,
                        m_impl->groupStates.data(),
                        sizeof(MetalGroupState) * m_impl->groupStates.size());
        }
        std::memcpy(m_impl->cameraBuffer.contents,
                    &m_impl->cameraBounds,
                    sizeof(MetalCameraBounds));
        *static_cast<std::uint32_t*>(m_impl->visibleCountBuffer.contents) = 0;

        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        if (!encoder) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal culling compute encoder creation failed";
            m_impl->cullingFailed.store(true, std::memory_order_release);
            m_impl->frameInFlight.store(false, std::memory_order_release);
            return;
        }

        [encoder setComputePipelineState:m_impl->cullingPipeline];
        [encoder setBuffer:m_impl->objectBuffer offset:0 atIndex:0];
        [encoder setBuffer:m_impl->groupStateBuffer offset:0 atIndex:1];
        [encoder setBuffer:m_impl->cameraBuffer offset:0 atIndex:2];
        [encoder setBuffer:m_impl->visibleIndexBuffer offset:0 atIndex:3];
        [encoder setBuffer:m_impl->visibleCountBuffer offset:0 atIndex:4];
        [encoder setBytes:&count length:sizeof(count) atIndex:5];

        const std::uint32_t groupCount =
            (count + THREADS_PER_GROUP - 1u) / THREADS_PER_GROUP;
        [encoder dispatchThreadgroups:MTLSizeMake(groupCount, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(THREADS_PER_GROUP, 1, 1)];
        [encoder endEncoding];

        Impl* state = m_impl.get();
        const std::uint64_t submittedGeneration =
            m_impl->generation.load(std::memory_order_acquire);
        id<MTLBuffer> resultCountBuffer = m_impl->visibleCountBuffer;
        id<MTLBuffer> resultIndicesBuffer = m_impl->visibleIndexBuffer;

        m_impl->cullingSubmitted.store(true, std::memory_order_release);
        m_impl->cullingCompleted.store(false, std::memory_order_release);
        m_impl->cullingFailed.store(false, std::memory_order_release);
        m_impl->submittedCullingFrames.fetch_add(1, std::memory_order_relaxed);

        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
            if (completedBuffer.status != MTLCommandBufferStatusCompleted) {
                if (state->generation.load(std::memory_order_acquire) == submittedGeneration) {
                    std::lock_guard<std::mutex> lock(state->textMutex);
                    state->error = completedBuffer.error
                        ? toStdString(completedBuffer.error.localizedDescription)
                        : "Metal object-culling command buffer did not complete";
                    state->cullingFailed.store(true, std::memory_order_release);
                }
                state->frameInFlight.store(false, std::memory_order_release);
                return;
            }

            const auto* countResult =
                static_cast<const std::uint32_t*>(resultCountBuffer.contents);
            const auto* indices =
                static_cast<const std::uint32_t*>(resultIndicesBuffer.contents);
            const std::uint32_t visible =
                std::min(countResult ? *countResult : 0u, count);

            std::uint32_t invalid = 0;
            for (std::uint32_t i = 0; i < visible; ++i) {
                if (!indices || indices[i] >= count)
                    ++invalid;
            }

            if (state->generation.load(std::memory_order_acquire) == submittedGeneration) {
                state->visibleObjectCount.store(visible, std::memory_order_release);
                state->culledObjectCount.store(count - visible, std::memory_order_release);
                state->invalidVisibleIndices.store(invalid, std::memory_order_release);
                state->completedCullingFrames.fetch_add(1, std::memory_order_relaxed);

                const double gpuStart = completedBuffer.GPUStartTime;
                const double gpuEnd = completedBuffer.GPUEndTime;
                if (gpuStart > 0.0 && gpuEnd >= gpuStart) {
                    state->cullingGpuTimeMilliseconds.store(
                        (gpuEnd - gpuStart) * 1000.0,
                        std::memory_order_release
                    );
                    state->cullingGpuTimeAvailable.store(true, std::memory_order_release);
                }
                state->cullingCompleted.store(true, std::memory_order_release);
            }

            state->frameInFlight.store(false, std::memory_order_release);
        }];

        [commandBuffer commit];
    }
}

void MetalBackend::moveGroup(
    std::int32_t groupId, float deltaX, float deltaY
) {
    if (groupId < 0 ||
        static_cast<std::size_t>(groupId) >= m_impl->combinationsPerGroupId.size())
        return;

    const auto& combinations =
        m_impl->combinationsPerGroupId[static_cast<std::size_t>(groupId)];
    for (const auto combinationIndex : combinations) {
        if (combinationIndex >= m_impl->groupStates.size())
            continue;
        auto& state = m_impl->groupStates[combinationIndex];
        state.offsetX += deltaX;
        state.offsetY += deltaY;
        m_impl->groupStateUpdates.fetch_add(1, std::memory_order_relaxed);
    }
    if (!combinations.empty())
        m_impl->groupStateDirty.store(true, std::memory_order_release);
}

void MetalBackend::rotateGroup(
    std::int32_t groupId,
    float angleDegrees,
    bool lockObjectRotation,
    std::optional<MetalPoint> centerPoint
) {
    if (groupId < 0 ||
        static_cast<std::size_t>(groupId) >= m_impl->combinationsPerGroupId.size())
        return;

    const float radians = angleDegrees * PI_F / 180.0f;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    const auto& combinations =
        m_impl->combinationsPerGroupId[static_cast<std::size_t>(groupId)];

    for (const auto combinationIndex : combinations) {
        if (combinationIndex >= m_impl->groupStates.size())
            continue;

        auto& state = m_impl->groupStates[combinationIndex];

        if (!lockObjectRotation) {
            multiplyByRotation(
                state.localM00, state.localM01,
                state.localM10, state.localM11,
                cosine, sine
            );
        }

        if (centerPoint) {
            multiplyByRotation(
                state.positionM00, state.positionM01,
                state.positionM10, state.positionM11,
                cosine, sine
            );

            const float x = state.offsetX - centerPoint->x;
            const float y = state.offsetY - centerPoint->y;
            state.offsetX = cosine * x + sine * y + centerPoint->x;
            state.offsetY = -sine * x + cosine * y + centerPoint->y;
        }

        m_impl->groupStateUpdates.fetch_add(1, std::memory_order_relaxed);
    }
    if (!combinations.empty())
        m_impl->groupStateDirty.store(true, std::memory_order_release);
}

void MetalBackend::toggleGroup(std::int32_t groupId, bool visible) {
    if (groupId < 0 ||
        static_cast<std::size_t>(groupId) >= m_impl->groupToggleEnabled.size())
        return;

    const auto index = static_cast<std::size_t>(groupId);
    const std::uint8_t value = visible ? 1u : 0u;
    if (m_impl->groupToggleEnabled[index] == value)
        return;

    m_impl->groupToggleEnabled[index] = value;
    if (index >= m_impl->combinationsPerGroupId.size())
        return;

    for (const auto combinationIndex : m_impl->combinationsPerGroupId[index]) {
        m_impl->recomputeCombination(combinationIndex);
        m_impl->groupStateUpdates.fetch_add(1, std::memory_order_relaxed);
    }
    if (!m_impl->combinationsPerGroupId[index].empty())
        m_impl->groupStateDirty.store(true, std::memory_order_release);
}

void MetalBackend::setGroupAlpha(std::int32_t groupId, float alpha) {
    if (groupId < 0 ||
        static_cast<std::size_t>(groupId) >= m_impl->groupAlpha.size())
        return;

    const auto index = static_cast<std::size_t>(groupId);
    const float value = std::clamp(alpha, 0.0f, 1.0f);
    if (std::abs(m_impl->groupAlpha[index] - value) <= 0.0001f)
        return;

    m_impl->groupAlpha[index] = value;
    if (index >= m_impl->combinationsPerGroupId.size())
        return;

    for (const auto combinationIndex : m_impl->combinationsPerGroupId[index]) {
        m_impl->recomputeCombination(combinationIndex);
        m_impl->groupStateUpdates.fetch_add(1, std::memory_order_relaxed);
    }
    if (!m_impl->combinationsPerGroupId[index].empty())
        m_impl->groupStateDirty.store(true, std::memory_order_release);
}

const std::vector<std::uint32_t>& MetalBackend::getUsedGroupIds() const {
    return m_impl->usedGroupIds;
}

void MetalBackend::resetGroupStates() {
    std::fill(m_impl->groupToggleEnabled.begin(),
              m_impl->groupToggleEnabled.end(), 1u);
    std::fill(m_impl->groupAlpha.begin(),
              m_impl->groupAlpha.end(), 1.0f);

    for (std::uint32_t index = 0;
         index < m_impl->groupStates.size(); ++index) {
        m_impl->groupStates[index] = MetalGroupState{};
        m_impl->recomputeCombination(index);
    }
    m_impl->groupStateDirty.store(true, std::memory_order_release);
}

MetalBackendSnapshot MetalBackend::snapshot() const {
    MetalBackendSnapshot result;
    result.deviceReady = m_impl->device != nil;
    result.commandQueueReady = m_impl->commandQueue != nil;
    result.computePipelineReady = m_impl->probePipeline != nil;
    result.probeSubmitted = m_impl->probeSubmitted.load(std::memory_order_acquire);
    result.probeCompleted = m_impl->probeCompleted.load(std::memory_order_acquire);
    result.probeFailed = m_impl->probeFailed.load(std::memory_order_acquire);
    result.expectedProbeThreads = PROBE_THREAD_COUNT;
    result.completedProbeThreads =
        m_impl->completedProbeThreads.load(std::memory_order_acquire);
    result.submittedDispatches =
        m_impl->submittedDispatches.load(std::memory_order_acquire);

    result.levelDataReady = m_impl->levelDataReady.load(std::memory_order_acquire);
    result.cullingPipelineReady = m_impl->cullingPipeline != nil;
    result.cullingSubmitted = m_impl->cullingSubmitted.load(std::memory_order_acquire);
    result.cullingCompleted = m_impl->cullingCompleted.load(std::memory_order_acquire);
    result.cullingFailed = m_impl->cullingFailed.load(std::memory_order_acquire);
    result.levelObjectCount = m_impl->objectCount.load(std::memory_order_acquire);
    result.groupCombinationCount =
        static_cast<std::uint32_t>(m_impl->groupIdsPerCombination.size());
    result.visibleObjectCount =
        m_impl->visibleObjectCount.load(std::memory_order_acquire);
    result.culledObjectCount =
        m_impl->culledObjectCount.load(std::memory_order_acquire);
    result.submittedCullingFrames =
        m_impl->submittedCullingFrames.load(std::memory_order_acquire);
    result.completedCullingFrames =
        m_impl->completedCullingFrames.load(std::memory_order_acquire);
    result.skippedBusyFrames =
        m_impl->skippedBusyFrames.load(std::memory_order_acquire);
    result.invalidVisibleIndices =
        m_impl->invalidVisibleIndices.load(std::memory_order_acquire);
    result.groupStateUpdates =
        m_impl->groupStateUpdates.load(std::memory_order_acquire);

    result.gpuTimeAvailable =
        m_impl->gpuTimeAvailable.load(std::memory_order_acquire);
    result.gpuTimeMilliseconds =
        m_impl->gpuTimeMilliseconds.load(std::memory_order_acquire);
    result.cullingGpuTimeAvailable =
        m_impl->cullingGpuTimeAvailable.load(std::memory_order_acquire);
    result.cullingGpuTimeMilliseconds =
        m_impl->cullingGpuTimeMilliseconds.load(std::memory_order_acquire);

    std::lock_guard<std::mutex> lock(m_impl->textMutex);
    result.deviceName = m_impl->deviceName;
    result.error = m_impl->error;
    return result;
}
