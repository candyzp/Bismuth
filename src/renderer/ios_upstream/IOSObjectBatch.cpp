#ifdef GEODE_IS_IOS

#include "IOSObjectBatch.hpp"

#include <Geode/Enums.hpp>
#include <Geode/binding/RingObject.hpp>
#include <Geode/cocos/CCDirector.h>
#include <Geode/utils/cocos.hpp>
#include <SpriteMeshDictionary.hpp>
#include <math/ConvexList.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

using namespace geode::prelude;

namespace IOSUpstream {
namespace {

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

    // Match the owner's ObjectUtils::unpackSpriteRecursively(): only the
    // sprite's explicit getDontDraw() flag suppresses extraction. Do not use
    // CCNode::isVisible() or ancestor visibility here. On iOS the GPU owns
    // camera visibility, while the optimized PlayLayer path intentionally
    // skips vanilla's per-object CPU visibility walk; those Cocos visibility
    // flags can therefore be stale and must never cull GPU-owned geometry.
    if (sprite && !sprite->getDontDraw())
        result.flags |= IOS_SPRITE_APPEARANCE_VISIBLE;
    else
        result.packedDisplayedColor &= 0x00ffffffu;

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
        if (source.object)
            source.object->release();
    }
    objectInfos.clear();
    objectIndices.clear();
    sourceVertices.clear();
    sourceInputSprites.clear();
    sourceSpriteAppearances.clear();
    sourceSpritePointers.clear();
    orderedVertices.clear();
    usedLayerKeys.clear();
    sortedBatch = {};
    appliedBlendStates = {};
    hasAppliedBlendStates = false;
    currentSpriteIndex = 0;
    orderRevision = 0;
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

    constexpr usize MAX_EXACT_ES2_FLOAT_INDEX = 1u << 24;

