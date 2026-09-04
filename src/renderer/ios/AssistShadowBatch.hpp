#pragma once

#ifdef GEODE_IS_IOS

#include "ResolvedStateLayer.hpp"
#include "../Buffer.hpp"
#include "../Shader.hpp"
#include <Geode/Geode.hpp>
#include <Geode/cocos/sprite_nodes/CCSpriteBatchNode.h>
#include <unordered_map>
#include <vector>

// Persistent GPU geometry for safe sprites that were already inside a stock
// CCSpriteBatchNode at renderer initialization. The node itself no longer draws
// as a sibling. CCTextureAtlas::drawQuads is interleaved instead so stock-only
// and GPU-owned sprites keep the exact live atlas order chosen by Cocos.
class AssistShadowBatch : public cocos2d::CCNode {
public:
    struct Stats {
        usize eligibleSprites = 0;
        usize batchedSprites = 0;
        usize rejectedSprites = 0;
        usize textureBatches = 0;
        usize drawCallsLastFrame = 0;
        usize indicesLastFrame = 0;
        usize verticesResident = 0;
        bool ready = false;
        bool visibleOwnership = false;
    };

    ~AssistShadowBatch() override;

    static geode::Ref<AssistShadowBatch> create(
        ResolvedStateLayer* resolvedState,
        Shader* shader,
        cocos2d::CCSpriteBatchNode* stockBatch
    );

    // Intentional no-op. Visible submission happens inside the stock atlas draw
    // hook, not as a later sibling draw.
    void draw() override;

    // Called once per atlas draw before this owner emits any GPU run.
    void beginAtlasFrame();

    // Draw a contiguous run from the stock atlas's CURRENT order. Persistent
    // vertices stay untouched; only a tiny dynamic index list is uploaded.
    bool drawOrderedSprites(const std::vector<cocos2d::CCSprite*>& sprites);

    // Atlas hook lookup. Registration exists only while this buffer is alive.
    static AssistShadowBatch* ownerForSprite(cocos2d::CCSprite* sprite);

    inline const Stats& getStats() const { return stats; }
    inline cocos2d::CCSpriteBatchNode* getStockBatch() const { return stockBatch; }
    inline const std::vector<cocos2d::CCSprite*>& getOwnedSprites() const { return ownedSprites; }

private:
    struct Vertex {
        glm::vec2 localPosition;
        glm::vec2 texCoord;
        float objectStateIndex = 0.f;
        float spriteStateIndex = 0.f;
    };

    struct CandidateWithTexture {
        ResolvedStateLayer::ShadowCandidate candidate;
        u32 textureId = 0;
        u32 atlasIndex = 0;
        usize originalOrder = 0;
    };

    bool initWithState(
        ResolvedStateLayer* resolvedState,
        Shader* shader,
        cocos2d::CCSpriteBatchNode* stockBatch
    );
    bool buildGeometry();
    bool drawDynamicIndices(
        const std::vector<u16>& indices,
        u32 textureId,
        u32 blendSrc,
        u32 blendDst
    );
    void registerOwnedSprites();
    void unregisterOwnedSprites();
    void destroyGL();

private:
    ResolvedStateLayer* resolvedState = nullptr;
    Shader* shader = nullptr;
    cocos2d::CCSpriteBatchNode* stockBatch = nullptr;

    Buffer* vertexBuffer = nullptr;
    u32 vao = 0;
    u32 interleaveIndexBuffer = 0;
    usize interleaveIndexCapacity = 0;

    std::unordered_map<cocos2d::CCSprite*, u16> vertexBaseBySprite;
    std::vector<u16> interleaveIndices;
    std::vector<cocos2d::CCSprite*> ownedSprites;
    Stats stats;
};

#endif
