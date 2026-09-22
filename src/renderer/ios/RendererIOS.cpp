#ifdef GEODE_IS_IOS

#include "../Renderer.hpp"
#include "../AreaVisualState.hpp"
#include "ResolvedStateLayer.hpp"
#include "AssistShadowBatch.hpp"
#include "StandaloneAssistBatch.hpp"
#include "AtlasInterleave.hpp"
#include "BackgroundGPU.hpp"
#include "GPUTruth.hpp"

#include "Geode/cocos/CCDirector.h"
#include "Geode/cocos/sprite_nodes/CCSpriteBatchNode.h"
#include <Geode/utils/cocos.hpp>

#include <cstring>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {
// Match the resolved-state sprite ceiling. The old 9,216/12,288 ownership
// caps discarded thousands of already-proven-safe sprites at setup time. Since
// live atlas order changes as GD activates/deactivates objects, those sampled
// owners became scattered later in the level and produced tiny GPU islands.
constexpr usize MAX_STANDALONE_BUFFER_SPRITES = 16384;
constexpr usize MAX_PERSISTENT_GPU_SPRITES = 16384;
constexpr usize MAX_IMMEDIATE_GPU_SPRITES = 16384;

struct StandaloneObjectDesc {
    GameObject* root = nullptr;
    std::vector<ResolvedStateLayer::ShadowCandidate> candidates;
};

struct StandaloneChunkDesc {
    std::vector<GameObject*> roots;
    std::vector<ResolvedStateLayer::ShadowCandidate> candidates;
};

struct IOSRendererState {
    BackgroundGPU background;
    std::unique_ptr<ColorChannelBuffer> colorChannels = std::make_unique<ColorChannelBuffer>();
    std::unique_ptr<ResolvedStateLayer> resolvedState;
    Shader* assistShader = nullptr;

    std::vector<Ref<AssistShadowBatch>> gpuBatches;
    // This vector contains both true standalone root-visit buffers and deferred
    // atlas buffers. Deferred buffers are attached beside their future stock
    // batch and draw once per frame; true standalone buffers remain unparented
    // and are addressed by root visit.
    std::vector<Ref<StandaloneAssistBatch>> standaloneBatches;

    std::unordered_set<cocos2d::CCSprite*> batchOwnedSprites;
    std::unordered_set<cocos2d::CCSprite*> deferredAtlasOwnedSprites;
    std::unordered_set<cocos2d::CCSprite*> standaloneOwnedSprites;
    std::unordered_set<cocos2d::CCSprite*> ownedSprites;
    std::unordered_set<GameObject*> standaloneOwnedRoots;
    std::unordered_map<GameObject*, usize> standaloneRootBatchIndices;

    usize gpuCandidateSprites = 0;
    usize candidatesWithBatch = 0;
    usize candidatesWithoutBatch = 0;
    usize candidateBatchNodes = 0;
    usize batchesWithoutParent = 0;

    usize standaloneObjectCandidates = 0;
    usize standaloneObjectEligible = 0;
    usize standaloneMixedRejected = 0;
    usize standaloneDuplicateRejected = 0;
    usize standaloneSharedRejected = 0;
    usize standaloneExternalRejected = 0;
    usize standaloneExternalGlowObjects = 0;
    usize standaloneExternalColorObjects = 0;
    usize standaloneExternalOtherObjects = 0;
    usize standaloneInvalidVisualRejected = 0;
    usize standaloneRootBatchRejected = 0;
    usize standaloneParentlessAtInit = 0;
    usize standaloneBufferCount = 0;
    usize standaloneRootVisitsCurrentFrame = 0;
    usize standaloneRootVisitsLastFrame = 0;
    usize standaloneCPUObjects = 0;
    usize persistentBudgetRejectedSprites = 0;

    usize deferredAtlasObjects = 0;
    usize deferredAtlasUnmapped = 0;
    usize deferredAtlasBatchNodes = 0;
    usize deferredAtlasBufferCount = 0;

    usize batchTransformSkipsCurrentFrame = 0;
    usize batchTransformSkipsLastFrame = 0;

    usize cpuWorkCurrentFrame = 0;
    usize cpuWorkLastFrame = 0;
    usize gpuWorkCurrentFrame = 0;
    usize gpuWorkLastFrame = 0;

    std::array<usize, 60> gpuSpriteHistory{};
    usize gpuSpriteHistoryIndex = 0;
    usize gpuSpriteHistoryCount = 0;
    usize gpuSpriteHistorySum = 0;

    std::string lastDebugText;
    bool suspended = false;
    bool enabledBeforeSuspend = false;
    bool gpuFramePrepared = false;
    bool debugWasEnabled = false;
    usize debugFrames = 0;

    ~IOSRendererState() {
        if (assistShader)
            Shader::destroy(assistShader);
    }
};

static std::unordered_map<Renderer*, std::unique_ptr<IOSRendererState>> g_iosStates;
static Renderer* currentRenderer = nullptr;

