#include "GroupManager.hpp"
#include "Renderer.hpp"
#include "glm/ext/matrix_transform.hpp"
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include "../profiler.hpp"
#include <optional>
#include <string>
#include <algorithm>

using namespace geode::prelude;

static std::optional<GroupID> getTargetGroupIdOfTransformingTrigger(GameObject* object) {
    switch (object->m_objectID) {
    case MOVE_TRIGGER_ID:
    case ROTATE_TRIGGER_ID:
    case FOLLOW_TRIGGER_ID:
        return ((EffectGameObject*)object)->m_targetGroupID;
    }
    return std::nullopt;
}

static std::optional<GroupID> getTargetGroupIdOfVisibilityTrigger(GameObject* object) {
    switch (object->m_objectID) {
    case ALPHA_TRIGGER_ID:
    case TOGGLE_TRIGGER_ID:
    case TOGGLE_ORB_ID:
        return ((EffectGameObject*)object)->m_targetGroupID;
    }
    return std::nullopt;
}

GroupCombination::GroupCombination(std::array<GroupID, 10>* groupIds, i32 count) {
    count = std::clamp<i32>(count, 0, 10);

    i32 i = 0;
    for (; i < count; i++)
        ids[i] = (*groupIds)[i];
    for (; i < 10; i++)
        ids[i] = 0;
    this->count = count;
    if (count < 2)
        return;
    std::sort(ids.data(), &ids[count]);
}

GroupCombination::GroupCombination(std::vector<GroupID> groupIds) {
    count = std::min<i32>((i32)groupIds.size(), 10);
    std::copy(groupIds.begin(), groupIds.begin() + count, ids.begin());
    std::fill(ids.begin() + count, ids.end(), 0);
    std::sort(ids.begin(), ids.begin() + count);
}

void GroupCombination::removeGroupIdsNotInSet(const std::set<GroupID> maskSet) {
    for (i32 i = 0; i < count; i++) {
        if (!maskSet.contains(ids[i]))
            ids[i] = 0;
    }

    i32 newCount = 0;
    i32 srcI = 0, dstI = 0;
    for (; srcI < count; srcI++) {
        if (ids[srcI] != 0) {
            ids[dstI] = ids[srcI];
            dstI++;
            newCount++;
        }
    }
    for (; dstI < 10; dstI++)
        ids[dstI] = 0;
    count = newCount;
}

GroupCombination::operator std::string() const {
    std::string ret = "[";
    for (u32 i = 0; i < count; i++) {
        if (i != 0) ret += ", ";
        ret += std::to_string(ids[i]);
    }
    return ret + "]";
}

GroupManager::~GroupManager() {}

