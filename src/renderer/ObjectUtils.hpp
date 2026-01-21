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

struct LayerIdentifier {
    ZLayer zlayer;
    SpriteSheet spriteSheet;
    bool blending;

    inline bool operator==(const LayerIdentifier& id) const {
        return zlayer == id.zlayer && spriteSheet == id.spriteSheet && blending == id.blending;
    }

    inline bool operator!=(const LayerIdentifier& id) const { return !(*this == id); }

    static inline ZLayer getNormalObjectZLayer(GameObject* object) {
        ZLayer zLayer = object->getObjectZLayer();
        if ((i32)zLayer % 2 == 0) {
            zLayer = (ZLayer)((i32)zLayer - 1);
            if (zLayer < ZLayer::B5)
                zLayer = ZLayer::B5;
        }
        return zLayer;
    }

    static inline LayerIdentifier getFromObject(GameObject* object, bool blending) {
        return { getNormalObjectZLayer(object), (SpriteSheet)object->getParentMode(), blending };
    }

    static inline LayerIdentifier getFromGlowSpriteObject(GameObject* object) {
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

    static u32 getSpriteColorChannel(SpriteType type, GameObject* object, cocos2d::CCSprite* sprite) {
        u32 colorChannel = type == SpriteType::DETAIL ? object->m_activeDetailColorID : object->m_activeMainColorID;

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