static IOSRendererState* iosState(Renderer* renderer) {
    auto it = g_iosStates.find(renderer);
    return it == g_iosStates.end() ? nullptr : it->second.get();
}

static bool isDescendantOf(cocos2d::CCNode* node, cocos2d::CCNode* ancestor) {
    if (!node || !ancestor)
        return false;

    for (auto current = node; current; current = current->getParent()) {
        if (current == ancestor)
            return true;
    }
    return false;
}

static void restoreOwnedStockQuads(IOSRendererState* state) {
    if (!state)
        return;

    for (auto sprite : state->batchOwnedSprites) {
        if (!sprite || !sprite->getBatchNode())
            continue;
        sprite->setDirty(true);
        sprite->updateTransform();
    }
}

static std::vector<StandaloneChunkDesc> buildStandaloneChunks(
    std::vector<StandaloneObjectDesc>& objects
) {
    // Buffer assignment is spatial and deterministic. The live atlas still
    // dictates draw order; nearby sprites simply share more persistent VAOs.
    const auto coordinate = [](float value) { return std::isfinite(value) ? value : 0.f; };
    std::sort(objects.begin(), objects.end(), [&](const auto& a, const auto& b) {
        const auto ax = coordinate(a.root ? a.root->getPositionX() : 0.f);
        const auto bx = coordinate(b.root ? b.root->getPositionX() : 0.f);
        if (ax != bx)
            return ax < bx;
        const auto ai = a.candidates.empty() ? 0 : a.candidates.front().objectStateIndex;
        const auto bi = b.candidates.empty() ? 0 : b.candidates.front().objectStateIndex;
        return ai < bi;
    });
    std::vector<StandaloneChunkDesc> chunks;
    StandaloneChunkDesc current;

    auto flush = [&]() {
        if (!current.roots.empty() && !current.candidates.empty())
            chunks.push_back(std::move(current));
        current = {};
    };

    for (const auto& object : objects) {
        if (!object.root || object.candidates.empty())
            continue;

        if (object.candidates.size() > MAX_STANDALONE_BUFFER_SPRITES)
            continue;

        if (!current.candidates.empty() &&
            current.candidates.size() + object.candidates.size() > MAX_STANDALONE_BUFFER_SPRITES) {
            flush();
        }

        current.roots.push_back(object.root);
        current.candidates.insert(
            current.candidates.end(),
            object.candidates.begin(),
            object.candidates.end()
        );
    }

    flush();
    return chunks;
}
} // namespace

Renderer::~Renderer() { terminate(); }