void GroupManager::initWithObjects(cocos2d::CCArray* objects) {
    if (!objects)
        return;

    auto emptyGroupComb = GroupCombination(std::vector<GroupID>{});
    addGroupCombination(emptyGroupComb, 0);
    GroupCombinationIndex groupCombIndex = 1;

    for (auto object : CCArrayExt<GameObject*>(objects)) {
        auto transformGroupId = getTargetGroupIdOfTransformingTrigger(object);
        auto alphaGroupId     = getTargetGroupIdOfVisibilityTrigger(object);

        if (transformGroupId)
            transformGroupIds.insert(transformGroupId.value());
        else if (alphaGroupId)
            alphaGroupIds.insert(alphaGroupId.value());

        GroupCombination comb = GroupCombination(object);

        if (groupCombinationIndicies.find(comb) == groupCombinationIndicies.end()) {
            addGroupCombination(comb, groupCombIndex);
            groupCombIndex++;
        }
    }

    groupCombinationCount = groupCombIndex;

    u32 transformCombIndex = 0;
    u32 alphaCombIndex     = 0;

    alphaIndiciesPerGroupCombinationIndex.resize(groupCombinationCount);

    for (auto comb : groupCombinations) {
        auto combIndex = groupCombinationIndicies[comb];

        GroupCombination transformComb = comb;
        transformComb.removeGroupIdsNotInSet(transformGroupIds);

        if (transformCombinationIndicies.find(transformComb) == transformCombinationIndicies.end()) {
            transformCombinationIndicies[transformComb] = transformCombIndex;
            firstGroupIndexPerTransformIndex.push_back(combIndex);
            transformCombIndex++;
        }
        
        GroupCombination alphaComb = comb;
        alphaComb.removeGroupIdsNotInSet(alphaGroupIds);

        if (alphaIndicies.find(alphaComb) == alphaIndicies.end()) {
            alphaIndicies[alphaComb] = alphaCombIndex;
            alphaCombIndex++;

            alphaIndexGroupIdFastStructure.push_back(alphaComb.getCount());
            for (auto id : alphaComb.getSpan())
                alphaIndexGroupIdFastStructure.push_back(id);
        }

        alphaIndiciesPerGroupCombinationIndex[combIndex] = alphaIndicies[alphaComb];
    }

    alphaIndiciesCount = alphaCombIndex;

    {
        std::set<GroupID> alphaGroupIdsInCombinations;

        usize i = 0;
        while (i < alphaIndexGroupIdFastStructure.size()) {
            u32 groupCount = alphaIndexGroupIdFastStructure[i++];

            for (u32 j = 0; j < groupCount && i < alphaIndexGroupIdFastStructure.size(); j++)
                alphaGroupIdsInCombinations.insert((GroupID)alphaIndexGroupIdFastStructure[i++]);
        }

        alphaUpdateGroupIds.assign(alphaGroupIdsInCombinations.begin(), alphaGroupIdsInCombinations.end());
    }

    transformCombinationCount = transformCombIndex;
    log::info("Number of unique group transformations: {}", transformCombinationCount);

    alphaValuesPerGroupId.resize(maxGroupId + 1);
    alphaValuesPerAlphaIndex.resize(alphaIndiciesCount);

    groupStateBuffer = Buffer::createDynamicDraw("GroupStateBuffer", getGroupStateBufferSize());
    groupStates = groupStateBuffer ? (GroupCombinationState*)groupStateBuffer->mapReadWrite() : nullptr;
    if (!groupStates)
        log::error("Failed to create the group state buffer");

    isGroupToggledOn.resize((usize)maxGroupId + 1, true);

    fetchVanilaRotateGroups();
}
    
GroupCombinationIndex GroupManager::getGroupCombinationIndexForObject(GameObject* object) {
    auto comb = GroupCombination(object);

    auto it = groupCombinationIndicies.find(comb);
    if (it == groupCombinationIndicies.end()) {
        log::warn("No group combination found for object {}", object ? object->m_objectID : -1);
        return 0;
    }
    return it->second;
}

GroupCombinationIndex GroupManager::getTransformCombinationIndexForObject(GameObject* object) {
    auto comb = GroupCombination(object);
    comb.removeGroupIdsNotInSet(transformGroupIds);

    auto it = transformCombinationIndicies.find(comb);
    if (it == transformCombinationIndicies.end())
        return 0;

    return it->second;
}

void GroupManager::fetchVanilaRotateGroups() {
    PlayLayer* layer = renderer.getPlayLayer();
    if (!layer)
        return;

    // Group ids used by objects are not guaranteed to have an entry in the
    // game's group arrays, so size this from both sources.
    usize groupArraySize = std::max(layer->m_staticGroups.size(), layer->m_optimizedGroups.size());
    vanillaRotateGroups.resize(std::max<usize>(groupArraySize + 1, (usize)maxGroupId + 1));

    for (usize id = 1; id < layer->m_staticGroups.size() && id < vanillaRotateGroups.size(); id++) {
        CCArray* group = layer->m_staticGroups[id];
        if (!group) continue;

        auto& dstGroup = vanillaRotateGroups[id];

        for (auto object : CCArrayExt<GameObject*, false>(group)) {
            if (object && object->m_objectType != GameObjectType::Decoration)
                dstGroup.insert(object);
        }
    }

    for (usize id = 1; id < vanillaRotateGroups.size(); id++) {
        GameObject* mainObject = layer->tryGetMainObject((int)id);
        if (!mainObject || !mainObject->m_groups)
            continue;

        for (usize i = 0; i < mainObject->m_groupCount && i < mainObject->m_groups->size(); i++) {
            GroupID groupId = mainObject->m_groups->at(i);
            if (groupId > 0 && (usize)groupId < vanillaRotateGroups.size())
                vanillaRotateGroups[groupId].insert(mainObject);
        }
    }
}

void GroupManager::updateGroupStateBuffer() {
    PROFILER_SCOPE("GroupManager::updateGroupStateBuffer");

    if (!groupStates || !groupStateBuffer)
        return;
    
    updateAlphaValues();
    updatePulseEntries();

    // Group transforms, opacity and pulse indices only change when a trigger
    // or a pulse changes them, so most frames don't need an upload at all.
    if (!stateDirty)
        return;

    {
        ScopedFrameTimer timer(renderer.getFrameTiming().groupStatesUpload);
        groupStateBuffer->flush(0, groupStateBuffer->getSize());
    }

    stateDirty = false;
}

