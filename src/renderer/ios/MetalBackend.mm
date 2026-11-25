#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "MetalBackend.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>

namespace {
constexpr std::uint32_t PROBE_THREAD_COUNT = 256;
constexpr std::uint32_t THREADS_PER_GROUP = 64;
constexpr float PI_F = 3.14159265358979323846f;

std::string toStdString(NSString* value) {
    if (!value)
        return {};
    const char* utf8 = [value UTF8String];
    return utf8 ? std::string(utf8) : std::string();
}

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

static_assert(sizeof(MetalObjectInput) == 68, "MetalObjectInput layout must match the MSL struct");
static_assert(sizeof(MetalGroupState) == 48, "MetalGroupState layout must match the MSL struct");
static_assert(sizeof(MetalCameraBounds) == 16, "MetalCameraBounds layout must match the MSL struct");
} // namespace

struct MetalBackend::Impl {
    __strong id<MTLDevice> device = nil;
    __strong id<MTLCommandQueue> commandQueue = nil;
    __strong id<MTLComputePipelineState> probePipeline = nil;
    __strong id<MTLComputePipelineState> cullingPipeline = nil;

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
    std::vector<std::uint32_t> usedGroupIds;
    MetalCameraBounds cameraBounds;
    std::atomic<std::uint32_t> objectCount{0};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<bool> levelDataReady{false};
    std::atomic<bool> frameInFlight{false};

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
                uint groupCount;
                uint groupIds[10];
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

            kernel void kyanite_probe(
                device atomic_uint* counter [[buffer(0)]],
                uint tid [[thread_position_in_grid]]
            ) {
                atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
            }

