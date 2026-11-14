#ifdef GEODE_IS_IOS

#include "IOSGPURenderer.hpp"
#include "IOSCrashDiagnostics.hpp"

#include "../Shader.hpp"
#include <Geode/cocos/CCDirector.h>
#include <Geode/cocos/label_nodes/CCLabelTTF.h>
#include <Geode/cocos/sprite_nodes/CCSpriteBatchNode.h>
#include <Geode/utils/cocos.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <limits>

using namespace geode::prelude;

namespace IOSUpstream {

IOSGPURenderer* IOSGPURenderer::currentRenderer = nullptr;

namespace {

cocos2d::CCLabelTTF* startupDiagnosticLabel = nullptr;

u32 selectedRenderMethodFromSettings() {
    const auto selected = Mod::get()->getSettingValue<std::string>("ios_render_method");
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(selected.c_str(), &end, 10);
    if (end == selected.c_str() || parsed < 1 || parsed > 20)
        return 1;
    return static_cast<u32>(parsed);
}

u32 uploadMethodFor(u32 method) {
    return method == 0 ? 0 : (method - 1) / 4;
}

u32 cullingMethodFor(u32 method) {
    return method == 0 ? 0 : (method - 1) % 4;
}

const char* renderMethodName(u32 method) {
    static constexpr const char* names[] = {
        "Persistent VBO + shader culling",
        "Persistent VBO + CPU visibility mask",
        "Persistent VBO + Metal visibility mask",
        "Persistent VBO + no camera culling",
        "Orphaned VBO + shader culling",
        "Orphaned VBO + CPU visibility mask",
        "Orphaned VBO + Metal visibility mask",
        "Orphaned VBO + no camera culling",
        "SubData VBO + shader culling",
        "SubData VBO + CPU visibility mask",
        "SubData VBO + Metal visibility mask",
        "SubData VBO + no camera culling",
        "Client arrays + shader culling",
        "Client arrays + CPU visibility mask",
        "Client arrays + Metal visibility mask",
        "Client arrays + no camera culling",
        "Triple VBO ring + shader culling",
        "Triple VBO ring + CPU visibility mask",
        "Triple VBO ring + Metal visibility mask",
        "Triple VBO ring + no camera culling"
    };
    return method >= 1 && method <= 20 ? names[method - 1] : "Unknown renderer method";
}

bool isClientArrayMethod(u32 method) {
    return uploadMethodFor(method) == 3;
}

bool isTripleBufferMethod(u32 method) {
    return uploadMethodFor(method) == 4;
}

constexpr GLint OBJECT_DATA_TEXTURE_UNIT = 0;
constexpr GLint GROUP_DATA_TEXTURE_UNIT = 1;
constexpr GLint BYTE_DATA_TEXTURE_UNIT = 2;
constexpr GLint SPRITE_SHEET_TEXTURE_UNIT = 3;
constexpr GLint METAL_VISIBILITY_TEXTURE_UNIT = 4;

static_assert(sizeof(ObjectVertex) == sizeof(float) * 10,
    "ES2 object vertex must contain exactly five vec2 attributes");
static_assert(offsetof(ObjectVertex, positionOffset) == 0);
static_assert(offsetof(ObjectVertex, texCoord) == sizeof(float) * 2);
static_assert(offsetof(ObjectVertex, objectAndGroupIndex) == sizeof(float) * 4);
static_assert(offsetof(ObjectVertex, spriteAndMeta) == sizeof(float) * 6);
static_assert(offsetof(ObjectVertex, objectStartPosition) == sizeof(float) * 8);

class IOSGLStateSnapshot {
public:
    static constexpr GLint ATTRIB_COUNT = 5;
    static constexpr GLint TEXTURE_UNIT_COUNT = 5;

    IOSGLStateSnapshot() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);

        blendEnabled = glIsEnabled(GL_BLEND);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEquationRGB);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEquationAlpha);

        for (GLint unit = 0; unit < TEXTURE_UNIT_COUNT; ++unit) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBindings[unit]);
        }
        glActiveTexture(static_cast<GLenum>(activeTexture));

        for (GLuint index = 0; index < ATTRIB_COUNT; ++index) {
            auto& attrib = attributes[index];
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attrib.enabled);
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attrib.size);
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attrib.type);
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attrib.normalized);
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attrib.stride);
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attrib.buffer);
            glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attrib.pointer);
        }
    }

    ~IOSGLStateSnapshot() {
        restore();
    }

    void restore() {
        if (didRestore)
            return;

        glUseProgram(static_cast<GLuint>(program));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));

        for (GLuint index = 0; index < ATTRIB_COUNT; ++index) {
            const auto& attrib = attributes[index];

            // Restore the complete ES2 attribute-array descriptor. In
            // particular, buffer 0 is a valid client-memory array binding; if
            // it was enabled before this draw, leaving our VBO pointer installed
            // would corrupt a later Cocos draw using the same attribute index.
            glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(attrib.buffer));
            glVertexAttribPointer(
                index,
                attrib.size,
                static_cast<GLenum>(attrib.type),
                static_cast<GLboolean>(attrib.normalized),
                attrib.stride,
                attrib.pointer
            );
            if (attrib.enabled)
                glEnableVertexAttribArray(index);
            else
                glDisableVertexAttribArray(index);
        }
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));

        for (GLint unit = 0; unit < TEXTURE_UNIT_COUNT; ++unit) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(textureBindings[unit]));
        }
        glActiveTexture(static_cast<GLenum>(activeTexture));

        if (blendEnabled)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);

        glBlendEquationSeparate(
            static_cast<GLenum>(blendEquationRGB),
            static_cast<GLenum>(blendEquationAlpha)
        );
        glBlendFuncSeparate(
            static_cast<GLenum>(blendSrcRGB),
            static_cast<GLenum>(blendDstRGB),
            static_cast<GLenum>(blendSrcAlpha),
            static_cast<GLenum>(blendDstAlpha)
        );

        didRestore = true;
    }

private:
    struct AttribState {
        GLint enabled = GL_FALSE;
        GLint size = 4;
        GLint type = GL_FLOAT;
        GLint normalized = GL_FALSE;
        GLint stride = 0;
        GLint buffer = 0;
        void* pointer = nullptr;
    };

    GLint program = 0;
    GLint arrayBuffer = 0;
    GLint activeTexture = GL_TEXTURE0;
    GLint blendSrcRGB = GL_ONE;
    GLint blendDstRGB = GL_ZERO;
    GLint blendSrcAlpha = GL_ONE;
    GLint blendDstAlpha = GL_ZERO;
    GLint blendEquationRGB = GL_FUNC_ADD;
    GLint blendEquationAlpha = GL_FUNC_ADD;
    GLboolean blendEnabled = GL_FALSE;
    GLint textureBindings[TEXTURE_UNIT_COUNT] = {};
    AttribState attributes[ATTRIB_COUNT] {};
    bool didRestore = false;
};

class IOSGPULayerNode final : public cocos2d::CCNode {
public:
    IOSGPULayerNode(PlayLayer* owner, LayerKey key)
      : owner(owner), key(key) {}

    void draw() override {
        // Layer nodes are siblings of the renderer owner so their Z order
        // matches the stock batches. Resolve the live owner at draw time rather
        // than retaining a raw pointer to a renderer that may be destroyed
        // before the PlayLayer's sibling nodes are released.
        auto renderer = IOSGPURenderer::getCurrent();
        if (renderer && renderer->getPlayLayer() == owner)
            renderer->drawLayer(key);
    }

private:
    PlayLayer* owner = nullptr;
    LayerKey key {};
};

