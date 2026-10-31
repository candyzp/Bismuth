#pragma once

#ifdef GEODE_IS_IOS

#include "IOSRenderRecords.hpp"
#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace IOSUpstream {

using GroupID = i16;
using GroupCombinationIndex = u32;

class GroupCombination {
public:
    GroupCombination(std::array<GroupID, IOS_GROUP_IDS_PER_OBJECT>* groupIds, i32 count);
    explicit GroupCombination(std::vector<GroupID> groupIds);
    GroupCombination() = default;
    explicit GroupCombination(GameObject* object);

    void removeGroupIdsNotInSet(const std::set<GroupID>& maskSet);
    bool operator<(const GroupCombination& other) const;

    std::span<GroupID> getSpan();
    std::span<const GroupID> getSpan() const;
    i32 getCount() const { return count; }
    operator std::string() const;

private:
    i32 count = 0;
    std::array<GroupID, IOS_GROUP_IDS_PER_OBJECT> ids {};
};

struct StoredGroupStates {
    std::vector<GroupCombinationState> groupStates;
    std::vector<bool> isGroupToggledOn;
};

class GroupManager {
public:
    explicit GroupManager(PlayLayer* layer = nullptr) : layer(layer) {}

    bool initWithObjects(PlayLayer* playLayer, cocos2d::CCArray* objects);

    GroupCombinationIndex getGroupCombinationIndexForObject(GameObject* object) const;
    GroupCombinationIndex getTransformCombinationIndexForObject(GameObject* object) const;

    u32 getGroupCombinationCount() const { return groupCombinationCount; }
    u32 getTransformCombinationCount() const { return transformCombinationCount; }
    GroupID getMaxGroupId() const { return maxGroupId; }

    std::span<GroupCombinationState> getGroupStates() {
        return { groupStates.data(), groupStates.size() };
    }
    std::span<const GroupCombinationState> getGroupStates() const {
        return { groupStates.data(), groupStates.size() };
    }
    std::span<const PulseEntry> getPulseEntries() const {
        return { pulseEntries.data(), pulseEntries.size() };
    }

    const std::set<GameObject*>& getVanillaRotateGroup(GroupID id) const;

    void updateGroupStateBuffer();
    void moveGroup(GroupID groupId, float deltaX, float deltaY);
    void rotateGroup(
        GroupID groupId,
        float angle,
        bool lockObjectRotation,
        std::optional<glm::vec2> centerPoint = std::nullopt
    );
    void toggleGroup(GroupID groupId, bool enabled);
    void resetGroupStates();

    void storeGroupStates(StoredGroupStates& storage) const;
    void restoreGroupStates(const StoredGroupStates& storage);
    void storeCheckpoint(CheckpointObject* checkpoint);
    void restoreCheckpoint(CheckpointObject* checkpoint);
    void removeCheckpoint(CheckpointObject* checkpoint);

private:
    void addGroupCombination(const GroupCombination& combination, GroupCombinationIndex index);
    void fetchVanillaRotateGroups();
    void updateAlphaValues();
    void updatePulseEntries();

private:
    PlayLayer* layer = nullptr;

    std::vector<GroupCombinationState> groupStates;
    std::vector<PulseEntry> pulseEntries;
    std::vector<bool> isGroupToggledOn;

    std::vector<u32> alphaIndicesPerGroupCombinationIndex;
    std::vector<u32> alphaIndexGroupIdFastStructure;
    std::vector<float> alphaValuesPerGroupId;
    std::vector<float> alphaValuesPerAlphaIndex;

    std::unordered_map<CheckpointObject*, StoredGroupStates> storedCheckpointGroupStates;

    u32 groupCombinationCount = 0;
    std::vector<GroupCombination> groupCombinations;
    std::map<GroupCombination, GroupCombinationIndex> groupCombinationIndices;

    u32 transformCombinationCount = 0;
    std::map<GroupCombination, u32> transformCombinationIndices;

    u32 alphaIndicesCount = 0;
    std::map<GroupCombination, u32> alphaIndices;

    std::vector<GroupCombinationIndex> firstGroupIndexPerTransformIndex;
    std::vector<std::vector<GroupCombinationIndex>> groupCombinationIndicesPerGroupId;

    GroupID maxGroupId = 0;
    std::set<GroupID> usedGroupIds;
    std::set<GroupID> transformGroupIds;
    std::set<GroupID> alphaGroupIds;

    std::vector<std::set<GameObject*>> vanillaRotateGroups;
};

} // namespace IOSUpstream

#endif
