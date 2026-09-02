#include "GroupManager.hpp"
#include "Renderer.hpp"
#include "glm/ext/matrix_transform.hpp"
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <optional>
#include <string>

using namespace geode::prelude;

#define MOVE_TRIGGER_ID   901
#define ALPHA_TRIGGER_ID  1007
#define TOGGLE_TRIGGER_ID 1049
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
    if (groupStateBuffer) {
        groupStateBuffer->unmap();
        Buffer::destroy(groupStateBuffer);
    }
}

void GroupManager::initWithObjects(cocos2d::CCArray* objects) {
    GroupCombinationIndex groupCombIndex = 0;

    for (auto object : CCArrayExt<GameObject*>(objects)) {
        std::optional<GroupID> targetGroupId = getTargetGroupIdOfTransformingTrigger(object);

        if (targetGroupId.has_value())
            transformGroupIds.insert(targetGroupId.value());
        else if (object->m_objectID == ALPHA_TRIGGER_ID || object->m_objectID == TOGGLE_TRIGGER_ID)
            alphaGroupIds.insert(((EffectGameObject*)object)->m_targetGroupID);

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
    haveGroupStatesScaled.resize(groupCombinationCount);

    for (auto object : CCArrayExt<GameObject*>(objects)) {
        GroupCombination comb = GroupCombination(object);
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

    transformCombinationCount = transformCombIndex;
    log::info("Number of unique group transformations: {}", transformCombinationCount);

    alphaValuesPerGroupId.resize((usize)maxGroupId + 1);
    alphaValuesPerAlphaIndex.resize(alphaIndiciesCount);

    groupStateBuffer = Buffer::createDynamicDraw("Group state buffer", getGroupStateBufferSize());
    groupStates = (GroupCombinationState*)groupStateBuffer->mapReadWrite();
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
    for (u32 id = 1; id <= maxGroupId; id++) {
        if (!disabledGroups.contains(id))
            alphaValuesPerGroupId[id] = renderer.getPlayLayer()->m_effectManager->opacityModForGroup(id);
        else
            alphaValuesPerGroupId[id] = 0.0;
    }

    u32* alphaFast = alphaIndexGroupIdFastStructure.data();
    for (u32 alphaIndex = 0; alphaIndex < alphaIndiciesCount; alphaIndex++) {
        u32 combCount = *(alphaFast++);
        float alpha = 1.0;
        while (combCount > 0) {
            alpha *= alphaValuesPerGroupId[*(alphaFast++)];
            combCount--;
        }
        alphaValuesPerAlphaIndex[alphaIndex] = alpha;
    }

    for (u32 combIndex = 0; combIndex < groupCombinationCount; combIndex++)
        groupStates[combIndex].opacity = alphaValuesPerAlphaIndex[alphaIndiciesPerGroupCombinationIndex[combIndex]];
}

void GroupManager::moveGroup(GroupID groupId, float deltaX, float deltaY) {
    auto combIndicies = groupCombinationIndiciesPerGroupId[groupId];
    for (auto combIndex : combIndicies)
        groupStates[combIndex].offset += glm::vec2(deltaX, deltaY);
}

void GroupManager::rotateGroup(
    GroupID groupId,
    float angle,
    bool lockObjectRotation,
    std::optional<glm::vec2> centerPoint
) {
    float cos = cosf(glm::radians(angle) * 0.5);
    float sin = sinf(glm::radians(angle) * 0.5);
    glm::mat2 matrix = {
        { cos, -sin },
        { sin,  cos }
    };

    auto combIndicies = groupCombinationIndiciesPerGroupId[groupId];
    for (auto combIndex : combIndicies) {
        auto& groupState = groupStates[combIndex];

        if (!lockObjectRotation)
            groupState.localTransform *= matrix;

        // A rotate trigger without a center still applies the local rotation to
        // every matching group combination. Do not exit after the first one.
        if (!centerPoint.has_value())
            continue;

        auto center = centerPoint.value();
        groupState.positionalTransform *= matrix;
        groupState.offset = matrix * (groupState.offset - center) + center;
        
        haveGroupStatesScaled[combIndex] = true;
    }
}

void GroupManager::toggleGroup(GroupID groupId, bool visible) {
    if (visible)
        disabledGroups.erase(groupId);
    else
        disabledGroups.insert(groupId);
}

void GroupManager::resetGroupStates() {
    for (auto& groupState : getGroupStates()) {
        groupState.positionalTransform = glm::identity<glm::mat2>();
        groupState.localTransform = glm::identity<glm::mat2>();
        groupState.offset = glm::vec2(0, 0);
    }
    disabledGroups.clear();
    std::fill(haveGroupStatesScaled.begin(), haveGroupStatesScaled.end(), false);
}

void GroupManager::addGroupCombination(GroupCombination& comb, GroupCombinationIndex index) {
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
