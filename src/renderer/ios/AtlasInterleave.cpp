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
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {
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
    usize* drawCalls = nullptr;
    usize* indexCount = nullptr;
};

struct BatchIndexCache {
    u32 buffer = 0;
    std::vector<u16> indices;
};

struct RegistryState {
    std::unordered_map<cocos2d::CCSprite*, SpriteOwner> spriteOwners;
    std::unordered_map<cocos2d::CCSpriteBatchNode*, AssistShadowBatch*> immediateByBatch;
    std::unordered_map<AssistShadowBatch*, Renderer*> immediateRenderers;
    std::unordered_map<StandaloneAssistBatch*, Renderer*> deferredRenderers;
    std::unordered_set<Renderer*> invalidRenderers;

    std::unordered_map<cocos2d::CCSpriteBatchNode*, BatchIndexCache> indexCaches;
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
    state.runs.clear();
    state.indices.clear();
    state.activeRenderer = nullptr;
    state.activeBatch = nullptr;
    state.atlasSprites.clear();
    state.atlasOwners.clear();
    state.invalidRenderers.clear();
}

static bool synchronizeDirtyAtlasWithStockDraw(cocos2d::CCTextureAtlas* atlas) {
    if (!atlas)
        return false;
    if (!atlas->isDirty())
        return true;

    const usize totalQuads = static_cast<usize>(atlas->getTotalQuads());
    if (totalQuads == 0)
        return true;

    GLboolean previousColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    GLboolean previousDepthMask = GL_TRUE;
    GLint previousStencilMask = 0;
    GLint previousBackStencilMask = 0;

    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &previousStencilMask);
    glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &previousBackStencilMask);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glStencilMask(0);

    atlas->drawNumberOfQuads(static_cast<unsigned int>(totalQuads), 0);

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

static void drawGPURun(
    cocos2d::CCSpriteBatchNode* batch,
    const OwnerDrawData& owner,
    const AtlasDrawRun& run,
    u32 indexBuffer
) {
    auto objectStateTexture = owner.resolvedState->getObjectStateTexture();
    auto spriteStateTexture = owner.resolvedState->getSpriteStateTexture();
    kmMat4 matrixP;
    kmMat4 matrixMV;
    kmMat4 matrixMVP;
    kmGLGetMatrix(KM_GL_PROJECTION, &matrixP);
    kmGLGetMatrix(KM_GL_MODELVIEW, &matrixMV);
    kmMat4Multiply(&matrixMVP, &matrixP, &matrixMV);

    GLint previousVAO = 0;
    GLint previousProgram = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTextures[3] = {0, 0, 0};
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVAO);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    for (i32 unit = 0; unit < 3; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTextures[unit]);
    }

    owner.shader->use();
    owner.shader->setMatrix4("u_mvp", matrixMVP.mat);
    objectStateTexture->bind(1);
    owner.shader->setInt("u_objectStateTexture", 1);
    owner.shader->setVec2("u_objectStateTextureSize", objectStateTexture->getSize());
    spriteStateTexture->bind(2);
    owner.shader->setInt("u_spriteStateTexture", 2);
    owner.shader->setVec2("u_spriteStateTextureSize", spriteStateTexture->getSize());

    glBindVertexArray(owner.vao);
    GLint previousOwnerIndices = 0;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &previousOwnerIndices);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
    owner.shader->setTexture("u_spriteSheetTexture", 0, batch->getTexture()->getName());
    const usize drawIndices = run.slotCount * 6;
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(drawIndices), GL_UNSIGNED_SHORT,
        reinterpret_cast<void*>(run.firstIndex * sizeof(u16)));
    if (owner.drawCalls)
        ++(*owner.drawCalls);
    if (owner.indexCount)
        *owner.indexCount += drawIndices;

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<u32>(previousOwnerIndices));
    glBindVertexArray(static_cast<u32>(previousVAO));
    glUseProgram(static_cast<u32>(previousProgram));
    for (i32 unit = 0; unit < 3; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, static_cast<u32>(previousTextures[unit]));
    }
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
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

    if (renderer && !rendererHasRegisteredOwners(renderer))
        state.invalidRenderers.erase(renderer);
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

    if (renderer && !rendererHasRegisteredOwners(renderer))
        state.invalidRenderers.erase(renderer);
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

    if (auto it = state.immediateByBatch.find(batch); it != state.immediateByBatch.end()) {
        auto owner = it->second;
        auto rendererIt = state.immediateRenderers.find(owner);
        if (owner && rendererIt != state.immediateRenderers.end() && rendererIt->second == renderer)
            return true;
    }

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
        if (record.renderer == renderer && record.deferred)
            return true;
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
    if (totalQuads == 0 || descendants->count() != totalQuads)
        return false;

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

    if (!hasGPU)
        return false;

    for (usize i = 0; i < totalQuads; ++i) {
        if (!state.atlasSprites[i])
            return false;
    }

    buildAtlasDrawPlan(state.atlasOwners, state.runs, state.indices);
    auto& cache = state.indexCaches[batch];
    if (!updateIndexCache(cache, state.indices))
        return false;

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
    for (const auto& run : state.runs)
        hasStock |= state.atlasOwners[run.firstSlot].empty();
    if (hasStock && !synchronizeDirtyAtlasWithStockDraw(atlas))
        return false;

    auto drawDataFor = [&](const SpriteOwner& record) -> OwnerDrawData {
        if (record.immediate) {
            auto owner = record.immediate;
            return {
                owner->resolvedState,
                owner->shader,
                owner->vao,
                &owner->stats.drawCallsLastFrame,
                &owner->stats.indicesLastFrame
            };
        }

        auto owner = record.deferred;
        return {
            owner->resolvedState,
            owner->shader,
            owner->vao,
            &owner->stats.drawCallsLastFrame,
            &owner->stats.indicesLastFrame
        };
    };

    for (const auto& run : state.runs) {
        const auto& owner = state.atlasOwners[run.firstSlot];
        if (owner.empty()) {
            atlas->drawNumberOfQuads(static_cast<unsigned int>(run.slotCount),
                static_cast<unsigned int>(run.firstSlot));
        } else {
            drawGPURun(batch, drawDataFor(owner), run, cache.buffer);
        }
    }
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