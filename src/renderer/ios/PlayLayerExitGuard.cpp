#ifdef GEODE_IS_IOS

#include "../Renderer.hpp"
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

// Stop the iOS GPU assist path before PlayLayer begins tearing down its
// CCSpriteBatchNode arrays. The exit crash at 9d37fd4 came from a late
// CCSpriteBatchNode::draw() calling AtlasInterleaveRegistry::ownsBatch() after
// PlayLayer's m_batchNodes storage had already entered teardown.
class $modify(RendererExitGuardPlayLayer, PlayLayer) {
    void onEnterTransitionDidFinish() {
        PlayLayer::onEnterTransitionDidFinish();
        if (auto renderer = Renderer::forPlayLayer(this))
            renderer->resumeGPU();
    }

    void onExit() {
        auto renderer = Renderer::forPlayLayer(this);
        if (renderer) {
            // Restore stock atlas quads while the PlayLayer and its batches are
            // still alive. Once enabled is false, all later sprite/batch hooks
            // immediately use the stock Cocos path during scene destruction.
            renderer->suspendGPU();
        }

        PlayLayer::onExit();
    }
};

// Opening GD's pause layer can drive the PlayLayer through the same exit guard,
// but resuming gameplay does not guarantee another PlayLayer enter-transition.
// Keep a strong reference across stock onResume(), then reactivate the exact
// renderer for the still-live PlayLayer after GD has restored its pause state.
// resumeGPU() is intentionally a no-op unless that renderer was suspended.
class $modify(RendererResumePauseLayer, PauseLayer) {
    void onResume(cocos2d::CCObject* sender) {
        auto renderer = Renderer::forPlayLayer(PlayLayer::get());
        PauseLayer::onResume(sender);
        if (renderer)
            renderer->resumeGPU();
    }
};

#endif
