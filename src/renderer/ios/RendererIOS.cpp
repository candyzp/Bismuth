#ifdef GEODE_IS_IOS

#include "../Renderer.hpp"
#include "../AreaVisualState.hpp"
#include "ResolvedStateLayer.hpp"
#include "AssistShadowBatch.hpp"
#include "StandaloneAssistBatch.hpp"

#include "Geode/cocos/CCDirector.h"
#include "Geode/cocos/sprite_nodes/CCSpriteBatchNode.h"

#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {
constexpr usize MAX_STANDALONE_RUN_SPRITES = 16383;

struct StandaloneObjectDesc {
    GameObject* root = nullptr;
    std::vector<ResolvedStateLayer::ShadowCandidate> candidates;
};

struct StandaloneRunDesc {
    cocos2d::CCNode* parent = nullptr;
    int zOrder = 0;
    unsigned int orderOfArrival = 0;
    std::vector<GameObject*> roots;
    std::vector<ResolvedStateLayer::ShadowCandidate> candidates;
};

struct IOSRendererState {
    std::unique_ptr<ColorChannelBuffer> colorChannels = std::make_unique<ColorChannelBuffer>();
    std::unique_ptr<ResolvedStateLayer> resolvedState;
    Shader* assistShader = nullptr;

    std::vector<Ref<AssistShadowBatch>> gpuBatches;
    std::vector<Ref<StandaloneAssistBatch>> standaloneBatches;

    std::unordered_set<cocos2d::CCSprite*> batchOwnedSprites;
    std::unordered_set<cocos2d::CCSprite*> standaloneOwnedSprites;
    std::unordered_set<cocos2d::CCSprite*> ownedSprites;
    std::unordered_set<GameObject*> standaloneOwnedRoots;

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
    usize standaloneMissingParentRejected = 0;
    usize standaloneRootBatchRejected = 0;
    usize standaloneParentCount = 0;
    usize standaloneRunCount = 0;

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

static std::string byteSizeToString(usize size) {
    if (size < 1024)
        return fmt::format("{} B", size);
    if (size < 1024 * 1024)
        return fmt::format("{:.2f} KiB", (double)size / 1024.0);
    return fmt::format("{:.2f} MiB", (double)size / (1024.0 * 1024.0));
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

static void parkOwnedStockQuads(IOSRendererState* state) {
    if (!state)
        return;

    // Only sprites actually living in a CCSpriteBatchNode have atlas quads to
    // park. Standalone objects are suppressed at the root visit stage.
    for (auto sprite : state->batchOwnedSprites) {
        if (!sprite)
            continue;

        auto atlas = sprite->getTextureAtlas();
        const auto atlasIndex = sprite->getAtlasIndex();
        if (!atlas || atlasIndex == CCSpriteIndexNotInitialized || atlasIndex >= atlas->getTotalQuads())
            continue;

        cocos2d::ccV3F_C4B_T2F_Quad parked {};
        atlas->updateQuad(&parked, atlasIndex);
        sprite->setDirty(false);
    }
}

static void restoreOwnedStockQuads(IOSRendererState* state) {
    if (!state)
        return;

    // Manual in-level disable only. The scene is known to still be alive here.
    for (auto sprite : state->batchOwnedSprites) {
        if (!sprite || !sprite->getBatchNode())
            continue;
        sprite->setDirty(true);
        sprite->updateTransform();
    }
}

static std::vector<StandaloneRunDesc> buildStandaloneRuns(
    const std::unordered_map<cocos2d::CCNode*, StandaloneObjectDesc>& lookup,
    const std::unordered_set<cocos2d::CCNode*>& parents
) {
    std::vector<StandaloneRunDesc> runs;

    for (auto parent : parents) {
        if (!parent || parent->getChildrenCount() == 0)
            continue;

        parent->sortAllChildren();
        auto children = parent->getChildren();
        if (!children)
            continue;

        StandaloneRunDesc current;
        bool hasRun = false;

        auto flush = [&]() {
            if (!current.roots.empty() && !current.candidates.empty())
                runs.push_back(std::move(current));
            current = {};
            hasRun = false;
        };

        for (auto child : CCArrayExt<cocos2d::CCNode*>(children)) {
            auto it = lookup.find(child);
            if (it == lookup.end()) {
                flush();
                continue;
            }

            const auto& objectDesc = it->second;
            if (!objectDesc.root || objectDesc.candidates.empty()) {
                flush();
                continue;
            }

            const int zOrder = objectDesc.root->getZOrder();
            const bool wouldOverflow =
                hasRun &&
                current.candidates.size() + objectDesc.candidates.size() > MAX_STANDALONE_RUN_SPRITES;

            if (hasRun && (zOrder != current.zOrder || wouldOverflow))
                flush();

            if (!hasRun) {
                current.parent = parent;
                current.zOrder = zOrder;
                current.orderOfArrival = objectDesc.root->getOrderOfArrival();
                hasRun = true;
            }

            // Append complete objects only. A run is never split through the
            // middle of a GameObject visual subtree.
            current.roots.push_back(objectDesc.root);
            current.candidates.insert(
                current.candidates.end(),
                objectDesc.candidates.begin(),
                objectDesc.candidates.end()
            );
        }

        flush();
    }

    return runs;
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

            // First establish the real render home of every safe visual sprite.
            // This is also used to prove that a standalone object is complete
            // before its root visit is ever suppressed.
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

            std::unordered_map<cocos2d::CCNode*, StandaloneObjectDesc> standaloneLookup;
            std::unordered_set<cocos2d::CCNode*> standaloneParents;
            standaloneLookup.reserve(candidatesByObject.size());
            standaloneParents.reserve(64);

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

                // Do not split one visual object between atlas and standalone
                // ownership. The already-working atlas path can keep doing its
                // thing, but standalone root suppression requires the full tree.
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
                // but skipping only the GameObject root cannot suppress them.
                // Keep them stock for now and report exactly what kind they are.
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

                auto parent = object->getParent();
                if (!parent) {
                    ++state->standaloneMissingParentRejected;
                    continue;
                }

                if (object->getBatchNode()) {
                    ++state->standaloneRootBatchRejected;
                    continue;
                }

                ++state->standaloneObjectEligible;
                standaloneLookup.emplace(
                    static_cast<cocos2d::CCNode*>(object),
                    StandaloneObjectDesc { object, objectCandidates }
                );
                standaloneParents.insert(parent);
            }

            state->standaloneParentCount = standaloneParents.size();

            // Runs are captured before adding any assist nodes. Unsupported or
            // animated siblings naturally split a run, preserving Cocos order.
            const auto standaloneRuns = buildStandaloneRuns(
                standaloneLookup,
                standaloneParents
            );
            state->standaloneRunCount = standaloneRuns.size();

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

                parent->addChild(gpuBatch, batch->getZOrder());
                state->gpuBatches.push_back(gpuBatch);
                for (auto sprite : gpuBatch->getOwnedSprites()) {
                    if (!sprite)
                        continue;
                    state->batchOwnedSprites.insert(sprite);
                    state->ownedSprites.insert(sprite);
                }
            }

