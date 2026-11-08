#include "Geode/cocos/CCDirector.h"
#include "Renderer.hpp"
#include <decomp/PlayLayer.hpp>
#include <BProfiler.hpp>
#ifdef GEODE_IS_IOS
#include <decomp/GJBaseGameLayer.hpp>
#include "ios_upstream/IOSGPURenderer.hpp"
#include "ios_upstream/IOSGroupManager.hpp"
#include <chrono>
#endif

#ifdef GEODE_IS_IOS
#include "ios/ResolvedStateLayer.hpp"
#include "ios/GPUTruth.hpp"
#include "ios/AtlasInterleave.hpp"
#endif

using namespace geode::prelude;

#ifdef GEODE_IS_IOS
static void pruneGPUOwnedDecorations(GJBaseGameLayer* layer);
#endif

static bool newPlayLayer = false;

#include <Geode/modify/PlayLayer.hpp>
class $modify(RendererPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        newPlayLayer = true;
        PlayLayer::setupHasCompleted();
        newPlayLayer = false;
    }

    void resetLevel() {
#ifdef GEODE_IS_IOS
        // Remove draw nodes before PlayLayer rebuilds its Z-order containers.
        auto existing = IOSUpstream::IOSGPURenderer::getCurrent();
        if (existing && existing->getPlayLayer() == this)
            existing->prepareForLevelReset();

        PlayLayer::resetLevel();

        if (newPlayLayer || (existing && existing->getPlayLayer() == this)) {
            auto renderer = IOSUpstream::IOSGPURenderer::create(this);
            if (!renderer) {
                log::error("Bismuth iOS GPU-only renderer could not be created");
                return;
            }

            // The owner node keeps SceneData/VBO/shader alive for this level;
            // draw nodes are siblings of Geometry Dash's stock Z-order batches.
            if (renderer->getParent() != this)
                this->addChild(renderer, -100000);

            renderer->reset();
            renderer->enforceGPUOwnership();
            pruneGPUOwnedDecorations(this);
        }
#else
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
            }
            return;
        }
        PlayLayer::resetLevel();
        auto renderer = Renderer::get();
        if (renderer)
            renderer->reset();
#endif
    }

    void updateVisibility(float dt) {
#ifdef GEODE_IS_IOS
        auto renderer = IOSUpstream::IOSGPURenderer::getCurrent();
        if (renderer && renderer->getPlayLayer() == this) {
            // Keep GD's effect/particle simulation, but skip stock per-object
            // visibility and sprite traversal: the GPU renderer owns visuals.
            ((decomp_PlayLayer*)this)->optimized_updateVisibility(dt);
            if (renderer->isReadyForDrawing())
                renderer->refreshBlendOrder();
            return;
        }
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
        if (object && object->m_objectType == GameObjectType::Decoration)
            array->removeObjectAtIndex(i);
        else
            i++;
    }
}

#ifdef GEODE_IS_IOS
static void pruneGPUOwnedDecorations(GJBaseGameLayer* baseLayer) {
    auto renderer = IOSUpstream::IOSGPURenderer::getCurrent();
    if (!baseLayer || !renderer || !renderer->isReadyForDrawing() ||
        static_cast<GJBaseGameLayer*>(renderer->getPlayLayer()) != baseLayer)
        return;

    // Match upstream ownership: decorations are GPU-rendered; static gameplay
    // objects keep their CPU movement/collision updates.
    for (auto group : baseLayer->m_optimizedGroups)
        removeDecoObjects(group);
    for (auto group : baseLayer->m_staticGroups)
        removeDecoObjects(group);
}
#endif

#include <Geode/modify/GJBaseGameLayer.hpp>
class $modify(RendererGJBaseGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        auto timer = BProfiler::start("GJBaseGameLayer::update");
#ifdef GEODE_IS_IOS
        const auto updateStart = std::chrono::steady_clock::now();
        GJBaseGameLayer::update(dt);
        timer.end();

        auto renderer = IOSUpstream::IOSGPURenderer::getCurrent();
        if (renderer && static_cast<GJBaseGameLayer*>(renderer->getPlayLayer()) == this) {
            const float elapsed = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - updateStart
            ).count();
            renderer->recordGameLayerUpdate(elapsed);
            if (renderer->isReadyForDrawing())
                renderer->updateScene(dt);
        }
