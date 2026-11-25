#include "GJEffectManager.hpp"

using namespace geode::prelude;

ccColor3B decomp_GJEffectManager::colorForPulseEffect(ccColor3B const& color, PulseEffectAction* action) {
    if (action->m_currentValue <= 0.0f)
        return color;

    if (action->m_pulseEffectType == PulseEffectType::Hsv) {
        ccColor3B srcColor = color;
        if (action->m_colorIndex)
            srcColor = activeColorForIndex(action->m_colorIndex);

        if (action->m_currentValue == 1.0f)
            return GameToolbox::transformColor(srcColor, action->m_hsv);

        ccHSVValue hsv;
        if (action->m_isDynamicHsv)
            hsv = GameToolbox::getMultipliedHSV(action->m_hsv, action->m_currentValue);
        else
            hsv = action->m_hsv;

        ccColor3B dstColor = GameToolbox::transformColor(srcColor, hsv);

        return GameToolbox::multipliedColorValue(color, dstColor, action->m_currentValue);
    }

    if (action->m_pulseEffectType != PulseEffectType::Color)
        return color;

    if (action->m_currentValue == 1.0f)
        return action->m_color;

    return GameToolbox::multipliedColorValue(color, action->m_color, action->m_currentValue);
}

ccColor3B decomp_GJEffectManager::colorForGroupID(int id, ccColor3B const& color, bool mainColor) {
    int groupId = std::clamp(id, 0, 9999);

    if (!m_unkVector2f0[groupId])
        return color;

    ccColor3B outColor = color;

    auto it = m_pulseEffectMap.find(groupId);
    if (it == m_pulseEffectMap.end())
        return outColor;

    for (auto& action : it->second) {
        bool shouldApply = true;

        if (action.m_mainOnly && !mainColor)
            shouldApply = false;
        if (action.m_detailOnly && mainColor)
            shouldApply = false;

        if (shouldApply)
            outColor = colorForPulseEffect(outColor, &action);
    }

    return outColor;
}