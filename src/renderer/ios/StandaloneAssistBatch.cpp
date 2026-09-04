#ifdef GEODE_IS_IOS

#include "StandaloneAssistBatch.hpp"
#include "AtlasInterleave.hpp"

#include "Geode/cocos/cocoa/CCAffineTransform.h"
#include "Geode/cocos/kazmath/include/kazmath/mat4.h"
#include <algorithm>
#include <cstddef>

using namespace geode::prelude;

namespace {
constexpr usize MAX_BATCH_SPRITES = 16383;

static glm::vec2 quadUV(const cocos2d::ccV3F_C4B_T2F& vertex) {
    return { vertex.texCoords.u, vertex.texCoords.v };
}

static bool getSpriteLocalTransform(
    GameObject* object,
    cocos2d::CCSprite* sprite,
    cocos2d::CCAffineTransform& out
) {
    out = cocos2d::CCAffineTransformMakeIdentity();
    if (!object || !sprite)
        return false;

    if (sprite == static_cast<cocos2d::CCSprite*>(object))
        return true;

    auto node = static_cast<cocos2d::CCNode*>(sprite);
    while (node && node != object) {
        out = cocos2d::CCAffineTransformConcat(out, node->nodeToParentTransform());
        node = node->getParent();
    }

    return node == object;
}
} // namespace

StandaloneAssistBatch::~StandaloneAssistBatch() {
    destroyGL();
}

geode::Ref<StandaloneAssistBatch> StandaloneAssistBatch::create(
    ResolvedStateLayer* state,
    Shader* assistShader,
    const std::vector<ResolvedStateLayer::ShadowCandidate>& candidates,
    bool addressableRoots
) {
    auto node = new StandaloneAssistBatch();
    if (node->initWithCandidates(state, assistShader, candidates, addressableRoots)) {
        node->autorelease();
        return node;
    }
    delete node;
    return nullptr;
}

bool StandaloneAssistBatch::initWithCandidates(
    ResolvedStateLayer* state,
    Shader* assistShader,
    const std::vector<ResolvedStateLayer::ShadowCandidate>& candidates,
    bool addressableRoots
) {
    if (!CCNode::init() || !state || !assistShader || candidates.empty() || !state->isGPUStateReady())
        return false;

    resolvedState = state;
    shader = assistShader;

    // Parentless-only sets are the deferred-atlas case discovered on iOS: GD
    // will later move those roots into a stock batch. They must be coalesced now
    // instead of reserving one GL range per future root. A genuinely parented
    // standalone set keeps root-addressable spans.
    bool hasParentedRoot = false;
    for (const auto& candidate : candidates) {
        if (candidate.object && candidate.object->getParent()) {
            hasParentedRoot = true;
            break;
        }
    }
    rootAddressable = addressableRoots && hasParentedRoot;

    GLint vertexTextureUnits = 0;
    glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &vertexTextureUnits);
    if (vertexTextureUnits < 2)
        return false;

    if (!buildGeometry(candidates))
        return false;

    setVisible(true);
    stats.ready = true;
    if (!rootAddressable)
        AtlasInterleaveRegistry::registerDeferred(this);
    return true;
}

void StandaloneAssistBatch::destroyGL() {
    AtlasInterleaveRegistry::unregisterDeferred(this);

    if (vao)
        glDeleteVertexArrays(1, &vao);
    vao = 0;

    if (vertexBuffer)
        Buffer::destroy(vertexBuffer);
    if (indexBuffer)
        Buffer::destroy(indexBuffer);
    vertexBuffer = nullptr;
    indexBuffer = nullptr;

    drawRanges.clear();
    rootDrawSpans.clear();
    ownedSprites.clear();
    stats.ready = false;
}

