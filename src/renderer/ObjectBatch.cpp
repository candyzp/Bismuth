#include "ObjectBatch.hpp"
#include "BProfiler.hpp"
#include "Renderer.hpp"
#include "SpriteMeshDictionary.hpp"
#include "common.hpp"
#include "glm/fwd.hpp"
#include "math/ConvexList.hpp"
#include "math/ConvexPolygon.hpp"
#include <culling/SpriteDrawMap.hpp>
#include <Geode/binding/CheckpointGameObject.hpp>
#include <cstring>
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

bool ObjectBatch::shouldTrackLiveSpriteObject(GameObject* object) const {
    if (!object)
        return false;

    // Geometry Dash's AnimatedGameObject path changes CCSpritePart display
    // frames and child state while the level is running. Bismuth's main VBO is
    // otherwise a level-start snapshot, so keep those ordinary quad sprites in
    // the lightweight live refresh path as well.
    if (object->m_classType == GameObjectClassType::Animated)
        return true;

    // Gameplay objects animate after interaction even when they are not an
    // AnimatedGameObject. Orbs/pads pulse, portals change child transforms,
    // and coins/collectibles swap or hide parts after collection. Mirror those
    // small live changes into the GPU VBO instead of redrawing them with Cocos.
    if (ObjectUtils::isInteractiveVisualObject(object))
        return true;

    return typeinfo_cast<CheckpointGameObject*>(object) != nullptr;
}

