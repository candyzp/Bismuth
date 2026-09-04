#include "Geode/cocos/CCDirector.h"
#include "Renderer.hpp"
#include <decomp/PlayLayer.hpp>
#include <BProfiler.hpp>

#ifdef GEODE_IS_IOS
#include "ios/ResolvedStateLayer.hpp"
#include "ios/AssistShadowBatch.hpp"
#include "ios/StandaloneAssistBatch.hpp"
#include "Geode/cocos/sprite_nodes/CCSpriteBatchNode.h"
#include "Geode/cocos/textures/CCTextureAtlas.h"
#include <cstring>
#include <vector>
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

        // RendererIOS::prepareGPUOwnedSprite() reads the current atlas quad to
        // see whether it is already parked. Cocos getQuads() marks the atlas
        // dirty even for that read. Detect the already-zero case from the public
        // CPU atlas storage so a pure read cannot force a full atlas VBO upload
        // every frame on decoration-heavy levels.
        auto atlas = this->getTextureAtlas();
        const auto atlasIndex = this->getAtlasIndex();
        bool restoreCleanAtlas = false;
        if (atlas && !atlas->m_bDirty && atlas->m_pQuads &&
            atlasIndex != CCSpriteIndexNotInitialized && atlasIndex < atlas->getTotalQuads()) {
            cocos2d::ccV3F_C4B_T2F_Quad zeroQuad {};
            restoreCleanAtlas = std::memcmp(
                &atlas->m_pQuads[atlasIndex],
                &zeroQuad,
                sizeof(zeroQuad)
            ) == 0;
        }

        if (!renderer || !renderer->prepareGPUOwnedSprite(this)) {
            cocos2d::CCSprite::updateTransform();
            return;
        }

        if (restoreCleanAtlas && atlas && atlas->m_bDirty)
            atlas->m_bDirty = false;

        // The GPU owns final quad expansion. prepareGPUOwnedSprite() also makes
        // sure a stock atlas slot that GD just populated is parked once before
        // the stock batch can draw it. No matrix expansion or updateQuad here.
        this->setDirty(false);

        // Stock CCSprite::updateTransform recursively visits batched children.
        // Preserve only that traversal. Owned children take this same fast path;
        // unowned animation/complex children immediately call stock Cocos.
        if (auto children = this->getChildren()) {
            for (auto child : CCArrayExt<cocos2d::CCNode*>(children)) {
                if (auto sprite = typeinfo_cast<cocos2d::CCSprite*>(child))
                    sprite->updateTransform();
            }
        }
    }
};

namespace {
struct AtlasGPUOwner {
    AssistShadowBatch* immediate = nullptr;
    StandaloneAssistBatch* deferred = nullptr;

    bool empty() const { return !immediate && !deferred; }
    bool operator==(const AtlasGPUOwner& other) const {
        return immediate == other.immediate && deferred == other.deferred;
    }
};

cocos2d::CCSpriteBatchNode* g_currentSpriteBatchDraw = nullptr;

static AtlasGPUOwner atlasOwnerForSprite(Renderer* renderer, cocos2d::CCSprite* sprite) {
    if (!renderer || !sprite || !renderer->isGPUOwnedSprite(sprite))
        return {};

    if (auto immediate = AssistShadowBatch::ownerForSprite(sprite))
        return { immediate, nullptr };
    if (auto deferred = StandaloneAssistBatch::deferredOwnerForSprite(sprite))
        return { nullptr, deferred };
    return {};
}

static bool syncWholeDirtyAtlas(cocos2d::CCTextureAtlas* atlas) {
    if (!atlas)
        return false;
    if (!atlas->m_bDirty)
        return true;

    const usize quadCount = static_cast<usize>(atlas->getTotalQuads());
    if (quadCount == 0) {
        atlas->m_bDirty = false;
        return true;
    }
    if (!atlas->m_pQuads || !atlas->m_pBuffersVBO[0])
        return false;

    // drawNumberOfQuads(start,count) may clear m_bDirty after uploading only the
    // requested sub-range on Cocos' non-VAO path. Interleaving requires several
    // partial draws, so synchronize the complete live CPU atlas exactly once.
    GLint previousVBO = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousVBO);
    glBindBuffer(GL_ARRAY_BUFFER, atlas->m_pBuffersVBO[0]);
    glBufferSubData(
        GL_ARRAY_BUFFER,
        0,
        static_cast<GLsizeiptr>(quadCount * sizeof(cocos2d::ccV3F_C4B_T2F_Quad)),
        atlas->m_pQuads
    );
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<u32>(previousVBO));
    atlas->m_bDirty = false;
    return true;
}

static void bindStockAtlasTexture(cocos2d::CCTextureAtlas* atlas) {
    if (!atlas || !atlas->getTexture())
        return;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas->getTexture()->getName());
}

static bool beginOwnerIfNeeded(
    const AtlasGPUOwner& owner,
    std::vector<AtlasGPUOwner>& begunOwners
) {
    for (const auto& begun : begunOwners) {
        if (begun == owner)
            return true;
    }

    if (owner.immediate)
        owner.immediate->beginAtlasFrame();
    else if (owner.deferred)
        owner.deferred->beginFrame();
    else
        return false;

    begunOwners.push_back(owner);
    return true;
}

