#pragma once

#ifdef GEODE_IS_IOS

#include "IOSSceneData.hpp"
#include "IOSGPUCapabilities.hpp"
#include <Geode/cocos/base_nodes/CCNode.h>
#include <set>
#include <string>
#include <vector>

namespace cocos2d { class CCLabelBMFont; }

class Shader;

namespace IOSUpstream {

class IOSGPURenderer : public cocos2d::CCNode {
public:
    IOSGPURenderer() = default;
    ~IOSGPURenderer() override;

    static IOSGPURenderer* create(PlayLayer* layer);
    static IOSGPURenderer* getCurrent();

    bool initForPlayLayer(PlayLayer* layer);
    void reset();
    void reportFailure(const std::string& reason) { fail(reason); }
    void enforceGPUOwnership();
    bool syncLayerNodes();
    void drawLayer(const LayerKey& key);
    void beginFrame();
    void finishFrame();

    PlayLayer* getPlayLayer() const { return layer; }
    bool isFailed() const { return failed; }
    bool isReadyForDrawing() const { return initialized && !failed; }
    const char* getFailureReason() const { return failureReason.c_str(); }

private:
    bool queryAndValidateCapabilities();
    bool createShaderProgram();
    bool createVertexBuffer();
    bool uploadOrderedVertices();
    bool createLayerNodes();
    void hideStockBatches();
    void createDebugOverlay();
    void updateDebugOverlay();
    void fail(const std::string& reason);

private:
    static IOSGPURenderer* currentRenderer;

    PlayLayer* layer = nullptr;
    SceneData sceneData;
    Shader* shader = nullptr;
    u32 vertexBuffer = 0;
    u64 uploadedOrderRevision = 0;
    std::vector<cocos2d::CCNode*> layerNodes;
    std::set<LayerKey> layerNodeKeys;

    IOSGPU::Capabilities capabilities;
    std::string failureReason;
    cocos2d::CCLabelBMFont* debugOverlay = nullptr;
    u32 drawsThisFrame = 0;
    u32 lastFrameDraws = 0;
    u32 activeLayerCount = 0;
    u32 missingLayerNodeCount = 0;
    bool initialized = false;
    bool failed = false;
    bool reportedDrawFailure = false;
};

} // namespace IOSUpstream

#endif
