#pragma once

#ifdef GEODE_IS_IOS

#include "IOSSceneData.hpp"
#include "IOSGPUCapabilities.hpp"
#include <Geode/cocos/base_nodes/CCNode.h>
#include <set>
#include <chrono>
#include <string>
#include <vector>

class Shader;

namespace IOSUpstream {

class IOSGPURenderer : public cocos2d::CCNode {
public:
    IOSGPURenderer() = default;
    ~IOSGPURenderer() override;

    static IOSGPURenderer* create(PlayLayer* layer);
    static IOSGPURenderer* getCurrent();
    static void updateStartupDiagnostic(const std::string& message);

    bool initForPlayLayer(PlayLayer* layer);
    void reset();
    void prepareForLevelReset();
    void reportFailure(const std::string& reason) { fail(reason); }
    void enforceGPUOwnership();
    bool updateScene(float dt);
    bool refreshBlendOrder();
    GroupManager& getGroupManager() { return sceneData.getGroupManager(); }
    void recordGameLayerUpdate(float milliseconds);
    void recordSceneUpdate(float milliseconds);
    void recordBlendRefresh(float milliseconds);
    bool syncLayerNodes();
    void drawLayer(const LayerKey& key);
    void beginFrame();
    void finishFrame();

    PlayLayer* getPlayLayer() const { return layer; }
    bool isFailed() const { return failed; }
    bool isReadyForDrawing() const { return initialized && !failed; }
    const char* getFailureReason() const { return failureReason.c_str(); }
    std::string getCrashDiagnosticSnapshot() const;

private:
    bool queryAndValidateCapabilities();
    bool createShaderProgram();
    bool createVertexBuffer();
    bool uploadOrderedVertices();
    bool uploadDirtyVertexRanges();
    bool createLayerNodes();
    void hideStockBatches();
    void updateDebugOverlay();
    void fail(const std::string& reason);

private:
    static IOSGPURenderer* currentRenderer;

    PlayLayer* layer = nullptr;
    SceneData sceneData;
    Shader* shader = nullptr;
    u32 vertexBuffer = 0;
    u64 uploadedOrderRevision = 0;
    u64 uploadedGeometryRevision = 0;
    std::vector<cocos2d::CCNode*> layerNodes;
    std::set<LayerKey> layerNodeKeys;

    IOSGPU::Capabilities capabilities;
    std::string failureReason;
    u32 layerVisitsThisFrame = 0;
    u32 lastFrameLayerVisits = 0;
    u32 drawAttemptsThisFrame = 0;
    u32 lastFrameDrawAttempts = 0;
    u32 drawsThisFrame = 0;
    u32 lastFrameDraws = 0;
    u32 activeLayerCount = 0;
    u32 missingLayerNodeCount = 0;
    u64 frameNumber = 0;
    float lastFrameMilliseconds = 0.f;
    std::chrono::steady_clock::time_point frameStartTime {};

    // Main-thread CPU work and GL submission counters. These are workload
    // metrics, not hardware GPU cycle or fragment-invocation counters.
    float overlayRefreshElapsedMilliseconds = 0.f;
    float gameLayerUpdateMillisecondsThisFrame = 0.f;
    float lastGameLayerUpdateMilliseconds = 0.f;
    float sceneUpdateMillisecondsThisFrame = 0.f;
    float lastSceneUpdateMilliseconds = 0.f;
    float stateBuildMillisecondsThisFrame = 0.f;
    float lastStateBuildMilliseconds = 0.f;
    float stateTransferMillisecondsThisFrame = 0.f;
    float lastStateTransferMilliseconds = 0.f;
    float blendRefreshMillisecondsThisFrame = 0.f;
    float lastBlendRefreshMilliseconds = 0.f;
    float drawSubmitMillisecondsThisFrame = 0.f;
    float lastDrawSubmitMilliseconds = 0.f;
    u64 spritesScannedThisFrame = 0;
    u64 lastSpritesScanned = 0;
    u64 spritesChangedThisFrame = 0;
    u64 lastSpritesChanged = 0;
    u64 groupStateRecordsThisFrame = 0;
    u64 lastGroupStateRecords = 0;
    u64 pulseEntriesThisFrame = 0;
    u64 lastPulseEntries = 0;
    u64 statePayloadTexelsThisFrame = 0;
    u64 lastStatePayloadTexels = 0;
    u64 statePayloadBytesThisFrame = 0;
    u64 lastStatePayloadBytes = 0;
    u64 vboUploadBytesThisFrame = 0;
    u64 lastVboUploadBytes = 0;
    u64 gpuVerticesSubmittedThisFrame = 0;
    u64 lastGPUVerticesSubmitted = 0;
    u64 gpuTrianglesSubmittedThisFrame = 0;
    u64 lastGPUTrianglesSubmitted = 0;
    bool initialized = false;
    bool failed = false;
    bool hasFinishedFrame = false;
    bool reportedDrawFailure = false;
};

} // namespace IOSUpstream

#endif
