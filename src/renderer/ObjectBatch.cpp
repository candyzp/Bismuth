#include "ObjectBatch.hpp"
#include "BProfiler.hpp"
#include "Renderer.hpp"
#include "SpriteMeshDictionary.hpp"
#include "common.hpp"
#include "glm/fwd.hpp"
#include "math/ConvexList.hpp"
#include "math/ConvexPolygon.hpp"
#include <culling/SpriteDrawMap.hpp>
#include <string>

using namespace geode::prelude;

#define QUAD_BL 0
#define QUAD_BR 1
#define QUAD_TL 2
#define QUAD_TR 3

ObjectBatch::~ObjectBatch() {
    if (vertexBuffer)
        Buffer::destroy(vertexBuffer);
    if (indexBuffer)
        Buffer::destroy(indexBuffer);
    if (vao)
        glDeleteVertexArrays(1, &vao);
}

SpriteVertexTransforms ObjectBatch::getSpriteVertexTransform(
    cocos2d::CCSprite* sprite,
    const cocos2d::CCAffineTransform& transform,
    SpriteSheet spriteSheet
) {
    CCRect crop = sprite->getTextureRect();

    // Normalize UVs against the exact texture backing this live sprite. This
    // avoids assuming that a separately-loaded atlas has the same HD/UHD
    // resolution/content scale as Geometry Dash's active batch texture.
    CCTexture2D* texture = sprite->getTexture();
    if (texture == nullptr)
        texture = renderer.getSpriteSheetTexture(spriteSheet);
    if (texture == nullptr) return {};

    glm::vec2 posBottomLeft  = ccPointToGLM(CCPointApplyAffineTransform(sprite->getOffsetPosition(), transform));
    glm::vec2 posRightVector = glm::vec2(transform.a, transform.b) * crop.size.width;
    glm::vec2 posUpVector    = glm::vec2(transform.c, transform.d) * crop.size.height;

    glm::vec2 texBottomLeft;
    glm::vec2 texRightVector;
    glm::vec2 texUpVector;

    if (!sprite->isTextureRectRotated()) {
        texBottomLeft  = { crop.origin.x, crop.origin.y + crop.size.height };
        texRightVector = { crop.size.width, 0 };
        texUpVector    = { 0, -crop.size.height };
    } else {
        texBottomLeft  = { crop.origin.x, crop.origin.y };
        texRightVector = { 0, crop.size.width };
        texUpVector    = { crop.size.height, 0 };
    }
    
    if (sprite->isFlipX()) {
        posBottomLeft += posRightVector;
        posRightVector = -posRightVector;
    }
    if (sprite->isFlipY()) {
        posBottomLeft += posUpVector;
        posUpVector = -posUpVector;
    }

    float contentScaleFactor = CCDirector::get()->getContentScaleFactor();
    glm::vec2 texCoordFactor = glm::vec2(
        contentScaleFactor / texture->getPixelsWide(),
        contentScaleFactor / texture->getPixelsHigh()
    );

    texBottomLeft  *= texCoordFactor;
    texRightVector *= texCoordFactor;
    texUpVector    *= texCoordFactor;

    return {
        posBottomLeft, posRightVector, posUpVector,
        texBottomLeft, texRightVector, texUpVector
    };
}

void ObjectBatch::prepareSpriteMeshWrite(
    GameObject* object,
    cocos2d::CCSprite* sprite,
    SpriteType type,
    const cocos2d::CCAffineTransform& transform
) {
    SpriteSheet spriteSheet = ObjectUtils::getSpritesheetOfObject(object, type);
    if (spriteSheetFilter != (SpriteSheet)-1 && spriteSheet != spriteSheetFilter)
        return;

    u32 colorChannel = ObjectUtils::getSpriteColorChannel(type, object, sprite);
    if (type == SpriteType::DETAIL)
        colorChannel |= A_COLOR_CHANNEL_IS_SPRITE_DETAIL;

    currentSpriteVertexTransforms    = getSpriteVertexTransform(sprite, transform, spriteSheet);
    currentSpriteObjectStartPosition = ccPointToGLM(object->m_startPosition);

    currentSpriteSRBIndex     = renderer.getObjectSRBIndex(object);
    currentSpriteColorChannel = colorChannel;
    currentSpriteVertexIndex  = verticies.size();
}

