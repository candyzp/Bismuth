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

    static inline bool isInteractiveVisualObject(GameObject* object) {
        if (!object)
            return false;

        switch (object->m_objectType) {
            case GameObjectType::InverseGravityPortal:
            case GameObjectType::NormalGravityPortal:
            case GameObjectType::ShipPortal:
            case GameObjectType::CubePortal:
            case GameObjectType::YellowJumpPad:
            case GameObjectType::PinkJumpPad:
            case GameObjectType::GravityPad:
            case GameObjectType::YellowJumpRing:
            case GameObjectType::PinkJumpRing:
            case GameObjectType::GravityRing:
            case GameObjectType::InverseMirrorPortal:
            case GameObjectType::NormalMirrorPortal:
            case GameObjectType::BallPortal:
            case GameObjectType::RegularSizePortal:
            case GameObjectType::MiniSizePortal:
            case GameObjectType::UfoPortal:
            case GameObjectType::SecretCoin:
            case GameObjectType::DualPortal:
            case GameObjectType::SoloPortal:
            case GameObjectType::WavePortal:
            case GameObjectType::RobotPortal:
            case GameObjectType::TeleportPortal:
            case GameObjectType::GreenRing:
            case GameObjectType::Collectible:
            case GameObjectType::UserCoin:
            case GameObjectType::DropRing:
            case GameObjectType::SpiderPortal:
            case GameObjectType::RedJumpPad:
            case GameObjectType::RedJumpRing:
            case GameObjectType::CustomRing:
            case GameObjectType::DashRing:
            case GameObjectType::GravityDashRing:
            case GameObjectType::SwingPortal:
            case GameObjectType::GravityTogglePortal:
            case GameObjectType::SpiderOrb:
            case GameObjectType::SpiderPad:
            case GameObjectType::TeleportOrb:
                return true;
            default:
                return false;
        }
    }

    static inline u32 sanitizeColorChannel(i32 colorChannel) {
        if (colorChannel < 0 || colorChannel >= COLOR_CHANNEL_COUNT)
            return COLOR_CHANNEL_WHITE;
        return (u32)colorChannel;
    }

    static u32 getSpriteColorChannel(SpriteType type, GameObject* object, cocos2d::CCSprite* sprite) {
        const bool detail = type == SpriteType::DETAIL;
        i32 rawColorChannel = detail
            ? object->m_activeDetailColorID
            : object->m_activeMainColorID;

        // Geometry Dash uses active color ID 0 as "use this sprite color's
        // authored/default channel" rather than as an ordinary color-table
        // lookup. The optimized renderer skipped that distinction and could
        // therefore turn stock level art white or otherwise sample the wrong
        // slot. Resolve the stored default before sanitizing for the GPU table.
        auto spriteColor = detail ? object->m_detailColor : object->m_baseColor;
        if ((rawColorChannel <= 0 || rawColorChannel >= COLOR_CHANNEL_COUNT) && spriteColor) {
            const i32 defaultColor = spriteColor->m_defaultColorID;
            if (defaultColor > 0 && defaultColor < COLOR_CHANNEL_COUNT)
                rawColorChannel = defaultColor;
        }

        u32 colorChannel = sanitizeColorChannel(rawColorChannel);

        // SpriteType already tracks whether a sprite belongs to the base/detail
        // subtree. Using "root vs child" here incorrectly treated every layered
        // base child (coins/orbs included) as a detail sprite.
        bool isSpriteBlack = false;
        if (type == SpriteType::DETAIL)
            isSpriteBlack = object->m_isColorSpriteBlack;
        else if (type == SpriteType::BASE || type == SpriteType::BLACK)
            isSpriteBlack = object->m_isObjectBlack;

        if (isSpriteBlack || type == SpriteType::BLACK)
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