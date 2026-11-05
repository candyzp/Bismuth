#pragma once

#include <common.hpp>
#include <ObjectUtils.hpp>
#include "BatchData.hpp"
#include <array>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

class Renderer;

using ColorChannelBlendStates = std::array<bool, 1101>;

struct SortedLayerInfo {
    u32 index = 0;        // layer index inside SortedBatchData
    u32 spriteCount = 0;
};

/*
    All sprites, sorted for one combination of color channel blending states
    (blending moves sprites between the normal and additive layers).
*/
struct SortedObjectBatch {
    bismuth::SortedBatchData data;
    std::map<LayerKey, SortedLayerInfo> layerIndexPerKey;
};

class WorkGroupScheduler {
public:
    WorkGroupScheduler() = default;
    ~WorkGroupScheduler();

    /// CPU path: target enough small groups to keep worker threads balanced.
    void setMaxWorkGroupCount(isize count) { maxWorkGroupCount = count; }

    /// GPU path: one compute lane processes one sprite in a fixed-width group.
    void setFixedSpritesPerWorkGroup(u32 count) { fixedSpritesPerWorkGroup = count ? count : 1; }

    void addSprite(
        GameObject* object,
        cocos2d::CCSprite* sprite,
        SpriteType type,
        u32 inputSpriteIndex,
        u32 triCount
    );

    inline usize getSpriteCount() const {
        return spriteEntries.size();
    }

    SortedObjectBatch* fetchSortedBatchForBlendStates(const ColorChannelBlendStates& blendStates);

    inline void waitForJobs() {
        std::vector<std::thread*> threads;
        {
            std::scoped_lock lock(mutex);
            for (auto& [k, job] : currentGenerationJobs)
                threads.push_back(&job->thread);
        }
        for (auto thread : threads)
            if (thread->joinable())
                thread->join();
    }

    /// Batches that were created since the last call (their GPU resources are new).
    std::vector<SortedObjectBatch*> allBatches();

private:
    void cleanUpFinishedJobs();

    void addGenerationJob(const ColorChannelBlendStates& blendStates);

private:
    struct SpriteEntry {
        i32 zOrder;
        ZLayer zLayer;
        SpriteSheet spriteSheet;
        u16 colorChannelId;
        u32 inputSpriteIndex;
        u32 triCount;
        u32 groupCombinationIndex;
        glm::vec2 startPosition;

        LayerKey getFullLayerKey(const ColorChannelBlendStates& blendStates) const;
    };

    std::vector<SpriteEntry> spriteEntries;
    // addSprite() gets an object's sprites one after another.
    GameObject* lastObject = nullptr;
    u32 lastGroupCombinationIndex = 0;

    struct GenerationJob {
        //// INPUTS ////
        // scheduler may only be accessed if its mutex has been locked!
        WorkGroupScheduler& scheduler;

        ColorChannelBlendStates blendStates;

        isize maxWorkGroupCount = -1;
        u32 fixedSpritesPerWorkGroup = 0;

        //// THREAD ////
        std::thread thread;

        //// VARIABLES USED IN GENERATION ////
        std::map<LayerKey, std::vector<const SpriteEntry*>> sortedSpriteLayers;

        inline GenerationJob(WorkGroupScheduler& scheduler) : scheduler(scheduler) {}

        inline ~GenerationJob() {
            if (thread.joinable())
                thread.join();
        }

        void generateSortedBatchForBlendStates();

        void loadAndSortSprites(const std::vector<SpriteEntry>& sprites);
    };

    isize maxWorkGroupCount = 8000;
    u32 fixedSpritesPerWorkGroup = 0;

    std::map<ColorChannelBlendStates, UPtr<GenerationJob>> currentGenerationJobs;

    //// VARIABLES ACCESSED BY ALL THREADS ////

    std::mutex mutex;

    std::map<ColorChannelBlendStates, UPtr<SortedObjectBatch>> sortedBatchesPerBlendStates;
    std::set<GenerationJob*> finishedGenerationJobs;
};
