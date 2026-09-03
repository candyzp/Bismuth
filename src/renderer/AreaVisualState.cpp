#include "AreaVisualState.hpp"

#ifdef GEODE_IS_IOS

#include <Geode/modify/GameObject.hpp>
#include <algorithm>
#include <unordered_map>

namespace {
    std::unordered_map<GameObject*, float> g_areaFadeOpacity;
}

namespace AreaVisualState {
    float getFadeOpacity(GameObject* object) {
        if (!object)
            return 1.f;
        auto it = g_areaFadeOpacity.find(object);
        return it == g_areaFadeOpacity.end() ? 1.f : it->second;
    }

    void clearFadeFrame() {
        g_areaFadeOpacity.clear();
    }
}

class $modify(BismuthAreaFadeGameObject, GameObject) {
    void setAreaOpacity(float opacity, float strength, int commandIndex) {
        // Let Geometry Dash resolve Area Fade priority/strength first. The
        // dedicated m_areaOpacityValue is authoritative after this call and is
        // independent from the stock Cocos visibility opacity that Bismuth skips.
        GameObject::setAreaOpacity(opacity, strength, commandIndex);
        g_areaFadeOpacity[this] = std::clamp(m_areaOpacityValue, 0.f, 1.f);
    }
};

#else

namespace AreaVisualState {
    float getFadeOpacity(GameObject*) {
        return 1.f;
    }

    void clearFadeFrame() {}
}

#endif
