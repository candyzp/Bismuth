#ifdef GEODE_IS_IOS

#include "../Renderer.hpp"
#include "../AreaVisualState.hpp"
#include "ResolvedStateLayer.hpp"
#include "AssistShadowBatch.hpp"
#include "StandaloneAssistBatch.hpp"
#include "AtlasInterleave.hpp"

#include "Geode/cocos/CCDirector.h"
#include "Geode/cocos/sprite_nodes/CCSpriteBatchNode.h"
#include <Geode/utils/cocos.hpp>

#include <cstring>
#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {
constexpr usize MAX_STANDALONE_BUFFER_SPRITES = 16383;

struct StandaloneObjectDesc {
    GameObject* root = nullptr;
    std::vector<ResolvedStateLayer::ShadowCandidate> candidates;
};

struct StandaloneChunkDesc {
    std::vector<GameObject*> roots;
    std::vector<ResolvedStateLayer::ShadowCandidate> candidates;
};

struct IOSRendererState {
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

    usize deferredAtlasObjects = 0;
    usize deferredAtlasUnmapped = 0;
    usize deferredAtlasBatchNodes = 0;
    usize deferredAtlasBufferCount = 0;

    usize batchTransformSkipsCurrentFrame = 0;
    usize batchTransformSkipsLastFrame = 0;
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
            std::unordered_map<GameObject*, std::vector<ResolvedStateLayer::ShadowCandidate>> candidatesByObject;
            std::unordered_map<cocos2d::CCSprite*, GameObject*> firstSpriteOwner;
            std::unordered_set<GameObject*> sharedVisualObjects;
            candidateBatches.reserve(64);
            candidatesByObject.reserve(state->resolvedState->getStats().safeObjects);
            firstSpriteOwner.reserve(candidates.size());

            for (const auto& candidate : candidates) {
                auto object = candidate.object;
                auto sprite = candidate.sprite;
                if (!object || !sprite)
                    continue;

                candidatesByObject[object].push_back(candidate);

                const auto [ownerIt, insertedOwner] = firstSpriteOwner.emplace(sprite, object);
                if (!insertedOwner && ownerIt->second != object) {
                    sharedVisualObjects.insert(object);
                    sharedVisualObjects.insert(ownerIt->second);
                }

                if (auto batch = sprite->getBatchNode()) {
                    ++state->candidatesWithBatch;
                    candidateBatches.insert(batch);
                } else {
                    ++state->candidatesWithoutBatch;
                }
            }

            state->candidateBatchNodes = candidateBatches.size();

            std::vector<StandaloneObjectDesc> standaloneObjects;
            std::unordered_map<cocos2d::CCSpriteBatchNode*, std::vector<StandaloneObjectDesc>> deferredAtlasObjectsByBatch;
            std::unordered_map<cocos2d::CCSpriteBatchNode*, cocos2d::CCNode*> gpuInsertionTails;
            standaloneObjects.reserve(candidatesByObject.size());
            deferredAtlasObjectsByBatch.reserve(32);
            gpuInsertionTails.reserve(64);

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

                if (anyAtlas) {
                    ++state->standaloneMixedRejected;
                    continue;
                }

                if (duplicateVisual) {
                    ++state->standaloneDuplicateRejected;
                    continue;
                }

                if (sharedVisualObjects.contains(object)) {
                    ++state->standaloneSharedRejected;
                    continue;
                }

                // External color/glow sprites are legitimate GD arrangements,
                // but a single root/batch handoff cannot suppress them completely.
                if (externalVisual) {
                    ++state->standaloneExternalRejected;
                    if (externalGlow)
                        ++state->standaloneExternalGlowObjects;
                    if (externalColor)
                        ++state->standaloneExternalColorObjects;
                    if (externalOther)
                        ++state->standaloneExternalOtherObjects;
                    continue;
                }

                if (invalidVisual) {
                    ++state->standaloneInvalidVisualRejected;
                    continue;
                }

                if (object->getBatchNode()) {
                    ++state->standaloneRootBatchRejected;
                    continue;
                }