bool Renderer::init(PlayLayer* playLayer) {
    if (currentRenderer)
        return false;

    currentRenderer = this;
    layer = playLayer;
    AreaVisualState::reset();

    if (!Mod::get()->getSettingValue<bool>("enabled")) {
        currentRenderer = nullptr;
        return false;
    }

    g_iosStates[this] = std::make_unique<IOSRendererState>();
    auto state = iosState(this);
    state->background.prepare(); // Compile during level setup, not the first draw.
    colorChannelBuffer = state->colorChannels.get();
    std::memset(colorChannelBuffer, 0, sizeof(ColorChannelBuffer));

    state->resolvedState = std::make_unique<ResolvedStateLayer>();
    if (!state->resolvedState->init(layer)) {
        log::warn("Bismuth iOS resolved-state layer unavailable; GPU assist cannot initialize");
    }

    if (state->resolvedState && state->resolvedState->isGPUStateReady()) {
        state->assistShader = Shader::create("assist_ios.vert", "assist_ios.frag");
        if (!state->assistShader) {
            log::warn("Bismuth iOS assist shader unavailable");
        } else {
            const auto candidates = state->resolvedState->getGPUCandidates();
            state->gpuCandidateSprites = candidates.size();

            std::unordered_set<cocos2d::CCSpriteBatchNode*> candidateBatches;
            std::unordered_map<cocos2d::CCSpriteBatchNode*, usize> candidateBatchCounts;
            std::unordered_map<GameObject*, std::vector<ResolvedStateLayer::ShadowCandidate>> candidatesByObject;
            std::unordered_map<cocos2d::CCSprite*, GameObject*> firstSpriteOwner;
            std::unordered_set<GameObject*> sharedVisualObjects;
            std::unordered_set<cocos2d::CCSprite*> sharedVisualSprites;
            candidateBatches.reserve(64);
            candidateBatchCounts.reserve(64);
            candidatesByObject.reserve(std::min<usize>(
                state->resolvedState->getStats().safeObjects,
                MAX_PERSISTENT_GPU_SPRITES
            ));
            firstSpriteOwner.reserve(candidates.size());
            sharedVisualSprites.reserve(32);

            std::vector<ResolvedStateLayer::ShadowCandidate> registryCandidates;
            std::unordered_set<cocos2d::CCSprite*> registryCandidateSprites;
            std::unordered_set<cocos2d::CCSprite*> deferredRegistrySprites;
            registryCandidates.reserve(candidates.size());
            registryCandidateSprites.reserve(candidates.size());
            deferredRegistrySprites.reserve(candidates.size());

            // Detect cross-object visual aliases before assigning any GPU owner.
            // One sprite cannot safely carry two object-state indices or two
            // persistent vertex identities. Shared visuals stay on stock Cocos.
            for (const auto& candidate : candidates) {
                auto object = candidate.object;
                auto sprite = candidate.sprite;
                if (!object || !sprite)
                    continue;

                const auto [ownerIt, insertedOwner] = firstSpriteOwner.emplace(sprite, object);
                if (!insertedOwner && ownerIt->second != object) {
                    sharedVisualSprites.insert(sprite);
                    sharedVisualObjects.insert(object);
                    sharedVisualObjects.insert(ownerIt->second);
                }
            }

            for (const auto& candidate : candidates) {
                auto object = candidate.object;
                auto sprite = candidate.sprite;
                if (!object || !sprite || sharedVisualSprites.contains(sprite))
                    continue;

                candidatesByObject[object].push_back(candidate);

                if (auto batch = sprite->getBatchNode()) {
                    ++state->candidatesWithBatch;
                    candidateBatches.insert(batch);
                    ++candidateBatchCounts[batch];
                    if (registryCandidateSprites.insert(sprite).second)
                        registryCandidates.push_back(candidate);
                } else {
                    ++state->candidatesWithoutBatch;
                }
            }

            state->candidateBatchNodes = candidateBatches.size();

            std::vector<StandaloneObjectDesc> standaloneObjects;
            // One registry VBO owns every safe sprite that either already lives
            // in a stock atlas or has a proven future stock-atlas home.
            std::unordered_set<cocos2d::CCSpriteBatchNode*> deferredAtlasTargetBatches;
            standaloneObjects.reserve(candidatesByObject.size());
            deferredAtlasTargetBatches.reserve(32);

            for (auto& [object, objectCandidates] : candidatesByObject) {
                if (!object || objectCandidates.empty())
                    continue;

                bool anyStandalone = false;
                bool anyAtlas = false;
                bool externalVisual = false;
                bool externalGlow = false;
                bool externalColor = false;
                bool externalOther = false;
                bool invalidVisual = false;
                bool duplicateVisual = false;
                std::unordered_set<cocos2d::CCSprite*> objectSprites;
                objectSprites.reserve(objectCandidates.size());

                for (const auto& candidate : objectCandidates) {
                    auto sprite = candidate.sprite;
                    if (!sprite) {
                        invalidVisual = true;
                        continue;
                    }

                    if (!objectSprites.insert(sprite).second)
                        duplicateVisual = true;

                    if (sprite->getBatchNode())
                        anyAtlas = true;
                    else
                        anyStandalone = true;

                    if (!sprite->getTexture())
                        invalidVisual = true;

                    if (!isDescendantOf(sprite, object)) {
                        externalVisual = true;
                        if (sprite == object->m_glowSprite)
                            externalGlow = true;
                        else if (sprite == object->m_colorSprite)
                            externalColor = true;
                        else
                            externalOther = true;
                    }
                }

                if (!anyStandalone)
                    continue;

                ++state->standaloneObjectCandidates;

                const bool forcedDecoration =
                    object->m_objectType == GameObjectType::Decoration;
                const bool complexResolvedSolid =
                    object->m_objectType == GameObjectType::Solid &&
                    (objectCandidates.size() > 1 ||
                     object->m_glowSprite || object->m_colorSprite ||
                     (object->getChildren() && object->getChildren()->count() != 0));
                const bool resolvedVisualTree = forcedDecoration || complexResolvedSolid;

                // Resolved visual trees may have some sprites already atlas-owned
                // and others still standalone. Keep atlas sprites in their exact
                // stock batch and build deferred geometry only for the standalone
                // subset. The old code granted this to Decoration only, which
                // discarded thousands of complex Solid candidates one layer
                // after classifyObject() had already proven them safe.
                std::vector<ResolvedStateLayer::ShadowCandidate> standaloneCandidates;
                if (resolvedVisualTree && anyAtlas) {
                    standaloneCandidates.reserve(objectCandidates.size());
                    for (const auto& candidate : objectCandidates) {
                        if (candidate.sprite && !candidate.sprite->getBatchNode())
                            standaloneCandidates.push_back(candidate);
                    }
                    if (standaloneCandidates.empty())
                        continue;
                } else {
                    standaloneCandidates = objectCandidates;
                }

                if (!resolvedVisualTree && anyAtlas) {
                    ++state->standaloneMixedRejected;
                    continue;
                }

                if (!forcedDecoration && duplicateVisual) {
                    ++state->standaloneDuplicateRejected;
                    continue;
                }

                if (!forcedDecoration && sharedVisualObjects.contains(object)) {
                    ++state->standaloneSharedRejected;
                    continue;
                }

                if (!resolvedVisualTree && externalVisual) {
                    ++state->standaloneExternalRejected;
                    if (externalGlow)
                        ++state->standaloneExternalGlowObjects;
                    if (externalColor)
                        ++state->standaloneExternalColorObjects;
                    if (externalOther)
                        ++state->standaloneExternalOtherObjects;
                    continue;
                }

                if (!forcedDecoration && invalidVisual) {
                    ++state->standaloneInvalidVisualRejected;
                    continue;
                }

                if (!resolvedVisualTree && object->getBatchNode()) {
                    ++state->standaloneRootBatchRejected;
                    continue;
                }

                auto parent = object->getParent();
                if (!parent) {
                    ++state->standaloneParentlessAtInit;

                    // GD removes these roots while inactive and later
                    // addMainSpriteToParent() inserts them into parentForZLayer().
                    // Simple roots and already-resolved complex visual trees both
                    // have exact deferred geometry. Interactive/animated objects
                    // never reach this candidate set.
                    const bool simpleDeferredRoot =
                        standaloneCandidates.size() == 1 &&
                        standaloneCandidates[0].sprite == static_cast<cocos2d::CCSprite*>(object);

                    if ((!resolvedVisualTree && !simpleDeferredRoot) || !layer->m_batchNodes) {
                        ++state->deferredAtlasUnmapped;
                        continue;
                    }

                    // Match stock GameObject::addMainSpriteToParent(). The old
                    // predictor used m_baseOrDetailBlending directly and skipped
                    // both updateBlendMode() and GD's color-sprite Z adjustment,
                    // which routed effect-heavy levels into the wrong stock batch.
                    object->updateBlendMode();
                    i32 targetZ = (i32)object->getObjectZLayer();
                    if (object->m_shouldBlendBase && object->m_colorSprite &&
                        !object->m_shouldBlendDetail && !object->m_colorZLayerRelated) {
                        ++targetZ;
                    }

                    auto targetNode = layer->parentForZLayer(
                        targetZ,
                        object->m_shouldBlendBase,
                        object->getParentMode(),
                        false
                    );
                    if (!targetNode || layer->m_batchNodes->indexOfObject(targetNode) == UINT_MAX) {
                        ++state->deferredAtlasUnmapped;
                        continue;
                    }

                    auto targetBatch = static_cast<cocos2d::CCSpriteBatchNode*>(targetNode);
                    if (!targetBatch->getParent()) {
                        ++state->deferredAtlasUnmapped;
                        continue;
                    }

                    ++state->standaloneObjectEligible;
                    ++state->deferredAtlasObjects;
                    deferredAtlasTargetBatches.insert(targetBatch);
                    for (const auto& candidate : standaloneCandidates) {
                        if (!candidate.sprite)
                            continue;
                        deferredRegistrySprites.insert(candidate.sprite);
                        if (registryCandidateSprites.insert(candidate.sprite).second)
                            registryCandidates.push_back(candidate);
                    }
                    continue;
                }

                // A genuinely parented non-batch root used to take the root-visit
                // GPU path. That path snapshots/restores a large amount of GL
                // state per object and can turn decoration-heavy scenes into
                // hundreds of tiny submissions. Keep these roots on stock Cocos;
                // deferred roots that later join an atlas still use the GPU.
                ++state->standaloneCPUObjects;
                continue;
            }

            // All atlas-capable safe geometry shares one persistent owner.
            // The VBO may contain sprites from many stock batches and textures;
            // AtlasInterleave selects only the live batch's exact sprites and
            // binds that stock batch's texture/blend state for each submission.
            state->deferredAtlasBatchNodes = deferredAtlasTargetBatches.size();
            if (!registryCandidates.empty()) {
                if (registryCandidates.size() > MAX_PERSISTENT_GPU_SPRITES) {
                    state->persistentBudgetRejectedSprites +=
                        registryCandidates.size() - MAX_PERSISTENT_GPU_SPRITES;
                    registryCandidates.resize(MAX_PERSISTENT_GPU_SPRITES);
                }

                auto gpuRegistry = StandaloneAssistBatch::create(
                    state->resolvedState.get(),
                    state->assistShader,
                    registryCandidates,
                    false
                );
                if (gpuRegistry && gpuRegistry->getStats().ready &&
                    gpuRegistry->getOwnedSprites().size() == registryCandidates.size()) {
                    state->standaloneBatches.push_back(gpuRegistry);
                    state->deferredAtlasBufferCount = 1;

                    for (auto sprite : gpuRegistry->getOwnedSprites()) {
                        if (!sprite)
                            continue;
                        state->batchOwnedSprites.insert(sprite);
                        state->ownedSprites.insert(sprite);
                        if (deferredRegistrySprites.contains(sprite))
                            state->deferredAtlasOwnedSprites.insert(sprite);
                    }
                } else {
                    state->persistentBudgetRejectedSprites += registryCandidates.size();
                }
            }

            // True standalone roots deliberately remain stock. The old
            // root-addressable GPU path performed expensive GL state capture and
            // restore work once per visible root, which was the opposite of an
            // optimization on dense iOS scenes.
            state->standaloneBufferCount = state->standaloneBatches.size();
            state->resolvedState->setGPUOwnedSprites(state->ownedSprites);
            // Compile the event-driven hot set now, not only after a scene
            // suspend/resume. Otherwise every persistently owned sprite looks
            // active during the first playthrough and dead level sections can
            // consume the hybrid GPU run budget.
            state->resolvedState->reseedActiveFromStock();

            log::info(
                "Bismuth iOS ownership: {} candidates ({} atlas-now / {} parentless-or-standalone); standalone {} candidates / {} ownership-eligible; rejects mixed {} / duplicate {} / shared {} / external {} (glow {} / color {} / other {}) / invalid {} / root-batched {}; parentless-at-init {} -> deferred atlas {} object(s), {} target batch(es), {} buffer(s), {} unmapped; {} immediate atlas node(s), {} true standalone root(s), {} total GPU sprite(s); CPU standalone {} / persistent-budget rejects {} sprite(s)",
                state->gpuCandidateSprites,
                state->candidatesWithBatch,
                state->candidatesWithoutBatch,
                state->standaloneObjectCandidates,
                state->standaloneObjectEligible,
                state->standaloneMixedRejected,
                state->standaloneDuplicateRejected,
                state->standaloneSharedRejected,
                state->standaloneExternalRejected,
                state->standaloneExternalGlowObjects,
                state->standaloneExternalColorObjects,
                state->standaloneExternalOtherObjects,
                state->standaloneInvalidVisualRejected,
                state->standaloneRootBatchRejected,
                state->standaloneParentlessAtInit,
                state->deferredAtlasObjects,
                state->deferredAtlasBatchNodes,
                state->deferredAtlasBufferCount,
                state->deferredAtlasUnmapped,
                state->deferredAtlasBufferCount,
                state->standaloneOwnedRoots.size(),
                state->ownedSprites.size(),
                state->standaloneCPUObjects,
                state->persistentBudgetRejectedSprites
            );
        }
    }

    ingameEnableDisable = false;
    useIndexCulling = false;

    debugText = CCLabelBMFont::create("", "chatFont.fnt");
    debugTextOutline1 = CCLabelBMFont::create("", "chatFont.fnt");
    debugTextOutline2 = CCLabelBMFont::create("", "chatFont.fnt");
    if (!debugText || !debugTextOutline1 || !debugTextOutline2)
        return false;

    debugTextOutline1->setColor({0, 0, 0});
    debugTextOutline1->setOpacity(200);
    debugTextOutline2->setColor({0, 0, 0});
    debugTextOutline2->setOpacity(200);

    debugText->setAnchorPoint(CCPoint(0, 1));
    debugText->setPosition(1, CCDirector::get()->getWinSize().height - 8);
    debugText->setScale(0.5);
    layer->addChild(debugText, 1000);

    debugTextOutline1->setAnchorPoint(CCPoint(0, 1));
    debugTextOutline1->setPosition(0.5, CCDirector::get()->getWinSize().height - 8.5);
    debugTextOutline1->setScale(0.5);
    layer->addChild(debugTextOutline1, 999);

    debugTextOutline2->setAnchorPoint(CCPoint(0, 1));
    debugTextOutline2->setPosition(1.5, CCDirector::get()->getWinSize().height - 7.5);
    debugTextOutline2->setScale(0.5);
    layer->addChild(debugTextOutline2, 999);

    enabled = true;
    for (auto& gpuBatch : state->gpuBatches) {
        if (gpuBatch)
            gpuBatch->setVisible(true);
    }
    for (auto& gpuBuffer : state->standaloneBatches) {
        if (gpuBuffer)
            gpuBuffer->setVisible(true);
    }

    setVisible(false);
    rendererStartTime = getTime();

    log::info("Bismuth iOS initialized: profitable atlas GPU assist with stock CPU ownership for fragmented/standalone/over-budget visuals");
    return true;
}

