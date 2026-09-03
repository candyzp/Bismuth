#ifdef GEODE_IS_IOS

#include "AssistShadowBatch.hpp"

#include "Geode/cocos/CCDirector.h"
#include "Geode/cocos/kazmath/include/kazmath/mat4.h"

#include <algorithm>
#include <cstddef>

using namespace geode::prelude;

namespace {
constexpr usize MAX_SHADOW_SPRITES = 8192;

struct CandidateWithTexture {
    ResolvedStateLayer::ShadowCandidate candidate;
    u32 textureId = 0;
};
} // namespace

AssistShadowBatch::~AssistShadowBatch() {
    destroyGL();
}

geode::Ref<AssistShadowBatch> AssistShadowBatch::create(
    ResolvedStateLayer* state,
    Shader* assistShader
) {
    auto node = new AssistShadowBatch();
    if (node->initWithState(state, assistShader)) {
        node->autorelease();
        return node;
    }
    delete node;
    return nullptr;
}

bool AssistShadowBatch::initWithState(ResolvedStateLayer* state, Shader* assistShader) {
    if (!CCNode::init() || !state || !assistShader || !state->isGPUStateReady())
        return false;

    resolvedState = state;
    shader = assistShader;

    GLint vertexTextureUnits = 0;
    glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &vertexTextureUnits);
    if (vertexTextureUnits < 2) {
        log::warn(
            "Bismuth iOS shadow batch needs 2 vertex texture units; device reports {}",
            vertexTextureUnits
        );
        return false;
    }

    if (!buildGeometry())
        return false;

    setVisible(true);
    stats.ready = true;
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
    stats.ready = false;
}

bool AssistShadowBatch::buildGeometry() {
    const auto& sourceCandidates = resolvedState->getShadowCandidates();
    stats.eligibleSprites = sourceCandidates.size();

    std::vector<CandidateWithTexture> candidates;
    candidates.reserve(std::min<usize>(sourceCandidates.size(), MAX_SHADOW_SPRITES));

    for (const auto& candidate : sourceCandidates) {
        if (candidates.size() >= MAX_SHADOW_SPRITES)
            break;
        if (!candidate.object || !candidate.sprite)
            continue;
        auto texture = candidate.sprite->getTexture();
        if (!texture || texture->getName() == 0)
            continue;
        candidates.push_back({ candidate, texture->getName() });
    }

    if (candidates.empty()) {
        log::info("Bismuth iOS shadow batch: no single-sprite static solid candidates");
        return false;
    }

    // Group only by the already-resolved backing texture. Draw ordering is not
    // part of this validation pass because all framebuffer writes are disabled.
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.textureId < b.textureId;
    });

    std::vector<Vertex> vertices;
    std::vector<u16> indices;
    vertices.reserve(candidates.size() * 4);
    indices.reserve(candidates.size() * 6);
    drawRanges.clear();

    const float contentScaleFactor = CCDirector::get()->getContentScaleFactor();
    u32 activeTexture = 0;
    DrawRange* activeRange = nullptr;

    for (const auto& entry : candidates) {
        auto sprite = entry.candidate.sprite;
        auto texture = sprite->getTexture();
        if (!texture)
            continue;

        const auto crop = sprite->getTextureRect();

        glm::vec2 posBottomLeft = ccPointToGLM(sprite->getOffsetPosition());
        glm::vec2 posRight = { crop.size.width, 0.f };
        glm::vec2 posUp = { 0.f, crop.size.height };

        glm::vec2 texBottomLeft;
        glm::vec2 texRight;
        glm::vec2 texUp;
        if (!sprite->isTextureRectRotated()) {
            texBottomLeft = { crop.origin.x, crop.origin.y + crop.size.height };
            texRight = { crop.size.width, 0.f };
            texUp = { 0.f, -crop.size.height };
        } else {
            texBottomLeft = { crop.origin.x, crop.origin.y };
            texRight = { 0.f, crop.size.width };
            texUp = { crop.size.height, 0.f };
        }

        // Match Cocos' quad construction. Flips reverse local geometry while UVs
        // stay attached to their original corners.
        if (sprite->isFlipX()) {
            posBottomLeft += posRight;
            posRight = -posRight;
        }
        if (sprite->isFlipY()) {
            posBottomLeft += posUp;
            posUp = -posUp;
        }

        const glm::vec2 texFactor = {
            contentScaleFactor / std::max(1.f, (float)texture->getPixelsWide()),
            contentScaleFactor / std::max(1.f, (float)texture->getPixelsHigh())
        };
        texBottomLeft *= texFactor;
        texRight *= texFactor;
        texUp *= texFactor;

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

        vertices.push_back({
            posBottomLeft,
            texBottomLeft,
            objectIndex,
            spriteIndex
        });
        vertices.push_back({
            posBottomLeft + posRight,
            texBottomLeft + texRight,
            objectIndex,
            spriteIndex
        });
        vertices.push_back({
            posBottomLeft + posUp,
            texBottomLeft + texUp,
            objectIndex,
            spriteIndex
        });
        vertices.push_back({
            posBottomLeft + posRight + posUp,
            texBottomLeft + texRight + texUp,
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
        ++stats.batchedSprites;
    }

    if (vertices.empty() || indices.empty() || drawRanges.empty())
        return false;

    vertexBuffer = Buffer::createStaticDraw(
        "Resolved assist shadow vertices",
        vertices.data(),
        vertices.size() * sizeof(Vertex)
    );
    indexBuffer = Buffer::createStaticDraw(
        "Resolved assist shadow indices",
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
        "Bismuth iOS shadow batch: {} static sprites, {} texture batch(es), {} vertices",
        stats.batchedSprites,
        stats.textureBatches,
        stats.verticesResident
    );
    return true;
}

void AssistShadowBatch::draw() {
    stats.drawCallsLastFrame = 0;
    stats.indicesLastFrame = 0;

    // This is deliberately tied to the diagnostic toggle. Normal gameplay still
    // pays zero shadow-draw cost until the first visible ownership handoff is
    // validated and can replace equivalent stock CPU rendering work.
    if (!stats.ready || !Mod::get()->getSettingValue<bool>("ios_gpu_debug"))
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
    GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean previousDepthMask = GL_TRUE;
    GLint previousStencilMask = 0;

    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVAO);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousVBO);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &previousIBO);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
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

    // The GPU genuinely executes vertex expansion/transforms and fragment
    // texture sampling, but this validation stage cannot alter a single visible
    // pixel or depth/stencil value. Stock GD is still the sole visual owner.
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
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
