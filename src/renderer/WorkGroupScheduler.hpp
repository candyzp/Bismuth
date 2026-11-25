#pragma once

#include <common.hpp>
#include <ObjectUtils.hpp>
#include "Buffer.hpp"
#include <map>
#include <queue>

#define WORK_GROUPS_PER_GPU_CORES 2.5

class Renderer;

using ColorChannelBlendStates = std::array<bool, 1101>;

struct WorkGroupRange {
    u32 offset, count;
};

struct SortedObjectBatch {
    u32 workGroupCount;
    std::map<LayerKey, WorkGroupRange> workGroupRangePerLayerKey;

    // Used to drop the least recently used batch from the cache.
    u64 lastUsedTime = 0;

    //// BUFFERS ////
    bool haveBuffersBeenCreated = false;
    UPtr<Buffer> spriteRemapBuffer;
    UPtr<Buffer> workGroupBuffer;
    UPtr<Buffer> drawCommandBuffer;

    //// BUFFER DATAS ////
    std::vector<u32> inputSpriteIndicies;
    std::vector<WorkGroup> workGroups;
    std::vector<DrawCommand> drawCommands;

    void bindBuffers();
};

class WorkGroupScheduler {
public:
    WorkGroupScheduler();
    ~WorkGroupScheduler();

    void addSprite(
        GameObject* object,
        cocos2d::CCSprite* sprite,
        SpriteType type,
        u32 inputSpriteIndex,
        u32 triCount
    );

    /*
        Sorts the sprite entries once, after every sprite of the level was
        added. Generation jobs then only have to bucket them, instead of
        sorting the whole level for every blend state change.
    */
    void finalizeSpriteEntries();

    /*
        The pre culled sprite data (start position and group combination) is
        the same for every blend state, so it lives in one buffer instead of
        one per sorted batch.
    */
    void writePreCulledSprites(PreCulledSprite::Buffer& buffer);

    inline usize getSpriteCount() const {
        return spriteEntries.size();
    }

    /*
        Returns the sorted batch for these blend states, or nullptr while one
        is still being generated. inUseBatch is never evicted from the cache.
    */
    SortedObjectBatch* fetchSortedBatchForBlendStates(
        const ColorChannelBlendStates& blendStates,
        SortedObjectBatch* inUseBatch = nullptr
    );

    inline void waitForJobs() {
        for (auto& [k, job] : currentGenerationJobs)
            if (job->thread.joinable())
                job->thread.join();
    }

private:
    void cleanUpFinishedJobs();

    void addGenerationJob(const ColorChannelBlendStates& blendStates, SortedObjectBatch* inUseBatch);

    /*
        Every blend state combination used to keep its own full set of gpu
        buffers forever. Keeping only the most recent ones bounds the vram
        usage of long sessions.
    */
    void trimSortedBatchCacheLocked(SortedObjectBatch* inUseBatch);

private:
    struct SpriteEntry {
        i32 zOrder;
        u32 entryIndex;
        ZLayer zLayer;
        SpriteSheet spriteSheet;
        SpriteType type;
        bool isDetailSpriteInFront;
        bool hasDetailSprite;
        u16 colorChannelId;
        u16 otherColorChannelId;
        u32 inputSpriteIndex;
        u32 triCount;
        u32 groupCombinationIndex;
        GameObject* object;

        LayerKey getFullLayerKey(const ColorChannelBlendStates& blendStates) const;
    };

    std::vector<SpriteEntry> spriteEntries;

    /*
        Indices into spriteEntries, sorted by z order. The sprite entries never
        change after loading, so generation jobs read this without locking.
    */
    std::vector<u32> spritesByZOrder;

    struct GenerationJob {
        //// INPUTS ////
        // scheduler may only be accessed if its mutex has been locked!
        WorkGroupScheduler& scheduler;
        
        ColorChannelBlendStates blendStates;

        isize maxWorkGroupCount = -1;

        //// THREAD ////
        std::thread thread;

        //// VARIABLES USED IN GENERATION ////
        std::map<LayerKey, std::vector<u32>> layerSprites;
        std::vector<u32> inputSpriteIndicies;
        std::vector<WorkGroup> workGroups;

        inline ~GenerationJob() {
            if (thread.joinable())
                thread.join();
        }

        void generateSortedBatchForBlendStates();

        void generateWorkGroupsForLayer(
            const std::vector<u32>& spriteIndicies,
            u32 spritesPerWorkGroup,
            u32& culledIndexBufferOutputIndex
        );
    };

    std::thread thread;

    isize maxWorkGroupCount = -1;

    std::map<ColorChannelBlendStates, UPtr<GenerationJob>> currentGenerationJobs;

    //// VARIABLES ACCESSED BY ALL THREADS ////
    
    std::mutex mutex;

    std::map<ColorChannelBlendStates, UPtr<SortedObjectBatch>> sortedBatchesPerBlendStates;
    std::set<GenerationJob*> finishedGenerationJobs;
};
