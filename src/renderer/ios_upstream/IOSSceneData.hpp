#pragma once

#ifdef GEODE_IS_IOS

#include "IOSObjectBatch.hpp"
#include "IOSGPUStateTables.hpp"
#include <array>
#include <string>
#include <vector>

namespace IOSUpstream {

class SceneData {
public:
    SceneData() = default;
    ~SceneData();

    static SceneData* getCurrent();

    bool init(PlayLayer* layer);
    void reset();
    bool update(float dt);
    bool refreshBlendOrder();
    void prepareForDraw();

    // Keep object-owned animation and particle effects alive while the GPU
    // renderer replaces PlayLayer's stock per-object visibility pass.
    void updateObjectParticles();
    void releaseObjectParticles();
    usize getAnimatedObjectCount() const { return animatedObjects.size(); }
    usize getLastAnimatedObjectsUpdated() const { return lastAnimatedObjectsUpdated; }
    usize getParticleObjectCount() const { return particleObjects.size(); }
    usize getClaimedParticleCount() const { return claimedParticleCount; }

    GroupManager& getGroupManager() { return groupManager; }
    const GroupManager& getGroupManager() const { return groupManager; }
    ObjectBatch& getObjectBatch() { return objectBatch; }
    const ObjectBatch& getObjectBatch() const { return objectBatch; }
    PlayLayer* getPlayLayer() const { return layer; }
    cocos2d::CCTexture2D* getSpriteSheetTexture(SpriteSheet sheet) const;
    const GameState& getGameState() const { return gameState; }
    const ColorChannelTable& getColorChannels() const { return colorChannels; }
    const ColorChannelBlendStates& getBlendStates() const { return blendStates; }
    GPUStateTables& getGPUStateTables() { return gpuStateTables; }
    const GPUStateTables& getGPUStateTables() const { return gpuStateTables; }
    bool isReady() const { return ready; }
    const std::string& getFailureReason() const { return failureReason; }

private:
    struct AnimatedObject {
        GameObject* object = nullptr;
        bool syncedAnimation = false;
        bool rotateAction = false;
        bool updateChildColor = false;
        bool interactiveHover = false;
    };
    struct ParticleObject {
        GameObject* object = nullptr;
        bool claimed = false;
    };

    bool buildSortedObjectList(std::vector<GameObject*>& objects) const;
    void fetchBlendingStates(ColorChannelBlendStates& states) const;
    void updateColorChannelTable();
    void updateGameState();
    void initializeLiveObjectTracking();
    void updateAnimatedObjects(float dt);

private:
    PlayLayer* layer = nullptr;
    std::array<cocos2d::CCTexture2D*, static_cast<usize>(SpriteSheet::COUNT)> spriteSheets {};
    GroupManager groupManager;
    ObjectBatch objectBatch;
    GPUStateTables gpuStateTables;
    GameState gameState {};
    ColorChannelTable colorChannels {};
    ColorChannelBlendStates blendStates {};
    std::vector<AnimatedObject> animatedObjects;
    std::vector<ParticleObject> particleObjects;
    usize lastAnimatedObjectsUpdated = 0;
    usize claimedParticleCount = 0;
    float gameTimer = 0.f;
    std::string failureReason;
    bool ready = false;
};

} // namespace IOSUpstream

#endif