void Renderer::generateBatchNodes() {}

void Renderer::terminate() {
    if (currentRenderer == this)
        currentRenderer = nullptr;
    enabled = false;

    // These labels are attached directly to PlayLayer rather than to Renderer.
    // Remove them explicitly so a renderer rebuild cannot leave ghost copies.
    if (debugText)
        debugText->removeFromParentAndCleanup(true);
    if (debugTextOutline1)
        debugTextOutline1->removeFromParentAndCleanup(true);
    if (debugTextOutline2)
        debugTextOutline2->removeFromParentAndCleanup(true);
    debugText = nullptr;
    debugTextOutline1 = nullptr;
    debugTextOutline2 = nullptr;

    auto state = iosState(this);
    if (state) {
        for (auto& gpuBatch : state->gpuBatches) {
            if (gpuBatch)
                gpuBatch->setVisible(false);
        }
        for (auto& gpuBuffer : state->standaloneBatches) {
            if (gpuBuffer)
                gpuBuffer->setVisible(false);
        }

        state->ownedSprites.clear();
        state->batchOwnedSprites.clear();
        state->deferredAtlasOwnedSprites.clear();
        state->standaloneOwnedSprites.clear();
        state->standaloneOwnedRoots.clear();
        state->standaloneRootBatchIndices.clear();
        state->gpuBatches.clear();
        state->standaloneBatches.clear();
    }

    if (shader)
        Shader::destroy(shader);
    shader = nullptr;
    if (basicShader)
        Shader::destroy(basicShader);
    basicShader = nullptr;

    colorChannelBuffer = nullptr;
    colorChannelBufferObject = nullptr;
    srbBuffer = nullptr;
    uniformBuffer = nullptr;

    AreaVisualState::reset();
    g_iosStates.erase(this);
    layer = nullptr;
}