                auto parent = object->getParent();
                if (!parent) {
                    ++state->standaloneParentlessAtInit;

                    // Source research + device counters showed that GD removes
                    // these roots while inactive and later addMainSpriteToParent()
                    // inserts them into parentForZLayer(). Only promote the simple
                    // one-root-sprite shape here; more complex visual trees remain
                    // stock until we have an equally exact render-home proof.
                    const bool simpleDeferredRoot =
                        objectCandidates.size() == 1 &&
                        objectCandidates[0].sprite == static_cast<cocos2d::CCSprite*>(object);
                    if (!simpleDeferredRoot || !layer->m_batchNodes) {
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
                    deferredAtlasObjectsByBatch[targetBatch].push_back({ object, objectCandidates });
                    continue;
                }

                // A genuinely parented non-batch root can still use the root-visit
                // path. This is separate from the parentless/deferred-atlas case.
                ++state->standaloneObjectEligible;
                standaloneObjects.push_back({ object, objectCandidates });
            }

            for (auto batch : candidateBatches) {
                if (!batch)
                    continue;

                auto parent = batch->getParent();
                if (!parent) {
                    ++state->batchesWithoutParent;
                    continue;
                }

                auto gpuBatch = AssistShadowBatch::create(
                    state->resolvedState.get(),
                    state->assistShader,
                    batch
                );
                if (!gpuBatch || !gpuBatch->getStats().ready || gpuBatch->getStats().batchedSprites == 0)
                    continue;

                // Same Z is not enough. Preserve the stock sibling ordering by
                // placing the GPU geometry directly after the exact atlas node it
                // shadows. Later deferred chunks for this batch continue the chain.
                parent->insertAfter(gpuBatch, batch);
                gpuInsertionTails[batch] = gpuBatch;
                state->gpuBatches.push_back(gpuBatch);
                for (auto sprite : gpuBatch->getOwnedSprites()) {
                    if (!sprite)
                        continue;
                    state->batchOwnedSprites.insert(sprite);
                    state->ownedSprites.insert(sprite);
                }
            }

            // Parentless-at-init roots are not really standalone. Build their
            // geometry once, grouped by the exact stock batch GD will later use,
            // and attach one shared buffer beside that batch. The shader consumes
            // stock visibility, so inactive/offscreen records cost no pixels.
            state->deferredAtlasBatchNodes = deferredAtlasObjectsByBatch.size();
            for (auto& [targetBatch, objects] : deferredAtlasObjectsByBatch) {
                if (!targetBatch)
                    continue;

                auto parent = targetBatch->getParent();
                if (!parent) {
                    state->deferredAtlasUnmapped += objects.size();
                    continue;
                }

                cocos2d::CCNode* insertionAnchor = targetBatch;
                if (auto tailIt = gpuInsertionTails.find(targetBatch); tailIt != gpuInsertionTails.end() && tailIt->second)
                    insertionAnchor = tailIt->second;

                const auto chunks = buildStandaloneChunks(objects);
                for (const auto& chunk : chunks) {
                    if (chunk.candidates.empty())
                        continue;

                    auto gpuBuffer = StandaloneAssistBatch::create(
                        state->resolvedState.get(),
                        state->assistShader,
                        chunk.candidates,
                        false // Explicit deferred-atlas ownership, even if GD reparents a root.
                    );
                    if (!gpuBuffer || !gpuBuffer->getStats().ready)
                        continue;
                    if (gpuBuffer->getOwnedSprites().size() != chunk.candidates.size())
                        continue;

                    parent->insertAfter(gpuBuffer, insertionAnchor);
                    insertionAnchor = gpuBuffer;
                    state->standaloneBatches.push_back(gpuBuffer);
                    ++state->deferredAtlasBufferCount;

                    for (auto sprite : gpuBuffer->getOwnedSprites()) {
                        if (!sprite)
                            continue;
                        state->deferredAtlasOwnedSprites.insert(sprite);
                        state->batchOwnedSprites.insert(sprite);
                        state->ownedSprites.insert(sprite);
                    }
                }

                gpuInsertionTails[targetBatch] = insertionAnchor;
            }

            // Preserve the true standalone root path only for objects that were
            // actually parented outside a batch at init.
            const auto standaloneChunks = buildStandaloneChunks(standaloneObjects);
            for (const auto& chunk : standaloneChunks) {
                if (chunk.roots.empty() || chunk.candidates.empty())
                    continue;

                auto gpuBuffer = StandaloneAssistBatch::create(
                    state->resolvedState.get(),
                    state->assistShader,
                    chunk.candidates
                );
                if (!gpuBuffer || !gpuBuffer->getStats().ready)
                    continue;

                if (gpuBuffer->getOwnedSprites().size() != chunk.candidates.size())
                    continue;

                const usize batchIndex = state->standaloneBatches.size();
                state->standaloneBatches.push_back(gpuBuffer);

                for (auto root : chunk.roots) {
                    if (!root || !gpuBuffer->ownsRoot(root))
                        continue;
                    state->standaloneOwnedRoots.insert(root);
                    state->standaloneRootBatchIndices[root] = batchIndex;
                }

                for (auto sprite : gpuBuffer->getOwnedSprites()) {
                    if (!sprite)
                        continue;
                    state->standaloneOwnedSprites.insert(sprite);
                    state->ownedSprites.insert(sprite);
                }
            }

