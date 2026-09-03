#pragma once

#ifdef GEODE_IS_IOS

#include "ResolvedStateLayer.hpp"
#include "../Buffer.hpp"
#include "../Shader.hpp"
#include <Geode/Geode.hpp>
#include <Geode/cocos/sprite_nodes/CCSpriteBatchNode.h>
#include <vector>

// Despite the historical name, this node is now the conservative visible
// ownership unit on iOS. One instance replaces exactly one stock
// CCSpriteBatchNode, and only when RendererIOS proves that every direct object
// in that stock batch belongs to the StaticSafe set. Animated/interactive
// batches never enter this path.
class AssistShadowBatch : public cocos2d::CCNode {
public:
    struct Stats {
        usize eligibleSprites = 0;
        usize batchedSprites = 0;
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

    void draw() override;

    inline const Stats& getStats() const { return stats; }
    inline cocos2d::CCSpriteBatchNode* getStockBatch() const { return stockBatch; }

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

    bool initWithState(
        ResolvedStateLayer* resolvedState,
        Shader* shader,
        cocos2d::CCSpriteBatchNode* stockBatch
    );
    bool buildGeometry();
    void destroyGL();

private:
    ResolvedStateLayer* resolvedState = nullptr;
    Shader* shader = nullptr;
    cocos2d::CCSpriteBatchNode* stockBatch = nullptr;

    Buffer* vertexBuffer = nullptr;
    Buffer* indexBuffer = nullptr;
    u32 vao = 0;

    u32 blendSrc = GL_SRC_ALPHA;
    u32 blendDst = GL_ONE_MINUS_SRC_ALPHA;

    std::vector<DrawRange> drawRanges;
    Stats stats;
};

#endif