void Renderer::prepareShaderUniforms() {}
void Renderer::prepareColorChannelBuffer() {}
void Renderer::generateStaticRenderingBuffer(ObjectSorter&) {}
void Renderer::draw() {}

void Renderer::updateDebugText() {
    auto state = iosState(this);
    if (!state || !debugText || !debugTextOutline1 || !debugTextOutline2)
        return;

    const bool show = Mod::get()->getSettingValue<bool>("ios_gpu_debug");
    std::string text;
    if (show) {
        usize calls = 0;
        usize indices = 0;
        for (const auto& batch : state->gpuBatches) {
            if (batch) {
                calls += batch->getStats().drawCallsLastFrame;
                indices += batch->getStats().indicesLastFrame;
            }
        }
        for (const auto& batch : state->standaloneBatches) {
            if (batch) {
                calls += batch->getStats().drawCallsLastFrame;
                indices += batch->getStats().indicesLastFrame;
            }
        }
        const usize currentSprites = indices / 6;
        if (state->gpuSpriteHistoryCount < state->gpuSpriteHistory.size()) {
            ++state->gpuSpriteHistoryCount;
        } else {
            state->gpuSpriteHistorySum -= state->gpuSpriteHistory[state->gpuSpriteHistoryIndex];
        }
        state->gpuSpriteHistory[state->gpuSpriteHistoryIndex] = currentSprites;
        state->gpuSpriteHistorySum += currentSprites;
        state->gpuSpriteHistoryIndex =
            (state->gpuSpriteHistoryIndex + 1) % state->gpuSpriteHistory.size();
        const usize averageSprites = state->gpuSpriteHistoryCount
            ? state->gpuSpriteHistorySum / state->gpuSpriteHistoryCount
            : 0;

        const bool ready = state->assistShader && state->resolvedState &&
            state->resolvedState->isGPUStateReady();
        const char* status = !enabled ? "OFF" : !ready ? "UNAVAILABLE" : calls ? "ACTIVE" : "IDLE";
        const auto& coverage = state->resolvedState
            ? state->resolvedState->getStats()
            : ResolvedStateLayer::Stats{};
        text = fmt::format(
            "Bismuth GPU [{}]\n"
            "GPU Draw: {} sprites/frame | avg {}\n"
            "Calls: {} | Transforms skipped: {}\n"
            "Work: CPU {} | GPU {}\n"
            "Active owned: {} | persistent: {} | candidates: {}\n"
            "Atlas: {} | no-batch: {} | deferred: {} | CPU roots: {} | mixed rej: {}",
            status, currentSprites, averageSprites, calls,
            state->batchTransformSkipsLastFrame + state->standaloneRootVisitsLastFrame,
            state->cpuWorkLastFrame,
            state->gpuWorkLastFrame,
            coverage.activeGPUSprites,
            state->ownedSprites.size(),
            state->gpuCandidateSprites,
            state->candidatesWithBatch,
            state->candidatesWithoutBatch,
            state->deferredAtlasObjects,
            state->standaloneCPUObjects,
            state->standaloneMixedRejected
        );
    }
    if (state->lastDebugText == text)
        return;
    state->lastDebugText = text;
    debugText->setString(text.c_str());
    debugTextOutline1->setString(text.c_str());
    debugTextOutline2->setString(text.c_str());
}

