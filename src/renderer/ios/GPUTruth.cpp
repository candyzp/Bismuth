#ifdef GEODE_IS_IOS

#include "GPUTruth.hpp"
#include "GroundOwnership.hpp"
#include "AtlasInterleave.hpp"
#include "../Renderer.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/GJGroundLayer.hpp>
#include <Geode/cocos/sprite_nodes/CCSpriteBatchNode.h>

#include <string>
#include <unordered_set>

using namespace geode::prelude;

namespace {
struct TruthState {
    Renderer* renderer = nullptr;
    bool enabled = false;

    usize backgroundSubmits = 0;
    usize backgroundFailures = 0;
    bool failureReported = false;
    usize objectSubmits = 0;
    usize objectSprites = 0;
    usize objectFailures = 0;
    std::string lastObjectFailureReason = "none";
    int lastObjectFailureSlot = -1;
    u32 lastObjectFailureAtlasSize = 0;
    u32 lastObjectFailureAge = 9999;
    usize clearSuspects = 0;
    usize spikeClearSuspects = 0;

    // The truth overlay is diagnostics, not part of rendering. Deep ownership /
    // opacity scans are sampled instead of repeating another full atlas walk on
    // every displayed frame. Submit/failure counters remain exact every frame.
    usize deepScanFrame = 0;
    bool deepScanThisFrame = true;

    usize groundSubmits = 0;
    usize groundFailures = 0;
    bool ground1 = false;
    bool ground2 = false;
    bool line = false;

    std::unordered_set<cocos2d::CCSprite*> seenObjectSprites;

    Ref<cocos2d::CCLabelBMFont> text;
    Ref<cocos2d::CCLabelBMFont> outline1;
    Ref<cocos2d::CCLabelBMFont> outline2;
    std::string lastText;
};

TruthState& truth() {
    static TruthState state;
    return state;
}

void clearLabels(TruthState& state) {
    // Labels live on PlayLayer, so dropping Ref<> alone leaves the nodes in the
    // scene. That created a new overlapping copy after every pause/resume.
    if (state.text)
        state.text->removeFromParentAndCleanup(true);
    if (state.outline1)
        state.outline1->removeFromParentAndCleanup(true);
    if (state.outline2)
        state.outline2->removeFromParentAndCleanup(true);

    state.text = nullptr;
    state.outline1 = nullptr;
    state.outline2 = nullptr;
    state.lastText.clear();
}

void resetFrameCounters(TruthState& state) {
    state.backgroundSubmits = 0;
    state.backgroundFailures = 0;
    state.objectSubmits = 0;
    state.objectFailures = 0;

    if (state.deepScanThisFrame) {
        state.objectSprites = 0;
        state.clearSuspects = 0;
        state.spikeClearSuspects = 0;
        state.seenObjectSprites.clear();
    }

    state.groundSubmits = 0;
    state.groundFailures = 0;
    state.ground1 = false;
    state.ground2 = false;
    state.line = false;
}

bool ensureLabels(TruthState& state, Renderer* renderer) {
    if (!renderer)
        return false;

    auto layer = renderer->getPlayLayer();
    if (!layer)
        return false;

    if (state.text && state.outline1 && state.outline2 &&
        state.text->getParent() == layer &&
        state.outline1->getParent() == layer &&
        state.outline2->getParent() == layer)
        return true;

    clearLabels(state);

    state.text = cocos2d::CCLabelBMFont::create("", "chatFont.fnt");
    state.outline1 = cocos2d::CCLabelBMFont::create("", "chatFont.fnt");
    state.outline2 = cocos2d::CCLabelBMFont::create("", "chatFont.fnt");
    if (!state.text || !state.outline1 || !state.outline2) {
        clearLabels(state);
        return false;
    }

    const float top = cocos2d::CCDirector::get()->getWinSize().height - 48.f;

    state.outline1->setColor({0, 0, 0});
    state.outline1->setOpacity(210);
    state.outline2->setColor({0, 0, 0});
    state.outline2->setOpacity(210);

    state.text->setAnchorPoint(cocos2d::CCPoint(0.f, 1.f));
    state.text->setPosition(cocos2d::CCPoint(1.f, top));
    state.text->setScale(0.42f);
    layer->addChild(state.text, 1000);

    state.outline1->setAnchorPoint(cocos2d::CCPoint(0.f, 1.f));
    state.outline1->setPosition(cocos2d::CCPoint(0.5f, top - 0.5f));
    state.outline1->setScale(0.42f);
    layer->addChild(state.outline1, 999);

    state.outline2->setAnchorPoint(cocos2d::CCPoint(0.f, 1.f));
    state.outline2->setPosition(cocos2d::CCPoint(1.5f, top + 0.5f));
    state.outline2->setScale(0.42f);
    layer->addChild(state.outline2, 999);

    return true;
}

void setChartVisible(TruthState& state, bool visible) {
    if (state.text)
        state.text->setVisible(visible);
    if (state.outline1)
        state.outline1->setVisible(visible);
    if (state.outline2)
        state.outline2->setVisible(visible);
}
} // namespace

