#ifdef GEODE_IS_IOS

#include "IOSGroupManager.hpp"

#include <Geode/Enums.hpp>
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/utils/cocos.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>

using namespace geode::prelude;

namespace IOSUpstream {
namespace {

static std::optional<GroupID> getTargetGroupIdOfTransformingTrigger(GameObject* object) {
    if (!object)
        return std::nullopt;

    switch (object->m_objectID) {
        case IOS_MOVE_TRIGGER_ID:
        case IOS_ROTATE_TRIGGER_ID:
        case IOS_FOLLOW_TRIGGER_ID:
            return static_cast<EffectGameObject*>(object)->m_targetGroupID;
        default:
            return std::nullopt;
    }
}

static u32 convertToShaderHSV(const cocos2d::ccHSVValue& hsv) {
    const u32 hue = static_cast<u32>(hsv.h + 256.f);
    const u32 saturation = static_cast<u32>(
        (hsv.s + (hsv.absoluteSaturation ? 1.0f : 0.0f)) * 127.5f
    );
    const u32 value = static_cast<u32>(
        (hsv.v + (hsv.absoluteBrightness ? 1.0f : 0.0f)) * 127.5f
    );

    u32 result =
        ((hue & 0x1ffu) << 16) |
        ((saturation & 0xffu) << 8) |
        (value & 0xffu);

    if (hsv.absoluteSaturation)
        result |= 0x20000000u;
    if (hsv.absoluteBrightness)
        result |= 0x10000000u;
    return result;
}

struct EncodedPulseEntry {
    u32 flags = 0;
    u32 colorOrHSV = 0;
};

static EncodedPulseEntry makePulseEntry(const PulseEffectAction& action) {
    EncodedPulseEntry entry;
    entry.flags |= IOS_PULSE_FLAG_IS_ACTIVE;
    entry.flags |=
        (static_cast<u32>(action.m_currentValue * 65535.0f) & 0xffffu);

    if (action.m_mainOnly)
        entry.flags |= IOS_PULSE_FLAG_IS_BASE_ONLY;
    if (action.m_detailOnly)
        entry.flags |= IOS_PULSE_FLAG_IS_DETAIL_ONLY;
    if (action.m_isDynamicHsv)
        entry.flags |= IOS_PULSE_FLAG_IS_DYNAMIC_HSV;

    if (action.m_pulseEffectType == PulseEffectType::Hsv) {
        entry.flags |= IOS_PULSE_FLAG_IS_HSV;
        entry.flags |= (static_cast<u32>(action.m_colorIndex) & 0x3ffu) << 16;
        entry.colorOrHSV = convertToShaderHSV(action.m_hsv);
    } else {
        const auto& color = action.m_color;
        entry.colorOrHSV =
            static_cast<u32>(color.r) |
            (static_cast<u32>(color.g) << 8) |
            (static_cast<u32>(color.b) << 16) |
            (0xffu << 24);
    }
    return entry;
}

} // namespace

GroupCombination::GroupCombination(
    std::array<GroupID, IOS_GROUP_IDS_PER_OBJECT>* groupIds,
    i32 count
) {
    if (!groupIds || count < 0 || count > static_cast<i32>(IOS_GROUP_IDS_PER_OBJECT)) {
        this->count = 0;
        ids.fill(0);
        return;
    }

    this->count = count;
    for (i32 i = 0; i < count; ++i)
        ids[static_cast<usize>(i)] = groupIds->at(static_cast<usize>(i));
    std::fill(ids.begin() + count, ids.end(), 0);
    std::sort(ids.begin(), ids.begin() + count);
}

GroupCombination::GroupCombination(std::vector<GroupID> groupIds) {
    count = static_cast<i32>(std::min<usize>(groupIds.size(), IOS_GROUP_IDS_PER_OBJECT));
    std::copy_n(groupIds.begin(), count, ids.begin());
    std::fill(ids.begin() + count, ids.end(), 0);
    std::sort(ids.begin(), ids.begin() + count);
}

GroupCombination::GroupCombination(GameObject* object) {
    if (!object) {
        count = 0;
        ids.fill(0);
        return;
    }
    auto groups = object->m_groups;
    count = groups
        ? std::clamp<i32>(object->m_groupCount, 0, IOS_GROUP_IDS_PER_OBJECT)
        : 0;
    for (i32 i = 0; i < count; ++i)
        ids[static_cast<usize>(i)] = groups->at(static_cast<usize>(i));
    std::fill(ids.begin() + count, ids.end(), 0);
    std::sort(ids.begin(), ids.begin() + count);
}

void GroupCombination::removeGroupIdsNotInSet(const std::set<GroupID>& maskSet) {
    i32 destination = 0;
    for (i32 source = 0; source < count; ++source) {
        const GroupID id = ids[static_cast<usize>(source)];
        if (maskSet.contains(id))
            ids[static_cast<usize>(destination++)] = id;
    }
    std::fill(ids.begin() + destination, ids.end(), 0);
    count = destination;
}

bool GroupCombination::operator<(const GroupCombination& other) const {
    if (count != other.count)
        return count < other.count;
    for (i32 i = 0; i < count; ++i) {
        if (ids[static_cast<usize>(i)] != other.ids[static_cast<usize>(i)])
            return ids[static_cast<usize>(i)] < other.ids[static_cast<usize>(i)];
    }
    return false;
}

std::span<GroupID> GroupCombination::getSpan() {
    return { ids.data(), static_cast<usize>(count) };
}

std::span<const GroupID> GroupCombination::getSpan() const {
    return { ids.data(), static_cast<usize>(count) };
}

GroupCombination::operator std::string() const {
    std::string result = "[";
    for (i32 i = 0; i < count; ++i) {
        if (i)
            result += ", ";
        result += std::to_string(ids[static_cast<usize>(i)]);
    }
    return result + "]";
}

bool GroupManager::initWithObjects(PlayLayer* playLayer, cocos2d::CCArray* objects) {
    if (!playLayer || !objects)
        return false;

    layer = playLayer;
    groupStates.clear();
    pulseEntries.clear();
    isGroupToggledOn.clear();
    alphaIndicesPerGroupCombinationIndex.clear();
    alphaIndexGroupIdFastStructure.clear();
    alphaValuesPerGroupId.clear();
    alphaValuesPerAlphaIndex.clear();
    groupCombinationCount = 0;
    transformCombinationCount = 0;
    alphaIndicesCount = 0;
    groupCombinations.clear();
    groupCombinationIndices.clear();
    transformCombinationIndices.clear();
    alphaIndices.clear();
    groupCombinationIndicesPerGroupId.clear();
    firstGroupIndexPerTransformIndex.clear();
    storedCheckpointGroupStates.clear();
    maxGroupId = 0;
    usedGroupIds.clear();
    transformGroupIds.clear();
    alphaGroupIds.clear();

    auto emptyCombination = GroupCombination(std::vector<GroupID>{});
    addGroupCombination(emptyCombination, 0);
    GroupCombinationIndex nextCombinationIndex = 1;

    for (auto object : CCArrayExt<GameObject*>(objects)) {
        if (!object)
            continue;

        const auto targetGroupId = getTargetGroupIdOfTransformingTrigger(object);
        if (targetGroupId.has_value())
            transformGroupIds.insert(targetGroupId.value());
        else if (object->m_objectID == IOS_ALPHA_TRIGGER_ID ||
                 object->m_objectID == IOS_TOGGLE_TRIGGER_ID) {
            alphaGroupIds.insert(
                static_cast<EffectGameObject*>(object)->m_targetGroupID
            );
        }

        const GroupCombination combination(object);
        if (!groupCombinationIndices.contains(combination)) {
            addGroupCombination(combination, nextCombinationIndex);
            ++nextCombinationIndex;
        }
    }

    groupCombinationCount = nextCombinationIndex;
    alphaIndicesPerGroupCombinationIndex.resize(groupCombinationCount);

    u32 nextTransformIndex = 0;
    u32 nextAlphaIndex = 0;

    for (auto object : CCArrayExt<GameObject*>(objects)) {
        if (!object)
            continue;

        const GroupCombination combination(object);
        const auto combinationIt = groupCombinationIndices.find(combination);
        if (combinationIt == groupCombinationIndices.end())
            continue;
        const GroupCombinationIndex combinationIndex = combinationIt->second;

        GroupCombination transformCombination = combination;
        transformCombination.removeGroupIdsNotInSet(transformGroupIds);
        if (!transformCombinationIndices.contains(transformCombination)) {
            transformCombinationIndices.emplace(transformCombination, nextTransformIndex);
            firstGroupIndexPerTransformIndex.push_back(combinationIndex);
            ++nextTransformIndex;
        }

        // The alpha mask must be applied to the alpha-specific copy. The
        // upstream source currently filters the wrong local variable here;
        // keeping this correction is necessary for per-group alpha parity.
        GroupCombination alphaCombination = combination;
        alphaCombination.removeGroupIdsNotInSet(alphaGroupIds);

        if (!alphaIndices.contains(alphaCombination)) {
            alphaIndices.emplace(alphaCombination, nextAlphaIndex++);
            alphaIndexGroupIdFastStructure.push_back(
                static_cast<u32>(alphaCombination.getCount())
            );
            for (const auto groupId : alphaCombination.getSpan())
                alphaIndexGroupIdFastStructure.push_back(static_cast<u32>(groupId));
        }

        alphaIndicesPerGroupCombinationIndex[combinationIndex] =
            alphaIndices.at(alphaCombination);
    }

    transformCombinationCount = nextTransformIndex;
    alphaIndicesCount = nextAlphaIndex;

    alphaValuesPerGroupId.assign(static_cast<usize>(maxGroupId) + 1, 1.f);
    alphaValuesPerAlphaIndex.assign(alphaIndicesCount, 1.f);
    groupStates.assign(groupCombinationCount, GroupCombinationState{});
    isGroupToggledOn.assign(static_cast<usize>(maxGroupId) + 1, true);

    fetchVanillaRotateGroups();
    updateGroupStateBuffer();
    return true;
}

GroupCombinationIndex GroupManager::getGroupCombinationIndexForObject(
    GameObject* object
) const {
    const GroupCombination combination(object);
    const auto it = groupCombinationIndices.find(combination);
    return it == groupCombinationIndices.end() ? 0 : it->second;
}

GroupCombinationIndex GroupManager::getTransformCombinationIndexForObject(
    GameObject* object
) const {
    GroupCombination combination(object);
    combination.removeGroupIdsNotInSet(transformGroupIds);
    const auto it = transformCombinationIndices.find(combination);
    return it == transformCombinationIndices.end() ? 0 : it->second;
}

const std::set<GameObject*>& GroupManager::getVanillaRotateGroup(GroupID id) const {
    static const std::set<GameObject*> empty;
    if (id < 0 || static_cast<usize>(id) >= vanillaRotateGroups.size())
        return empty;
    return vanillaRotateGroups[static_cast<usize>(id)];
}

void GroupManager::fetchVanillaRotateGroups() {
    vanillaRotateGroups.clear();
    if (!layer)
        return;

    vanillaRotateGroups.resize(
        std::max(layer->m_staticGroups.size(), layer->m_optimizedGroups.size())
    );

    for (GroupID id = 1; id < static_cast<GroupID>(layer->m_staticGroups.size()); ++id) {
        CCArray* group = layer->m_staticGroups[static_cast<usize>(id)];
        if (!group)
            continue;

        auto& destination = vanillaRotateGroups[static_cast<usize>(id)];
        for (auto object : CCArrayExt<GameObject*, false>(group)) {
            if (object && object->m_objectType != GameObjectType::Decoration)
                destination.insert(object);
        }
    }

    for (GroupID id = 1; id < static_cast<GroupID>(vanillaRotateGroups.size()); ++id) {
        GameObject* mainObject = layer->tryGetMainObject(id);
        if (!mainObject || !mainObject->m_groups)
            continue;

        for (usize i = 0; i < mainObject->m_groupCount; ++i) {
            const auto targetId = mainObject->m_groups->at(i);
            if (targetId >= 0 && static_cast<usize>(targetId) < vanillaRotateGroups.size())
                vanillaRotateGroups[static_cast<usize>(targetId)].insert(mainObject);
        }
    }
}

void GroupManager::updateGroupStateBuffer() {
    updateAlphaValues();
    updatePulseEntries();
}

void GroupManager::updateAlphaValues() {
    if (!layer || !layer->m_effectManager)
        return;

    for (GroupID id = 1; id <= maxGroupId; ++id) {
        const usize index = static_cast<usize>(id);
        alphaValuesPerGroupId[index] = isGroupToggledOn[index]
            ? layer->m_effectManager->opacityModForGroup(id)
            : 0.f;
    }

    usize fastIndex = 0;
    for (u32 alphaIndex = 0; alphaIndex < alphaIndicesCount; ++alphaIndex) {
        if (fastIndex >= alphaIndexGroupIdFastStructure.size())
            break;

        const u32 groupCount = alphaIndexGroupIdFastStructure[fastIndex++];
        float alpha = 1.f;
        for (u32 i = 0; i < groupCount && fastIndex < alphaIndexGroupIdFastStructure.size(); ++i) {
            const u32 groupId = alphaIndexGroupIdFastStructure[fastIndex++];
            if (groupId < alphaValuesPerGroupId.size())
                alpha *= alphaValuesPerGroupId[groupId];
        }
        alphaValuesPerAlphaIndex[alphaIndex] = alpha;
    }

    for (u32 combinationIndex = 0;
         combinationIndex < groupCombinationCount;
         ++combinationIndex) {
        const u32 alphaIndex =
            alphaIndicesPerGroupCombinationIndex[combinationIndex];
        groupStates[combinationIndex].opacity =
            alphaIndex < alphaValuesPerAlphaIndex.size()
                ? alphaValuesPerAlphaIndex[alphaIndex]
                : 1.f;
    }
}

void GroupManager::updatePulseEntries() {
    pulseEntries.clear();
    pulseEntries.push_back(PulseEntry::endIndicator());
    for (auto& state : groupStates)
        state.pulseIndex = 0;

    if (!layer || !layer->m_effectManager)
        return;

    auto effectManager = layer->m_effectManager;
    std::vector<GroupCombinationIndex> pulsingCombinations;

    for (GroupID groupId = 0;
         groupId < static_cast<GroupID>(effectManager->m_unkVector2f0.size());
         ++groupId) {
        if (!effectManager->m_unkVector2f0[static_cast<usize>(groupId)] ||
            static_cast<usize>(groupId) >= groupCombinationIndicesPerGroupId.size())
            continue;

        for (const auto combinationIndex :
             groupCombinationIndicesPerGroupId[static_cast<usize>(groupId)]) {
            pulsingCombinations.push_back(combinationIndex);
        }
    }

    for (const auto combinationIndex : pulsingCombinations) {
        if (combinationIndex >= groupCombinations.size())
            continue;

        const u32 pulseIndex = static_cast<u32>(pulseEntries.size());
        const auto& combination = groupCombinations[combinationIndex];

        for (const GroupID groupId : combination.getSpan()) {
            if (groupId < 0 ||
                static_cast<usize>(groupId) >= effectManager->m_unkVector2f0.size() ||
                !effectManager->m_unkVector2f0[static_cast<usize>(groupId)])
                continue;

            auto pulseIt = effectManager->m_pulseEffectMap.find(groupId);
            if (pulseIt == effectManager->m_pulseEffectMap.end())
                continue;

            for (const auto& action : pulseIt->second) {
                if (action.m_pulseEffectType != PulseEffectType::Hsv &&
                    action.m_pulseEffectType != PulseEffectType::Color)
                    continue;

                const auto encoded = makePulseEntry(action);
                PulseEntry entry;
                entry.flags = encoded.flags;
                entry.hsv = encoded.colorOrHSV;
                pulseEntries.push_back(entry);
            }
        }

        if (pulseIndex != pulseEntries.size())
            groupStates[combinationIndex].pulseIndex = pulseIndex;
        if (pulseIndex != pulseEntries.size())
            pulseEntries.push_back(PulseEntry::endIndicator());
    }
}

void GroupManager::moveGroup(GroupID groupId, float deltaX, float deltaY) {
    if (groupId < 0 ||
        static_cast<usize>(groupId) >= groupCombinationIndicesPerGroupId.size())
        return;

    for (const auto combinationIndex :
         groupCombinationIndicesPerGroupId[static_cast<usize>(groupId)]) {
        if (combinationIndex < groupStates.size())
            groupStates[combinationIndex].offset += glm::vec2(deltaX, deltaY);
    }
}

void GroupManager::rotateGroup(
    GroupID groupId,
    float angle,
    bool lockObjectRotation,
    std::optional<glm::vec2> centerPoint
) {
    if (groupId < 0 ||
        static_cast<usize>(groupId) >= groupCombinationIndicesPerGroupId.size())
        return;

    const float cosine = std::cos(glm::radians(angle));
    const float sine = std::sin(glm::radians(angle));
    // Match the owner's column-major matrix convention exactly.
    const glm::mat2 rotation(
        glm::vec2(cosine, -sine),
        glm::vec2(sine, cosine)
    );

    for (const auto combinationIndex :
         groupCombinationIndicesPerGroupId[static_cast<usize>(groupId)]) {
        if (combinationIndex >= groupStates.size())
            continue;

        auto& state = groupStates[combinationIndex];
        if (!lockObjectRotation)
            state.localTransform *= rotation;

        // Do not return here: a rotation without a center still affects every
        // matching combination's local transform.
        if (!centerPoint.has_value())
            continue;

        const auto center = centerPoint.value();
        state.positionalTransform *= rotation;
        state.offset = rotation * (state.offset - center) + center;
    }
}

void GroupManager::toggleGroup(GroupID groupId, bool enabled) {
    if (groupId < 0 || static_cast<usize>(groupId) >= isGroupToggledOn.size())
        return;
    isGroupToggledOn[static_cast<usize>(groupId)] = enabled;
}

void GroupManager::resetGroupStates() {
    for (auto& state : groupStates) {
        state.positionalTransform = glm::mat2(1.f);
        state.localTransform = glm::mat2(1.f);
        state.offset = glm::vec2(0.f);
        state.opacity = 1.f;
        state.pulseIndex = 0;
    }
    std::fill(isGroupToggledOn.begin(), isGroupToggledOn.end(), true);
    updateGroupStateBuffer();
}

void GroupManager::storeGroupStates(StoredGroupStates& storage) const {
    storage.groupStates = groupStates;
    storage.isGroupToggledOn = isGroupToggledOn;
}

void GroupManager::restoreGroupStates(const StoredGroupStates& storage) {
    const usize count = std::min(storage.groupStates.size(), groupStates.size());
    std::copy_n(storage.groupStates.begin(), count, groupStates.begin());
    if (storage.isGroupToggledOn.size() == isGroupToggledOn.size())
        isGroupToggledOn = storage.isGroupToggledOn;
}

void GroupManager::storeCheckpoint(CheckpointObject* checkpoint) {
    if (!checkpoint)
        return;
    storeGroupStates(storedCheckpointGroupStates[checkpoint]);
}

void GroupManager::restoreCheckpoint(CheckpointObject* checkpoint) {
    const auto found = storedCheckpointGroupStates.find(checkpoint);
    if (found != storedCheckpointGroupStates.end())
        restoreGroupStates(found->second);
}

void GroupManager::removeCheckpoint(CheckpointObject* checkpoint) {
    storedCheckpointGroupStates.erase(checkpoint);
}

void GroupManager::addGroupCombination(
    GroupCombination& combination,
    GroupCombinationIndex index
) {
    if (index >= groupCombinations.size())
        groupCombinations.resize(static_cast<usize>(index) + 1);
    groupCombinations[index] = combination;
    groupCombinationIndices[combination] = index;

    for (const GroupID groupId : combination.getSpan()) {
        usedGroupIds.insert(groupId);
        maxGroupId = std::max(maxGroupId, groupId);

        if (groupId < 0)
            continue;
        if (static_cast<usize>(groupId) >= groupCombinationIndicesPerGroupId.size())
            groupCombinationIndicesPerGroupId.resize(static_cast<usize>(groupId) + 1);
        groupCombinationIndicesPerGroupId[static_cast<usize>(groupId)].push_back(index);
    }
}

} // namespace IOSUpstream

#endif