Shader* Renderer::prepareDraw() { return nullptr; }
void Renderer::finishDraw() {}

void Renderer::update(float dt) {
    gameTimer += dt;
}

bool Renderer::drawGPUBackground(cocos2d::CCSprite* sprite) {
    if (!enabled || !layer || !layer->m_background || !sprite ||
        !isDescendantOf(sprite, layer->m_background))
        return false;
    auto state = iosState(this);
    // Only claim the stock draw after every pre-submit requirement is proven.
    // If background GPU initialization/texture eligibility is unavailable, let
    // CCSprite::draw() run normally. Once the custom draw begins, ownership is
    // strict because a driver error cannot tell us whether pixels were emitted.
    if (!state || !state->background.canDraw(sprite))
        return false;

    const bool submitted = state->background.draw(sprite);
    GPUTruth::recordBackground(this, submitted);
    return true;
}

void Renderer::beginGPUFrame() {
    auto state = iosState(this);
    if (!state)
        return;
    state->gpuFramePrepared = false;
    state->standaloneRootVisitsCurrentFrame = 0;
    state->batchTransformSkipsCurrentFrame = 0;
    state->cpuWorkCurrentFrame = 0;
    state->gpuWorkCurrentFrame = 0;
    AtlasInterleaveRegistry::beginFrame();
    for (auto& buffer : state->standaloneBatches) {
        if (buffer)
            buffer->beginFrame();
    }
}

