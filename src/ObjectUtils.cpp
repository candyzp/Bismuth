#include "ObjectUtils.hpp"
#include "Geode/cocos/cocoa/CCAffineTransform.h"

#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {
struct CollectedSprite {
    cocos2d::CCSprite* sprite = nullptr;
    GameObject* parentObject = nullptr;
    SpriteType type = SpriteType::BASE;
    cocos2d::CCAffineTransform transform = cocos2d::CCAffineTransformMakeIdentity();
};
} // namespace

u8 LayerKey::asNumber() const {
    u8 zlayerKey = 0;
    switch (zlayer) {
    case ZLayer::B5:      zlayerKey = 0; break;
    case ZLayer::B4:      zlayerKey = 1; break;
    case ZLayer::B3:      zlayerKey = 2; break;
    case ZLayer::B2:      zlayerKey = 3; break;
    case ZLayer::B1:      zlayerKey = 4; break;
    default:
    case ZLayer::Default: zlayerKey = 5; break;
    case ZLayer::T1:      zlayerKey = 6; break;
    case ZLayer::T2:      zlayerKey = 7; break;
    case ZLayer::T3:      zlayerKey = 8; break;
    case ZLayer::T4:      zlayerKey = 9; break;
    }

    u8 spriteSheetKey = 0;
    switch (spriteSheet) {
    default:
    case SpriteSheet::GAME_1:   spriteSheetKey = 0; break;
    case SpriteSheet::GAME_2:   spriteSheetKey = 1; break;
    case SpriteSheet::TEXT:     spriteSheetKey = 2; break;
    case SpriteSheet::FIRE:     spriteSheetKey = 3; break;
    case SpriteSheet::SPECIAL:  spriteSheetKey = 4; break;
    case SpriteSheet::GLOW:     spriteSheetKey = 5; break;
    case SpriteSheet::PIXEL:    spriteSheetKey = 6; break;
    case SpriteSheet::PARTICLE: spriteSheetKey = 7; break;
    }

    return zlayerKey | (spriteSheetKey << 4) | (blending ? LAYER_KEY_BLENDING : 0);
}

bool ObjectUtils::unpackObjectIntoSprites(
    GameObject* object,
    ReceiveUnpackedSpriteFunc func,
    SpriteUnpackStats* stats
) {
    if (!object || !func)
        return false;

    // Collect first, then publish the completed sprite stream. Cocos visual
    // trees are heterogeneous: non-sprite container nodes still carry useful
    // transforms and may contain sprite descendants.
    std::vector<CollectedSprite> collected;
    collected.reserve(8);

    auto collect = [&](const UnpackedSprite& unpacked) {
        collected.push_back({
            unpacked.sprite,
            unpacked.parentObject,
            unpacked.type,
            unpacked.transform
        });
    };

    CCSprite* colorSprite = object->m_colorSprite;
    const bool shouldUnpackColorSprite =
        colorSprite && colorSprite->getParent() != object;
    const bool isColorSpriteInFront = object->m_colorZLayerRelated;
    auto transform = CCAffineTransformMakeIdentity();

    if (object->m_glowSprite &&
        !unpackSpriteRecursively(object, object->m_glowSprite, transform, collect, stats)) {
        return false;
    }

    if (shouldUnpackColorSprite && !isColorSpriteInFront &&
        !unpackSpriteRecursively(object, colorSprite, transform, collect, stats)) {
        return false;
    }

    if (!unpackSpriteRecursively(object, object, transform, collect, stats))
        return false;

    if (shouldUnpackColorSprite && isColorSpriteInFront &&
        !unpackSpriteRecursively(object, colorSprite, transform, collect, stats)) {
        return false;
    }

    // The owner-style traversal can discover the same glow/color sprite once
    // via its dedicated pointer and once through the root child tree. Keep its
    // first (owner-defined draw-order) occurrence instead of rejecting the
    // entire GameObject or producing duplicate GPU geometry.
    std::unordered_set<cocos2d::CCSprite*> uniqueSprites;
    uniqueSprites.reserve(collected.size());
    std::vector<CollectedSprite> uniqueCollected;
    uniqueCollected.reserve(collected.size());
    for (const auto& unpacked : collected) {
        if (!unpacked.sprite)
            return false;
        if (!uniqueSprites.insert(unpacked.sprite).second) {
            if (stats)
                ++stats->duplicateSprites;
            continue;
        }
        uniqueCollected.push_back(unpacked);
    }

    for (const auto& unpacked : uniqueCollected) {
        func({
            unpacked.sprite,
            unpacked.parentObject,
            unpacked.type,
            unpacked.transform
        });
    }

    return true;
}

bool ObjectUtils::unpackSpriteRecursively(
    GameObject* object,
    cocos2d::CCNode* node,
    cocos2d::CCAffineTransform transform,
    ReceiveUnpackedSpriteFunc func,
    SpriteUnpackStats* stats,
    SpriteType type
) {
    if (!object || !node || !func)
        return false;

    transform = CCAffineTransformConcat(node->nodeToParentTransform(), transform);

    auto sprite = typeinfo_cast<cocos2d::CCSprite*>(node);
    SpriteType spriteType = type;
    if (sprite && sprite == object->m_colorSprite)
        spriteType = SpriteType::DETAIL;
    if (sprite && sprite == object->m_glowSprite)
        spriteType = SpriteType::GLOW;

    std::vector<cocos2d::CCNode*> nodeChildren;
    if (auto children = node->getChildren()) {
        nodeChildren.reserve(children->count());
        for (auto child : CCArrayExt<cocos2d::CCNode*>(children)) {
            if (!child)
                continue;
            if (!typeinfo_cast<cocos2d::CCSprite*>(child) && stats)
                ++stats->nonSpriteChildren;
            nodeChildren.push_back(child);
        }
    }

    for (auto child : nodeChildren) {
        if (child->getZOrder() < 0 &&
            !unpackSpriteRecursively(object, child, transform, func, stats, spriteType)) {
            return false;
        }
    }

    if (sprite) {
        bool isSpriteBlack = false;
        if (sprite == object)
            isSpriteBlack = object->m_isObjectBlack;
        else if (spriteType != SpriteType::GLOW)
            isSpriteBlack = object->m_isColorSpriteBlack;
        if (sprite->getTag() == 997)
            isSpriteBlack = false;

        const SpriteType outputType =
            isSpriteBlack ? SpriteType::BLACK : spriteType;
        if (!sprite->getDontDraw())
            func({sprite, object, outputType, transform});
    }

    for (auto child : nodeChildren) {
        if (child->getZOrder() >= 0 &&
            !unpackSpriteRecursively(object, child, transform, func, stats, spriteType)) {
            return false;
        }
    }

    return true;
}
