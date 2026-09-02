#include "ObjectBatchNode.hpp"
#include "Geode/cocos/platform/win32/CCGL.h"
#include "Renderer.hpp"
#include <profiler.hpp>

using namespace geode::prelude;

#ifdef GEODE_IS_IOS
namespace {

constexpr int IOS_GPU_DEBUG_TAG = 0xB15D;
constexpr u64 IOS_GPU_DEBUG_UPDATE_NS = 200000000ULL;

struct IOSGPUDebugState {
    Renderer* renderer = nullptr;
    u64 drawSubmissions = 0;
    u64 submittedIndices = 0;
    u64 successfulSubmissions = 0;
    u64 lastOverlayUpdate = 0;
};

static IOSGPUDebugState g_gpuDebug;

static cocos2d::CCLabelBMFont* getGPUDebugLabel(Renderer* renderer, bool create) {
    auto layer = renderer ? renderer->getPlayLayer() : nullptr;
    if (!layer)
        return nullptr;

    auto existing = layer->getChildByTag(IOS_GPU_DEBUG_TAG);
    if (existing)
        return static_cast<cocos2d::CCLabelBMFont*>(existing);
    if (!create)
        return nullptr;

    auto label = cocos2d::CCLabelBMFont::create("", "chatFont.fnt");
    if (!label)
        return nullptr;

    label->setTag(IOS_GPU_DEBUG_TAG);
    label->setAnchorPoint({ 0.f, 1.f });
    label->setPosition(3.f, cocos2d::CCDirector::get()->getWinSize().height - 8.f);
    label->setScale(0.45f);
    layer->addChild(label, 1002);
    return label;
}

static i32 countBoundStateTextures() {
    GLint previousActiveTexture = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);

    i32 bound = 0;
    for (i32 unit = 1; unit <= 3; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        GLint texture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        if (texture != 0)
            ++bound;
    }

    glActiveTexture((GLenum)previousActiveTexture);
    return bound;
}

static void updateGPUDebugOverlay(
    Renderer* renderer,
    ObjectBatch::LayerDrawCall* drawCall,
    GLint shaderProgram,
    i32 boundStateTextures,
    GLint vertexTextureUnits,
    GLenum drawError
) {
    if (g_gpuDebug.renderer != renderer) {
        g_gpuDebug = {};
        g_gpuDebug.renderer = renderer;
    }

    const u32 submittedNow = drawCall ? drawCall->indexCount : 0;
    if (submittedNow > 0) {
        ++g_gpuDebug.drawSubmissions;
        g_gpuDebug.submittedIndices += submittedNow;
        if (drawError == GL_NO_ERROR)
            ++g_gpuDebug.successfulSubmissions;
    }

    auto label = getGPUDebugLabel(renderer, true);
    if (!label)
        return;
    label->setVisible(true);

    const u64 now = getTime();
    if (g_gpuDebug.lastOverlayUpdate != 0 && now - g_gpuDebug.lastOverlayUpdate < IOS_GPU_DEBUG_UPDATE_NS)
        return;
    g_gpuDebug.lastOverlayUpdate = now;

    const bool active =
        shaderProgram != 0 &&
        boundStateTextures == 3 &&
        submittedNow > 0 &&
        drawError == GL_NO_ERROR;

    const char* gpuRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));

    label->setColor(active ? cocos2d::ccColor3B{ 80, 255, 120 } : cocos2d::ccColor3B{ 255, 180, 80 });
    label->setString(fmt::format(
        "Bismuth iOS GPU Debug\n"
        "GPU path: {}\n"
        "GPU: {}\n"
        "GL: {}\n"
        "Shader program: {}\n"
        "State textures: {}/3 bound\n"
        "Vertex texture units: {}\n"
        "Draw submissions: {} ({} clean)\n"
        "Indices submitted: {}\n"
        "Last draw: {} indices | GL {}",
        active ? "SUBMITTING" : "CHECKING / ERROR",
        gpuRenderer ? gpuRenderer : "unknown",
        glVersion ? glVersion : "unknown",
        shaderProgram,
        boundStateTextures,
        vertexTextureUnits,
        g_gpuDebug.drawSubmissions,
        g_gpuDebug.successfulSubmissions,
        g_gpuDebug.submittedIndices,
        submittedNow,
        drawError == GL_NO_ERROR ? "OK" : fmt::format("0x{:X}", (u32)drawError)
    ).c_str());
}

static void hideGPUDebugOverlay(Renderer* renderer) {
    if (auto label = getGPUDebugLabel(renderer, false))
        label->setVisible(false);
}

} // namespace
#endif

bool ObjectBatchNode::init() {
    auto renderer = Renderer::get();
    spriteSheetTexture = renderer->getSpriteSheetTexture(layerId.spriteSheet);
    drawCall = renderer->getObjectBatch().getDrawCall(layerId);
    return true;
}

void ObjectBatchNode::draw() {
    profiler::functionPush("ObjectBatchNode::draw");
    auto renderer = Renderer::get();

    auto shader = renderer->prepareDraw();
    shader->setUInt("u_spriteSheet", (u32)layerId.spriteSheet);
    shader->setTexture("u_spriteSheetTexture", 0, spriteSheetTexture);

#ifdef GEODE_IS_IOS
    const bool gpuDebugEnabled = Mod::get()->getSettingValue<bool>("ios_gpu_debug");
    GLint debugShaderProgram = 0;
    GLint debugVertexTextureUnits = 0;
    i32 debugBoundStateTextures = 0;

    if (gpuDebugEnabled) {
        glGetIntegerv(GL_CURRENT_PROGRAM, &debugShaderProgram);
        glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &debugVertexTextureUnits);
        debugBoundStateTextures = countBoundStateTextures();
    } else {
        hideGPUDebugOverlay(renderer);
    }
#endif

    glEnable(GL_BLEND);
    if (layerId.blending)
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    else
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    renderer->getObjectBatch().draw(drawCall);

#ifdef GEODE_IS_IOS
    if (gpuDebugEnabled) {
        const GLenum drawError = glGetError();
        updateGPUDebugOverlay(
            renderer,
            drawCall,
            debugShaderProgram,
            debugBoundStateTextures,
            debugVertexTextureUnits,
            drawError
        );
    }
#endif

    renderer->finishDraw();
    profiler::functionPop();
}