void ObjectBatch::writeSpriteVertex(glm::vec2 pos) {
    isize index = verticies.size();
    verticies.resize(index + 1);
    ObjectVertex& vertex = verticies[index];

    auto& transforms = currentSpriteVertexTransforms;

    vertex.positionOffset = transforms.positionRight * pos.x +
                            transforms.positionUp    * pos.y +
                            transforms.positionBottomLeft - currentSpriteObjectStartPosition;
    
    vertex.texCoord = transforms.texCoordRight * pos.x +
                      transforms.texCoordUp    * pos.y +
                      transforms.texCoordBottomLeft;

    vertex.srbIndex     = currentSpriteSRBIndex;
    vertex.colorChannel = currentSpriteColorChannel;
}

void ObjectBatch::writeSpriteIndex(u32 index) {
    indicies.push_back(currentSpriteVertexIndex + index);
}

void ObjectBatch::writeSpriteMeshFromConvexList(const ConvexList& list) {
    u32 vertexIndex = 0;
    list.triangulate([&](const glm::vec2& p1, const glm::vec2& p2, const glm::vec2& p3) {
        writeSpriteVertex(p1);
        writeSpriteVertex(p2);
        writeSpriteVertex(p3);
        writeSpriteIndex(vertexIndex + 0);
        writeSpriteIndex(vertexIndex + 1);
        writeSpriteIndex(vertexIndex + 2);
        vertexIndex += 3;
    });
}

static SpriteDrawMap drawMap;

void ObjectBatch::addSprite(
    GameObject* object,
    cocos2d::CCSprite* sprite,
    SpriteType type,
    const cocos2d::CCAffineTransform& transform
) {
    prepareSpriteMeshWrite(object, sprite, type, transform);

    usize indiciesBegin = indicies.size();

    ConvexList* spriteMesh = SpriteMeshDictionary::getSpriteMeshForSprite(sprite);

    drawMap.addSprite(object, sprite, type, {});

    if (spriteMesh) {
        writeSpriteMeshFromConvexList(*spriteMesh);
    } else {
        writeSpriteVertex({ 0, 0 });
        writeSpriteVertex({ 1, 0 });
        writeSpriteVertex({ 0, 1 });
        writeSpriteVertex({ 1, 1 });

        writeSpriteIndex(QUAD_BL);
        writeSpriteIndex(QUAD_TL);
        writeSpriteIndex(QUAD_TR);
        writeSpriteIndex(QUAD_BL);
        writeSpriteIndex(QUAD_TR);
        writeSpriteIndex(QUAD_BR);
    }

    visibilityManager.addObjectSprite(sprite, type, indiciesBegin, indicies.size());
}

void ObjectBatch::writeGameObject(GameObject* object) {
    float originalScaleX = object->getScaleX();
    float originalScaleY = object->getScaleY();

    if (object->m_usesAudioScale) {
        object->setScaleX(object->m_scaleX);
        object->setScaleY(object->m_scaleY);
    }

    visibilityManager.prepareForObject(object);

    ObjectUtils::unpackObjectIntoSprites(object, [&](const UnpackedSprite& sprite) {
        addSprite(sprite.parentObject, sprite.sprite, sprite.type, sprite.transform);
    });

    object->setScaleX(originalScaleX);
    object->setScaleY(originalScaleY);
}

void ObjectBatch::finishWriting() {
    storeGLStates();

    auto layers = visibilityManager.getUsedLayerIds();
    for (auto& layerId : layers)
        layerDrawCalls.push_back({ layerId, visibilityManager.getLayer(layerId), 0, 0 });

    visibilityManager.generateFastStructures();

    if (vertexBuffer) {
        Buffer::destroy(vertexBuffer);
        vertexBuffer = nullptr;
    }

    if (indexBuffer) {
        Buffer::destroy(indexBuffer);
        indexBuffer = nullptr;
    }

    // log::info("ORDER SET COUNT {}", drawMap.orderSetsCount());

    vertexCount = verticies.size();
    indexCount  = indicies.size();

    vertexBuffer = Buffer::createStaticDraw("Object vertex buffer", verticies.data(), vertexCount * sizeof(ObjectVertex));
    culledIndicies.resize(indexCount);
    indexBuffer = Buffer::createDynamicDraw("Object index buffer", indexCount * sizeof(u32));

    verticies.clear();
    
    prepareVAO();
    restoreGLStates();
}