void Renderer::prepareGPUFrame() {
    auto state = iosState(this);
    if (!state || state->gpuFramePrepared || !enabled)
        return;
    state->gpuFramePrepared = true;
    // Called from render traversal, after stock scheduler/actions/physics. One
    // state capture per displayed frame, independent of simulation substeps.
    if (state->resolvedState) {
        state->resolvedState->update(!state->ownedSprites.empty());
        state->resolvedState->finishEventFrame();
    }
}

void Renderer::finishGPUFrame() {
    auto state = iosState(this);
    if (!state)
        return;
    // A CPU-only frame never needs a final hidden-state upload, but it still
    // must retire deactivated records from the event-driven hot set. Leaving
    // them queued until some future GPU submission can make dead level sections
    // keep participating in hybrid scheduling for arbitrarily long stretches.
    if (!state->gpuFramePrepared && state->resolvedState)
        state->resolvedState->finishEventFrame();

    state->standaloneRootVisitsLastFrame = state->standaloneRootVisitsCurrentFrame;
    state->batchTransformSkipsLastFrame = state->batchTransformSkipsCurrentFrame;
    state->cpuWorkLastFrame = state->cpuWorkCurrentFrame;
    state->gpuWorkLastFrame = state->gpuWorkCurrentFrame;
    const bool show = Mod::get()->getSettingValue<bool>("ios_gpu_debug");
    if (show != state->debugWasEnabled || (show && state->debugFrames++ % 12 == 0))
        updateDebugText();
    state->debugWasEnabled = show;
}

bool Renderer::isColorChannelBlending(i32 channel) {
    return layer && layer->shouldBlend(channel);
}

CCSpriteBatchNode* Renderer::getSpriteBatchNodeWithLayerId(LayerKey id) {
    if (!layer || !layer->m_batchNodes)
        return nullptr;

    CCNode* node = layer->parentForZLayer((i32)id.zlayer, id.blending, (i32)id.spriteSheet, false);
    if (!node || layer->m_batchNodes->indexOfObject(node) == UINT_MAX)
        return nullptr;
    return static_cast<CCSpriteBatchNode*>(node);
}

Ref<Renderer> Renderer::create(PlayLayer* playLayer) {
    auto ren = new Renderer;
    if (ren->init(playLayer)) {
        ren->autorelease();
        return ren;
    }
    delete ren;
    return nullptr;
}

Ref<Renderer> Renderer::get() { return currentRenderer; }

Ref<Renderer> Renderer::forPlayLayer(PlayLayer* playLayer) {
    for (const auto& [renderer, state] : g_iosStates) {
        if (renderer->layer == playLayer)
            return renderer;
    }
    return nullptr;
}

void Renderer::suspendGPU() {
    auto state = iosState(this);
    if (!state)
        return;
    if (!state->suspended) {
        state->enabledBeforeSuspend = enabled;
        state->suspended = true;
        setEnabled(false); // Restore quads while the outgoing layer is alive.
    }
    if (currentRenderer == this)
        currentRenderer = nullptr;
    if (state->resolvedState)
        state->resolvedState->setCurrent(false);
}

void Renderer::resumeGPU() {
    auto state = iosState(this);
    if (!state)
        return;

    // Idempotent rebind: even if the suspended flag was missed or already
    // cleared, this exact PlayLayer renderer can reclaim the singleton.
    if (currentRenderer && currentRenderer != this)
        currentRenderer->suspendGPU();
    currentRenderer = this;

    if (state->resolvedState) {
        state->resolvedState->setCurrent(true);
        state->resolvedState->resync();
        state->resolvedState->reseedActiveFromStock();
    }

    const bool wasSuspended = state->suspended;
    state->suspended = false;
    beginGPUFrame();

    if (wasSuspended)
        setEnabled(state->enabledBeforeSuspend && Mod::get()->getSettingValue<bool>("enabled"));
    else if (Mod::get()->getSettingValue<bool>("enabled") && !enabled)
        setEnabled(true);
}

bool Renderer::useOptimizations() {
    return false;
}