void GroupManager::storeGroupStates(StoredGroupStates& storage) {
    if (!groupStates)
        return;

    storage.groupStates.resize(groupCombinationCount);
    std::memcpy(storage.groupStates.data(), groupStates, getGroupStateBufferSize());

    storage.isGroupToggledOn.resize(isGroupToggledOn.size());
    std::copy(isGroupToggledOn.begin(), isGroupToggledOn.end(), storage.isGroupToggledOn.begin());
}

void GroupManager::restoreGroupStates(StoredGroupStates& storage) {
    if (!groupStates)
        return;

    usize numToCopy = std::min<usize>(storage.groupStates.size(), groupCombinationCount);
    std::memcpy(groupStates, storage.groupStates.data(), numToCopy * sizeof(GroupCombinationState));

    usize numToggles = std::min(storage.isGroupToggledOn.size(), isGroupToggledOn.size());
    std::copy_n(storage.isGroupToggledOn.begin(), numToggles, isGroupToggledOn.begin());

    /*
        Pulse indices are rebuilt from the effect manager every frame, so a
        restored index would only be stale. Clearing them keeps the tracked
        list of pulsing combinations correct.
    */
    for (auto& groupState : getGroupStates())
        groupState.pulseIndex = 0;

    pulsingGroupCombinationIndicies.clear();

    stateDirty = true;
}

void GroupManager::storeCheckpoint(CheckpointObject* checkpoint) {
    auto& groupStates = storedCheckpointGroupStates[checkpoint];
    storeGroupStates(groupStates);
}

void GroupManager::restoreCheckpoint(CheckpointObject* checkpoint) {
    if (!storedCheckpointGroupStates.contains(checkpoint))
        return;

    auto& groupStates = storedCheckpointGroupStates[checkpoint];
    restoreGroupStates(groupStates);
}

void GroupManager::removeCheckpoint(CheckpointObject* checkpoint) {
    storedCheckpointGroupStates.erase(checkpoint);
}

void GroupManager::clearCheckpoints() {
    storedCheckpointGroupStates.clear();
}

void GroupManager::moveGroup(GroupID groupId, float deltaX, float deltaY) {
    if (!groupStates || groupId < 0 || groupId > maxGroupId)
        return;

    if ((usize)groupId >= groupCombinationIndiciesPerGroupId.size())
        return;

    const auto& combIndicies = groupCombinationIndiciesPerGroupId[groupId];
    for (auto combIndex : combIndicies)
        groupStates[combIndex].offset += glm::vec2(deltaX, deltaY);

    stateDirty = true;
}

void GroupManager::rotateGroup(
    GroupID groupId,
    float angle,
    bool lockObjectRotation,
    std::optional<glm::vec2> centerPoint
) {
    if (!groupStates || groupId < 0 || groupId > maxGroupId)
        return;

    if ((usize)groupId >= groupCombinationIndiciesPerGroupId.size())
        return;

    float cos = cosf(glm::radians(angle));
    float sin = sinf(glm::radians(angle));
    glm::mat2 matrix = {
        { cos, -sin },
        { sin,  cos }
    };

    const auto& combIndicies = groupCombinationIndiciesPerGroupId[groupId];
    for (auto combIndex : combIndicies) {
        auto& groupState = groupStates[combIndex];

        if (!lockObjectRotation)
            groupState.localTransform *= matrix;

        if (centerPoint) {
            auto center = centerPoint.value();

            groupState.positionalTransform *= matrix;
            groupState.offset = matrix * (groupState.offset - center) + center;
        }
    }

    stateDirty = true;
}

void GroupManager::toggleGroup(GroupID groupId, bool enabled) {
    if (groupId < 0 || groupId > maxGroupId)
        return;

    if ((usize)groupId >= isGroupToggledOn.size())
        return;

    isGroupToggledOn[groupId] = enabled;
}

void GroupManager::resetGroupStates() {
    if (!groupStates)
        return;

    for (auto& groupState : getGroupStates()) {
        groupState.positionalTransform = glm::identity<glm::mat2>();
        groupState.localTransform = glm::identity<glm::mat2>();
        groupState.offset = glm::vec2(0, 0);
    }

    std::fill(isGroupToggledOn.begin(), isGroupToggledOn.end(), true);

    for (auto& groupState : getGroupStates())
        groupState.pulseIndex = 0;

    pulsingGroupCombinationIndicies.clear();

    stateDirty = true;
    alphaValuesInitialized = false;
}