static bool isValidDrawRange(const LayerDrawRange& range, usize vertexCount) {
    const u64 endVertex =
        static_cast<u64>(range.firstVertex) + static_cast<u64>(range.vertexCount);
    return range.vertexCount > 0 &&
        range.vertexCount % 3 == 0 &&
        endVertex <= vertexCount &&
        range.firstVertex <= static_cast<u32>(std::numeric_limits<GLint>::max()) &&
        range.vertexCount <= static_cast<u32>(std::numeric_limits<GLsizei>::max());
}

} // namespace

IOSGPURenderer::~IOSGPURenderer() {
    // Layer nodes are siblings under PlayLayer::m_objectLayer. They resolve
    // their owner through getCurrent(), so they become inert as soon as this
    // renderer is destroyed and are released with their owning PlayLayer.
    layerNodes.clear();

    if (vertexRingBuffers[0] || vertexRingBuffers[1] || vertexRingBuffers[2]) {
        const GLuint buffers[3] = {
            static_cast<GLuint>(vertexRingBuffers[0]),
            static_cast<GLuint>(vertexRingBuffers[1]),
            static_cast<GLuint>(vertexRingBuffers[2])
        };
        glDeleteBuffers(3, buffers);
        vertexRingBuffers = {};
        vertexBuffer = 0;
    } else if (vertexBuffer) {
        glDeleteBuffers(1, &vertexBuffer);
        vertexBuffer = 0;
    }
    if (cpuVisibilityTexture) {
        const GLuint texture = static_cast<GLuint>(cpuVisibilityTexture);
        glDeleteTextures(1, &texture);
        cpuVisibilityTexture = 0;
    }
    if (shader) {
        Shader::destroy(shader);
        shader = nullptr;
    }
    if (currentRenderer == this)
        currentRenderer = nullptr;
}

IOSGPURenderer* IOSGPURenderer::getCurrent() {
    return currentRenderer;
}

void IOSGPURenderer::updateStartupDiagnostic(const std::string& message) {
    auto director = CCDirector::get();
    if (!director)
        return;

    // Keep startup diagnostics outside PlayLayer so resetLevel cannot remove
    // them, and so they exist before IOSGPURenderer construction succeeds.
    auto scene = director->getRunningScene();
    if (!scene) {
        log::warn("Bismuth iOS GPU diagnostic: no running scene is available yet");
        return;
    }

    if (!startupDiagnosticLabel) {
        startupDiagnosticLabel = cocos2d::CCLabelTTF::create(
            "BISMUTH iOS GPU: STARTING",
            "Arial",
            11.0f
        );
        if (!startupDiagnosticLabel) {
            log::error("Bismuth iOS GPU diagnostic: failed to create CCLabelTTF");
            return;
        }

        startupDiagnosticLabel->retain();
        startupDiagnosticLabel->setAnchorPoint(CCPoint(0.f, 1.f));
        startupDiagnosticLabel->setScale(1.0f);
        startupDiagnosticLabel->setOpacity(245);
    }

    if (startupDiagnosticLabel->getParent() != scene) {
        if (startupDiagnosticLabel->getParent())
            startupDiagnosticLabel->removeFromParentAndCleanup(false);
        scene->addChild(startupDiagnosticLabel, 1000000);
    }

    const auto winSize = director->getWinSize();
    startupDiagnosticLabel->setAnchorPoint(CCPoint(0.f, 1.f));
    startupDiagnosticLabel->setPosition(CCPoint(8.f, winSize.height - 8.f));

    ccColor3B color{255, 225, 90};
    if (message.find("FAILED") != std::string::npos)
        color = {255, 100, 100};
    else if (message.find("ACTIVE") != std::string::npos)
        color = {110, 255, 150};

    startupDiagnosticLabel->setColor(color);
    startupDiagnosticLabel->setString(message.c_str());
    startupDiagnosticLabel->setVisible(true);
}

void IOSGPURenderer::updateDebugOverlay() {
    const auto& batch = sceneData.getObjectBatch();

    std::string status = failed
        ? "BISMUTH iOS GPU: FAILED"
        : initialized
            ? "BISMUTH iOS GPU: ACTIVE"
            : "BISMUTH iOS GPU: STARTING";

    if (!failed && initialized) {
        if (batch.getSpriteCount() == 0 || batch.getOrderedVertices().empty())
            status = "BISMUTH iOS GPU: READY, ZERO GEOMETRY";
        else if (activeLayerCount == 0)
            status = "BISMUTH iOS GPU: READY, ZERO ACTIVE LAYERS";
        else if (layerNodes.empty())
            status = "BISMUTH iOS GPU: READY, NO GPU LAYER NODES";
        else if (hasFinishedFrame && lastFrameLayerVisits == 0)
            status = "BISMUTH iOS GPU: NO GPU LAYER VISITS";
        else if (hasFinishedFrame && lastFrameDraws == 0)
            status = "BISMUTH iOS GPU: VISITED, NO GPU DRAWS";
    }

    std::string detail = failed
        ? fmt::format("Failure: {}\n", failureReason)
        : std::string{};
    detail += fmt::format(
        "Level O:{} S:{} G:{}\n"
        "CPU ms GD:{:.1f} SC:{:.1f} BL:{:.1f} SUB:{:.1f}\n"
        "CPU sprites scanned:{} changed:{}\n"
        "Group records:{} | pulse entries:{}\n"
        "Table pack:{:.1f}ms upload:{:.1f}ms\n"
        "Table data:{:.1f}KB / {} texels\n"
        "GPU draws:{} verts:{} triangles:{}\n"
        "VBO:{:.1f}KB | frame:{} work:{:.1f}ms cadence:{:.1f}Hz\n"
        "Nodes:{} ranges:{} missing:{} visits:{}",
        layer && layer->m_objects ? layer->m_objects->count() : 0,
        batch.getSpriteCount(),
        sceneData.getGroupManager().getGroupCombinationCount(),
        lastGameLayerUpdateMilliseconds,
        lastSceneUpdateMilliseconds,
        lastBlendRefreshMilliseconds,
        lastDrawSubmitMilliseconds,
        lastSpritesScanned,
        lastSpritesChanged,
        lastGroupStateRecords,
        lastPulseEntries,
        lastStateBuildMilliseconds,
        lastStateTransferMilliseconds,
        static_cast<double>(lastStatePayloadBytes) / 1024.0,
        lastStatePayloadTexels,
        lastFrameDraws,
        lastGPUVerticesSubmitted,
        lastGPUTrianglesSubmitted,
        static_cast<double>(lastVboUploadBytes) / 1024.0,
        frameNumber,
        lastFrameMilliseconds,
        lastFrameIntervalMilliseconds > 0.f
            ? 1000.f / lastFrameIntervalMilliseconds : 0.f,
        layerNodes.size(),
        activeLayerCount,
        missingLayerNodeCount,
        lastFrameLayerVisits
    );

    static constexpr const char* uploadNames[] = {
        "persistent VBO", "orphan + upload", "subdata VBO",
        "client arrays", "triple VBO ring"
    };
    static constexpr const char* cullingNames[] = {
        "vertex shader", "CPU visibility texture", "Metal shared mask", "disabled"
    };
    detail = fmt::format(
        "METHOD {:02}/20: {}\\nUPLOAD: {} | CULL: {}\\n",
        renderMethod,
        renderMethodName(renderMethod),
        uploadNames[std::min<u32>(uploadMethodFor(renderMethod), 4)],
        cullingNames[std::min<u32>(cullingMethodFor(renderMethod), 3)]
    ) + detail;
    detail += fmt::format(
        "Metal mask:{} GL tex:{} | CPU mask:{}\\n",
        metalInterop.selfTestPassed() ? "READY" : "OFF",
        metalInterop.openGLESTextureName(),
        cpuVisibilityTexture ? "READY" : "OFF"
    );

    updateStartupDiagnostic(status + "\n" + detail);
}

