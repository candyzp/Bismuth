#include "GJBaseGameLayer.hpp"

#ifdef GEODE_IS_IOS
#include "../renderer/ios_upstream/IOSGroupManager.hpp"
#include <Geode/binding/EffectGameObject.hpp>
#endif

using namespace geode::prelude;

void decomp_GJBaseGameLayer::moveObjects(cocos2d::CCArray* objects, double moveX, double moveY, bool unused) {
    if (objects == nullptr)
        return;

    for (auto object : CCArrayExt<GameObject*>(objects)) {
        if (!object->m_isDecoration2 && object->m_unk4C4 != m_gameState.m_commandIndex) {
            object->m_unk4C4 = m_gameState.m_commandIndex;

            object->m_lastPosition.x = object->m_positionX;
            object->m_lastPosition.y = object->m_positionY;
            object->dirtifyObjectRect();
        }

        if (moveX != 0.0 && !object->m_tempOffsetXRelated)
            object->m_positionX += moveX;
        if (moveY != 0.0)
            object->m_positionY += moveY;
            
        object->dirtifyObjectPos();
        updateObjectSection(object);
    }

    updateDisabledObjectsLastPos(objects);
}

void decomp_GJBaseGameLayer::processMoveActions() {
    m_effectManager->processMoveCalculations();
    for (auto node : m_effectManager->m_unkVector6c0) {
        if (node->m_unk0d0)
            continue;

        int groupId = node->getTag();
        CCArray* objects = getStaticGroup(groupId);
        if (objects)
            moveObjects(objects, node->m_unk038, node->m_unk040, node->m_uIndexInArray == 13); // index in array thing is unused

        objects = getOptimizedGroup(groupId);
        if (objects)
            moveObjects(objects, node->m_unk090, node->m_unk098, node->m_uIndexInArray == 13); // index in array thing is unused
    }
}

#ifdef GEODE_IS_IOS

