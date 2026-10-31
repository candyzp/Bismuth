#ifdef GEODE_IS_IOS

#include "IOSGPURenderer.hpp"

#include "../Shader.hpp"
#include <Geode/cocos/CCDirector.h>
#include <Geode/cocos/sprite_nodes/CCSpriteBatchNode.h>
#include <Geode/utils/cocos.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>

using namespace geode::prelude;

namespace IOSUpstream {

IOSGPURenderer* IOSGPURenderer::currentRenderer = nullptr;

namespace {

constexpr GLint OBJECT_DATA_TEXTURE_UNIT = 0;
constexpr GLint GROUP_DATA_TEXTURE_UNIT = 1;
constexpr GLint BYTE_DATA_TEXTURE_UNIT = 2;
constexpr GLint SPRITE_SHEET_TEXTURE_UNIT = 3;

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
    static constexpr GLint TEXTURE_UNIT_COUNT = 4;

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

            // GLES2 attribute arrays are VBO-backed. A disabled attribute may
            // have no buffer associated with it; do not attempt to restore a
            // client-memory pointer in that case.
            if (attrib.buffer != 0) {
                glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(attrib.buffer));
                glVertexAttribPointer(
                    index,
                    attrib.size,
                    static_cast<GLenum>(attrib.type),
                    static_cast<GLboolean>(attrib.normalized),
                    attrib.stride,
                    attrib.pointer
                );
            }
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

    if (vertexBuffer) {
        glDeleteBuffers(1, &vertexBuffer);
        vertexBuffer = 0;
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

IOSGPURenderer* IOSGPURenderer::create(PlayLayer* playLayer) {
    if (currentRenderer && currentRenderer->getPlayLayer() != playLayer) {
        auto previous = currentRenderer;
        if (previous->getParent())
            previous->removeFromParentAndCleanup(true);
    }
    if (currentRenderer && currentRenderer->getPlayLayer() == playLayer)
        return currentRenderer;

    auto ret = new IOSGPURenderer();
    if (!ret->cocos2d::CCNode::init()) {
        delete ret;
        return nullptr;
    }

    // GPU-only beta: initialization failure never unhides stock Cocos batches.
    // Keep the failed owner node attached so the failure remains visible in logs
    // and the legacy renderer cannot silently reclaim the same frame.
    ret->initForPlayLayer(playLayer);
    ret->autorelease();
    return ret;
}

void IOSGPURenderer::fail(const std::string& reason) {
    if (failed)
        return;

    failed = true;
    failureReason = reason;
    log::error("Bismuth iOS GPU-only beta FAILED: {}", failureReason);

    // No CPU renderer fallback. Stop any remaining layer submissions after the
    // first error in this frame; stock Cocos batches remain hidden.
    for (auto node : layerNodes) {
        if (node)
            node->setVisible(false);
    }
}

void IOSGPURenderer::hideStockBatches() {
    if (!layer || !layer->m_batchNodes)
        return;

    for (auto batch : CCArrayExt<cocos2d::CCNode*>(layer->m_batchNodes)) {
        if (batch)
            batch->setVisible(false);
    }
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
    if (capabilities.maxCombinedTextureUnits < 4 ||
        capabilities.maxFragmentTextureUnits < 1) {
        fail("requires four combined texture units and one fragment texture unit");
        return false;
    }
    if (capabilities.maxVertexAttribs < 5) {
        fail("requires five vertex attributes");
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
    if (vertexBuffer) {
        glDeleteBuffers(1, &vertexBuffer);
        vertexBuffer = 0;
    }

    // A valid level may contain no renderable sprites (for example, a blank
    // level or one containing only gameplay triggers). The GPU renderer still
    // owns that scene; it simply has no vertex buffer or layer submissions.
    if (sceneData.getObjectBatch().getOrderedVertices().empty()) {
        uploadedOrderRevision = sceneData.getObjectBatch().getOrderRevision();
        return true;
    }

    glGenBuffers(1, &vertexBuffer);
    if (!vertexBuffer) {
        fail("glGenBuffers returned zero for ordered object vertex stream");
        return false;
    }

    return uploadOrderedVertices();
}

bool IOSGPURenderer::uploadOrderedVertices() {
    if (!sceneData.isReady())
        return false;

    const auto& vertices = sceneData.getObjectBatch().getOrderedVertices();
    if (vertices.empty()) {
        uploadedOrderRevision = sceneData.getObjectBatch().getOrderRevision();
        return true;
    }
    if (!vertexBuffer)
        return false;

    if (vertices.size() >
        static_cast<usize>(std::numeric_limits<GLsizei>::max())) {
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
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(byteCount),
        vertices.data(),
        GL_DYNAMIC_DRAW
    );
    const GLenum error = glGetError();

    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousBuffer));
    if (error != GL_NO_ERROR) {
        fail(fmt::format("ordered vertex-buffer upload failed (GL error 0x{:X})", error));
        return false;
    }

    uploadedOrderRevision = sceneData.getObjectBatch().getOrderRevision();
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

    // The owner tracks both potential blend variants, but only currently
    // scheduled ranges need nodes. A color action may activate the other
    // variant later, so synchronize this set again after each state update.
    const auto& ranges = sceneData.getObjectBatch().getSortedBatch().layerRanges;
    for (const auto& [key, range] : ranges) {
        if (range.vertexCount == 0 || layerNodeKeys.contains(key))
            continue;

        cocos2d::CCNode* stockNode = layer->parentForZLayer(
            static_cast<i32>(key.zlayer),
            key.blending,
            static_cast<i32>(key.spriteSheet),
            false
        );

        if (!stockNode || layer->m_batchNodes->indexOfObject(stockNode) == UINT_MAX) {
            fail(fmt::format(
                "active GPU layer has no stock Z-order container (Z={}, sheet={}, blend={})",
                static_cast<i32>(key.zlayer),
                static_cast<i32>(key.spriteSheet),
                key.blending
            ));
            return false;
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

    // No active ranges means there is nothing to submit. This is a valid
    // GPU-owned empty scene, not a renderer initialization failure.
    return true;
}

bool IOSGPURenderer::initForPlayLayer(PlayLayer* playLayer) {
    layer = playLayer;
    currentRenderer = this;

    if (!layer) {
        fail("PlayLayer is null");
        return false;
    }

    // This is deliberately the first rendering-ownership operation. There is no
    // fallback, including during shader/table initialization failure.
    hideStockBatches();

    if (!queryAndValidateCapabilities())
        return false;

    if (!sceneData.init(layer)) {
        fail("owner-style CPU scene data or GPU state tables failed to initialize");
        return false;
    }

    if (!createShaderProgram())
        return false;

    if (!createVertexBuffer())
        return false;

    if (!createLayerNodes())
        return false;

    log::info(
        "Bismuth iOS GPU-only renderer initialized: {} sprites, {} layer nodes, {} group combinations",
        sceneData.getObjectBatch().getSpriteCount(),
        layerNodes.size(),
        sceneData.getGroupManager().getGroupCombinationCount()
    );
    return true;
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
    syncLayerNodes();
}

void IOSGPURenderer::drawLayer(const LayerKey& key) {
    if (failed || !sceneData.isReady() || !shader || !vertexBuffer)
        return;

    const auto& batch = sceneData.getObjectBatch();
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

    if (batch.getOrderRevision() != uploadedOrderRevision &&
        !uploadOrderedVertices()) {
        fail("could not upload updated blend-order vertex stream");
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

}

} // namespace IOSUpstream

#endif