std::string IOSGPURenderer::getCrashDiagnosticSnapshot() const {
    const auto& batch = sceneData.getObjectBatch();
    return fmt::format(
        "PlayLayer={:p} renderer={:p} method={:02}/20 ({}) status={} sceneReady={} sprites={} sourceVertices={} orderedVertices={} gpuNodes={} activeRanges={} missingNodes={} frameVisits={} drawAttempts={} draws={} animatedObjects={} animatedUpdated={} particleObjects={} particlesClaimed={} shader={} vbo={} failure='{}'",
        static_cast<void*>(layer),
        static_cast<const void*>(this),
        renderMethod,
        renderMethodName(renderMethod),
        failed ? "FAILED" : initialized ? "READY" : "STARTING",
        sceneData.isReady(),
        batch.getSpriteCount(),
        batch.getVertices().size(),
        batch.getOrderedVertices().size(),
        layerNodes.size(),
        activeLayerCount,
        missingLayerNodeCount,
        lastFrameLayerVisits,
        lastFrameDrawAttempts,
        lastFrameDraws,
        sceneData.getAnimatedObjectCount(),
        sceneData.getLastAnimatedObjectsUpdated(),
        sceneData.getParticleObjectCount(),
        sceneData.getClaimedParticleCount(),
        shader ? "ready" : "missing",
        vertexBuffer ? "ready" : "missing",
        failureReason
    );
}

void IOSGPURenderer::beginFrame() {
    if (isTripleBufferMethod(renderMethod) && vertexRingBuffers[0]) {
        currentRingIndex = static_cast<u32>(frameNumber % vertexRingBuffers.size());
        vertexBuffer = vertexRingBuffers[currentRingIndex];
    }
    const auto frameStart = std::chrono::steady_clock::now();
    if (previousFrameStartTime.time_since_epoch().count() != 0) {
        lastFrameIntervalMilliseconds = std::chrono::duration<float, std::milli>(
            frameStart - previousFrameStartTime
        ).count();
    }
    previousFrameStartTime = frameStart;

    layerVisitsThisFrame = 0;
    drawAttemptsThisFrame = 0;
    drawsThisFrame = 0;
    gameLayerUpdateMillisecondsThisFrame = 0.f;
    sceneUpdateMillisecondsThisFrame = 0.f;
    stateBuildMillisecondsThisFrame = 0.f;
    stateTransferMillisecondsThisFrame = 0.f;
    blendRefreshMillisecondsThisFrame = 0.f;
    drawSubmitMillisecondsThisFrame = 0.f;
    spritesScannedThisFrame = 0;
    spritesChangedThisFrame = 0;
    groupStateRecordsThisFrame = 0;
    pulseEntriesThisFrame = 0;
    statePayloadTexelsThisFrame = 0;
    statePayloadBytesThisFrame = 0;
    vboUploadBytesThisFrame = 0;
    gpuVerticesSubmittedThisFrame = 0;
    gpuTrianglesSubmittedThisFrame = 0;
    frameStartTime = frameStart;
}

void IOSGPURenderer::finishFrame() {
    const auto frameEndTime = std::chrono::steady_clock::now();
    if (frameStartTime.time_since_epoch().count() != 0) {
        lastFrameMilliseconds = std::chrono::duration<float, std::milli>(
            frameEndTime - frameStartTime
        ).count();
    }
    ++frameNumber;
    lastFrameLayerVisits = layerVisitsThisFrame;
    lastFrameDrawAttempts = drawAttemptsThisFrame;
    lastFrameDraws = drawsThisFrame;
    lastGameLayerUpdateMilliseconds = gameLayerUpdateMillisecondsThisFrame;
    lastSceneUpdateMilliseconds = sceneUpdateMillisecondsThisFrame;
    lastStateBuildMilliseconds = stateBuildMillisecondsThisFrame;
    lastStateTransferMilliseconds = stateTransferMillisecondsThisFrame;
    lastBlendRefreshMilliseconds = blendRefreshMillisecondsThisFrame;
    lastDrawSubmitMilliseconds = drawSubmitMillisecondsThisFrame;
    lastSpritesScanned = spritesScannedThisFrame;
    lastSpritesChanged = spritesChangedThisFrame;
    lastGroupStateRecords = groupStateRecordsThisFrame;
    lastPulseEntries = pulseEntriesThisFrame;
    lastStatePayloadTexels = statePayloadTexelsThisFrame;
    lastStatePayloadBytes = statePayloadBytesThisFrame;
    lastVboUploadBytes = vboUploadBytesThisFrame;
    lastGPUVerticesSubmitted = gpuVerticesSubmittedThisFrame;
    lastGPUTrianglesSubmitted = gpuTrianglesSubmittedThisFrame;
    hasFinishedFrame = true;

    // Do not take visual ownership until every active GPU layer has completed a
    // real GL submission. Until then, Geometry Dash's own batches remain visible.
    if (!stockOwnershipTransferred && isReadyForDrawing() && activeLayerCount > 0 &&
        lastFrameDraws >= activeLayerCount) {
        hideStockBatches();
        stockOwnershipTransferred = stockBatchesHidden;
        if (stockOwnershipTransferred)
            log::info("Bismuth iOS renderer took ownership after {} successful layer draws", lastFrameDraws);
    }

    // CCLabelTTF::setString regenerates a text texture. Keep counters per-frame,
    // but refresh the visible label at 10 Hz to avoid making the HUD itself costly.
    overlayRefreshElapsedMilliseconds += lastFrameIntervalMilliseconds > 0.f
        ? lastFrameIntervalMilliseconds : lastFrameMilliseconds;
    if (overlayRefreshElapsedMilliseconds >= 100.f) {
        overlayRefreshElapsedMilliseconds = 0.f;
        updateDebugOverlay();
    }
}

void IOSGPURenderer::recordGameLayerUpdate(float milliseconds) {
    gameLayerUpdateMillisecondsThisFrame += milliseconds;
}

void IOSGPURenderer::recordSceneUpdate(float milliseconds) {
    sceneUpdateMillisecondsThisFrame += milliseconds;
    if (!sceneData.isReady() || !sceneData.getPlayLayer() ||
        sceneData.getPlayLayer()->m_isPaused)
        return;

    const auto& batch = sceneData.getObjectBatch();
    const auto& groupManager = sceneData.getGroupManager();
    const auto& tables = sceneData.getGPUStateTables();
    spritesScannedThisFrame += batch.getLastAppearanceSpritesScanned();
    spritesChangedThisFrame += batch.getLastAppearanceSpritesChanged();
    groupStateRecordsThisFrame += groupManager.getGroupStates().size();
    pulseEntriesThisFrame += groupManager.getPulseEntries().size();
    stateBuildMillisecondsThisFrame += tables.getLastBuildMilliseconds();
    stateTransferMillisecondsThisFrame += tables.getLastTransferMilliseconds();
    statePayloadTexelsThisFrame += tables.getLastPayloadTexels();
    statePayloadBytesThisFrame += tables.getLastPayloadBytes();
}

void IOSGPURenderer::recordBlendRefresh(float milliseconds) {
    blendRefreshMillisecondsThisFrame += milliseconds;
}