            for (const auto& run : standaloneRuns) {
                if (!run.parent || run.roots.empty() || run.candidates.empty())
                    continue;

                auto gpuRun = StandaloneAssistBatch::create(
                    state->resolvedState.get(),
                    state->assistShader,
                    run.candidates
                );
                if (!gpuRun || !gpuRun->getStats().ready)
                    continue;

                // The batch builder is all-or-nothing. Do not suppress any root
                // unless every visual sprite in this run made it into GPU geometry.
                if (gpuRun->getOwnedSprites().size() != run.candidates.size())
                    continue;

                run.parent->addChild(gpuRun, run.zOrder);
                gpuRun->setOrderOfArrival(run.orderOfArrival);
                state->standaloneBatches.push_back(gpuRun);

                for (auto root : run.roots) {
                    if (root)
                        state->standaloneOwnedRoots.insert(root);
                }

                for (auto sprite : gpuRun->getOwnedSprites()) {
                    if (!sprite)
                        continue;
                    state->standaloneOwnedSprites.insert(sprite);
                    state->ownedSprites.insert(sprite);
                }
            }

            // Only records that actually feed visible GPU geometry are polled in
            // the per-frame state hot path.
            state->resolvedState->setGPUOwnedSprites(state->ownedSprites);

            log::info(
                "Bismuth iOS ownership: {} sprite candidates ({} atlas / {} standalone); standalone {} candidates / {} eligible; rejects mixed {} / duplicate {} / shared {} / external {} (glow {} / color {} / other {}) / invalid {} / no-parent {} / root-batched {}; {} atlas GPU nodes + {} standalone runs; {} roots / {} standalone visual sprites owned",
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
                state->standaloneMissingParentRejected,
                state->standaloneRootBatchRejected,
                state->gpuBatches.size(),
                state->standaloneBatches.size(),
                state->standaloneOwnedRoots.size(),
                state->standaloneOwnedSprites.size()
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
    for (auto& gpuRun : state->standaloneBatches) {
        if (gpuRun)
            gpuRun->setVisible(true);
    }
    parkOwnedStockQuads(state);

    setVisible(false);
    rendererStartTime = getTime();

    log::info("Bismuth iOS initialized: complete standalone subtree + atlas GPU math ownership, stock animation lifecycle");
    return true;
}

void Renderer::generateBatchNodes() {}

void Renderer::terminate() {
    if (currentRenderer == this)
        currentRenderer = nullptr;
    enabled = false;

    auto state = iosState(this);
    if (state) {
        // Never call into scene sprites/parents during teardown. The PlayLayer may
        // already be partially destroyed; simply disable and release our refs.
        for (auto& gpuBatch : state->gpuBatches) {
            if (gpuBatch)
                gpuBatch->setVisible(false);
        }
        for (auto& gpuRun : state->standaloneBatches) {
            if (gpuRun)
                gpuRun->setVisible(false);
        }

        state->ownedSprites.clear();
        state->batchOwnedSprites.clear();
        state->standaloneOwnedSprites.clear();
        state->standaloneOwnedRoots.clear();
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
    if (!debugText)
        return;

    std::string text;
    if (Mod::get()->getSettingValue<bool>("ios_gpu_debug")) {
        const char* gpuRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        auto state = iosState(this);
        auto resolved = state ? state->resolvedState.get() : nullptr;

        if (resolved) {
            const auto& stats = resolved->getStats();

            usize residentVertices = 0;
            usize gpuDraws = 0;
            usize gpuIndices = 0;
            usize textureRanges = 0;
            usize rejectedSprites = 0;

            if (state) {
                for (const auto& batch : state->gpuBatches) {
                    if (!batch)
                        continue;
                    const auto& s = batch->getStats();
                    residentVertices += s.verticesResident;
                    gpuDraws += s.drawCallsLastFrame;
                    gpuIndices += s.indicesLastFrame;
                    textureRanges += s.textureBatches;
                    rejectedSprites += s.rejectedSprites;
                }

                for (const auto& run : state->standaloneBatches) {
                    if (!run)
                        continue;
                    const auto& s = run->getStats();
                    residentVertices += s.verticesResident;
                    gpuDraws += s.drawCallsLastFrame;
                    gpuIndices += s.indicesLastFrame;
                    textureRanges += s.textureRanges;
                }
            }

            const usize atlasBatchCount = state ? state->gpuBatches.size() : 0;
            const usize standaloneBatchCount = state ? state->standaloneBatches.size() : 0;
            const usize ownedSprites = state ? state->ownedSprites.size() : 0;
            const usize batchOwned = state ? state->batchOwnedSprites.size() : 0;
            const usize standaloneOwned = state ? state->standaloneOwnedSprites.size() : 0;
            const usize standaloneRoots = state ? state->standaloneOwnedRoots.size() : 0;
            const bool ownsPixels = enabled && ownedSprites > 0;

            text = fmt::format(
                "Bismuth iOS GPU Assist\n"
                "Visible output: {}\n"
                "GPU: {}\n"
                "Resolved GPU state: {} | transform shader: {}\n"
                "Safe objects: {} ({} static / {} dynamic)\n"
                "Stock animation/complex: {} | safe sprite records: {}\n"
                "Collection safety: {} unsafe object(s) | {} non-sprite child | {} duplicate | {} invalid | init retained {} obj / {} sprites | revalidate {}\n"
                "Discovery: {} sprites | {} atlas | {} standalone\n"
                "Standalone objects: {} candidates | {} eligible\n"
                "Reject: {} mixed | {} duplicate | {} shared | {} external [glow {} / color {} / other {}] | {} invalid | {} no-parent | {} root-batched\n"
                "GPU nodes: {} atlas + {} standalone runs | owned roots: {} | visual sprites: {} ({} atlas + {} standalone)\n"
                "Active GPU state: {} objects | {} sprites\n"
                "Dirty: {} transform | {} appearance | {} visibility | {} UV\n"
                "Static GPU reused: {}/{} | uploads: {} in {} call(s)\n"
                "Resident verts: {} | GPU draws: {} / frame | indices: {} | ranges: {} | rejected: {}\n"
                "CPU render skipped: {} atlas quad transforms | {} complete standalone root visits\n"
                "Framebuffer writes: {}\n"
                "Animation lifecycle ownership: STOCK GD",
                ownsPixels ? "GPU SAFE SPRITES + STOCK ANIMATION" : "STOCK GD",
                gpuRenderer ? gpuRenderer : "unknown",
                resolved->isGPUStateReady() ? "READY" : "UNAVAILABLE",
                state && state->assistShader ? "READY" : "UNAVAILABLE",
                stats.safeObjects,
                stats.staticObjects,
                stats.dynamicObjects,
                stats.stockObjects,
                stats.safeSprites,
                stats.unsafeCollectionObjects,
                stats.invalidChildNodes,
                stats.duplicateSpriteRecords,
                stats.invalidSpriteRecords,
                stats.retainedInitObjects,
                stats.retainedInitSprites,
                stats.initRevalidationFailures,
                state ? state->gpuCandidateSprites : 0,
                state ? state->candidatesWithBatch : 0,
                state ? state->candidatesWithoutBatch : 0,
                state ? state->standaloneObjectCandidates : 0,
                state ? state->standaloneObjectEligible : 0,
                state ? state->standaloneMixedRejected : 0,
                state ? state->standaloneDuplicateRejected : 0,
                state ? state->standaloneSharedRejected : 0,
                state ? state->standaloneExternalRejected : 0,
                state ? state->standaloneExternalGlowObjects : 0,
                state ? state->standaloneExternalColorObjects : 0,
                state ? state->standaloneExternalOtherObjects : 0,
                state ? state->standaloneInvalidVisualRejected : 0,
                state ? state->standaloneMissingParentRejected : 0,
                state ? state->standaloneRootBatchRejected : 0,
                atlasBatchCount,
                standaloneBatchCount,
                standaloneRoots,
                ownedSprites,
                batchOwned,
                standaloneOwned,
                stats.activeGPUObjects,
                stats.activeGPUSprites,
                stats.dirtyTransforms,
                stats.dirtyAppearance,
                stats.dirtyVisibility,
                stats.dirtyUVs,
                stats.staticObjectsReused,
                stats.activeStaticObjects,
                byteSizeToString(stats.bytesUploaded),
                stats.uploadCalls,
                residentVertices,
                gpuDraws,
                gpuIndices,
                textureRanges,
                rejectedSprites,
                batchOwned,
                standaloneRoots,
                ownsPixels ? "ON (owned safe sprites)" : "OFF"
            );
        } else {
            text = fmt::format(
                "Bismuth iOS GPU Assist\n"
                "Visible output: STOCK GD\n"
                "GPU: {}\n"
                "Resolved GPU state: UNAVAILABLE\n"
                "GPU transform shader: UNAVAILABLE\n"
                "Animation lifecycle ownership: STOCK GD",
                gpuRenderer ? gpuRenderer : "unknown"
            );
        }
    }

    debugText->setString(text.c_str());
    debugTextOutline1->setString(text.c_str());
    debugTextOutline2->setString(text.c_str());
}

Shader* Renderer::prepareDraw() { return nullptr; }
void Renderer::finishDraw() {}

void Renderer::update(float dt) {
    gameTimer += dt;

    const bool detailedProbe = Mod::get()->getSettingValue<bool>("ios_gpu_debug");
    if (auto state = iosState(this); state && state->resolvedState) {
        const bool gpuConsumesResolvedState = enabled && !state->ownedSprites.empty();
        state->resolvedState->update(detailedProbe || gpuConsumesResolvedState);
    }

    updateDebugText();
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

bool Renderer::useOptimizations() {
    return false;
}

bool Renderer::isGPUOwnedSprite(cocos2d::CCSprite* sprite) const {
    if (!enabled || !sprite)
        return false;

    auto state = iosState(const_cast<Renderer*>(this));
    return state && state->batchOwnedSprites.contains(sprite);
}

bool Renderer::isGPUOwnedStandaloneSprite(cocos2d::CCSprite* sprite) const {
    if (!enabled || !sprite)
        return false;

    auto state = iosState(const_cast<Renderer*>(this));
    if (!state)
        return false;

    auto object = typeinfo_cast<GameObject*>(sprite);
    return object && state->standaloneOwnedRoots.contains(object);
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
            for (auto& gpuRun : state->standaloneBatches) {
                if (gpuRun)
                    gpuRun->setVisible(false);
            }
            restoreOwnedStockQuads(state);
        }
    } else {
        enabled = true;
        if (state) {
            parkOwnedStockQuads(state);
            for (auto& gpuBatch : state->gpuBatches) {
                if (gpuBatch)
                    gpuBatch->setVisible(true);
            }
            for (auto& gpuRun : state->standaloneBatches) {
                if (gpuRun)
                    gpuRun->setVisible(true);
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
        if (enabled)
            parkOwnedStockQuads(state);
    }
}

void Renderer::drawLine(const glm::vec2&, const glm::vec2&, const glm::vec4&) {}

void storeGLStates() {}
void restoreGLStates() {}

#endif