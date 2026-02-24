#include "GJBaseGameLayer.hpp"

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