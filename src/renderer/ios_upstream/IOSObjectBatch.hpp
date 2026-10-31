#pragma once

#ifdef GEODE_IS_IOS

#include "IOSGroupManager.hpp"
#include "IOSRenderRecords.hpp"
#include "IOSWorkGroupScheduler.hpp"
#include <functional>
#include <unordered_map>

class ConvexList;

namespace IOSUpstream {

class ObjectBatch {
public:
    using TextureResolver = std::function<cocos2d::CCTexture2D*(SpriteSheet)>;

    bool build(
        const std::vector<GameObject*>& objects,
        GroupManager& groupManager,
        const ColorChannelBlendStates& blendStates,
        const TextureResolver& resolveTexture
    );

    void refreshSpriteAppearances();

    const std::vector<ObjectInfo>& getObjectInfos() const { return objectInfos; }
    const std::vector<ObjectVertex>& getVertices() const { return vertices; }
    const std::vector<InputSprite>& getInputSprites() const { return inputSprites; }
    const std::vector<SpriteAppearance>& getSpriteAppearances() const { return spriteAppearances; }
    const std::vector<PreCulledSprite>& getPreCulledSprites() const { return sortedBatch.preCulledSprites; }
    const SortedObjectBatch& getSortedBatch() const { return sortedBatch; }
    const std::set<LayerKey>& getUsedLayerKeys() const { return usedLayerKeys; }
    usize getSpriteCount() const { return inputSprites.size(); }

    u32 getObjectIndex(GameObject* object) const;
    bool isBuilt() const { return built; }

private:
    struct SpriteSource {
        GameObject* object = nullptr;
        cocos2d::CCSprite* sprite = nullptr;
        SpriteType type = SpriteType::BASE;
    };

    struct SpriteVertexTransforms {
        glm::vec2 positionBottomLeft {};
        glm::vec2 positionRight {};
        glm::vec2 positionUp {};
        glm::vec2 texCoordBottomLeft {};
        glm::vec2 texCoordRight {};
        glm::vec2 texCoordUp {};
    };

    bool buildObjectInfo(const std::vector<GameObject*>& objects);
    bool writeGameObject(GameObject* object, GroupManager& groupManager);
    bool addSprite(
        GameObject* object,
        cocos2d::CCSprite* sprite,
        SpriteType type,
        const cocos2d::CCAffineTransform& transform,
        GroupManager& groupManager
    );
    SpriteVertexTransforms getSpriteVertexTransform(
        cocos2d::CCSprite* sprite,
        const cocos2d::CCAffineTransform& transform,
        SpriteSheet spriteSheet
    ) const;
    void writeSpriteVertex(
        glm::vec2 point,
        const SpriteVertexTransforms& transforms,
        glm::vec2 objectStartPosition,
        u32 spriteIndex
    );
    bool finalizeSortedGeometry(const ColorChannelBlendStates& blendStates);
    void clear();

private:
    TextureResolver resolveTexture;
    WorkGroupScheduler workGroupScheduler;

    std::vector<ObjectInfo> objectInfos;
    std::unordered_map<GameObject*, u32> objectIndices;

    // Initial object order, before the owner-style layer/sheet/blend sort.
    std::vector<ObjectVertex> sourceVertices;
    std::vector<InputSprite> sourceInputSprites;
    std::vector<SpriteAppearance> sourceSpriteAppearances;
    std::vector<SpriteSource> sourceSpritePointers;

    // Final owner-style draw order. Vertex spriteIndex values index the
    // correspondingly ordered InputSprite / SpriteAppearance arrays.
    std::vector<ObjectVertex> vertices;
    std::vector<InputSprite> inputSprites;
    std::vector<SpriteAppearance> spriteAppearances;
    std::vector<SpriteSource> spritePointers;

    std::vector<LayerKey> usedLayerKeyStorage;
    std::set<LayerKey> usedLayerKeys;
    SortedObjectBatch sortedBatch;

    SpriteVertexTransforms currentTransforms;
    glm::vec2 currentObjectStartPosition {};
    u32 currentObjectIndex = 0;
    u32 currentSpriteIndex = 0;

    bool built = false;
};

} // namespace IOSUpstream

#endif
