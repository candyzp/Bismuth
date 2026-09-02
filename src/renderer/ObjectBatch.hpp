#pragma once

#include <Geode/binding/GameObject.hpp>
#include <common.hpp>
#include <Geode/Geode.hpp>
#include <array>
#include <vector>

#include "Buffer.hpp"
#include "VisibilityManager.hpp"
#include "glm/fwd.hpp"
#include "math/ConvexList.hpp"
#include "ObjectUtils.hpp"

using namespace geode;

/*
    These are the vertex attributes of the object batch.
    iOS/ES2 has no integer vertex attributes, so the lookup indices are stored
    as floats there. They are exact for the ranges Geometry Dash uses.
*/
#ifdef GEODE_IS_IOS
#define OBJECT_VERTEX_ATTRIBUTES(ATTRIB) \
    ATTRIB(0, vec2,  positionOffset) \
    ATTRIB(1, vec2,  texCoord) \
    ATTRIB(2, float, srbIndex) \
    ATTRIB(3, float, colorChannel) \

#else
#define OBJECT_VERTEX_ATTRIBUTES(ATTRIB) \
    ATTRIB(0, vec2, positionOffset) \
    ATTRIB(1, vec2, texCoord) \
    ATTRIB(2, u32,  srbIndex) \
    ATTRIB(3, u16,  colorChannel) \

#endif

#define VERTEX_ATTRIBUTE_AS_STRUCT_MEMBER(ID, TYPE, NAME) \
    TYPE NAME;

#define VERTICIES_PER_QUAD 4
#define INDICIES_PER_QUAD  6

struct ObjectVertex {
    OBJECT_VERTEX_ATTRIBUTES(VERTEX_ATTRIBUTE_AS_STRUCT_MEMBER);
};

struct ObjectQuad {
    union {
        ObjectVertex verticies[VERTICIES_PER_QUAD];
        struct {
            ObjectVertex bl;
            ObjectVertex br;
            ObjectVertex tl;
            ObjectVertex tr;
        };
    };
};

struct ObjectIndicies {
    u32 indicies[INDICIES_PER_QUAD];
};

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
    struct LayerDrawCall {
        LayerKey id;
        VisibilityManager::Layer layer;
        u32 startIndex;
        u32 indexCount;
    };

public:
    inline ObjectBatch(Renderer& renderer)
        : renderer(renderer) {}
    ~ObjectBatch();

    SpriteVertexTransforms getSpriteVertexTransform(
        cocos2d::CCSprite* sprite,
        const cocos2d::CCAffineTransform& transform,
        SpriteSheet spriteSheet
    );

    void prepareSpriteMeshWrite(
        GameObject* parentObject,
        cocos2d::CCSprite* sprite,
        SpriteType type,
        const cocos2d::CCAffineTransform& transform
    );

    void writeSpriteVertex(glm::vec2 pos);
    void writeSpriteIndex(u32 index);

    void writeSpriteMeshFromConvexList(const ConvexList& list);

    void addSprite(
        GameObject* parentObject,
        cocos2d::CCSprite* sprite,
        SpriteType type,
        const cocos2d::CCAffineTransform& transform
    );

    void writeGameObject(GameObject* object);

    void finishWriting();

    inline void bind() {
        glBindVertexArray(vao);
        indexBuffer->bindAs(GL_ELEMENT_ARRAY_BUFFER);
    }

    inline usize getVertexBufferSize() {
        return vertexCount * sizeof(ObjectVertex);
    }

    inline void setSpriteSheetFilter(SpriteSheet sheet) {
        spriteSheetFilter = sheet;
    }

    inline std::vector<LayerKey> getUsedLayerIds() {
        return visibilityManager.getUsedLayerIds();
    }

    void predraw(const CameraView& view);

    LayerDrawCall* getDrawCall(const LayerKey& id);

    void draw(LayerDrawCall* drawCall);

private:
    struct LiveSpriteRecord {
        GameObject* object = nullptr;
        cocos2d::CCSprite* sprite = nullptr;
        SpriteType type = SpriteType::BASE;
        u32 vertexBegin = 0;
        std::array<ObjectVertex, VERTICIES_PER_QUAD> vertices {};
    };

    void prepareVAO();
    bool shouldTrackLiveSpriteObject(GameObject* object) const;
    void refreshLiveSpriteData();

private:
    Renderer& renderer;
    VisibilityManager visibilityManager;
    SpriteSheet spriteSheetFilter = (SpriteSheet)-1;

    Buffer* vertexBuffer = nullptr;
    Buffer* indexBuffer  = nullptr;

    u32 vao = 0;

    std::vector<ObjectVertex> verticies;
    u32 vertexCount = 0;

    std::vector<u32> indicies;
    u32 indexCount = 0;

    std::vector<u32> culledIndicies;
    u32 culledIndexCount = 0;

    std::vector<LayerDrawCall> layerDrawCalls;
    std::vector<LiveSpriteRecord> liveSprites;

    SpriteVertexTransforms currentSpriteVertexTransforms;
    glm::vec2 currentSpriteObjectStartPosition;
    u32 currentSpriteVertexIndex;
    u32 currentSpriteSRBIndex;
    u16 currentSpriteColorChannel;
};