bool StandaloneAssistBatch::buildGeometry(
    const std::vector<ResolvedStateLayer::ShadowCandidate>& candidates
) {
    if (candidates.empty() || candidates.size() > MAX_BATCH_SPRITES)
        return false;

    std::vector<Vertex> vertices;
    std::vector<u16> indices;
    vertices.reserve(candidates.size() * 4);
    indices.reserve(candidates.size() * 6);
    ownedSprites.clear();
    drawRanges.clear();
    rootDrawSpans.clear();

    u32 activeTexture = 0;
    u32 activeBlendSrc = 0;
    u32 activeBlendDst = 0;
    DrawRange* activeRange = nullptr;

    GameObject* currentRoot = nullptr;
    usize currentRootFirstRange = 0;

    auto finishRoot = [&]() {
        if (!rootAddressable || !currentRoot)
            return;
        const usize count = drawRanges.size() - currentRootFirstRange;
        if (count > 0)
            rootDrawSpans[currentRoot] = { currentRootFirstRange, count };
    };

    for (const auto& candidate : candidates) {
        auto object = candidate.object;
        auto sprite = candidate.sprite;
        if (!object || !sprite || sprite->getBatchNode())
            return false;

        // Root-addressable buffers need a range boundary per object so a stock
        // root visit can draw exactly one subtree. Deferred-atlas buffers are
        // parentless at creation, so identical texture/blend state stays merged
        // across object boundaries and thousands of roots do not become thousands
        // of GL draw calls.
        if (rootAddressable && object != currentRoot) {
            finishRoot();
            currentRoot = object;
            currentRootFirstRange = drawRanges.size();
            activeRange = nullptr;
            activeTexture = 0;
            activeBlendSrc = 0;
            activeBlendDst = 0;
        }

        auto texture = sprite->getTexture();
        if (!texture || !texture->getName())
            return false;

        cocos2d::CCAffineTransform localTransform;
        if (!getSpriteLocalTransform(object, sprite, localTransform))
            return false;

        const auto crop = sprite->getTextureRect();
        const auto localBottomLeftPoint = cocos2d::CCPointApplyAffineTransform(
            sprite->getOffsetPosition(),
            localTransform
        );

        const glm::vec2 bottomLeft = ccPointToGLM(localBottomLeftPoint);
        const glm::vec2 right = {
            localTransform.a * crop.size.width,
            localTransform.b * crop.size.width
        };
        const glm::vec2 up = {
            localTransform.c * crop.size.height,
            localTransform.d * crop.size.height
        };

        const auto quad = sprite->getQuad();
        const glm::vec2 uvBL = quadUV(quad.bl);
        const glm::vec2 uvBR = quadUV(quad.br);
        const glm::vec2 uvTL = quadUV(quad.tl);
        const glm::vec2 uvTR = quadUV(quad.tr);

        const auto blend = sprite->getBlendFunc();
        const u32 textureId = texture->getName();
        const u32 blendSrc = (u32)blend.src;
        const u32 blendDst = (u32)blend.dst;

        if (!activeRange ||
            activeTexture != textureId ||
            activeBlendSrc != blendSrc ||
            activeBlendDst != blendDst) {
            drawRanges.push_back({
                textureId,
                blendSrc,
                blendDst,
                (u32)indices.size(),
                0
            });
            activeRange = &drawRanges.back();
            activeTexture = textureId;
            activeBlendSrc = blendSrc;
            activeBlendDst = blendDst;
        }

        const u16 baseVertex = (u16)vertices.size();
        const float objectIndex = (float)candidate.objectStateIndex;
        const float spriteIndex = (float)candidate.spriteStateIndex;

        vertices.push_back({ bottomLeft, uvBL, objectIndex, spriteIndex });
        vertices.push_back({ bottomLeft + right, uvBR, objectIndex, spriteIndex });
        vertices.push_back({ bottomLeft + up, uvTL, objectIndex, spriteIndex });
        vertices.push_back({ bottomLeft + right + up, uvTR, objectIndex, spriteIndex });

        indices.push_back(baseVertex + 0);
        indices.push_back(baseVertex + 2);
        indices.push_back(baseVertex + 3);
        indices.push_back(baseVertex + 0);
        indices.push_back(baseVertex + 3);
        indices.push_back(baseVertex + 1);
        activeRange->indexCount += 6;

        ownedSprites.push_back(sprite);
    }

    finishRoot();

    if (ownedSprites.size() != candidates.size() || vertices.empty() || indices.empty() || drawRanges.empty())
        return false;
    if (rootAddressable && rootDrawSpans.empty())
        return false;

    vertexBuffer = Buffer::createStaticDraw(
        rootAddressable ? "Standalone resolved GPU vertices" : "Deferred atlas GPU vertices",
        vertices.data(),
        vertices.size() * sizeof(Vertex)
    );
    indexBuffer = Buffer::createStaticDraw(
        rootAddressable ? "Standalone resolved GPU indices" : "Deferred atlas GPU indices",
        indices.data(),
        indices.size() * sizeof(u16)
    );
    if (!vertexBuffer || !indexBuffer) {
        destroyGL();
        return false;
    }

    GLint previousVAO = 0;
    GLint previousVBO = 0;
    GLint previousIBO = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVAO);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousVBO);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &previousIBO);

    glGenVertexArrays(1, &vao);
    if (!vao) {
        destroyGL();
        return false;
    }

    glBindVertexArray(vao);
    vertexBuffer->bindAs(GL_ARRAY_BUFFER);
    indexBuffer->bindAs(GL_ELEMENT_ARRAY_BUFFER);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, localPosition));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, texCoord));
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, objectStateIndex));
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, spriteStateIndex));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);

    glBindVertexArray((u32)previousVAO);
    glBindBuffer(GL_ARRAY_BUFFER, (u32)previousVBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (u32)previousIBO);

    stats.sprites = ownedSprites.size();
    stats.textureRanges = drawRanges.size();
    stats.verticesResident = vertices.size();
    return true;
}

void StandaloneAssistBatch::beginFrame() {
    stats.drawCallsLastFrame = 0;
    stats.indicesLastFrame = 0;
}

