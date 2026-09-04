#pragma once

#ifdef GEODE_IS_IOS

#include "ResolvedStateLayer.hpp"
#include "../Buffer.hpp"
#include "../Shader.hpp"
#include <Geode/Geode.hpp>
#include <unordered_map>
#include <vector>

// Persistent GameObject-local geometry for safe non-atlas-at-init sprites.
// In root-addressable mode, each GameObject gets an independent draw span for
// exact stock root-visit replacement. In coalesced mode, object boundaries do
// not force GL ranges, allowing parentless-at-init objects that later enter the
// same stock atlas to render in a small number of shared draws.
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
        const std::vector<ResolvedStateLayer::ShadowCandidate>& candidates,
        bool rootAddressable = true
    );

    void draw() override;
    void beginFrame();
    bool ownsRoot(GameObject* root) const;
    bool drawRoot(GameObject* root);

    inline const Stats& getStats() const { return stats; }
    inline const std::vector<cocos2d::CCSprite*>& getOwnedSprites() const { return ownedSprites; }
    inline bool isRootAddressable() const { return rootAddressable; }

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
        const std::vector<ResolvedStateLayer::ShadowCandidate>& candidates,
        bool rootAddressable
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

    bool rootAddressable = true;
    std::vector<DrawRange> drawRanges;
    std::unordered_map<GameObject*, RootDrawSpan> rootDrawSpans;
    std::vector<cocos2d::CCSprite*> ownedSprites;
    Stats stats;
};

#endif