#ifdef GEODE_IS_IOS

#include "ResolvedStateLayer.hpp"
#include "../Renderer.hpp"
#include <Geode/modify/CCDirector.hpp>

using namespace geode::prelude;

class $modify(BismuthFrameVisualSync, cocos2d::CCDirector) {
    void drawScene() {
        auto renderer = Renderer::get();
        if (renderer && renderer->isEnabled()) {
            if (auto resolved = ResolvedStateLayer::getCurrent())
                resolved->update(true);
        }

        cocos2d::CCDirector::drawScene();
    }
};

#endif