    for (auto object : objects) {
        if (!object || object->isTrigger() || object->m_isHide ||
            object->m_objectID == IOS_GUIDE_OBJECT_ID)
            continue;

        if (objectInfos.size() >= MAX_EXACT_ES2_FLOAT_INDEX) {
            log::error("Bismuth iOS upstream batch exceeded the ES2 exact object-index range");
            return false;
        }

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
        scaleFactor / std::max(1u, texture->getPixelsWide()),
        scaleFactor / std::max(1u, texture->getPixelsHigh())
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
    vertex.objectAndGroupIndex = glm::vec2(
        static_cast<float>(currentObjectIndex),
        static_cast<float>(currentGroupCombinationIndex)
    );
    vertex.spriteAndMeta = glm::vec2(
        static_cast<float>(spriteIndex),
        static_cast<float>(
            currentSpriteType * 65536u + currentColorChannelFlags
        )
    );
    vertex.objectStartPosition = objectStartPosition;
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

    if (sourceInputSprites.size() >= (1u << 24)) {
        log::error("Bismuth iOS upstream batch exceeded the exact ES2 float sprite-index range");
        return false;
    }

    const SpriteSheet spriteSheet = ObjectUtils::getSpritesheetOfObject(object, type);
    const u32 objectIndex = getObjectIndex(object);
    const u32 groupCombinationIndex =
        groupManager.getGroupCombinationIndexForObject(object);

    // Object/group/sprite indices are carried as float vertex attributes in
    // ES2. Reject an index that cannot be represented exactly instead of
    // silently addressing another record in a large custom level.
    constexpr u32 MAX_EXACT_ES2_FLOAT_INDEX = 1u << 24;
    if (objectIndex >= MAX_EXACT_ES2_FLOAT_INDEX ||
        groupCombinationIndex >= MAX_EXACT_ES2_FLOAT_INDEX ||
        sourceInputSprites.size() >= MAX_EXACT_ES2_FLOAT_INDEX) {
        log::error("Bismuth iOS upstream batch exceeded an exact ES2 float metadata index");
        return false;
    }

    u32 colorChannelFlags = ObjectUtils::getSpriteColorChannel(type, object, sprite);
    if (type == SpriteType::DETAIL)
        colorChannelFlags |= IOS_COLOR_CHANNEL_DETAIL_FLAG;

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
    currentObjectIndex = objectIndex;
    currentGroupCombinationIndex = groupCombinationIndex;
    currentSpriteType = static_cast<u32>(type);
    currentColorChannelFlags = colorChannelFlags;
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

    // The vertex metadata above is mirrored in the source input record so
    // the culling/order stage can validate and inspect it.
    InputSprite input;
    input.objectIndex = objectIndex;
    input.spriteType = static_cast<u32>(type);
    input.indexStart = static_cast<u32>(firstTriangle);
    input.indexCount = triangleCount;
    input.colorChannelFlags = colorChannelFlags;
    sourceInputSprites.push_back(input);

    const bool directTint =
        (colorChannelFlags & IOS_COLOR_CHANNEL_DIRECT_FLAG) != 0;
    sourceSpriteAppearances.push_back(
        captureSpriteAppearance(sprite, directTint)
    );
    // The sprite may outlive its parent while the GPU cache is in use.
    // Retain both the leaf and its owning GameObject so refreshSpriteAppearances
    // never dereferences a destroyed orb/object root after gameplay activation.
    sprite->retain();
    object->retain();
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
    // Keep sprite/object/appearance records in their source order. Vertex
    // attributes carry those source indices, so only the geometry stream is
    // materialized in the scheduler's final layer order.
    if (newSortedBatch.inputSpriteIndices.size() != sourceInputSprites.size() ||
        newSortedBatch.preCulledSprites.size() != sourceInputSprites.size())
        return false;

    std::vector<u8> seen(sourceInputSprites.size(), 0);
    std::vector<ObjectVertex> newOrderedVertices;
    newOrderedVertices.reserve(sourceVertices.size());

    for (u32 sourceIndex : newSortedBatch.inputSpriteIndices) {
        if (sourceIndex >= sourceInputSprites.size() || seen[sourceIndex] != 0)
            return false;
        seen[sourceIndex] = 1;

        const auto& input = sourceInputSprites[sourceIndex];
        const usize firstVertex = static_cast<usize>(input.indexStart) * 3;
        const usize vertexCount = static_cast<usize>(input.indexCount) * 3;
        if (firstVertex > sourceVertices.size() ||
            vertexCount > sourceVertices.size() - firstVertex)
            return false;

        newOrderedVertices.insert(
            newOrderedVertices.end(),
            sourceVertices.begin() + firstVertex,
            sourceVertices.begin() + firstVertex + vertexCount
        );
    }

    if (newOrderedVertices.size() != sourceVertices.size())
        return false;

    u64 expectedFirstSprite = 0;
    u64 expectedFirstVertex = 0;
    u64 expectedFirstWorkGroup = 0;

    for (const auto& [key, range] : newSortedBatch.layerRanges) {
        const u64 endSprite =
            static_cast<u64>(range.firstInputSprite) + range.spriteCount;
        const u64 endVertex =
            static_cast<u64>(range.firstVertex) + range.vertexCount;
        const u64 endWorkGroup =
            static_cast<u64>(range.firstWorkGroup) + range.workGroupCount;

        if (range.key != key ||
            range.spriteCount == 0 || range.vertexCount == 0 ||
            range.workGroupCount == 0 || range.vertexCount % 3 != 0 ||
            range.firstInputSprite != expectedFirstSprite ||
            range.firstVertex != expectedFirstVertex ||
            range.firstWorkGroup != expectedFirstWorkGroup ||
            endSprite > newSortedBatch.inputSpriteIndices.size() ||
            endVertex > newOrderedVertices.size() ||
            endWorkGroup > newSortedBatch.workGroups.size())
            return false;

        u64 expectedRangeVertexCount = 0;
        for (u64 spriteOffset = 0; spriteOffset < range.spriteCount; ++spriteOffset) {
            const u32 sourceIndex =
                newSortedBatch.inputSpriteIndices[range.firstInputSprite + spriteOffset];
            expectedRangeVertexCount +=
                static_cast<u64>(sourceInputSprites[sourceIndex].indexCount) * 3;
        }
        if (expectedRangeVertexCount != range.vertexCount)
            return false;

        u64 spriteCursor = range.firstInputSprite;
        u64 vertexCursor = range.firstVertex;
        for (u64 workGroupIndex = range.firstWorkGroup;
             workGroupIndex < endWorkGroup;
             ++workGroupIndex) {
            const auto& workGroup =
                newSortedBatch.workGroups[static_cast<usize>(workGroupIndex)];
            if (workGroup.inputSpriteStart != spriteCursor ||
                workGroup.inputSpriteEnd <= workGroup.inputSpriteStart ||
                workGroup.inputSpriteEnd > endSprite ||
                static_cast<u64>(workGroup.culledIndexBufferOutputIndex) * 3 != vertexCursor)
                return false;

            for (u64 spriteIndex = workGroup.inputSpriteStart;
                 spriteIndex < workGroup.inputSpriteEnd;
                 ++spriteIndex) {
                const u32 sourceIndex =
                    newSortedBatch.inputSpriteIndices[static_cast<usize>(spriteIndex)];
                vertexCursor +=
                    static_cast<u64>(sourceInputSprites[sourceIndex].indexCount) * 3;
            }
            spriteCursor = workGroup.inputSpriteEnd;
        }

        if (spriteCursor != endSprite ||
            vertexCursor != endVertex)
            return false;

        expectedFirstSprite = endSprite;
        expectedFirstVertex = endVertex;
        expectedFirstWorkGroup = endWorkGroup;
    }

    if (expectedFirstSprite != newSortedBatch.inputSpriteIndices.size() ||
        expectedFirstVertex != newOrderedVertices.size() ||
        expectedFirstWorkGroup != newSortedBatch.workGroups.size())
        return false;

    // Commit atomically only after every range and source index has passed.
    // A malformed newly-generated blend order must not destroy the last valid
    // GPU batch while the game continues running.
    sortedBatch = newSortedBatch;
    orderedVertices = std::move(newOrderedVertices);
    ++orderRevision;
    return true;
}

bool ObjectBatch::finalizeSortedGeometry(const ColorChannelBlendStates& blendStates) {
    const SortedObjectBatch* generated =
        workGroupScheduler.waitForSortedBatch(blendStates);
    if (!generated)
        return false;

    if (!applySortedBatch(*generated))
        return false;

    appliedBlendStates = blendStates;
    hasAppliedBlendStates = true;
    return true;
}

bool ObjectBatch::refreshBlendOrder(const ColorChannelBlendStates& blendStates) {
    if (!built || !hasAppliedBlendStates)
        return false;

    // The owner requests the current blend-state variant every frame. The
    // scheduler builds unseen variants asynchronously; keep the last valid
    // draw order until the requested variant is ready, then swap atomically.
    if (blendStates == appliedBlendStates)
        return true;

    const SortedObjectBatch* generated =
        workGroupScheduler.fetchSortedBatchForBlendStates(blendStates);
    if (!generated)
        return true;

    if (!applySortedBatch(*generated)) {
        log::error("Bismuth iOS upstream batch rejected a refreshed blend-state schedule");
        return false;
    }

    appliedBlendStates = blendStates;
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
            object->m_objectID == IOS_GUIDE_OBJECT_ID)
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
    if (!built || sourceSpritePointers.size() != sourceSpriteAppearances.size() ||
        sourceInputSprites.size() != sourceSpriteAppearances.size())
        return;

    for (usize i = 0; i < sourceSpritePointers.size(); ++i) {
        const auto& source = sourceSpritePointers[i];
        const bool directTint =
            (sourceInputSprites[i].colorChannelFlags & IOS_COLOR_CHANNEL_DIRECT_FLAG) != 0;
        sourceSpriteAppearances[i] = captureSpriteAppearance(source.sprite, directTint);
    }
}

} // namespace IOSUpstream

#endif
