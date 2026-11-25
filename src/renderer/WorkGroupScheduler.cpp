#include "WorkGroupScheduler.hpp"
#include "Renderer.hpp"
#include "../cl.hpp"
#include <algorithm>

void SortedObjectBatch::bindBuffers() {
    if (!haveBuffersBeenCreated) {
        spriteRemapBuffer = Buffer::createStaticDraw(
            "SpriteRemapBuffer",
            inputSpriteIndicies.data(),
            inputSpriteIndicies.size() * sizeof(u32)
        );

        if (!spriteRemapBuffer)
            return;

        workGroupBuffer = Buffer::createStaticDraw(
            "WorkGroupBuffer",
            workGroups.data(),
            workGroups.size() * sizeof(WorkGroup)
        );

        if (!workGroupBuffer)
            return;

        drawCommandBuffer = Buffer::createGPUExclusive(
            "DrawCommandBuffer",
            workGroups.size() * sizeof(DrawCommand)
        );

        if (!drawCommandBuffer)
            return; // FIXME: Make sure that this shows an error and exits the level

        drawCommandBuffer->write(drawCommands.data(), drawCommandBuffer->getSize());

        inputSpriteIndicies.clear();
        workGroups.clear();
        drawCommands.clear();
        haveBuffersBeenCreated = true;
    }

    spriteRemapBuffer->bindAsStorageBuffer(SPRITE_REMAP_BUFFER_BINDING);
    workGroupBuffer->bindAsStorageBuffer(WORK_GROUP_BUFFER_BINDING);
    drawCommandBuffer->bindAsStorageBuffer(DRAW_COMMAND_BUFFER_BINDING);
}

WorkGroupScheduler::WorkGroupScheduler() {
    if (maxWorkGroupCount == -1) {
        maxWorkGroupCount = getGPUCoreCount();
        if (maxWorkGroupCount == -1)
            maxWorkGroupCount = 8000; // dummy value
        else
            maxWorkGroupCount = (double)maxWorkGroupCount * WORK_GROUPS_PER_GPU_CORES;
    }
}

WorkGroupScheduler::~WorkGroupScheduler() {
    if (thread.joinable())
        thread.join();
}

void WorkGroupScheduler::addSprite(
    GameObject* object,
    cocos2d::CCSprite* sprite,
    SpriteType type,
    u32 inputSpriteIndex,
    u32 triCount
) {
    spriteEntries.push_back({
        .zOrder = object->getObjectZOrder(),
        .entryIndex = (u32)spriteEntries.size(),
        .zLayer = object->getObjectZLayer(),
        .spriteSheet = ObjectUtils::getSpritesheetOfObject(object, type),
        .type = type,
        .isDetailSpriteInFront = object->m_colorZLayerRelated,
        .hasDetailSprite = !!object->m_colorSprite,
        .colorChannelId = (u16)ObjectUtils::getSpriteColorChannel(type, object, sprite),
        .otherColorChannelId = (type == SpriteType::BASE) ? (u16)object->m_activeDetailColorID : (u16)object->m_activeMainColorID,
        .inputSpriteIndex = inputSpriteIndex,
        .triCount = triCount,
        .groupCombinationIndex = Renderer::get()->getGroupManager().getGroupCombinationIndexForObject(object),
        .object = object
    });
}

void WorkGroupScheduler::finalizeSpriteEntries() {
    spritesByZOrder.resize(spriteEntries.size());

    for (u32 i = 0; i < spriteEntries.size(); i++)
        spritesByZOrder[i] = i;

    std::sort(spritesByZOrder.begin(), spritesByZOrder.end(), [this](u32 a, u32 b) {
        const auto& spriteA = spriteEntries[a];
        const auto& spriteB = spriteEntries[b];

        if (spriteA.zOrder != spriteB.zOrder)
            return spriteA.zOrder < spriteB.zOrder;

        return spriteA.entryIndex < spriteB.entryIndex;
    });
}

void WorkGroupScheduler::writePreCulledSprites(PreCulledSprite::Buffer& buffer) {
    for (const auto& sprite : spriteEntries) {
        buffer[sprite.inputSpriteIndex].groupCombinationIndex = sprite.groupCombinationIndex;
        buffer[sprite.inputSpriteIndex].startPosition = ccPointToGLM(sprite.object->m_startPosition);
    }
}

