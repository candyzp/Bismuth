#pragma once

#ifdef GEODE_IS_IOS
#include <Geode/binding/GJGroundLayer.hpp>

namespace GroundOwnership {
enum class Part { Other, Ground1, Ground2, Line };

inline GJGroundLayer* owner(cocos2d::CCNode* node) {
    for (auto parent = node ? node->getParent() : nullptr; parent; parent = parent->getParent()) {
        if (auto ground = geode::prelude::typeinfo_cast<GJGroundLayer*>(parent))
            return ground;
    }
    return nullptr;
}

inline Part part(GJGroundLayer* ground, cocos2d::CCNode* node) {
    if (!ground)
        return Part::Other;
    // G1/G2 are scrolling containers. The textured tiles below them, rather
    // than just the container pointers, provide the actual floor draw proof.
    for (; node && node != ground; node = node->getParent()) {
        if (node == ground->m_ground1Sprite) return Part::Ground1;
        if (node == ground->m_ground2Sprite) return Part::Ground2;
        if (node == ground->m_lineSprite) return Part::Line;
    }
    return Part::Other;
}
} // namespace GroundOwnership
#endif
