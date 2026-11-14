#pragma once

#if defined(GEODE_IS_IOS)

#include "IOSMetalCompute.hpp"
#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <glm/glm.hpp>
#include <array>
#include <span>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace BismuthIOSMetal {

using GroupID = std::int16_t;

class GroupCombination {
public:
    GroupCombination() = default;
    explicit GroupCombination(GameObject* object);
    explicit GroupCombination(const std::vector<GroupID>& ids);

    void removeGroupIdsNotInSet(const std::set<GroupID>& mask);
    std::span<const GroupID> getSpan() const;
    std::int32_t getCount() const { return count; }
    bool operator<(const GroupCombination& other) const;

private:
    std::int32_t count = 0;
    std::array<GroupID, 10> ids {};
};

struct StoredGroupStates {
    std::vector<GroupCombinationState> states;
    std::vector<bool> toggled;
};

class GroupManager {
public:
    GroupManager() = default;

    bool initialize(PlayLayer* layer, cocos2d::CCArray* objects);
    void reset();

    std::uint32_t getGroupCombinationIndexForObject(GameObject* object) const;
    std::span<const GroupCombinationState> getStates() const { return states; }
    std::span<const PulseEntry> getPulseEntries() const { return pulseEntries; }

    // Recomputes only alpha-combination state and pulse-action records.
    // Work scales with unique group combinations, never sprite count.
    void update();

    void moveGroup(GroupID groupId, float deltaX, float deltaY);
    void rotateGroup(
        GroupID groupId,
        float angle,
        bool lockObjectRotation,
        std::optional<glm::vec2> centerPoint = std::nullopt
    );
    void toggleGroup(GroupID groupId, bool enabled);

    void storeCheckpoint(CheckpointObject* checkpoint);
    void restoreCheckpoint(CheckpointObject* checkpoint);
    void removeCheckpoint(CheckpointObject* checkpoint);

private:
    void addGroupCombination(const GroupCombination& combination, std::uint32_t index);
    void updateAlphaValues();
    void updatePulseEntries();

private:
    PlayLayer* layer = nullptr;

    std::vector<GroupCombinationState> states;
    std::vector<PulseEntry> pulseEntries;
    std::vector<bool> toggled;

    std::vector<std::uint32_t> alphaIndexPerCombination;
    std::vector<std::uint32_t> alphaIndexGroupIdFast;
    std::vector<float> alphaPerGroup;
    std::vector<float> alphaPerCombination;

    std::unordered_map<CheckpointObject*, StoredGroupStates> checkpoints;

    std::uint32_t groupCombinationCount = 0;
    std::vector<GroupCombination> combinations;
    std::map<GroupCombination, std::uint32_t> combinationIndices;
    std::vector<std::vector<std::uint32_t>> combinationsPerGroup;

    std::set<GroupID> transformGroupIds;
    std::set<GroupID> alphaGroupIds;
    GroupID maxGroupId = 0;
    std::uint32_t alphaCombinationCount = 0;
};

} // namespace BismuthIOSMetal

#endif