SortedObjectBatch* WorkGroupScheduler::fetchSortedBatchForBlendStates(
    const ColorChannelBlendStates& blendStates,
    SortedObjectBatch* inUseBatch
) {
    cleanUpFinishedJobs();
    
    {
        std::scoped_lock lock(mutex);

        auto it = sortedBatchesPerBlendStates.find(blendStates);

        if (it != sortedBatchesPerBlendStates.end()) {
            if (it->second) {
                it->second->lastUsedTime = getTime();
                return it->second.get();
            }

            return nullptr;
        }
    }

    addGenerationJob(blendStates, inUseBatch);
    return nullptr;
}

static constexpr usize MAX_CACHED_SORTED_BATCHES = 12;

void WorkGroupScheduler::trimSortedBatchCacheLocked(SortedObjectBatch* inUseBatch) {
    while (sortedBatchesPerBlendStates.size() > MAX_CACHED_SORTED_BATCHES) {
        auto victim = sortedBatchesPerBlendStates.end();
        u64 oldest = ~0ull;

        for (auto it = sortedBatchesPerBlendStates.begin(); it != sortedBatchesPerBlendStates.end(); ++it) {
            if (!it->second || it->second.get() == inUseBatch)
                continue;

            if (it->second->lastUsedTime > oldest)
                continue;

            oldest = it->second->lastUsedTime;
            victim = it;
        }

        if (victim == sortedBatchesPerBlendStates.end())
            return;

        sortedBatchesPerBlendStates.erase(victim);
    }
}

void WorkGroupScheduler::cleanUpFinishedJobs() {
    std::scoped_lock lock(mutex);

    while (!finishedGenerationJobs.empty()) {
        GenerationJob* job = *finishedGenerationJobs.begin();
        finishedGenerationJobs.erase(job);

        for (auto it = currentGenerationJobs.begin(); it != currentGenerationJobs.end(); it++) {
            if (it->second.get() == job) {
                // The job owns its thread. Join it before the job is destroyed
                // so the thread can never outlive the object it runs on.
                if (job->thread.joinable())
                    job->thread.join();

                currentGenerationJobs.erase(it);
                break;
            }
        }
    }
}

void WorkGroupScheduler::addGenerationJob(const ColorChannelBlendStates& blendStates, SortedObjectBatch* inUseBatch) {
    std::scoped_lock lock(mutex);

    /*
        Only one batch is generated at a time. Generating several of them in
        parallel used to put one thread per blend state on the level's sprites
        at once, which stole the CPU from the game without finishing any of
        them sooner.
    */
    if (!currentGenerationJobs.empty())
        return;

    if (currentGenerationJobs.contains(blendStates))
        return;

    trimSortedBatchCacheLocked(inUseBatch);

    auto job = std::make_unique<GenerationJob>(*this);

    job->blendStates = blendStates;
    job->maxWorkGroupCount = maxWorkGroupCount;

    job->thread = std::thread(&GenerationJob::generateSortedBatchForBlendStates, job.get());

    currentGenerationJobs[blendStates] = std::move(job);
}

LayerKey WorkGroupScheduler::SpriteEntry::getFullLayerKey(const ColorChannelBlendStates& blendingStates) const {
    auto zLayerOut = zLayer;

    auto isColorChannelBlending      = blendingStates[colorChannelId];
    auto isOtherColorChannelBlending = blendingStates[otherColorChannelId];

    if (spriteSheet != SpriteSheet::PARTICLE) {
        if (
            type == SpriteType::BASE &&
            hasDetailSprite &&
            isColorChannelBlending &&
            !isOtherColorChannelBlending &&
            !isDetailSpriteInFront
        ) {
            zLayerOut = (ZLayer)((int)zLayerOut + 1);
        } else if (type == SpriteType::DETAIL && isColorChannelBlending && isDetailSpriteInFront) {
            zLayerOut = (ZLayer)((int)zLayerOut + 1);
        }
    }

    return { zLayerOut, spriteSheet, isColorChannelBlending };
}

