#include "ObjectUtils.hpp"
#include "Geode/cocos/cocoa/CCAffineTransform.h"

using namespace geode::prelude;

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

void ObjectUtils::unpackObjectIntoSprites(GameObject* object, ReceiveUnpackedSpriteFunc func) {
    CCSprite* colorSprite = object->m_colorSprite;

    bool shouldUnpackColorSprite = colorSprite && colorSprite->getParent() != object;
    bool isColorSpriteInFront    = object->m_colorZLayerRelated;

    auto transform = CCAffineTransformMakeIdentity();

    if (object->m_glowSprite)
        unpackSpriteRecursively(object, object->m_glowSprite, transform, func);

    if (shouldUnpackColorSprite && !isColorSpriteInFront)
        unpackSpriteRecursively(object, colorSprite, transform, func);

    unpackSpriteRecursively(object, object, transform, func);

    if (shouldUnpackColorSprite && isColorSpriteInFront)
        unpackSpriteRecursively(object, colorSprite, transform, func);
}

void ObjectUtils::unpackSpriteRecursively(
    GameObject* object,
    cocos2d::CCSprite* sprite,
    cocos2d::CCAffineTransform transform,
    ReceiveUnpackedSpriteFunc func,
    SpriteType type
) {
    transform = CCAffineTransformConcat(sprite->nodeToParentTransform(), transform);

    if (sprite == object->m_colorSprite) type = SpriteType::DETAIL;
    if (sprite == object->m_glowSprite)  type = SpriteType::GLOW;

    CCArrayExt<CCSprite*> spriteChildren = sprite->getChildren();

    for (auto child : spriteChildren) {
        if (child->getZOrder() < 0)
            unpackSpriteRecursively(object, child, transform, func, type);
    }
    
    if (!sprite->getDontDraw())
        func({sprite, object, type, transform});

    for (auto child : spriteChildren) {
        if (child->getZOrder() >= 0)
            unpackSpriteRecursively(object, child, transform, func, type);
    }
}