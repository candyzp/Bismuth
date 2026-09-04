#include "Geode/cocos/CCDirector.h"
#include "Renderer.hpp"
#include <decomp/PlayLayer.hpp>
#include <BProfiler.hpp>

#ifdef GEODE_IS_IOS
#include "ios/ResolvedStateLayer.hpp"
#endif

using namespace geode::prelude;

static bool newPlayLayer = false;

#include <Geode/modify/PlayLayer.hpp>
class $modify(RendererPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        newPlayLayer = true;
        PlayLayer::setupHasCompleted();
        newPlayLayer = false;
    }

    void resetLevel() {
        if (newPlayLayer && Renderer::get() == nullptr) {
            PlayLayer::resetLevel();

            auto batchLayer = this->m_objectLayer;
            if (!batchLayer) {
                log::error("failed to attach renderer: batch layer not found");
                return;
            }

            auto renderer = Renderer::create(this);
            if (renderer) {
                batchLayer->addChild(renderer, -100000);
                renderer->reset();
#ifdef GEODE_IS_IOS
                if (auto resolved = ResolvedStateLayer::getCurrent())
                    resolved->reseedActiveFromStock();
#endif
            }
            return;
        }

        PlayLayer::resetLevel();

        auto renderer = Renderer::get();
        if (renderer) {
            renderer->reset();
#ifdef GEODE_IS_IOS
            if (auto resolved = ResolvedStateLayer::getCurrent())
                resolved->reseedActiveFromStock();
#endif
        }
    }

    void updateVisibility(float dt) {
#ifdef GEODE_IS_IOS
        PlayLayer::updateVisibility(dt);
#else
        auto renderer = Renderer::get();
        if (renderer && renderer->useOptimizations()) {
            ((decomp_PlayLayer*)this)->optimized_updateVisibility(dt);
            return;
        }
        PlayLayer::updateVisibility(dt);
#endif
    }
};

static void removeDecoObjects(CCArray* array) {
    if (!array)
        return;

    for (u32 i = 0; i < array->count();) {
        auto object = (GameObject*)array->objectAtIndex(i);
        if (object->m_objectType == GameObjectType::Decoration)
            array->removeObjectAtIndex(i);
        else
            i++;
    }
}

#include <Geode/modify/GJBaseGameLayer.hpp>
class $modify(RendererGJBaseGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        auto timer = BProfiler::start("GJBaseGameLayer::update");
        GJBaseGameLayer::update(dt);
        timer.end();

        auto renderer = Renderer::get();
        if (renderer) {
            renderer->update(dt);
#ifdef GEODE_IS_IOS
            if (auto resolved = ResolvedStateLayer::getCurrent())
                resolved->finishEventFrame();
#endif
        }
    }

    void processMoveActions() {
#ifdef GEODE_IS_IOS
        GJBaseGameLayer::processMoveActions();
        return;
#else
        auto renderer = Renderer::get();
        if (renderer == nullptr) {
            GJBaseGameLayer::processMoveActions();
            return;
        }

        m_effectManager->processMoveCalculations();
        for (auto node : m_effectManager->m_unkVector6c0) {
            if (node->m_unk0d0)
                continue;

            int groupId = node->getTag();
            renderer->getGroupManager().moveGroup(groupId, node->m_unk038, node->m_unk040);

            CCArray* objects = getStaticGroup(groupId);
            if (objects)
                moveObjects(objects, node->m_unk038, node->m_unk040, 0);

            objects = getOptimizedGroup(groupId);
            if (objects)
                moveObjects(objects, node->m_unk090, node->m_unk098, 0);
        }
#endif
    }

    void processRotationActions() {
#ifdef GEODE_IS_IOS
        GJBaseGameLayer::processRotationActions();
        return;
#else
        auto renderer = Renderer::get();
        if (renderer == nullptr) {
            GJBaseGameLayer::processRotationActions();
            return;
        }

        auto eman = m_effectManager;
        for (auto cmdObj : eman->m_unkVector5b0) {
            if (cmdObj->m_someInterpValue1RelatedFalse)
                continue;

            i32 targetId = cmdObj->m_targetGroupID;
            i32 centerId = cmdObj->m_centerGroupID;

            auto mainObject = tryGetMainObject(centerId);
            auto staticGroup = getStaticGroup(targetId);
            const bool hasStaticGroup = staticGroup && staticGroup->count() != 0;

            float rotation;
            if (hasStaticGroup)
                rotation = cmdObj->m_someInterpValue1RelatedOne - cmdObj->m_someInterpValue1RelatedZero;
            else
                rotation = cmdObj->m_someInterpValue2RelatedOne - cmdObj->m_someInterpValue2RelatedZero;

            if (rotation == 0.0 && !cmdObj->m_finishRelated)
                continue;

            if (eman->m_unkMap770.find({ targetId, centerId }) != eman->m_unkMap770.end()) {
                for (auto obj : eman->m_unkMap770[{ targetId, centerId }]) {
                    if (obj->m_someInterpValue1RelatedFalse)
                        continue;
                    if (hasStaticGroup)
                        rotation += obj->m_someInterpValue1RelatedOne - obj->m_someInterpValue1RelatedZero;
                    else
                        rotation += obj->m_someInterpValue2RelatedOne - obj->m_someInterpValue2RelatedZero;
                }
            }

            if (mainObject == nullptr)
                renderer->getGroupManager().rotateGroup(targetId, rotation, cmdObj->m_lockObjectRotation);
            else {
                auto pos = mainObject->getUnmodifiedPosition();
                renderer->getGroupManager().rotateGroup(targetId, rotation, cmdObj->m_lockObjectRotation, ccPointToGLM(pos));
            }
        }

        GJBaseGameLayer::processRotationActions();
#endif
    }

    void processFollowActions() {
#ifdef GEODE_IS_IOS
        GJBaseGameLayer::processFollowActions();
        return;
#else
        auto renderer = Renderer::get();
        if (renderer == nullptr) {
            GJBaseGameLayer::processFollowActions();
            return;
        }

        auto eman = m_effectManager;
        for (auto node : eman->m_unkVector6d8) {
            i32 targetGroupId = node->getTag();
            i32 followGroupId = node->m_unk074;

            auto mainObject = tryGetMainObject(followGroupId);
            if (mainObject == nullptr)
                continue;

            double moveX = 0.0, moveY = 0.0;
            if (mainObject->m_unk4C4 == m_gameState.m_commandIndex) {
                moveX = (mainObject->m_positionX - mainObject->m_lastPosition.x) * node->m_unk080;
                moveY = (mainObject->m_positionY - mainObject->m_lastPosition.y) * node->m_unk088;
            }

            renderer->getGroupManager().moveGroup(targetGroupId, moveX, moveY);
        }

        GJBaseGameLayer::processFollowActions();
#endif
    }

    void toggleGroup(int id, bool activate) {
        GJBaseGameLayer::toggleGroup(id, activate);
#ifndef GEODE_IS_IOS
        auto renderer = Renderer::get();
        if (renderer)
            renderer->getGroupManager().toggleGroup(id, activate);
#endif
    }

    void optimizeMoveGroups() {
        GJBaseGameLayer::optimizeMoveGroups();

#ifdef GEODE_IS_IOS
        return;
#else
        auto renderer = Renderer::get();
        if (!renderer || !renderer->useOptimizations())
            return;

        for (auto array : m_optimizedGroups)
            removeDecoObjects(array);
        for (auto array : m_staticGroups)
            removeDecoObjects(array);
#endif
    }
};

