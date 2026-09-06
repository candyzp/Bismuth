#ifdef GEODE_IS_IOS

#include "../Renderer.hpp"
#include "GPUTruth.hpp"
#include <Geode/modify/CCDirector.hpp>

using namespace geode::prelude;

class $modify(BismuthFrameVisualSync, cocos2d::CCDirector) {
    void drawScene() {
        if (auto renderer = Renderer::get()) {
            GPUTruth::beginFrame(renderer.data());
            renderer->beginGPUFrame();
        } else {
            GPUTruth::beginFrame(nullptr);
        }

        cocos2d::CCDirector::drawScene();

        // Reacquire: stock drawScene can replace the running scene. Counters
        // describe the completed render, even when physics updated several times.
        if (auto renderer = Renderer::get()) {
            renderer->finishGPUFrame();
            // Update the diagnostic UI only after the scene traversal has ended,
            // so creating/updating labels cannot mutate the child list mid-draw.
            GPUTruth::finishFrame(renderer.data());
        }
    }
};

#endif
