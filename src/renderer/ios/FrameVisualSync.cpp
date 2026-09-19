#ifdef GEODE_IS_IOS

#include "../Renderer.hpp"
#include "GPUTruth.hpp"
#include "ResolvedStateLayer.hpp"
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/modify/CCDirector.hpp>

using namespace geode::prelude;

class $modify(BismuthFrameVisualSync, cocos2d::CCDirector) {
    void drawScene() {
        // Recovery net for missed pause/transition callbacks. Never wake the GPU
        // while stock GD says the PlayLayer is paused.
        if (auto playLayer = PlayLayer::get(); playLayer && !playLayer->m_isPaused) {
            auto exact = Renderer::forPlayLayer(playLayer);
            auto active = Renderer::get();
            if (exact && (!active || active.data() != exact.data()))
                exact->resumeGPU();
        }

        if (auto renderer = Renderer::get()) {
            GPUTruth::beginFrame(renderer.data());
            renderer->beginGPUFrame();
            // Geometry ownership may be queried several times while one live
            // atlas is replanned. Start one validation epoch for this rendered
            // frame so repeated safety checks reuse the first exact result.
            if (auto resolved = ResolvedStateLayer::getCurrent())
                resolved->beginFrameValidation();
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