bool Renderer::isGPUPersistentlyOwnedSprite(cocos2d::CCSprite* sprite) const {
    if (!enabled || !sprite)
        return false;

    auto state = iosState(const_cast<Renderer*>(this));
    if (!state || !state->batchOwnedSprites.contains(sprite) ||
        !state->resolvedState || !state->resolvedState->canDrawSprite(sprite))
        return false;

    // Deferred geometry becomes drawable only after GD inserts the sprite into
    // a real stock atlas. Before that there is no exact live render home.
    if (state->deferredAtlasOwnedSprites.contains(sprite) && !sprite->getBatchNode())
        return false;

    return true;
}

bool Renderer::isGPUOwnedSprite(cocos2d::CCSprite* sprite) const {
    if (!isGPUPersistentlyOwnedSprite(sprite))
        return false;

    auto state = iosState(const_cast<Renderer*>(this));
    return state && state->resolvedState &&
        state->resolvedState->isSpriteActive(sprite);
}

void Renderer::recordCPUWork(cocos2d::CCSprite* sprite) {
    if (!sprite || !layer || !layer->m_batchNodes ||
        !Mod::get()->getSettingValue<bool>("ios_gpu_debug"))
        return;

    auto batch = sprite->getBatchNode();
    if (!batch || layer->m_batchNodes->indexOfObject(batch) == UINT_MAX)
        return;

    if (auto state = iosState(this))
        ++state->cpuWorkCurrentFrame;
}

void Renderer::recordGPUWork(usize sprites) {
    if (!sprites || !Mod::get()->getSettingValue<bool>("ios_gpu_debug"))
        return;
    if (auto state = iosState(this))
        state->gpuWorkCurrentFrame += sprites;
}

bool Renderer::hasForcedDecorationInBatch(cocos2d::CCSpriteBatchNode* batch) const {
    if (!enabled || !batch)
        return false;

    auto state = iosState(const_cast<Renderer*>(this));
    if (!state || !state->resolvedState)
        return false;

    auto descendants = batch->getDescendants();
    if (!descendants)
        return false;

    for (u32 i = 0; i < descendants->count(); ++i) {
        auto sprite = typeinfo_cast<cocos2d::CCSprite*>(descendants->objectAtIndex(i));
        if (sprite && sprite->getBatchNode() == batch &&
            state->resolvedState->isForcedDecorationSprite(sprite))
            return true;
    }
    return false;
}

bool Renderer::prepareGPUOwnedSprite(cocos2d::CCSprite* sprite) {
    if (!enabled || !AtlasInterleaveRegistry::shouldSkipTransform(this, sprite))
        return false;
    if (auto state = iosState(this))
        ++state->batchTransformSkipsCurrentFrame;
    return true;
}

bool Renderer::isGPUOwnedStandaloneSprite(cocos2d::CCSprite* sprite) const {
    if (!enabled || !sprite)
        return false;

    auto state = iosState(const_cast<Renderer*>(this));
    if (!state || !state->resolvedState || sprite->getBatchNode())
        return false;

    auto object = typeinfo_cast<GameObject*>(sprite);
    if (!object)
        return false;

    auto it = state->standaloneRootBatchIndices.find(object);
    if (it == state->standaloneRootBatchIndices.end() || it->second >= state->standaloneBatches.size())
        return false;

    auto& buffer = state->standaloneBatches[it->second];
    if (!buffer || !state->resolvedState->canDrawSprite(sprite)) {
        GPUTruth::recordObjectFailure(const_cast<Renderer*>(this));
        return true;
    }
    const_cast<Renderer*>(this)->prepareGPUFrame();
    if (!buffer->drawRoot(object)) {
        GPUTruth::recordObjectFailure(const_cast<Renderer*>(this));
        return true; // Suppress stock redraw for this owned root, even on failure.
    }

    ++state->standaloneRootVisitsCurrentFrame;
    return true;
}

void Renderer::setEnabled(bool value) {
    auto state = iosState(this);

    if (!value) {
        enabled = false;
        if (state) {
            for (auto& gpuBatch : state->gpuBatches) {
                if (gpuBatch)
                    gpuBatch->setVisible(false);
            }
            for (auto& gpuBuffer : state->standaloneBatches) {
                if (gpuBuffer)
                    gpuBuffer->setVisible(false);
            }
            restoreOwnedStockQuads(state);
        }
    } else {
        enabled = true;
        if (state) {
            for (auto& gpuBatch : state->gpuBatches) {
                if (gpuBatch)
                    gpuBatch->setVisible(true);
            }
            for (auto& gpuBuffer : state->standaloneBatches) {
                if (gpuBuffer)
                    gpuBuffer->setVisible(true);
            }
        }
    }

    setVisible(false);
    updateDebugText();
}

void Renderer::reset() {
    AreaVisualState::reset();
    if (auto state = iosState(this); state && state->resolvedState) {
        state->resolvedState->resync();
    }
}

void Renderer::drawLine(const glm::vec2&, const glm::vec2&, const glm::vec4&) {}

void storeGLStates() {}
void restoreGLStates() {}

#endif
