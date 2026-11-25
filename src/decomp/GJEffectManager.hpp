#pragma once
#include <Geode/Geode.hpp>

class decomp_GJEffectManager : public GJEffectManager {
public:
    cocos2d::ccColor3B colorForPulseEffect(cocos2d::ccColor3B const& color, PulseEffectAction* action);

    cocos2d::ccColor3B colorForGroupID(int id, cocos2d::ccColor3B const& color, bool mainColor);
};