void GroupManager::addGroupCombination(GroupCombination& comb, GroupCombinationIndex index) {
    if (index >= groupCombinations.size())
        groupCombinations.resize(index + 1);
    groupCombinations[index] = comb;

    groupCombinationIndicies[comb] = index;

    for (auto groupId : comb.getSpan()) {
        usedGroupIds.insert(groupId);

        if (groupId > maxGroupId)
            maxGroupId = groupId;

        if (groupId >= groupCombinationIndiciesPerGroupId.size())
            groupCombinationIndiciesPerGroupId.resize(groupId + 1);

        groupCombinationIndiciesPerGroupId[groupId].push_back(index);
    }
}

void GroupManager::updateAlphaValues() {
    ScopedFrameTimer timer(renderer.getFrameTiming().groupStatesAlpha);

    PlayLayer* layer = renderer.getPlayLayer();
    if (!layer || !layer->m_effectManager)
        return;

    auto effectManager = layer->m_effectManager;

    // Only group ids that appear in an alpha combination can affect the
    // opacity of a group combination, so the level's other (potentially
    // thousands of) group ids don't have to be queried every frame.
    bool groupValueChanged = !alphaValuesInitialized;

    for (GroupID id : alphaUpdateGroupIds) {
        usize groupId = (usize)id;

        if (groupId >= alphaValuesPerGroupId.size() || groupId >= isGroupToggledOn.size())
            continue;

        float value = isGroupToggledOn[groupId] ? effectManager->opacityModForGroup(id) : 0.0f;

        if (alphaValuesPerGroupId[groupId] != value) {
            alphaValuesPerGroupId[groupId] = value;
            groupValueChanged = true;
        }
    }

    if (!groupValueChanged)
        return;

    alphaValuesInitialized = true;

    u32* alphaFast = alphaIndexGroupIdFastStructure.data();
    for (u32 alphaIndex = 0; alphaIndex < alphaIndiciesCount; alphaIndex++) {
        u32 combCount = *(alphaFast++);
        float alpha = 1.0;
        while (combCount > 0) {
            u32 groupId = *(alphaFast++);
            if (groupId < alphaValuesPerGroupId.size())
                alpha *= alphaValuesPerGroupId[groupId];
            combCount--;
        }
        if (alphaIndex < alphaValuesPerAlphaIndex.size())
            alphaValuesPerAlphaIndex[alphaIndex] = alpha;
    }

    for (u32 combIndex = 0; combIndex < groupCombinationCount; combIndex++) {
        if (combIndex >= alphaIndiciesPerGroupCombinationIndex.size())
            continue;

        u32 alphaIndex = alphaIndiciesPerGroupCombinationIndex[combIndex];
        if (alphaIndex >= alphaValuesPerAlphaIndex.size())
            continue;

        float opacity = alphaValuesPerAlphaIndex[alphaIndex];

        if (groupStates[combIndex].opacity == opacity)
            continue;

        groupStates[combIndex].opacity = opacity;
        stateDirty = true;
    }
}

struct PulseEntry {
    u32 flags = 0;
    union {
        PackedHSV hsv;
        ccColor4B color;
    };

    inline static PulseEntry fromAction(const PulseEffectAction& action) {
        PulseEntry ret;

        ret.flags |= PULSE_FLAG_IS_ACTIVE;
        ret.flags |= (u32)(action.m_currentValue * 65535.0f) & 0xffff;

        if (action.m_mainOnly)     ret.flags |= PULSE_FLAG_IS_BASE_ONLY;
        if (action.m_detailOnly)   ret.flags |= PULSE_FLAG_IS_DETAIL_ONLY;
        if (action.m_isDynamicHsv) ret.flags |= PULSE_FLAG_IS_DYNAMIC_HSV;

        if (action.m_pulseEffectType == PulseEffectType::Hsv) {
            ret.flags |= PULSE_FLAG_IS_HSV;
            ret.flags |= (action.m_colorIndex & 0x3ff) << 16;
            ret.hsv = packHSV(action.m_hsv);
        } else {
            ret.color = { action.m_color.r, action.m_color.g, action.m_color.b, 255 };
        }

        return ret;
    }

    inline static PulseEntry endIndicator() { return PulseEntry(); }
};

