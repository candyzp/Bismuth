#include "MetalDiagnosticsOverlay.hpp"
#include "MetalBackend.hpp"

#include <Geode/cocos/CCDirector.h>
#include <algorithm>

using namespace geode::prelude;

MetalDiagnosticsOverlay* MetalDiagnosticsOverlay::create() {
    auto ret = new MetalDiagnosticsOverlay();
    if (ret && ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool MetalDiagnosticsOverlay::init() {
    if (!CCNode::init())
        return false;

    const auto winSize = CCDirector::get()->getWinSize();
    setAnchorPoint({0, 1});
    setPosition({8.0f, winSize.height - 8.0f});
    setZOrder(100000);

    m_background = CCLayerColor::create({0, 0, 0, 190});
    if (!m_background)
        return false;
    m_background->setAnchorPoint({0, 1});
    addChild(m_background, 0);

    m_label = CCLabelBMFont::create("", "chatFont.fnt");
    if (!m_label)
        return false;
    m_label->setAnchorPoint({0, 1});
    m_label->setPosition({6.0f, -5.0f});
    m_label->setScale(0.42f);
    addChild(m_label, 1);

    scheduleUpdate();
    refreshText();
    return true;
}

void MetalDiagnosticsOverlay::update(float dt) {
    m_refreshElapsed += dt;
    if (m_refreshElapsed < 0.25f)
        return;

    m_refreshElapsed = 0.0f;
    const bool visible =
        Mod::get()->getSettingValue<bool>("diagnostics_overlay");
    setVisible(visible);

    if (!visible)
        return;

    if (!m_probeRequested &&
        Mod::get()->getSettingValue<bool>("enabled") &&
        Mod::get()->getSettingValue<bool>("run_metal_probe")) {
        m_probeRequested = true;
        auto& backend = MetalBackend::get();
        backend.initialize();
        backend.submitComputeProbeOnce();
    }

    refreshText();
}

void MetalDiagnosticsOverlay::refreshText() {
    const bool enabled = Mod::get()->getSettingValue<bool>("enabled");
    const bool runProbe = Mod::get()->getSettingValue<bool>("run_metal_probe");
    const auto state = MetalBackend::get().snapshot();

    std::string text;
    text += "KYANITE / iOS METAL PORT\n";

    if (!enabled) {
        text += "Metal diagnostics: DISABLED IN SETTINGS\n";
    } else if (!runProbe) {
        text += "Metal compute self-test: DISABLED IN SETTINGS\n";
    } else {
        text += fmt::format(
            "Metal device: {}\n",
            state.deviceReady ? "READY" : "NOT INITIALIZED"
        );
        if (!state.deviceName.empty())
            text += fmt::format("GPU: {}\n", state.deviceName);

        text += fmt::format(
            "Command queue: {}\nCompute pipeline: {}\n",
            state.commandQueueReady ? "READY" : "NOT READY",
            state.computePipelineReady ? "READY" : "NOT READY"
        );

        if (state.probeCompleted) {
            text += fmt::format(
                "GPU self-test: COMPLETE ({}/{})\n",
                state.completedProbeThreads,
                state.expectedProbeThreads
            );
            if (state.completedProbeThreads != state.expectedProbeThreads)
                text += "GPU self-test result: MISMATCH\n";
            if (state.gpuTimeAvailable) {
                text += fmt::format(
                    "Probe GPU time: {:.3f} ms\n",
                    state.gpuTimeMilliseconds
                );
            } else {
                text += "Probe GPU time: unavailable\n";
            }
        } else if (state.probeSubmitted) {
            text += "GPU self-test: SUBMITTED / WAITING\n";
        } else if (state.probeFailed) {
            text += "GPU self-test: FAILED\n";
        } else {
            text += "GPU self-test: NOT SUBMITTED\n";
        }
    }

    if (!state.error.empty())
        text += fmt::format("Metal error: {}\n", state.error);

    // Do not label the probe as level rendering. The Metal object renderer
    // is not connected to Geometry Dash's draw order at this stage.
    text += "Level object buffers: NOT CONNECTED\n";
    text += "Culling / object draws: NOT RUN\n";
    text += "Active level renderer: STOCK GD (SAFE FALLBACK)\n";

    m_label->setString(text.c_str());
    const auto labelSize = m_label->getContentSize();
    const float labelScale = m_label->getScale();
    m_background->setContentSize({
        labelSize.width * labelScale + 12.0f,
        labelSize.height * labelScale + 10.0f
    });
}
