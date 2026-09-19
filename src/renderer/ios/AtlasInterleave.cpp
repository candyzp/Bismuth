#ifdef GEODE_IS_IOS

#include "AtlasInterleave.hpp"
#include "AtlasDrawPlan.hpp"
#include "AssistShadowBatch.hpp"
#include "StandaloneAssistBatch.hpp"
#include "../Renderer.hpp"

#include "Geode/cocos/CCDirector.h"
#include "Geode/cocos/kazmath/include/kazmath/mat4.h"
#include "Geode/cocos/textures/CCTextureAtlas.h"

#include <climits>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {
constexpr usize MAX_REASONABLE_ATLAS_QUADS = 262144;

struct SpriteOwner {
    Renderer* renderer = nullptr;
    AssistShadowBatch* immediate = nullptr;
    StandaloneAssistBatch* deferred = nullptr;
    u16 baseVertex = 0;

    bool empty() const {
        return !immediate && !deferred;
    }

    bool sameOwner(const SpriteOwner& other) const {
        return renderer == other.renderer &&
            immediate == other.immediate &&
            deferred == other.deferred;
    }
};

struct OwnerDrawData {
    ResolvedStateLayer* resolvedState = nullptr;
    Shader* shader = nullptr;
    u32 vao = 0;
    u32 ownerIndexBuffer = 0;
    usize* drawCalls = nullptr;
    usize* indexCount = nullptr;
};

struct AssistUniformLocations {
    GLint mvp = -1;
    GLint objectStateTexture = -1;
    GLint objectStateTextureSize = -1;
    GLint spriteStateTexture = -1;
    GLint spriteStateTextureSize = -1;
    GLint spriteSheetTexture = -1;
};

struct SavedGLState {
    GLint vao = 0;
    GLint elementArrayBuffer = 0;
    GLint program = 0;
    GLint activeTexture = GL_TEXTURE0;
    GLint textures[3] = {0, 0, 0};
};

struct BatchIndexCache {
    u32 buffer = 0;
    std::vector<u16> indices;
    std::vector<SpriteOwner> owners;
    std::vector<AtlasDrawRun> runs;
};

struct RegistryState {
    std::unordered_map<cocos2d::CCSprite*, SpriteOwner> spriteOwners;
    std::unordered_map<cocos2d::CCSpriteBatchNode*, AssistShadowBatch*> immediateByBatch;
    std::unordered_map<AssistShadowBatch*, Renderer*> immediateRenderers;
    std::unordered_map<StandaloneAssistBatch*, Renderer*> deferredRenderers;
    std::unordered_set<Renderer*> invalidRenderers;

    std::unordered_map<cocos2d::CCSpriteBatchNode*, BatchIndexCache> indexCaches;
    // Positive ownership cache. The first frame proves a live batch by scanning
    // descendants; later frames can take the O(1) gate and let drawBatch do the
    // single authoritative live-atlas validation.
    std::unordered_map<cocos2d::CCSpriteBatchNode*, Renderer*> ownedBatches;
    std::unordered_map<Shader*, AssistUniformLocations> uniformLocations;
    std::vector<cocos2d::CCSprite*> atlasSprites;
    std::vector<SpriteOwner> atlasOwners;
    std::vector<AtlasDrawRun> runs;
    std::vector<u16> indices;
    Renderer* activeRenderer = nullptr;
    cocos2d::CCSpriteBatchNode* activeBatch = nullptr;
};

static RegistryState& registry() {
    static RegistryState state;
    return state;
}

static bool isIdentityBatchTransform(cocos2d::CCSpriteBatchNode* batch) {
    if (!batch)
        return false;

    const auto transform = batch->nodeToParentTransform();
    constexpr float epsilon = 0.0001f;
    return std::fabs(transform.a - 1.f) <= epsilon &&
        std::fabs(transform.b) <= epsilon &&
        std::fabs(transform.c) <= epsilon &&
        std::fabs(transform.d - 1.f) <= epsilon &&
        std::fabs(transform.tx) <= epsilon &&
        std::fabs(transform.ty) <= epsilon;
}

