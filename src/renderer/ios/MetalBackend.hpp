#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct MetalObjectInput {
    float x = 0.0f;
    float y = 0.0f;
    float halfWidth = 1.0f;
    float halfHeight = 1.0f;
    float rotationRadians = 0.0f;
    std::uint32_t groupCount = 0;
    std::array<std::uint32_t, 10> groupIds{};
    std::uint32_t flags = 0;
};


struct MetalSpriteVertex {
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    std::uint32_t objectIndex = 0;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct MetalSpriteBatch {
    std::uint32_t firstVertex = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t textureIndex = 0;
    bool additive = false;
};

struct MetalTextureInput {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

struct MetalPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct MetalBackendSnapshot {
    bool deviceReady = false;
    bool commandQueueReady = false;
    bool computePipelineReady = false;
    bool probeSubmitted = false;
    bool probeCompleted = false;
    bool probeFailed = false;

    bool levelDataReady = false;
    bool cullingPipelineReady = false;
    bool cullingSubmitted = false;
    bool cullingCompleted = false;
    bool cullingFailed = false;
    bool spritePipelineReady = false;
    bool spriteGeometryReady = false;
    bool spriteViewAttached = false;
    std::uint32_t renderedFrames = 0;

    std::uint32_t expectedProbeThreads = 0;
    std::uint32_t completedProbeThreads = 0;
    std::uint32_t submittedDispatches = 0;

    std::uint32_t levelObjectCount = 0;
    std::uint32_t groupCombinationCount = 0;
    std::uint32_t visibleObjectCount = 0;
    std::uint32_t culledObjectCount = 0;
    std::uint32_t submittedCullingFrames = 0;
    std::uint32_t completedCullingFrames = 0;
    std::uint32_t skippedBusyFrames = 0;
    std::uint32_t invalidVisibleIndices = 0;
    std::uint32_t groupStateUpdates = 0;

    bool gpuTimeAvailable = false;
    double gpuTimeMilliseconds = 0.0;
    bool cullingGpuTimeAvailable = false;
    double cullingGpuTimeMilliseconds = 0.0;

    std::string deviceName;
    std::string error;
};

/*
 * Native iOS Metal backend.
 *
 * Immutable object metadata is uploaded once per level. Each object points
 * to one shared group-combination state, matching Kyanite's core data model.
 * A Metal compute pass creates a GPU-visible object-index list; it does not
 * draw sprites yet, so Geometry Dash remains the authoritative renderer.
 */
class MetalBackend {
public:
    static MetalBackend& get();

    bool initialize();
    void submitComputeProbeOnce();

    bool uploadLevelObjects(const std::vector<MetalObjectInput>& objects);
    bool uploadSpriteGeometry(
        const std::vector<MetalSpriteVertex>& vertices,
        const std::vector<MetalSpriteBatch>& batches,
        const std::vector<MetalTextureInput>& textures
    );
    bool attachSpriteView();
    void detachSpriteView();
    void renderFrame(void* metalView);
    void updateCameraBounds(float minX, float minY, float maxX, float maxY);
    void submitCullingFrame();

    void moveGroup(std::int32_t groupId, float deltaX, float deltaY);
    void rotateGroup(
        std::int32_t groupId,
        float angleDegrees,
        bool lockObjectRotation,
        std::optional<MetalPoint> centerPoint = std::nullopt
    );
    void toggleGroup(std::int32_t groupId, bool visible);
    void setGroupAlpha(std::int32_t groupId, float alpha);
    const std::vector<std::uint32_t>& getUsedGroupIds() const;
    void resetGroupStates();

    MetalBackendSnapshot snapshot() const;

    MetalBackend(const MetalBackend&) = delete;
    MetalBackend& operator=(const MetalBackend&) = delete;

private:
    MetalBackend();
    ~MetalBackend();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
