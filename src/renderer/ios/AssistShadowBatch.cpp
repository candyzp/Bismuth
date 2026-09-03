#ifdef GEODE_IS_IOS

#include "AssistShadowBatch.hpp"

#include "Geode/cocos/CCDirector.h"
#include "Geode/cocos/cocoa/CCAffineTransform.h"
#include "Geode/cocos/kazmath/include/kazmath/mat4.h"

#include <algorithm>
#include <cstddef>

using namespace geode::prelude;

namespace {
// Four vertices per sprite with a u16 index buffer. If one stock batch contains
// more than this many safe sprites we keep the overflow on stock Cocos instead
// of disabling GPU ownership for the entire batch.
constexpr usize MAX_BATCH_SPRITES = 16383;

struct CandidateWithTexture {
    ResolvedStateLayer::ShadowCandidate candidate;
    u32 textureId = 0;
    u32 atlasIndex = 0;
    usize originalOrder = 0;
};

static bool isDescendantOf(cocos2d::CCNode* node, cocos2d::CCNode* ancestor) {
    for (auto current = node; current; current = current->getParent()) {
        if (current == ancestor)
            return true;
    }
    return false;
}

// Build only child-to-GameObject geometry. Geometry Dash resolves the root
// GameObject state; the vertex shader applies root translation/rotation/scale.
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

static glm::vec2 quadUV(const cocos2d::ccV3F_C4B_T2F& vertex) {
    return { vertex.texCoords.u, vertex.texCoords.v };
}
} // namespace

AssistShadowBatch::~AssistShadowBatch() {
    destroyGL();
}

geode::Ref<AssistShadowBatch> AssistShadowBatch::create(
    ResolvedStateLayer* state,
    Shader* assistShader,
    cocos2d::CCSpriteBatchNode* sourceBatch
) {
    auto node = new AssistShadowBatch();
    if (node->initWithState(state, assistShader, sourceBatch)) {
        node->autorelease();
        return node;
    }
    delete node;
    return nullptr;
}

bool AssistShadowBatch::initWithState(
    ResolvedStateLayer* state,
    Shader* assistShader,
    cocos2d::CCSpriteBatchNode* sourceBatch
) {
    if (!CCNode::init() || !state || !assistShader || !sourceBatch || !state->isGPUStateReady())
        return false;

    resolvedState = state;
    shader = assistShader;
    stockBatch = sourceBatch;

    GLint vertexTextureUnits = 0;
    glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &vertexTextureUnits);
    if (vertexTextureUnits < 2) {
        log::warn(
            "Bismuth iOS assist batch needs 2 vertex texture units; device reports {}",
            vertexTextureUnits
        );
        return false;
    }

    const auto blend = stockBatch->getBlendFunc();
    blendSrc = (u32)blend.src;
    blendDst = (u32)blend.dst;

    if (!buildGeometry())
        return false;

    setVisible(true);
    stats.ready = true;
    stats.visibleOwnership = true;
    return true;
}

void AssistShadowBatch::destroyGL() {
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
    stats.visibleOwnership = false;
}