static bool isExactGameplayBatch(Renderer* renderer, cocos2d::CCSpriteBatchNode* batch) {
    if (!renderer || !renderer->isEnabled() || !batch)
        return false;

    auto layer = renderer->getPlayLayer();
    if (!layer || !layer->m_batchNodes)
        return false;

    if (layer->m_batchNodes->indexOfObject(batch) == UINT_MAX)
        return false;

    if (!isIdentityBatchTransform(batch))
        return false;

    auto atlas = batch->getTextureAtlas();
    return atlas && atlas->getTotalQuads() > 0;
}

static void invalidateRenderer(Renderer* renderer, const char* reason) {
    if (!renderer)
        return;

    auto& state = registry();
    if (state.invalidRenderers.insert(renderer).second)
        log::error("Bismuth iOS atlas interleave disabled for this PlayLayer: {}", reason);
}

static void clearOwnedBatchCache(Renderer* renderer) {
    if (!renderer)
        return;
    auto& state = registry();
    for (auto it = state.ownedBatches.begin(); it != state.ownedBatches.end();) {
        if (it->second == renderer)
            it = state.ownedBatches.erase(it);
        else
            ++it;
    }
}

static bool rendererHasRegisteredOwners(Renderer* renderer) {
    if (!renderer)
        return false;

    auto& state = registry();
    for (const auto& [owner, registeredRenderer] : state.immediateRenderers) {
        if (owner && registeredRenderer == renderer)
            return true;
    }
    for (const auto& [owner, registeredRenderer] : state.deferredRenderers) {
        if (owner && registeredRenderer == renderer)
            return true;
    }
    return false;
}

static void releaseScratchIfUnused() {
    auto& state = registry();
    if (!state.immediateRenderers.empty() || !state.deferredRenderers.empty())
        return;

    for (auto& [batch, cache] : state.indexCaches) {
        if (cache.buffer)
            glDeleteBuffers(1, &cache.buffer);
    }
    state.indexCaches.clear();
    state.ownedBatches.clear();
    state.uniformLocations.clear();
    state.runs.clear();
    state.indices.clear();
    state.activeRenderer = nullptr;
    state.activeBatch = nullptr;
    state.atlasSprites.clear();
    state.atlasOwners.clear();
    state.invalidRenderers.clear();
}

static bool synchronizeDirtyAtlasWithStockDraw(
    cocos2d::CCTextureAtlas* atlas,
    usize stockSlot
) {
    if (!atlas)
        return false;
    if (!atlas->isDirty())
        return true;

    const usize totalQuads = static_cast<usize>(atlas->getTotalQuads());
    if (totalQuads == 0)
        return true;
    if (stockSlot >= totalQuads)
        return false;

    GLboolean previousColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    GLboolean previousDepthMask = GL_TRUE;
    GLint previousStencilMask = 0;
    GLint previousBackStencilMask = 0;

    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &previousStencilMask);
    glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &previousBackStencilMask);

    // CCTextureAtlas uploads its dirty VBO before issuing the draw. We only
    // need to force that upload and verify it completed; drawing the entire
    // ~10k-sprite atlas invisibly was pure duplicate GPU work. One known stock
    // quad is enough to trigger the exact same upload path.
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glStencilMask(0);

    atlas->drawNumberOfQuads(1, static_cast<unsigned int>(stockSlot));

    glColorMask(
        previousColorMask[0],
        previousColorMask[1],
        previousColorMask[2],
        previousColorMask[3]
    );
    glDepthMask(previousDepthMask);
    glStencilMaskSeparate(GL_FRONT, static_cast<u32>(previousStencilMask));
    glStencilMaskSeparate(GL_BACK, static_cast<u32>(previousBackStencilMask));

    return !atlas->isDirty();
}

static bool updateIndexCache(BatchIndexCache& cache, const std::vector<u16>& indices) {
    if (cache.buffer && cache.indices == indices)
        return true;
    if (!cache.buffer)
        glGenBuffers(1, &cache.buffer);
    if (!cache.buffer)
        return false;

    GLint previousBuffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, cache.buffer);
    (void)glGetError();
    glBufferData(GL_ARRAY_BUFFER, indices.size() * sizeof(u16), indices.data(), GL_DYNAMIC_DRAW);
    const auto error = glGetError();
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<u32>(previousBuffer));
    if (error != GL_NO_ERROR) {
        cache.indices.clear();
        return false;
    }
    cache.indices = indices;
    return true;
}

