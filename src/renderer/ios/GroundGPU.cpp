#ifdef GEODE_IS_IOS

#include "../Renderer.hpp"
#include <Geode/cocos/sprite_nodes/CCSpriteBatchNode.h>

// Ground is deliberately NOT a Bismuth GPU-owned visual.
//
// Geometry Dash / Cocos remains the only authority for floor scrolling,
// recycling, timing, colors and draw order. This file keeps inert symbols only
// so ground ownership cannot accidentally return through an old call site.
namespace GroundGPU {
bool ownsBatch(Renderer*, cocos2d::CCSpriteBatchNode*) {
    return false;
}

bool drawBatch(Renderer*, cocos2d::CCSpriteBatchNode*) {
    return false;
}
} // namespace GroundGPU

#endif
