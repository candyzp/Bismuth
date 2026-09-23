#pragma once
#ifdef GEODE_IS_IOS

#include "ResolvedStateLayer.hpp"
#include <algorithm>
#include <array>
#include <cstring>

struct AssistVertex {
    glm::vec2 localPosition;
    glm::vec2 texCoord;
    float objectStateIndex = 0.f;
    float spriteStateIndex = 0.f;
};

// Persistent geometry, refreshed only for sprites in the live draw. Geometry
// changes do not require rebuilding the level or dropping back to stock Cocos.
class LiveGeometry {
    struct Record {
        ResolvedStateLayer::ShadowCandidate candidate;
        cocos2d::CCRect rect;
        cocos2d::CCPoint offset;
        cocos2d::CCAffineTransform transform;
        std::array<glm::vec2, 4> uv;
        bool initialized = false;
    };
    std::vector<Record> records;
    std::vector<AssistVertex> vertices;
    std::vector<usize> dirty;

public:
    bool canUseBatch(usize index, cocos2d::CCNode* batch) const {
        if (index >= records.size() || !batch)
            return false;
        const auto& candidate = records[index].candidate;
        if (!candidate.object || !candidate.sprite)
            return false;

        // A nested sprite's immediate parent is often another sprite, not the
        // CCSpriteBatchNode. getBatchNode() is the authoritative render home for
        // atlas descendants and keeps complex decoration children GPU-owned.
        if (candidate.sprite->getBatchNode())
            return candidate.sprite->getBatchNode() == batch;

        return candidate.object->getParent() == batch ||
            candidate.sprite->getParent() == batch;
    }
    void clear() { records.clear(); vertices.clear(); dirty.clear(); }
    void add(const ResolvedStateLayer::ShadowCandidate& candidate, const AssistVertex* quad) {
        Record record{};
        record.candidate = candidate;
        records.push_back(record);
        vertices.insert(vertices.end(), quad, quad + 4);
    }
    bool refresh(usize index) {
        if (index >= records.size())
            return false;
        auto& record = records[index];
        auto sprite = record.candidate.sprite;
        auto object = record.candidate.object;
        if (!sprite || !object || !sprite->getTexture())
            return false;
        auto transform = cocos2d::CCAffineTransformMakeIdentity();
        // Once GD has inserted the sprite into a stock atlas, Cocos'
        // m_transformToBatch is authoritative. Keep the VBO in sprite-local
        // space and let the shader consume that exact matrix.
        if (!sprite->getBatchNode() && sprite != object) {
            auto node = static_cast<cocos2d::CCNode*>(sprite);
            while (node && node != object) {
                transform = cocos2d::CCAffineTransformConcat(transform, node->nodeToParentTransform());
                node = node->getParent();
            }
            if (node != object) {
                transform = cocos2d::CCAffineTransformConcat(
                    sprite->nodeToWorldTransform(), object->worldToNodeTransform());
            }
        }
        const auto rect = sprite->getTextureRect();
        const auto offset = sprite->getOffsetPosition();
        const auto& quad = sprite->getQuad();
        const std::array<glm::vec2, 4> uv{{
            {quad.bl.texCoords.u, quad.bl.texCoords.v}, {quad.br.texCoords.u, quad.br.texCoords.v},
            {quad.tl.texCoords.u, quad.tl.texCoords.v}, {quad.tr.texCoords.u, quad.tr.texCoords.v}}};
        const auto& old = record.transform;
        const bool same = record.initialized && rect.size.width == record.rect.size.width &&
            rect.size.height == record.rect.size.height && offset.x == record.offset.x &&
            offset.y == record.offset.y && transform.a == old.a && transform.b == old.b &&
            transform.c == old.c && transform.d == old.d && transform.tx == old.tx && transform.ty == old.ty &&
            std::equal(uv.begin(), uv.end(), record.uv.begin(), [](const auto& a, const auto& b) {
                return a.x == b.x && a.y == b.y;
            });
        if (same)
            return true;
        const auto point = cocos2d::CCPointApplyAffineTransform(offset, transform);
        const glm::vec2 bl{point.x, point.y};
        const glm::vec2 right{transform.a * rect.size.width, transform.b * rect.size.width};
        const glm::vec2 up{transform.c * rect.size.height, transform.d * rect.size.height};
        const std::array<glm::vec2, 4> positions{bl, bl + right, bl + up, bl + right + up};
        for (usize corner = 0; corner < 4; ++corner) {
            vertices[index * 4 + corner].localPosition = positions[corner];
            vertices[index * 4 + corner].texCoord = uv[corner];
        }
        record.rect = rect; record.offset = offset; record.transform = transform;
        record.uv = uv; record.initialized = true;
        dirty.push_back(index);
        return true;
    }
    bool flush(u32 buffer) {
        if (dirty.empty())
            return true;
        if (!buffer)
            return false;
        std::sort(dirty.begin(), dirty.end());
        GLint previous = 0;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous);

        // Do not let a stale error raised by Geometry Dash or another mod poison
        // this upload. Only GL errors generated by this Bismuth transaction
        // should make the owned GPU draw fail.
        while (glGetError() != GL_NO_ERROR) {}

        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        usize first = dirty.front(), last = first;
        const auto upload = [&]() {
            glBufferSubData(GL_ARRAY_BUFFER, first * 4 * sizeof(AssistVertex),
                (last - first + 1) * 4 * sizeof(AssistVertex), vertices.data() + first * 4);
        };
        for (usize i = 1; i < dirty.size(); ++i) {
            if (dirty[i] <= last + 1) { last = std::max(last, dirty[i]); continue; }
            upload(); first = last = dirty[i];
        }
        upload();

        bool uploadOK = true;
        while (glGetError() != GL_NO_ERROR)
            uploadOK = false;

        glBindBuffer(GL_ARRAY_BUFFER, static_cast<u32>(previous));
        if (!uploadOK)
            return false;
        dirty.clear();
        return true;
    }
};
#endif
