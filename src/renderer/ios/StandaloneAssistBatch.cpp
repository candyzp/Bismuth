#ifdef GEODE_IS_IOS

#include "StandaloneAssistBatch.hpp"

#include "Geode/cocos/cocoa/CCAffineTransform.h"
#include "Geode/cocos/kazmath/include/kazmath/mat4.h"
#include "Geode/cocos/sprite_nodes/CCSpriteBatchNode.h"
#include <algorithm>
#include <cstddef>
#include <unordered_map>

using namespace geode::prelude;

namespace {
constexpr usize MAX_BATCH_SPRITES = 16383;

std::unordered_map<cocos2d::CCSprite*, StandaloneAssistBatch*> g_deferredOwnerBySprite;

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

StandaloneAssistBatch* StandaloneAssistBatch::deferredOwnerForSprite(cocos2d::CCSprite* sprite) {
    if (!sprite)
        return nullptr;
    auto it = g_deferredOwnerBySprite.find(sprite);
    return it == g_deferredOwnerBySprite.end() ? nullptr : it->second;
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

    // Parentless-only sets are deferred-atlas geometry. GD later attaches those
    // roots to a stock CCSpriteBatchNode. We keep their vertices resident now,
    // but actual submission waits for the live atlas order hook.
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

    if (!rootAddressable)
        registerDeferredSprites();

    setVisible(true);
    stats.ready = true;
    return true;
}

void StandaloneAssistBatch::registerDeferredSprites() {
    if (rootAddressable)
        return;

    for (auto sprite : ownedSprites) {
        if (!sprite)
            continue;
        auto [it, inserted] = g_deferredOwnerBySprite.emplace(sprite, this);
        if (!inserted && it->second != this) {
            log::warn("Bismuth iOS deferred atlas owner collision for sprite {}; using newest safe owner", (void*)sprite);
            it->second = this;
        }
    }
}

void StandaloneAssistBatch::unregisterDeferredSprites() {
    if (rootAddressable)
        return;

    for (auto sprite : ownedSprites) {
        auto it = g_deferredOwnerBySprite.find(sprite);
        if (it != g_deferredOwnerBySprite.end() && it->second == this)
            g_deferredOwnerBySprite.erase(it);
    }
}

void StandaloneAssistBatch::destroyGL() {
    unregisterDeferredSprites();

    if (vao)
        glDeleteVertexArrays(1, &vao);
    vao = 0;

    if (interleaveIndexBuffer)
        glDeleteBuffers(1, &interleaveIndexBuffer);
    interleaveIndexBuffer = 0;
    interleaveIndexCapacity = 0;

    if (vertexBuffer)
        Buffer::destroy(vertexBuffer);
    if (indexBuffer)
        Buffer::destroy(indexBuffer);
    vertexBuffer = nullptr;
    indexBuffer = nullptr;

    vertexBaseBySprite.clear();
    interleaveIndices.clear();
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
    vertexBaseBySprite.clear();
    drawRanges.clear();
    rootDrawSpans.clear();
    vertexBaseBySprite.reserve(candidates.size());

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

        const glm::vec2 rootAnchor = ccPointToGLM(object->getAnchorPointInPoints());
        const glm::vec2 bottomLeft = ccPointToGLM(localBottomLeftPoint) - rootAnchor;
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
        const u32 blendSrc = static_cast<u32>(blend.src);
        const u32 blendDst = static_cast<u32>(blend.dst);

        if (!activeRange ||
            activeTexture != textureId ||
            activeBlendSrc != blendSrc ||
            activeBlendDst != blendDst) {
            drawRanges.push_back({
                textureId,
                blendSrc,
                blendDst,
                static_cast<u32>(indices.size()),
                0
            });
            activeRange = &drawRanges.back();
            activeTexture = textureId;
            activeBlendSrc = blendSrc;
            activeBlendDst = blendDst;
        }

        if (vertices.size() + 4 > 65535)
            return false;

        const u16 baseVertex = static_cast<u16>(vertices.size());
        const float objectIndex = static_cast<float>(candidate.objectStateIndex);
        const float spriteIndex = static_cast<float>(candidate.spriteStateIndex);

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

        vertexBaseBySprite.emplace(sprite, baseVertex);
        ownedSprites.push_back(sprite);
    }

    finishRoot();

    if (ownedSprites.size() != candidates.size() || vertices.empty() || indices.empty() || drawRanges.empty())
        return false;
    if (vertexBaseBySprite.size() != ownedSprites.size())
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

    if (!rootAddressable) {
        glGenBuffers(1, &interleaveIndexBuffer);
        if (!interleaveIndexBuffer) {
            glBindVertexArray(static_cast<u32>(previousVAO));
            glBindBuffer(GL_ARRAY_BUFFER, static_cast<u32>(previousVBO));
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<u32>(previousIBO));
            destroyGL();
            return false;
        }

        interleaveIndexCapacity = ownedSprites.size() * 6;
        interleaveIndices.reserve(interleaveIndexCapacity);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, interleaveIndexBuffer);
        glBufferData(
            GL_ELEMENT_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(interleaveIndexCapacity * sizeof(u16)),
            nullptr,
            GL_DYNAMIC_DRAW
        );
    }

    glBindVertexArray(static_cast<u32>(previousVAO));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<u32>(previousVBO));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<u32>(previousIBO));

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
    // Deferred-atlas buffers are submitted from CCTextureAtlas::drawQuads so
    // they land at the exact live atlas index. Drawing them here would put the
    // whole deferred subset after stock sprites and recreate the ordering bug.
    if (!rootAddressable)
        return;

    beginFrame();
    drawRangeSpan(0, drawRanges.size());
}

bool StandaloneAssistBatch::drawOrderedSprites(
    const std::vector<cocos2d::CCSprite*>& orderedSprites
) {
    if (rootAddressable || !stats.ready || !isVisible() || !resolvedState || !shader || !vao || !interleaveIndexBuffer)
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

    if (orderedSprites.size() * 6 > interleaveIndexCapacity)
        return false;

    interleaveIndices.clear();
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

bool StandaloneAssistBatch::drawDynamicIndices(
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
    indexBuffer->bindAs(GL_ELEMENT_ARRAY_BUFFER);
    glEnable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    glStencilMask(0);

    for (usize rangeIndex = firstRange; rangeIndex < endRange; ++rangeIndex) {
        const auto& range = drawRanges[rangeIndex];
        if (!range.textureId || !range.indexCount)
            continue;

        glBlendFunc(static_cast<GLenum>(range.blendSrc), static_cast<GLenum>(range.blendDst));
        shader->setTexture("u_spriteSheetTexture", 0, range.textureId);
        glDrawElements(
            GL_TRIANGLES,
            static_cast<GLsizei>(range.indexCount),
            GL_UNSIGNED_SHORT,
            (void*)(static_cast<usize>(range.startIndex) * sizeof(u16))
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