// Port of the owner's current optimized_processRotationActions. GPU-owned
// decoration transforms are accumulated in GroupManager; only objects which
// must retain vanilla physics / oriented-box behavior are transformed here.
void decomp_GJBaseGameLayer::optimized_processRotationActions(
    IOSUpstream::GroupManager& groupManager
) {
    PROFILER_PUSH("optimized_processRotationActions");

    const float objectLayerScaleX = m_objectLayer->getScaleX();
    const float objectLayerScaleY = m_objectLayer->getScaleY();
    const CCPoint objectLayerPosition = m_objectLayer->getPosition();

    m_objectLayer->setScale(1.0f);
    m_objectLayer->setPosition(CCPointZero);

    for (auto& [_, commands] : m_effectManager->m_unkMap5c8) {
        for (auto command : commands) {
            if (command)
                command->m_unkInt204 = m_gameState.m_commandIndex;
        }
    }

    for (auto command : m_effectManager->m_unkVector5b0) {
        if (!command ||
            command->m_unkInt204 != m_gameState.m_commandIndex ||
            command->m_someInterpValue1RelatedFalse) {
            continue;
        }

        GameObject* mainObject = tryGetMainObject(command->m_centerGroupID);
        const auto objectsToRotate =
            groupManager.getVanillaRotateGroup(command->m_targetGroupID);
        CCArray* staticGroup = getStaticGroup(command->m_targetGroupID);

        float rotation = 0.f;
        bool ignoreStaticGroups = false;
        if (staticGroup && staticGroup->count() != 0) {
            rotation = command->m_someInterpValue1RelatedOne -
                command->m_someInterpValue1RelatedZero;
            ignoreStaticGroups = false;
        } else {
            rotation = command->m_someInterpValue2RelatedOne -
                command->m_someInterpValue2RelatedZero;
            ignoreStaticGroups = true;
        }

        if (rotation == 0.f && !command->m_finishRelated)
            continue;

        const float localRotation =
            command->m_lockObjectRotation ? 0.f : rotation;

        command->m_someInterpValue1RelatedFalse = true;

        claimRotationAction(
            command->m_targetGroupID,
            command->m_centerGroupID,
            rotation,
            localRotation,
            ignoreStaticGroups,
            true
        );

        if (!mainObject) {
            groupManager.rotateGroup(
                static_cast<IOSUpstream::GroupID>(command->m_targetGroupID),
                rotation,
                command->m_lockObjectRotation
            );
        } else {
            const CCPoint position = mainObject->getUnmodifiedPosition();
            groupManager.rotateGroup(
                static_cast<IOSUpstream::GroupID>(command->m_targetGroupID),
                rotation,
                command->m_lockObjectRotation,
                ccPointToGLM(position)
            );
        }

        if (objectsToRotate.empty())
            continue;

        if (mainObject) {
            const CCPoint centerPoint = mainObject->getUnmodifiedPosition();
            const CCPoint claimedMove =
                claimMoveAction(command->m_targetGroupID, ignoreStaticGroups);

            if (claimedMove.x != 0.f || claimedMove.y != 0.f) {
                groupManager.moveGroup(
                    static_cast<IOSUpstream::GroupID>(command->m_targetGroupID),
                    claimedMove.x,
                    claimedMove.y
                );
            }

            m_areaTransformNode->setPosition(centerPoint);
            m_areaTransformNode->setScaleX(1.f);
            m_areaTransformNode->setScaleY(1.f);
            m_areaScaleNode->setScaleX(1.f);
            m_areaScaleNode->setScaleY(1.f);
            m_areaTransformNode->setSkewX(0.f);
            m_areaTransformNode->setSkewY(0.f);
            m_areaSkewNode->setSkewX(0.f);
            m_areaSkewNode->setSkewY(0.f);
            m_areaTransformNode->setRotation(rotation);
            prepareTransformParent(false);

            const CCAffineTransform transform1 =
                m_areaTransformNode->nodeToWorldTransform();

            // Keep the owner's transform-node reset sequence. The skew-node
            // matrix is not consumed by this optimized rotation calculation.
            (void)m_areaSkewNode->nodeToWorldTransform();
            m_areaTransformNode2->setScaleX(1.f);
            m_areaTransformNode2->setScaleY(1.f);
            m_areaTransformNode2->setSkewX(0.f);
            m_areaTransformNode2->setSkewY(0.f);
            m_areaTransformNode2->setRotation(0.f);

            for (auto object : objectsToRotate) {
                if (!object)
                    continue;

                object->m_unk4fb = command->m_finishRelated;
                if (!object->m_isDecoration2 &&
                    object->m_unk4C4 != m_gameState.m_commandIndex) {
                    object->m_lastPosition.x = object->m_positionX;
                    object->m_lastPosition.y = object->m_positionY;
                    object->m_unk4C4 = m_gameState.m_commandIndex;
                    object->dirtifyObjectRect();
                }

                object->m_isDirty = true;
                object->m_isUnmodifiedPosDirty = true;

                const CCPoint centerToObject = ccp(
                    object->m_positionX - object->m_positionXOffset - centerPoint.x,
                    object->m_positionY - object->m_positionYOffset - centerPoint.y
                );
                const CCPoint rotatedCenterToObject =
                    CCPointApplyAffineTransform(centerToObject, transform1);

                object->m_positionX += rotatedCenterToObject.x +
                    claimedMove.x - object->m_positionX + object->m_positionXOffset;
                object->m_positionY += rotatedCenterToObject.y +
                    claimedMove.y - object->m_positionY + object->m_positionYOffset;

                if (localRotation != 0.f && object->m_canRotateFree) {
                    object->m_rotationXOffset += localRotation;
                    object->m_rotationYOffset += localRotation;
                    object->addRotation(localRotation);
                    if (object->m_objectType != GameObjectType::Decoration &&
                        !object->m_shouldUseOuterOb) {
                        object->calculateOrientedBox();
                    }
                }

                updateObjectSection(object);
            }
        } else if (localRotation != 0.f) {
            for (auto object : objectsToRotate) {
                if (!object || !object->m_canRotateFree)
                    continue;

                object->m_rotationXOffset += localRotation;
                object->m_rotationYOffset += localRotation;
                object->addRotation(localRotation);
                if (object->m_objectType != GameObjectType::Decoration) {
                    object->m_isObjectRectDirty = true;
                    object->m_isOrientedBoxDirty = true;
                    if (!object->m_shouldUseOuterOb)
                        object->calculateOrientedBox();
                }
            }
        }
    }

    m_objectLayer->setScaleX(objectLayerScaleX);
    m_objectLayer->setScaleY(objectLayerScaleY);
    m_objectLayer->setPosition(objectLayerPosition);

    PROFILER_POP();
}

#endif
