#ifdef GEODE_IS_IOS

#include "AtlasInterleave.hpp"
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

struct RegistryState {
    std::unordered_map<cocos2d::CCSprite*, SpriteOwner> spriteOwners;
    std::unordered_map<cocos2d::CCSpriteBatchNode*, AssistShadowBatch*> immediateByBatch;
    std::unordered_map<AssistShadowBatch*, Renderer*> immediateRenderers;
    std::unordered_map<StandaloneAssistBatch*, Renderer*> deferredRenderers;
    std::unordered_set<Renderer*> invalidRenderers;

    std::unordered_map<AssistShadowBatch*, u32> submittedImmediateFrame;
    std::unordered_map<StandaloneAssistBatch*, u32> submittedDeferredFrame;

    // Reused CPU-side scratch only. GPU index data remains the persistent EBO
    // built by AssistShadowBatch / StandaloneAssistBatch.
    std::vector<cocos2d::CCSprite*> atlasSprites;
    std::vector<SpriteOwner> atlasOwners;
};

static RegistryState& registry() {
    static RegistryState state;
    return state;
}

static u32 currentFrame() {
    auto director = cocos2d::CCDirector::get();
    return director ? director->getTotalFrames() : 0;
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

    // Process-wide hook hard barrier. Menu/loading/Geode UI batches never reach
    // custom atlas or GL work.
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

    // The Cocos VAO path clears its dirty bit on the first atlas draw. A full
    // range at start=0 is the one proven case that uploads every live quad. Let
    // stock Cocos perform that upload while framebuffer writes are masked, then
    // later partial stock runs reuse the synchronized VBO.
    GLboolean previousColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    GLboolean previousDepthMask = GL_TRUE;
    GLint previousStencilMask = 0;

    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &previousStencilMask);

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
    glStencilMask(static_cast<u32>(previousStencilMask));

    return !atlas->isDirty();
}

