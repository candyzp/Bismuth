#include "WorkGroupScheduler.hpp"
#include "Renderer.hpp"

#include <algorithm>

using namespace geode::prelude;

WorkGroupScheduler::~WorkGroupScheduler() {
    waitForJobs();
    std::scoped_lock lock(mutex);
    currentGenerationJobs.clear();
}

void WorkGroupScheduler::addSprite(
    GameObject* object,
    cocos2d::CCSprite* sprite,
    SpriteType type,
    u32 inputSpriteIndex,
    u32 triCount
) {
    if (object != lastObject) {
        lastObject = object;
        lastGroupCombinationIndex = Renderer::get()->getGroupManager().getGroupCombinationIndexForObject(object);
    }

    spriteEntries.push_back({
        .zOrder = object->getObjectZOrder(),
        .zLayer = object->getObjectZLayer(),
        .spriteSheet = ObjectUtils::getSpritesheetOfObject(object, type),
        .colorChannelId = (u16)ObjectUtils::getSpriteColorChannel(type, object, sprite),
        .inputSpriteIndex = inputSpriteIndex,
        .triCount = triCount,
        .groupCombinationIndex = lastGroupCombinationIndex,
        .startPosition = ccPointToGLM(object->m_startPosition)
    });
}

SortedObjectBatch* WorkGroupScheduler::fetchSortedBatchForBlendStates(const ColorChannelBlendStates& blendStates) {
    cleanUpFinishedJobs();

    {
        std::scoped_lock lock(mutex);
        auto it = sortedBatchesPerBlendStates.find(blendStates);
        if (it != sortedBatchesPerBlendStates.end())
            return it->second.get();
    }

    addGenerationJob(blendStates);
    return nullptr;
}

std::vector<SortedObjectBatch*> WorkGroupScheduler::allBatches() {
    std::scoped_lock lock(mutex);
    std::vector<SortedObjectBatch*> batches;
    for (auto& [_, batch] : sortedBatchesPerBlendStates)
        batches.push_back(batch.get());
    return batches;
}

void WorkGroupScheduler::cleanUpFinishedJobs() {
    std::vector<UPtr<GenerationJob>> finished;
    {
        std::scoped_lock lock(mutex);

        while (!finishedGenerationJobs.empty()) {
            GenerationJob* job = *finishedGenerationJobs.begin();
            finishedGenerationJobs.erase(job);

            for (auto it = currentGenerationJobs.begin(); it != currentGenerationJobs.end(); it++) {
                if (it->second.get() == job) {
                    finished.push_back(std::move(it->second));
                    currentGenerationJobs.erase(it);
                    break;
                }
            }
        }
    }
    // Joined outside the lock (the thread may still be releasing it).
    finished.clear();
}

void WorkGroupScheduler::addGenerationJob(const ColorChannelBlendStates& blendStates) {
    std::scoped_lock lock(mutex);
    if (currentGenerationJobs.contains(blendStates))
        return;

    auto job = std::make_unique<GenerationJob>(*this);

    job->blendStates = blendStates;
    job->maxWorkGroupCount = std::clamp<isize>(maxWorkGroupCount, 1, 65535);
    job->fixedSpritesPerWorkGroup = fixedSpritesPerWorkGroup;

    job->thread = std::thread(&GenerationJob::generateSortedBatchForBlendStates, job.get());

    currentGenerationJobs[blendStates] = std::move(job);
}

LayerKey WorkGroupScheduler::SpriteEntry::getFullLayerKey(const ColorChannelBlendStates& blendingStates) const {
    return { zLayer, spriteSheet, blendingStates[colorChannelId] };
}

void WorkGroupScheduler::GenerationJob::generateSortedBatchForBlendStates() {
    using bismuth::UVec4;
    using bismuth::floatBits;

    usize spriteCount;
    {
        std::scoped_lock lock(scheduler.mutex);
        spriteCount = scheduler.spriteEntries.size();
        loadAndSortSprites(scheduler.spriteEntries);
    }

    u32 approxWorkGroupCount = 1;
    u32 spritesPerWorkGroup = 1;
    if (fixedSpritesPerWorkGroup > 0) {
        spritesPerWorkGroup = fixedSpritesPerWorkGroup;
        approxWorkGroupCount = std::max<u32>(
            (u32)((spriteCount + spritesPerWorkGroup - 1u) / spritesPerWorkGroup), 1u);
    } else {
        approxWorkGroupCount = std::max<u32>(std::min<u32>((u32)spriteCount, (u32)maxWorkGroupCount), 1);
        spritesPerWorkGroup = std::max<u32>((u32)spriteCount / approxWorkGroupCount, 1);
    }

    auto sortedBatch = std::make_unique<SortedObjectBatch>();
    auto& data = sortedBatch->data;

    std::vector<UVec4> sorted;
    std::vector<UVec4> workGroups;
    std::vector<UVec4> layers;
    sorted.reserve(spriteCount);
    workGroups.reserve(approxWorkGroupCount + sortedSpriteLayers.size());

    u32 outputTriangle = 0;

    for (auto& [key, sprites] : sortedSpriteLayers) {
        u32 layerIndex = (u32)layers.size();
        u32 firstWorkGroup = (u32)workGroups.size();
        u32 layerOutput = outputTriangle;

        usize i = 0;
        while (i < sprites.size()) {
            u32 begin = (u32)sorted.size();
            u32 workGroupOutput = outputTriangle;

            for (u32 n = 0; n < spritesPerWorkGroup && i < sprites.size(); n++, i++) {
                const SpriteEntry* sprite = sprites[i];
                sorted.push_back(UVec4(
                    floatBits(sprite->startPosition.x),
                    floatBits(sprite->startPosition.y),
                    sprite->groupCombinationIndex,
                    sprite->inputSpriteIndex
                ));
                outputTriangle += sprite->triCount;
            }

            workGroups.push_back(UVec4(begin, (u32)sorted.size(), workGroupOutput, layerIndex));
        }

        layers.push_back(UVec4(firstWorkGroup, (u32)workGroups.size() - firstWorkGroup, layerOutput, 0));
        sortedBatch->layerIndexPerKey[key] = { layerIndex, (u32)sprites.size() };
    }

    data.buffer = std::move(sorted);
    data.workGroupOffset = (u32)data.buffer.size();
    data.workGroupCount = (u32)workGroups.size();
    data.buffer.insert(data.buffer.end(), workGroups.begin(), workGroups.end());
    data.layerOffset = (u32)data.buffer.size();
    data.layerCount = (u32)layers.size();
    data.buffer.insert(data.buffer.end(), layers.begin(), layers.end());

    log::info(
        "Generated SortedObjectBatch: layers={}, workGroups={}, size={}",
        data.layerCount, data.workGroupCount, byteSizeToString(data.buffer.size() * sizeof(UVec4))
    );

    {
        std::scoped_lock lock(scheduler.mutex);

        scheduler.sortedBatchesPerBlendStates[blendStates] = std::move(sortedBatch);
        scheduler.finishedGenerationJobs.insert(this);
    }
}

void WorkGroupScheduler::GenerationJob::loadAndSortSprites(const std::vector<SpriteEntry>& sprites) {
    sortedSpriteLayers.clear();

    for (auto& sprite : sprites) {
        auto key = sprite.getFullLayerKey(blendStates);
        sortedSpriteLayers[key].push_back(&sprite);
    }

    for (auto& [key, layer] : sortedSpriteLayers) {
        std::stable_sort(layer.begin(), layer.end(), [](const SpriteEntry* a, const SpriteEntry* b) {
            return a->zOrder < b->zOrder;
        });
    }
}
