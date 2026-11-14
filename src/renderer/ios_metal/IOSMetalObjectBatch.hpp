#pragma once

#if defined(GEODE_IS_IOS)

#include "IOSMetalCompute.hpp"
#include <ObjectUtils.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/cocos/textures/CCTexture2D.h>
#include <functional>
#include <glm/fwd.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace BismuthIOSMetal {

struct Vertex {
    float positionOffset[2] {};
    float texCoord[2] {};
    float spriteIndex = 0.f;
};
static_assert(sizeof(Vertex) == sizeof(float) * 5);

struct LayerRange {
    LayerKey key {};
    std::uint32_t firstVertex = 0;
    std::uint32_t vertexCount = 0;
};

using GroupIndexResolver = std::function<std::uint32_t(GameObject*)>;
using TextureResolver = std::function<cocos2d::CCTexture2D*(SpriteSheet)>;
using BlendStates = std::array<bool, 1101>;

class ObjectBatch {
public:
    ObjectBatch() = default;
    ~ObjectBatch();

    ObjectBatch(const ObjectBatch&) = delete;
    ObjectBatch& operator=(const ObjectBatch&) = delete;

    // All per-sprite records, mesh triangles, and object metadata are built
    // once for a level. This is the only function that walks level objects.
    bool build(
        GJBaseGameLayer* layer,
        const std::vector<GameObject*>& objects,
        const GroupIndexResolver& groupIndexForObject,
        const BlendStates& blendStates,
        const TextureResolver& textureForSheet
    );

    // Called only when a color channel changes its blending mode. It does not
    // run every frame and it never recomputes sprite transforms or visibility.
    bool refreshBlendOrder(const BlendStates& blendStates);

    void clear();

    std::uint32_t getVertexBuffer() const { return vertexBuffer; }
    std::uint32_t getVertexCount() const { return static_cast<std::uint32_t>(orderedVertices.size()); }
    std::uint32_t getSpriteCount() const { return static_cast<std::uint32_t>(workItems.size()); }

    const std::vector<SpriteWorkItem>& getWorkItems() const { return workItems; }
    const std::vector<InputSprite>& getInputSprites() const { return inputSprites; }
    const std::vector<ObjectInfo>& getObjectInfos() const { return objectInfos; }
    const std::vector<LayerRange>& getLayerRanges() const { return layerRanges; }
    const std::string& getLastBuildError() const { return lastBuildError; }

private:
    struct SpriteTransforms {
        glm::vec2 positionBottomLeft {};
        glm::vec2 positionRight {};
        glm::vec2 positionUp {};
        glm::vec2 texCoordBottomLeft {};
        glm::vec2 texCoordRight {};
        glm::vec2 texCoordUp {};
    };

    struct SortEntry {
        LayerKey key {};
        std::int32_t zOrder = 0;
        std::uint32_t sourceSpriteIndex = 0;
        std::uint32_t colorChannel = 0;
    };

    bool buildObjectInfos(const std::vector<GameObject*>& objects);
    bool addSprite(
        GameObject* object,
        cocos2d::CCSprite* sprite,
        SpriteType type,
        const cocos2d::CCAffineTransform& transform,
        const GroupIndexResolver& groupIndexForObject
    );
    SpriteTransforms getSpriteVertexTransform(
        cocos2d::CCSprite* sprite,
        const cocos2d::CCAffineTransform& transform,
        cocos2d::CCTexture2D* texture
    ) const;
    void writeSpriteVertex(
        const glm::vec2& point,
        const SpriteTransforms& transforms,
        const glm::vec2& objectStartPosition,
        std::uint32_t spriteIndex
    );
    bool writeGameObject(
        GameObject* object,
        const GroupIndexResolver& groupIndexForObject
    );
    bool buildOrderedVertices(const BlendStates& blendStates);
    bool uploadOrderedVertices();

private:
    GJBaseGameLayer* layer = nullptr;
    TextureResolver textureForSheet;

    std::unordered_map<GameObject*, std::uint32_t> objectIndices;
    std::vector<ObjectInfo> objectInfos;
    std::vector<SpriteWorkItem> workItems;
    std::vector<InputSprite> inputSprites;

    std::vector<Vertex> sourceVertices;
    std::vector<Vertex> orderedVertices;
    std::vector<SortEntry> sortEntries;
    std::vector<LayerRange> layerRanges;

    std::uint32_t vertexBuffer = 0;
    BlendStates appliedBlendStates {};
    bool hasBlendStates = false;
    bool built = false;
    std::string lastBuildError;
};

} // namespace BismuthIOSMetal

#endif
