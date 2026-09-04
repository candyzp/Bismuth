#include "Geode/cocos/CCDirector.h"
#include "Renderer.hpp"
#include <decomp/PlayLayer.hpp>
#include <BProfiler.hpp>

#ifdef GEODE_IS_IOS
#include "../ObjectUtils.hpp"
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
        // Never replace GD's visual lifecycle on iOS. Animation activation,
        // visibility, colors and child state remain stock even while selected
        // final sprite transforms are drawn by the GPU assist path.
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
            // Stock GD has completed this frame's gameplay, trigger and
            // visibility lifecycle before the GPU state is sampled.
            renderer->update(dt);
#ifdef GEODE_IS_IOS
            // Deactivated GPU records stayed live through renderer->update() so
            // their final visible=false state reached the shader. They can now
            // leave the hot polling set until stock activates them again.
            if (auto resolved = ResolvedStateLayer::getCurrent())
                resolved->finishEventFrame();
#endif
        }
    }

    void processMoveActions() {
#ifdef GEODE_IS_IOS
        // GD resolves all move state. Bismuth reads the final result later; it
        // does not simulate Move triggers or mutate movement groups.
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
        // Keep GD's movement/group bookkeeping fully intact on iOS.
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
        // STOCK GD owns the decision and performs the real hierarchy/batch
        // attachment first. Bismuth only observes the completed lifecycle state.
        GameObject::activateObject();

        if (Renderer::get()) {
            if (auto resolved = ResolvedStateLayer::getCurrent())
                resolved->onObjectActivated(this);
        }
    }

    void deactivateObject(bool value) {
        // Same rule in reverse: stock GD hides/removes the object first. Keep it
        // in the GPU hot set for one final resolved-state upload this frame.
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

        // Only an already-validated atlas plan may skip expansion. Leave the
        // sprite dirty so any later stock draw regenerates its real quad.
        this->setDirty(true);
    }
};

static bool batchContainsStockSensitiveRoot(
    Renderer* renderer,
    cocos2d::CCSpriteBatchNode* batch
) {
    if (!renderer || !batch)
        return true;

    auto children = batch->getChildren();
    if (!children)
        return false;

    for (auto child : CCArrayExt<cocos2d::CCNode*>(children)) {
        auto object = typeinfo_cast<GameObject*>(child);
        if (!object || renderer->isGPUOwnedSprite(object))
            continue;

        const bool complexStockVisual =
            object->m_classType == GameObjectClassType::Animated ||
            ObjectUtils::isInteractiveVisualObject(object) ||
            object->getHasSyncedAnimation() ||
            object->m_isInvisibleBlock ||
            object->m_glowSprite != nullptr ||
            object->m_colorSprite != nullptr ||
            (object->getChildren() && object->getChildren()->count() != 0);

        if (complexStockVisual)
            return true;
    }

    return false;
}

#include <Geode/modify/CCSpriteBatchNode.hpp>
class $modify(RendererInterleavedSpriteBatchNode, cocos2d::CCSpriteBatchNode) {
    void draw() {
        // Startup and teardown safety: before a live enabled Bismuth PlayLayer
        // recognizes this exact gameplay batch, go directly to stock Cocos. Do
        // not inspect descendants or atlas state while the renderer is disabled.
        auto renderer = Renderer::get();
        if (!renderer || !renderer->isEnabled()) {
            cocos2d::CCSpriteBatchNode::draw();
            return;
        }

        // First prove this is an exact Bismuth gameplay batch. Unrelated menu,
        // loading-screen and Geode UI batches never reach the child/visual scan.
        if (!renderer->isGPUInterleavedBatch(this)) {
            cocos2d::CCSpriteBatchNode::draw();
            return;
        }

        // Portals, animations and other multi-part stock roots must stay inside
        // one uninterrupted stock atlas submission. GPU-owned sprites in these
        // mixed batches are drawn by stock Cocos in their original atlas slots.
        if (batchContainsStockSensitiveRoot(renderer.data(), this)) {
            cocos2d::CCSpriteBatchNode::draw();
            return;
        }

        auto atlas = this->getTextureAtlas();
        if (!atlas || atlas->getTotalQuads() == 0) {
            cocos2d::CCSpriteBatchNode::draw();
            return;
        }

        // Keep the old child-cast crash fix: validate as CCNode first and only
        // call CCSprite methods after a checked RTTI cast. Any unexpected child
        // fails closed to the exact stock draw before Bismuth has submitted work.
        if (auto children = this->getChildren()) {
            for (auto child : CCArrayExt<cocos2d::CCNode*>(children)) {
                if (!typeinfo_cast<cocos2d::CCSprite*>(child)) {
                    cocos2d::CCSpriteBatchNode::draw();
                    return;
                }
            }
        }

        CC_NODE_DRAW_SETUP();
        const auto blend = this->getBlendFunc();
        ccGLBlendFunc(blend.src, blend.dst);
        if (!renderer->drawGPUInterleavedBatch(this))
            cocos2d::CCSpriteBatchNode::draw();
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