static void drawGPURun(
    cocos2d::CCSpriteBatchNode* batch,
    const OwnerDrawData& owner,
    usize atlasStart,
    usize atlasEnd
) {
    auto& state = registry();
    if (atlasStart >= atlasEnd || atlasEnd > state.atlasOwners.size())
        return;

    const auto& firstRecord = state.atlasOwners[atlasStart];
    const usize firstOrdinal = static_cast<usize>(firstRecord.baseVertex) / 4;
    const usize spriteCount = atlasEnd - atlasStart;
    const usize firstIndex = firstOrdinal * 6;
    const usize drawIndices = spriteCount * 6;

    auto objectStateTexture = owner.resolvedState->getObjectStateTexture();
    auto spriteStateTexture = owner.resolvedState->getSpriteStateTexture();
    auto texture = batch->getTexture();

    kmMat4 matrixP;
    kmMat4 matrixMV;
    kmMat4 matrixMVP;
    kmGLGetMatrix(KM_GL_PROJECTION, &matrixP);
    kmGLGetMatrix(KM_GL_MODELVIEW, &matrixMV);
    kmMat4Multiply(&matrixMVP, &matrixP, &matrixMV);

    GLint previousVAO = 0;
    GLint previousProgram = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTextures[3] = { 0, 0, 0 };
    GLint previousBlendSrcRGB = GL_SRC_ALPHA;
    GLint previousBlendDstRGB = GL_ONE_MINUS_SRC_ALPHA;
    GLint previousBlendSrcAlpha = GL_SRC_ALPHA;
    GLint previousBlendDstAlpha = GL_ONE_MINUS_SRC_ALPHA;
    GLboolean previousBlendEnabled = glIsEnabled(GL_BLEND);
    GLboolean previousColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    GLboolean previousDepthMask = GL_TRUE;
    GLint previousStencilMask = 0;

    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVAO);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glGetIntegerv(GL_BLEND_SRC_RGB, &previousBlendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &previousBlendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousBlendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &previousBlendDstAlpha);
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &previousStencilMask);

    for (i32 unit = 0; unit < 3; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTextures[unit]);
    }
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));

    owner.shader->use();
    owner.shader->setMatrix4("u_mvp", matrixMVP.mat);

    objectStateTexture->bind(1);
    owner.shader->setInt("u_objectStateTexture", 1);
    owner.shader->setVec2("u_objectStateTextureSize", objectStateTexture->getSize());

    spriteStateTexture->bind(2);
    owner.shader->setInt("u_spriteStateTexture", 2);
    owner.shader->setVec2("u_spriteStateTextureSize", spriteStateTexture->getSize());

    // The owner's original VAO still points at its original persistent EBO.
    // No temporary index buffer, EBO rebinding or buffer upload is needed.
    glBindVertexArray(owner.vao);

    const auto blend = batch->getBlendFunc();
    glEnable(GL_BLEND);
    glBlendFunc(static_cast<GLenum>(blend.src), static_cast<GLenum>(blend.dst));
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    glStencilMask(0);

    owner.shader->setTexture("u_spriteSheetTexture", 0, texture->getName());
    glDrawElements(
        GL_TRIANGLES,
        static_cast<GLsizei>(drawIndices),
        GL_UNSIGNED_SHORT,
        reinterpret_cast<void*>(firstIndex * sizeof(u16))
    );

    if (owner.drawCalls)
        ++(*owner.drawCalls);
    if (owner.indexCount)
        *owner.indexCount += drawIndices;

    glColorMask(
        previousColorMask[0],
        previousColorMask[1],
        previousColorMask[2],
        previousColorMask[3]
    );
    glDepthMask(previousDepthMask);
    glStencilMask(static_cast<u32>(previousStencilMask));
    glBlendFuncSeparate(
        static_cast<GLenum>(previousBlendSrcRGB),
        static_cast<GLenum>(previousBlendDstRGB),
        static_cast<GLenum>(previousBlendSrcAlpha),
        static_cast<GLenum>(previousBlendDstAlpha)
    );
    if (!previousBlendEnabled)
        glDisable(GL_BLEND);

    // Raw GL calls never update Cocos' state cache. Put the real state back to
    // exactly what that cache still believes is active.
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
    state.submittedImmediateFrame.erase(owner);

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
    state.submittedDeferredFrame.erase(owner);

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

    // Deferred objects are parentless when registered. Discover their live stock
    // batch only after GD attaches them.
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
    if (state.invalidRenderers.contains(renderer))
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

    std::unordered_set<AssistShadowBatch*> touchedImmediate;
    std::unordered_set<StandaloneAssistBatch*> touchedDeferred;
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

        if (!renderer->isGPUOwnedSprite(sprite))
            continue;

        auto spriteTexture = sprite->getTexture();
        if (!spriteTexture || spriteTexture->getName() != texture->getName())
            return false;

        auto recordIt = state.spriteOwners.find(sprite);
        if (recordIt == state.spriteOwners.end() || !ownerReady(recordIt->second))
            return false;

        const auto record = recordIt->second;
        state.atlasOwners[atlasIndex] = record;
        if (record.immediate)
            touchedImmediate.insert(record.immediate);
        else
            touchedDeferred.insert(record.deferred);
        hasGPU = true;
    }

    if (!hasGPU)
        return false;

    for (usize i = 0; i < totalQuads; ++i) {
        if (!state.atlasSprites[i])
            return false;
    }

    // Prove every currently attached GPU-owned sprite from each touched owner is
    // represented by the exact atlas slot about to be replaced.
    for (auto owner : touchedImmediate) {
        if (!owner)
            return false;
        for (auto sprite : owner->ownedSprites) {
            if (!sprite || !renderer->isGPUOwnedSprite(sprite))
                continue;
            auto actualBatch = sprite->getBatchNode();
            if (!actualBatch)
                continue;
            if (actualBatch != batch)
                return false;
            const auto atlasIndex = sprite->getAtlasIndex();
            if (atlasIndex == CCSpriteIndexNotInitialized || atlasIndex >= totalQuads ||
                state.atlasSprites[atlasIndex] != sprite ||
                state.atlasOwners[atlasIndex].immediate != owner) {
                return false;
            }
        }
    }

    for (auto owner : touchedDeferred) {
        if (!owner)
            return false;
        for (auto sprite : owner->ownedSprites) {
            if (!sprite || !renderer->isGPUOwnedSprite(sprite))
                continue;
            auto actualBatch = sprite->getBatchNode();
            if (!actualBatch)
                continue;
            if (actualBatch != batch)
                return false;
            const auto atlasIndex = sprite->getAtlasIndex();
            if (atlasIndex == CCSpriteIndexNotInitialized || atlasIndex >= totalQuads ||
                state.atlasSprites[atlasIndex] != sprite ||
                state.atlasOwners[atlasIndex].deferred != owner) {
                return false;
            }
        }
    }

    // All failure-prone validation and stock VBO synchronization happens before
    // the first visible split draw. After this point we never fall back to a
    // second full atlas draw, so alpha-blended content cannot be double-rendered.
    if (!synchronizeDirtyAtlasWithStockDraw(atlas))
        return false;

    for (auto owner : touchedImmediate) {
        owner->stats.drawCallsLastFrame = 0;
        owner->stats.indicesLastFrame = 0;
    }
    for (auto owner : touchedDeferred) {
        owner->stats.drawCallsLastFrame = 0;
        owner->stats.indicesLastFrame = 0;
    }

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

    usize stockStart = 0;
    usize atlasIndex = 0;
    while (atlasIndex < totalQuads) {
        const auto owner = state.atlasOwners[atlasIndex];
        if (owner.empty()) {
            ++atlasIndex;
            continue;
        }

        if (atlasIndex > stockStart) {
            atlas->drawNumberOfQuads(
                static_cast<unsigned int>(atlasIndex - stockStart),
                static_cast<unsigned int>(stockStart)
            );
        }

        // Draw directly from the owner's persistent EBO. A segment can stay one
        // draw only while both atlas ownership and persistent vertex ordinal are
        // contiguous. If GD reorders two GPU sprites, split the GPU calls rather
        // than rewriting any index buffer.
        usize runEnd = atlasIndex + 1;
        u16 previousBaseVertex = owner.baseVertex;
        while (runEnd < totalQuads) {
            const auto& next = state.atlasOwners[runEnd];
            if (!next.sameOwner(owner) || next.baseVertex != previousBaseVertex + 4)
                break;
            previousBaseVertex = next.baseVertex;
            ++runEnd;
        }

        drawGPURun(batch, drawDataFor(owner), atlasIndex, runEnd);
        atlasIndex = runEnd;
        stockStart = runEnd;
    }

    if (stockStart < totalQuads) {
        atlas->drawNumberOfQuads(
            static_cast<unsigned int>(totalQuads - stockStart),
            static_cast<unsigned int>(stockStart)
        );
    }

    const u32 frame = currentFrame();
    for (auto owner : touchedImmediate)
        state.submittedImmediateFrame[owner] = frame;
    for (auto owner : touchedDeferred)
        state.submittedDeferredFrame[owner] = frame;

    return true;
}

bool AtlasInterleaveRegistry::consumeSubmission(AssistShadowBatch* owner) {
    if (!owner)
        return false;
    auto& state = registry();
    auto it = state.submittedImmediateFrame.find(owner);
    return it != state.submittedImmediateFrame.end() && it->second == currentFrame();
}

bool AtlasInterleaveRegistry::consumeSubmission(StandaloneAssistBatch* owner) {
    if (!owner)
        return false;
    auto& state = registry();
    auto it = state.submittedDeferredFrame.find(owner);
    return it != state.submittedDeferredFrame.end() && it->second == currentFrame();
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
