#pragma once

#ifdef GEODE_IS_IOS

#include "ResolvedStateLayer.hpp"
#include "../Buffer.hpp"
#include "../Shader.hpp"
#include <Geode/Geode.hpp>
#include <vector>

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
    };

    ~AssistShadowBatch() override;

    static geode::Ref<AssistShadowBatch> create(
        ResolvedStateLayer* resolvedState,
        Shader* shader
    );

    void draw() override;

    inline const Stats& getStats() const { return stats; }

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

    bool initWithState(ResolvedStateLayer* resolvedState, Shader* shader);
    bool buildGeometry();
    void destroyGL();

private:
    ResolvedStateLayer* resolvedState = nullptr;
    Shader* shader = nullptr;

    Buffer* vertexBuffer = nullptr;
    Buffer* indexBuffer = nullptr;
    u32 vao = 0;

    std::vector<DrawRange> drawRanges;
    Stats stats;
};

#endif
