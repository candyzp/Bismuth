#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "MetalDiagnosticsOverlay.hpp"

using namespace geode::prelude;

class $modify(KyaniteIOSDiagnosticsPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        auto overlay = MetalDiagnosticsOverlay::create();
        if (!overlay)
            return;

        overlay->setID("iliashdz.kyanite/ios-metal-diagnostics");
        addChild(overlay, 100000);
        log::info("Kyanite iOS diagnostics overlay attached; stock rendering remains enabled.");
    }
};