            state->standaloneBufferCount = state->standaloneBatches.size();
            state->resolvedState->setGPUOwnedSprites(state->ownedSprites);

            log::info(
                "Bismuth iOS ownership: {} candidates ({} atlas-now / {} parentless-or-standalone); standalone {} candidates / {} ownership-eligible; rejects mixed {} / duplicate {} / shared {} / external {} (glow {} / color {} / other {}) / invalid {} / root-batched {}; parentless-at-init {} -> deferred atlas {} object(s), {} target batch(es), {} buffer(s), {} unmapped; {} immediate atlas node(s), {} true standalone root(s), {} total GPU sprite(s)",
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
                state->gpuBatches.size(),
                state->standaloneOwnedRoots.size(),
                state->ownedSprites.size()
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

    log::info("Bismuth iOS initialized: immediate + deferred atlas GPU math, stock visibility/animation lifecycle");
    return true;
}

void Renderer::generateBatchNodes() {}

void Renderer::terminate() {
    if (currentRenderer == this)
        currentRenderer = nullptr;
    enabled = false;

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
        const bool ready = state->assistShader && state->resolvedState &&
            state->resolvedState->isGPUStateReady();
        const char* status = !enabled ? "OFF" : !ready ? "UNAVAILABLE" : calls ? "ACTIVE" : "IDLE";
        text = fmt::format(
            "Bismuth GPU [{}]\nGPU Draw: {} sprites/frame\nCalls: {} | CPU Saved: {}",
            status, indices / 6, calls,
            state->batchTransformSkipsLastFrame + state->standaloneRootVisitsLastFrame
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

void Renderer::beginGPUFrame() {
    auto state = iosState(this);
    if (!state)
        return;
    state->gpuFramePrepared = false;
    state->standaloneRootVisitsCurrentFrame = 0;
    state->batchTransformSkipsCurrentFrame = 0;
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
    // Also retire deactivations on frames with no eligible GPU draw.
    prepareGPUFrame();
    state->standaloneRootVisitsLastFrame = state->standaloneRootVisitsCurrentFrame;
    state->batchTransformSkipsLastFrame = state->batchTransformSkipsCurrentFrame;
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
    if (!state || !state->suspended)
        return;
    if (currentRenderer && currentRenderer != this)
        currentRenderer->suspendGPU();
    currentRenderer = this;
    if (state->resolvedState) {
        state->resolvedState->setCurrent(true);
        state->resolvedState->resync();
        state->resolvedState->reseedActiveFromStock();
    }
    state->suspended = false;
    beginGPUFrame();
    setEnabled(state->enabledBeforeSuspend && Mod::get()->getSettingValue<bool>("enabled"));
}

bool Renderer::useOptimizations() {
    return false;
}

bool Renderer::isGPUOwnedSprite(cocos2d::CCSprite* sprite) const {
    if (!enabled || !sprite)
        return false;

    auto state = iosState(const_cast<Renderer*>(this));
    if (!state || !state->batchOwnedSprites.contains(sprite) ||
        !state->resolvedState || !state->resolvedState->canDrawSprite(sprite))
        return false;

    // A deferred-atlas sprite is only allowed to bypass stock matrix expansion
    // after GD has actually inserted it into a stock batch.
    if (state->deferredAtlasOwnedSprites.contains(sprite) && !sprite->getBatchNode())
        return false;

    return true;
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
    if (!state || !state->resolvedState || !state->resolvedState->canDrawSprite(sprite) ||
        sprite->getBatchNode())
        return false;

    auto object = typeinfo_cast<GameObject*>(sprite);
    if (!object)
        return false;

    auto it = state->standaloneRootBatchIndices.find(object);
    if (it == state->standaloneRootBatchIndices.end() || it->second >= state->standaloneBatches.size())
        return false;

    auto& buffer = state->standaloneBatches[it->second];
    if (!buffer)
        return false;
    const_cast<Renderer*>(this)->prepareGPUFrame();
    if (!buffer->drawRoot(object))
        return false;

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
