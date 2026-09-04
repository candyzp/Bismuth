#ifdef GEODE_IS_IOS

#include "AssistShadowBatch.hpp"

#include "Geode/cocos/CCDirector.h"
#include "Geode/cocos/cocoa/CCAffineTransform.h"
#include "Geode/cocos/kazmath/include/kazmath/mat4.h"

#include <algorithm>
#include <cstddef>
#include <unordered_map>

using namespace geode::prelude;

namespace {
constexpr usize MAX_BATCH_SPRITES = 16383;

std::unordered_map<cocos2d::CCSprite*, AssistShadowBatch*> g_assistOwnerBySprite;

static bool isDescendantOf(cocos2d::CCNode* node, cocos2d::CCNode* ancestor) {
    for (auto current = node; current; current = current->getParent()) {
        if (current == ancestor)
            return true;
    }
    return false;
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

static glm::vec2 quadUV(const cocos2d::ccV3F_C4B_T2F& vertex) {
    return { vertex.texCoords.u, vertex.texCoords.v };
}
} // namespace

AssistShadowBatch::~AssistShadowBatch() {
    destroyGL();
}

AssistShadowBatch* AssistShadowBatch::ownerForSprite(cocos2d::CCSprite* sprite) {
    if (!sprite)
        return nullptr;
    auto it = g_assistOwnerBySprite.find(sprite);
    return it == g_assistOwnerBySprite.end() ? nullptr : it->second;
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

    if (!buildGeometry())
        return false;

    registerOwnedSprites();
    setVisible(true);
    stats.ready = true;
    stats.visibleOwnership = true;
    return true;
}

void AssistShadowBatch::registerOwnedSprites() {
    for (auto sprite : ownedSprites) {
        if (!sprite)
            continue;
        auto [it, inserted] = g_assistOwnerBySprite.emplace(sprite, this);
        if (!inserted && it->second != this) {
            log::warn("Bismuth iOS atlas owner collision for sprite {}; using newest safe owner", (void*)sprite);
            it->second = this;
        }
    }
}

void AssistShadowBatch::unregisterOwnedSprites() {
    for (auto sprite : ownedSprites) {
        auto it = g_assistOwnerBySprite.find(sprite);
        if (it != g_assistOwnerBySprite.end() && it->second == this)
            g_assistOwnerBySprite.erase(it);
    }
}

void AssistShadowBatch::destroyGL() {
    unregisterOwnedSprites();

    if (vao)
        glDeleteVertexArrays(1, &vao);
    vao = 0;

    if (interleaveIndexBuffer)
        glDeleteBuffers(1, &interleaveIndexBuffer);
    interleaveIndexBuffer = 0;
    interleaveIndexCapacity = 0;

    if (vertexBuffer)
        Buffer::destroy(vertexBuffer);
    vertexBuffer = nullptr;

    vertexBaseBySprite.clear();
    interleaveIndices.clear();
    ownedSprites.clear();
    stats.ready = false;
    stats.visibleOwnership = false;
}

bool AssistShadowBatch::buildGeometry() {
    const auto sourceCandidates = resolvedState->getGPUCandidates();

    std::vector<CandidateWithTexture> candidates;
    candidates.reserve(std::min<usize>(sourceCandidates.size(), MAX_BATCH_SPRITES));
    ownedSprites.clear();
    vertexBaseBySprite.clear();

    usize ordinal = 0;
    for (const auto& candidate : sourceCandidates) {
        if (!candidate.object || !candidate.sprite)
            continue;
        if (!isDescendantOf(candidate.sprite, stockBatch))
            continue;

        ++stats.eligibleSprites;

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

    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.atlasIndex != b.atlasIndex)
            return a.atlasIndex < b.atlasIndex;
        return a.originalOrder < b.originalOrder;
    });

    std::vector<Vertex> vertices;
    vertices.reserve(candidates.size() * 4);
    ownedSprites.reserve(candidates.size());
    vertexBaseBySprite.reserve(candidates.size());

    for (const auto& entry : candidates) {
        auto object = entry.candidate.object;
        auto sprite = entry.candidate.sprite;
        auto texture = sprite ? sprite->getTexture() : nullptr;
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

        const glm::vec2 rootAnchor = ccPointToGLM(object->getAnchorPointInPoints());
        const glm::vec2 posBottomLeft = ccPointToGLM(localBottomLeftPoint) - rootAnchor;
        const glm::vec2 posRight = {
            localTransform.a * crop.size.width,
            localTransform.b * crop.size.width
        };
        const glm::vec2 posUp = {
            localTransform.c * crop.size.height,
            localTransform.d * crop.size.height
        };

        const auto stockQuad = sprite->getQuad();
        const glm::vec2 uvBL = quadUV(stockQuad.bl);
        const glm::vec2 uvBR = quadUV(stockQuad.br);
        const glm::vec2 uvTL = quadUV(stockQuad.tl);
        const glm::vec2 uvTR = quadUV(stockQuad.tr);

        if (vertices.size() + 4 > 65535) {
            ++stats.rejectedSprites;
            continue;
        }

        const u16 baseVertex = static_cast<u16>(vertices.size());
        const float objectIndex = static_cast<float>(entry.candidate.objectStateIndex);
        const float spriteIndex = static_cast<float>(entry.candidate.spriteStateIndex);

        vertices.push_back({ posBottomLeft, uvBL, objectIndex, spriteIndex });
        vertices.push_back({ posBottomLeft + posRight, uvBR, objectIndex, spriteIndex });
        vertices.push_back({ posBottomLeft + posUp, uvTL, objectIndex, spriteIndex });
        vertices.push_back({
            posBottomLeft + posRight + posUp,
            uvTR,
            objectIndex,
            spriteIndex
        });

        vertexBaseBySprite.emplace(sprite, baseVertex);
        ownedSprites.push_back(sprite);
        ++stats.batchedSprites;
    }

    if (vertices.empty() || ownedSprites.empty() || vertexBaseBySprite.size() != ownedSprites.size())
        return false;

    vertexBuffer = Buffer::createStaticDraw(
        "Resolved safe GPU vertices",
        vertices.data(),
        vertices.size() * sizeof(Vertex)
    );
    if (!vertexBuffer) {
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
    glGenBuffers(1, &interleaveIndexBuffer);
    if (!vao || !interleaveIndexBuffer) {
        destroyGL();
        return false;
    }

    interleaveIndexCapacity = ownedSprites.size() * 6;
    interleaveIndices.reserve(interleaveIndexCapacity);

    glBindVertexArray(vao);
    vertexBuffer->bindAs(GL_ARRAY_BUFFER);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, interleaveIndexBuffer);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(interleaveIndexCapacity * sizeof(u16)),
        nullptr,
        GL_DYNAMIC_DRAW
    );

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, localPosition));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, texCoord));
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, objectStateIndex));
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, spriteStateIndex));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);

    glBindVertexArray(static_cast<u32>(previousVAO));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<u32>(previousVBO));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<u32>(previousIBO));

    stats.textureBatches = 1;
    stats.verticesResident = vertices.size();

    log::info(
        "Bismuth iOS atlas-interleave buffer: {} owned / {} eligible sprites, {} rejected, {} resident vertices",
        stats.batchedSprites,
        stats.eligibleSprites,
        stats.rejectedSprites,
        stats.verticesResident
    );
    return true;
}

