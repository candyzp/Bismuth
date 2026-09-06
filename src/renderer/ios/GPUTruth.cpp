#ifdef GEODE_IS_IOS

#include "GPUTruth.hpp"
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

    usize objectSubmits = 0;
    usize objectSprites = 0;
    usize objectFailures = 0;
    usize clearSuspects = 0;
    usize spikeClearSuspects = 0;

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
    state.text = nullptr;
    state.outline1 = nullptr;
    state.outline2 = nullptr;
    state.lastText.clear();
}

void resetFrameCounters(TruthState& state) {
    state.objectSubmits = 0;
    state.objectSprites = 0;
    state.objectFailures = 0;
    state.clearSuspects = 0;
    state.spikeClearSuspects = 0;

    state.groundSubmits = 0;
    state.groundFailures = 0;
    state.ground1 = false;
    state.ground2 = false;
    state.line = false;

    state.seenObjectSprites.clear();
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

    const float top = cocos2d::CCDirector::get()->getWinSize().height - 38.f;

    state.outline1->setColor({0, 0, 0});
    state.outline1->setOpacity(210);
    state.outline2->setColor({0, 0, 0});
    state.outline2->setOpacity(210);

    state.text->setAnchorPoint({0.f, 1.f});
    state.text->setPosition({1.f, top});
    state.text->setScale(0.42f);
    layer->addChild(state.text, 1000);

    state.outline1->setAnchorPoint({0.f, 1.f});
    state.outline1->setPosition({0.5f, top - 0.5f});
    state.outline1->setScale(0.42f);
    layer->addChild(state.outline1, 999);

    state.outline2->setAnchorPoint({0.f, 1.f});
    state.outline2->setPosition({1.5f, top + 0.5f});
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
    }

    resetFrameCounters(state);
}

void recordObjectBatch(Renderer* renderer, cocos2d::CCSpriteBatchNode* batch) {
    auto& state = truth();
    if (!renderer || renderer != state.renderer || !batch)
        return;

    ++state.objectSubmits;

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
    if (renderer && renderer == state.renderer)
        ++state.objectFailures;
}

void recordGroundSuccess(
    Renderer* renderer,
    GJGroundLayer* ground,
    cocos2d::CCSprite* sprite
) {
    auto& state = truth();
    if (!renderer || renderer != state.renderer || !ground || !sprite)
        return;

    ++state.groundSubmits;
    if (sprite == ground->m_ground1Sprite)
        state.ground1 = true;
    if (sprite == ground->m_ground2Sprite)
        state.ground2 = true;
    if (sprite == ground->m_lineSprite)
        state.line = true;
}

void recordGroundFailure(
    Renderer* renderer,
    GJGroundLayer*,
    cocos2d::CCSprite*
) {
    auto& state = truth();
    if (renderer && renderer == state.renderer)
        ++state.groundFailures;
}

void finishFrame(Renderer* renderer) {
    auto& state = truth();
    if (!renderer || renderer != state.renderer)
        return;

    const bool show = Mod::get()->getSettingValue<bool>("ios_gpu_debug");
    if (!show) {
        setChartVisible(state, false);
        return;
    }

    if (!ensureLabels(state, renderer))
        return;
    setChartVisible(state, true);

    const bool objectsYES = state.objectSubmits > 0;
    const bool floorYES = state.ground1 || state.ground2;
    const bool gpuYES = objectsYES || state.groundSubmits > 0;

    const std::string chart = fmt::format(
        "GPU TRUTH | CONCLUSION: {}\n"
        "Objects: {} | submits {} | sprites {}\n"
        "Floor: {} | G1 {} | G2 {} | Line {}\n"
        "Clear suspects: {} | spikes {}\n"
        "Failures: objects {} | floor {}",
        gpuYES ? "YES" : "NO",
        objectsYES ? "YES" : "NO",
        state.objectSubmits,
        state.objectSprites,
        floorYES ? "YES" : "NO",
        state.ground1 ? "YES" : "NO",
        state.ground2 ? "YES" : "NO",
        state.line ? "YES" : "NO",
        state.clearSuspects,
        state.spikeClearSuspects,
        state.objectFailures,
        state.groundFailures
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