void ObjectBatch::predraw(const CameraView& view) {
    auto timer = BProfiler::start("Calculate visibilities");
    visibilityManager.calculateVisibilitiesForCameraView(view);
    timer.end();

    timer = BProfiler::start("Generate indicies");

    usize index = 0;

    for (auto& drawCall : layerDrawCalls) {
        drawCall.startIndex = index;
        
        visibilityManager.forEachVisibleSpriteIndexRangeInLayer(drawCall.layer, [&](usize begin, usize end) {
            // Maybe use std::copy?
            memcpy(culledIndicies.data() + index, indicies.data() + begin, (end - begin) * sizeof(u32));
            index += end - begin;
        });

        drawCall.indexCount = index - drawCall.startIndex;
    }

    // log::info("INDICIES COUNT {}", index);
    // for (i32 i = 0; i < index; i++)
    //     log::info("INDEX {}", culledIndicies[i]);

    culledIndexCount = index;
    indexBuffer->write(culledIndicies.data(), culledIndexCount * sizeof(u32));

    timer.end();
}

ObjectBatch::LayerDrawCall* ObjectBatch::getDrawCall(const LayerKey& id) {
    for (auto& drawCall : layerDrawCalls) {
        if (drawCall.id == id)
            return &drawCall;
    }
    return nullptr;
}

void ObjectBatch::draw(LayerDrawCall* drawCall) {
    if (drawCall == nullptr)
        return;

    bind();
    glDrawElements(GL_TRIANGLES, drawCall->indexCount, GL_UNSIGNED_INT, (void*)(drawCall->startIndex * sizeof(u32)));
}

struct AttribTypeInfo {
    i32 openGlType;
    i32 componentCount;
    u32 size;
};

static AttribTypeInfo getInfoOfAttributeTypeString(std::string type) {
    if (type == "float") return { GL_FLOAT, 1, sizeof(float) * 1 };
    if (type == "vec2")  return { GL_FLOAT, 2, sizeof(float) * 2 };
    if (type == "vec3")  return { GL_FLOAT, 3, sizeof(float) * 3 };
    if (type == "vec4")  return { GL_FLOAT, 4, sizeof(float) * 4 };

    if (type == "i8")                   return { GL_BYTE,  1, sizeof(i8)  };
    if (type == "i16")                  return { GL_SHORT, 1, sizeof(i16) };
    if (type == "int" || type == "i32") return { GL_INT,   1, sizeof(i32) };

    if (type == "u8")  return { GL_UNSIGNED_BYTE,  1, sizeof(u8)  };
    if (type == "u16") return { GL_UNSIGNED_SHORT, 1, sizeof(u16) };
    if (type == "u32") return { GL_UNSIGNED_INT,   1, sizeof(u32) };
    
    return { 0, 0 };
}

static void vertexAttribPointer(u32 id, const AttribTypeInfo& info, usize stride, usize offset) {
    if (info.openGlType == GL_FLOAT)
        glVertexAttribPointer(id, info.componentCount, info.openGlType, GL_FALSE, sizeof(ObjectVertex), (void*)offset);
    else if (info.openGlType == GL_DOUBLE)
        glVertexAttribLPointer(id, info.componentCount, info.openGlType, sizeof(ObjectVertex), (void*)offset);
    else
        glVertexAttribIPointer(id, info.componentCount, info.openGlType, sizeof(ObjectVertex), (void*)offset);
}

#define VERTEX_ATTRIBUTE_AS_ATTRIB_POINTER_CALL(ID, TYPE, NAME) \
    { \
        auto info = getInfoOfAttributeTypeString(#TYPE); \
        vertexAttribPointer(ID, info, sizeof(ObjectVertex), offsetof(ObjectVertex, NAME)); \
        glEnableVertexAttribArray(ID); \
    }

void ObjectBatch::prepareVAO() {
    if (vao == 0)
        glGenVertexArrays(1, &vao);

    glBindVertexArray(vao);
    vertexBuffer->bindAs(GL_ARRAY_BUFFER);

    OBJECT_VERTEX_ATTRIBUTES(VERTEX_ATTRIBUTE_AS_ATTRIB_POINTER_CALL)
}