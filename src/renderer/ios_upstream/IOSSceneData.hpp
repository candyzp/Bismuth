#pragma once

#ifdef GEODE_IS_IOS

#include "IOSObjectBatch.hpp"
#include "IOSGPUStateTables.hpp"
#include <array>

namespace IOSUpstream {

class SceneData {
public:
    SceneData() = default;
    ~SceneData();

    static SceneData* getCurrent();

    bool init(PlayLayer* layer);
    void reset();
    void update(float dt);
    void prepareForDraw();

    GroupManager& getGroupManager() { return groupManager; }
    ObjectBatch& getObjectBatch() { return objectBatch; }
    PlayLayer* getPlayLayer() const { return layer; }
    cocos2d::CCTexture2D* getSpriteSheetTexture(SpriteSheet sheet) const;
    const GameState& getGameState() const { return gameState; }
    const ColorChannelTable& getColorChannels() const { return colorChannels; }
    const ColorChannelBlendStates& getBlendStates() const { return blendStates; }
    GPUStateTables& getGPUStateTables() { return gpuStateTables; }
    const GPUStateTables& getGPUStateTables() const { return gpuStateTables; }
    bool isReady() const { return ready; }

private:
    cocos2d::CCTexture2D* getSpriteSheetTexture(SpriteSheet sheet) const;
    bool buildSortedObjectList(std::vector<GameObject*>& objects) const;
    void fetchBlendingStates(ColorChannelBlendStates& states) const;
    void updateColorChannelTable();
    void updateGameState();

private:
    PlayLayer* layer = nullptr;
    std::array<cocos2d::CCTexture2D*, static_cast<usize>(SpriteSheet::COUNT)> spriteSheets {};
    GroupManager groupManager;
    ObjectBatch objectBatch;
    GPUStateTables gpuStateTables;
    GameState gameState {};
    ColorChannelTable colorChannels {};
    ColorChannelBlendStates blendStates {};
    float gameTimer = 0.f;
    bool ready = false;
};

} // namespace IOSUpstream

#endif