namespace GPUTruth {
void beginFrame(Renderer* renderer) {
    auto& state = truth();

    if (state.renderer != renderer) {
        clearLabels(state);
        state.renderer = renderer;
        state.failureReported = false;
        state.deepScanFrame = 0;
        state.lastObjectFailureReason = "none";
        state.lastObjectFailureSlot = -1;
        state.lastObjectFailureAtlasSize = 0;
        state.lastObjectFailureAge = 9999;
        state.objectSprites = 0;
        state.clearSuspects = 0;
        state.spikeClearSuspects = 0;
        state.seenObjectSprites.clear();
    }

    state.enabled = renderer && Mod::get()->getSettingValue<bool>("ios_gpu_debug");
    // The deep diagnostic walk is intentionally rare on decoration-heavy
    // scenes. Submit/failure truth remains frame-exact; opacity/sprite totals are
    // sampled once per second at 60 Hz so the overlay cannot become the 4k+ wall.
    state.deepScanThisFrame = state.enabled && (state.deepScanFrame++ % 60 == 0);
    if (state.lastObjectFailureAge < 9999)
        ++state.lastObjectFailureAge;
    resetFrameCounters(state);
}

void recordObjectBatch(Renderer* renderer, cocos2d::CCSpriteBatchNode* batch) {
    auto& state = truth();
    if (!state.enabled || !renderer || renderer != state.renderer || !batch)
        return;

    ++state.objectSubmits;

    if (!state.deepScanThisFrame)
        return;

    auto descendants = batch->getDescendants();
    if (!descendants)
        return;

    for (u32 i = 0; i < descendants->count(); ++i) {
        auto sprite = typeinfo_cast<cocos2d::CCSprite*>(descendants->objectAtIndex(i));
        if (!sprite || sprite->getBatchNode() != batch || !renderer->isGPUOwnedSprite(sprite))
            continue;
        if (!state.seenObjectSprites.insert(sprite).second)
            continue;

        ++state.objectSprites;

        // This is diagnostic, not a judgement that a fade is wrong. A GPU-owned
        // sprite below near-opaque means the resolved state entering the shader is
        // already translucent and is worth checking when the user sees clear art.
        if (sprite->getDisplayedOpacity() < 250) {
            ++state.clearSuspects;
            if (auto object = typeinfo_cast<GameObject*>(sprite);
                object && object->m_objectType == GameObjectType::Hazard) {
                ++state.spikeClearSuspects;
            }
        }
    }
}

void recordObjectFailure(Renderer* renderer) {
    auto& state = truth();
    if (!renderer || renderer != state.renderer)
        return;

    ++state.objectFailures;

    const char* reason = AtlasInterleaveRegistry::lastFailureReason();
    state.lastObjectFailureReason =
        (reason && reason[0] && std::string(reason) != "none") ? reason : "standalone-or-unknown";
    state.lastObjectFailureSlot = AtlasInterleaveRegistry::lastFailureSlot();
    state.lastObjectFailureAtlasSize = AtlasInterleaveRegistry::lastFailureAtlasSize();
    state.lastObjectFailureAge = 0;
}

void recordBackground(Renderer* renderer, bool submitted) {
    auto& state = truth();
    if (!renderer || renderer != state.renderer)
        return;
    if (submitted) ++state.backgroundSubmits;
    else ++state.backgroundFailures;
}

void recordGroundSuccess(
    Renderer* renderer,
    GJGroundLayer* ground,
    cocos2d::CCSprite* sprite
) {
    auto& state = truth();
    if (!state.enabled || !renderer || renderer != state.renderer || !ground || !sprite)
        return;

    ++state.groundSubmits;
    const auto part = GroundOwnership::part(ground, sprite);
    if (part == GroundOwnership::Part::Ground1)
        state.ground1 = true;
    if (part == GroundOwnership::Part::Ground2)
        state.ground2 = true;
    if (part == GroundOwnership::Part::Line)
        state.line = true;
}

void recordGroundFailure(
    Renderer* renderer,
    GJGroundLayer*,
    cocos2d::CCSprite*
) {
    auto& state = truth();
    if (state.enabled && renderer && renderer == state.renderer)
        ++state.groundFailures;
}

void finishFrame(Renderer* renderer) {
    auto& state = truth();
    if (!renderer || renderer != state.renderer)
        return;

    const bool failed = state.objectFailures || state.backgroundFailures;
    if (failed && !state.failureReported) {
        log::error(
            "Bismuth strict GPU draw failed: batches {}, background {}; reason '{}' slot {}/{}; stock redraw suppressed",
            state.objectFailures,
            state.backgroundFailures,
            state.lastObjectFailureReason,
            state.lastObjectFailureSlot,
            state.lastObjectFailureAtlasSize
        );
        state.failureReported = true;
    } else if (!failed) {
        state.failureReported = false;
    }
    if (!state.enabled && !failed) {
        setChartVisible(state, false);
        return;
    }

    if (!ensureLabels(state, renderer))
        return;
    setChartVisible(state, true);

    const bool objectsYES = state.objectSubmits > 0;
    const bool gpuYES = objectsYES || state.backgroundSubmits > 0;

    const bool showStickyFailure =
        state.lastObjectFailureReason != "none" && state.lastObjectFailureAge <= 120;

    const std::string chart = fmt::format(
        "GPU TRUTH: {} | submits {} | sprites {}\n"
        "Clear suspects: {} | faded hazards {}\n"
        "Background: {} draws | failures {}\n"
        "Failures: batches {} | strict, no redraw\n"
        "Last fail: {} | slot {}/{} | {}f ago",
        gpuYES ? "YES" : "NO",
        state.objectSubmits,
        state.objectSprites,
        state.clearSuspects,
        state.spikeClearSuspects,
        state.backgroundSubmits, state.backgroundFailures,
        state.objectFailures,
        showStickyFailure ? state.lastObjectFailureReason : "none",
        showStickyFailure ? state.lastObjectFailureSlot : -1,
        showStickyFailure ? state.lastObjectFailureAtlasSize : 0,
        showStickyFailure ? state.lastObjectFailureAge : 0
    );

    if (state.lastText == chart)
        return;

    state.lastText = chart;
    state.text->setString(chart.c_str());
    state.outline1->setString(chart.c_str());
    state.outline2->setString(chart.c_str());
}
} // namespace GPUTruth

#endif
