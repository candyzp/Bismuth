#pragma once

#include <Geode/binding/GameObject.hpp>

namespace AreaVisualState {
    // Returns the final Area Fade opacity captured from Geometry Dash's own
    // setAreaOpacity path for the current visual frame. Objects without an
    // active Area Fade contribution use the neutral multiplier 1.0.
    float getFadeOpacity(GameObject* object);

    // The renderer consumes captured Area Fade state once per frame after GD
    // has processed area visual actions.
    void clearFadeFrame();
}