IOSGPURenderer* IOSGPURenderer::create(PlayLayer* playLayer) {
    updateStartupDiagnostic("BISMUTH iOS GPU STARTUP\nRenderer create: ENTERED");
    crashBreadcrumb(fmt::format(
        "IOSGPURenderer::create ENTER layer={:p} objects={}",
        static_cast<void*>(playLayer),
        playLayer && playLayer->m_objects ? playLayer->m_objects->count() : 0
    ));

    if (currentRenderer && currentRenderer->getPlayLayer() != playLayer) {
        auto previous = currentRenderer;
        // Idempotent safety net for unusual replacement-scene order. Normal
        // PlayLayer::onExit releases these systems before this point.
        previous->suspendForPlayLayerExit();
        if (previous->getParent())
            previous->removeFromParentAndCleanup(true);
    }
    if (currentRenderer && currentRenderer->getPlayLayer() == playLayer) {
        updateStartupDiagnostic(
            currentRenderer->isReadyForDrawing()
                ? "BISMUTH iOS GPU: ACTIVE\nRenderer create: REUSED EXISTING OWNER"
                : currentRenderer->isFailed()
                    ? std::string("BISMUTH iOS GPU: FAILED\n") + currentRenderer->getFailureReason()
                    : "BISMUTH iOS GPU: STARTING\nRenderer create: REUSED EXISTING OWNER"
        );
        return currentRenderer;
    }

    auto ret = new IOSGPURenderer();
    if (!ret->cocos2d::CCNode::init()) {
        delete ret;
        updateStartupDiagnostic("BISMUTH iOS GPU: FAILED\nRenderer CCNode::init returned false");
        return nullptr;
    }

    // Keep the owner node attached even when initialization fails so its
    // on-screen diagnostic remains available. Stock ownership is transferred
    // only after the complete GPU path is ready.
    const bool initSucceeded = ret->initForPlayLayer(playLayer);
    if (!initSucceeded)
        ret->updateDebugOverlay();
    ret->autorelease();
    return ret;
}

void IOSGPURenderer::fail(const std::string& reason) {
    if (failed)
        return;

    crashBreadcrumb(fmt::format("GPU renderer failure: {}", reason));
    failed = true;
    initialized = false;
    failureReason = reason;
    sceneData.releaseObjectParticles();
    log::error("Bismuth iOS GPU-only beta FAILED: {}", failureReason);

    for (auto node : layerNodes) {
        if (node)
            node->setVisible(false);
    }

    // Any error returns visual ownership to Cocos. The previous visibility of
    // each stock batch is restored instead of forcing every batch visible.
    restoreStockBatches();
    stockOwnershipTransferred = false;
    updateDebugOverlay();
}

void IOSGPURenderer::hideStockBatches() {
    if (!layer || !layer->m_batchNodes)
        return;

    if (stockBatchesHidden) {
        for (auto batch : CCArrayExt<cocos2d::CCNode*>(layer->m_batchNodes)) {
            if (batch)
                batch->setVisible(false);
        }
        return;
    }

    stockBatchVisibility.clear();
    for (auto batch : CCArrayExt<cocos2d::CCNode*>(layer->m_batchNodes)) {
        if (!batch)
            continue;
        stockBatchVisibility.emplace_back(batch, batch->isVisible());
        batch->setVisible(false);
    }
    stockBatchesHidden = !stockBatchVisibility.empty();
}

void IOSGPURenderer::restoreStockBatches() {
    for (const auto& [batch, wasVisible] : stockBatchVisibility) {
        if (batch)
            batch->setVisible(wasVisible);
    }
    stockBatchVisibility.clear();
    stockBatchesHidden = false;
}

void IOSGPURenderer::enforceGPUOwnership() {
    // Attach/synchronize GPU layer nodes first. finishFrame() transfers visual
    // ownership only after all active layers have successfully drawn once.
    if (isReadyForDrawing())
        syncLayerNodes();
    else {
        restoreStockBatches();
        stockOwnershipTransferred = false;
    }
    updateDebugOverlay();
}

bool IOSGPURenderer::queryAndValidateCapabilities() {
    if (!IOSGPU::queryCapabilities(capabilities)) {
        fail("could not query active OpenGL ES context");
        return false;
    }
    IOSGPU::logCapabilities(capabilities);

    if (!capabilities.isOpenGLES) {
        fail("active context is not OpenGL ES");
        return false;
    }
    if (capabilities.maxVertexTextureUnits < 3 ||
        !capabilities.hasVertexTextureSampling) {
        fail("requires at least three vertex texture image units");
        return false;
    }
    const u32 cullingMethod = cullingMethodFor(renderMethod);
    const GLint requiredCombinedUnits = cullingMethod == 0 || cullingMethod == 3 ? 4 : 5;
    if (capabilities.maxCombinedTextureUnits < requiredCombinedUnits ||
        capabilities.maxFragmentTextureUnits < 1) {
        fail(fmt::format(
            "selected renderer method requires {} combined texture units and one fragment texture unit",
            requiredCombinedUnits
        ));
        return false;
    }
    if (capabilities.maxVertexAttribs < 5) {
        fail("requires five vertex attributes");
        return false;
    }
    if (!capabilities.hasFragmentHighpPrecision) {
        fail("requires fragment highp precision for sprite atlas coordinates");
        return false;
    }
    if (!capabilities.hasFloatTexture) {
        fail("floating-point RGBA vertex data textures are unavailable");
        return false;
    }
    if (capabilities.maxTextureSize < 128) {
        fail("maximum texture size is too small for Bismuth state tables");
        return false;
    }
    return true;
}

bool IOSGPURenderer::createShaderProgram() {
    shader = Shader::create("object_es2.vert", "object_es2.frag");
    if (!shader) {
        fail("failed to compile or link owner-derived ES2 object shaders");
        return false;
    }
    return true;
}

bool IOSGPURenderer::createVertexBuffer() {
    if (vertexRingBuffers[0] || vertexRingBuffers[1] || vertexRingBuffers[2]) {
        const GLuint oldBuffers[3] = {
            static_cast<GLuint>(vertexRingBuffers[0]),
            static_cast<GLuint>(vertexRingBuffers[1]),
            static_cast<GLuint>(vertexRingBuffers[2])
        };
        glDeleteBuffers(3, oldBuffers);
        vertexRingBuffers = {};
    } else if (vertexBuffer) {
        const GLuint oldBuffer = static_cast<GLuint>(vertexBuffer);
        glDeleteBuffers(1, &oldBuffer);
    }
    vertexBuffer = 0;
    vertexBufferCapacity = 0;

    const auto& vertices = sceneData.getObjectBatch().getOrderedVertices();
    if (vertices.empty()) {
        uploadedOrderRevision = sceneData.getObjectBatch().getOrderRevision();
        uploadedGeometryRevision = sceneData.getObjectBatch().getGeometryRevision();
        sceneData.getObjectBatch().clearDirtyVertexRanges();
        return true;
    }

    const u32 uploadMethod = uploadMethodFor(renderMethod);
    if (uploadMethod == 3) {
        // Client arrays point directly into ObjectBatch's stable CPU vertex stream.
        uploadedOrderRevision = sceneData.getObjectBatch().getOrderRevision();
        uploadedGeometryRevision = sceneData.getObjectBatch().getGeometryRevision();
        sceneData.getObjectBatch().clearDirtyVertexRanges();
        return true;
    }

    if (uploadMethod == 4) {
        GLuint buffers[3] = {};
        glGenBuffers(3, buffers);
        if (!buffers[0] || !buffers[1] || !buffers[2]) {
            glDeleteBuffers(3, buffers);
            fail("glGenBuffers failed while creating the triple-buffer ring");
            return false;
        }
        for (usize i = 0; i < vertexRingBuffers.size(); ++i)
            vertexRingBuffers[i] = buffers[i];
        currentRingIndex = 0;
        vertexBuffer = vertexRingBuffers[currentRingIndex];
    } else {
        GLuint buffer = 0;
        glGenBuffers(1, &buffer);
        if (!buffer) {
            fail("glGenBuffers returned zero for ordered object vertex stream");
            return false;
        }
        vertexBuffer = buffer;
    }

    return uploadOrderedVertices();
}

