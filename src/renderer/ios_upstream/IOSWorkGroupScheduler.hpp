#pragma once

#ifdef GEODE_IS_IOS

#include "IOSRenderRecords.hpp"
#include <future>
#include <map>
#include <mutex>
#include <vector>

namespace IOSUpstream {

class WorkGroupScheduler {
public:
    explicit WorkGroupScheduler(u32 spritesPerWorkGroup = 128);
    ~WorkGroupScheduler();

    WorkGroupScheduler(const WorkGroupScheduler&) = delete;
    WorkGroupScheduler& operator=(const WorkGroupScheduler&) = delete;

    void addSprite(
        GameObject* object,
        cocos2d::CCSprite* sprite,
        SpriteType type,
        u32 inputSpriteIndex,
        u32 triangleCount,
        u32 groupCombinationIndex
    );

    usize getSpriteCount() const;

    // Returns nullptr while an order set is still being generated, matching
    // the owner's asynchronous scheduling contract.
    const SortedObjectBatch* fetchSortedBatchForBlendStates(
        const ColorChannelBlendStates& blendStates
    );

    const SortedObjectBatch* waitForSortedBatch(
        const ColorChannelBlendStates& blendStates
    );

    void waitForJobs();
    void clear();

private:
    struct SpriteEntry {
        i32 zOrder = 0;
        ZLayer zLayer = ZLayer::Default;
        SpriteSheet spriteSheet = SpriteSheet::GAME_1;
        u16 colorChannelId = 0;
        u32 inputSpriteIndex = 0;
        u32 triangleCount = 0;
        u32 groupCombinationIndex = 0;
        glm::vec2 startPosition {};
    };

    static SortedObjectBatch buildSortedBatch(
        const std::vector<SpriteEntry>& sprites,
        const ColorChannelBlendStates& blendStates,
        u32 spritesPerWorkGroup
    );

    void collectFinishedJobsLocked();

private:
    const u32 spritesPerWorkGroup;
    std::vector<SpriteEntry> spriteEntries;
    mutable std::mutex mutex;
    std::map<ColorChannelBlendStates, SortedObjectBatch> sortedBatches;
    std::map<ColorChannelBlendStates, std::future<SortedObjectBatch>> generationJobs;
};

} // namespace IOSUpstream

#endif
