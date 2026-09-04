#ifdef GEODE_IS_IOS

#include "AssistShadowBatch.hpp"
#include "AtlasInterleave.hpp"

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
    AtlasInterleaveRegistry::registerImmediate(this);
    return true;
}

void AssistShadowBatch::destroyGL() {
    AtlasInterleaveRegistry::unregisterImmediate(this);

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

        glm::vec2 posBottomLeft = ccPointToGLM(localBottomLeftPoint);
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

void AssistShadowBatch::draw() {}

#endif