bool IOSGPURenderer::uploadOrderedVertices() {
    if (!sceneData.isReady())
        return false;

    const auto& batch = sceneData.getObjectBatch();
    const auto& vertices = batch.getOrderedVertices();
    if (vertices.empty()) {
        uploadedOrderRevision = batch.getOrderRevision();
        uploadedGeometryRevision = batch.getGeometryRevision();
        batch.clearDirtyVertexRanges();
        return true;
    }

    const u32 uploadMethod = uploadMethodFor(renderMethod);
    if (uploadMethod == 3) {
        uploadedOrderRevision = batch.getOrderRevision();
        uploadedGeometryRevision = batch.getGeometryRevision();
        batch.clearDirtyVertexRanges();
        return true;
    }
    if (!vertexBuffer)
        return false;

    if (vertices.size() > static_cast<usize>(std::numeric_limits<GLsizei>::max())) {
        fail("ordered object vertex count exceeds GLsizei");
        return false;
    }

    const usize byteCount = vertices.size() * sizeof(ObjectVertex);
    if (byteCount > static_cast<usize>(std::numeric_limits<GLsizeiptr>::max())) {
        fail("ordered object vertex buffer exceeds GLsizeiptr");
        return false;
    }

    GLint previousBuffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousBuffer);
    while (glGetError() != GL_NO_ERROR) {}
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(vertexBuffer));

    GLenum usage = uploadMethod == 1 || uploadMethod == 4
        ? GL_STREAM_DRAW : GL_DYNAMIC_DRAW;

    switch (uploadMethod) {
        case 0: // Persistent VBO: replace storage with the latest full vertex stream.
            glBufferData(
                GL_ARRAY_BUFFER,
                static_cast<GLsizeiptr>(byteCount),
                vertices.data(),
                GL_DYNAMIC_DRAW
            );
            vertexBufferCapacity = byteCount;
            break;

        case 1: // Orphan old storage, then stream the complete current vertex array.
        case 4: // Triple-buffer ring: orphan the selected slot before filling it.
            glBufferData(
                GL_ARRAY_BUFFER,
                static_cast<GLsizeiptr>(byteCount),
                nullptr,
                usage
            );
            glBufferSubData(
                GL_ARRAY_BUFFER,
                0,
                static_cast<GLsizeiptr>(byteCount),
                vertices.data()
            );
            vertexBufferCapacity = byteCount;
            break;

        case 2: // Keep allocated storage and update it through glBufferSubData.
            if (vertexBufferCapacity < byteCount) {
                glBufferData(
                    GL_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(byteCount),
                    nullptr,
                    GL_DYNAMIC_DRAW
                );
                vertexBufferCapacity = byteCount;
            }
            glBufferSubData(
                GL_ARRAY_BUFFER,
                0,
                static_cast<GLsizeiptr>(byteCount),
                vertices.data()
            );
            break;

        default:
            glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousBuffer));
            fail("unknown vertex upload strategy");
            return false;
    }

    const GLenum error = glGetError();
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousBuffer));
    if (error != GL_NO_ERROR) {
        fail(fmt::format("ordered vertex-buffer upload failed (GL error 0x{:X})", error));
        return false;
    }

    vboUploadBytesThisFrame += byteCount;
    uploadedOrderRevision = batch.getOrderRevision();
    uploadedGeometryRevision = batch.getGeometryRevision();
    batch.clearDirtyVertexRanges();
    return true;
}

bool IOSGPURenderer::uploadDirtyVertexRanges() {
    if (!sceneData.isReady())
        return false;

    auto& batch = sceneData.getObjectBatch();
    const auto& vertices = batch.getOrderedVertices();
    const auto& ranges = batch.getDirtyVertexRanges();
    const u32 uploadMethod = uploadMethodFor(renderMethod);

    if (uploadMethod == 3) {
        uploadedGeometryRevision = batch.getGeometryRevision();
        batch.clearDirtyVertexRanges();
        return true;
    }

    if (uploadMethod == 1 || uploadMethod == 4)
        return uploadOrderedVertices();

    if (ranges.empty()) {
        uploadedGeometryRevision = batch.getGeometryRevision();
        return true;
    }
    if (!vertexBuffer)
        return false;

    for (const auto& range : ranges) {
        const usize endVertex =
            static_cast<usize>(range.firstVertex) + range.vertexCount;
        const usize byteOffset =
            static_cast<usize>(range.firstVertex) * sizeof(ObjectVertex);
        const usize byteCount =
            static_cast<usize>(range.vertexCount) * sizeof(ObjectVertex);
        if (range.vertexCount == 0 || endVertex > vertices.size() ||
            byteOffset > static_cast<usize>(std::numeric_limits<GLintptr>::max()) ||
            byteCount > static_cast<usize>(std::numeric_limits<GLsizeiptr>::max())) {
            fail("animated sprite update produced an invalid VBO subrange");
            return false;
        }
    }

    const usize fullByteCount = vertices.size() * sizeof(ObjectVertex);
    if (vertexBufferCapacity < fullByteCount)
        return uploadOrderedVertices();

    GLint previousBuffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousBuffer);
    while (glGetError() != GL_NO_ERROR) {}
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(vertexBuffer));

    usize uploadedBytes = 0;
    for (const auto& range : ranges) {
        const usize byteOffset =
            static_cast<usize>(range.firstVertex) * sizeof(ObjectVertex);
        const usize byteCount =
            static_cast<usize>(range.vertexCount) * sizeof(ObjectVertex);
        uploadedBytes += byteCount;
        glBufferSubData(
            GL_ARRAY_BUFFER,
            static_cast<GLintptr>(byteOffset),
            static_cast<GLsizeiptr>(byteCount),
            vertices.data() + range.firstVertex
        );
    }

    const GLenum error = glGetError();
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousBuffer));
    if (error != GL_NO_ERROR) {
        fail(fmt::format("animated sprite VBO update failed (GL error 0x{:X})", error));
        return false;
    }

    vboUploadBytesThisFrame += uploadedBytes;
    uploadedGeometryRevision = batch.getGeometryRevision();
    batch.clearDirtyVertexRanges();
    return true;
}

bool IOSGPURenderer::createLayerNodes() {
    return syncLayerNodes();
}

bool IOSGPURenderer::syncLayerNodes() {
    if (failed)
        return false;
    if (!layer || !layer->m_objectLayer || !layer->m_batchNodes) {
        fail("PlayLayer object layer or stock batch-node array is missing");
        return false;
    }

    // Match Renderer::generateBatchNodes(): attach a draw node for each
    // active owner-derived layer key that has a Geometry Dash Z-order container.
    const auto& batch = sceneData.getObjectBatch();
    const auto& ranges = batch.getSortedBatch().layerRanges;
    activeLayerCount = 0;
    missingLayerNodeCount = 0;
    for (const auto& [key, range] : ranges) {
        if (range.vertexCount != 0)
            ++activeLayerCount;
    }

    for (const auto& key : batch.getUsedLayerKeys()) {
        if (layerNodeKeys.contains(key))
            continue;

        cocos2d::CCNode* stockNode = layer->parentForZLayer(
            static_cast<i32>(key.zlayer),
            key.blending,
            static_cast<i32>(key.spriteSheet),
            false
        );

        if (!stockNode || layer->m_batchNodes->indexOfObject(stockNode) == UINT_MAX) {
            const auto activeRange = ranges.find(key);
            if (activeRange != ranges.end() && activeRange->second.vertexCount != 0)
                ++missingLayerNodeCount;
            continue;
        }

        auto node = new IOSGPULayerNode(layer, key);
        if (!node->init()) {
            delete node;
            fail("failed to initialize a GPU layer node");
            return false;
        }

        node->autorelease();
        layer->m_objectLayer->addChild(node, stockNode->getZOrder());
        layerNodes.push_back(node);
        layerNodeKeys.insert(key);
    }

    u32 attachedActiveLayerCount = 0;
    for (const auto& [key, range] : ranges) {
        if (range.vertexCount != 0 && layerNodeKeys.contains(key))
            ++attachedActiveLayerCount;
    }

    if (activeLayerCount != attachedActiveLayerCount) {
        fail(fmt::format(
            "GPU layer attachment incomplete after PlayLayer reset: {} of {} active ranges attached ({} missing Z-order nodes)",
            attachedActiveLayerCount,
            activeLayerCount,
            missingLayerNodeCount
        ));
        return false;
    }

    return true;
}

