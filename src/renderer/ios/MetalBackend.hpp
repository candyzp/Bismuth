#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct MetalBackendSnapshot {
    bool deviceReady = false;
    bool commandQueueReady = false;
    bool computePipelineReady = false;
    bool probeSubmitted = false;
    bool probeCompleted = false;
    bool probeFailed = false;

    std::uint32_t expectedProbeThreads = 0;
    std::uint32_t completedProbeThreads = 0;
    std::uint32_t submittedDispatches = 0;

    bool gpuTimeAvailable = false;
    double gpuTimeMilliseconds = 0.0;

    std::string deviceName;
    std::string error;
};

/*
 * Native iOS Metal capability and compute self-test.
 *
 * This is deliberately not advertised as the Kyanite level renderer:
 * it verifies the Metal runtime and a real compute dispatch while the
 * object-buffer, culling, trigger-state, and draw integration is developed.
 */
class MetalBackend {
public:
    static MetalBackend& get();

    bool initialize();
    void submitComputeProbeOnce();
    MetalBackendSnapshot snapshot() const;

    MetalBackend(const MetalBackend&) = delete;
    MetalBackend& operator=(const MetalBackend&) = delete;

private:
    MetalBackend();
    ~MetalBackend();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
