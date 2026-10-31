#ifdef GEODE_IS_IOS

#include "IOSWorkGroupScheduler.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

namespace IOSUpstream {

WorkGroupScheduler::WorkGroupScheduler(u32 spritesPerWorkGroup)
  : spritesPerWorkGroup(std::max<u32>(1, spritesPerWorkGroup)) {}

WorkGroupScheduler::~WorkGroupScheduler() {
    waitForJobs();
}

void WorkGroupScheduler::addSprite(
    GameObject* object,
    cocos2d::CCSprite* sprite,
    SpriteType type,
    u32 inputSpriteIndex,
    u32 triangleCount,
    u32 groupCombinationIndex
) {
    if (!object || !sprite || triangleCount == 0)
        return;

    SpriteEntry entry;
    entry.zOrder = object->getObjectZOrder();
    // Match upstream WorkGroupScheduler::addSprite: the scheduler stores
    // the raw GameObject layer. ObjectSorter performs its own normalization
    // only when finding the original object collection layer.
    entry.zLayer = object->getObjectZLayer();
    entry.spriteSheet = ObjectUtils::getSpritesheetOfObject(object, type);
    const u32 colorChannelFlags = ObjectUtils::getSpriteColorChannel(type, object, sprite);
    // Detail/direct-tint metadata shares the high bits of the fork's encoded
    // channel field. The upstream scheduler's blend-state lookup uses only
    // the actual color channel ID, never those metadata bits.
    entry.colorChannelId = static_cast<u16>(colorChannelForSortKey(colorChannelFlags));
    entry.inputSpriteIndex = inputSpriteIndex;
    entry.triangleCount = triangleCount;
    entry.groupCombinationIndex = groupCombinationIndex;
    entry.startPosition = ccPointToGLM(object->m_startPosition);

    std::scoped_lock lock(mutex);
    spriteEntries.push_back(entry);
}

usize WorkGroupScheduler::getSpriteCount() const {
    std::scoped_lock lock(mutex);
    return spriteEntries.size();
}

const SortedObjectBatch* WorkGroupScheduler::fetchSortedBatchForBlendStates(
    const ColorChannelBlendStates& blendStates
) {
    std::scoped_lock lock(mutex);
    collectFinishedJobsLocked();

    if (auto found = sortedBatches.find(blendStates); found != sortedBatches.end())
        return &found->second;

    if (!generationJobs.contains(blendStates)) {
        const auto snapshot = spriteEntries;
        const u32 groupSize = spritesPerWorkGroup;
        generationJobs.emplace(
            blendStates,
            std::async(std::launch::async, [snapshot, blendStates, groupSize]() {
                return buildSortedBatch(snapshot, blendStates, groupSize);
            })
        );
    }

    return nullptr;
}

const SortedObjectBatch* WorkGroupScheduler::waitForSortedBatch(
    const ColorChannelBlendStates& blendStates
) {
    {
        std::scoped_lock lock(mutex);
        if (auto found = sortedBatches.find(blendStates); found != sortedBatches.end())
            return &found->second;

        if (!generationJobs.contains(blendStates)) {
            const auto snapshot = spriteEntries;
            const u32 groupSize = spritesPerWorkGroup;
            generationJobs.emplace(
                blendStates,
                std::async(std::launch::async, [snapshot, blendStates, groupSize]() {
                    return buildSortedBatch(snapshot, blendStates, groupSize);
                })
            );
        }
    }

    std::future<SortedObjectBatch> pending;
    {
        std::scoped_lock lock(mutex);
        auto found = generationJobs.find(blendStates);
        if (found == generationJobs.end()) {
            // Another fetch may have collected the completed future while this
            // caller was between the two lock scopes.
            auto ready = sortedBatches.find(blendStates);
            return ready == sortedBatches.end() ? nullptr : &ready->second;
        }
        pending = std::move(found->second);
        generationJobs.erase(found);
    }

    SortedObjectBatch completed = pending.get();
    std::scoped_lock lock(mutex);
    auto [it, inserted] = sortedBatches.emplace(blendStates, std::move(completed));
    return &it->second;
}

void WorkGroupScheduler::waitForJobs() {
    std::map<ColorChannelBlendStates, std::future<SortedObjectBatch>> pending;
    {
        std::scoped_lock lock(mutex);
        pending.swap(generationJobs);
    }

    for (auto& [blendStates, future] : pending) {
        if (!future.valid())
            continue;
        auto completed = future.get();
        std::scoped_lock lock(mutex);
        sortedBatches.insert_or_assign(blendStates, std::move(completed));
    }
}

void WorkGroupScheduler::clear() {
    waitForJobs();
    std::scoped_lock lock(mutex);
    spriteEntries.clear();
    sortedBatches.clear();
    generationJobs.clear();
}

void WorkGroupScheduler::collectFinishedJobsLocked() {
    for (auto it = generationJobs.begin(); it != generationJobs.end();) {
        if (it->second.valid() &&
            it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto completed = it->second.get();
            sortedBatches.insert_or_assign(it->first, std::move(completed));
            it = generationJobs.erase(it);
        } else {
            ++it;
        }
    }
}

SortedObjectBatch WorkGroupScheduler::buildSortedBatch(
    const std::vector<SpriteEntry>& sprites,
    const ColorChannelBlendStates& blendStates,
    u32 spritesPerWorkGroup
) {
    struct LayerKeyLess {
        bool operator()(const LayerKey& lhs, const LayerKey& rhs) const {
            return lhs < rhs;
        }
    };

    std::map<LayerKey, std::vector<const SpriteEntry*>, LayerKeyLess> sortedLayers;
    for (const auto& sprite : sprites) {
        const LayerKey key {
            sprite.zLayer,
            sprite.spriteSheet,
            blendStates[colorChannelForSortKey(sprite.colorChannelId)]
        };
        sortedLayers[key].push_back(&sprite);
    }

    for (auto& [key, layer] : sortedLayers) {
        std::stable_sort(layer.begin(), layer.end(), [](const auto* lhs, const auto* rhs) {
            return lhs->zOrder < rhs->zOrder;
        });
    }

    SortedObjectBatch result;
    result.inputSpriteIndices.reserve(sprites.size());
    result.preCulledSprites.reserve(sprites.size());

    u32 outputTriangleIndex = 0;
    for (const auto& [key, layer] : sortedLayers) {
        LayerDrawRange range;
        range.key = key;
        range.firstInputSprite = static_cast<u32>(result.inputSpriteIndices.size());
        range.firstVertex = outputTriangleIndex * 3;
        range.firstWorkGroup = static_cast<u32>(result.workGroups.size());

        for (usize begin = 0; begin < layer.size(); begin += spritesPerWorkGroup) {
            const usize end = std::min<usize>(layer.size(), begin + spritesPerWorkGroup);
            WorkGroup group;
            group.inputSpriteStart = static_cast<u32>(result.inputSpriteIndices.size());
            group.inputSpriteEnd = group.inputSpriteStart;
            group.culledIndexBufferOutputIndex = outputTriangleIndex;

            for (usize i = begin; i < end; ++i) {
                const auto& sprite = *layer[i];
                result.inputSpriteIndices.push_back(sprite.inputSpriteIndex);
                result.preCulledSprites.push_back({
                    sprite.groupCombinationIndex,
                    sprite.startPosition
                });
                outputTriangleIndex += sprite.triangleCount;
                ++group.inputSpriteEnd;
            }

            result.workGroups.push_back(group);
        }

        range.spriteCount = static_cast<u32>(layer.size());
        range.vertexCount = (outputTriangleIndex * 3) - range.firstVertex;
        range.workGroupCount =
            static_cast<u32>(result.workGroups.size()) - range.firstWorkGroup;
        result.layerRanges.emplace(key, range);
    }

    result.workGroupCount = static_cast<u32>(result.workGroups.size());
    return result;
}

} // namespace IOSUpstream

#endif
