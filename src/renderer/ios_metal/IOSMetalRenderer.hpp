#pragma once

#if defined(GEODE_IS_IOS)

#include "IOSMetalCompute.hpp"
#include "IOSMetalGroupManager.hpp"
#include "IOSMetalObjectBatch.hpp"

#include <Geode/binding/PlayLayer.hpp>
#include <Geode/cocos/sprite_nodes/CCSpriteBatchNode.h>
#include <string>
#include <vector>

namespace BismuthIOSMetal {

class Renderer final : public cocos2d::CCNode {
public:
    static Renderer* create(PlayLayer* layer, cocos2d::CCArray* objects);
    static Renderer* getCurrent();

    ~Renderer() override;

    PlayLayer* getPlayLayer() const { return layer; }
    bool hasTakenOwnership() const { return takenOwnership; }
    bool isReady() const { return ready; }
    GroupManager& getGroupManager() { return groupManager; }

    void update(float dt) override;
    void draw() override;
    void drawLayer(const LayerKey& key);
    void finishDrawFrame();
    void reset();

private:
    bool initWithLevel(PlayLayer* layer, cocos2d::CCArray* objects);
    bool loadSpriteSheets();
    bool createGLESProgram();
    bool createLayerDrawNodes();
    void restoreVanillaBatches();
    void fetchBlendStates(BlendStates& output) const;
    void fetchColorChannels();
    FrameState captureFrameState(glm::mat4& mvp) const;
    void createDebugOverlay();
    void updateDebugOverlay();

private:
    static Renderer* current;

    PlayLayer* layer = nullptr;
    std::array<cocos2d::CCTexture2D*, static_cast<std::size_t>(SpriteSheet::COUNT)> spriteSheets {};

    ObjectBatch objectBatch;
    GroupManager groupManager;
    ComputeRenderer metalCompute;

    std::array<std::uint32_t, 1101> colorChannels {};
    std::vector<cocos2d::CCSpriteBatchNode*> vanillaBatchNodes;
    std::vector<LayerKey> drawLayerKeys;

    GLuint glesProgram = 0;
    GLint stateTextureUniform = -1;
    GLint stateTextureSizeUniform = -1;
    GLint mvpUniform = -1;
    GLint spriteSheetUniform = -1;

    OutputFrame activeOutput {};
    glm::mat4 activeMVP {1.f};
    cocos2d::CCLabelBMFont* debugOverlay = nullptr;
    float gameTimer = 0.f;
    float estimatedFPS = 0.f;
    float overlayRefreshTimer = 0.f;

    double cpuBlendOrderMs = 0.0;
    double cpuColorFetchMs = 0.0;
    double cpuGroupStateMs = 0.0;
    double cpuFrameStateMs = 0.0;
    double cpuSubmitMs = 0.0;
    double cpuLevelBuildMs = 0.0;
    double cpuMetalStaticUploadMs = 0.0;
    std::uint64_t submittedFrameCount = 0;
    std::string dispatchStatus = "INITIALIZING";
    std::string lastDispatchError;
    // Retained after reset so Renderer::create can show a failure HUD even
    // when initialization falls back to vanilla drawing.
    std::string initializationError;

    std::uint32_t drawsThisFrame = 0;
    std::uint32_t drawFailuresThisFrame = 0;
    bool frameHasOutput = false;
    bool ready = false;
    bool takenOwnership = false;
};

} // namespace BismuthIOSMetal

#endif