void WorkGroupScheduler::GenerationJob::generateSortedBatchForBlendStates() {
    /*
        The sprite entries never change once the level finished loading, so
        this thread reads them without the scheduler lock. That lock used to be
        held for the entire sort, which blocked the main thread (and with it
        the frame) every time the level changed a blending state.
    */
    const auto& sprites = scheduler.spriteEntries;

    if (sprites.empty()) {
        std::scoped_lock lock(scheduler.mutex);
        scheduler.sortedBatchesPerBlendStates[blendStates] = nullptr;
        scheduler.finishedGenerationJobs.insert(this);
        return;
    }

    usize spriteCount = sprites.size();

    /*
        Bucketing the sprites in z order already yields sorted layer contents,
        so no sort has to happen here at all. This is what makes regenerating a
        batch cheap enough to never be noticeable on a large level.
    */
    for (auto& [key, layer] : layerSprites)
        layer.clear();

    for (u32 spriteIndex : scheduler.spritesByZOrder) {
        auto key = sprites[spriteIndex].getFullLayerKey(blendStates);
        layerSprites[key].push_back(spriteIndex);
    }

    inputSpriteIndicies.clear();
    inputSpriteIndicies.reserve(spriteCount);

    u32 maxWorkGroups = (u32)std::max<isize>(maxWorkGroupCount, 1);
    u32 approxWorkGroupCount = (u32)std::min<usize>(spriteCount, maxWorkGroups);
    u32 approxSpritesPerWorkGroup = (u32)(spriteCount / approxWorkGroupCount);

    workGroups.clear();
    workGroups.reserve(approxWorkGroupCount + 100);

    std::map<LayerKey, WorkGroupRange> workGroupRangePerLayerKey;

    u32 culledIndexBufferOutputIndex = 0;

    for (auto& [key, layer] : layerSprites) {
        if (layer.empty())
            continue;

        u32 workGroupStartIndex = workGroups.size();
        generateWorkGroupsForLayer(layer, approxSpritesPerWorkGroup, culledIndexBufferOutputIndex);
        u32 workGroupEndIndex = workGroups.size();

        workGroupRangePerLayerKey[key] = {
            .offset = workGroupStartIndex,
            .count  = workGroupEndIndex - workGroupStartIndex
        };
    }

    std::vector<DrawCommand> drawCommands;
    drawCommands.resize(workGroups.size());

    for (usize i = 0; i < workGroups.size(); i++) {
        drawCommands[i].baseInstance  = 0;
        drawCommands[i].instanceCount = 1;
        drawCommands[i].first = workGroups[i].culledIndexBufferOutputIndex * 3;
    }

    usize totalSize = inputSpriteIndicies.size() * sizeof(u32) +
                      workGroups.size() * sizeof(WorkGroup) +
                      drawCommands.size() * sizeof(DrawCommand);

    log::info("Generated SortedObjectBatch: workGroupCount={}, vramUsage={}", workGroups.size(), byteSizeToString(totalSize));

    auto sortedBatch = std::make_unique<SortedObjectBatch>();

    sortedBatch->workGroupCount = (u32)workGroups.size();
    sortedBatch->workGroupRangePerLayerKey = std::move(workGroupRangePerLayerKey);
    sortedBatch->haveBuffersBeenCreated = false; // Buffers can only be created on the main thread!
    sortedBatch->inputSpriteIndicies = std::move(inputSpriteIndicies);
    sortedBatch->workGroups = std::move(workGroups);
    sortedBatch->drawCommands = std::move(drawCommands);
    sortedBatch->lastUsedTime = getTime();

    {
        std::scoped_lock lock(scheduler.mutex);

        scheduler.sortedBatchesPerBlendStates[blendStates] = std::move(sortedBatch);
        scheduler.finishedGenerationJobs.insert(this);
    }
}

void WorkGroupScheduler::GenerationJob::generateWorkGroupsForLayer(
    const std::vector<u32>& spriteIndicies,
    u32 spritesPerWorkGroup,
    u32& culledIndexBufferOutputIndex
) {
    assert(spritesPerWorkGroup > 0);

    const auto& sprites = scheduler.spriteEntries;

    u32 i = 0;
    while (i < spriteIndicies.size()) {
        u32 spritesToAdd = spritesPerWorkGroup;

        u32 culledOutputIndex = culledIndexBufferOutputIndex;
        u32 startIndex = inputSpriteIndicies.size();

        while (spritesToAdd > 0 && i < spriteIndicies.size()) {
            const auto& sprite = sprites[spriteIndicies[i]];

            inputSpriteIndicies.push_back(sprite.inputSpriteIndex);
            culledIndexBufferOutputIndex += sprite.triCount;

            spritesToAdd--;
            i++;
        }

        u32 endIndex = inputSpriteIndicies.size();

        workGroups.push_back({
            .inputSpriteStart = startIndex,
            .inputSpriteEnd   = endIndex,
            .culledIndexBufferOutputIndex = culledOutputIndex
        });
    }
}
