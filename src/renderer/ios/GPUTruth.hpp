#pragma once

#ifdef GEODE_IS_IOS

namespace cocos2d {
class CCSprite;
class CCSpriteBatchNode;
}

class Renderer;
class GJGroundLayer;

namespace GPUTruth {
void beginFrame(Renderer* renderer);
void recordObjectBatch(Renderer* renderer, cocos2d::CCSpriteBatchNode* batch);
void recordObjectFailure(Renderer* renderer);
void recordGroundSuccess(Renderer* renderer, GJGroundLayer* ground, cocos2d::CCSprite* sprite);
void recordGroundFailure(Renderer* renderer, GJGroundLayer* ground, cocos2d::CCSprite* sprite);
void finishFrame(Renderer* renderer);
}

#endif