// Reused between frames so a frame that has pulses doesn't allocate.
static std::vector<GroupCombinationIndex> pulsingCombinationScratch;
static std::vector<PulseEntry> pulseEntriesScratch;

void GroupManager::updatePulseEntries() {
    PROFILER_SCOPE("GroupManager::updatePulseEntries");
    ScopedFrameTimer timer(renderer.getFrameTiming().groupStatesPulse);

    PlayLayer* layer = renderer.getPlayLayer();
    if (!layer || !layer->m_effectManager || !groupStates)
        return;

    auto effectManager = layer->m_effectManager;

    /*
        Only the combinations that pulsed last frame can have a non zero pulse
        index. This used to walk every group combination in the level on every
        frame just to clear the few that actually pulsed.
    */
    for (auto combIndex : pulsingGroupCombinationIndicies) {
        groupStates[combIndex].pulseIndex = 0;
        stateDirty = true;
    }

    pulsingGroupCombinationIndicies.clear();

    if (groupCombinations.empty())
        return;

    auto& pulsingCombinations = pulsingCombinationScratch;
    pulsingCombinations.clear();

    for (usize gid = 0; gid < effectManager->m_unkVector2f0.size(); gid++) {
        if (!effectManager->m_unkVector2f0[gid] || gid >= groupCombinationIndiciesPerGroupId.size())
            continue;

        for (const auto& groupComb : groupCombinationIndiciesPerGroupId[gid])
            pulsingCombinations.push_back(groupComb);
    }

    auto& pulseEntries = pulseEntriesScratch;
    pulseEntries.clear();

    // This is so that pulse index 0 can be skipped immediately without
    // having to be checked
    pulseEntries.push_back(PulseEntry::endIndicator());

    for (auto groupCombIndex : pulsingCombinations) {
        u32 pulseIndex = (u32)pulseEntries.size();

        const auto& comb = groupCombinations[groupCombIndex];

        for (GroupID gid : comb.getSpan()) {
            if (gid < 0 || (usize)gid >= effectManager->m_unkVector2f0.size())
                continue;

            if (!effectManager->m_unkVector2f0[gid])
                continue;

            // find() instead of operator[], which would insert an empty entry
            // for a pulsing group that has no pulse actions.
            auto pulseActionsIt = effectManager->m_pulseEffectMap.find(gid);
            if (pulseActionsIt == effectManager->m_pulseEffectMap.end())
                continue;

            for (const auto& action : pulseActionsIt->second) {
                if (
                    action.m_pulseEffectType != PulseEffectType::Hsv &&
                    action.m_pulseEffectType != PulseEffectType::Color
                ) {
                    continue;
                }

                pulseEntries.push_back(PulseEntry::fromAction(action));
            }
        }

        if (pulseIndex == pulseEntries.size())
            continue;

        pulseEntries.push_back(PulseEntry::endIndicator());

        groupStates[groupCombIndex].pulseIndex = pulseIndex;
        pulsingGroupCombinationIndicies.push_back(groupCombIndex);
        stateDirty = true;
    }

    // A lone end indicator means nothing pulses, which is the case for most
    // frames. Its contents never change, so the buffer can keep its value.
    bool nothingPulses = pulseEntries.size() == 1;

    if (nothingPulses && pulseEntryBuffer && pulseEntryBufferHadNoPulses)
        return;

    pulseEntryBufferHadNoPulses = nothingPulses;

    pulseEntryBufferEnsureSize(pulseEntries.size() * sizeof(PulseEntry));
    if (!pulseEntryBuffer)
        return;

    // write() uploads through the mapping when the buffer is persistently
    // mapped, so the extra flush() this used to do was a second upload.
    pulseEntryBuffer->write(std::span<PulseEntry>(pulseEntries));
}

#define PULSE_ENTRY_BUFFER_SIZE_STEP (32 * 1024)

void GroupManager::pulseEntryBufferEnsureSize(usize size) {
    if (pulseEntryBuffer && pulseEntryBuffer->getSize() >= size)
        return;

    pulseEntryBuffer = nullptr;

    usize newSize = (size / PULSE_ENTRY_BUFFER_SIZE_STEP + 1) * PULSE_ENTRY_BUFFER_SIZE_STEP;

    log::info("Resized PulseEntryBuffer to {}", byteSizeToString(newSize));

    pulseEntryBuffer = Buffer::createDynamicDraw("PulseEntryBuffer", newSize);
}