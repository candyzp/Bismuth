#ifdef GEODE_IS_IOS

#include "IOSObjectBatch.hpp"

#include <Geode/Enums.hpp>
#include <Geode/binding/EnhancedGameObject.hpp>
#include <Geode/binding/RingObject.hpp>
#include <Geode/cocos/CCDirector.h>
#include <Geode/utils/cocos.hpp>
#include <SpriteMeshDictionary.hpp>
#include <math/ConvexList.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace geode::prelude;

namespace IOSUpstream {
namespace {

constexpr u32 IOS_SPRITE_APPEARANCE_VISIBLE = 1u << 0;
constexpr u32 IOS_SPRITE_APPEARANCE_OPACITY_MODIFY_RGB = 1u << 1;
constexpr u32 IOS_SPRITE_APPEARANCE_DIRECT_TINT = 1u << 2;

u32 convertToShaderHSV(const cocos2d::ccHSVValue& hsv) {
    const u32 hue = static_cast<u32>(hsv.h + 256.f);
    const u32 saturation = static_cast<u32>(
        (hsv.s + (hsv.absoluteSaturation ? 1.0f : 0.0f)) * 127.5f
    );
    const u32 value = static_cast<u32>(
        (hsv.v + (hsv.absoluteBrightness ? 1.0f : 0.0f)) * 127.5f
    );

    u32 result =
        ((hue & 0x1ffu) << 16) |
        ((saturation & 0xffu) << 8) |
        (value & 0xffu);

    if (hsv.absoluteSaturation)
        result |= 0x20000000u;
    if (hsv.absoluteBrightness)
        result |= 0x10000000u;
    return result;
}

u32 packDisplayedColor(cocos2d::CCSprite* sprite) {
    if (!sprite)
        return 0;

    auto color = sprite->getDisplayedColor();
    u32 alpha = sprite->getDisplayedOpacity();
    if (sprite->isOpacityModifyRGB()) {
        const float factor = static_cast<float>(alpha) / 255.f;
        color.r = static_cast<GLubyte>(color.r * factor);
        color.g = static_cast<GLubyte>(color.g * factor);
        color.b = static_cast<GLubyte>(color.b * factor);
    }

    return static_cast<u32>(color.r) |
        (static_cast<u32>(color.g) << 8) |
        (static_cast<u32>(color.b) << 16) |
        (alpha << 24);
}

SpriteAppearance captureSpriteAppearance(
    cocos2d::CCSprite* sprite,
    bool usesDirectTint
) {
    SpriteAppearance result;
    result.packedDisplayedColor = packDisplayedColor(sprite);
    if (sprite && sprite->isVisible() && !sprite->getDontDraw())
        result.flags |= IOS_SPRITE_APPEARANCE_VISIBLE;
    if (sprite && sprite->isOpacityModifyRGB())
        result.flags |= IOS_SPRITE_APPEARANCE_OPACITY_MODIFY_RGB;
    if (usesDirectTint)
        result.flags |= IOS_SPRITE_APPEARANCE_DIRECT_TINT;
    return result;
}

} // namespace

ObjectBatch::~ObjectBatch() {
    clear();
}

void ObjectBatch::clear() {
    workGroupScheduler.clear();
    for (const auto& source : sourceSpritePointers) {
        if (source.sprite)
            source.sprite->release();
    }
    objectInfos.clear();
    objectIndices.clear();
    sourceVertices.clear();
    sourceInputSprites.clear();
    sourceSpriteAppearances.clear();
    sourceSpritePointers.clear();
    vertices.clear();
    inputSprites.clear();
    spriteAppearances.clear();
    spritePointers.clear();
    usedLayerKeyStorage.clear();
    usedLayerKeys.clear();
    sortedBatch = {};
    currentSpriteIndex = 0;
    hasCurrentBlendStates = false;
    currentBlendStates.fill(false);
    built = false;
}

u32 ObjectBatch::getObjectIndex(GameObject* object) const {
    const auto found = objectIndices.find(object);
    return found == objectIndices.end() ? 0u : found->second;
}

bool ObjectBatch::buildObjectInfo(const std::vector<GameObject*>& objects) {
    objectInfos.clear();
    objectIndices.clear();
    objectInfos.reserve(objects.size());
    objectIndices.reserve(objects.size());

    for (auto object : objects) {
        if (!object || object->isTrigger() || object->m_isHide ||
            object->m_objectID == GUIDE_OBJECT_ID)
            continue;

        ObjectInfo info;
        if (object->getHasRotateAction())
            info.rotationSpeed = static_cast<EnhancedGameObject*>(object)->m_rotationDelta;

        info.flags = 0;
        if (object->m_usesAudioScale) {
            info.flags |= IOS_OBJECT_FLAG_USES_AUDIO_SCALE;
            if (object->m_customAudioScale) {
                info.flags |= IOS_OBJECT_FLAG_CUSTOM_AUDIO_SCALE;
                info.audioScaleMin = object->m_minAudioScale;
                info.audioScaleMax = object->m_maxAudioScale;
            }
            if (typeinfo_cast<RingObject*>(object))
                info.flags |= IOS_OBJECT_FLAG_IS_ORB;
        }

        info.baseColorChannelId = static_cast<u32>(
            std::max<i32>(0, object->m_activeMainColorID)
        );
        info.detailColorChannelId = static_cast<u32>(
            std::max<i32>(0, object->m_activeDetailColorID)
        );

        if (object->m_baseColor && object->m_baseColor->m_usesHSV) {
            info.flags |= IOS_OBJECT_FLAG_HAS_BASE_HSV;
            info.baseHSV = convertToShaderHSV(object->m_baseColor->m_hsv);
        }
        if (object->m_detailColor && object->m_detailColor->m_usesHSV) {
            info.flags |= IOS_OBJECT_FLAG_HAS_DETAIL_HSV;
            info.detailHSV = convertToShaderHSV(object->m_detailColor->m_hsv);
        }

        info.opacity = object->m_opacityMod2 > 0.0f
            ? object->m_opacityMod2 : 1.0f;

        if (object->m_isInvisibleBlock)
            info.flags |= IOS_OBJECT_FLAG_IS_INVISIBLE_BLOCK;
        if (object->m_customGlowColor)
            info.flags |= IOS_OBJECT_FLAG_SPECIAL_GLOW_COLOR;
        if (object->m_glowColorIsLBG)
            info.flags |= IOS_OBJECT_FLAG_IS_GLOW_COLOR_LBG;
        if (object->m_objectType == GameObjectType::Solid)
            info.flags |= IOS_OBJECT_FLAG_IS_STATIC_OBJECT;

        info.fadeMargin = object->m_fadeMargin;

        const u32 index = static_cast<u32>(objectInfos.size());
        objectIndices.emplace(object, index);
        objectInfos.push_back(info);
    }

    return true;
}

ObjectBatch::SpriteVertexTransforms ObjectBatch::getSpriteVertexTransform(
    cocos2d::CCSprite* sprite,
    const cocos2d::CCAffineTransform& transform,
    cocos2d::CCTexture2D* texture
) const {
    if (!sprite || !texture)
        return {};

    const CCRect crop = sprite->getTextureRect();

    const glm::vec2 positionBottomLeft = ccPointToGLM(
        CCPointApplyAffineTransform(sprite->getOffsetPosition(), transform)
    );
    const glm::vec2 positionRight = glm::vec2(transform.a, transform.b) * crop.size.width;
    const glm::vec2 positionUp = glm::vec2(transform.c, transform.d) * crop.size.height;

    glm::vec2 textureBottomLeft;
    glm::vec2 textureRight;
    glm::vec2 textureUp;

    if (!sprite->isTextureRectRotated()) {
        textureBottomLeft = { crop.origin.x, crop.origin.y + crop.size.height };
        textureRight = { crop.size.width, 0.f };
        textureUp = { 0.f, -crop.size.height };
    } else {
        textureBottomLeft = { crop.origin.x, crop.origin.y };
        textureRight = { 0.f, crop.size.width };
        textureUp = { crop.size.height, 0.f };
    }

    glm::vec2 adjustedBottomLeft = positionBottomLeft;
    glm::vec2 adjustedRight = positionRight;
    glm::vec2 adjustedUp = positionUp;

    if (sprite->isFlipX()) {
        adjustedBottomLeft += adjustedRight;
        adjustedRight = -adjustedRight;
    }
    if (sprite->isFlipY()) {
        adjustedBottomLeft += adjustedUp;
        adjustedUp = -adjustedUp;
    }

    const float scaleFactor = CCDirector::get()->getContentScaleFactor();
    const glm::vec2 textureCoordFactor(
        scaleFactor / std::max(1, texture->getPixelsWide()),
        scaleFactor / std::max(1, texture->getPixelsHigh())
    );

    textureBottomLeft *= textureCoordFactor;
    textureRight *= textureCoordFactor;
    textureUp *= textureCoordFactor;

    return {
        adjustedBottomLeft,
        adjustedRight,
        adjustedUp,
        textureBottomLeft,
        textureRight,
        textureUp
    };
}

void ObjectBatch::writeSpriteVertex(
    glm::vec2 point,
    const SpriteVertexTransforms& transforms,
    glm::vec2 objectStartPosition,
    u32 spriteIndex
) {
    ObjectVertex vertex;
    vertex.positionOffset =
        transforms.positionRight * point.x +
        transforms.positionUp * point.y +
        transforms.positionBottomLeft - objectStartPosition;
    vertex.texCoord =
        transforms.texCoordRight * point.x +
        transforms.texCoordUp * point.y +
        transforms.texCoordBottomLeft;
    vertex.spriteIndex = static_cast<float>(spriteIndex);
    sourceVertices.push_back(vertex);
}

bool ObjectBatch::addSprite(
    GameObject* object,
    cocos2d::CCSprite* sprite,
    SpriteType type,
    const cocos2d::CCAffineTransform& transform,
    GroupManager& groupManager
) {
    if (!object || !sprite)
        return false;
    if (object->m_hasNoGlow && type == SpriteType::GLOW)
        return true;

    const SpriteSheet spriteSheet = ObjectUtils::getSpritesheetOfObject(object, type);
    const u32 objectIndex = getObjectIndex(object);
    const u32 groupCombinationIndex =
        groupManager.getGroupCombinationIndexForObject(object);

    const auto texture = resolveTexture
        ? resolveTexture(spriteSheet)
        : sprite->getTexture();
    if (!texture) {
        log::error(
            "Bismuth iOS upstream batch: missing texture for sprite sheet {}",
            static_cast<u32>(spriteSheet)
        );
        return false;
    }

    currentTransforms = getSpriteVertexTransform(sprite, transform, texture);
    currentObjectStartPosition = ccPointToGLM(object->m_startPosition);
    currentSpriteIndex = static_cast<u32>(sourceInputSprites.size());

    const usize firstTriangle = sourceVertices.size() / 3;
    const ConvexList* mesh = SpriteMeshDictionary::getSpriteMeshForSprite(sprite);

    if (mesh) {
        mesh->triangulate([&](const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
            writeSpriteVertex(a, currentTransforms, currentObjectStartPosition, currentSpriteIndex);
            writeSpriteVertex(b, currentTransforms, currentObjectStartPosition, currentSpriteIndex);
            writeSpriteVertex(c, currentTransforms, currentObjectStartPosition, currentSpriteIndex);
        });
    } else {
        // Match the owner's six-vertex/two-triangle quad layout.
        writeSpriteVertex({0.f, 0.f}, currentTransforms, currentObjectStartPosition, currentSpriteIndex);
        writeSpriteVertex({1.f, 0.f}, currentTransforms, currentObjectStartPosition, currentSpriteIndex);
        writeSpriteVertex({1.f, 1.f}, currentTransforms, currentObjectStartPosition, currentSpriteIndex);
        writeSpriteVertex({1.f, 1.f}, currentTransforms, currentObjectStartPosition, currentSpriteIndex);
        writeSpriteVertex({0.f, 1.f}, currentTransforms, currentObjectStartPosition, currentSpriteIndex);
        writeSpriteVertex({0.f, 0.f}, currentTransforms, currentObjectStartPosition, currentSpriteIndex);
    }

    const u32 triangleCount = static_cast<u32>(sourceVertices.size() / 3 - firstTriangle);
    if (triangleCount == 0)
        return false;

    const u32 colorChannelFlags = ObjectUtils::getSpriteColorChannel(type, object, sprite);
    InputSprite input;
    input.objectIndex = objectIndex;
    input.spriteType = static_cast<u32>(type);
    input.indexStart = static_cast<u32>(firstTriangle);
    input.indexCount = triangleCount;
    input.colorChannelFlags = colorChannelFlags;
    sourceInputSprites.push_back(input);

    const bool directTint =
        (colorChannelFlags & IOS_COLOR_CHANNEL_DIRECT_FLAG) != 0;
    sourceSpriteAppearances.push_back(captureSpriteAppearance(sprite, directTint));
    sprite->retain();
    sourceSpritePointers.push_back({object, sprite, type});

    workGroupScheduler.addSprite(
        object, sprite, type, currentSpriteIndex, triangleCount, groupCombinationIndex
    );

    usedLayerKeys.insert({
        object->getObjectZLayer(),
        spriteSheet,
        false
    });
    usedLayerKeys.insert({
        object->getObjectZLayer(),
        spriteSheet,
        true
    });
    return true;
}

bool ObjectBatch::writeGameObject(GameObject* object, GroupManager& groupManager) {
    if (!object)
        return false;

    const float originalScaleX = object->getScaleX();
    const float originalScaleY = object->getScaleY();
    if (object->m_usesAudioScale) {
        object->setScaleX(object->m_scaleX);
        object->setScaleY(object->m_scaleY);
    }

    bool spriteBuildOK = true;
    SpriteUnpackStats stats;
    const bool unpacked = ObjectUtils::unpackObjectIntoSprites(
        object,
        [&](const UnpackedSprite& unpackedSprite) {
            if (!spriteBuildOK)
                return;
            spriteBuildOK = addSprite(
                unpackedSprite.parentObject,
                unpackedSprite.sprite,
                unpackedSprite.type,
                unpackedSprite.transform,
                groupManager
            );
        },
        &stats
    );

    object->setScaleX(originalScaleX);
    object->setScaleY(originalScaleY);

    if (!unpacked || !spriteBuildOK) {
        log::error(
            "Bismuth iOS upstream batch rejected object {} (non-sprite children {}, duplicate sprites {})",
            object->m_objectID,
            stats.nonSpriteChildren,
            stats.duplicateSprites
        );
        return false;
    }
    return true;
}

bool ObjectBatch::applySortedBatch(const SortedObjectBatch& newSortedBatch) {
    sortedBatch = newSortedBatch;
    vertices.clear();
    inputSprites.clear();
    spriteAppearances.clear();
    spritePointers.clear();

    vertices.reserve(sourceVertices.size());
    inputSprites.reserve(sourceInputSprites.size());
    spriteAppearances.reserve(sourceSpriteAppearances.size());
    spritePointers.reserve(sourceSpritePointers.size());

    for (u32 sourceSpriteIndex : sortedBatch.inputSpriteIndices) {
        if (sourceSpriteIndex >= sourceInputSprites.size())
            return false;

        const InputSprite& sourceInput = sourceInputSprites[sourceSpriteIndex];
        const usize sourceVertexStart = static_cast<usize>(sourceInput.indexStart) * 3;
        const usize sourceVertexCount = static_cast<usize>(sourceInput.indexCount) * 3;
        if (sourceVertexStart > sourceVertices.size() ||
            sourceVertexCount > sourceVertices.size() - sourceVertexStart)
            return false;

        const u32 orderedSpriteIndex = static_cast<u32>(inputSprites.size());
        InputSprite orderedInput = sourceInput;
        orderedInput.indexStart = static_cast<u32>(vertices.size() / 3);
        inputSprites.push_back(orderedInput);

        for (usize i = 0; i < sourceVertexCount; ++i) {
            ObjectVertex vertex = sourceVertices[sourceVertexStart + i];
            vertex.spriteIndex = static_cast<float>(orderedSpriteIndex);
            vertices.push_back(vertex);
        }

        spriteAppearances.push_back(sourceSpriteAppearances[sourceSpriteIndex]);
        spritePointers.push_back(sourceSpritePointers[sourceSpriteIndex]);
    }

    usedLayerKeys.clear();
    for (const auto& [key, range] : sortedBatch.layerRanges) {
        if (range.spriteCount != 0)
            usedLayerKeys.insert(key);
    }

    if (vertices.size() != sourceVertices.size() ||
        inputSprites.size() != sourceInputSprites.size() ||
        spriteAppearances.size() != sourceSpriteAppearances.size() ||
        spritePointers.size() != sourceSpritePointers.size())
        return false;

    // The scheduler's cumulative triangle offsets must match the reordered
    // vertex stream exactly. This catches stale sprite indices before step 3
    // uploads any geometry to the GPU.
    for (const auto& [key, range] : sortedBatch.layerRanges) {
        const u64 endVertex = static_cast<u64>(range.firstVertex) + range.vertexCount;
        if (endVertex > vertices.size() || range.vertexCount % 3 != 0)
            return false;
    }

    return true;
}

bool ObjectBatch::finalizeSortedGeometry(const ColorChannelBlendStates& blendStates) {
    const SortedObjectBatch* generated =
        workGroupScheduler.waitForSortedBatch(blendStates);
    if (!generated)
        return false;

    if (!applySortedBatch(*generated))
        return false;
    currentBlendStates = blendStates;
    hasCurrentBlendStates = true;
    return true;
}

bool ObjectBatch::updateBlendStates(const ColorChannelBlendStates& blendStates) {
    if (!built || (hasCurrentBlendStates && currentBlendStates == blendStates))
        return true;

    const SortedObjectBatch* generated =
        workGroupScheduler.fetchSortedBatchForBlendStates(blendStates);
    if (!generated)
        return false; // Owner-style async generation; keep prior sort until ready.

    if (!applySortedBatch(*generated))
        return false;

    currentBlendStates = blendStates;
    hasCurrentBlendStates = true;
    return true;
}

bool ObjectBatch::build(
    const std::vector<GameObject*>& objects,
    GroupManager& groupManager,
    const ColorChannelBlendStates& blendStates,
    const TextureResolver& textureCallback
) {
    clear();
    resolveTexture = textureCallback;

    if (!buildObjectInfo(objects))
        return false;

    for (auto object : objects) {
        if (!object || object->isTrigger() || object->m_isHide ||
            object->m_objectID == GUIDE_OBJECT_ID)
            continue;
        if (!writeGameObject(object, groupManager)) {
            clear();
            return false;
        }
    }

    if (!finalizeSortedGeometry(blendStates)) {
        log::error("Bismuth iOS upstream batch could not finalize sorted geometry");
        clear();
        return false;
    }

    built = true;
    return true;
}

void ObjectBatch::refreshSpriteAppearances() {
    if (!built || spritePointers.size() != spriteAppearances.size())
        return;

    for (usize i = 0; i < spritePointers.size(); ++i) {
        const auto& source = spritePointers[i];
        const bool directTint =
            (inputSprites[i].colorChannelFlags & IOS_COLOR_CHANNEL_DIRECT_FLAG) != 0;
        spriteAppearances[i] = captureSpriteAppearance(source.sprite, directTint);
    }
}

} // namespace IOSUpstream

#endif
