#ifdef GEODE_IS_IOS

#include "../Renderer.hpp"
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

// Stop the iOS GPU assist path before PlayLayer begins tearing down its
// CCSpriteBatchNode arrays. The exit crash at 9d37fd4 came from a late
// CCSpriteBatchNode::draw() calling AtlasInterleaveRegistry::ownsBatch() after
// PlayLayer's m_batchNodes storage had already entered teardown.
class $modify(RendererExitGuardPlayLayer, PlayLayer) {
    void onExit() {
        auto renderer = Renderer::get();
        if (renderer && renderer->getPlayLayer() == this && renderer->isEnabled()) {
            // Restore stock atlas quads while the PlayLayer and its batches are
            // still alive. Once enabled is false, all later sprite/batch hooks
            // immediately use the stock Cocos path during scene destruction.
            renderer->setEnabled(false);
        }

        PlayLayer::onExit();
    }
};

#endif
