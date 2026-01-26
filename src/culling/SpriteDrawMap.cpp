#include "SpriteDrawMap.hpp"

using namespace geode::prelude;

OrderKey OrderKey::getOrderKeyOfSprite(GameObject* object, cocos2d::CCSprite* sprite, SpriteType type) {
    LayerKey key;
    if (type == SpriteType::GLOW)
        key = LayerKey::getFromGlowSpriteObject(object);
    else
        key = LayerKey::getFromObject(object, false);
    return {
        .layer = key,
        .zorder = object->getObjectZOrder(),
        .colorChannel = ObjectUtils::getSpriteColorChannel(type, object, sprite)
    };
}

void SpriteDrawMap::addSprite(
    GameObject* object,
    CCSprite* sprite,
    SpriteType type,
    const std::span<u32>& drawInfo
) {
    OrderKey key = OrderKey::getOrderKeyOfSprite(object, sprite, type);
    orderSets[key].addSprite(drawInfo);
}