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

using namespace geode::prelude;

namespace {
struct IOSRendererState {
    std::unique_ptr<ColorChannelBuffer> colorChannels = std::make_unique<ColorChannelBuffer>();
    std::unique_ptr<ResolvedStateLayer> resolvedState;
    Shader* assistShader = nullptr;
    Ref<AssistShadowBatch> shadowBatch;

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

    // iOS is intentionally GD-authoritative. Bismuth observes final state and
    // prepares GPU resources without recreating animation/trigger semantics.
    g_iosStates[this] = std::make_unique<IOSRendererState>();
    auto state = iosState(this);
    colorChannelBuffer = state->colorChannels.get();
    std::memset(colorChannelBuffer, 0, sizeof(ColorChannelBuffer));

    state->resolvedState = std::make_unique<ResolvedStateLayer>();
    if (!state->resolvedState->init(layer)) {
        log::warn("Bismuth iOS resolved-state layer unavailable; continuing with pure stock GD rendering");
    }

    if (state->resolvedState && state->resolvedState->isGPUStateReady()) {
        state->assistShader = Shader::create("assist_ios.vert", "assist_ios.frag");
        if (!state->assistShader) {
            log::warn("Bismuth iOS assist shader unavailable; stock GD rendering remains active");
        } else if (layer->m_objectLayer) {
            // First real GPU submission is a shadow pass. It batches only plain,
            // single-sprite static solids and disables every framebuffer write.
            // The A15 performs real transform/raster work while GD remains the
            // sole visible renderer, so ordering/duplication bugs are impossible.
            state->shadowBatch = AssistShadowBatch::create(
                state->resolvedState.get(),
                state->assistShader
            );
            if (state->shadowBatch)
                layer->m_objectLayer->addChild(state->shadowBatch, 1000000);
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

    log::info("Bismuth iOS initialized: GD-authoritative resolved-state GPU assist architecture");
    return true;
}

void Renderer::generateBatchNodes() {}

void Renderer::terminate() {
    if (auto state = iosState(this); state && state->shadowBatch) {
        state->shadowBatch->removeFromParentAndCleanup(true);
        state->shadowBatch = nullptr;
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
    // The Renderer node itself never replaces stock visuals. GPU-assist work is
    // opt-in per conservative child batch.
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
            const auto* shadow = state && state->shadowBatch ? &state->shadowBatch->getStats() : nullptr;

            text = fmt::format(
                "Bismuth iOS GPU Assist\n"
                "Visible output: STOCK GD (authoritative)\n"
                "GPU: {}\n"
                "Resolved GPU state: {} | transform shader: {}\n"
                "Safe objects: {} ({} static / {} dynamic)\n"
                "Stock fallback: {} | safe sprite records: {}\n"
                "Dirty: {} transform | {} appearance | {} visibility | {} UV\n"
                "Static reused: {}/{} | uploads: {} in {} call(s)\n"
                "Shadow candidates: {} | batched: {} | resident verts: {}\n"
                "Shadow GPU draws: {} / frame | indices: {} | texture batches: {}\n"
                "Framebuffer writes: OFF (validation only)\n"
                "Visible GPU ownership: OFF",
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
                shadow ? shadow->eligibleSprites : 0,
                shadow ? shadow->batchedSprites : 0,
                shadow ? shadow->verticesResident : 0,
                shadow ? shadow->drawCallsLastFrame : 0,
                shadow ? shadow->indicesLastFrame : 0,
                shadow ? shadow->textureBatches : 0
            );
        } else {
            text = fmt::format(
                "Bismuth iOS GPU Assist\n"
                "Visible output: STOCK GD (authoritative)\n"
                "GPU: {}\n"
                "Resolved GPU state: UNAVAILABLE\n"
                "GPU transform shader: UNAVAILABLE\n"
                "Shadow GPU batch: OFF\n"
                "Visible GPU ownership: OFF",
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
    if (auto state = iosState(this); state && state->resolvedState)
        state->resolvedState->update(detailedProbe);

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
    // The old replacement lifecycle is permanently disabled on iOS. Future
    // optimization is opt-in per safe batch, not a global renderer takeover.
    return false;
}

void Renderer::setEnabled(bool value) {
    enabled = value;

    // Stock Cocos batch nodes stay authoritative and visible. Never hide the
    // whole GD renderer just because Bismuth's assist layer is enabled.
    for (auto batch : CCArrayExt<CCNode*>(layer->m_batchNodes))
        batch->setVisible(true);

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
