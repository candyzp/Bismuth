#ifdef GEODE_IS_IOS

#include "../Renderer.hpp"
#include <Geode/modify/CCSprite.hpp>

using namespace geode::prelude;

class $modify(RendererStandaloneOwnedCCSprite, cocos2d::CCSprite) {
    void visit() {
        auto renderer = Renderer::get();
        if (renderer && renderer->isGPUOwnedStandaloneSprite(this)) {
            // Standalone ownership is only granted to leaf sprites. Their GD
            // state/actions were already updated; skipping visit removes Cocos'
            // per-sprite matrix setup, shader setup and draw call while the GPU
            // sibling run draws the same resolved sprite in the same parent slot.
            return;
        }

        cocos2d::CCSprite::visit();
    }
};

#endif