#ifdef GEODE_IS_IOS
#include <Geode/modify/GameObject.hpp>
class $modify(RendererTrackedGameObject, GameObject) {
    void activateObject() {
        GameObject::activateObject();

        if (Renderer::get()) {
            if (auto resolved = ResolvedStateLayer::getCurrent())
                resolved->onObjectActivated(this);
        }
    }

    void deactivateObject(bool value) {
        GameObject::deactivateObject(value);

        if (Renderer::get()) {
            if (auto resolved = ResolvedStateLayer::getCurrent())
                resolved->onObjectDeactivated(this);
        }
    }
};

#include <Geode/modify/CCSprite.hpp>
class $modify(RendererOwnedCCSprite, cocos2d::CCSprite) {
    void updateTransform() {
        auto renderer = Renderer::get();
        if (!renderer || !renderer->prepareGPUOwnedSprite(this)) {
            cocos2d::CCSprite::updateTransform();
            return;
        }

        this->setDirty(true);
    }
};

#include <Geode/modify/CCSpriteBatchNode.hpp>
class $modify(RendererInterleavedSpriteBatchNode, cocos2d::CCSpriteBatchNode) {
    void draw() {
        auto renderer = Renderer::get();
        if (!renderer || !renderer->isEnabled()) {
            cocos2d::CCSpriteBatchNode::draw();
            return;
        }

        if (!renderer->isGPUInterleavedBatch(this)) {
            cocos2d::CCSpriteBatchNode::draw();
            return;
        }

        CC_NODE_DRAW_SETUP();
        const auto blend = this->getBlendFunc();
        ccGLBlendFunc(blend.src, blend.dst);
        if (!renderer->drawGPUInterleavedBatch(this))
            log::error("Bismuth iOS interleave failed for an owned gameplay batch");
    }
};
#endif

#include <Geode/modify/CCDisplayLinkDirector.hpp>
class $modify(RendererCCDisplayLinkDirector, CCDisplayLinkDirector) {
    void mainLoop() {
        auto ren = Renderer::get();
        if (!ren) {
            CCDisplayLinkDirector::mainLoop();
            return;
        }

        auto timer = BProfiler::start("Frame time");
        CCDisplayLinkDirector::mainLoop();
        timer.end();

        BProfiler::frameEnd();
    }
};