static bool drawOwnerRun(
    const AtlasGPUOwner& owner,
    const std::vector<cocos2d::CCSprite*>& run
) {
    if (owner.immediate)
        return owner.immediate->drawOrderedSprites(run);
    if (owner.deferred)
        return owner.deferred->drawOrderedSprites(run);
    return false;
}

static bool tryDrawInterleavedAtlas(cocos2d::CCTextureAtlas* atlas) {
    auto renderer = Renderer::get();
    auto stockBatch = g_currentSpriteBatchDraw;
    if (!renderer || !renderer->isEnabled() || !atlas || !stockBatch)
        return false;
    if (stockBatch->getTextureAtlas() != atlas)
        return false;

    const u32 totalQuads = atlas->getTotalQuads();
    if (totalQuads == 0)
        return false;

    auto descendants = stockBatch->getDescendants();
    if (!descendants)
        return false;

    // Reused scratch avoids per-frame allocation on decoration-heavy levels.
    static thread_local std::vector<cocos2d::CCSprite*> atlasSprites;
    static thread_local std::vector<AtlasGPUOwner> atlasOwners;
    static thread_local std::vector<cocos2d::CCSprite*> runSprites;
    static thread_local std::vector<AtlasGPUOwner> begunOwners;

    atlasSprites.assign(totalQuads, nullptr);
    atlasOwners.assign(totalQuads, {});
    begunOwners.clear();

    for (u32 i = 0; i < descendants->count(); ++i) {
        auto sprite = typeinfo_cast<cocos2d::CCSprite*>(descendants->objectAtIndex(i));
        if (!sprite || sprite->getBatchNode() != stockBatch)
            continue;

        const u32 atlasIndex = sprite->getAtlasIndex();
        if (atlasIndex == CCSpriteIndexNotInitialized || atlasIndex >= totalQuads)
            continue;
        atlasSprites[atlasIndex] = sprite;
    }

    bool hasGPU = false;
    for (u32 atlasIndex = 0; atlasIndex < totalQuads; ++atlasIndex) {
        auto sprite = atlasSprites[atlasIndex];
        if (!sprite)
            continue;

        auto owner = atlasOwnerForSprite(renderer, sprite);
        if (owner.empty())
            continue;

        atlasOwners[atlasIndex] = owner;
        hasGPU = true;
    }

    if (!hasGPU)
        return false;
    if (!syncWholeDirtyAtlas(atlas))
        return false;

    // Ensure texture unit zero really contains the stock atlas before the first
    // GPU handoff. Shader::use()/setTexture use raw GL and intentionally do not
    // mutate Cocos' state cache, so restoring the actual GL binding keeps both
    // the cache and hardware state coherent across every stock/GPU switch.
    bindStockAtlasTexture(atlas);

    u32 stockStart = 0;
    u32 atlasIndex = 0;
    while (atlasIndex < totalQuads) {
        const auto owner = atlasOwners[atlasIndex];
        if (owner.empty()) {
            ++atlasIndex;
            continue;
        }

        if (atlasIndex > stockStart) {
            bindStockAtlasTexture(atlas);
            atlas->drawNumberOfQuads(atlasIndex - stockStart, stockStart);
        }

        runSprites.clear();
        u32 runEnd = atlasIndex;
        while (runEnd < totalQuads && atlasOwners[runEnd] == owner && !owner.empty()) {
            auto sprite = atlasSprites[runEnd];
            if (!sprite)
                break;
            runSprites.push_back(sprite);
            ++runEnd;
        }

        beginOwnerIfNeeded(owner, begunOwners);
        if (!drawOwnerRun(owner, runSprites)) {
            // Registration is created only after persistent geometry + GL state
            // are ready, so a live registered owner must be drawable. Do not
            // silently route parked quads back through stock and hide a renderer
            // bug behind a fake fallback.
            static bool loggedInvariantFailure = false;
            if (!loggedInvariantFailure) {
                log::error(
                    "Bismuth iOS atlas interleave invariant failed at atlas index {} ({} sprite run)",
                    atlasIndex,
                    runSprites.size()
                );
                loggedInvariantFailure = true;
            }
        }

        atlasIndex = runEnd;
        stockStart = runEnd;
    }

    if (stockStart < totalQuads) {
        bindStockAtlasTexture(atlas);
        atlas->drawNumberOfQuads(totalQuads - stockStart, stockStart);
    }

    return true;
}
} // namespace

#include <Geode/modify/CCSpriteBatchNode.hpp>
class $modify(RendererTrackedSpriteBatchNode, cocos2d::CCSpriteBatchNode) {
    void draw() {
        auto previousBatch = g_currentSpriteBatchDraw;
        g_currentSpriteBatchDraw = this;
        cocos2d::CCSpriteBatchNode::draw();
        g_currentSpriteBatchDraw = previousBatch;
    }
};

#include <Geode/modify/CCTextureAtlas.hpp>
class $modify(RendererInterleavedTextureAtlas, cocos2d::CCTextureAtlas) {
    void drawQuads() {
        if (!tryDrawInterleavedAtlas(this))
            cocos2d::CCTextureAtlas::drawQuads();
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
