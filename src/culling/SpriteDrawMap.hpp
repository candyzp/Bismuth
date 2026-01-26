#pragma once

#include <ObjectUtils.hpp>
#include <map>

struct OrderKey {
    LayerKey layer;

    i32 zorder;
    u32 colorChannel;

    inline bool operator<(const OrderKey& o) const {
        if (layer != o.layer) return layer < o.layer;
        if (colorChannel != o.colorChannel) return colorChannel < o.colorChannel;
        return zorder < o.zorder;
    }

    static OrderKey getOrderKeyOfSprite(GameObject* object, cocos2d::CCSprite* sprite, SpriteType type);
};

class SpriteDrawOrderSet {
public:
    inline void addSprite(const std::span<u32>& drawInfo) {
        usize oldSize = spriteDrawInfo.size();
        spriteDrawInfo.resize(spriteDrawInfo.size() + drawInfo.size());
        std::copy(drawInfo.begin(), drawInfo.end(), spriteDrawInfo.begin() + oldSize);
    }

private:
    std::vector<u32> spriteDrawInfo;
};

class SpriteDrawMap {
public:
    void addSprite(
        GameObject* object,
        cocos2d::CCSprite* sprite,
        SpriteType type,
        const std::span<u32>& drawInfo
    );

    inline usize orderSetsCount() const { return orderSets.size(); }

private:
    std::map<OrderKey, SpriteDrawOrderSet> orderSets;
};