#else
        GJBaseGameLayer::update(dt);
        timer.end();
        auto renderer = Renderer::get();
        if (renderer)
            renderer->update(dt);
#endif
    }

    void processMoveActions() {
#ifdef GEODE_IS_IOS
        auto renderer = IOSUpstream::IOSGPURenderer::getCurrent();
        if (!renderer || !renderer->isReadyForDrawing() ||
            static_cast<GJBaseGameLayer*>(renderer->getPlayLayer()) != this) {
            GJBaseGameLayer::processMoveActions();
            return;
        }

        m_effectManager->processMoveCalculations();
        for (auto node : m_effectManager->m_unkVector6c0) {
            if (node->m_unk0d0)
                continue;
            const i32 groupId = node->getTag();
            renderer->getGroupManager().moveGroup(
                static_cast<IOSUpstream::GroupID>(groupId),
                node->m_unk038, node->m_unk040
            );
            CCArray* objects = getStaticGroup(groupId);
            if (objects)
                moveObjects(objects, node->m_unk038, node->m_unk040, 0);
        }
#else
        auto renderer = Renderer::get();
        if (!renderer) {
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
        auto renderer = IOSUpstream::IOSGPURenderer::getCurrent();
        if (renderer && renderer->isReadyForDrawing() &&
            static_cast<GJBaseGameLayer*>(renderer->getPlayLayer()) == this) {
            ((decomp_GJBaseGameLayer*)this)->optimized_processRotationActions(
                renderer->getGroupManager()
            );
            return;
        }
        GJBaseGameLayer::processRotationActions();
#else
        auto renderer = Renderer::get();
        if (!renderer) {
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
            if (!mainObject)
                renderer->getGroupManager().rotateGroup(targetId, rotation, cmdObj->m_lockObjectRotation);
            else
                renderer->getGroupManager().rotateGroup(
                    targetId, rotation, cmdObj->m_lockObjectRotation,
                    ccPointToGLM(mainObject->getUnmodifiedPosition())
                );
        }
        GJBaseGameLayer::processRotationActions();
#endif
    }

    void processFollowActions() {
#ifdef GEODE_IS_IOS
        auto renderer = IOSUpstream::IOSGPURenderer::getCurrent();
        if (!renderer || !renderer->isReadyForDrawing() ||
            static_cast<GJBaseGameLayer*>(renderer->getPlayLayer()) != this) {
            GJBaseGameLayer::processFollowActions();
            return;
        }
        auto eman = m_effectManager;
        for (auto node : eman->m_unkVector6d8) {
            const i32 targetGroupId = node->getTag();
            const i32 followGroupId = node->m_unk074;
            auto mainObject = tryGetMainObject(followGroupId);
            if (!mainObject)
                continue;
            double moveX = 0.0, moveY = 0.0;
            if (mainObject->m_unk4C4 == m_gameState.m_commandIndex) {
                moveX = (mainObject->m_positionX - mainObject->m_lastPosition.x) * node->m_unk080;
                moveY = (mainObject->m_positionY - mainObject->m_lastPosition.y) * node->m_unk088;
            }
            renderer->getGroupManager().moveGroup(
                static_cast<IOSUpstream::GroupID>(targetGroupId),
                static_cast<float>(moveX), static_cast<float>(moveY)
            );
        }
        GJBaseGameLayer::processFollowActions();
#else
        auto renderer = Renderer::get();
        if (!renderer) {
            GJBaseGameLayer::processFollowActions();
            return;
        }
        auto eman = m_effectManager;
        for (auto node : eman->m_unkVector6d8) {
            i32 targetGroupId = node->getTag();
            i32 followGroupId = node->m_unk074;
            auto mainObject = tryGetMainObject(followGroupId);
            if (!mainObject)
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
#ifdef GEODE_IS_IOS
        auto renderer = IOSUpstream::IOSGPURenderer::getCurrent();
        if (renderer && renderer->isReadyForDrawing() &&
            static_cast<GJBaseGameLayer*>(renderer->getPlayLayer()) == this)
            renderer->getGroupManager().toggleGroup(
                static_cast<IOSUpstream::GroupID>(id), activate
            );
#else
        auto renderer = Renderer::get();
        if (renderer)
            renderer->getGroupManager().toggleGroup(id, activate);
#endif
    }

    void optimizeMoveGroups() {
        GJBaseGameLayer::optimizeMoveGroups();
#ifdef GEODE_IS_IOS
        pruneGPUOwnedDecorations(this);
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

        if (auto children = this->getChildren(); children && children->count()) {
            // This GPU-owned quad is expanded by the shader, but stock descendants
            // still need the complete transform to the atlas. Include plain CCNode
            // containers as well as CCSprite parents; otherwise nested decoration
            // children lose an ancestor transform and appear offset or missing.
            auto batch = this->getBatchNode();
            auto transform = this->nodeToParentTransform();
            bool hidden = !this->isVisible() || this->getDontDraw();

            for (auto parent = this->getParent(); parent && parent != batch;
                parent = parent->getParent()) {
                transform = cocos2d::CCAffineTransformConcat(
                    transform, parent->nodeToParentTransform()
                );
                hidden |= !parent->isVisible();
                if (auto parentSprite = typeinfo_cast<cocos2d::CCSprite*>(parent)) {
                    hidden |= parentSprite->getDontDraw() ||
                        parentSprite->m_bShouldBeHidden;
                }
            }

            this->m_transformToBatch = transform;
            this->m_bShouldBeHidden = hidden;
            cocos2d::CCNode::updateTransform();
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

        // Delay resolved-state work until the atlas scheduler proves that this
        // batch will actually submit GPU geometry. CPU-only hybrid frames should
        // not pay the GPU preparation tax.
        CC_NODE_DRAW_SETUP();
        const auto blend = this->getBlendFunc();
        ccGLBlendFunc(blend.src, blend.dst);

        // drawGPUInterleavedBatch performs one complete live-atlas validation.
        // Retrying the exact same batch inside one draw call can repeat expensive
        // atlas synchronization and leaves less room for a clean next-frame
        // recovery if GD mutates the batch during traversal.
        const bool submitted = renderer->drawGPUInterleavedBatch(this);

        if (submitted) {
            // This is the truth signal: the live-atlas path actually completed a
            // GPU submission for this batch. Merely owning a sprite is not enough.
            GPUTruth::recordObjectBatch(renderer.data(), this);
            return;
        }

        if (AtlasInterleaveRegistry::lastFailureCanUseStock()) {
            // Hybrid CPU lane. drawGPUInterleavedBatch only allows this when it
            // rejected the custom pass before submitting any stock/GPU prefix,
            // so one normal Cocos draw cannot double-render anything.
            cocos2d::CCSpriteBatchNode::draw();
            return;
        }

        // Only late submission errors remain unrecoverable in-frame. Do not
        // redraw the whole atlas after a partial custom pass.
        GPUTruth::recordObjectFailure(renderer.data());
        return;
    }
};
#endif

#include <Geode/modify/CCDisplayLinkDirector.hpp>
class $modify(RendererCCDisplayLinkDirector, CCDisplayLinkDirector) {
    void mainLoop() {
#ifdef GEODE_IS_IOS
        auto renderer = IOSUpstream::IOSGPURenderer::getCurrent();
        if (renderer)
            renderer->beginFrame();

        auto timer = BProfiler::start("Frame time");
        CCDisplayLinkDirector::mainLoop();
        timer.end();

        if (auto finishedRenderer = IOSUpstream::IOSGPURenderer::getCurrent())
            finishedRenderer->finishFrame();
        BProfiler::frameEnd();
#else
        auto ren = Renderer::get();
        if (!ren) {
            CCDisplayLinkDirector::mainLoop();
            return;
        }
        auto timer = BProfiler::start("Frame time");
        CCDisplayLinkDirector::mainLoop();
        timer.end();
        BProfiler::frameEnd();
#endif
    }
};