void ObjectBatch::addSprite(
    GameObject* object,
    cocos2d::CCSprite* sprite,
    SpriteType type,
    const cocos2d::CCAffineTransform& transform
) {
    prepareSpriteMeshWrite(object, sprite, type, transform);

    usize indiciesBegin = indicies.size();
    usize vertexBegin = verticies.size();

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

    // Dynamic object art is still ordinary quad art. Keep its static positions
    // in the fast batch, but remember the four vertices so a frame swap or
    // base/detail color-state change can be mirrored with a tiny partial VBO
    // update instead of rebuilding the level.
    if (!spriteMesh && shouldTrackLiveSpriteObject(object) && verticies.size() - vertexBegin == VERTICIES_PER_QUAD) {
        LiveSpriteRecord record;
        record.object = object;
        record.sprite = sprite;
        record.type = type;
        record.vertexBegin = (u32)vertexBegin;
        record.bakedTransform = transform;
        record.bakedObjectTransform = object->nodeToParentTransform();
        for (u32 i = 0; i < VERTICIES_PER_QUAD; ++i)
            record.vertices[i] = verticies[vertexBegin + i];
        liveSprites.push_back(record);
    }
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

cocos2d::CCAffineTransform ObjectBatch::getLiveSpriteTransform(const LiveSpriteRecord& record) const {
    auto transform = CCAffineTransformMakeIdentity();
    auto node = static_cast<cocos2d::CCNode*>(record.sprite);

    // Rebuild the current child-to-object transform while freezing the root
    // GameObject transform at its baked value. Sprite-part animation therefore
    // reaches the VBO, while GPU group translation/rotation is not applied a
    // second time on the CPU.
    while (node && node != record.object) {
        transform = CCAffineTransformConcat(transform, node->nodeToParentTransform());
        node = node->getParent();
    }

    if (node == record.object)
        return CCAffineTransformConcat(transform, record.bakedObjectTransform);

    // Detached color/glow sprites do not have an object-relative parent chain.
    // Keep their baked placement, but still allow their frame UV to refresh.
    return record.bakedTransform;
}

static bool isLiveChildSpriteVisible(const ObjectBatch::LiveSpriteRecord& record) {
    if (!record.object || !record.sprite)
        return false;

    bool belongsToObjectTree = false;
    for (auto node = static_cast<cocos2d::CCNode*>(record.sprite); node; node = node->getParent()) {
        if (node == record.object) {
            belongsToObjectTree = true;
            break;
        }
    }

    // Detached base/detail/glow sprites can inherit stale visibility from GD's
    // disabled stock batch nodes. Do not treat that external state as live.
    if (!belongsToObjectTree)
        return true;

    // Inside the object tree, child visibility is meaningful for coin/orb/
    // portal animation. Deliberately stop before the root GameObject because
    // its own Cocos visibility can be stale when Bismuth owns culling.
    for (auto node = static_cast<cocos2d::CCNode*>(record.sprite); node && node != record.object; node = node->getParent()) {
        if (!node->isVisible())
            return false;
    }
    return true;
}

void ObjectBatch::refreshLiveSpriteData() {
    if (!vertexBuffer || liveSprites.empty())
        return;

    const auto identity = CCAffineTransformMakeIdentity();

    for (auto& record : liveSprites) {
        if (!record.object || !record.sprite)
            continue;

        const bool isAnimated = record.object->m_classType == GameObjectClassType::Animated;
        const bool isInteractive = ObjectUtils::isInteractiveVisualObject(record.object);
        const bool hasLiveChildState = isAnimated || isInteractive;

        if (isAnimated && !record.object->m_isActivated)
            continue;

        auto updated = record.vertices;

        if (hasLiveChildState && !isLiveChildSpriteVisible(record)) {
            // Keep the draw entirely in the GPU batch. Hidden interaction parts
            // are moved outside clip space until GD makes the child visible
            // again; no stock Cocos draw is re-enabled.
            constexpr float HIDDEN_VERTEX = 1000000.0f;
            for (u32 i = 0; i < VERTICIES_PER_QUAD; ++i)
                updated[i].positionOffset = { HIDDEN_VERTEX, HIDDEN_VERTEX };

            if (std::memcmp(updated.data(), record.vertices.data(), VERTICIES_PER_QUAD * sizeof(ObjectVertex)) != 0) {
                vertexBuffer->write(
                    updated.data(),
                    VERTICIES_PER_QUAD * sizeof(ObjectVertex),
                    (usize)record.vertexBegin * sizeof(ObjectVertex)
                );
                record.vertices = updated;
            }
            continue;
        }

        SpriteSheet spriteSheet = ObjectUtils::getSpritesheetOfObject(record.object, record.type);
        auto liveTransform = hasLiveChildState ? getLiveSpriteTransform(record) : identity;
        auto transforms = getSpriteVertexTransform(record.sprite, liveTransform, spriteSheet);

        u32 colorChannel = ObjectUtils::getSpriteColorChannel(record.type, record.object, record.sprite);
        if (record.type == SpriteType::DETAIL)
            colorChannel |= A_COLOR_CHANNEL_IS_SPRITE_DETAIL;

        // Keep group movement/rotation in the GPU group-state path, but mirror
        // current child-part transform and frame rectangle for animated and
        // gameplay-interactive objects. This lets orb/portal/coin animation
        // change the GPU VBO without rebuilding the level or drawing via Cocos.
        if (hasLiveChildState) {
            auto positionBottomLeft = transforms.positionBottomLeft - ccPointToGLM(record.object->m_startPosition);
            updated[QUAD_BL].positionOffset = positionBottomLeft;
            updated[QUAD_BR].positionOffset = positionBottomLeft + transforms.positionRight;
            updated[QUAD_TL].positionOffset = positionBottomLeft + transforms.positionUp;
            updated[QUAD_TR].positionOffset = positionBottomLeft + transforms.positionRight + transforms.positionUp;
        }

        updated[QUAD_BL].texCoord = transforms.texCoordBottomLeft;
        updated[QUAD_BR].texCoord = transforms.texCoordBottomLeft + transforms.texCoordRight;
        updated[QUAD_TL].texCoord = transforms.texCoordBottomLeft + transforms.texCoordUp;
        updated[QUAD_TR].texCoord = transforms.texCoordBottomLeft + transforms.texCoordRight + transforms.texCoordUp;

        for (u32 i = 0; i < VERTICIES_PER_QUAD; ++i)
            updated[i].colorChannel = colorChannel;

        if (std::memcmp(updated.data(), record.vertices.data(), VERTICIES_PER_QUAD * sizeof(ObjectVertex)) == 0)
            continue;

        vertexBuffer->write(
            updated.data(),
            VERTICIES_PER_QUAD * sizeof(ObjectVertex),
            (usize)record.vertexBegin * sizeof(ObjectVertex)
        );
        record.vertices = updated;
    }
}

void ObjectBatch::predraw(const CameraView& view) {
    auto timer = BProfiler::start("Calculate visibilities");
    visibilityManager.calculateVisibilitiesForCameraView(view);
    timer.end();

    // Visibility calculation activates newly visible AnimatedGameObjects and
    // establishes their current CCSpritePart state before the VBO is refreshed.
    refreshLiveSpriteData();

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