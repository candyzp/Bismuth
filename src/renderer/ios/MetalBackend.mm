#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "MetalBackend.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace {
constexpr std::uint32_t PROBE_THREAD_COUNT = 256;

std::string toStdString(NSString* value) {
    if (!value)
        return {};
    const char* utf8 = [value UTF8String];
    return utf8 ? std::string(utf8) : std::string();
}
} // namespace

struct MetalBackend::Impl {
    __strong id<MTLDevice> device = nil;
    __strong id<MTLCommandQueue> commandQueue = nil;
    __strong id<MTLComputePipelineState> probePipeline = nil;
    __strong id<MTLBuffer> probeCounter = nil;

    mutable std::mutex textMutex;
    std::string deviceName;
    std::string error;

    std::atomic<bool> probeSubmitted{false};
    std::atomic<bool> probeCompleted{false};
    std::atomic<bool> probeFailed{false};
    std::atomic<std::uint32_t> completedProbeThreads{0};
    std::atomic<std::uint32_t> submittedDispatches{0};
    std::atomic<bool> gpuTimeAvailable{false};
    std::atomic<double> gpuTimeMilliseconds{0.0};
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
            m_impl->probePipeline && m_impl->probeCounter)
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

            kernel void kyanite_probe(
                device atomic_uint* counter [[buffer(0)]],
                uint tid [[thread_position_in_grid]]
            ) {
                atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
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

        id<MTLFunction> function = [library newFunctionWithName:@"kyanite_probe"];
        if (!function) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal compute function kyanite_probe was not found";
            return false;
        }

        NSError* pipelineError = nil;
        m_impl->probePipeline =
            [m_impl->device newComputePipelineStateWithFunction:function
                                                            error:&pipelineError];
        if (!m_impl->probePipeline) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = pipelineError
                ? toStdString(pipelineError.localizedDescription)
                : "Metal compute pipeline creation failed";
            return false;
        }

        m_impl->probeCounter =
            [m_impl->device newBufferWithLength:sizeof(std::uint32_t)
                                        options:MTLResourceStorageModeShared];
        if (!m_impl->probeCounter) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal shared counter buffer allocation failed";
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
            m_impl->error = "Metal counter buffer has no CPU-visible contents";
            m_impl->probeFailed.store(true, std::memory_order_release);
            return;
        }
        *counter = 0;

        id<MTLCommandBuffer> commandBuffer = [m_impl->commandQueue commandBuffer];
        if (!commandBuffer) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal command buffer creation failed";
            m_impl->probeFailed.store(true, std::memory_order_release);
            return;
        }

        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        if (!encoder) {
            std::lock_guard<std::mutex> lock(m_impl->textMutex);
            m_impl->error = "Metal compute encoder creation failed";
            m_impl->probeFailed.store(true, std::memory_order_release);
            return;
        }

        [encoder setComputePipelineState:m_impl->probePipeline];
        [encoder setBuffer:m_impl->probeCounter offset:0 atIndex:0];
        [encoder dispatchThreadgroups:MTLSizeMake(4, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder endEncoding];

        Impl* state = m_impl.get();
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
            if (completedBuffer.status != MTLCommandBufferStatusCompleted) {
                std::lock_guard<std::mutex> lock(state->textMutex);
                state->error = completedBuffer.error
                    ? toStdString(completedBuffer.error.localizedDescription)
                    : "Metal compute command buffer did not complete";
                state->probeFailed.store(true, std::memory_order_release);
                return;
            }

            auto* result = static_cast<const std::uint32_t*>(
                state->probeCounter.contents
            );
            state->completedProbeThreads.store(
                result ? *result : 0,
                std::memory_order_release
            );

            const double gpuStart = completedBuffer.GPUStartTime;
            const double gpuEnd = completedBuffer.GPUEndTime;
            if (gpuStart > 0.0 && gpuEnd >= gpuStart) {
                state->gpuTimeMilliseconds.store(
                    (gpuEnd - gpuStart) * 1000.0,
                    std::memory_order_release
                );
                state->gpuTimeAvailable.store(true, std::memory_order_release);
            }

            state->probeCompleted.store(true, std::memory_order_release);
        }];

        m_impl->probeSubmitted.store(true, std::memory_order_release);
        m_impl->submittedDispatches.fetch_add(1, std::memory_order_relaxed);
        [commandBuffer commit];
    }
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
    result.gpuTimeAvailable =
        m_impl->gpuTimeAvailable.load(std::memory_order_acquire);
    result.gpuTimeMilliseconds =
        m_impl->gpuTimeMilliseconds.load(std::memory_order_acquire);

    std::lock_guard<std::mutex> lock(m_impl->textMutex);
    result.deviceName = m_impl->deviceName;
    result.error = m_impl->error;
    return result;
}
