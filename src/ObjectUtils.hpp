#pragma once

#include "Geode/cocos/cocoa/CCAffineTransform.h"
#include "Geode/cocos/sprite_nodes/CCSprite.h"
#include <Geode/binding/GameObject.hpp>
#include <common.hpp>
#include <functional>
#include "../../resources/shaders/shared.h"

enum class SpriteType {
    BASE,
    DETAIL,
    BLACK,
    GLOW
};

enum class SpriteSheet {
    GAME_1,
    GAME_2,
    TEXT,
    FIRE,
    SPECIAL,
    GLOW,
    PIXEL,
    _UNK,
    PARTICLE,

    COUNT
};

#define LAYER_KEY_BLENDING 0x80

struct LayerKey {
    ZLayer zlayer;
    SpriteSheet spriteSheet;
    bool blending;

    inline bool operator==(const LayerKey& id) const {
        return zlayer == id.zlayer && spriteSheet == id.spriteSheet && blending == id.blending;
    }

    inline bool operator!=(const LayerKey& id) const { return !(*this == id); }

    inline bool operator<(const LayerKey& id) const {
        if (zlayer != id.zlayer) return zlayer < id.zlayer;
        if (spriteSheet != id.spriteSheet) return spriteSheet < id.spriteSheet;
        return blending < id.blending;
    }

    u8 asNumber() const;

    static inline ZLayer getNormalObjectZLayer(GameObject* object) {
        ZLayer zLayer = object->getObjectZLayer();
        if ((i32)zLayer % 2 == 0) {
            zLayer = (ZLayer)((i32)zLayer - 1);
            if (zLayer < ZLayer::B5)
                zLayer = ZLayer::B5;
        }
        return zLayer;
    }

    static inline LayerKey getFromObject(GameObject* object, bool blending) {
        return { getNormalObjectZLayer(object), (SpriteSheet)object->getParentMode(), blending };
    }

    static inline LayerKey getFromGlowSpriteObject(GameObject* object) {
        return { getNormalObjectZLayer(object), SpriteSheet::GLOW, true };
    }
};

struct UnpackedSprite {
    cocos2d::CCSprite* sprite;
    GameObject* parentObject;
    SpriteType type;
    const cocos2d::CCAffineTransform& transform;
};

using ReceiveUnpackedSpriteFunc = std::function<void(const UnpackedSprite&)>;

class ObjectUtils {
public:
    static inline SpriteSheet getSpritesheetOfObject(GameObject* object, SpriteType type) {
        return type == SpriteType::GLOW ? SpriteSheet::GLOW : (SpriteSheet)object->getParentMode();
    }

    static inline u32 sanitizeColorChannel(i32 colorChannel) {
        // 0/negative IDs mean no normal color channel for a number of special
        // objects. On iOS these are texture lookups, so allowing a negative ID
        // to wrap to u16/u32 samples an unrelated edge texel (often a wild
        // solid color). White is the neutral tint for those fallback sprites.
        if (colorChannel <= 0 || colorChannel >= COLOR_CHANNEL_COUNT)
            return COLOR_CHANNEL_WHITE;
        return (u32)colorChannel;
    }

    static u32 getSpriteColorChannel(SpriteType type, GameObject* object, cocos2d::CCSprite* sprite) {
        i32 rawColorChannel = type == SpriteType::DETAIL
            ? object->m_activeDetailColorID
            : object->m_activeMainColorID;
        u32 colorChannel = sanitizeColorChannel(rawColorChannel);

        bool isSpriteBlack = (sprite == object) ? object->m_isObjectBlack : object->m_isColorSpriteBlack;
        if (isSpriteBlack)
            return COLOR_CHANNEL_BLACK;
        if (type == SpriteType::GLOW && object->m_glowColorIsLBG)
            return COLOR_CHANNEL_LBG;
        return colorChannel;
    }

    /*
        This function unpacks GameObject and turns them into
        individual sprites and calls func for every sprite
        in draw order.
    */
    static void unpackObjectIntoSprites(GameObject* object, ReceiveUnpackedSpriteFunc func);

private:
    static void unpackSpriteRecursively(
        GameObject* object,
        cocos2d::CCSprite* sprite,
        cocos2d::CCAffineTransform transform,
        ReceiveUnpackedSpriteFunc func,
        SpriteType type = SpriteType::BASE
    );
};