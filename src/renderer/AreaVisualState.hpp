#pragma once

#include <Geode/binding/GameObject.hpp>

namespace AreaVisualState {
    // Returns the final Area Fade opacity captured from Geometry Dash's own
    // setAreaOpacity path. Objects without a captured Area Fade contribution
    // use the neutral multiplier 1.0.
    float getFadeOpacity(GameObject* object);

    // Area opacity is persistent gameplay state (it is checkpoint-restored by
    // GD), so keep captured values across frames and clear them only when a new
    // renderer/level begins or the current one is torn down.
    void reset();
}
