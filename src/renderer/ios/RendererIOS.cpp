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

    // Visible GPU ownership is deliberately coarse: one GPU node replaces one
    // complete stock CCSpriteBatchNode only when every direct GameObject in that
    // batch is StaticSafe. Mixed/animated batches remain 100% stock Cocos.
    std::vector<Ref<AssistShadowBatch>> staticBatches;
    std::vector<cocos2d::CCSpriteBatchNode*> ownedStockBatches;

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

static bool batchContainsOnlyStaticSafeObjects(
    cocos2d::CCSpriteBatchNode* batch,
    const std::unordered_set<GameObject*>& staticSafeObjects
) {
    if (!batch || !batch->isVisible())
        return false;

    auto children = batch->getChildren();
    if (!children || children->count() == 0)
        return false;

    bool sawGameObject = false;
    for (auto child : CCArrayExt<cocos2d::CCNode*>(children)) {
        auto object = typeinfo_cast<GameObject*>(child);

        // Reparented glow/detail sprites, particles, or any non-GameObject direct
        // child make the entire batch stock-owned. This is intentionally strict:
        // hiding a mixed stock batch is exactly how animation pieces vanished in
        // the previous replacement renderer.
        if (!object)
            return false;
        if (!staticSafeObjects.contains(object))
            return false;

        sawGameObject = true;
    }

    return sawGameObject;
}