void AssistShadowBatch::draw() {
    // Deliberately empty. Drawing here would place the GPU-owned subset after
    // all stock quads and recreate the exact ordering regression this class now
    // exists to avoid. The CCTextureAtlas hook calls drawOrderedSprites().
}

void AssistShadowBatch::beginAtlasFrame() {
    stats.drawCallsLastFrame = 0;
    stats.indicesLastFrame = 0;
}

bool AssistShadowBatch::drawOrderedSprites(
    const std::vector<cocos2d::CCSprite*>& orderedSprites
) {
    if (!stats.ready || !isVisible() || !resolvedState || !shader || !vao || !interleaveIndexBuffer)
        return false;
    if (!resolvedState->isGPUStateReady() || orderedSprites.empty())
        return false;

    auto firstSprite = orderedSprites.front();
    auto actualBatch = firstSprite ? firstSprite->getBatchNode() : nullptr;
    if (!actualBatch)
        return false;

    auto texture = actualBatch->getTexture();
    if (!texture || !texture->getName())
        return false;

    interleaveIndices.clear();
    if (orderedSprites.size() * 6 > interleaveIndexCapacity)
        return false;

    for (auto sprite : orderedSprites) {
        if (!sprite || sprite->getBatchNode() != actualBatch)
            return false;

        auto it = vertexBaseBySprite.find(sprite);
        if (it == vertexBaseBySprite.end())
            return false;

        const u16 baseVertex = it->second;
        interleaveIndices.push_back(baseVertex + 0);
        interleaveIndices.push_back(baseVertex + 2);
        interleaveIndices.push_back(baseVertex + 3);
        interleaveIndices.push_back(baseVertex + 0);
        interleaveIndices.push_back(baseVertex + 3);
        interleaveIndices.push_back(baseVertex + 1);
    }

    const auto blend = actualBatch->getBlendFunc();
    return drawDynamicIndices(
        interleaveIndices,
        texture->getName(),
        static_cast<u32>(blend.src),
        static_cast<u32>(blend.dst)
    );
}

bool AssistShadowBatch::drawDynamicIndices(
    const std::vector<u16>& indices,
    u32 textureId,
    u32 blendSrc,
    u32 blendDst
) {
    if (indices.empty() || !textureId || indices.size() > interleaveIndexCapacity)
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
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, interleaveIndexBuffer);
    glBufferSubData(
        GL_ELEMENT_ARRAY_BUFFER,
        0,
        static_cast<GLsizeiptr>(indices.size() * sizeof(u16)),
        indices.data()
    );

    glEnable(GL_BLEND);
    glBlendFunc(static_cast<GLenum>(blendSrc), static_cast<GLenum>(blendDst));
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    glStencilMask(0);

    shader->setTexture("u_spriteSheetTexture", 0, textureId);
    glDrawElements(
        GL_TRIANGLES,
        static_cast<GLsizei>(indices.size()),
        GL_UNSIGNED_SHORT,
        nullptr
    );
    ++stats.drawCallsLastFrame;
    stats.indicesLastFrame += indices.size();

    glColorMask(
        previousColorMask[0],
        previousColorMask[1],
        previousColorMask[2],
        previousColorMask[3]
    );
    glDepthMask(previousDepthMask);
    glStencilMask(static_cast<u32>(previousStencilMask));
    glBlendFuncSeparate(
        static_cast<GLenum>(previousBlendSrcRGB),
        static_cast<GLenum>(previousBlendDstRGB),
        static_cast<GLenum>(previousBlendSrcAlpha),
        static_cast<GLenum>(previousBlendDstAlpha)
    );
    if (!previousBlendEnabled)
        glDisable(GL_BLEND);

    glBindVertexArray(static_cast<u32>(previousVAO));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<u32>(previousVBO));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<u32>(previousIBO));
    glUseProgram(static_cast<u32>(previousProgram));

    for (i32 unit = 0; unit < 3; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, static_cast<u32>(previousTextures[unit]));
    }
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    return true;
}

#endif
