#pragma once

#ifdef GEODE_IS_IOS

#include "IOSGroupManager.hpp"
#include "IOSRenderRecords.hpp"
#include "IOSWorkGroupScheduler.hpp"
#include <functional>
#include <set>
#include <unordered_map>

class ConvexList;

namespace IOSUpstream {

class ObjectBatch {
public:
    struct VertexRange {
        u32 firstVertex = 0;
        u32 vertexCount = 0;
    };

    ~ObjectBatch();
    using TextureResolver = std::function<cocos2d::CCTexture2D*(SpriteSheet)>;

    bool build(
        const std::vector<GameObject*>& objects,
        GroupManager& groupManager,
        const ColorChannelBlendStates& blendStates,
        const TextureResolver& resolveTexture
    );

    void refreshSpriteAppearances();
    bool refreshBlendOrder(const ColorChannelBlendStates& blendStates);

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
    u64 getGeometryRevision() const { return geometryRevision; }
    u64 getLastAppearanceSpritesScanned() const { return lastAppearanceSpritesScanned; }
    u64 getLastAppearanceSpritesChanged() const { return lastAppearanceSpritesChanged; }
    const std::vector<VertexRange>& getDirtyVertexRanges() const { return dirtyVertexRanges; }
    void clearDirtyVertexRanges() { dirtyVertexRanges.clear(); }

private:
    struct SpriteSource {
        GameObject* object = nullptr;
        cocos2d::CCSprite* sprite = nullptr;
        SpriteType type = SpriteType::BASE;
        cocos2d::CCAffineTransform transform {};
        // Some gameplay animations rotate/move child nodes rather than
        // changing the GameObject root. Refresh these sources from live Cocos
        // node transforms instead of leaving the initial transform frozen.
        bool refreshTransform = false;
        cocos2d::CCRect lastTextureRect {};
        cocos2d::CCTexture2D* lastTexture = nullptr;
        bool lastTextureRectRotated = false;
        bool lastFlipX = false;
        bool lastFlipY = false;
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
    // Keep the normalized mesh points so an animated atlas frame can rebuild
    // only that sprite's vertices without traversing the entire level.
    std::vector<glm::vec2> sourceVertexPoints;
    std::vector<InputSprite> sourceInputSprites;
    std::vector<SpriteAppearance> sourceSpriteAppearances;
    std::vector<SpriteSource> sourceSpritePointers;

    // Only geometry is duplicated into scheduler draw order. Its per-vertex
    // sprite/object/group indices continue to address the source-order metadata
    // tables above, avoiding copies of the InputSprite/appearance/pointer arrays.
    std::vector<ObjectVertex> orderedVertices;
    // Source sprite index -> first vertex in the scheduler-ordered VBO stream.
    std::vector<u32> orderedVertexStarts;
    std::vector<VertexRange> dirtyVertexRanges;

    std::set<LayerKey> usedLayerKeys;
    SortedObjectBatch sortedBatch;
    ColorChannelBlendStates appliedBlendStates{};
    bool hasAppliedBlendStates = false;

    SpriteVertexTransforms currentTransforms;
    glm::vec2 currentObjectStartPosition {};
    u32 currentObjectIndex = 0;
    u32 currentGroupCombinationIndex = 0;
    u32 currentSpriteType = 0;
    u32 currentColorChannelFlags = 0;
    u32 currentSpriteIndex = 0;

    u64 orderRevision = 0;
    u64 geometryRevision = 0;
    u64 lastAppearanceSpritesScanned = 0;
    u64 lastAppearanceSpritesChanged = 0;
    bool built = false;
};

} // namespace IOSUpstream

#endif
