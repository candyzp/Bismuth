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
    if (node == object)
        return true;

    // Forced complex decorations can keep glow/detail sprites in separate GD
    // visual homes. Derive sprite-local -> object-local from live world matrices
    // instead of rejecting those external sprite arrangements.
    out = cocos2d::CCAffineTransformConcat(
        sprite->nodeToWorldTransform(),
        object->worldToNodeTransform()
    );
    return true;
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
    cocos2d::CCSpriteBatchNode* sourceBatch,
    usize ownershipLimit
) {
    auto node = new AssistShadowBatch();
    if (node->initWithState(state, assistShader, sourceBatch, ownershipLimit)) {
        node->autorelease();
        return node;
    }
    delete node;
    return nullptr;
}

bool AssistShadowBatch::initWithState(
    ResolvedStateLayer* state,
    Shader* assistShader,
    cocos2d::CCSpriteBatchNode* sourceBatch,
    usize ownershipLimit
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

    if (!buildGeometry(ownershipLimit))
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
    liveGeometry.clear();
    stats.ready = false;
    stats.visibleOwnership = false;
}

bool AssistShadowBatch::buildGeometry(usize ownershipLimit) {
    const auto sourceCandidates = resolvedState->getGPUCandidates();

    std::vector<CandidateWithTexture> candidates;
    candidates.reserve(std::min<usize>(sourceCandidates.size(), MAX_BATCH_SPRITES));
    ownedSprites.clear();
    liveGeometry.clear();

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

    if (candidates.empty() || ownershipLimit == 0)
        return false;

    // Preserve Cocos atlas order among the GPU-owned subset. Animated/complex
    // sprites remain in the stock batch and are never inserted here.
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.atlasIndex != b.atlasIndex)
            return a.atlasIndex < b.atlasIndex;
        return a.originalOrder < b.originalOrder;
    });

    // A giant atlas must not consume the complete persistent budget and then
    // leave the rest of the level GPU-idle. When a batch is larger than its
    // ownership quota, keep atlas-contiguous windows spread across the whole
    // batch. Each window remains cheap to draw, while later level sections still
    // retain GPU-owned geometry.
    const usize limit = std::min<usize>({
        ownershipLimit,
        MAX_BATCH_SPRITES,
        candidates.size()
    });
    if (candidates.size() > limit) {
        // Prefer broad GPU islands on dense decoration atlases. Tiny ~64-sprite
        // windows were excellent for coverage but terrible for actual offload:
        // a complex section often intersected only 20-70 owned sprites. Keep
        // ownership distributed across the level, but make each window roughly
        // 384 sprites so visible dense sections can feed the GPU hundreds of
        // sprites in a small number of draw calls.
        constexpr usize TARGET_WINDOW_SPRITES = 384;
        constexpr usize MAX_OWNERSHIP_WINDOWS = 32;
        const usize windowCount = std::max<usize>(
            1,
            std::min<usize>(
                MAX_OWNERSHIP_WINDOWS,
                std::max<usize>(1, limit / TARGET_WINDOW_SPRITES)
            )
        );
        std::vector<CandidateWithTexture> distributed;
        distributed.reserve(limit);

        for (usize window = 0; window < windowCount && distributed.size() < limit; ++window) {
            const usize segmentBegin = window * candidates.size() / windowCount;
            const usize segmentEnd = (window + 1) * candidates.size() / windowCount;
            if (segmentEnd <= segmentBegin)
                continue;

            const usize remaining = limit - distributed.size();
            const usize windowsLeft = windowCount - window;
            const usize target = (remaining + windowsLeft - 1) / windowsLeft;
            const usize segmentSize = segmentEnd - segmentBegin;
            const usize take = std::min(target, segmentSize);
            const usize begin = segmentBegin + (segmentSize - take) / 2;
            distributed.insert(
                distributed.end(),
                candidates.begin() + begin,
                candidates.begin() + begin + take
            );
        }

        stats.rejectedSprites += candidates.size() - distributed.size();
        candidates = std::move(distributed);
    }

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
        liveGeometry.add(entry.candidate, vertices.data() + baseVertex);
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
