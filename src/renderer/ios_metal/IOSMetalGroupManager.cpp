#if defined(GEODE_IS_IOS)

#include "IOSMetalGroupManager.hpp"

#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/Enums.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace geode::prelude;

namespace BismuthIOSMetal {
namespace {

constexpr std::int32_t MOVE_TRIGGER_ID = 901;
constexpr std::int32_t ALPHA_TRIGGER_ID = 1007;
constexpr std::int32_t TOGGLE_TRIGGER_ID = 1049;
constexpr std::int32_t ROTATE_TRIGGER_ID = 1346;
constexpr std::int32_t FOLLOW_TRIGGER_ID = 1347;

constexpr std::uint32_t PULSE_BASE_ONLY = 1u << 26;
constexpr std::uint32_t PULSE_DETAIL_ONLY = 1u << 27;
constexpr std::uint32_t PULSE_IS_HSV = 1u << 28;
constexpr std::uint32_t PULSE_DYNAMIC_HSV = 1u << 29;
constexpr std::uint32_t PULSE_ACTIVE = 1u << 30;

struct PackedHSV {
    std::uint32_t hueAndFlags = 0;
    std::uint32_t saturationAndValue = 0;
};

PackedHSV packHSV(const cocos2d::ccHSVValue& hsv) {
    PackedHSV result;
    result.hueAndFlags = glm::packHalf2x16(glm::vec2(hsv.h, 0.f)) & 0xffffu;
    if (hsv.absoluteSaturation)
        result.hueAndFlags |= 0x20000000u;
    if (hsv.absoluteBrightness)
        result.hueAndFlags |= 0x10000000u;
    result.saturationAndValue = glm::packHalf2x16(glm::vec2(hsv.s, hsv.v));
    return result;
}

std::optional<GroupID> transformingTarget(GameObject* object) {
    if (!object)
        return std::nullopt;
    switch (object->m_objectID) {
        case MOVE_TRIGGER_ID:
        case ROTATE_TRIGGER_ID:
        case FOLLOW_TRIGGER_ID:
            return static_cast<EffectGameObject*>(object)->m_targetGroupID;
        default:
            return std::nullopt;
    }
}

GroupCombinationState makeIdentityState() {
    GroupCombinationState state;
    state.positionalTransform[0] = 1.f;
    state.positionalTransform[1] = 0.f;
    state.positionalTransform[2] = 0.f;
    state.positionalTransform[3] = 1.f;
    state.localTransform[0] = 1.f;
    state.localTransform[1] = 0.f;
    state.localTransform[2] = 0.f;
    state.localTransform[3] = 1.f;
    state.offset[0] = 0.f;
    state.offset[1] = 0.f;
    state.opacity = 1.f;
    state.pulseIndex = 0;
    return state;
}

glm::mat2 loadMatrix(const float values[4]) {
    return glm::mat2(
        glm::vec2(values[0], values[1]),
        glm::vec2(values[2], values[3])
    );
}

void storeMatrix(float values[4], const glm::mat2& matrix) {
    values[0] = matrix[0].x;
    values[1] = matrix[0].y;
    values[2] = matrix[1].x;
    values[3] = matrix[1].y;
}

PulseEntry encodePulseAction(const PulseEffectAction& action) {
    PulseEntry entry;
    entry.flags = PULSE_ACTIVE |
        (static_cast<std::uint32_t>(action.m_currentValue * 65535.f) & 0xffffu);

    if (action.m_mainOnly)
        entry.flags |= PULSE_BASE_ONLY;
    if (action.m_detailOnly)
        entry.flags |= PULSE_DETAIL_ONLY;
    if (action.m_isDynamicHsv)
        entry.flags |= PULSE_DYNAMIC_HSV;

    if (action.m_pulseEffectType == PulseEffectType::Hsv) {
        const PackedHSV hsv = packHSV(action.m_hsv);
        entry.flags |= PULSE_IS_HSV;
        entry.flags |= (static_cast<std::uint32_t>(action.m_colorIndex) & 0x3ffu) << 16;
        entry.colorOrHSV = hsv.hueAndFlags;
        entry.hsvSaturationAndValue = hsv.saturationAndValue;
    } else {
        entry.colorOrHSV =
            static_cast<std::uint32_t>(action.m_color.r) |
            (static_cast<std::uint32_t>(action.m_color.g) << 8) |
            (static_cast<std::uint32_t>(action.m_color.b) << 16) |
            (255u << 24);
    }
    return entry;
}

} // namespace

GroupCombination::GroupCombination(GameObject* object) {
    if (!object || !object->m_groups) {
        count = 0;
        ids.fill(0);
        return;
    }

    count = std::clamp<std::int32_t>(
        static_cast<std::int32_t>(object->m_groupCount), 0,
        static_cast<std::int32_t>(ids.size())
    );
    for (std::int32_t i = 0; i < count; ++i)
        ids[static_cast<std::size_t>(i)] = object->m_groups->at(static_cast<std::size_t>(i));
    std::fill(ids.begin() + count, ids.end(), 0);
    std::sort(ids.begin(), ids.begin() + count);
}

GroupCombination::GroupCombination(const std::vector<GroupID>& source) {
    count = std::min<std::int32_t>(
        static_cast<std::int32_t>(source.size()),
        static_cast<std::int32_t>(ids.size())
    );
    std::copy_n(source.begin(), count, ids.begin());
    std::fill(ids.begin() + count, ids.end(), 0);
    std::sort(ids.begin(), ids.begin() + count);
}

void GroupCombination::removeGroupIdsNotInSet(const std::set<GroupID>& mask) {
    std::int32_t writeIndex = 0;
    for (std::int32_t readIndex = 0; readIndex < count; ++readIndex) {
        const GroupID id = ids[static_cast<std::size_t>(readIndex)];
        if (id != 0 && mask.contains(id))
            ids[static_cast<std::size_t>(writeIndex++)] = id;
    }
    count = writeIndex;
    std::fill(ids.begin() + count, ids.end(), 0);
}

std::span<const GroupID> GroupCombination::getSpan() const {
    return std::span<const GroupID>(ids.data(), static_cast<std::size_t>(count));
}

bool GroupCombination::operator<(const GroupCombination& other) const {
    if (count != other.count)
        return count < other.count;
    for (std::int32_t i = 0; i < count; ++i) {
        const GroupID left = ids[static_cast<std::size_t>(i)];
        const GroupID right = other.ids[static_cast<std::size_t>(i)];
        if (left != right)
            return left < right;
    }
    return false;
}

void GroupManager::reset() {
    layer = nullptr;
    states.clear();
    pulseEntries.clear();
    toggled.clear();
    alphaIndexPerCombination.clear();
    alphaIndexGroupIdFast.clear();
    alphaPerGroup.clear();
    alphaPerCombination.clear();
    checkpoints.clear();
    groupCombinationCount = 0;
    combinations.clear();
    combinationIndices.clear();
    combinationsPerGroup.clear();
    transformGroupIds.clear();
    alphaGroupIds.clear();
    maxGroupId = 0;
    alphaCombinationCount = 0;
}

void GroupManager::addGroupCombination(
    const GroupCombination& combination,
    std::uint32_t index
) {
    if (index >= combinations.size())
        combinations.resize(static_cast<std::size_t>(index) + 1u);
    combinations[index] = combination;
    combinationIndices[combination] = index;

    for (GroupID groupId : combination.getSpan()) {
        if (groupId <= 0)
            continue;
        maxGroupId = std::max(maxGroupId, groupId);
        if (static_cast<std::size_t>(groupId) >= combinationsPerGroup.size())
            combinationsPerGroup.resize(static_cast<std::size_t>(groupId) + 1u);
        combinationsPerGroup[static_cast<std::size_t>(groupId)].push_back(index);
    }
}

bool GroupManager::initialize(PlayLayer* playLayer, cocos2d::CCArray* objects) {
    reset();
    if (!playLayer || !objects)
        return false;

    layer = playLayer;
    addGroupCombination(GroupCombination{}, 0);
    std::uint32_t nextCombination = 1;

    for (auto object : CCArrayExt<GameObject*>(objects)) {
        if (!object)
            continue;

        const auto target = transformingTarget(object);
        if (target.has_value() && target.value() > 0)
            transformGroupIds.insert(target.value());
        else if (object->m_objectID == ALPHA_TRIGGER_ID ||
                 object->m_objectID == TOGGLE_TRIGGER_ID) {
            const auto groupId =
                static_cast<EffectGameObject*>(object)->m_targetGroupID;
            if (groupId > 0)
                alphaGroupIds.insert(groupId);
        }

        GroupCombination combination(object);
        if (!combinationIndices.contains(combination)) {
            addGroupCombination(combination, nextCombination);
            ++nextCombination;
        }
    }

    groupCombinationCount = nextCombination;
    states.resize(groupCombinationCount);
    for (auto& state : states)
        state = makeIdentityState();

    alphaIndexPerCombination.resize(groupCombinationCount);

    std::map<GroupCombination, std::uint32_t> alphaIndices;
    std::uint32_t nextAlphaIndex = 0;
    for (std::uint32_t index = 0; index < groupCombinationCount; ++index) {
        GroupCombination alphaCombination = combinations[index];
        alphaCombination.removeGroupIdsNotInSet(alphaGroupIds);

        auto found = alphaIndices.find(alphaCombination);
        if (found == alphaIndices.end()) {
            const std::uint32_t newIndex = nextAlphaIndex++;
            found = alphaIndices.emplace(alphaCombination, newIndex).first;

            alphaIndexGroupIdFast.push_back(
                static_cast<std::uint32_t>(alphaCombination.getCount())
            );
            for (GroupID groupId : alphaCombination.getSpan())
                alphaIndexGroupIdFast.push_back(static_cast<std::uint32_t>(groupId));
        }
        alphaIndexPerCombination[index] = found->second;
    }

    alphaCombinationCount = nextAlphaIndex;
    alphaPerGroup.resize(static_cast<std::size_t>(maxGroupId) + 1u, 1.f);
    alphaPerCombination.resize(alphaCombinationCount, 1.f);
    toggled.resize(static_cast<std::size_t>(maxGroupId) + 1u, true);

    update();
    return true;
}

std::uint32_t GroupManager::getGroupCombinationIndexForObject(GameObject* object) const {
    const auto found = combinationIndices.find(GroupCombination(object));
    return found == combinationIndices.end() ? 0u : found->second;
}

void GroupManager::update() {
    updateAlphaValues();
    updatePulseEntries();
}

void GroupManager::updateAlphaValues() {
    if (!layer || !layer->m_effectManager)
        return;

    auto effectManager = layer->m_effectManager;
    for (GroupID id = 1; id <= maxGroupId; ++id) {
        const auto index = static_cast<std::size_t>(id);
        alphaPerGroup[index] = toggled[index]
            ? effectManager->opacityModForGroup(id)
            : 0.f;
    }

    std::size_t cursor = 0;
    for (std::uint32_t alphaIndex = 0;
         alphaIndex < alphaCombinationCount;
         ++alphaIndex) {
        if (cursor >= alphaIndexGroupIdFast.size())
            break;

        const std::uint32_t groupCount = alphaIndexGroupIdFast[cursor++];
        float alpha = 1.f;
        for (std::uint32_t i = 0;
             i < groupCount && cursor < alphaIndexGroupIdFast.size();
             ++i) {
            const std::uint32_t groupId = alphaIndexGroupIdFast[cursor++];
            if (groupId < alphaPerGroup.size())
                alpha *= alphaPerGroup[groupId];
        }
        alphaPerCombination[alphaIndex] = alpha;
    }

    for (std::uint32_t index = 0; index < groupCombinationCount; ++index) {
        const std::uint32_t alphaIndex = alphaIndexPerCombination[index];
        states[index].opacity = alphaIndex < alphaPerCombination.size()
            ? alphaPerCombination[alphaIndex] : 1.f;
    }
}

void GroupManager::updatePulseEntries() {
    pulseEntries.clear();
    pulseEntries.push_back(PulseEntry{});
    for (auto& state : states)
        state.pulseIndex = 0;

    if (!layer || !layer->m_effectManager)
        return;

    auto effectManager = layer->m_effectManager;
    std::vector<std::uint32_t> pulsingCombinations;
    const auto& pulsingGroups = effectManager->m_unkVector2f0;

    for (GroupID groupId = 0;
         groupId < static_cast<GroupID>(pulsingGroups.size());
         ++groupId) {
        const auto groupIndex = static_cast<std::size_t>(groupId);
        if (!pulsingGroups[groupIndex] ||
            groupIndex >= combinationsPerGroup.size())
            continue;

        const auto& groupCombinations = combinationsPerGroup[groupIndex];
        pulsingCombinations.insert(
            pulsingCombinations.end(),
            groupCombinations.begin(),
            groupCombinations.end()
        );
    }

    for (const std::uint32_t combinationIndex : pulsingCombinations) {
        if (combinationIndex >= combinations.size())
            continue;

        const std::uint32_t pulseIndex =
            static_cast<std::uint32_t>(pulseEntries.size());
        const auto& combination = combinations[combinationIndex];

        for (GroupID groupId : combination.getSpan()) {
            if (groupId < 0 ||
                static_cast<std::size_t>(groupId) >= pulsingGroups.size() ||
                !pulsingGroups[static_cast<std::size_t>(groupId)])
                continue;

            const auto pulseIt = effectManager->m_pulseEffectMap.find(groupId);
            if (pulseIt == effectManager->m_pulseEffectMap.end())
                continue;

            for (const auto& action : pulseIt->second) {
                if (action.m_pulseEffectType != PulseEffectType::Hsv &&
                    action.m_pulseEffectType != PulseEffectType::Color)
                    continue;
                pulseEntries.push_back(encodePulseAction(action));
            }
        }

        if (pulseIndex == pulseEntries.size())
            continue;

        pulseEntries.push_back(PulseEntry{});
        states[combinationIndex].pulseIndex = pulseIndex;
    }
}

void GroupManager::moveGroup(GroupID groupId, float deltaX, float deltaY) {
    if (groupId < 0 ||
        static_cast<std::size_t>(groupId) >= combinationsPerGroup.size())
        return;

    for (const std::uint32_t index : combinationsPerGroup[static_cast<std::size_t>(groupId)]) {
        if (index >= states.size())
            continue;
        states[index].offset[0] += deltaX;
        states[index].offset[1] += deltaY;
    }
}

void GroupManager::rotateGroup(
    GroupID groupId,
    float angle,
    bool lockObjectRotation,
    std::optional<glm::vec2> centerPoint
) {
    if (groupId < 0 ||
        static_cast<std::size_t>(groupId) >= combinationsPerGroup.size())
        return;

    const float cosine = std::cos(glm::radians(angle));
    const float sine = std::sin(glm::radians(angle));
    const glm::mat2 rotation(
        glm::vec2(cosine, -sine),
        glm::vec2(sine, cosine)
    );

    for (const std::uint32_t index : combinationsPerGroup[static_cast<std::size_t>(groupId)]) {
        if (index >= states.size())
            continue;

        auto& state = states[index];
        if (!lockObjectRotation) {
            glm::mat2 local = loadMatrix(state.localTransform);
            local *= rotation;
            storeMatrix(state.localTransform, local);
        }

        if (!centerPoint.has_value())
            continue;

        glm::mat2 positional = loadMatrix(state.positionalTransform);
        positional *= rotation;
        storeMatrix(state.positionalTransform, positional);

        const glm::vec2 offset(state.offset[0], state.offset[1]);
        const glm::vec2 moved = rotation * (offset - centerPoint.value()) + centerPoint.value();
        state.offset[0] = moved.x;
        state.offset[1] = moved.y;
    }
}

void GroupManager::toggleGroup(GroupID groupId, bool enabled) {
    if (groupId < 0 || static_cast<std::size_t>(groupId) >= toggled.size())
        return;
    toggled[static_cast<std::size_t>(groupId)] = enabled;
}

void GroupManager::storeCheckpoint(CheckpointObject* checkpoint) {
    if (!checkpoint)
        return;
    checkpoints[checkpoint] = {states, toggled};
}

void GroupManager::restoreCheckpoint(CheckpointObject* checkpoint) {
    if (!checkpoint)
        return;
    const auto found = checkpoints.find(checkpoint);
    if (found == checkpoints.end())
        return;

    const auto& stored = found->second;
    if (stored.states.size() == states.size())
        states = stored.states;
    if (stored.toggled.size() == toggled.size())
        toggled = stored.toggled;
}

void GroupManager::removeCheckpoint(CheckpointObject* checkpoint) {
    checkpoints.erase(checkpoint);
}

} // namespace BismuthIOSMetal

#endif
