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
struct StandaloneRunDesc {
    cocos2d::CCNode* parent = nullptr;
    int zOrder = 0;
    unsigned int orderOfArrival = 0;
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

    usize gpuCandidateSprites = 0;
    usize candidatesWithBatch = 0;
    usize candidatesWithoutBatch = 0;
    usize candidateBatchNodes = 0;
    usize batchesWithoutParent = 0;

    usize standaloneLeafCandidates = 0;
    usize standaloneUnsupported = 0;
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

static void parkOwnedStockQuads(IOSRendererState* state) {
    if (!state)
        return;

    // Only sprites actually living in a CCSpriteBatchNode have atlas quads to
    // park. Standalone ownership is suppressed by the CCSprite::visit hook.
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
    IOSRendererState* state,
    const std::unordered_map<cocos2d::CCSprite*, ResolvedStateLayer::ShadowCandidate>& lookup,
    const std::unordered_set<cocos2d::CCNode*>& parents
) {
    std::vector<StandaloneRunDesc> runs;
    if (!state)
        return runs;

    for (auto parent : parents) {
        if (!parent || parent->getChildrenCount() == 0)
            continue;

        parent->sortAllChildren();
        auto children = parent->getChildren();
        if (!children)
            continue;

        StandaloneRunDesc current;
        cocos2d::ccBlendFunc currentBlend {0, 0};
        bool hasRun = false;

        auto flush = [&]() {
            if (!current.candidates.empty())
                runs.push_back(std::move(current));
            current = {};
            currentBlend = {0, 0};
            hasRun = false;
        };

        for (auto child : CCArrayExt<cocos2d::CCNode*>(children)) {
            auto sprite = typeinfo_cast<cocos2d::CCSprite*>(child);
            auto it = sprite ? lookup.find(sprite) : lookup.end();
            if (it == lookup.end()) {
                flush();
                continue;
            }

            const auto blend = sprite->getBlendFunc();
            const int zOrder = sprite->getZOrder();
            if (hasRun && (
                zOrder != current.zOrder ||
                blend.src != currentBlend.src ||
                blend.dst != currentBlend.dst
            )) {
                flush();
            }

            if (!hasRun) {
                current.parent = parent;
                current.zOrder = zOrder;
                current.orderOfArrival = sprite->getOrderOfArrival();
                currentBlend = blend;
                hasRun = true;
            }

            current.candidates.push_back(it->second);
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
            std::unordered_map<cocos2d::CCSprite*, ResolvedStateLayer::ShadowCandidate> standaloneLookup;
            std::unordered_set<cocos2d::CCNode*> standaloneParents;
            candidateBatches.reserve(64);
            standaloneLookup.reserve(candidates.size());
            standaloneParents.reserve(64);

            for (const auto& candidate : candidates) {
                auto object = candidate.object;
                auto sprite = candidate.sprite;
                if (!object || !sprite)
                    continue;

                if (auto batch = sprite->getBatchNode()) {
                    ++state->candidatesWithBatch;
                    candidateBatches.insert(batch);
                    continue;
                }

                ++state->candidatesWithoutBatch;

                // First standalone phase: only a root leaf sprite. This lets us
                // skip CCSprite::visit entirely without losing child traversal or
                // interfering with glow/detail/animation hierarchies.
                auto parent = sprite->getParent();
                const bool leafRoot =
                    parent &&
                    sprite == static_cast<cocos2d::CCSprite*>(object) &&
                    sprite->getChildrenCount() == 0 &&
                    sprite->getTexture() != nullptr;

                if (!leafRoot) {
                    ++state->standaloneUnsupported;
                    continue;
                }

                ++state->standaloneLeafCandidates;
                standaloneLookup.emplace(sprite, candidate);
                standaloneParents.insert(parent);
            }

            state->candidateBatchNodes = candidateBatches.size();
            state->standaloneParentCount = standaloneParents.size();

            // Capture parent/z/order runs before inserting any assist nodes. A
            // run is only consecutive safe siblings, so an unsafe/animated node
            // naturally splits the batch and keeps its original draw position.
            const auto standaloneRuns = buildStandaloneRuns(
                state,
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
                if (!run.parent || run.candidates.empty())
                    continue;

                auto gpuRun = StandaloneAssistBatch::create(
                    state->resolvedState.get(),
                    state->assistShader,
                    run.candidates
                );
                if (!gpuRun || !gpuRun->getStats().ready || gpuRun->getStats().sprites == 0)
                    continue;

                // Drop the replacement node into the exact sibling ordering slot
                // occupied by the first safe sprite in this consecutive run.
                run.parent->addChild(gpuRun, run.zOrder);
                gpuRun->setOrderOfArrival(run.orderOfArrival);
                state->standaloneBatches.push_back(gpuRun);

                for (auto sprite : gpuRun->getOwnedSprites()) {
                    if (!sprite)
                        continue;
                    state->standaloneOwnedSprites.insert(sprite);
                    state->ownedSprites.insert(sprite);
                }
            }

            state->resolvedState->setGPUOwnedSprites(state->ownedSprites);

            log::info(
                "Bismuth iOS ownership: {} candidates, {} atlas candidates, {} standalone leaf candidates, {} unsupported standalone; {} atlas GPU nodes + {} standalone runs; {} total owned sprites",
                state->gpuCandidateSprites,
                state->candidatesWithBatch,
                state->standaloneLeafCandidates,
                state->standaloneUnsupported,
                state->gpuBatches.size(),
                state->standaloneBatches.size(),
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
    for (auto& gpuRun : state->standaloneBatches) {
        if (gpuRun)
            gpuRun->setVisible(true);
    }
    parkOwnedStockQuads(state);

    setVisible(false);
    rendererStartTime = getTime();

    log::info("Bismuth iOS initialized: atlas + standalone GPU math ownership, stock animation lifecycle");
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
            const bool ownsPixels = enabled && ownedSprites > 0;

            text = fmt::format(
                "Bismuth iOS GPU Assist\n"
                "Visible output: {}\n"
                "GPU: {}\n"
                "Resolved GPU state: {} | transform shader: {}\n"
                "Safe objects: {} ({} static / {} dynamic)\n"
                "Stock animation/complex: {} | safe sprite records: {}\n"
                "Discovery: {} total | {} atlas | {} standalone leaf | {} unsupported standalone\n"
                "GPU nodes: {} atlas + {} standalone runs | owned: {} ({} atlas + {} standalone)\n"
                "Active GPU state: {} objects | {} sprites\n"
                "Dirty: {} transform | {} appearance | {} visibility | {} UV\n"
                "Static GPU reused: {}/{} | uploads: {} in {} call(s)\n"
                "Resident verts: {} | GPU draws: {} / frame | indices: {} | ranges: {} | rejected: {}\n"
                "CPU render skipped: {} atlas quad transforms | {} standalone visits\n"
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
                state ? state->gpuCandidateSprites : 0,
                state ? state->candidatesWithBatch : 0,
                state ? state->standaloneLeafCandidates : 0,
                state ? state->standaloneUnsupported : 0,
                atlasBatchCount,
                standaloneBatchCount,
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
                standaloneOwned,
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
    return state && state->standaloneOwnedSprites.contains(sprite);
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