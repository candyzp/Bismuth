#include "ObjectBatchNode.hpp"
#include "Geode/cocos/platform/win32/CCGL.h"
#include "Renderer.hpp"
#include <profiler.hpp>

using namespace geode::prelude;

#ifdef GEODE_IS_IOS
namespace {

constexpr int IOS_GPU_DEBUG_TAG = 0xB15D;
constexpr u64 IOS_GPU_DEBUG_UPDATE_NS = 200000000ULL;
constexpr u64 IOS_GPU_ACTIVE_GRACE_NS = 750000000ULL;
constexpr u64 IOS_GPU_ERROR_GRACE_NS = 1000000000ULL;

struct IOSGPUDebugState {
    Renderer* renderer = nullptr;

    u64 observedBatchPasses = 0;
    u64 emptyBatchPasses = 0;
    u64 drawSubmissions = 0;
    u64 submittedIndices = 0;
    u64 successfulSubmissions = 0;
    u64 failedSubmissions = 0;
    u64 glErrorCount = 0;

    u32 lastNonEmptyIndices = 0;
    GLenum lastErrorCode = GL_NO_ERROR;
    u64 lastSuccessfulSubmission = 0;
    u64 lastErrorTime = 0;

    u64 lastOverlayUpdate = 0;
    u64 sampledSubmissionCount = 0;
    u64 sampledIndexCount = 0;
    double submissionsPerSecond = 0.0;
    double indicesPerSecond = 0.0;
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
    label->setScale(0.40f);
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

    const u64 now = getTime();
    const u32 submittedNow = drawCall ? drawCall->indexCount : 0;

    ++g_gpuDebug.observedBatchPasses;

    if (drawError != GL_NO_ERROR) {
        ++g_gpuDebug.glErrorCount;
        g_gpuDebug.lastErrorCode = drawError;
        g_gpuDebug.lastErrorTime = now;
    }

    if (submittedNow > 0) {
        ++g_gpuDebug.drawSubmissions;
        g_gpuDebug.submittedIndices += submittedNow;
        g_gpuDebug.lastNonEmptyIndices = submittedNow;

        if (drawError == GL_NO_ERROR) {
            ++g_gpuDebug.successfulSubmissions;
            g_gpuDebug.lastSuccessfulSubmission = now;
        } else {
            ++g_gpuDebug.failedSubmissions;
        }
    } else {
        ++g_gpuDebug.emptyBatchPasses;
    }

    auto label = getGPUDebugLabel(renderer, true);
    if (!label)
        return;
    label->setVisible(true);

    if (g_gpuDebug.lastOverlayUpdate != 0) {
        const u64 elapsedNs = now - g_gpuDebug.lastOverlayUpdate;
        if (elapsedNs < IOS_GPU_DEBUG_UPDATE_NS)
            return;

        const double elapsedSeconds = (double)elapsedNs / 1000000000.0;
        if (elapsedSeconds > 0.0) {
            g_gpuDebug.submissionsPerSecond =
                (double)(g_gpuDebug.drawSubmissions - g_gpuDebug.sampledSubmissionCount) / elapsedSeconds;
            g_gpuDebug.indicesPerSecond =
                (double)(g_gpuDebug.submittedIndices - g_gpuDebug.sampledIndexCount) / elapsedSeconds;
        }
    }

    g_gpuDebug.lastOverlayUpdate = now;
    g_gpuDebug.sampledSubmissionCount = g_gpuDebug.drawSubmissions;
    g_gpuDebug.sampledIndexCount = g_gpuDebug.submittedIndices;

    const bool pipelineReady =
        shaderProgram != 0 &&
        boundStateTextures == 3 &&
        vertexTextureUnits >= 3;

    const bool recentlyActive =
        g_gpuDebug.lastSuccessfulSubmission != 0 &&
        now - g_gpuDebug.lastSuccessfulSubmission <= IOS_GPU_ACTIVE_GRACE_NS;

    const bool recentError =
        g_gpuDebug.lastErrorTime != 0 &&
        now - g_gpuDebug.lastErrorTime <= IOS_GPU_ERROR_GRACE_NS;

    const char* gpuPathStatus = "READY / WAITING";
    cocos2d::ccColor3B debugColor = { 255, 220, 90 };

    if (!pipelineReady) {
        gpuPathStatus = "NOT READY";
        debugColor = { 255, 100, 100 };
    } else if (recentError) {
        gpuPathStatus = "GL ERROR";
        debugColor = { 255, 100, 100 };
    } else if (recentlyActive) {
        gpuPathStatus = "ACTIVE / SUBMITTING";
        debugColor = { 80, 255, 120 };
    } else if (g_gpuDebug.successfulSubmissions > 0) {
        gpuPathStatus = "READY / BETWEEN DRAWS";
        debugColor = { 120, 240, 180 };
    }

    const double cleanPercent = g_gpuDebug.drawSubmissions > 0
        ? (double)g_gpuDebug.successfulSubmissions * 100.0 / (double)g_gpuDebug.drawSubmissions
        : 0.0;

    std::string recentActivity = "none yet";
    if (g_gpuDebug.lastSuccessfulSubmission != 0) {
        recentActivity = fmt::format(
            "{:.2f} ms ago",
            (double)(now - g_gpuDebug.lastSuccessfulSubmission) / 1000000.0
        );
    }

    std::string lastError = "none";
    if (g_gpuDebug.lastErrorTime != 0)
        lastError = fmt::format("0x{:X}", (u32)g_gpuDebug.lastErrorCode);

    const char* gpuRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));

    auto& groupManager = renderer->getGroupManager();

    label->setColor(debugColor);
    label->setString(fmt::format(
        "Bismuth iOS GPU Debug+\n"
        "GPU path: {}\n"
        "GPU: {}\n"
        "GL: {}\n"
        "Shader: {} | state tex: {}/3 | vertex tex units: {}\n"
        "GPU draws: {} clean / {} non-empty ({:.2f}%)\n"
        "Indices submitted: {} | last non-empty: {}\n"
        "Live submit rate: {:.1f} draws/s | {:.2f}M indices/s\n"
        "Batch passes: {} | empty passes: {}\n"
        "Groups: {} combinations | {} transform sets\n"
        "GL errors: {} | last error: {}\n"
        "Last clean GPU submit: {}",
        gpuPathStatus,
        gpuRenderer ? gpuRenderer : "unknown",
        glVersion ? glVersion : "unknown",
        shaderProgram,
        boundStateTextures,
        vertexTextureUnits,
        g_gpuDebug.successfulSubmissions,
        g_gpuDebug.drawSubmissions,
        cleanPercent,
        g_gpuDebug.submittedIndices,
        g_gpuDebug.lastNonEmptyIndices,
        g_gpuDebug.submissionsPerSecond,
        g_gpuDebug.indicesPerSecond / 1000000.0,
        g_gpuDebug.observedBatchPasses,
        g_gpuDebug.emptyBatchPasses,
        groupManager.getGroupCombinationCount(),
        groupManager.getTransformCombinationCount(),
        g_gpuDebug.glErrorCount,
        lastError,
        recentActivity
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