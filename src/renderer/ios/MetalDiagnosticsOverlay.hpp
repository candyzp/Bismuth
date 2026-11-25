#pragma once

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>

class MetalDiagnosticsOverlay : public cocos2d::CCNode {
public:
    static MetalDiagnosticsOverlay* create(PlayLayer* layer);

protected:
    bool init(PlayLayer* layer);
    void update(float dt) override;

private:
    bool uploadLevelObjects();
    void submitVisibilityWork();
    void refreshText();

    PlayLayer* m_layer = nullptr;
    cocos2d::CCLayerColor* m_background = nullptr;
    cocos2d::CCLabelBMFont* m_label = nullptr;
    float m_refreshElapsed = 0.0f;
    bool m_probeRequested = false;
    bool m_levelUploadAttempted = false;
    bool m_levelUploaded = false;
};
