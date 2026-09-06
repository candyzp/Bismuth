#ifdef GEODE_IS_IOS

#include "../Renderer.hpp"
#include <Geode/modify/CCDirector.hpp>

using namespace geode::prelude;

class $modify(BismuthFrameVisualSync, cocos2d::CCDirector) {
    void drawScene() {
        if (auto renderer = Renderer::get())
            renderer->beginGPUFrame();

        cocos2d::CCDirector::drawScene();

        // Reacquire: stock drawScene can replace the running scene. Counters
        // describe the completed render, even when physics updated several times.
        if (auto renderer = Renderer::get())
            renderer->finishGPUFrame();
    }
};

#endif
