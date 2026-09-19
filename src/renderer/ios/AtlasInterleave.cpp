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
            sprite->getTexture()->getName() == batch->getTexture()->getName())
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

        if (sprite->getParent() != batch)
            continue;

        auto spriteTexture = sprite->getTexture();
        if (!spriteTexture || spriteTexture->getName() != texture->getName())
            continue;

        // Use the registry record we already need instead of routing through
        // Renderer::isGPUOwnedSprite(), which performs another ownership hash
        // lookup. The owner record plus live batch/parent checks are the exact
        // atlas ownership proof here; canDrawSprite() still validates live UV/
        // frame safety once per render epoch.
        auto recordIt = state.spriteOwners.find(sprite);
        if (recordIt == state.spriteOwners.end() || !ownerReady(recordIt->second))
            continue;

        auto resolved = recordIt->second.immediate
            ? recordIt->second.immediate->resolvedState
            : recordIt->second.deferred->resolvedState;
        if (!resolved || !resolved->canDrawSprite(sprite))
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

    auto& cache = state.indexCaches[batch];
    const bool samePlan = cache.buffer && cache.owners.size() == state.atlasOwners.size() &&
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

    // Only stock-owned sprites need Cocos atlas transform expansion. Calling
    // updateTransform() on every GPU-owned decoration was pure per-frame CPU
    // overhead and also dirtied the native atlas, which then forced a hidden
    // full-atlas stock draw before the real interleaved draw.
    //
    // This is intentionally NOT a static-decoration cache. Live GPU state still
    // updates every frame; we are only stopping duplicate native render prep for
    // sprites whose transform is already consumed by the Bismuth shader.
    state.activeRenderer = renderer;
    state.activeBatch = batch;
    for (usize slot = 0; slot < totalQuads; ++slot) {
        if (!state.atlasOwners[slot].empty())
            continue;
        auto sprite = state.atlasSprites[slot];
        if (sprite)
            sprite->updateTransform();
    }
    state.activeBatch = nullptr;
    state.activeRenderer = nullptr;

    // updateTransform() does not reorder atlas membership. The previous second
    // O(N) descendant walk revalidated ~10k entries every frame after we had
    // already built the exact atlasIndex -> sprite map above.
    if (atlas->getTotalQuads() != totalQuads || descendants->count() != totalQuads)
        return false;

    // Do not perform an invisible full-atlas "warmup" draw here. The first real
    // stock run will let CCTextureAtlas upload any dirty stock quads itself.
    // When every run is GPU-owned, the native atlas does not need a draw/upload
    // at all. This removes the worst duplicate work on decoration-heavy levels.

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

    // Cocos changes its cached VAO/texture bindings during stock atlas draws.
    // Capture the actual state at each stock -> GPU boundary, not once for the
    // entire batch. Restoring an older snapshot desynchronizes real GL from
    // ccGLBindVAO/ccGLBindTexture2D, so a later stock run can read another atlas's
    // vertices. Dirty-atlas warmup used to hide this until a later clean frame.
    SavedGLState stockState;
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
        const auto& ownerRecord = state.atlasOwners[run.firstSlot];
        if (ownerRecord.empty()) {
            if (gpuStateActive)
                restoreStockState();
            atlas->drawNumberOfQuads(static_cast<unsigned int>(run.slotCount),
                static_cast<unsigned int>(run.firstSlot));
            continue;
        }

        const auto owner = drawDataFor(ownerRecord);
        auto objectStateTexture = owner.resolvedState->getObjectStateTexture();
        auto spriteStateTexture = owner.resolvedState->getSpriteStateTexture();

        if (!gpuStateActive)
            stockState = captureGLState();

        // Stock runs may switch program/texture state. Re-enter the assist state
        // only at GPU block boundaries; consecutive GPU owners stay in one state
        // scope and only change VAO when their persistent geometry differs.
        if (!gpuStateActive || activeShader != owner.shader) {
            owner.shader->use();
            activeShader = owner.shader;
        }

        if (configuredShader != owner.shader || configuredResolvedState != owner.resolvedState) {
            configuredLocations = queryAssistUniformLocations(owner.shader);
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
