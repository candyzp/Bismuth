#pragma once

#ifdef GEODE_IS_IOS

#include "IOSObjectBatch.hpp"
#include <array>

namespace IOSUpstream {

class SceneData {
public:
    bool init(PlayLayer* layer);
    void reset();
    void update();

    GroupManager& getGroupManager() { return groupManager; }
    ObjectBatch& getObjectBatch() { return objectBatch; }
    PlayLayer* getPlayLayer() const { return layer; }
    bool isReady() const { return ready; }

private:
    cocos2d::CCTexture2D* getSpriteSheetTexture(SpriteSheet sheet) const;
    bool buildSortedObjectList(std::vector<GameObject*>& objects) const;
    void fetchBlendingStates(ColorChannelBlendStates& states) const;

private:
    PlayLayer* layer = nullptr;
    std::array<cocos2d::CCTexture2D*, static_cast<usize>(SpriteSheet::COUNT)> spriteSheets {};
    GroupManager groupManager;
    ObjectBatch objectBatch;
    bool ready = false;
};

} // namespace IOSUpstream

#endif
