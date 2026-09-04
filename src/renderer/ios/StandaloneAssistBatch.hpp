#pragma once

#ifdef GEODE_IS_IOS

#include "ResolvedStateLayer.hpp"
#include "../Buffer.hpp"
#include "../Shader.hpp"
#include <Geode/Geode.hpp>
#include <unordered_map>
#include <vector>

// Persistent standalone geometry shared by many complete safe GameObject roots.
// Geometry Dash still owns gameplay, visibility, child lifecycle, colors and
// animation. The stock GameObject root visit selects exactly one root slice at
// the moment Cocos would have rendered it, preserving stock scene order.
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

    // Direct root-visit path. The caller is already inside the stock Cocos parent
    // traversal, so drawing only this root's range preserves exact z/order without
    // requiring the GameObject to have a parent during level initialization.
    void beginFrame();
    bool drawRoot(GameObject* root);
    bool ownsRoot(GameObject* root) const;

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
        u32 blendSrc = GL_SRC_ALPHA;
        u32 blendDst = GL_ONE_MINUS_SRC_ALPHA;
        u32 startIndex = 0;
        u32 indexCount = 0;
    };

    struct RootDrawSpan {
        usize firstRange = 0;
        usize rangeCount = 0;
    };

    bool initWithCandidates(
        ResolvedStateLayer* resolvedState,
        Shader* shader,
        const std::vector<ResolvedStateLayer::ShadowCandidate>& candidates
    );
    bool buildGeometry(const std::vector<ResolvedStateLayer::ShadowCandidate>& candidates);
    bool drawRangeSpan(usize firstRange, usize rangeCount);
    void destroyGL();

private:
    ResolvedStateLayer* resolvedState = nullptr;
    Shader* shader = nullptr;

    Buffer* vertexBuffer = nullptr;
    Buffer* indexBuffer = nullptr;
    u32 vao = 0;

    std::vector<DrawRange> drawRanges;
    std::unordered_map<GameObject*, RootDrawSpan> rootDrawSpans;
    std::vector<cocos2d::CCSprite*> ownedSprites;
    Stats stats;
};

#endif