static SavedGLState captureGLState() {
    SavedGLState saved;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved.vao);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &saved.elementArrayBuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &saved.program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &saved.activeTexture);
    for (i32 unit = 0; unit < 3; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved.textures[unit]);
    }
    glActiveTexture(static_cast<GLenum>(saved.activeTexture));
    return saved;
}

static void restoreGLState(const SavedGLState& saved) {
    glBindVertexArray(static_cast<u32>(saved.vao));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<u32>(saved.elementArrayBuffer));
    glUseProgram(static_cast<u32>(saved.program));
    for (i32 unit = 0; unit < 3; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, static_cast<u32>(saved.textures[unit]));
    }
    glActiveTexture(static_cast<GLenum>(saved.activeTexture));
}

static AssistUniformLocations queryAssistUniformLocations(Shader* shader) {
    AssistUniformLocations locations;
    if (!shader)
        return locations;
    locations.mvp = static_cast<GLint>(shader->location("u_mvp"));
    locations.objectStateTexture = static_cast<GLint>(shader->location("u_objectStateTexture"));
    locations.objectStateTextureSize = static_cast<GLint>(shader->location("u_objectStateTextureSize"));
    locations.spriteStateTexture = static_cast<GLint>(shader->location("u_spriteStateTexture"));
    locations.spriteStateTextureSize = static_cast<GLint>(shader->location("u_spriteStateTextureSize"));
    locations.spriteSheetTexture = static_cast<GLint>(shader->location("u_spriteSheetTexture"));
    return locations;
}

static const AssistUniformLocations& getAssistUniformLocations(Shader* shader) {
    auto& state = registry();
    auto it = state.uniformLocations.find(shader);
    if (it != state.uniformLocations.end())
        return it->second;
    return state.uniformLocations.emplace(shader, queryAssistUniformLocations(shader)).first->second;
}

static bool cachedPlanSane(const BatchIndexCache& cache, usize totalQuads) {
    if (!cache.buffer || cache.owners.size() != totalQuads)
        return false;

    usize previousEnd = 0;
    for (const auto& run : cache.runs) {
        if (run.slotCount == 0 || run.firstSlot >= totalQuads ||
            run.slotCount > totalQuads - run.firstSlot)
            return false;
        if (run.firstSlot != previousEnd)
            return false;
        previousEnd = run.firstSlot + run.slotCount;

        if (!cache.owners[run.firstSlot].empty()) {
            if (run.firstIndex > cache.indices.size())
                return false;
            const usize maxSprites = (cache.indices.size() - run.firstIndex) / 6;
            if (run.slotCount > maxSprites)
                return false;
        }
    }
    return previousEnd == totalQuads;
}
} // namespace

void AtlasInterleaveRegistry::registerImmediate(AssistShadowBatch* owner) {
    if (!owner || !owner->stockBatch || owner->ownedSprites.empty())
        return;

    auto rendererRef = Renderer::get();
    auto renderer = rendererRef.data();
    if (!renderer)
        return;

    auto& state = registry();
    state.immediateRenderers[owner] = renderer;

    auto [batchIt, insertedBatch] = state.immediateByBatch.emplace(owner->stockBatch, owner);
    if (!insertedBatch && batchIt->second != owner) {
        invalidateRenderer(renderer, "multiple immediate GPU owners claimed one stock batch");
        return;
    }

    for (usize i = 0; i < owner->ownedSprites.size(); ++i) {
        auto sprite = owner->ownedSprites[i];
        if (!sprite || i * 4 + 3 > 65535) {
            invalidateRenderer(renderer, "invalid immediate GPU sprite vertex mapping");
            return;
        }

        SpriteOwner record {
            renderer,
            owner,
            nullptr,
            static_cast<u16>(i * 4)
        };
        auto [it, inserted] = state.spriteOwners.emplace(sprite, record);
        if (!inserted && !it->second.sameOwner(record)) {
            invalidateRenderer(renderer, "GPU sprite ownership collision");
            return;
        }
    }
}

void AtlasInterleaveRegistry::unregisterImmediate(AssistShadowBatch* owner) {
    if (!owner)
        return;

    auto& state = registry();
    Renderer* renderer = nullptr;
    if (auto it = state.immediateRenderers.find(owner); it != state.immediateRenderers.end()) {
        renderer = it->second;
        state.immediateRenderers.erase(it);
    }

    for (auto it = state.spriteOwners.begin(); it != state.spriteOwners.end();) {
        if (it->second.immediate == owner)
            it = state.spriteOwners.erase(it);
        else
            ++it;
    }

    for (auto it = state.immediateByBatch.begin(); it != state.immediateByBatch.end();) {
        if (it->second == owner)
            it = state.immediateByBatch.erase(it);
        else
            ++it;
    }

    if (renderer) {
        clearOwnedBatchCache(renderer);
        if (!rendererHasRegisteredOwners(renderer))
            state.invalidRenderers.erase(renderer);
    }
    releaseScratchIfUnused();
}

