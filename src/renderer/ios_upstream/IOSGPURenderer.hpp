#pragma once

#ifdef GEODE_IS_IOS

#include "IOSSceneData.hpp"
#include "IOSGPUCapabilities.hpp"
#include <Geode/cocos/base_nodes/CCNode.h>
#include <vector>

class Shader;

namespace IOSUpstream {

class IOSGPULayerNode;

class IOSGPURenderer : public cocos2d::CCNode {
public:
    IOSGPURenderer() = default;
    ~IOSGPURenderer() override;

    static IOSGPURenderer* create(PlayLayer* layer);
    static IOSGPURenderer* getCurrent();

    bool initForPlayLayer(PlayLayer* layer);
    void reset();
    void drawLayer(const LayerKey& key);

    PlayLayer* getPlayLayer() const { return layer; }
    bool isFailed() const { return failed; }
    const char* getFailureReason() const { return failureReason.c_str(); }

private:
    bool queryAndValidateCapabilities();
    bool createShaderProgram();
    bool createVertexBuffer();
    bool uploadOrderedVertices();
    bool createLayerNodes();
    void hideStockBatches();
    void fail(const std::string& reason);

private:
    static IOSGPURenderer* currentRenderer;

    PlayLayer* layer = nullptr;
    SceneData sceneData;
    Shader* shader = nullptr;
    u32 vertexBuffer = 0;
    u64 uploadedOrderRevision = 0;
    std::vector<IOSGPULayerNode*> layerNodes;

    IOSGPU::Capabilities capabilities;
    std::string failureReason;
    bool failed = false;
    bool reportedDrawFailure = false;
};

} // namespace IOSUpstream

#endif
