#pragma once

#include <Geode/Geode.hpp>

class MetalDiagnosticsOverlay : public cocos2d::CCNode {
public:
    static MetalDiagnosticsOverlay* create();

protected:
    bool init() override;
    void update(float dt) override;

private:
    void refreshText();

    cocos2d::CCLayerColor* m_background = nullptr;
    cocos2d::CCLabelBMFont* m_label = nullptr;
    float m_refreshElapsed = 0.0f;
    bool m_probeRequested = false;
};
