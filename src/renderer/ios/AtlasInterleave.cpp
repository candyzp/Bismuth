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

// iPhone-class tile GPUs are happiest when Bismuth does not try to own every
// decoration in a pathological scene. Keep headroom below the ~4k wall seen in
// dense levels, and only move useful contiguous work to the GPU. The remaining
// sprites stay on stock Cocos in the same atlas draw, so CPU + GPU cooperate.
constexpr usize HYBRID_GPU_SPRITE_BUDGET = 3072;
constexpr usize HYBRID_MIN_GPU_RUN = 48;
constexpr usize HYBRID_SMALL_RUN_GRACE = 8;
constexpr usize HYBRID_MAX_GPU_RUNS_PER_BATCH = 8;
constexpr usize HYBRID_MAX_GPU_RUNS_PER_FRAME = 24;

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
    LiveGeometry* geometry = nullptr;
    u32 vertexBuffer = 0;
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

    const char* lastFailureReason = "none";
    int lastFailureSlot = -1;
    u32 lastFailureAtlasSize = 0;
    bool lastFailureCanUseStock = true;

    usize gpuSpritesUsedThisFrame = 0;
    usize gpuDrawRunsUsedThisFrame = 0;
};

static RegistryState& registry() {
    static RegistryState state;
    return state;
}

static void drainGLErrors() {
    while (glGetError() != GL_NO_ERROR) {}
}

static bool consumeGLErrors() {
    bool clean = true;
    while (glGetError() != GL_NO_ERROR)
        clean = false;
    return clean;
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
    state.lastFailureCanUseStock = true;
    state.gpuSpritesUsedThisFrame = 0;
    state.gpuDrawRunsUsedThisFrame = 0;
}