bool IOSGPURenderer::initForPlayLayer(PlayLayer* playLayer) {
    layer = playLayer;
    currentRenderer = this;
    renderMethod = selectedRenderMethodFromSettings();
    updateStartupDiagnostic(fmt::format(
        "BISMUTH iOS RENDER METHOD {:02}/20\\n{}",
        renderMethod,
        renderMethodName(renderMethod)
    ));

    if (!layer) {
        fail("PlayLayer is null");
        return false;
    }

    updateStartupDiagnostic(fmt::format(
        "BISMUTH iOS RENDER METHOD {:02}/20\\n{}\\nRenderer init: ENTERED",
        renderMethod,
        renderMethodName(renderMethod)
    ));

    // Geometry Dash keeps visual ownership until the selected renderer has
    // completed a full first frame.
    if (!queryAndValidateCapabilities())
        return false;

    // Scene extraction supplies the sprite order and group combinations
    // used to build the live Metal visibility records.
    crashBreadcrumb("IOSGPURenderer::initForPlayLayer capabilities PASS; SceneData::init BEGIN");

    if (!sceneData.init(layer)) {
        const auto& reason = sceneData.getFailureReason();
        fail(reason.empty()
            ? "scene extraction or GPU state-table initialization failed"
            : reason);
        return false;
    }
    crashBreadcrumb("IOSGPURenderer::initForPlayLayer SceneData::init PASS");

    if (!initializeSelectedCulling())
        return false;

    crashBreadcrumb(fmt::format(
        "iOS renderer method {:02} ({}) selected culling path initialized",
        renderMethod,
        renderMethodName(renderMethod)
    ));

    updateStartupDiagnostic(
        "BISMUTH iOS GPU STARTUP\nScene extraction/state tables: PASS\nSprites: " +
        std::to_string(sceneData.getObjectBatch().getSpriteCount()) +
        " | Vertices: " +
        std::to_string(sceneData.getObjectBatch().getOrderedVertices().size())
    );

    crashBreadcrumb("IOSGPURenderer::initForPlayLayer shader creation BEGIN");
    if (!createShaderProgram())
        return false;
    crashBreadcrumb("IOSGPURenderer::initForPlayLayer shader creation PASS");
    updateStartupDiagnostic("BISMUTH iOS GPU STARTUP\nShader compile/link: PASS");

    crashBreadcrumb("IOSGPURenderer::initForPlayLayer VBO creation/upload BEGIN");
    if (!createVertexBuffer())
        return false;
    crashBreadcrumb("IOSGPURenderer::initForPlayLayer VBO creation/upload PASS");
    updateStartupDiagnostic("BISMUTH iOS GPU STARTUP\nVertex buffer upload: PASS");

    // PlayLayer::resetLevel() creates/rebuilds the stock Z-order containers
    // referenced by parentForZLayer(). The strict GPU layer nodes are attached
    // by enforceGPUOwnership() after that reset returns, not during extraction.
    initialized = true;
    updateStartupDiagnostic(
        "BISMUTH iOS GPU STARTUP\nGPU core initialized: YES\n"
        "Waiting for PlayLayer reset to finish before attaching GPU layer nodes"
    );

    log::info(
        "Bismuth iOS GPU-only renderer initialized: {} sprites, {} layer nodes, {} group combinations",
        sceneData.getObjectBatch().getSpriteCount(),
        layerNodes.size(),
        sceneData.getGroupManager().getGroupCombinationCount()
    );
    updateDebugOverlay();
    return true;
}

void IOSGPURenderer::prepareForLevelReset() {
    restoreStockBatches();
    stockOwnershipTransferred = false;

    // Release particles while this PlayLayer and its particle manager are valid.
    sceneData.releaseObjectParticles();

    // Retire draw nodes while the old object-layer containers are still alive.
    for (auto node : layerNodes) {
        if (!node)
            continue;
        node->setVisible(false);
        if (node->getParent())
            node->removeFromParentAndCleanup(true);
    }
    layerNodes.clear();
    layerNodeKeys.clear();
}

void IOSGPURenderer::suspendForPlayLayerExit() {
    if (suspended)
        return;

    // Called from PlayLayer::onExit before the particle manager is destroyed.
    // Never defer unclaimParticle() to SceneData's child-node destructor.
    suspended = true;
    restoreStockBatches();
    stockOwnershipTransferred = false;
    sceneData.releaseObjectParticles();
    for (auto node : layerNodes) {
        if (node)
            node->setVisible(false);
    }
}

void IOSGPURenderer::resumeAfterPlayLayerExit() {
    if (!suspended)
        return;

    suspended = false;
    if (failed || !initialized)
        return;

    // Re-acquire visual ownership only after a successful resumed frame.
    stockOwnershipTransferred = false;
    for (auto node : layerNodes) {
        if (node)
            node->setVisible(true);
    }
    updateDebugOverlay();
}

void IOSGPURenderer::reset() {
    if (failed || !sceneData.isReady())
        return;

    sceneData.reset();
    if (!sceneData.getGPUStateTables().isReady()) {
        fail("GPU state-table reset upload failed");
        return;
    }
    if (sceneData.getObjectBatch().getOrderRevision() != uploadedOrderRevision &&
        !uploadOrderedVertices()) {
        fail("ordered vertex upload failed during reset");
        return;
    }
    if (!syncLayerNodes() && !failed)
        fail("GPU layer nodes could not be restored after PlayLayer reset");
}

bool IOSGPURenderer::updateScene(float dt) {
    if (!isReadyForDrawing() || !sceneData.isReady())
        return false;

    const auto start = std::chrono::steady_clock::now();
    const bool updated = sceneData.update(dt);
    const float elapsed = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - start
    ).count();
    recordSceneUpdate(elapsed);

    if (!updated) {
        const auto& reason = sceneData.getFailureReason();
        fail(reason.empty() ? "GPU scene-state update failed" : reason);
        return false;
    }
    return true;
}

bool IOSGPURenderer::refreshBlendOrder() {
    if (!isReadyForDrawing() || !sceneData.isReady())
        return false;

    const auto start = std::chrono::steady_clock::now();
    if (!sceneData.refreshBlendOrder()) {
        fail("GPU blend-order refresh failed");
        return false;
    }

    sceneData.prepareForDraw();

    // A ring slot is reused every third frame, so it must receive the full
    // current geometry before any layer from this frame is submitted.
    if (isTripleBufferMethod(renderMethod) && !uploadOrderedVertices())
        return false;

    switch (cullingMethodFor(renderMethod)) {
        case 1:
            if (!updateCPUVisibility())
                return false;
            break;
        case 2:
            if (!updateMetalCulling())
                return false;
            break;
        default:
            break;
    }

    const float elapsed = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - start
    ).count();
    recordBlendRefresh(elapsed);
    return true;
}