void AtlasInterleaveRegistry::registerDeferred(StandaloneAssistBatch* owner) {
    if (!owner || owner->rootAddressable || owner->ownedSprites.empty())
        return;

    auto rendererRef = Renderer::get();
    auto renderer = rendererRef.data();
    if (!renderer)
        return;

    auto& state = registry();
    state.deferredRenderers[owner] = renderer;

    for (usize i = 0; i < owner->ownedSprites.size(); ++i) {
        auto sprite = owner->ownedSprites[i];
        if (!sprite || i * 4 + 3 > 65535) {
            invalidateRenderer(renderer, "invalid deferred GPU sprite vertex mapping");
            return;
        }

        SpriteOwner record {
            renderer,
            nullptr,
            owner,
            static_cast<u16>(i * 4)
        };
        auto [it, inserted] = state.spriteOwners.emplace(sprite, record);
        if (!inserted && !it->second.sameOwner(record)) {
            invalidateRenderer(renderer, "deferred GPU sprite ownership collision");
            return;
        }
    }
}

void AtlasInterleaveRegistry::unregisterDeferred(StandaloneAssistBatch* owner) {
    if (!owner)
        return;

    auto& state = registry();
    Renderer* renderer = nullptr;
    if (auto it = state.deferredRenderers.find(owner); it != state.deferredRenderers.end()) {
        renderer = it->second;
        state.deferredRenderers.erase(it);
    }

    for (auto it = state.spriteOwners.begin(); it != state.spriteOwners.end();) {
        if (it->second.deferred == owner)
            it = state.spriteOwners.erase(it);
        else
            ++it;
    }

    if (renderer) {
        clearOwnedBatchCache(renderer);
        if (!rendererHasRegisteredOwners(renderer))
            state.invalidRenderers.erase(renderer);
    }
    releaseScratchIfUnused();
}

bool AtlasInterleaveRegistry::ownsBatch(
    Renderer* renderer,
    cocos2d::CCSpriteBatchNode* batch
) {
    if (!isExactGameplayBatch(renderer, batch))
        return false;

    auto& state = registry();
    if (state.invalidRenderers.contains(renderer))
        return false;

    if (auto cached = state.ownedBatches.find(batch);
        cached != state.ownedBatches.end() && cached->second == renderer)
        return true;

    auto descendants = batch->getDescendants();
    if (!descendants)
        return false;

    for (u32 i = 0; i < descendants->count(); ++i) {
        auto sprite = typeinfo_cast<cocos2d::CCSprite*>(descendants->objectAtIndex(i));
        if (!sprite || sprite->getBatchNode() != batch)
            continue;

        auto ownerIt = state.spriteOwners.find(sprite);
        if (ownerIt == state.spriteOwners.end())
            continue;
        const auto& record = ownerIt->second;
        // Registered storage can outlive its visible sprites. A stock-only
        // live batch must not enter the strict GPU-submit path and retry forever.
        if (record.renderer == renderer &&
            (record.deferred || (record.immediate && record.immediate->stockBatch == batch)) &&
            sprite->getParent() == batch && renderer->isGPUOwnedSprite(sprite) &&
            sprite->getTexture() && batch->getTexture() &&
            sprite->getTexture()->getName() == batch->getTexture()->getName()) {
            state.ownedBatches[batch] = renderer;
            return true;
        }
    }

    return false;
}

