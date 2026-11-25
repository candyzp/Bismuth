#include "MetalDiagnosticsOverlay.hpp"
#include "MetalBackend.hpp"

#include <Geode/Enums.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/cocos/CCDirector.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace geode::prelude;

namespace {
constexpr float DEG_TO_RAD = 0.01745329251994329577f;
constexpr std::uint32_t MAX_GROUP_ID = 9999;

bool shouldIncludeObject(PlayLayer* layer, GameObject* object) {
    if (!layer || !object)
        return false;
    if (object == layer->m_anticheatSpike || object == layer->m_endPortal)
        return false;
    if (object->isTrigger() || object->m_isHide)
        return false;
    if (object->m_objectID == GUIDE_OBJECT_ID ||
        object->m_objectID == KEYFRAME_OBJECT_ID)
        return false;
    return true;
}
} // namespace

MetalDiagnosticsOverlay* MetalDiagnosticsOverlay::create(PlayLayer* layer) {
    auto ret = new MetalDiagnosticsOverlay();
    if (ret && ret->init(layer)) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool MetalDiagnosticsOverlay::init(PlayLayer* layer) {
    if (!CCNode::init() || !layer)
        return false;

    m_layer = layer;

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
    MetalBackend::get().initialize();
    uploadLevelObjects();
    refreshText();
    return true;
}

bool MetalDiagnosticsOverlay::uploadLevelObjects() {
    if (!m_layer || !m_layer->m_objects)
        return false;

    std::vector<MetalObjectInput> inputs;
    inputs.reserve(m_layer->m_objects->count());

    std::uint32_t groupStateCount = 1;
    for (auto object : CCArrayExt<GameObject*, false>(m_layer->m_objects)) {
        if (!shouldIncludeObject(m_layer, object))
            continue;

        const auto size = object->getContentSize();
        MetalObjectInput input;
        input.x = object->m_startPosition.x;
        input.y = object->m_startPosition.y;
        input.halfWidth = std::max(
            1.0f,
            std::abs(size.width * object->m_startScaleX) * 0.5f
        );
        input.halfHeight = std::max(
            1.0f,
            std::abs(size.height * object->m_startScaleY) * 0.5f
        );
        input.rotationRadians = object->m_startRotationX * DEG_TO_RAD;

        const std::uint32_t groupCount = static_cast<std::uint32_t>(
            std::clamp<int>(object->m_groupCount, 0, 10)
        );
        for (std::uint32_t i = 0; i < groupCount; ++i) {
            if (!object->m_groups)
            break;
        const int groupId = (*object->m_groups)[i];
            if (groupId < 0 || static_cast<std::uint32_t>(groupId) > MAX_GROUP_ID)
                continue;

            input.groupIds[input.groupCount++] = static_cast<std::uint32_t>(groupId);
            groupStateCount = std::max(
                groupStateCount,
                static_cast<std::uint32_t>(groupId) + 1u
            );
        }

        inputs.push_back(input);
    }

    m_levelUploaded = MetalBackend::get().uploadLevelObjects(inputs, groupStateCount);
    m_levelUploadAttempted = m_levelUploaded;
    if (!m_levelUploaded) {
        const auto state = MetalBackend::get().snapshot();
        if (state.error.find("deferred because a culling frame is still in flight") == std::string::npos)
            m_levelUploadAttempted = true;
    }
    log::info(
        "Kyanite iOS: uploaded {} static object records to Metal; group states: {}",
        inputs.size(),
        groupStateCount
    );
    return m_levelUploaded;
}

void MetalDiagnosticsOverlay::submitVisibilityWork() {
    if (!m_layer || !m_layer->m_objectLayer || !m_levelUploaded)
        return;

    const auto winSize = CCDirector::get()->getWinSize();
    auto* objectLayer = m_layer->m_objectLayer;

    const CCPoint corners[] = {
        objectLayer->convertToNodeSpace({0.0f, 0.0f}),
        objectLayer->convertToNodeSpace({winSize.width, 0.0f}),
        objectLayer->convertToNodeSpace({0.0f, winSize.height}),
        objectLayer->convertToNodeSpace({winSize.width, winSize.height})
    };

    float minX = corners[0].x;
    float maxX = corners[0].x;
    float minY = corners[0].y;
    float maxY = corners[0].y;
    for (const auto& point : corners) {
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
    }

    auto& backend = MetalBackend::get();
    if (m_layer->m_effectManager) {
        for (const auto groupId : backend.getUsedGroupIds()) {
            backend.setGroupAlpha(
                static_cast<std::int32_t>(groupId),
                m_layer->m_effectManager->opacityModForGroup(static_cast<int>(groupId))
            );
        }
    }
    backend.updateCameraBounds(minX, minY, maxX, maxY);
    backend.submitCullingFrame();
}

void MetalDiagnosticsOverlay::update(float dt) {
    const bool enabled = Mod::get()->getSettingValue<bool>("enabled");
    const bool runWork = Mod::get()->getSettingValue<bool>("run_metal_probe");
    const bool showOverlay =
        Mod::get()->getSettingValue<bool>("diagnostics_overlay");
    setVisible(showOverlay);

    if (enabled && runWork) {
        if (!m_probeRequested) {
            m_probeRequested = true;
            MetalBackend::get().submitComputeProbeOnce();
        }

        if (!m_levelUploadAttempted)
            uploadLevelObjects();

        submitVisibilityWork();
    }

    m_refreshElapsed += dt;
    if (m_refreshElapsed < 0.20f)
        return;

    m_refreshElapsed = 0.0f;
    if (showOverlay)
        refreshText();
}

void MetalDiagnosticsOverlay::refreshText() {
    const bool enabled = Mod::get()->getSettingValue<bool>("enabled");
    const bool runWork = Mod::get()->getSettingValue<bool>("run_metal_probe");
    const auto state = MetalBackend::get().snapshot();

    std::string text;
    text += "KYANITE / iOS METAL PORT\n";

    if (!enabled) {
        text += "Metal workload: DISABLED IN SETTINGS\n";
    } else if (!runWork) {
        text += "Metal workload: DISABLED IN SETTINGS\n";
    } else {
        text += fmt::format(
            "Metal device: {} | queue: {}\n",
            state.deviceReady ? "READY" : "NOT READY",
            state.commandQueueReady ? "READY" : "NOT READY"
        );
        if (!state.deviceName.empty())
            text += fmt::format("GPU: {}\n", state.deviceName);

        text += fmt::format(
            "Compute pipelines: probe {} / culling {}\n",
            state.computePipelineReady ? "READY" : "NOT READY",
            state.cullingPipelineReady ? "READY" : "NOT READY"
        );

        if (state.probeCompleted) {
            text += fmt::format(
                "Probe: COMPLETE ({}/{})\n",
                state.completedProbeThreads,
                state.expectedProbeThreads
            );
        } else if (state.probeSubmitted) {
            text += "Probe: SUBMITTED / WAITING\n";
        } else if (state.probeFailed) {
            text += "Probe: FAILED\n";
        } else {
            text += "Probe: NOT SUBMITTED\n";
        }

        if (!m_levelUploadAttempted) {
            text += "Level object data: WAITING\n";
        } else if (!m_levelUploaded || !state.levelDataReady) {
            text += "Level object data: UPLOAD FAILED\n";
        } else {
            text += fmt::format(
                "Static object records: {} (uploaded once)\n",
                state.levelObjectCount
            );
            if (state.cullingCompleted) {
                text += fmt::format(
                    "GPU visibility: {} visible / {} culled\n",
                    state.visibleObjectCount,
                    state.culledObjectCount
                );
                text += fmt::format(
                    "Culling frames: {} completed / {} submitted\n",
                    state.completedCullingFrames,
                    state.submittedCullingFrames
                );
                if (state.cullingGpuTimeAvailable) {
                    text += fmt::format(
                        "Culling GPU time: {:.3f} ms\n",
                        state.cullingGpuTimeMilliseconds
                    );
                } else {
                    text += "Culling GPU time: unavailable\n";
                }
            } else if (state.cullingSubmitted) {
                text += "GPU visibility: SUBMITTED / WAITING\n";
            } else if (state.cullingFailed) {
                text += "GPU visibility: FAILED\n";
            } else {
                text += "GPU visibility: NOT SUBMITTED\n";
            }

            text += fmt::format(
                "Group-state updates: {} | skipped busy frames: {}\n",
                state.groupStateUpdates,
                state.skippedBusyFrames
            );
            if (state.invalidVisibleIndices != 0) {
                text += fmt::format(
                    "WARNING: {} invalid GPU indices\n",
                    state.invalidVisibleIndices
                );
            }
        }
    }

    if (!state.error.empty())
        text += fmt::format("Metal error: {}\n", state.error);

    text += "Metal sprite draw pass: NOT CONNECTED\n";
    text += "Active level renderer: STOCK GD (SAFE FALLBACK)\n";

    m_label->setString(text.c_str());
    const auto labelSize = m_label->getContentSize();
    const float labelScale = m_label->getScale();
    m_background->setContentSize({
        labelSize.width * labelScale + 12.0f,
        labelSize.height * labelScale + 10.0f
    });
}