bool AssistShadowBatch::buildGeometry() {
    const auto sourceCandidates = resolvedState->getGPUCandidates();

    std::vector<CandidateWithTexture> candidates;
    candidates.reserve(std::min<usize>(sourceCandidates.size(), MAX_BATCH_SPRITES));
    ownedSprites.clear();

    usize ordinal = 0;
    for (const auto& candidate : sourceCandidates) {
        if (!candidate.object || !candidate.sprite)
            continue;
        if (!isDescendantOf(candidate.sprite, stockBatch))
            continue;

        ++stats.eligibleSprites;

        // No whole-batch fallback. Bad/overflow records stay stock individually
        // while every valid safe sprite in the same Cocos batch still gets GPU
        // ownership.
        if (candidates.size() >= MAX_BATCH_SPRITES) {
            ++stats.rejectedSprites;
            continue;
        }

        auto texture = candidate.sprite->getTexture();
        if (!texture || texture->getName() == 0) {
            ++stats.rejectedSprites;
            continue;
        }

        if (candidate.sprite->getBatchNode() != stockBatch ||
            candidate.sprite->getAtlasIndex() == CCSpriteIndexNotInitialized) {
            ++stats.rejectedSprites;
            continue;
        }

        cocos2d::CCAffineTransform localTransform;
        if (!getSpriteLocalTransform(candidate.object, candidate.sprite, localTransform)) {
            ++stats.rejectedSprites;
            continue;
        }

        candidates.push_back({
            candidate,
            texture->getName(),
            candidate.sprite->getAtlasIndex(),
            ordinal++
        });
    }

    if (candidates.empty())
        return false;

    // Preserve Cocos atlas order among the GPU-owned subset. Animated/complex
    // sprites remain in the stock batch and are never inserted here.
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.atlasIndex != b.atlasIndex)
            return a.atlasIndex < b.atlasIndex;
        return a.originalOrder < b.originalOrder;
    });

    std::vector<Vertex> vertices;
    std::vector<u16> indices;
    vertices.reserve(candidates.size() * 4);
    indices.reserve(candidates.size() * 6);
    drawRanges.clear();

    u32 activeTexture = 0;
    DrawRange* activeRange = nullptr;

    for (const auto& entry : candidates) {
        auto object = entry.candidate.object;
        auto sprite = entry.candidate.sprite;
        auto texture = sprite->getTexture();
        if (!object || !sprite || !texture) {
            ++stats.rejectedSprites;
            continue;
        }

        cocos2d::CCAffineTransform localTransform;
        if (!getSpriteLocalTransform(object, sprite, localTransform)) {
            ++stats.rejectedSprites;
            continue;
        }

        const auto crop = sprite->getTextureRect();
        const auto localBottomLeftPoint = cocos2d::CCPointApplyAffineTransform(
            sprite->getOffsetPosition(),
            localTransform
        );

        // Cocos' root nodeToParentTransform rotates/scales around the root
        // anchor point. The assist shader uses the GameObject position itself as
        // that pivot, so express all child/root geometry relative to the same
        // anchor before the GPU applies the root transform.
        const glm::vec2 rootAnchor = ccPointToGLM(object->getAnchorPointInPoints());
        glm::vec2 posBottomLeft = ccPointToGLM(localBottomLeftPoint) - rootAnchor;
        glm::vec2 posRight = {
            localTransform.a * crop.size.width,
            localTransform.b * crop.size.width
        };
        glm::vec2 posUp = {
            localTransform.c * crop.size.height,
            localTransform.d * crop.size.height
        };

        // Do not reconstruct flip/rotated-frame UV behavior ourselves. Cocos has
        // already resolved it in m_sQuad. Reusing those exact UV corners avoids
        // mirrored/rotated sprite mismatches while GD keeps frame ownership.
        const auto stockQuad = sprite->getQuad();
        const glm::vec2 uvBL = quadUV(stockQuad.bl);
        const glm::vec2 uvBR = quadUV(stockQuad.br);
        const glm::vec2 uvTL = quadUV(stockQuad.tl);
        const glm::vec2 uvTR = quadUV(stockQuad.tr);

        if (!activeRange || activeTexture != entry.textureId) {
            drawRanges.push_back({
                entry.textureId,
                (u32)indices.size(),
                0
            });
            activeRange = &drawRanges.back();
            activeTexture = entry.textureId;
        }

        const u16 baseVertex = (u16)vertices.size();
        const float objectIndex = (float)entry.candidate.objectStateIndex;
        const float spriteIndex = (float)entry.candidate.spriteStateIndex;

        vertices.push_back({ posBottomLeft, uvBL, objectIndex, spriteIndex });
        vertices.push_back({ posBottomLeft + posRight, uvBR, objectIndex, spriteIndex });
        vertices.push_back({ posBottomLeft + posUp, uvTL, objectIndex, spriteIndex });
        vertices.push_back({
            posBottomLeft + posRight + posUp,
            uvTR,
            objectIndex,
            spriteIndex
        });

        indices.push_back(baseVertex + 0);
        indices.push_back(baseVertex + 2);
        indices.push_back(baseVertex + 3);
        indices.push_back(baseVertex + 0);
        indices.push_back(baseVertex + 3);
        indices.push_back(baseVertex + 1);
        activeRange->indexCount += 6;
        ownedSprites.push_back(sprite);
        ++stats.batchedSprites;
    }

    if (vertices.empty() || indices.empty() || drawRanges.empty() || ownedSprites.empty())
        return false;

    vertexBuffer = Buffer::createStaticDraw(
        "Resolved safe GPU vertices",
        vertices.data(),
        vertices.size() * sizeof(Vertex)
    );
    indexBuffer = Buffer::createStaticDraw(
        "Resolved safe GPU indices",
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

    stats.textureBatches = drawRanges.size();
    stats.verticesResident = vertices.size();

    log::info(
        "Bismuth iOS mixed GPU batch: {} owned / {} eligible sprites, {} rejected, {} draw range(s)",
        stats.batchedSprites,
        stats.eligibleSprites,
        stats.rejectedSprites,
        stats.textureBatches
    );
    return true;
}

void AssistShadowBatch::draw() {
    stats.drawCallsLastFrame = 0;
    stats.indicesLastFrame = 0;

    if (!stats.ready || !isVisible())
        return;
    if (!resolvedState || !resolvedState->isGPUStateReady() || !shader || !vao)
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