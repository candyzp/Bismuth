#ifdef GEODE_IS_IOS

#include "../Renderer.hpp"
#include <Geode/modify/CCSprite.hpp>

using namespace geode::prelude;

class $modify(RendererStandaloneOwnedCCSprite, cocos2d::CCSprite) {
    void visit() {
        auto renderer = Renderer::get();
        if (renderer && renderer->isGPUOwnedStandaloneSprite(this)) {
            // This returns true only for a GameObject root whose COMPLETE safe
            // visual subtree was accepted by a standalone GPU batch. GD already
            // ran gameplay/state/animation updates; skipping the root visit now
            // removes the stock subtree render traversal without touching those
            // lifecycle systems.
            return;
        }

        cocos2d::CCSprite::visit();
    }
};

#endif
