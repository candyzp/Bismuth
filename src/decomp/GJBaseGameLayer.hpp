#pragma once
#include <Geode/Geode.hpp>

#ifdef GEODE_IS_IOS
namespace IOSUpstream {
class GroupManager;
}
#endif

class decomp_GJBaseGameLayer : public GJBaseGameLayer {
public:
    void moveObjects(cocos2d::CCArray* objects, double moveX, double moveY, bool unused);

    void processMoveActions();
};