#pragma once

#include <Geode/binding/GameObject.hpp>
#include <common.hpp>
#include <Geode/Geode.hpp>
#include <vector>

#include "Buffer.hpp"
#include "glm/fwd.hpp"
#include "math/ConvexList.hpp"
#include "ObjectUtils.hpp"
#include "WorkGroupScheduler.hpp"
#include "../utils/GPUTimer.hpp"

using namespace geode;

class Renderer;

struct SpriteVertexTransforms {
    glm::vec2 positionBottomLeft;
    glm::vec2 positionRight;
    glm::vec2 positionUp;
    glm::vec2 texCoordBottomLeft;
    glm::vec2 texCoordRight;
    glm::vec2 texCoordUp;
};

class ObjectBatch {
public:
    inline ObjectBatch(Renderer& renderer)
        : renderer(renderer) {}

    SpriteVertexTransforms getSpriteVertexTransform(
        cocos2d::CCSprite* sprite,
        const cocos2d::CCAffineTransform& transform,
        SpriteSheet spriteSheet
    );

    /*
        Returns false if this sprite cannot be rendered by the batch (for
        example, when its sprite sheet is not loaded). Callers must not write
        any vertices in that case.
    */
    bool prepareSpriteMeshWrite(
        GameObject* parentObject,
        cocos2d::CCSprite* sprite,
        SpriteType type,
        const cocos2d::CCAffineTransform& transform
    );

    void writeSpriteVertex(glm::vec2 pos);
    // void writeSpriteIndex(u32 index);

    void writeSpriteMeshFromConvexList(const ConvexList& list);

    void addSprite(
        GameObject* parentObject,
        cocos2d::CCSprite* sprite,
        SpriteType type,
        const cocos2d::CCAffineTransform& transform
    );

    void addSpriteWithCustomVanillaMesh(cocos2d::CCSprite* sprite);

    void writeGameObject(GameObject* object);

    void finishWriting();

    void draw(const LayerKey& layer);

    inline bool shouldDraw(const LayerKey& layer) const {
        if (!sortedBatch) return false;
        auto it = sortedBatch->workGroupRangePerLayerKey.find(layer);
        return it != sortedBatch->workGroupRangePerLayerKey.end() &&
               it->second.count > 0;
    }

    inline usize getVertexBufferSize() { return vertexCount * sizeof(ObjectVertex); }
    inline void setSpriteSheetFilter(SpriteSheet sheet) { spriteSheetFilter = sheet; }
    inline const std::set<LayerKey>& getUsedLayerIds() { return usedLayerKeys; }
    inline usize getSpriteCount() { return workGroupScheduler.getSpriteCount(); }

    void predraw();

    void prepareCullingComputeShader();

private:
    Renderer& renderer;

    SpriteSheet spriteSheetFilter = (SpriteSheet)-1;

    // Static buffers
    UPtr<Buffer> vertexBuffer = nullptr;
    UPtr<Buffer> indexBuffer = nullptr;
    UPtr<Buffer> inputSpriteBuffer = nullptr;

    /*
        Start position and group combination per sprite. They don't depend on
        the blend states, so every sorted batch shares this one buffer instead
        of uploading its own copy.
    */
    UPtr<PreCulledSprite::Buffer> preCulledSpritesBuffer = nullptr;

    // GPU buffers
    UPtr<RawSprite::Buffer> rawSpriteBuffer = nullptr;
    UPtr<Buffer> culledIndexBuffer = nullptr;

    GPUTimer cullingComputeShaderTimer { "Culling compute shader time" };

    /*
        A blend state combination that only lasts for a frame or two is not
        worth sorting the whole level for, and looking it up on every frame is
        wasted work while nothing changes.
    */
    static constexpr u32 REGENERATE_AFTER_STABLE_FRAMES = 3;

    WorkGroupScheduler workGroupScheduler;
    SortedObjectBatch* sortedBatch = nullptr;

    /*
        The drawing program and the storage buffer bindings are global GL
        state, so they only have to be set up once per batch instead of on
        every layer draw.
    */
    bool drawStateIsBound = false;
    const SortedObjectBatch* drawStateBoundBatch = nullptr;

    ColorChannelBlendStates requestedBlendStates {};
    ColorChannelBlendStates lastSeenBlendStates {};
    u32 stableBlendStateFrames = 0;
    bool haveBatchForRequestedStates = false;
    bool blendStatesSeen = false;

    usize spriteCount = 0;

    std::vector<ObjectVertex> verticies;
    u32 vertexCount = 0;

    // std::vector<u32> indicies;
    // u32 indexCount = 0;

    std::vector<InputSprite> inputSprites;
    // std::vector<PreCulledSprite> preCulledSprites;

    std::set<LayerKey> usedLayerKeys;

    SpriteVertexTransforms currentSpriteVertexTransforms;
    glm::vec2 currentSpriteObjectStartPosition;
    u32 currentSpriteVertexIndex;
    u32 currentSpriteObjectIndex;
    u32 currentSpriteIndex;
};