bool StandaloneAssistBatch::ownsRoot(GameObject* root) const {
    return rootAddressable && root && rootDrawSpans.contains(root);
}

bool StandaloneAssistBatch::drawRoot(GameObject* root) {
    if (!rootAddressable || !root)
        return false;
    auto it = rootDrawSpans.find(root);
    if (it == rootDrawSpans.end())
        return false;
    return drawRangeSpan(it->second.firstRange, it->second.rangeCount);
}

void StandaloneAssistBatch::draw() {
    if (rootAddressable)
        drawRangeSpan(0, drawRanges.size());
}

bool StandaloneAssistBatch::drawRangeSpan(usize firstRange, usize rangeCount) {
    if (!stats.ready || !isVisible() || !resolvedState || !shader || !vao)
        return false;
    if (!resolvedState->isGPUStateReady() || rangeCount == 0 || firstRange >= drawRanges.size())
        return false;

    const usize endRange = std::min<usize>(
        static_cast<usize>(drawRanges.size()),
        firstRange + rangeCount
    );
    if (endRange <= firstRange)
        return false;

    auto objectStateTexture = resolvedState->getObjectStateTexture();
    auto spriteStateTexture = resolvedState->getSpriteStateTexture();
    if (!objectStateTexture || !spriteStateTexture)
        return false;

    kmMat4 matrixP;
    kmMat4 matrixMV;
    kmMat4 matrixMVP;
    kmGLGetMatrix(KM_GL_PROJECTION, &matrixP);
    kmGLGetMatrix(KM_GL_MODELVIEW, &matrixMV);
    kmMat4Multiply(&matrixMVP, &matrixP, &matrixMV);

    GLint previousVAO = 0;
    GLint previousVBO = 0;
    GLint previousIBO = 0;
    GLint previousProgram = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTextures[3] = {0, 0, 0};
    GLint previousBlendSrcRGB = GL_SRC_ALPHA;
    GLint previousBlendDstRGB = GL_ONE_MINUS_SRC_ALPHA;
    GLint previousBlendSrcAlpha = GL_SRC_ALPHA;
    GLint previousBlendDstAlpha = GL_ONE_MINUS_SRC_ALPHA;
    GLboolean previousBlendEnabled = glIsEnabled(GL_BLEND);
    GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean previousDepthMask = GL_TRUE;
    GLint previousStencilMask = 0;

    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVAO);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousVBO);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &previousIBO);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glGetIntegerv(GL_BLEND_SRC_RGB, &previousBlendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &previousBlendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousBlendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &previousBlendDstAlpha);
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &previousStencilMask);

    for (i32 unit = 0; unit < 3; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTextures[unit]);
    }

    shader->use();
    shader->setMatrix4("u_mvp", matrixMVP.mat);

    objectStateTexture->bind(1);
    shader->setInt("u_objectStateTexture", 1);
    shader->setVec2("u_objectStateTextureSize", objectStateTexture->getSize());

    spriteStateTexture->bind(2);
    shader->setInt("u_spriteStateTexture", 2);
    shader->setVec2("u_spriteStateTextureSize", spriteStateTexture->getSize());

    glBindVertexArray(vao);
    glEnable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    glStencilMask(0);

    for (usize rangeIndex = firstRange; rangeIndex < endRange; ++rangeIndex) {
        const auto& range = drawRanges[rangeIndex];
        if (!range.textureId || !range.indexCount)
            continue;

        glBlendFunc((GLenum)range.blendSrc, (GLenum)range.blendDst);
        shader->setTexture("u_spriteSheetTexture", 0, range.textureId);
        glDrawElements(
            GL_TRIANGLES,
            (GLsizei)range.indexCount,
            GL_UNSIGNED_SHORT,
            (void*)((usize)range.startIndex * sizeof(u16))
        );
        ++stats.drawCallsLastFrame;
        stats.indicesLastFrame += range.indexCount;
    }

    glColorMask(
        previousColorMask[0],
        previousColorMask[1],
        previousColorMask[2],
        previousColorMask[3]
    );
    glDepthMask(previousDepthMask);
    glStencilMask((u32)previousStencilMask);
    glBlendFuncSeparate(
        (GLenum)previousBlendSrcRGB,
        (GLenum)previousBlendDstRGB,
        (GLenum)previousBlendSrcAlpha,
        (GLenum)previousBlendDstAlpha
    );
    if (!previousBlendEnabled)
        glDisable(GL_BLEND);

    glBindVertexArray((u32)previousVAO);
    glBindBuffer(GL_ARRAY_BUFFER, (u32)previousVBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (u32)previousIBO);
    glUseProgram((u32)previousProgram);

    for (i32 unit = 0; unit < 3; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, (u32)previousTextures[unit]);
    }
    glActiveTexture((GLenum)previousActiveTexture);
    return true;
}

#endif