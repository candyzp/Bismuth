#pragma once

#ifdef GEODE_IS_IOS

#include "ResolvedStateLayer.hpp"
#include "../Buffer.hpp"
#include "../Shader.hpp"
#include <Geode/Geode.hpp>
#include <vector>

// Replaces a consecutive run of standalone safe CCSprites that share the same
// parent/z/blend slot. The original sprites remain alive so Geometry Dash keeps
// resolving gameplay, triggers, colors, visibility and other state; their stock
// render visits are skipped while this node submits the persistent GPU geometry.
class StandaloneAssistBatch : public cocos2d::CCNode {
public:
    struct Stats {
        usize sprites = 0;
        usize textureRanges = 0;
        usize drawCallsLastFrame = 0;
        usize indicesLastFrame = 0;
        usize verticesResident = 0;
        bool ready = false;
    };

    ~StandaloneAssistBatch() override;

    static geode::Ref<StandaloneAssistBatch> create(
        ResolvedStateLayer* resolvedState,
        Shader* shader,
        const std::vector<ResolvedStateLayer::ShadowCandidate>& candidates
    );

    void draw() override;

    inline const Stats& getStats() const { return stats; }
    inline const std::vector<cocos2d::CCSprite*>& getOwnedSprites() const { return ownedSprites; }

private:
    struct Vertex {
        glm::vec2 localPosition;
        glm::vec2 texCoord;
        float objectStateIndex = 0.f;
        float spriteStateIndex = 0.f;
    };

    struct DrawRange {
        u32 textureId = 0;
        u32 startIndex = 0;
        u32 indexCount = 0;
    };

    bool initWithCandidates(
        ResolvedStateLayer* resolvedState,
        Shader* shader,
        const std::vector<ResolvedStateLayer::ShadowCandidate>& candidates
    );
    bool buildGeometry(const std::vector<ResolvedStateLayer::ShadowCandidate>& candidates);
    void destroyGL();

private:
    ResolvedStateLayer* resolvedState = nullptr;
    Shader* shader = nullptr;

    Buffer* vertexBuffer = nullptr;
    Buffer* indexBuffer = nullptr;
    u32 vao = 0;

    u32 blendSrc = GL_SRC_ALPHA;
    u32 blendDst = GL_ONE_MINUS_SRC_ALPHA;

    std::vector<DrawRange> drawRanges;
    std::vector<cocos2d::CCSprite*> ownedSprites;
    Stats stats;
};

#endif