bool IOSGPURenderer::initializeSelectedCulling() {
    const u32 cullingMethod = cullingMethodFor(renderMethod);
    if (cullingMethod == 0 || cullingMethod == 3)
        return true;

    const usize spriteCount = sceneData.getObjectBatch().getSpriteCount();
    visibilityTextureWidth = static_cast<u32>(
        std::min(1024, std::max(2, capabilities.maxTextureSize))
    );
    const usize textureHeight = std::max<usize>(
        1,
        (std::max<usize>(2, spriteCount) + visibilityTextureWidth - 1) /
            visibilityTextureWidth
    );
    if (textureHeight > static_cast<usize>(capabilities.maxTextureSize)) {
        fail(fmt::format(
            "visibility mask needs {} rows, exceeding GL_MAX_TEXTURE_SIZE {}",
            textureHeight,
            capabilities.maxTextureSize
        ));
        return false;
    }
    visibilityTextureHeight = static_cast<u32>(textureHeight);

    if (cullingMethod == 1)
        return initializeCPUVisibility();

    std::string error;
    if (!metalInterop.initialize(
            visibilityTextureWidth, visibilityTextureHeight, error)) {
        fail(fmt::format(
            "Metal visibility initialization failed: {}",
            error.empty() ? "unknown initialization error" : error
        ));
        return false;
    }
    if (!metalInterop.runSelfTest(error)) {
        metalInterop.reset();
        fail(fmt::format(
            "Metal visibility self-test failed: {}",
            error.empty() ? "unknown self-test error" : error
        ));
        return false;
    }

    log::info(
        "Bismuth iOS renderer method {:02}: Metal shared visibility mask ready ({} sprites)",
        renderMethod,
        spriteCount
    );
    return true;
}

bool IOSGPURenderer::initializeCPUVisibility() {
    if (cpuVisibilityTexture)
        return true;

    const usize pixelCount =
        static_cast<usize>(visibilityTextureWidth) * visibilityTextureHeight;
    if (pixelCount == 0 || pixelCount >
        std::numeric_limits<usize>::max() / 4) {
        fail("CPU visibility texture dimensions overflowed");
        return false;
    }

    cpuVisibilityPixels.assign(pixelCount * 4, 0);

    GLint previousActiveTexture = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0 + METAL_VISIBILITY_TEXTURE_UNIT);

    GLint previousBinding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousBinding);
    while (glGetError() != GL_NO_ERROR) {}

    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (!texture) {
        glActiveTexture(static_cast<GLenum>(previousActiveTexture));
        fail("glGenTextures returned zero for CPU visibility mask");
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,
        static_cast<GLsizei>(visibilityTextureWidth),
        static_cast<GLsizei>(visibilityTextureHeight),
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        cpuVisibilityPixels.data()
    );

    const GLenum error = glGetError();
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousBinding));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    if (error != GL_NO_ERROR) {
        glDeleteTextures(1, &texture);
        fail(fmt::format(
            "CPU visibility texture allocation failed (GL error 0x{:X})",
            error
        ));
        return false;
    }

    cpuVisibilityTexture = texture;
    log::info(
        "Bismuth iOS renderer method {:02}: CPU visibility texture ready ({}x{})",
        renderMethod,
        visibilityTextureWidth,
        visibilityTextureHeight
    );
    return true;
}

bool IOSGPURenderer::updateCPUVisibility() {
    if (!cpuVisibilityTexture && !initializeCPUVisibility())
        return false;

    const auto& batch = sceneData.getObjectBatch();
    const auto& sprites = batch.getPreCulledSprites();
    const auto& inputIndices = batch.getSortedBatch().inputSpriteIndices;
    const auto groupStates = sceneData.getGroupManager().getGroupStates();
    const auto& camera = sceneData.getGameState();
    const usize spriteCount = batch.getSpriteCount();

    if (sprites.size() != spriteCount || inputIndices.size() != spriteCount) {
        fail("CPU culling scheduler records do not match the source sprite count");
        return false;
    }

    const usize pixelCount =
        static_cast<usize>(visibilityTextureWidth) * visibilityTextureHeight;
    cpuVisibilityPixels.assign(pixelCount * 4, 0);

    for (usize i = 0; i < spriteCount; ++i) {
        const u32 sourceIndex = inputIndices[i];
        const auto& sprite = sprites[i];
        if (sourceIndex >= pixelCount ||
            sprite.groupCombinationIndex >= groupStates.size()) {
            fail("CPU culling scheduler returned an invalid sprite or group index");
            return false;
        }

        const auto& group = groupStates[sprite.groupCombinationIndex];
        const glm::vec2 objectPosition =
            group.positionalTransform * sprite.startPosition + group.offset;
        const glm::vec2 cameraOffset = objectPosition - camera.cameraPosition;
        const glm::vec2 screenPosition(
            glm::dot(camera.cameraRight, cameraOffset),
            glm::dot(camera.cameraUp, cameraOffset)
        );

        constexpr float margin = 100.f;
        const bool visible =
            screenPosition.x >= -margin &&
            screenPosition.x <= camera.cameraSize.x + margin &&
            screenPosition.y >= -margin &&
            screenPosition.y <= camera.cameraSize.y + margin;

        const u8 value = visible ? 255 : 0;
        const usize byteOffset = static_cast<usize>(sourceIndex) * 4;
        cpuVisibilityPixels[byteOffset + 0] = value;
        cpuVisibilityPixels[byteOffset + 1] = value;
        cpuVisibilityPixels[byteOffset + 2] = value;
        cpuVisibilityPixels[byteOffset + 3] = 255;
    }

    GLint previousActiveTexture = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0 + METAL_VISIBILITY_TEXTURE_UNIT);

    GLint previousBinding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousBinding);
    while (glGetError() != GL_NO_ERROR) {}

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(cpuVisibilityTexture));
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        static_cast<GLsizei>(visibilityTextureWidth),
        static_cast<GLsizei>(visibilityTextureHeight),
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        cpuVisibilityPixels.data()
    );
    const GLenum error = glGetError();

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousBinding));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    if (error != GL_NO_ERROR) {
        fail(fmt::format(
            "CPU visibility texture upload failed (GL error 0x{:X})",
            error
        ));
        return false;
    }
    return true;
}

bool IOSGPURenderer::updateMetalCulling() {
    if (!metalInterop.isReady() || !metalInterop.selfTestPassed()) {
        fail("Metal visibility culling is unavailable; refusing to draw without Metal results");
        return false;
    }

    const auto& batch=sceneData.getObjectBatch();
    const auto& orderedSprites=batch.getPreCulledSprites();
    const auto& inputIndices=batch.getSortedBatch().inputSpriteIndices;
    const auto groupStates=sceneData.getGroupManager().getGroupStates();
    const usize spriteCount=batch.getSpriteCount();

    if (orderedSprites.size()!=spriteCount || inputIndices.size()!=spriteCount) {
        fail("Metal culling scheduler records do not match the source sprite count");
        return false;
    }

    metalCullInputs.resize(spriteCount);
    for (usize i=0;i<spriteCount;++i) {
        const u32 sourceIndex=inputIndices[i];
        const auto& sprite=orderedSprites[i];
        if (sourceIndex>=spriteCount || sprite.groupCombinationIndex>=groupStates.size()) {
            fail("Metal culling scheduler returned an invalid source sprite or group index");
            return false;
        }

        const auto& group=groupStates[sprite.groupCombinationIndex];
        metalCullInputs[sourceIndex]={
            sprite.startPosition.x,sprite.startPosition.y,
            group.positionalTransform[0].x,group.positionalTransform[0].y,
            group.positionalTransform[1].x,group.positionalTransform[1].y,
            group.offset.x,group.offset.y
        };
    }

    const auto& camera=sceneData.getGameState();
    IOSMetalCullParameters params;
    params.cameraPositionX=camera.cameraPosition.x;
    params.cameraPositionY=camera.cameraPosition.y;
    params.cameraRightX=camera.cameraRight.x;
    params.cameraRightY=camera.cameraRight.y;
    params.cameraUpX=camera.cameraUp.x;
    params.cameraUpY=camera.cameraUp.y;
    params.cameraWidth=camera.cameraSize.x;
    params.cameraHeight=camera.cameraSize.y;

    std::string error;
    if (!metalInterop.dispatchCulling(metalCullInputs.data(),
            static_cast<std::uint32_t>(spriteCount),params,error)) {
        fail(error.empty() ? "Metal visibility dispatch failed" : error);
        return false;
    }
    return true;
}

