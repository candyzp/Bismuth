#if defined(GEODE_IS_IOS)

#include "IOSMetalObjectBatch.hpp"

#include <Geode/binding/EnhancedGameObject.hpp>
#include <Geode/binding/RingObject.hpp>
#include <Geode/cocos/CCDirector.h>
#include "../SpriteMeshDictionary.hpp"
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

using namespace geode::prelude;

namespace BismuthIOSMetal {
namespace {

constexpr std::uint32_t IOS_OBJECT_FLAG_AUDIO_SCALE = 1u << 0;
constexpr std::uint32_t IOS_OBJECT_FLAG_CUSTOM_AUDIO_SCALE = 1u << 1;
constexpr std::uint32_t IOS_OBJECT_FLAG_ORB = 1u << 2;
constexpr std::uint32_t IOS_OBJECT_FLAG_INVISIBLE_BLOCK = 1u << 3;
constexpr std::uint32_t IOS_OBJECT_FLAG_SPECIAL_GLOW = 1u << 4;
constexpr std::uint32_t IOS_OBJECT_FLAG_GLOW_LBG = 1u << 5;
constexpr std::uint32_t IOS_OBJECT_FLAG_BASE_HSV = 1u << 6;
constexpr std::uint32_t IOS_OBJECT_FLAG_DETAIL_HSV = 1u << 7;
constexpr std::uint32_t IOS_OBJECT_FLAG_STATIC_OBJECT = 1u << 8;
constexpr std::uint32_t IOS_OBJECT_FLAG_STATIC_GLOW = 1u << 9;

struct PackedHSV {
    std::uint32_t hueAndFlags = 0;
    std::uint32_t saturationAndValue = 0;
};

PackedHSV packHSV(const cocos2d::ccHSVValue& hsv) {
    PackedHSV result;
    result.hueAndFlags = glm::packHalf2x16(glm::vec2(hsv.h, 0.f));
    result.saturationAndValue = glm::packHalf2x16(glm::vec2(hsv.s, hsv.v));
    if (hsv.absoluteSaturation)
        result.hueAndFlags |= 0x20000000u;
    if (hsv.absoluteBrightness)
        result.hueAndFlags |= 0x10000000u;
    return result;
}

std::uint32_t packRGBA(cocos2d::ccColor3B color, std::uint8_t alpha = 255) {
    return static_cast<std::uint32_t>(color.r) |
        (static_cast<std::uint32_t>(color.g) << 8) |
        (static_cast<std::uint32_t>(color.b) << 16) |
        (static_cast<std::uint32_t>(alpha) << 24);
}

std::uint32_t safeColorChannel(std::uint32_t channel) {
    return std::min<std::uint32_t>(channel, 1100u);
}

bool hasCustomVanillaMesh(GameObject* object) {
    const int id = object->m_objectID;
    return id >= 1769 && id <= 1810 && id != 1798;
}

} // namespace

ObjectBatch::~ObjectBatch() {
    clear();
}

void ObjectBatch::clear() {
    if (vertexBuffer) {
        glDeleteBuffers(1, &vertexBuffer);
        vertexBuffer = 0;
    }
    layer = nullptr;
    textureForSheet = {};
    objectIndices.clear();
    objectInfos.clear();
    workItems.clear();
    inputSprites.clear();
    sourceVertices.clear();
    orderedVertices.clear();
    sortEntries.clear();
    layerRanges.clear();
    appliedBlendStates = {};
    hasBlendStates = false;
    built = false;
}

bool ObjectBatch::buildObjectInfos(const std::vector<GameObject*>& objects) {
    objectIndices.clear();
    objectInfos.clear();
    objectIndices.reserve(objects.size());
    objectInfos.reserve(objects.size());

    for (GameObject* object : objects) {
        if (!object || object == layer->m_anticheatSpike ||
            object == layer->m_endPortal || object->isTrigger() ||
            object->m_isHide || object->m_objectID == 2016 ||
            object->m_objectID == 3032 || objectIndices.contains(object))
            continue;

        ObjectInfo info;
        if (object->getHasRotateAction())
            info.rotationSpeed =
                static_cast<EnhancedGameObject*>(object)->m_rotationDelta;

        std::uint32_t flags = 0;
        if (object->m_usesAudioScale && !object->m_hasNoAudioScale) {
            flags |= IOS_OBJECT_FLAG_AUDIO_SCALE;
            if (object->m_customAudioScale) {
                flags |= IOS_OBJECT_FLAG_CUSTOM_AUDIO_SCALE;
                info.audioScaleMin = object->m_minAudioScale;
                info.audioScaleMax = object->m_maxAudioScale;
            }
            if (typeinfo_cast<RingObject*>(object))
                flags |= IOS_OBJECT_FLAG_ORB;
        }

        info.baseColorChannelId = safeColorChannel(static_cast<std::uint32_t>(
            std::max<std::int32_t>(0, static_cast<std::int32_t>(object->m_activeMainColorID))
        ));
        info.detailColorChannelId = safeColorChannel(static_cast<std::uint32_t>(
            std::max<std::int32_t>(0, static_cast<std::int32_t>(object->m_activeDetailColorID))
        ));

        if (object->m_baseColor && object->m_baseColor->m_usesHSV) {
            flags |= IOS_OBJECT_FLAG_BASE_HSV;
            const auto hsv = packHSV(object->m_baseColor->m_hsv);
            info.baseHSVHueAndFlags = hsv.hueAndFlags;
            info.baseHSVSaturationAndValue = hsv.saturationAndValue;
        }
        if (object->m_detailColor && object->m_detailColor->m_usesHSV) {
            flags |= IOS_OBJECT_FLAG_DETAIL_HSV;
            const auto hsv = packHSV(object->m_detailColor->m_hsv);
            info.detailHSVHueAndFlags = hsv.hueAndFlags;
            info.detailHSVSaturationAndValue = hsv.saturationAndValue;
        }

        const float opacity = object->m_opacityMod2 > 0.f
            ? object->m_opacityMod2 : 1.f;
        const float glowOpacity = object->m_opacityMod;
        float blackChildOpacity = 1.f;
        if (object->m_hasCustomChild && !object->m_blackChildOpacityLocked &&
            object->m_blackChildOpacity > 0.f)
            blackChildOpacity = object->m_blackChildOpacity;

        info.opacityValues = glm::packUnorm4x8(
            glm::vec4(opacity, blackChildOpacity, glowOpacity, 0.f)
        );

        if (object->m_cantColorGlow && object->m_glowSprite) {
            flags |= IOS_OBJECT_FLAG_STATIC_GLOW;
            info.staticGlowColor = packRGBA(object->m_glowSprite->getColor());
        }
        if (object->m_isInvisibleBlock)
            flags |= IOS_OBJECT_FLAG_INVISIBLE_BLOCK;
        if (object->m_customGlowColor)
            flags |= IOS_OBJECT_FLAG_SPECIAL_GLOW;
        if (object->m_glowColorIsLBG)
            flags |= IOS_OBJECT_FLAG_GLOW_LBG;
        if (object->m_objectType == GameObjectType::Solid)
            flags |= IOS_OBJECT_FLAG_STATIC_OBJECT;

        info.flags = static_cast<std::int32_t>(flags);
        info.fadeMargin = object->m_fadeMargin;

        const auto index = static_cast<std::uint32_t>(objectInfos.size());
        objectIndices.emplace(object, index);
        objectInfos.push_back(info);
    }

    return !objectInfos.empty();
}

ObjectBatch::SpriteTransforms ObjectBatch::getSpriteVertexTransform(
    cocos2d::CCSprite* sprite,
    const cocos2d::CCAffineTransform& transform,
    cocos2d::CCTexture2D* texture
) const {
    const CCRect crop = sprite->getTextureRect();
    glm::vec2 positionBottomLeft = ccPointToGLM(
        CCPointApplyAffineTransform(sprite->getOffsetPosition(), transform)
    );
    glm::vec2 positionRight = glm::vec2(transform.a, transform.b) * crop.size.width;
    glm::vec2 positionUp = glm::vec2(transform.c, transform.d) * crop.size.height;

    glm::vec2 texBottomLeft;
    glm::vec2 texRight;
    glm::vec2 texUp;
    if (!sprite->isTextureRectRotated()) {
        texBottomLeft = {crop.origin.x, crop.origin.y + crop.size.height};
        texRight = {crop.size.width, 0.f};
        texUp = {0.f, -crop.size.height};
    } else {
        texBottomLeft = {crop.origin.x, crop.origin.y};
        texRight = {0.f, crop.size.width};
        texUp = {crop.size.height, 0.f};
    }

    if (sprite->isFlipX()) {
        positionBottomLeft += positionRight;
        positionRight = -positionRight;
    }
    if (sprite->isFlipY()) {
        positionBottomLeft += positionUp;
        positionUp = -positionUp;
    }

    const float contentScale = CCDirector::get()->getContentScaleFactor();
    const glm::vec2 textureScale(
        contentScale / static_cast<float>(texture->getPixelsWide()),
        contentScale / static_cast<float>(texture->getPixelsHigh())
    );
    texBottomLeft *= textureScale;
    texRight *= textureScale;
    texUp *= textureScale;

    return {
        positionBottomLeft, positionRight, positionUp,
        texBottomLeft, texRight, texUp
    };
}

void ObjectBatch::writeSpriteVertex(
    const glm::vec2& point,
    const SpriteTransforms& transforms,
    const glm::vec2& objectStartPosition,
    std::uint32_t spriteIndex
) {
    const glm::vec2 position =
        transforms.positionRight * point.x +
        transforms.positionUp * point.y +
        transforms.positionBottomLeft - objectStartPosition;
    const glm::vec2 texCoord =
        transforms.texCoordRight * point.x +
        transforms.texCoordUp * point.y +
        transforms.texCoordBottomLeft;

    Vertex vertex;
    vertex.positionOffset[0] = position.x;
    vertex.positionOffset[1] = position.y;
    vertex.texCoord[0] = texCoord.x;
    vertex.texCoord[1] = texCoord.y;
    vertex.spriteIndex = static_cast<float>(spriteIndex);
    sourceVertices.push_back(vertex);
}

bool ObjectBatch::addSprite(
    GameObject* object,
    cocos2d::CCSprite* sprite,
    SpriteType type,
    const cocos2d::CCAffineTransform& transform,
    const GroupIndexResolver& groupIndexForObject
) {
    if (!object || !sprite) {
        lastBuildError = "Sprite extraction received a null object or sprite";
        return false;
    }
    if (object->m_hasNoGlow && type == SpriteType::GLOW)
        return true;

    const SpriteSheet sheet = ObjectUtils::getSpritesheetOfObject(object, type);
    // These sheets are intentionally not managed by Bismuth's custom
    // renderer on desktop either. Leave them in their original Cocos batch
    // instead of letting one unsupported sprite abort the entire level.
    if (sheet == SpriteSheet::SPECIAL || sheet == SpriteSheet::_UNK)
        return true;

    const auto objectIt = objectIndices.find(object);
    if (objectIt == objectIndices.end()) {
        lastBuildError = fmt::format(
            "Object {} was unpacked but has no static metadata record",
            object->m_objectID
        );
        return false;
    }

    constexpr std::uint32_t MAX_EXACT_FLOAT_INDEX = 1u << 24;
    const std::uint32_t spriteIndex = static_cast<std::uint32_t>(inputSprites.size());
    const std::uint32_t objectIndex = objectIt->second;
    const std::uint32_t groupIndex = groupIndexForObject(object);
    if (spriteIndex >= MAX_EXACT_FLOAT_INDEX ||
        objectIndex >= MAX_EXACT_FLOAT_INDEX ||
        groupIndex >= MAX_EXACT_FLOAT_INDEX) {
        lastBuildError = "A sprite, object, or group index exceeds the exact float-index range";
        return false;
    }

    cocos2d::CCTexture2D* texture = textureForSheet
        ? textureForSheet(sheet) : nullptr;
    // Use the sprite's own texture for a valid sheet when a lookup entry is
    // unavailable. SPECIAL/_UNK have already been left on the vanilla path.
    if (!texture)
        texture = sprite->getTexture();
    if (!texture) {
        lastBuildError = fmt::format(
            "No texture for object {} on sprite sheet {}",
            object->m_objectID, static_cast<int>(sheet)
        );
        return false;
    }

    const SpriteTransforms transforms = getSpriteVertexTransform(
        sprite, transform, texture
    );
    const glm::vec2 objectStartPosition = ccPointToGLM(object->m_startPosition);
    const std::uint32_t firstTriangle =
        static_cast<std::uint32_t>(sourceVertices.size() / 3);

    const auto writeDefaultQuad = [&] {
        writeSpriteVertex({0.f, 0.f}, transforms, objectStartPosition, spriteIndex);
        writeSpriteVertex({1.f, 0.f}, transforms, objectStartPosition, spriteIndex);
        writeSpriteVertex({1.f, 1.f}, transforms, objectStartPosition, spriteIndex);
        writeSpriteVertex({1.f, 1.f}, transforms, objectStartPosition, spriteIndex);
        writeSpriteVertex({0.f, 1.f}, transforms, objectStartPosition, spriteIndex);
        writeSpriteVertex({0.f, 0.f}, transforms, objectStartPosition, spriteIndex);
    };

    if (hasCustomVanillaMesh(object)) {
        const auto quad = sprite->getQuad();
        const CCPoint offset = sprite->getOffsetPosition();
        const CCPoint size = sprite->getTextureRect().size;

        if (size.x == 0.f || size.y == 0.f) {
            writeDefaultQuad();
        } else {
            const CCPoint bl = (ccp(quad.bl.vertices.x, quad.bl.vertices.y) - offset) / size;
            const CCPoint br = (ccp(quad.br.vertices.x, quad.br.vertices.y) - offset) / size;
            const CCPoint tl = (ccp(quad.tl.vertices.x, quad.tl.vertices.y) - offset) / size;
            const CCPoint tr = (ccp(quad.tr.vertices.x, quad.tr.vertices.y) - offset) / size;
            writeSpriteVertex(ccPointToGLM(bl), transforms, objectStartPosition, spriteIndex);
            writeSpriteVertex(ccPointToGLM(br), transforms, objectStartPosition, spriteIndex);
            writeSpriteVertex(ccPointToGLM(tr), transforms, objectStartPosition, spriteIndex);
            writeSpriteVertex(ccPointToGLM(tr), transforms, objectStartPosition, spriteIndex);
            writeSpriteVertex(ccPointToGLM(tl), transforms, objectStartPosition, spriteIndex);
            writeSpriteVertex(ccPointToGLM(bl), transforms, objectStartPosition, spriteIndex);
        }
    } else {
        const ConvexList* mesh = SpriteMeshDictionary::getSpriteMeshForSprite(sprite);
        if (mesh) {
            mesh->triangulate([&](const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
                writeSpriteVertex(a, transforms, objectStartPosition, spriteIndex);
                writeSpriteVertex(b, transforms, objectStartPosition, spriteIndex);
                writeSpriteVertex(c, transforms, objectStartPosition, spriteIndex);
            });
            // An empty dictionary entry is equivalent to a missing custom
            // mesh: retain the standard six-vertex sprite quad.
            if (sourceVertices.size() / 3 == firstTriangle)
                writeDefaultQuad();
        } else {
            writeDefaultQuad();
        }
    }

    const std::uint32_t triangleCount =
        static_cast<std::uint32_t>(sourceVertices.size() / 3) - firstTriangle;
    if (triangleCount == 0) {
        lastBuildError = fmt::format(
            "Sprite mesh produced no triangles for object {} (type {}, sheet {})",
            object->m_objectID, static_cast<int>(type), static_cast<int>(sheet)
        );
        return false;
    }

    InputSprite input;
    input.objectIndex = objectIndex;
    input.spriteType = static_cast<std::uint32_t>(type);
    input.triangleStart = firstTriangle;
    input.triangleCount = triangleCount;
    inputSprites.push_back(input);

    SpriteWorkItem work;
    work.startX = objectStartPosition.x;
    work.startY = objectStartPosition.y;
    work.groupCombinationIndex = groupIndex;
    work.spriteIndex = spriteIndex;
    workItems.push_back(work);

    SortEntry entry;
    entry.key = {object->getObjectZLayer(), sheet, false};
    entry.zOrder = object->getObjectZOrder();
    entry.sourceSpriteIndex = spriteIndex;
    entry.colorChannel = safeColorChannel(
        ObjectUtils::getSpriteColorChannel(type, object, sprite)
    );
    sortEntries.push_back(entry);

    return true;
}

bool ObjectBatch::writeGameObject(
    GameObject* object,
    const GroupIndexResolver& groupIndexForObject
) {
    if (!object)
        return false;

    const float originalScaleX = object->getScaleX();
    const float originalScaleY = object->getScaleY();
    if (object->m_usesAudioScale) {
        object->setScaleX(object->m_scaleX);
        object->setScaleY(object->m_scaleY);
    }

    bool success = true;
    ObjectUtils::unpackObjectIntoSprites(object, [&](const UnpackedSprite& unpacked) {
        if (success)
            success = addSprite(
                unpacked.parentObject,
                unpacked.sprite,
                unpacked.type,
                unpacked.transform,
                groupIndexForObject
            );
    });

    object->setScaleX(originalScaleX);
    object->setScaleY(originalScaleY);
    return success;
}

bool ObjectBatch::buildOrderedVertices(const BlendStates& blendStates) {
    orderedVertices.clear();
    layerRanges.clear();

    struct LayeredSprite {
        LayerKey key;
        std::int32_t zOrder;
        std::uint32_t spriteIndex;
    };

    std::vector<LayeredSprite> ordered;
    ordered.reserve(sortEntries.size());

    for (const auto& entry : sortEntries) {
        LayeredSprite item;
        item.key = entry.key;
        item.key.blending = blendStates[entry.colorChannel];
        item.zOrder = entry.zOrder;
        item.spriteIndex = entry.sourceSpriteIndex;
        ordered.push_back(item);
    }

    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        const auto left = a.key.asNumber();
        const auto right = b.key.asNumber();
        if (left != right)
            return left < right;
        return a.zOrder < b.zOrder;
    });

    std::uint32_t currentVertex = 0;
    std::size_t cursor = 0;
    while (cursor < ordered.size()) {
        const LayerKey key = ordered[cursor].key;
        LayerRange range;
        range.key = key;
        range.firstVertex = currentVertex;

        while (cursor < ordered.size() &&
            ordered[cursor].key.asNumber() == key.asNumber()) {
            const std::uint32_t spriteIndex = ordered[cursor].spriteIndex;
            const InputSprite& input = inputSprites[spriteIndex];
            const std::size_t first =
                static_cast<std::size_t>(input.triangleStart) * 3;
            const std::size_t count =
                static_cast<std::size_t>(input.triangleCount) * 3;
            if (first > sourceVertices.size() ||
                count > sourceVertices.size() - first)
                return false;

            orderedVertices.insert(
                orderedVertices.end(),
                sourceVertices.begin() + static_cast<std::ptrdiff_t>(first),
                sourceVertices.begin() + static_cast<std::ptrdiff_t>(first + count)
            );
            currentVertex += static_cast<std::uint32_t>(count);
            ++cursor;
        }

        range.vertexCount = currentVertex - range.firstVertex;
        if (range.vertexCount)
            layerRanges.push_back(range);
    }

    return orderedVertices.size() == sourceVertices.size();
}

