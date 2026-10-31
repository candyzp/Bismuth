#ifdef GEODE_IS_IOS

#include "IOSGPURenderer.hpp"

#include "../Shader.hpp"
#include <Geode/cocos/CCDirector.h>
#include <Geode/cocos/label_nodes/CCLabelBMFont.h>
#include <Geode/cocos/label_nodes/CCLabelTTF.h>
#include <Geode/cocos/sprite_nodes/CCSpriteBatchNode.h>
#include <Geode/utils/cocos.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>

using namespace geode::prelude;

namespace IOSUpstream {

namespace {

cocos2d::CCNode* resolveDebugParent(PlayLayer* layer) {
    if (layer && layer->getParent())
        return layer;
    if (auto scene = CCDirector::get()->getRunningScene())
        return scene;
    return nullptr;
}

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
    if (debugOverlay) {
        if (debugOverlay->getParent())
            debugOverlay->removeFromParentAndCleanup(true);
        debugOverlay->release();
        debugOverlay = nullptr;
    }

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

void IOSGPURenderer::createDebugOverlay() {
    if (!layer || debugOverlay)
        return;

    auto parent = resolveDebugParent(layer);
    if (!parent) {
        log::error("Bismuth iOS GPU: no scene parent available for startup debug overlay");
        return;
    }

    auto label = cocos2d::CCLabelTTF::create(
        "BISMUTH iOS GPU: STARTING",
        "Arial",
        20.0f
    );
    if (!label) {
        log::error("Bismuth iOS GPU: failed to create the startup debug status label");
        return;
    }

    debugOverlay = label;
    debugOverlay->retain();
    debugOverlay->setAnchorPoint(CCPoint(0.f, 1.f));
    debugOverlay->setPosition(CCPoint(
        8.f,
        CCDirector::get()->getWinSize().height - 8.f
    ));
    debugOverlay->setScale(1.0f);
    debugOverlay->setColor({255, 235, 90});
    debugOverlay->setOpacity(240);
    debugOverlay->setVisible(true);
    parent->addChild(debugOverlay, 1000000);
    updateDebugOverlay();
}

void IOSGPURenderer::updateDebugOverlay() {
    if (!debugOverlay)
        return;

    auto parent = resolveDebugParent(layer);
    if (!parent) {
        log::warn("Bismuth iOS GPU: cannot reattach startup debug overlay; no scene parent is active");
        return;
    }

    if (debugOverlay->getParent() != parent) {
        if (debugOverlay->getParent())
            debugOverlay->removeFromParentAndCleanup(false);
        parent->addChild(debugOverlay, 1000000);
    }

    const auto winSize = CCDirector::get()->getWinSize();
    debugOverlay->setPosition(CCPoint(8.f, winSize.height - 8.f));

    std::string status = failed
        ? "BISMUTH iOS GPU: FAILED"
        : initialized
            ? "BISMUTH iOS GPU: ACTIVE"
            : "BISMUTH iOS GPU: STARTING";

    std::string detail;
    if (failed) {
        detail = failureReason;
        debugOverlay->setColor({255, 105, 105});
    } else {
        const auto& batch = sceneData.getObjectBatch();
        detail = "Sprites " + std::to_string(batch.getSpriteCount()) +
            " | Vertices " + std::to_string(batch.getOrderedVertices().size()) +
            " | GPU layers " + std::to_string(layerNodes.size()) + "/" +
            std::to_string(activeLayerCount) +
            " | Missing " + std::to_string(missingLayerNodeCount) +
            " | Visits/frame " + std::to_string(lastFrameLayerVisits) +
            " | GPU draws/frame " + std::to_string(lastFrameDraws);
        if (batch.getSpriteCount() == 0 ||
            batch.getOrderedVertices().empty() ||
            (activeLayerCount != 0 && lastFrameDraws == 0)) {
            debugOverlay->setColor({255, 190, 80});
        } else {
            debugOverlay->setColor({105, 255, 150});
        }
        debugOverlay->setColor(initialized
            ? ccColor3B{105, 255, 150}
            : ccColor3B{255, 235, 90});
    }

    const std::string message = status + "\n" + detail;
    debugOverlay->setString(message.c_str());
    debugOverlay->setVisible(true);
}

void IOSGPURenderer::beginFrame() {
    layerVisitsThisFrame = 0;
    drawsThisFrame = 0;
}

void IOSGPURenderer::finishFrame() {
    lastFrameLayerVisits = layerVisitsThisFrame;
    lastFrameDraws = drawsThisFrame;
    updateDebugOverlay();
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

    // Keep the owner node attached even when initialization fails so its
    // on-screen diagnostic remains available. Stock ownership is transferred
    // only after the complete GPU path is ready.
    ret->initForPlayLayer(playLayer);
    ret->autorelease();
    return ret;
}

} // namespace IOSUpstream

#endif
