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

    // Collection is transactional. The public callback is only invoked after
    // the complete visual tree has been proven to consist entirely of sprites.
    // If one unsupported child exists anywhere, the whole object stays stock.
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

    bool shouldUnpackColorSprite = colorSprite && colorSprite->getParent() != object;
    bool isColorSpriteInFront    = object->m_colorZLayerRelated;

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

    // A visual sprite must map to one record for one object. Duplicate discovery
    // can otherwise create two GPU records for the same Cocos node and makes
    // all-or-nothing standalone suppression impossible to prove.
    std::unordered_set<cocos2d::CCSprite*> uniqueSprites;
    uniqueSprites.reserve(collected.size());
    for (const auto& unpacked : collected) {
        if (!unpacked.sprite || !uniqueSprites.insert(unpacked.sprite).second) {
            if (stats)
                ++stats->duplicateSprites;
            return false;
        }
    }

    for (const auto& unpacked : collected) {
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
    cocos2d::CCSprite* sprite,
    cocos2d::CCAffineTransform transform,
    ReceiveUnpackedSpriteFunc func,
    SpriteUnpackStats* stats,
    SpriteType type
) {
    if (!object || !sprite || !func)
        return false;

    transform = CCAffineTransformConcat(sprite->nodeToParentTransform(), transform);

    if (sprite == object->m_colorSprite) type = SpriteType::DETAIL;
    if (sprite == object->m_glowSprite)  type = SpriteType::GLOW;

    // CCNode::getChildren() is heterogeneous. Never reinterpret every child as
    // CCSprite: decoration-heavy objects can contain other CCNode subclasses.
    std::vector<cocos2d::CCSprite*> spriteChildren;
    if (auto children = sprite->getChildren()) {
        spriteChildren.reserve(children->count());
        for (auto childNode : CCArrayExt<cocos2d::CCNode*>(children)) {
            auto childSprite = typeinfo_cast<cocos2d::CCSprite*>(childNode);
            if (!childSprite) {
                if (stats)
                    ++stats->nonSpriteChildren;
                return false;
            }
            spriteChildren.push_back(childSprite);
        }
    }

    for (auto child : spriteChildren) {
        if (child->getZOrder() < 0 &&
            !unpackSpriteRecursively(object, child, transform, func, stats, type)) {
            return false;
        }
    }

    if (!sprite->getDontDraw())
        func({sprite, object, type, transform});

    for (auto child : spriteChildren) {
        if (child->getZOrder() >= 0 &&
            !unpackSpriteRecursively(object, child, transform, func, stats, type)) {
            return false;
        }
    }

    return true;
}