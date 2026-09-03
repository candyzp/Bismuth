#ifdef GEODE_IS_IOS

#include "StandaloneAssistBatch.hpp"

#include "Geode/cocos/kazmath/include/kazmath/mat4.h"
#include <algorithm>
#include <cstddef>

using namespace geode::prelude;

namespace {
constexpr usize MAX_BATCH_SPRITES = 16383;

static glm::vec2 quadUV(const cocos2d::ccV3F_C4B_T2F& vertex) {
    return { vertex.texCoords.u, vertex.texCoords.v };
}
} // namespace

StandaloneAssistBatch::~StandaloneAssistBatch() {
    destroyGL();
}

geode::Ref<StandaloneAssistBatch> StandaloneAssistBatch::create(
    ResolvedStateLayer* state,
    Shader* assistShader,
    const std::vector<ResolvedStateLayer::ShadowCandidate>& candidates
) {
    auto node = new StandaloneAssistBatch();
    if (node->initWithCandidates(state, assistShader, candidates)) {
        node->autorelease();
        return node;
    }
    delete node;
    return nullptr;
}

bool StandaloneAssistBatch::initWithCandidates(
    ResolvedStateLayer* state,
    Shader* assistShader,
    const std::vector<ResolvedStateLayer::ShadowCandidate>& candidates
) {
    if (!CCNode::init() || !state || !assistShader || candidates.empty() || !state->isGPUStateReady())
        return false;

    resolvedState = state;
    shader = assistShader;

    GLint vertexTextureUnits = 0;
    glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &vertexTextureUnits);
    if (vertexTextureUnits < 2)
        return false;

    auto firstSprite = candidates.front().sprite;
    if (!firstSprite)
        return false;

    const auto blend = firstSprite->getBlendFunc();
    blendSrc = (u32)blend.src;
    blendDst = (u32)blend.dst;

    if (!buildGeometry(candidates))
        return false;

    setVisible(true);
    stats.ready = true;
    return true;
}

void StandaloneAssistBatch::destroyGL() {
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
    ownedSprites.clear();
    stats.ready = false;
}

bool StandaloneAssistBatch::buildGeometry(
    const std::vector<ResolvedStateLayer::ShadowCandidate>& candidates
) {
    std::vector<Vertex> vertices;
    std::vector<u16> indices;
    vertices.reserve(std::min<usize>(candidates.size(), MAX_BATCH_SPRITES) * 4);
    indices.reserve(std::min<usize>(candidates.size(), MAX_BATCH_SPRITES) * 6);
    ownedSprites.clear();
    drawRanges.clear();

    u32 activeTexture = 0;
    DrawRange* activeRange = nullptr;

    for (const auto& candidate : candidates) {
        if (ownedSprites.size() >= MAX_BATCH_SPRITES)
            break;

        auto object = candidate.object;
        auto sprite = candidate.sprite;
        if (!object || !sprite || sprite != static_cast<cocos2d::CCSprite*>(object))
            continue;
        if (sprite->getBatchNode() || sprite->getChildrenCount() != 0)
            continue;

        auto texture = sprite->getTexture();
        if (!texture || !texture->getName())
            continue;

        const auto blend = sprite->getBlendFunc();
        if ((u32)blend.src != blendSrc || (u32)blend.dst != blendDst)
            continue;

        const auto crop = sprite->getTextureRect();
        const auto offset = sprite->getOffsetPosition();
        const auto anchor = object->getAnchorPointInPoints();

        // The assist node is an identity sibling under the same parent. The
        // shader receives the GameObject position/rotation/scale, so geometry is
        // authored relative to the root anchor exactly like the batched path.
        const glm::vec2 bottomLeft = {
            offset.x - anchor.x,
            offset.y - anchor.y
        };
        const glm::vec2 right = { crop.size.width, 0.f };
        const glm::vec2 up = { 0.f, crop.size.height };

        // Standalone CCSprite already owns the authoritative Cocos quad. Reuse
        // its final UV corners rather than recreating rotated/flip rules.
        const auto quad = sprite->getQuad();
        const glm::vec2 uvBL = quadUV(quad.bl);
        const glm::vec2 uvBR = quadUV(quad.br);
        const glm::vec2 uvTL = quadUV(quad.tl);
        const glm::vec2 uvTR = quadUV(quad.tr);

        if (!activeRange || activeTexture != texture->getName()) {
            drawRanges.push_back({ texture->getName(), (u32)indices.size(), 0 });
            activeRange = &drawRanges.back();
            activeTexture = texture->getName();
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

    if (vertices.empty() || indices.empty() || drawRanges.empty())
        return false;

    vertexBuffer = Buffer::createStaticDraw(
        "Standalone resolved GPU vertices",
        vertices.data(),
        vertices.size() * sizeof(Vertex)
    );
    indexBuffer = Buffer::createStaticDraw(
        "Standalone resolved GPU indices",
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

void StandaloneAssistBatch::draw() {
    stats.drawCallsLastFrame = 0;
    stats.indicesLastFrame = 0;

    if (!stats.ready || !isVisible() || !resolvedState || !shader || !vao)
        return;
    if (!resolvedState->isGPUStateReady())
        return;

    auto objectStateTexture = resolvedState->getObjectStateTexture();
    auto spriteStateTexture = resolvedState->getSpriteStateTexture();
    if (!objectStateTexture || !spriteStateTexture)
        return;

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
    glBlendFunc((GLenum)blendSrc, (GLenum)blendDst);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    glStencilMask(0);

    for (const auto& range : drawRanges) {
        if (!range.textureId || !range.indexCount)
            continue;

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
}

#endif