bool ObjectBatch::uploadOrderedVertices() {
    if (orderedVertices.empty())
        return true;

    if (!vertexBuffer)
        glGenBuffers(1, &vertexBuffer);
    if (!vertexBuffer)
        return false;

    GLint previous = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(orderedVertices.size() * sizeof(Vertex)),
        orderedVertices.data(),
        GL_STATIC_DRAW
    );
    const GLenum error = glGetError();
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previous));
    return error == GL_NO_ERROR;
}

bool ObjectBatch::build(
    GJBaseGameLayer* gameLayer,
    const std::vector<GameObject*>& objects,
    const GroupIndexResolver& groupIndexForObject,
    const BlendStates& blendStates,
    const TextureResolver& textureCallback
) {
    clear();
    lastBuildError.clear();
    if (!gameLayer || !groupIndexForObject || !textureCallback) {
        lastBuildError = "Missing game layer, group-index resolver, or texture resolver";
        return false;
    }

    layer = gameLayer;
    textureForSheet = textureCallback;

    if (!buildObjectInfos(objects)) {
        lastBuildError = "No eligible static object metadata could be extracted";
        clear();
        return false;
    }

    for (GameObject* object : objects) {
        if (!object || object == layer->m_anticheatSpike ||
            object == layer->m_endPortal || object->isTrigger() ||
            object->m_isHide || object->m_objectID == 2016 ||
            object->m_objectID == 3032)
            continue;
        if (!writeGameObject(object, groupIndexForObject)) {
            if (lastBuildError.empty())
                lastBuildError = fmt::format(
                    "Sprite extraction failed while processing object {}",
                    object->m_objectID
                );
            clear();
            return false;
        }
    }

    if (workItems.size() != inputSprites.size() ||
        inputSprites.empty() || sourceVertices.empty()) {
        lastBuildError = fmt::format(
            "Extracted sprite records are empty or inconsistent (work={}, sprites={}, vertices={})",
            workItems.size(), inputSprites.size(), sourceVertices.size()
        );
        clear();
        return false;
    }

    if (!buildOrderedVertices(blendStates)) {
        lastBuildError = "Static vertex ordering failed";
        clear();
        return false;
    }
    if (!uploadOrderedVertices()) {
        lastBuildError = "Static OpenGL ES vertex-buffer upload failed";
        clear();
        return false;
    }

    appliedBlendStates = blendStates;
    hasBlendStates = true;
    built = true;
    return true;
}

bool ObjectBatch::refreshBlendOrder(const BlendStates& blendStates) {
    if (!built || !hasBlendStates || blendStates == appliedBlendStates)
        return true;

    // This is triggered only by an actual blend-mode change. Static sprite
    // meshes and per-sprite metadata are never rebuilt during normal frames.
    if (!buildOrderedVertices(blendStates) || !uploadOrderedVertices())
        return false;

    appliedBlendStates = blendStates;
    return true;
}

} // namespace BismuthIOSMetal

#endif