void IOSGPURenderer::drawLayer(const LayerKey& key) {
    ++layerVisitsThisFrame;
    if (!isReadyForDrawing() || !sceneData.isReady() || !shader || !vertexBuffer)
        return;
    ++drawAttemptsThisFrame;

    // Blend-state selection is refreshed once in PlayLayer::updateVisibility,
    // before Cocos visits the per-layer GPU nodes. Every node in this frame
    // therefore consumes one stable sorted batch and matching draw ranges.
    auto& batch = sceneData.getObjectBatch();
    const auto rangeIt = batch.getSortedBatch().layerRanges.find(key);
    if (rangeIt == batch.getSortedBatch().layerRanges.end())
        return;

    const auto& range = rangeIt->second;
    if (range.vertexCount == 0)
        return;

    if (!isValidDrawRange(range, batch.getOrderedVertices().size())) {
        fail("scheduler returned an invalid GPU draw range");
        return;
    }

    if (batch.getOrderRevision() != uploadedOrderRevision) {
        if (!uploadOrderedVertices()) {
            fail("could not upload updated blend-order vertex stream");
            return;
        }
    } else if (batch.getGeometryRevision() != uploadedGeometryRevision &&
        !uploadDirtyVertexRanges()) {
        fail("could not upload animated sprite vertex updates");
        return;
    }

    auto spriteSheet = sceneData.getSpriteSheetTexture(key.spriteSheet);
    if (!spriteSheet) {
        fail("sprite sheet texture required by GPU layer is missing");
        return;
    }

    if (!sceneData.getGPUStateTables().isReady()) {
        fail("GPU state tables are not ready at draw time");
        return;
    }

    sceneData.prepareForDraw();
    const auto& gameState = sceneData.getGameState();
    const auto& tableLayout = sceneData.getGPUStateTables().getLayout();

    const auto submitStart = std::chrono::steady_clock::now();
    IOSGLStateSnapshot savedGLState;

    while (glGetError() != GL_NO_ERROR) {}
    shader->use();

    sceneData.getGPUStateTables().bind(
        OBJECT_DATA_TEXTURE_UNIT,
        GROUP_DATA_TEXTURE_UNIT,
        BYTE_DATA_TEXTURE_UNIT
    );
    shader->setInt("u_objectDataTexture", OBJECT_DATA_TEXTURE_UNIT);
    shader->setInt("u_groupDataTexture", GROUP_DATA_TEXTURE_UNIT);
    shader->setInt("u_byteDataTexture", BYTE_DATA_TEXTURE_UNIT);
    shader->setTexture(
        "u_spriteSheetTexture",
        SPRITE_SHEET_TEXTURE_UNIT,
        spriteSheet
    );

    shader->setInt("u_metalVisibilityTexture",METAL_VISIBILITY_TEXTURE_UNIT);
    shader->setVec2("u_metalVisibilityTextureSize",glm::vec2(
        static_cast<float>(metalInterop.textureWidth()),
        static_cast<float>(metalInterop.textureHeight())
    ));
    glActiveTexture(GL_TEXTURE0+METAL_VISIBILITY_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D,metalInterop.openGLESTextureName());

    shader->setVec2("u_objectDataTextureSize", tableLayout.objectTextureSize);
    shader->setVec2("u_groupDataTextureSize", tableLayout.groupTextureSize);
    shader->setVec2("u_byteDataTextureSize", tableLayout.byteTextureSize);

    shader->setFloat("u_colorChannelsOffset", static_cast<float>(tableLayout.colorChannelsOffset));
    shader->setFloat("u_objectHSVOffset", static_cast<float>(tableLayout.objectHSVOffset));
    shader->setFloat("u_pulseEntriesOffset", static_cast<float>(tableLayout.pulseEntriesOffset));
    shader->setFloat("u_spriteAppearanceOffset", static_cast<float>(tableLayout.spriteAppearanceOffset));

    shader->setMatrix4("u_mvp", glm::value_ptr(gameState.mvp));
    shader->setFloat("u_timer", gameState.timer);
    shader->setFloat("u_audioScale", gameState.audioScale);
    shader->setVec2("u_cameraViewSize", gameState.cameraViewSize);
    shader->setVec2("u_cameraPosition", gameState.cameraPosition);
    shader->setVec2("u_cameraRight", gameState.cameraRight);
    shader->setVec2("u_cameraUp", gameState.cameraUp);
    shader->setVec2("u_cameraSize", gameState.cameraSize);
    shader->setVec2("u_winSize", gameState.windowSize);
    shader->setFloat("u_screenRight", gameState.screenRight);
    shader->setFloat("u_cameraUnzoomedX", gameState.cameraUnzoomedX);
    shader->setVec3("u_specialLightBGColor", gameState.specialLightBGColor);
    shader->setFloat("u_gameStateFlags", static_cast<float>(gameState.gameStateFlags));

    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    const GLsizei stride = static_cast<GLsizei>(sizeof(ObjectVertex));

    glVertexAttribPointer(
        0, 2, GL_FLOAT, GL_FALSE, stride,
        reinterpret_cast<const void*>(offsetof(ObjectVertex, positionOffset))
    );
    glVertexAttribPointer(
        1, 2, GL_FLOAT, GL_FALSE, stride,
        reinterpret_cast<const void*>(offsetof(ObjectVertex, texCoord))
    );
    glVertexAttribPointer(
        2, 2, GL_FLOAT, GL_FALSE, stride,
        reinterpret_cast<const void*>(offsetof(ObjectVertex, objectAndGroupIndex))
    );
    glVertexAttribPointer(
        3, 2, GL_FLOAT, GL_FALSE, stride,
        reinterpret_cast<const void*>(offsetof(ObjectVertex, spriteAndMeta))
    );
    glVertexAttribPointer(
        4, 2, GL_FLOAT, GL_FALSE, stride,
        reinterpret_cast<const void*>(offsetof(ObjectVertex, objectStartPosition))
    );

    for (GLuint index = 0; index < 5; ++index)
        glEnableVertexAttribArray(index);

    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    if (key.blending || key.spriteSheet == SpriteSheet::GLOW)
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    else
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glDrawArrays(
        GL_TRIANGLES,
        static_cast<GLint>(range.firstVertex),
        static_cast<GLsizei>(range.vertexCount)
    );

    // Restore Cocos' GL state before checking the submission result so
    // failures in either drawing or restoration are reported consistently.
    savedGLState.restore();
    const GLenum drawError = glGetError();
    if (drawError != GL_NO_ERROR) {
        fail(fmt::format("GPU layer draw/state restore failed (GL error 0x{:X})", drawError));
        return;
    }

    ++drawsThisFrame;
    gpuVerticesSubmittedThisFrame += range.vertexCount;
    gpuTrianglesSubmittedThisFrame += range.vertexCount / 3;
    drawSubmitMillisecondsThisFrame += std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - submitStart
    ).count();
}

} // namespace IOSUpstream

#endif