            kernel void kyanite_cull_objects(
                device const KyaniteObject* objects [[buffer(0)]],
                device const KyaniteGroupState* groups [[buffer(1)]],
                constant KyaniteCameraBounds& camera [[buffer(2)]],
                device uint* visibleIndices [[buffer(3)]],
                device atomic_uint* visibleCount [[buffer(4)]],
                constant uint& objectCount [[buffer(5)]],
                uint tid [[thread_position_in_grid]]
            ) {
                if (tid >= objectCount)
                    return;

                const KyaniteObject object = objects[tid];
                float2 center = float2(object.x, object.y);
                float c = cos(object.rotation);
                float s = sin(object.rotation);
                float2 axisX = float2(c, s);
                float2 axisY = float2(-s, c);
                float alpha = 1.0;
                bool enabled = true;

                uint groupLimit = min(object.groupCount, 10u);
                for (uint i = 0; i < groupLimit; ++i) {
                    uint groupId = object.groupIds[i];
                    KyaniteGroupState state = groups[groupId];

                    if (state.visible == 0u) {
                        enabled = false;
                        break;
                    }

                    center = float2(
                        state.positionM00 * center.x + state.positionM01 * center.y + state.offsetX,
                        state.positionM10 * center.x + state.positionM11 * center.y + state.offsetY
                    );

                    axisX = float2(
                        state.localM00 * axisX.x + state.localM01 * axisX.y,
                        state.localM10 * axisX.x + state.localM11 * axisX.y
                    );
                    axisY = float2(
                        state.localM00 * axisY.x + state.localM01 * axisY.y,
                        state.localM10 * axisY.x + state.localM11 * axisY.y
                    );

                    alpha *= state.alpha;
                }

                if (!enabled || alpha <= 0.001)
                    return;

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
    const std::vector<MetalObjectInput>& objects,
    std::uint32_t groupStateCount
) {
    @autoreleasepool {
        if (!initialize())
            return false;

        if (m_impl->frameInFlight.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Level data upload deferred because a culling frame is still in flight";
            return false;
        }

        const std::uint32_t count = static_cast<std::uint32_t>(
            std::min<std::size_t>(objects.size(),
                                  std::numeric_limits<std::uint32_t>::max())
        );
        const std::uint32_t safeGroupCount = std::max(1u, groupStateCount);

        id<MTLBuffer> newObjectBuffer = nil;
        if (count > 0) {
            newObjectBuffer =
                [m_impl->device newBufferWithBytes:objects.data()
                                             length:sizeof(MetalObjectInput) * count
                                            options:MTLResourceStorageModeShared];
        } else {
            newObjectBuffer =
                [m_impl->device newBufferWithLength:sizeof(MetalObjectInput)
                                            options:MTLResourceStorageModeShared];
        }

        id<MTLBuffer> newGroupBuffer =
            [m_impl->device newBufferWithLength:sizeof(MetalGroupState) * safeGroupCount
                                        options:MTLResourceStorageModeShared];
        id<MTLBuffer> newCameraBuffer =
            [m_impl->device newBufferWithLength:sizeof(MetalCameraBounds)
                                        options:MTLResourceStorageModeShared];
        id<MTLBuffer> newVisibleBuffer =
            [m_impl->device newBufferWithLength:sizeof(std::uint32_t) * std::max(1u, count)
                                        options:MTLResourceStorageModeShared];
        id<MTLBuffer> newVisibleCountBuffer =
            [m_impl->device newBufferWithLength:sizeof(std::uint32_t)
                                        options:MTLResourceStorageModeShared];

        if (!newObjectBuffer || !newGroupBuffer || !newCameraBuffer ||
            !newVisibleBuffer || !newVisibleCountBuffer) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Failed to allocate one or more Metal level/culling buffers";
            m_impl->levelDataReady.store(false, std::memory_order_release);
            return false;
        }

        m_impl->groupStates.assign(safeGroupCount, MetalGroupState{});
        m_impl->usedGroupIds.clear();
        for (const auto& object : objects) {
            for (std::uint32_t i = 0; i < std::min(object.groupCount, 10u); ++i)
                m_impl->usedGroupIds.push_back(object.groupIds[i]);
        }
        std::sort(m_impl->usedGroupIds.begin(), m_impl->usedGroupIds.end());
        m_impl->usedGroupIds.erase(
            std::unique(m_impl->usedGroupIds.begin(), m_impl->usedGroupIds.end()),
            m_impl->usedGroupIds.end()
        );
        std::memcpy(newGroupBuffer.contents, m_impl->groupStates.data(),
                    sizeof(MetalGroupState) * safeGroupCount);
        std::memcpy(newCameraBuffer.contents, &m_impl->cameraBounds,
                    sizeof(MetalCameraBounds));
        *static_cast<std::uint32_t*>(newVisibleCountBuffer.contents) = 0;

        m_impl->objectBuffer = newObjectBuffer;
        m_impl->groupStateBuffer = newGroupBuffer;
        m_impl->cameraBuffer = newCameraBuffer;
        m_impl->visibleIndexBuffer = newVisibleBuffer;
        m_impl->visibleCountBuffer = newVisibleCountBuffer;

        m_impl->objectCount.store(count, std::memory_order_release);
        m_impl->generation.fetch_add(1, std::memory_order_acq_rel);
        m_impl->visibleObjectCount.store(0, std::memory_order_release);
        m_impl->culledObjectCount.store(0, std::memory_order_release);
        m_impl->completedCullingFrames.store(0, std::memory_order_release);
        m_impl->invalidVisibleIndices.store(0, std::memory_order_release);
        m_impl->cullingSubmitted.store(false, std::memory_order_release);
        m_impl->cullingCompleted.store(false, std::memory_order_release);
        m_impl->cullingFailed.store(false, std::memory_order_release);
        m_impl->levelDataReady.store(true, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error.clear();
        }
        return true;
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
            m_impl->cullingCompleted.store(true, std::memory_order_release);
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

        std::memcpy(m_impl->groupStateBuffer.contents,
                    m_impl->groupStates.data(),
                    sizeof(MetalGroupState) * m_impl->groupStates.size());
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
    if (groupId < 0 || static_cast<std::size_t>(groupId) >= m_impl->groupStates.size())
        return;

    auto& state = m_impl->groupStates[static_cast<std::size_t>(groupId)];
    state.offsetX += deltaX;
    state.offsetY += deltaY;
    m_impl->groupStateUpdates.fetch_add(1, std::memory_order_relaxed);
}

void MetalBackend::rotateGroup(
    std::int32_t groupId,
    float angleDegrees,
    bool lockObjectRotation,
    std::optional<MetalPoint> centerPoint
) {
    if (groupId < 0 || static_cast<std::size_t>(groupId) >= m_impl->groupStates.size())
        return;

    auto& state = m_impl->groupStates[static_cast<std::size_t>(groupId)];
    const float radians = angleDegrees * PI_F / 180.0f;
    const float c = std::cos(radians);
    const float s = std::sin(radians);

    auto multiplyByRotation = [c, s](
        float& m00, float& m01, float& m10, float& m11
    ) {
        const float old00 = m00;
        const float old01 = m01;
        const float old10 = m10;
        const float old11 = m11;
        m00 = old00 * c - old01 * s;
        m01 = old00 * s + old01 * c;
        m10 = old10 * c - old11 * s;
        m11 = old10 * s + old11 * c;
    };

    if (!lockObjectRotation)
        multiplyByRotation(state.localM00, state.localM01,
                           state.localM10, state.localM11);

    if (centerPoint) {
        multiplyByRotation(state.positionM00, state.positionM01,
                           state.positionM10, state.positionM11);
        const float x = state.offsetX - centerPoint->x;
        const float y = state.offsetY - centerPoint->y;
        state.offsetX = c * x + s * y + centerPoint->x;
        state.offsetY = -s * x + c * y + centerPoint->y;
    }

    m_impl->groupStateUpdates.fetch_add(1, std::memory_order_relaxed);
}

void MetalBackend::toggleGroup(std::int32_t groupId, bool visible) {
    if (groupId < 0 || static_cast<std::size_t>(groupId) >= m_impl->groupStates.size())
        return;

    m_impl->groupStates[static_cast<std::size_t>(groupId)].visible = visible ? 1u : 0u;
    m_impl->groupStateUpdates.fetch_add(1, std::memory_order_relaxed);
}

void MetalBackend::setGroupAlpha(std::int32_t groupId, float alpha) {
    if (groupId < 0 || static_cast<std::size_t>(groupId) >= m_impl->groupStates.size())
        return;

    auto& current = m_impl->groupStates[static_cast<std::size_t>(groupId)].alpha;
    const float value = std::clamp(alpha, 0.0f, 1.0f);
    if (std::abs(current - value) > 0.0001f) {
        current = value;
        m_impl->groupStateUpdates.fetch_add(1, std::memory_order_relaxed);
    }
}

std::vector<std::uint32_t> MetalBackend::getUsedGroupIds() const {
    return m_impl->usedGroupIds;
}

void MetalBackend::resetGroupStates() {
    for (auto& state : m_impl->groupStates)
        state = MetalGroupState{};
    m_impl->groupStateUpdates.fetch_add(1, std::memory_order_relaxed);
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