bool AtlasInterleaveRegistry::drawBatch(
    Renderer* renderer,
    cocos2d::CCSpriteBatchNode* batch
) {
    if (!isExactGameplayBatch(renderer, batch))
        return false;

    auto& state = registry();
    if (state.invalidRenderers.contains(renderer) || state.activeBatch)
        return false;

    auto atlas = batch->getTextureAtlas();
    auto texture = batch->getTexture();
    auto descendants = batch->getDescendants();
    if (!atlas || !texture || !texture->getName() || !descendants)
        return false;

    const usize totalQuads = static_cast<usize>(atlas->getTotalQuads());
    const usize descendantCount = static_cast<usize>(descendants->count());
    if (totalQuads == 0 || descendantCount != totalQuads ||
        totalQuads > MAX_REASONABLE_ATLAS_QUADS) {
        state.ownedBatches.erase(batch);
        return false;
    }

    state.atlasSprites.assign(totalQuads, nullptr);
    state.atlasOwners.assign(totalQuads, {});

    bool hasGPU = false;

    auto ownerReady = [&](const SpriteOwner& record) -> bool {
        if (record.renderer != renderer || record.empty())
            return false;

        if (record.immediate) {
            auto owner = record.immediate;
            auto rendererIt = state.immediateRenderers.find(owner);
            return rendererIt != state.immediateRenderers.end() &&
                rendererIt->second == renderer &&
                owner->stockBatch == batch &&
                owner->stats.ready && owner->isVisible() &&
                owner->resolvedState && owner->resolvedState->isGPUStateReady() &&
                owner->resolvedState->getObjectStateTexture() &&
                owner->resolvedState->getSpriteStateTexture() &&
                owner->shader && owner->vao && owner->indexBuffer;
        }

        auto owner = record.deferred;
        auto rendererIt = state.deferredRenderers.find(owner);
        return owner && rendererIt != state.deferredRenderers.end() &&
            rendererIt->second == renderer && !owner->rootAddressable &&
            owner->stats.ready && owner->isVisible() &&
            owner->resolvedState && owner->resolvedState->isGPUStateReady() &&
            owner->resolvedState->getObjectStateTexture() &&
            owner->resolvedState->getSpriteStateTexture() &&
            owner->shader && owner->vao && owner->indexBuffer;
    };

    for (u32 i = 0; i < descendants->count(); ++i) {
        auto sprite = typeinfo_cast<cocos2d::CCSprite*>(descendants->objectAtIndex(i));
        if (!sprite || sprite->getBatchNode() != batch)
            return false;

        const auto atlasIndex = sprite->getAtlasIndex();
        if (atlasIndex == CCSpriteIndexNotInitialized || atlasIndex >= totalQuads)
            return false;
        if (state.atlasSprites[atlasIndex] && state.atlasSprites[atlasIndex] != sprite)
            return false;
        state.atlasSprites[atlasIndex] = sprite;

        if (!renderer->isGPUOwnedSprite(sprite) || sprite->getParent() != batch)
            continue;

        auto spriteTexture = sprite->getTexture();
        if (!spriteTexture || spriteTexture->getName() != texture->getName())
            continue;

        auto recordIt = state.spriteOwners.find(sprite);
        if (recordIt == state.spriteOwners.end() || !ownerReady(recordIt->second))
            continue;

        state.atlasOwners[atlasIndex] = recordIt->second;
        hasGPU = true;
    }

    if (!hasGPU) {
        state.ownedBatches.erase(batch);
        return false;
    }
    state.ownedBatches[batch] = renderer;

    for (usize i = 0; i < totalQuads; ++i) {
        if (!state.atlasSprites[i])
            return false;
    }

    auto& cache = state.indexCaches[batch];
    const bool saneCache = cachedPlanSane(cache, totalQuads);
    const bool samePlan = saneCache &&
        std::equal(cache.owners.begin(), cache.owners.end(), state.atlasOwners.begin(),
            [](const auto& a, const auto& b) {
                return a.sameOwner(b) && a.baseVertex == b.baseVertex;
            });
    if (!samePlan) {
        buildAtlasDrawPlan(state.atlasOwners, state.runs, state.indices);
        if (!updateIndexCache(cache, state.indices)) {
            cache.owners.clear();
            cache.runs.clear();
            return false;
        }
        cache.owners = state.atlasOwners;
        cache.runs = state.runs;
    }

    state.activeRenderer = renderer;
    state.activeBatch = batch;
    if (auto children = batch->getChildren()) {
        for (auto child : CCArrayExt<cocos2d::CCNode*>(children)) {
            if (auto sprite = typeinfo_cast<cocos2d::CCSprite*>(child))
                sprite->updateTransform();
        }
    }
    state.activeBatch = nullptr;
    state.activeRenderer = nullptr;

    if (atlas->getTotalQuads() != totalQuads || descendants->count() != totalQuads)
        return false;
    for (usize i = 0; i < totalQuads; ++i) {
        auto sprite = typeinfo_cast<cocos2d::CCSprite*>(descendants->objectAtIndex(i));
        if (!sprite || sprite->getBatchNode() != batch)
            return false;
        const usize slot = sprite->getAtlasIndex();
        if (slot >= totalQuads || state.atlasSprites[slot] != sprite)
            return false;
    }

    bool hasStock = false;
    usize firstStockSlot = 0;
    for (const auto& run : cache.runs) {
        if (run.slotCount == 0 || run.firstSlot >= totalQuads ||
            run.slotCount > totalQuads - run.firstSlot) {
            cache.owners.clear();
            cache.runs.clear();
            cache.indices.clear();
            return false;
        }
        if (!hasStock && state.atlasOwners[run.firstSlot].empty()) {
            hasStock = true;
            firstStockSlot = run.firstSlot;
        }
    }
    if (hasStock && !synchronizeDirtyAtlasWithStockDraw(atlas, firstStockSlot))
        return false;

    auto drawDataFor = [&](const SpriteOwner& record) -> OwnerDrawData {
        if (record.immediate) {
            auto owner = record.immediate;
            return {
                owner->resolvedState,
                owner->shader,
                owner->vao,
                owner->indexBuffer->getId(),
                &owner->stats.drawCallsLastFrame,
                &owner->stats.indicesLastFrame
            };
        }

        auto owner = record.deferred;
        return {
            owner->resolvedState,
            owner->shader,
            owner->vao,
            owner->indexBuffer->getId(),
            &owner->stats.drawCallsLastFrame,
            &owner->stats.indicesLastFrame
        };
    };

    // glGet* is a synchronization point on mobile drivers. The old path queried
    // VAO/program/three textures at every stock -> GPU transition, which became
    // thousands of driver queries per frame on heavily interleaved levels.
    //
    // Capture the entry state once. After the first real stock run, capture the
    // stable state that Cocos established for this exact atlas once more. Raw
    // Bismuth bindings do not update Cocos' cache, so restoring that learned
    // state keeps real GL and Cocos' cached state synchronized for every later
    // boundary without querying the driver again.
    const SavedGLState entryState = captureGLState();
    SavedGLState stockState = entryState;
    bool learnedStockState = false;
    kmMat4 matrixP;
    kmMat4 matrixMV;
    kmMat4 matrixMVP;
    kmGLGetMatrix(KM_GL_PROJECTION, &matrixP);
    kmGLGetMatrix(KM_GL_MODELVIEW, &matrixMV);
    kmMat4Multiply(&matrixMVP, &matrixP, &matrixMV);

    bool gpuStateActive = false;
    Shader* activeShader = nullptr;
    ResolvedStateLayer* activeResolvedState = nullptr;
    u32 activeVAO = 0;
    u32 activeOwnerIndexBuffer = 0;

    Shader* configuredShader = nullptr;
    ResolvedStateLayer* configuredResolvedState = nullptr;
    AssistUniformLocations configuredLocations;

    auto restoreActiveOwnerVAO = [&]() {
        if (!activeVAO || !activeOwnerIndexBuffer)
            return;
        glBindVertexArray(activeVAO);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, activeOwnerIndexBuffer);
        activeVAO = 0;
        activeOwnerIndexBuffer = 0;
    };

    auto restoreStockState = [&]() {
        restoreActiveOwnerVAO();
        restoreGLState(stockState);
        gpuStateActive = false;
        activeShader = nullptr;
        activeResolvedState = nullptr;
    };

    for (const auto& run : cache.runs) {
        if (run.slotCount == 0 || run.firstSlot >= totalQuads ||
            run.slotCount > totalQuads - run.firstSlot) {
            if (gpuStateActive)
                restoreStockState();
            return false;
        }

        const auto& ownerRecord = state.atlasOwners[run.firstSlot];
        if (ownerRecord.empty()) {
            if (gpuStateActive)
                restoreStockState();
            atlas->drawNumberOfQuads(static_cast<unsigned int>(run.slotCount),
                static_cast<unsigned int>(run.firstSlot));
            if (!learnedStockState) {
                stockState = captureGLState();
                learnedStockState = true;
            }
            continue;
        }

        if (run.firstIndex > cache.indices.size() ||
            run.slotCount > (cache.indices.size() - run.firstIndex) / 6) {
            if (gpuStateActive)
                restoreStockState();
            return false;
        }

        const auto owner = drawDataFor(ownerRecord);
        if (!owner.resolvedState || !owner.shader || !owner.vao || !owner.ownerIndexBuffer) {
            if (gpuStateActive)
                restoreStockState();
            return false;
        }
        auto objectStateTexture = owner.resolvedState->getObjectStateTexture();
        auto spriteStateTexture = owner.resolvedState->getSpriteStateTexture();
        if (!objectStateTexture || !spriteStateTexture) {
            if (gpuStateActive)
                restoreStockState();
            return false;
        }

        // Stock runs may switch program/texture state. Re-enter the assist state
        // only at GPU block boundaries; consecutive GPU owners stay in one state
        // scope and only change VAO when their persistent geometry differs.
        if (!gpuStateActive || activeShader != owner.shader) {
            owner.shader->use();
            activeShader = owner.shader;
        }

        if (configuredShader != owner.shader || configuredResolvedState != owner.resolvedState) {
            configuredLocations = getAssistUniformLocations(owner.shader);
            glUniformMatrix4fv(configuredLocations.mvp, 1, GL_FALSE, matrixMVP.mat);
            glUniform1i(configuredLocations.objectStateTexture, 1);
            const auto objectTextureSize = objectStateTexture->getSize();
            glUniform2f(configuredLocations.objectStateTextureSize,
                objectTextureSize.x, objectTextureSize.y);
            glUniform1i(configuredLocations.spriteStateTexture, 2);
            const auto spriteTextureSize = spriteStateTexture->getSize();
            glUniform2f(configuredLocations.spriteStateTextureSize,
                spriteTextureSize.x, spriteTextureSize.y);
            glUniform1i(configuredLocations.spriteSheetTexture, 0);
            configuredShader = owner.shader;
            configuredResolvedState = owner.resolvedState;
        }

        if (!gpuStateActive || activeResolvedState != owner.resolvedState) {
            objectStateTexture->bind(1);
            spriteStateTexture->bind(2);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture->getName());
            activeResolvedState = owner.resolvedState;
        }

        if (activeVAO != owner.vao) {
            restoreActiveOwnerVAO();
            glBindVertexArray(owner.vao);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cache.buffer);
            activeVAO = owner.vao;
            activeOwnerIndexBuffer = owner.ownerIndexBuffer;
        }

        gpuStateActive = true;
        const usize drawIndices = run.slotCount * 6;
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(drawIndices), GL_UNSIGNED_SHORT,
            reinterpret_cast<void*>(run.firstIndex * sizeof(u16)));
        if (owner.drawCalls)
            ++(*owner.drawCalls);
        if (owner.indexCount)
            *owner.indexCount += drawIndices;
    }

    if (gpuStateActive)
        restoreStockState();
    return true;
}

