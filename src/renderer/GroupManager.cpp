#include "GroupManager.hpp"
#include "Renderer.hpp"
#include "glm/ext/matrix_transform.hpp"
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <optional>
#include <string>

using namespace geode::prelude;

#define MOVE_TRIGGER_ID   901
#define ROTATE_TRIGGER_ID 1346
#define FOLLOW_TRIGGER_ID 1347

static std::optional<GroupID> getTargetGroupIdOfTransformingTrigger(GameObject* object) {
    switch (object->m_objectID) {
    case MOVE_TRIGGER_ID:
    case ROTATE_TRIGGER_ID:
    case FOLLOW_TRIGGER_ID:
        return ((EffectGameObject*)object)->m_targetGroupID;
    }
    return std::nullopt;
}

GroupCombination::GroupCombination(std::array<GroupID, 10>* groupIds, i32 count) {
    i32 i = 0;
    for (; i < count; i++)
        ids[i] = groupIds->at(i);
    for (; i < 10; i++)
        ids[i] = 0;
    this->count = count;
    if (count < 2)
        return;
    std::sort(ids.data(), &ids[count]);
}

GroupCombination::GroupCombination(std::vector<GroupID> groupIds) {
    count = groupIds.size();
    std::copy(groupIds.begin(), groupIds.end(), ids.begin());
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

GroupManager::~GroupManager() {
    if (groupStateBuffer)
        Buffer::destroy(groupStateBuffer);
}

static std::set<GameObject*> dobjects;

void GroupManager::initWithObjects(cocos2d::CCArray* objects) {
    GroupCombinationIndex groupCombIndex = 0;

    for (auto object : CCArrayExt<GameObject*>(objects)) {
        std::optional<GroupID> targetGroupId = getTargetGroupIdOfTransformingTrigger(object);
        if (targetGroupId.has_value())
            transformGroupIds.insert(targetGroupId.value());

        dobjects.insert(object);
        
        GroupCombination comb = GroupCombination(object);

        if (groupCombinationIndicies.find(comb) == groupCombinationIndicies.end()) {
            addGroupCombination(comb, groupCombIndex);
            groupCombIndex++;
        }
    }

    groupCombinationCount = groupCombIndex;

    GroupCombinationIndex transformCombIndex = 0;

    for (auto object : CCArrayExt<GameObject*>(objects)) {
        GroupCombination comb = GroupCombination(object);
        GroupCombination rawComb = comb;

        comb.removeGroupIdsNotInSet(transformGroupIds);

        if (transformCombinationIndicies.find(comb) == transformCombinationIndicies.end()) {
            transformCombinationIndicies[comb] = transformCombIndex;
            std::string s = "";
            for (i32 a : rawComb.getSpan()) {
                if (s != "") s += ", ";
                s += std::to_string(a);
            }
            // log::info("Transform Id {} = ({})", transformCombIndex, s);
            firstGroupIndexPerTransformIndex.push_back(groupCombinationIndicies[rawComb]);
            transformCombIndex++;
        }
    }

    transformCombinationCount = transformCombIndex;
    log::info("Number of unique group transformations: {}", transformCombinationCount);

    groupStates.resize(groupCombinationCount);
    groupStateBuffer = Buffer::createDynamicDraw("Group state buffer", getGroupStateBufferSize());
}
    
GroupCombinationIndex GroupManager::getGroupCombinationIndexForObject(GameObject* object) {
    auto comb = GroupCombination(object);

    auto it = groupCombinationIndicies.find(comb);
    assert(it != groupCombinationIndicies.end());

    return it->second;
}

GroupCombinationIndex GroupManager::getTransformCombinationIndexForObject(GameObject* object) {
    auto comb = GroupCombination(object);
    comb.removeGroupIdsNotInSet(transformGroupIds);

    auto it = transformCombinationIndicies.find(comb);
    assert(it != transformCombinationIndicies.end());

    return it->second;
}

void GroupManager::prepareGroupStateBuffer() {
    for (u32 i = 0; i < getGroupCombinationCount(); i++)
        groupStates[i].opacity = 1.f;

    for (auto groupId : usedGroupIds) {
        auto it = groupCombinationIndiciesPerGroupId.find(groupId);
        if (it == groupCombinationIndiciesPerGroupId.end())
            continue;

        bool isDisabled = disabledGroups.contains(groupId);

        for (auto combIndex : it->second) {
            if (isDisabled)
                groupStates[combIndex].opacity = 0.0;
            else
                groupStates[combIndex].opacity *= renderer.getPlayLayer()->m_effectManager->opacityModForGroup(groupId);
        }
    }

    groupStateBuffer->write(groupStates.data(), getGroupStateBufferSize());
}

void GroupManager::moveGroup(GroupID groupId, float deltaX, float deltaY) {
    auto it = groupCombinationIndiciesPerGroupId.find(groupId);
    if (it == groupCombinationIndiciesPerGroupId.end())
        return;

    for (auto combIndex : it->second)
        groupStates[combIndex].offset += glm::vec2(deltaX, deltaY);
}

void GroupManager::rotateGroup(
    GroupID groupId,
    float angle,
    bool lockObjectRotation,
    std::optional<glm::vec2> centerPoint
) {
    auto it = groupCombinationIndiciesPerGroupId.find(groupId);
    if (it == groupCombinationIndiciesPerGroupId.end())
        return;
    
    float cos = cosf(glm::radians(angle) * 0.5);
    float sin = sinf(glm::radians(angle) * 0.5);
    glm::mat2 matrix = {
        { cos, -sin },
        { sin,  cos }
    };

    for (auto combIndex : it->second) {
        auto& groupState = groupStates[combIndex];

        if (!lockObjectRotation)
            groupState.localTransform *= matrix;

        if (!centerPoint.has_value())
            return;

        auto center = centerPoint.value();
        groupState.positionalTransform *= matrix;
        groupState.offset = matrix * (groupState.offset - center) + center;
    }
}

void GroupManager::toggleGroup(GroupID groupId, bool visible) {
    if (visible)
        disabledGroups.erase(groupId);
    else
        disabledGroups.insert(groupId);
}

void GroupManager::resetGroupStates() {
    for (auto& groupState : groupStates) {
        groupState.positionalTransform = glm::identity<glm::mat2>();
        groupState.localTransform = glm::identity<glm::mat2>();
        groupState.offset = glm::vec2(0, 0);
    }
    disabledGroups.clear();
}

void GroupManager::addGroupCombination(GroupCombination& comb, GroupCombinationIndex index) {
    groupCombinationIndicies[comb] = index;

    for (auto groupId : comb.getSpan()) {
        usedGroupIds.insert(groupId);

        if (groupId > maxGroupId)
            maxGroupId = groupId;

        auto it = groupCombinationIndiciesPerGroupId.find(groupId);
        if (it != groupCombinationIndiciesPerGroupId.end()) {
            it->second.push_back(index);
        } else {
            groupCombinationIndiciesPerGroupId[groupId] = { index };
        }
    }
}