static bool uploadDirtyAtlas(cocos2d::CCTextureAtlas* atlas) {
    if (!atlas)
        return false;
    if (!atlas->isDirty())
        return true;

    const usize totalQuads = static_cast<usize>(atlas->getTotalQuads());
    if (totalQuads == 0 || totalQuads > MAX_REASONABLE_ATLAS_QUADS)
        return totalQuads == 0;

    // Synchronize the existing VBO without submitting every quad invisibly.
    // Starting at zero also avoids Cocos' partial dirty-draw (n-start) underflow
    // and mapped-buffer memcpy. This is an upload, not a second render pass.
    if (totalQuads > atlas->getCapacity() || !atlas->m_pBuffersVBO[0])
        return false;
    const auto quads = atlas->getQuads();
    if (!quads)
        return false;
    GLint previousBuffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, atlas->m_pBuffersVBO[0]);
    drainGLErrors();
    glBufferData(GL_ARRAY_BUFFER, totalQuads * sizeof(*quads), quads, GL_DYNAMIC_DRAW);
    const bool uploadOK = consumeGLErrors();
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousBuffer));
    if (!uploadOK)
        return false;
    atlas->setDirty(false);
    return true;
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
    drainGLErrors();
    glBufferData(GL_ARRAY_BUFFER, indices.size() * sizeof(u16), indices.data(), GL_DYNAMIC_DRAW);
    const bool uploadOK = consumeGLErrors();
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<u32>(previousBuffer));
    if (!uploadOK) {
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

bool AtlasInterleaveRegistry::ownsBatch(Renderer* renderer, cocos2d::CCSpriteBatchNode* batch) {
    if (!isExactGameplayBatch(renderer, batch))
        return false;

    auto& state = registry();
    if (auto cached = state.ownedBatches.find(batch);
        cached != state.ownedBatches.end() && cached->second == renderer)
        return true;

    auto descendants = batch->getDescendants();
    if (!descendants)
        return false;

    // Strict ownership is registration-based. Once Bismuth has claimed a live
    // sprite in this atlas, a transient readiness problem must not silently send
    // the batch back through stock Cocos. The resolved-state/live-geometry fixes
    // below are responsible for keeping those claims drawable.
    for (u32 i = 0; i < descendants->count(); ++i) {
        auto sprite = typeinfo_cast<cocos2d::CCSprite*>(descendants->objectAtIndex(i));
        if (!sprite || sprite->getBatchNode() != batch)
            continue;

        const auto owner = state.spriteOwners.find(sprite);
        if (owner != state.spriteOwners.end() && owner->second.renderer == renderer) {
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
    auto& state = registry();
    bool submittedAny = false;
    auto fail = [&](const char* reason, int slot = -1, u32 atlasSize = 0) -> bool {
        state.lastFailureReason = reason;
        state.lastFailureSlot = slot;
        state.lastFailureAtlasSize = atlasSize;
        // A complete stock redraw is only safe before this custom pass has
        // emitted anything. This makes structural/readiness failures invisible
        // instead of turning them into a one-frame atlas flash.
        state.lastFailureCanUseStock = !submittedAny;
        return false;
    };

    if (!isExactGameplayBatch(renderer, batch))
        return fail("batch-not-exact");

    if (state.invalidRenderers.contains(renderer) || state.activeBatch)
        return fail(state.activeBatch ? "reentrant-draw" : "renderer-invalid");

    auto atlas = batch->getTextureAtlas();
    auto texture = batch->getTexture();
    auto descendants = batch->getDescendants();
    if (!atlas || !texture || !texture->getName() || !descendants)
        return fail("missing-atlas-state");

    const usize totalQuads = static_cast<usize>(atlas->getTotalQuads());
    const usize descendantCount = static_cast<usize>(descendants->count());
    if (totalQuads == 0 || descendantCount != totalQuads ||
        totalQuads > MAX_REASONABLE_ATLAS_QUADS) {
        state.ownedBatches.erase(batch);
        return fail("atlas-count-mismatch", -1, static_cast<u32>(totalQuads));
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
                owner->shader && owner->vao && owner->indexBuffer && owner->vertexBuffer;
        }

        auto owner = record.deferred;
        auto rendererIt = state.deferredRenderers.find(owner);
        return owner && rendererIt != state.deferredRenderers.end() &&
            rendererIt->second == renderer && !owner->rootAddressable &&
            owner->stats.ready && owner->isVisible() &&
            owner->resolvedState && owner->resolvedState->isGPUStateReady() &&
            owner->resolvedState->getObjectStateTexture() &&
            owner->resolvedState->getSpriteStateTexture() &&
            owner->shader && owner->vao && owner->indexBuffer && owner->vertexBuffer;
    };

    for (u32 i = 0; i < descendants->count(); ++i) {
        auto sprite = typeinfo_cast<cocos2d::CCSprite*>(descendants->objectAtIndex(i));
        if (!sprite || sprite->getBatchNode() != batch)
            return fail("descendant-not-in-batch", static_cast<int>(i), static_cast<u32>(totalQuads));

        const auto atlasIndex = sprite->getAtlasIndex();
        if (atlasIndex == CCSpriteIndexNotInitialized || atlasIndex >= totalQuads)
            return fail("bad-atlas-index", static_cast<int>(i), static_cast<u32>(totalQuads));
        if (state.atlasSprites[atlasIndex] && state.atlasSprites[atlasIndex] != sprite)
            return fail("duplicate-atlas-slot", static_cast<int>(atlasIndex), static_cast<u32>(totalQuads));
        state.atlasSprites[atlasIndex] = sprite;

        auto recordIt = state.spriteOwners.find(sprite);
        if (recordIt == state.spriteOwners.end() || recordIt->second.renderer != renderer)
            continue;
        // Hybrid ownership is per frame, not a permanent all-or-nothing claim.
        // If a registered sprite is temporarily not ready, has changed texture,
        // or cannot refresh its live geometry, leave just that sprite on stock
        // Cocos for this frame. Its neighbors can still be GPU drawn.
        if (!renderer->isGPUOwnedSprite(sprite) || !ownerReady(recordIt->second))
            continue;

        auto spriteTexture = sprite->getTexture();
        if (!spriteTexture || spriteTexture->getName() != texture->getName())
            continue;

        const auto& record = recordIt->second;
        auto& geometry = record.immediate ? record.immediate->liveGeometry : record.deferred->liveGeometry;
        // Eligibility is cheap. Do not rebuild live geometry yet: the hybrid
        // scheduler may put this sprite on the CPU anyway. Refreshing thousands
        // of candidates before selection caused the assist-time frame spikes.
        if (!geometry.canUseBatch(record.baseVertex / 4, batch))
            continue;

        state.atlasOwners[atlasIndex] = record;
        hasGPU = true;
    }

    const usize frameSpritesBeforeBatch = state.gpuSpritesUsedThisFrame;
    const usize frameRunsBeforeBatch = state.gpuDrawRunsUsedThisFrame;

    // A GPU sprite only helps when enough neighbors can be submitted with it.
    // The previous sprite-only budget still allowed pathological levels to make
    // 100+ tiny GL submissions per frame. On iOS those state switches cost more
    // than Cocos' CPU transform work. Rank contiguous owner runs by size and only
    // keep the profitable ones, with hard per-batch and per-frame call ceilings.
    if (hasGPU) {
        struct CandidateRun {
            usize start = 0;
            usize count = 0;
        };

        std::vector<CandidateRun> candidateRuns;
        for (usize start = 0; start < totalQuads;) {
            if (state.atlasOwners[start].empty()) {
                ++start;
                continue;
            }

            const auto owner = state.atlasOwners[start];
            usize end = start + 1;
            while (end < totalQuads && state.atlasOwners[end].sameOwner(owner))
                ++end;

            candidateRuns.push_back({ start, end - start });
            start = end;
        }

        // Keep atlas order stable. Re-sorting by run size made the chosen GPU
        // islands jump around as objects entered/left visibility, forcing index
        // cache uploads and producing periodic assist spikes.
        const usize spriteBudgetLeft =
            state.gpuSpritesUsedThisFrame < HYBRID_GPU_SPRITE_BUDGET
                ? HYBRID_GPU_SPRITE_BUDGET - state.gpuSpritesUsedThisFrame
                : 0;
        const usize frameRunBudgetLeft =
            state.gpuDrawRunsUsedThisFrame < HYBRID_MAX_GPU_RUNS_PER_FRAME
                ? HYBRID_MAX_GPU_RUNS_PER_FRAME - state.gpuDrawRunsUsedThisFrame
                : 0;

        usize remainingSprites = spriteBudgetLeft;
        usize remainingRuns = std::min(HYBRID_MAX_GPU_RUNS_PER_BATCH, frameRunBudgetLeft);
        usize smallRunGraceLeft =
            state.gpuDrawRunsUsedThisFrame < HYBRID_SMALL_RUN_GRACE
                ? HYBRID_SMALL_RUN_GRACE - state.gpuDrawRunsUsedThisFrame
                : 0;
        usize keptSprites = 0;
        usize keptRuns = 0;
        std::vector<bool> selected(totalQuads, false);

        for (const auto& run : candidateRuns) {
            if (!remainingRuns || !remainingSprites)
                break;

            const bool smallRun = run.count < HYBRID_MIN_GPU_RUN;
            // Do not abort the atlas just because an early tiny island exhausted
            // the small-run grace. Larger profitable runs may still exist later
            // in atlas order, especially with distributed long-level ownership.
            if (smallRun && !smallRunGraceLeft)
                continue;

            const usize keep = std::min(run.count, remainingSprites);
            if (!keep)
                continue;
            if (smallRun)
                --smallRunGraceLeft;
            else if (keep < HYBRID_MIN_GPU_RUN)
                continue;

            for (usize slot = run.start; slot < run.start + keep; ++slot)
                selected[slot] = true;

            keptSprites += keep;
            remainingSprites -= keep;
            --remainingRuns;
            ++keptRuns;
        }

        for (usize slot = 0; slot < totalQuads; ++slot) {
            if (!state.atlasOwners[slot].empty() && !selected[slot])
                state.atlasOwners[slot] = {};
        }

        state.gpuSpritesUsedThisFrame += keptSprites;
        state.gpuDrawRunsUsedThisFrame += keptRuns;
        hasGPU = keptSprites != 0;
    }

    // Only now touch live geometry for sprites that survived hybrid selection.
    // This turns the expensive refresh from "all eligible sprites" into "actual
    // GPU work" and keeps CPU-only frames genuinely cheap.
    if (hasGPU) {
        for (usize slot = 0; slot < totalQuads; ++slot) {
            const auto record = state.atlasOwners[slot];
            if (record.empty())
                continue;
            auto& geometry = record.immediate
                ? record.immediate->liveGeometry
                : record.deferred->liveGeometry;
            if (!geometry.refresh(record.baseVertex / 4))
                state.atlasOwners[slot] = {};
        }

        usize actualSprites = 0;
        usize actualRuns = 0;
        for (usize slot = 0; slot < totalQuads; ++slot) {
            const auto& owner = state.atlasOwners[slot];
            if (owner.empty())
                continue;
            ++actualSprites;
            if (slot == 0 || state.atlasOwners[slot - 1].empty() ||
                !owner.sameOwner(state.atlasOwners[slot - 1])) {
                ++actualRuns;
            }
        }
        state.gpuSpritesUsedThisFrame = frameSpritesBeforeBatch + actualSprites;
        state.gpuDrawRunsUsedThisFrame = frameRunsBeforeBatch + actualRuns;
        hasGPU = actualSprites != 0;
    }

    // Returning false here is intentional: the batch-node hook will execute the
    // normal stock draw because no custom submission has happened yet. This is
    // the CPU half of the hybrid scheduler, not an emergency double-render.
    if (!hasGPU) {
        state.ownedBatches[batch] = renderer;
        return fail("hybrid-cpu-only", -1, static_cast<u32>(totalQuads));
    }
    state.ownedBatches[batch] = renderer;

    // Resolved-state capture/upload is also delayed until we have proved this
    // frame will submit object GPU geometry. Idle/CPU-only frames no longer pay
    // the state-texture update cost.
    renderer->prepareGPUFrame();

    for (usize i = 0; i < totalQuads; ++i) {
        if (!state.atlasSprites[i])
            return fail("missing-atlas-slot", static_cast<int>(i), static_cast<u32>(totalQuads));
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
            return fail("index-cache-upload", -1, static_cast<u32>(totalQuads));
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
        return fail("atlas-mutated-after-transform", -1, static_cast<u32>(totalQuads));
    for (usize i = 0; i < totalQuads; ++i) {
        auto sprite = typeinfo_cast<cocos2d::CCSprite*>(descendants->objectAtIndex(i));
        if (!sprite || sprite->getBatchNode() != batch)
            return fail("post-transform-batch-change", static_cast<int>(i), static_cast<u32>(totalQuads));
        const usize slot = sprite->getAtlasIndex();
        if (slot >= totalQuads || state.atlasSprites[slot] != sprite)
            return fail("post-transform-order-change", static_cast<int>(slot), static_cast<u32>(totalQuads));
    }

    bool hasStock = false;
    for (const auto& run : cache.runs) {
        if (run.slotCount == 0 || run.firstSlot >= totalQuads ||
            run.slotCount > totalQuads - run.firstSlot) {
            cache.owners.clear();
            cache.runs.clear();
            cache.indices.clear();
            return fail("invalid-draw-plan", static_cast<int>(run.firstSlot), static_cast<u32>(totalQuads));
        }
        if (state.atlasOwners[run.firstSlot].empty())
            hasStock = true;
    }
    if (hasStock && !uploadDirtyAtlas(atlas))
        return fail("dirty-atlas-upload", -1, static_cast<u32>(totalQuads));

    auto drawDataFor = [&](const SpriteOwner& record) -> OwnerDrawData {
        if (record.immediate) {
            auto owner = record.immediate;
            return {
                owner->resolvedState,
                owner->shader,
                owner->vao,
                owner->indexBuffer->getId(),
                &owner->liveGeometry, owner->vertexBuffer->getId(),
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
            &owner->liveGeometry, owner->vertexBuffer->getId(),
            &owner->stats.drawCallsLastFrame,
            &owner->stats.indicesLastFrame
        };
    };

    // Preflight every GPU run before the first stock/GPU submission. Any upload
    // or state-readiness problem can therefore recover with one clean stock draw
    // without duplicating a prefix that was already rendered.
    for (const auto& run : cache.runs) {
        if (run.slotCount == 0 || run.firstSlot >= totalQuads ||
            run.slotCount > totalQuads - run.firstSlot)
            return fail("preflight-run-range", static_cast<int>(run.firstSlot), static_cast<u32>(totalQuads));

        const auto& ownerRecord = state.atlasOwners[run.firstSlot];
        if (ownerRecord.empty())
            continue;

        if (run.firstIndex > cache.indices.size() ||
            run.slotCount > (cache.indices.size() - run.firstIndex) / 6)
            return fail("preflight-index-range", static_cast<int>(run.firstSlot), static_cast<u32>(totalQuads));

        const auto owner = drawDataFor(ownerRecord);
        if (!owner.resolvedState || !owner.shader || !owner.vao || !owner.ownerIndexBuffer ||
            !owner.geometry || !owner.vertexBuffer)
            return fail("preflight-owner-data", static_cast<int>(run.firstSlot), static_cast<u32>(totalQuads));

        if (!owner.geometry->flush(owner.vertexBuffer))
            return fail("preflight-geometry-upload", static_cast<int>(run.firstSlot), static_cast<u32>(totalQuads));

        if (!owner.resolvedState->getObjectStateTexture() ||
            !owner.resolvedState->getSpriteStateTexture())
            return fail("preflight-state-texture", static_cast<int>(run.firstSlot), static_cast<u32>(totalQuads));
    }

    // glGet* is a synchronization point on mobile drivers. The old path queried
    // VAO/program/three textures at every stock -> GPU transition, which became
    // thousands of driver queries per frame on heavily interleaved levels.
    //
    // Capture the entry state once. After the first real stock run, capture the
    // stable state that Cocos established for this exact atlas once more. Raw
    // Bismuth bindings do not update Cocos' cache, so restoring that learned
    // state keeps real GL and Cocos' cached state synchronized for every later
    // boundary without querying the driver again.
    // Ignore errors left behind by stock rendering or another mod. From here
    // onward the final error check represents this Bismuth draw only.
    drainGLErrors();
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
            return fail("draw-run-out-of-range", static_cast<int>(run.firstSlot), static_cast<u32>(totalQuads));
        }

        const auto& ownerRecord = state.atlasOwners[run.firstSlot];
        if (ownerRecord.empty()) {
            if (gpuStateActive)
                restoreStockState();
            atlas->drawNumberOfQuads(static_cast<unsigned int>(run.slotCount),
                static_cast<unsigned int>(run.firstSlot));
            submittedAny = true;
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
            return fail("owner-index-range", static_cast<int>(run.firstSlot), static_cast<u32>(totalQuads));
        }

        const auto owner = drawDataFor(ownerRecord);
        if (!owner.resolvedState || !owner.shader || !owner.vao || !owner.ownerIndexBuffer) {
            if (gpuStateActive)
                restoreStockState();
            return fail("owner-draw-data", static_cast<int>(run.firstSlot), static_cast<u32>(totalQuads));
        }
        auto objectStateTexture = owner.resolvedState->getObjectStateTexture();
        auto spriteStateTexture = owner.resolvedState->getSpriteStateTexture();
        if (!objectStateTexture || !spriteStateTexture) {
            if (gpuStateActive)
                restoreStockState();
            return fail("state-texture-missing", static_cast<int>(run.firstSlot), static_cast<u32>(totalQuads));
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
        submittedAny = true;
        if (owner.drawCalls)
            ++(*owner.drawCalls);
        if (owner.indexCount)
            *owner.indexCount += drawIndices;
    }

    if (gpuStateActive)
        restoreStockState();
    if (!consumeGLErrors())
        return fail("gl-submit-error", -1, static_cast<u32>(totalQuads));
    return true;
}

const char* AtlasInterleaveRegistry::lastFailureReason() {
    return registry().lastFailureReason;
}

int AtlasInterleaveRegistry::lastFailureSlot() {
    return registry().lastFailureSlot;
}

unsigned int AtlasInterleaveRegistry::lastFailureAtlasSize() {
    return registry().lastFailureAtlasSize;
}

bool AtlasInterleaveRegistry::lastFailureCanUseStock() {
    return registry().lastFailureCanUseStock;
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
    state.lastFailureReason = "none";
    state.lastFailureSlot = -1;
    state.lastFailureAtlasSize = 0;
    state.lastFailureCanUseStock = true;
    state.gpuSpritesUsedThisFrame = 0;
    state.gpuDrawRunsUsedThisFrame = 0;
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