bool AtlasInterleaveRegistry::shouldSkipTransform(
    Renderer* renderer, cocos2d::CCSprite* sprite
) {
    auto& state = registry();
    if (!sprite || state.activeRenderer != renderer || !state.activeBatch ||
        sprite->getBatchNode() != state.activeBatch)
        return false;
    const usize slot = sprite->getAtlasIndex();
    return slot < state.atlasOwners.size() && state.atlasSprites[slot] == sprite &&
        !state.atlasOwners[slot].empty();
}

void AtlasInterleaveRegistry::beginFrame() {
    auto& state = registry();
    for (const auto& [owner, renderer] : state.immediateRenderers) {
        owner->stats.drawCallsLastFrame = 0;
        owner->stats.indicesLastFrame = 0;
    }
    for (const auto& [owner, renderer] : state.deferredRenderers) {
        owner->stats.drawCallsLastFrame = 0;
        owner->stats.indicesLastFrame = 0;
    }
}

bool Renderer::isGPUInterleavedBatch(cocos2d::CCSpriteBatchNode* batch) const {
    if (!enabled || !batch)
        return false;
    return AtlasInterleaveRegistry::ownsBatch(const_cast<Renderer*>(this), batch);
}

bool Renderer::drawGPUInterleavedBatch(cocos2d::CCSpriteBatchNode* batch) {
    if (!enabled || !batch)
        return false;
    return AtlasInterleaveRegistry::drawBatch(this, batch);
}

#endif
