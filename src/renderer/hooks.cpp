#include "Geode/cocos/CCDirector.h"
#include "Renderer.hpp"
#include <decomp/PlayLayer.hpp>
#include <BProfiler.hpp>

#ifdef GEODE_IS_IOS
#include "ios/ResolvedStateLayer.hpp"
#include "ios/GPUTruth.hpp"
#include "ios/AtlasInterleave.hpp"
#include "ios_upstream/IOSGPUCapabilities.hpp"
#include "ios_upstream/IOSSceneData.hpp"
#include <decomp/GJBaseGameLayer.hpp>
#include <Geode/binding/CheckpointObject.hpp>
#include <optional>
#endif

using namespace geode::prelude;

static bool newPlayLayer = false;

#include <Geode/modify/PlayLayer.hpp>
class $modify(RendererPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        newPlayLayer = true;
        PlayLayer::setupHasCompleted();
#ifdef GEODE_IS_IOS
        static bool loggedGPUCapabilities = false;
        if (!loggedGPUCapabilities) {
            IOSGPU::Capabilities capabilities;
            if (IOSGPU::queryCapabilities(capabilities))
                IOSGPU::logCapabilities(capabilities);
            else
                log::error("Bismuth iOS could not query the active OpenGL ES context");
            loggedGPUCapabilities = true;
        }
#endif
        newPlayLayer = false;
    }

    void resetLevel() {
#ifdef GEODE_IS_IOS
        // A replacement PlayLayer can finish setup before the outgoing layer's
        // destructor runs. Its renderer must not block this level's creation.
        if (newPlayLayer) {
            if (auto existing = Renderer::forPlayLayer(this))
                existing->resumeGPU();
            else if (auto previous = Renderer::get())
                previous->suspendGPU();
        }
#endif
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

#ifdef GEODE_IS_IOS
        if (auto scene = IOSUpstream::SceneData::getCurrent();
            scene && scene->getPlayLayer() == this) {
            scene->reset();
        }

        // Menu restart may reuse this layer without another enter-transition
        // callback. The active singleton is null while its renderer is suspended.
        auto renderer = Renderer::forPlayLayer(this);
        if (renderer)
            renderer->resumeGPU();
#else
        auto renderer = Renderer::get();
#endif
        if (renderer) {
            renderer->reset();
#ifdef GEODE_IS_IOS
            if (auto resolved = ResolvedStateLayer::getCurrent())
                resolved->reseedActiveFromStock();
#endif
        }
    }

    CheckpointObject* createCheckpoint() {
        CheckpointObject* checkpoint = PlayLayer::createCheckpoint();
#ifdef GEODE_IS_IOS
        if (auto scene = IOSUpstream::SceneData::getCurrent();
            scene && scene->getPlayLayer() == this) {
            scene->getGroupManager().storeCheckpoint(checkpoint);
        }
#endif
        return checkpoint;
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::loadFromCheckpoint(checkpoint);
#ifdef GEODE_IS_IOS
        if (auto scene = IOSUpstream::SceneData::getCurrent();
            scene && scene->getPlayLayer() == this) {
            scene->getGroupManager().restoreCheckpoint(checkpoint);
        }
#endif
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
#ifdef GEODE_IS_IOS
        if (renderer && static_cast<GJBaseGameLayer*>(renderer->getPlayLayer()) !=
            static_cast<GJBaseGameLayer*>(this))
            return;

        if (auto scene = IOSUpstream::SceneData::getCurrent();
            scene && scene->isReady() &&
            static_cast<GJBaseGameLayer*>(scene->getPlayLayer()) == this) {
            scene->update(dt);
        }
#endif
        if (renderer) {
            renderer->update(dt);
        }
    }

    void processMoveActions() {
#ifdef GEODE_IS_IOS
        auto scene = IOSUpstream::SceneData::getCurrent();
        if (!scene || !scene->isReady() ||
            static_cast<GJBaseGameLayer*>(scene->getPlayLayer()) != this) {
            GJBaseGameLayer::processMoveActions();
            return;
        }

        // Port the owner's move-action split: Geometry Dash still advances its
        // move command nodes and physical object data, while GroupManager mirrors
        // the same deltas into the render-state table consumed by the GPU.
        m_effectManager->processMoveCalculations();
        for (auto node : m_effectManager->m_unkVector6c0) {
            if (!node || node->m_unk0d0)
                continue;

            const i32 groupId = node->getTag();
            scene->getGroupManager().moveGroup(
                static_cast<IOSUpstream::GroupID>(groupId),
                static_cast<float>(node->m_unk038),
                static_cast<float>(node->m_unk040)
            );

            CCArray* objects = getStaticGroup(groupId);
            if (objects) {
                ((decomp_GJBaseGameLayer*)this)->moveObjects(
                    objects, node->m_unk038, node->m_unk040, 0
                );
            }

            // In the owner's optimized path decorations have already been
            // removed from the vanilla group arrays by optimizeMoveGroups().
            // Their movement is represented exclusively in GroupManager.
        }
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
        auto scene = IOSUpstream::SceneData::getCurrent();
        if (!scene || !scene->isReady() ||
            static_cast<GJBaseGameLayer*>(scene->getPlayLayer()) != this) {
            GJBaseGameLayer::processRotationActions();
            return;
        }

        ((decomp_GJBaseGameLayer*)this)->optimized_processRotationActions(
            scene->getGroupManager()
        );
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
        auto scene = IOSUpstream::SceneData::getCurrent();
        if (!scene || !scene->isReady() ||
            static_cast<GJBaseGameLayer*>(scene->getPlayLayer()) != this) {
            GJBaseGameLayer::processFollowActions();
            return;
        }

        auto effectManager = m_effectManager;
        for (auto node : effectManager->m_unkVector6d8) {
            if (!node)
                continue;
            const i32 targetGroupId = node->getTag();
            const i32 followGroupId = node->m_unk074;
            auto mainObject = tryGetMainObject(followGroupId);
            if (!mainObject)
                continue;

            double moveX = 0.0;
            double moveY = 0.0;
            if (mainObject->m_unk4C4 == m_gameState.m_commandIndex) {
                moveX = (mainObject->m_positionX - mainObject->m_lastPosition.x) * node->m_unk080;
                moveY = (mainObject->m_positionY - mainObject->m_lastPosition.y) * node->m_unk088;
            }

            scene->getGroupManager().moveGroup(
                static_cast<IOSUpstream::GroupID>(targetGroupId),
                static_cast<float>(moveX),
                static_cast<float>(moveY)
            );
        }

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
#ifdef GEODE_IS_IOS
        if (auto scene = IOSUpstream::SceneData::getCurrent();
            scene && scene->isReady() &&
            static_cast<GJBaseGameLayer*>(scene->getPlayLayer()) == this) {
            scene->getGroupManager().toggleGroup(
                static_cast<IOSUpstream::GroupID>(id), activate
            );
        }
#else
        auto renderer = Renderer::get();
        if (renderer)
            renderer->getGroupManager().toggleGroup(id, activate);
#endif
    }

    void optimizeMoveGroups() {
        GJBaseGameLayer::optimizeMoveGroups();

#ifdef GEODE_IS_IOS
        auto scene = IOSUpstream::SceneData::getCurrent();
        if (!scene || !scene->isReady() ||
            static_cast<GJBaseGameLayer*>(scene->getPlayLayer()) != this)
            return;

        // Match the owner's optimized renderer: GPU-managed decorations are
        // removed from vanilla move/rotation groups while solid gameplay
        // objects remain available for physics and oriented-box updates.
        for (auto array : m_optimizedGroups)
            removeDecoObjects(array);
        for (auto array : m_staticGroups)
            removeDecoObjects(array);
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
#include <Geode/modify/CheckpointObject.hpp>
class $modify(RendererCheckpointObject, CheckpointObject) {
    struct Fields {
        CheckpointObject* checkpoint = nullptr;
        ~Fields() {
            if (auto scene = IOSUpstream::SceneData::getCurrent())
                scene->getGroupManager().removeCheckpoint(checkpoint);
        }
    };

    bool init() {
        if (!CheckpointObject::init())
            return false;
        m_fields.self()->checkpoint = this;
        return true;
    }
};

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
            // Stock children still need their parent's affine/visibility state,
            // even though this owned quad is expanded by the GPU. Visit children
            // so each keeps its own stock/GPU ownership and update order.
            this->m_transformToBatch = this->nodeToParentTransform();
            this->m_bShouldBeHidden = !this->isVisible();
            if (auto parent = typeinfo_cast<cocos2d::CCSprite*>(this->getParent());
                parent && this->getParent() != this->getBatchNode()) {
                this->m_transformToBatch = cocos2d::CCAffineTransformConcat(
                    this->m_transformToBatch, parent->m_transformToBatch);
                this->m_bShouldBeHidden |= parent->m_bShouldBeHidden;
            }
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
