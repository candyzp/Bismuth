#ifdef GEODE_IS_IOS

#include "../Renderer.hpp"
#include "../AreaVisualState.hpp"
#include "DataTexture.hpp"
#include "../SpriteMeshDictionary.hpp"
#include "../VisibilityManager.hpp"
#include "../ObjectBatchNode.hpp"
#include "../ObjectSorter.hpp"
#include "../GroupManager.hpp"
#include "BProfiler.hpp"
#include "profiler.hpp"

#include "Geode/cocos/CCDirector.h"
#include "Geode/cocos/kazmath/include/kazmath/mat4.h"
#include "Geode/cocos/sprite_nodes/CCSpriteBatchNode.h"
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/RingObject.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>

using namespace geode::prelude;

namespace {
constexpr i32 IOS_STATIC_TEXTURE_UNIT = 1;
constexpr i32 IOS_GROUP_TEXTURE_UNIT = 2;
constexpr i32 IOS_COLOR_TEXTURE_UNIT = 3;

struct IOSRendererState {
    DataTexture* staticDataTexture = nullptr;
    DataTexture* groupDataTexture = nullptr;
    DataTexture* colorDataTexture = nullptr;
    std::unique_ptr<ColorChannelBuffer> colorChannels = std::make_unique<ColorChannelBuffer>();

    ~IOSRendererState() {
        if (staticDataTexture) DataTexture::destroy(staticDataTexture);
        if (groupDataTexture) DataTexture::destroy(groupDataTexture);
        if (colorDataTexture) DataTexture::destroy(colorDataTexture);
    }
};

static std::unordered_map<Renderer*, std::unique_ptr<IOSRendererState>> g_iosStates;
static Renderer* currentRenderer = nullptr;

static IOSRendererState* iosState(Renderer* renderer) {
    auto it = g_iosStates.find(renderer);
    return it == g_iosStates.end() ? nullptr : it->second.get();
}
} // namespace

Renderer::~Renderer() { terminate(); }

bool Renderer::init(PlayLayer* layer) {
    if (currentRenderer)
        return false;

    currentRenderer = this;
    this->layer = layer;
    AreaVisualState::reset();

    if (!Mod::get()->getSettingValue<bool>("enabled"))
        return false;

    // iOS hybrid mode deliberately leaves Geometry Dash's sprite renderer,
    // culling, colors and animation lifecycle untouched. This removes Bismuth's
    // static sprite replacement path, which was the source of seams, white art
    // and frozen/detached animation children on 2.2081.
    g_iosStates[this] = std::make_unique<IOSRendererState>();
    auto state = iosState(this);
    colorChannelBuffer = state->colorChannels.get();
    std::memset(colorChannelBuffer, 0, sizeof(ColorChannelBuffer));

    ingameEnableDisable = Mod::get()->getSettingValue<bool>("ingame_enable");
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
    setVisible(false); // no replacement GPU draw node in hybrid mode
    rendererStartTime = getTime();
    log::info("Bismuth iOS hybrid initialized: stock GD rendering + GPU math helper");
    return true;
}

void Renderer::generateBatchNodes() {}

void Renderer::terminate() {
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
    // Geometry Dash owns drawing in hybrid mode.
}

void Renderer::updateDebugText() {
    if (!debugText)
        return;

    std::string text;
    if (Mod::get()->getSettingValue<bool>("ios_gpu_debug")) {
        const char* gpuRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        text = fmt::format(
            "Bismuth iOS Hybrid\n"
            "Render path: STOCK GD / CPU-managed sprites\n"
            "GPU: {}\n"
            "GPU math helper: ACTIVE\n"
            "Bismuth batch drawing: OFF\n"
            "Animations/colors/culling: STOCK GD",
            gpuRenderer ? gpuRenderer : "unknown"
        );
    }

    debugText->setString(text.c_str());
    debugTextOutline1->setString(text.c_str());
    debugTextOutline2->setString(text.c_str());
}

Shader* Renderer::prepareDraw() { return nullptr; }
void Renderer::finishDraw() {}

void Renderer::update(float dt) {
    gameTimer += dt;
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

Ref<Renderer> Renderer::create(PlayLayer* layer) {
    auto ren = new Renderer;
    if (ren->init(layer)) {
        ren->autorelease();
        return ren;
    }
    delete ren;
    return nullptr;
}

Ref<Renderer> Renderer::get() { return currentRenderer; }

bool Renderer::useOptimizations() {
    // Never replace GD's visual lifecycle on iOS hybrid mode.
    return false;
}

void Renderer::setEnabled(bool value) {
    enabled = value;
    // Stock Cocos batch nodes always stay visible. Bismuth no longer owns draw.
    for (auto batch : CCArrayExt<CCNode*>(layer->m_batchNodes))
        batch->setVisible(true);
    setVisible(false);
    updateDebugText();
}

void Renderer::reset() { AreaVisualState::reset(); }
void Renderer::drawLine(const glm::vec2&, const glm::vec2&, const glm::vec4&) {}

void storeGLStates() {}
void restoreGLStates() {}

#endif