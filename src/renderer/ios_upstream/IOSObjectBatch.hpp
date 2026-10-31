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
    ~ObjectBatch();
    using TextureResolver = std::function<cocos2d::CCTexture2D*(SpriteSheet)>;

    bool build(
        const std::vector<GameObject*>& objects,
        GroupManager& groupManager,
        const ColorChannelBlendStates& blendStates,
        const TextureResolver& resolveTexture
    );

    void refreshSpriteAppearances();
    bool updateBlendStates(const ColorChannelBlendStates& blendStates);

    const std::vector<ObjectInfo>& getObjectInfos() const { return objectInfos; }
    const std::vector<ObjectVertex>& getVertices() const { return sourceVertices; }
    // ES2 draw stream arranged by the owner's scheduler remap. The source
    // vertex array and InputSprite indices remain untouched.
    const std::vector<ObjectVertex>& getOrderedVertices() const { return orderedVertices; }
    const std::vector<InputSprite>& getInputSprites() const { return sourceInputSprites; }
    const std::vector<SpriteAppearance>& getSpriteAppearances() const { return sourceSpriteAppearances; }
    const std::vector<PreCulledSprite>& getPreCulledSprites() const { return sortedBatch.preCulledSprites; }
    const SortedObjectBatch& getSortedBatch() const { return sortedBatch; }
    const std::set<LayerKey>& getUsedLayerKeys() const { return usedLayerKeys; }
    usize getSpriteCount() const { return sourceInputSprites.size(); }

    u32 getObjectIndex(GameObject* object) const;
    bool isBuilt() const { return built; }
    u64 getOrderRevision() const { return orderRevision; }

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
        cocos2d::CCTexture2D* texture
    ) const;
    void writeSpriteVertex(
        glm::vec2 point,
        const SpriteVertexTransforms& transforms,
        glm::vec2 objectStartPosition,
        u32 spriteIndex
    );
    bool finalizeSortedGeometry(const ColorChannelBlendStates& blendStates);
    bool applySortedBatch(const SortedObjectBatch& sortedBatch);
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

    // Only geometry is duplicated into scheduler draw order. Its per-vertex
    // sprite/object/group indices continue to address the source-order metadata
    // tables above, avoiding copies of the InputSprite/appearance/pointer arrays.
    std::vector<ObjectVertex> orderedVertices;

    std::set<LayerKey> usedLayerKeys;
    SortedObjectBatch sortedBatch;

    SpriteVertexTransforms currentTransforms;
    glm::vec2 currentObjectStartPosition {};
    u32 currentObjectIndex = 0;
    u32 currentGroupCombinationIndex = 0;
    u32 currentSpriteType = 0;
    u32 currentColorChannelFlags = 0;
    u32 currentSpriteIndex = 0;

    ColorChannelBlendStates currentBlendStates {};
    bool hasCurrentBlendStates = false;
    u64 orderRevision = 0;
    bool built = false;
};

} // namespace IOSUpstream

#endif
