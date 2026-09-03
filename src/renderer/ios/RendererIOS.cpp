#ifdef GEODE_IS_IOS

#include "../Renderer.hpp"
#include "../AreaVisualState.hpp"
#include "ResolvedStateLayer.hpp"
#include "AssistShadowBatch.hpp"

#include "Geode/cocos/CCDirector.h"
#include "Geode/cocos/sprite_nodes/CCSpriteBatchNode.h"

#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {
struct IOSRendererState {
    std::unique_ptr<ColorChannelBuffer> colorChannels = std::make_unique<ColorChannelBuffer>();
    std::unique_ptr<ResolvedStateLayer> resolvedState;
    Shader* assistShader = nullptr;

    std::vector<Ref<AssistShadowBatch>> gpuBatches;
    std::unordered_set<cocos2d::CCSprite*> ownedSprites;

    // Discovery statistics are intentionally separate from the safety
    // classifier. A sprite may be completely safe for GPU math but not currently
    // live inside a CCSpriteBatchNode. This tells us where the remaining CPU work
    // actually lives instead of silently treating it as a renderer rejection.
    usize gpuCandidateSprites = 0;
    usize candidatesWithBatch = 0;
    usize candidatesWithoutBatch = 0;
    usize candidateBatchNodes = 0;
    usize batchesWithoutParent = 0;

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

// Park an owned Cocos atlas slot exactly when ownership is established/reset.
// The per-frame CCSprite hook then only clears its dirty flag, so safe sprites
// stop doing CPU matrix expansion AND stop dirtying the stock atlas every frame.
static void parkOwnedStockQuads(IOSRendererState* state) {
    if (!state)
        return;

    for (auto sprite : state->ownedSprites) {
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

// This is ONLY for a manual in-level disable. At that point the PlayLayer,
// sprites and stock batches are still alive. Renderer destruction must never
// call this because Cocos may already be tearing those objects down.
static void restoreOwnedStockQuads(IOSRendererState* state) {
    if (!state)
        return;

    for (auto sprite : state->ownedSprites) {
        if (!sprite || !sprite->getBatchNode())
            continue;
        sprite->setDirty(true);
        sprite->updateTransform();
    }
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
            // Do not discover ownership from PlayLayer::m_batchNodes anymore.
            // The authoritative relationship is already on every live safe
            // CCSprite. Bucket by sprite->getBatchNode() so nested/alternate GD
            // batch nodes are not invisible to Bismuth simply because they are
            // absent from that one PlayLayer array.
            const auto candidates = state->resolvedState->getGPUCandidates();
            state->gpuCandidateSprites = candidates.size();

            std::unordered_set<cocos2d::CCSpriteBatchNode*> candidateBatches;
            candidateBatches.reserve(64);

            for (const auto& candidate : candidates) {
                auto sprite = candidate.sprite;
                if (!sprite)
                    continue;

                auto batch = sprite->getBatchNode();
                if (!batch) {
                    ++state->candidatesWithoutBatch;
                    continue;
                }

                ++state->candidatesWithBatch;
                candidateBatches.insert(batch);
            }

            state->candidateBatchNodes = candidateBatches.size();

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

                // Keep the stock batch alive. Only exact safe sprite atlas slots
                // are parked; animation/interactive/unknown sprites stay stock.
                parent->addChild(gpuBatch, batch->getZOrder());
                state->gpuBatches.push_back(gpuBatch);
                for (auto sprite : gpuBatch->getOwnedSprites()) {
                    if (sprite)
                        state->ownedSprites.insert(sprite);
                }
            }

            log::info(
                "Bismuth iOS direct discovery: {} candidates, {} batched, {} unbatched, {} unique batch nodes, {} GPU nodes, {} owned sprites",
                state->gpuCandidateSprites,
                state->candidatesWithBatch,
                state->candidatesWithoutBatch,
                state->candidateBatchNodes,
                state->gpuBatches.size(),
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
    parkOwnedStockQuads(state);

    setVisible(false);
    rendererStartTime = getTime();

    log::info("Bismuth iOS initialized: direct per-sprite GPU batch discovery + stock animation lifecycle");
    return true;
}

void Renderer::generateBatchNodes() {}

void Renderer::terminate() {
    // PlayLayer/CCSprite destruction order is not guaranteed relative to this
    // node. Drop the global hook target first so any late CCSprite transform goes
    // straight through stock Cocos instead of consulting half-destroyed state.
    if (currentRenderer == this)
        currentRenderer = nullptr;
    enabled = false;

    auto state = iosState(this);
    if (state) {
        // Do NOT restore atlas quads or remove GPU nodes from their parents here.
        // On level exit those parent/sprite pointers may already be stale. The
        // scene is being destroyed anyway, so touching it only creates UAF risk.
        for (auto& gpuBatch : state->gpuBatches) {
            if (gpuBatch)
                gpuBatch->setVisible(false);
        }
        state->ownedSprites.clear();
        state->gpuBatches.clear();
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
                    const auto& batchStats = batch->getStats();
                    residentVertices += batchStats.verticesResident;
                    gpuDraws += batchStats.drawCallsLastFrame;
                    gpuIndices += batchStats.indicesLastFrame;
                    textureRanges += batchStats.textureBatches;
                    rejectedSprites += batchStats.rejectedSprites;
                }
            }

            const usize gpuBatchCount = state ? state->gpuBatches.size() : 0;
            const usize ownedSprites = state ? state->ownedSprites.size() : 0;
            const bool ownsPixels = enabled && ownedSprites > 0;

            text = fmt::format(
                "Bismuth iOS GPU Assist\n"
                "Visible output: {}\n"
                "GPU: {}\n"
                "Resolved GPU state: {} | transform shader: {}\n"
                "Safe objects: {} ({} static / {} dynamic)\n"
                "Stock animation/complex objects: {} | safe sprite records: {}\n"
                "Discovery: {} candidates | {} batched | {} no-batch | {} batch nodes\n"
                "Dirty: {} transform | {} appearance | {} visibility | {} UV\n"
                "Static reused: {}/{} | uploads: {} in {} call(s)\n"
                "GPU assist batches: {} | owned sprites: {} | rejected: {} | parentless batches: {}\n"
                "Resident verts: {} | GPU draws: {} / frame | indices: {} | ranges: {}\n"
                "Stock CPU quad transforms skipped: {}\n"
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
                state ? state->candidatesWithoutBatch : 0,
                state ? state->candidateBatchNodes : 0,
                stats.dirtyTransforms,
                stats.dirtyAppearance,
                stats.dirtyVisibility,
                stats.dirtyUVs,
                stats.staticObjectsReused,
                stats.staticObjects,
                byteSizeToString(stats.bytesUploaded),
                stats.uploadCalls,
                gpuBatchCount,
                ownedSprites,
                rejectedSprites,
                state ? state->batchesWithoutParent : 0,
                residentVertices,
                gpuDraws,
                gpuIndices,
                textureRanges,
                ownsPixels ? ownedSprites : 0,
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
    return state && state->ownedSprites.contains(sprite);
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