static void restoreOwnedStockBatches(IOSRendererState* state) {
    if (!state)
        return;
    for (auto batch : state->ownedStockBatches) {
        if (batch)
            batch->setVisible(true);
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

    if (!Mod::get()->getSettingValue<bool>("enabled"))
        return false;

    // Keep Geometry Dash authoritative for gameplay, triggers, colors and all
    // animation lifecycle. The optimization below only replaces complete stock
    // batch nodes that contain conservative StaticSafe objects.
    g_iosStates[this] = std::make_unique<IOSRendererState>();
    auto state = iosState(this);
    colorChannelBuffer = state->colorChannels.get();
    std::memset(colorChannelBuffer, 0, sizeof(ColorChannelBuffer));

    state->resolvedState = std::make_unique<ResolvedStateLayer>();
    if (!state->resolvedState->init(layer)) {
        log::warn("Bismuth iOS resolved-state layer unavailable; continuing with stock GD rendering");
    }

    if (state->resolvedState && state->resolvedState->isGPUStateReady()) {
        state->assistShader = Shader::create("assist_ios.vert", "assist_ios.frag");
        if (!state->assistShader) {
            log::warn("Bismuth iOS assist shader unavailable; stock GD rendering remains active");
        } else if (layer->m_batchNodes) {
            // Build the set once from the conservative classifier. Animated,
            // synced-animation, interactive, checkpoint and grouped/dynamic
            // objects are not StaticSafe and therefore poison their whole stock
            // batch back to Cocos rather than being partially replaced.
            std::unordered_set<GameObject*> staticSafeObjects;
            const auto candidates = state->resolvedState->getStaticShadowCandidates();
            staticSafeObjects.reserve(candidates.size());
            for (const auto& candidate : candidates) {
                if (candidate.object)
                    staticSafeObjects.insert(candidate.object);
            }

            for (auto node : CCArrayExt<cocos2d::CCNode*>(layer->m_batchNodes)) {
                auto batch = static_cast<cocos2d::CCSpriteBatchNode*>(node);
                if (!batchContainsOnlyStaticSafeObjects(batch, staticSafeObjects))
                    continue;

                auto parent = batch->getParent();
                if (!parent)
                    continue;

                auto gpuBatch = AssistShadowBatch::create(
                    state->resolvedState.get(),
                    state->assistShader,
                    batch
                );
                if (!gpuBatch || !gpuBatch->getStats().ready || gpuBatch->getStats().batchedSprites == 0)
                    continue;

                // The GPU node occupies the exact scene-graph layer of the stock
                // batch. Only after geometry is complete do we hide that one stock
                // batch, so a failed candidate never produces missing art.
                parent->addChild(gpuBatch, batch->getZOrder());
                batch->setVisible(false);
                state->ownedStockBatches.push_back(batch);
                state->staticBatches.push_back(gpuBatch);
            }

            log::info(
                "Bismuth iOS visible ownership: {} complete static batch node(s)",
                state->staticBatches.size()
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
    setVisible(false);
    rendererStartTime = getTime();

    log::info("Bismuth iOS initialized: static GPU batch ownership + stock animation lifecycle");
    return true;
}

void Renderer::generateBatchNodes() {}

void Renderer::terminate() {
    auto state = iosState(this);
    restoreOwnedStockBatches(state);

    if (state) {
        for (auto& gpuBatch : state->staticBatches) {
            if (gpuBatch)
                gpuBatch->removeFromParentAndCleanup(true);
        }
        state->staticBatches.clear();
        state->ownedStockBatches.clear();
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
    if (currentRenderer == this)
        currentRenderer = nullptr;
}

void Renderer::prepareShaderUniforms() {}
void Renderer::prepareColorChannelBuffer() {}
void Renderer::generateStaticRenderingBuffer(ObjectSorter&) {}

void Renderer::draw() {
    // Visible GPU work is submitted by the per-stock-layer AssistShadowBatch
    // children. Keeping Renderer itself draw-less avoids an extra global pass.
}

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

            usize ownedSprites = 0;
            usize residentVertices = 0;
            usize gpuDraws = 0;
            usize gpuIndices = 0;
            usize textureRanges = 0;
            if (state) {
                for (const auto& batch : state->staticBatches) {
                    if (!batch)
                        continue;
                    const auto& batchStats = batch->getStats();
                    ownedSprites += batchStats.batchedSprites;
                    residentVertices += batchStats.verticesResident;
                    gpuDraws += batchStats.drawCallsLastFrame;
                    gpuIndices += batchStats.indicesLastFrame;
                    textureRanges += batchStats.textureBatches;
                }
            }

            const usize ownedBatchCount = state ? state->staticBatches.size() : 0;
            const bool ownsPixels = enabled && ownedBatchCount > 0;

            text = fmt::format(
                "Bismuth iOS GPU Assist\n"
                "Visible output: {}\n"
                "GPU: {}\n"
                "Resolved GPU state: {} | transform shader: {}\n"
                "Safe objects: {} ({} static / {} dynamic)\n"
                "Stock animation/complex objects: {} | safe sprite records: {}\n"
                "Dirty: {} transform | {} appearance | {} visibility | {} UV\n"
                "Static reused: {}/{} | uploads: {} in {} call(s)\n"
                "GPU-owned stock batches: {} | sprites: {} | resident verts: {}\n"
                "GPU draws: {} / frame | indices: {} | texture ranges: {}\n"
                "Framebuffer writes: {}\n"
                "Animation lifecycle ownership: STOCK GD",
                ownsPixels ? "GPU STATIC BATCHES + STOCK ANIMATION" : "STOCK GD",
                gpuRenderer ? gpuRenderer : "unknown",
                resolved->isGPUStateReady() ? "READY" : "UNAVAILABLE",
                state && state->assistShader ? "READY" : "UNAVAILABLE",
                stats.safeObjects,
                stats.staticObjects,
                stats.dynamicObjects,
                stats.stockObjects,
                stats.safeSprites,
                stats.dirtyTransforms,
                stats.dirtyAppearance,
                stats.dirtyVisibility,
                stats.dirtyUVs,
                stats.staticObjectsReused,
                stats.staticObjects,
                byteSizeToString(stats.bytesUploaded),
                stats.uploadCalls,
                ownedBatchCount,
                ownedSprites,
                residentVertices,
                gpuDraws,
                gpuIndices,
                textureRanges,
                ownsPixels ? "ON (owned static batches)" : "OFF"
            );
        } else {
            text = fmt::format(
                "Bismuth iOS GPU Assist\n"
                "Visible output: STOCK GD\n"
                "GPU: {}\n"
                "Resolved GPU state: UNAVAILABLE\n"
                "GPU transform shader: UNAVAILABLE\n"
                "Visible GPU ownership: OFF\n"
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
        // Once a stock batch is actually replaced, the resolved textures are no
        // longer diagnostic data. Keep them current every frame so GD-resolved
        // position/color/opacity/visibility continues feeding the GPU without
        // changing any animation code.
        const bool gpuConsumesResolvedState = enabled && !state->staticBatches.empty();
        state->resolvedState->update(detailedProbe || gpuConsumesResolvedState);
    }

    updateDebugText();
}

bool Renderer::isColorChannelBlending(i32 channel) {
    return layer->shouldBlend(channel);
}

CCSpriteBatchNode* Renderer::getSpriteBatchNodeWithLayerId(LayerKey id) {
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
    // IMPORTANT: iOS still runs Geometry Dash's normal visibility/animation
    // lifecycle in hooks.cpp. Visible ownership here only removes draw work for
    // complete StaticSafe sprite batches; it never switches to the old optimized
    // visibility loop that froze animations.
    return false;
}

void Renderer::setEnabled(bool value) {
    enabled = value;

    if (auto state = iosState(this)) {
        for (auto& gpuBatch : state->staticBatches) {
            if (gpuBatch)
                gpuBatch->setVisible(value);
        }
        for (auto batch : state->ownedStockBatches) {
            if (batch)
                batch->setVisible(!value);
        }
    }

    // The Renderer node itself has no draw pass.
    setVisible(false);
    updateDebugText();
}

void Renderer::reset() {
    AreaVisualState::reset();
    if (auto state = iosState(this); state && state->resolvedState)
        state->resolvedState->resync();
}

void Renderer::drawLine(const glm::vec2&, const glm::vec2&, const glm::vec4&) {}

void storeGLStates() {}
void restoreGLStates() {}

#endif
