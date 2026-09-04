#pragma once

#ifdef GEODE_IS_IOS

namespace cocos2d {
class CCSpriteBatchNode;
class CCSprite;
}

class Renderer;
class AssistShadowBatch;
class StandaloneAssistBatch;

// Gameplay-only atlas ordering registry. Owner registration happens only after
// persistent GPU geometry is ready. The single CCSpriteBatchNode hook asks this
// registry whether the exact live gameplay batch has verified GPU-owned slots.
class AtlasInterleaveRegistry {
public:
    static void registerImmediate(AssistShadowBatch* owner);
    static void unregisterImmediate(AssistShadowBatch* owner);

    static void registerDeferred(StandaloneAssistBatch* owner);
    static void unregisterDeferred(StandaloneAssistBatch* owner);

    static bool ownsBatch(Renderer* renderer, cocos2d::CCSpriteBatchNode* batch);
    static bool drawBatch(Renderer* renderer, cocos2d::CCSpriteBatchNode* batch);

    static bool shouldSkipTransform(Renderer* renderer, cocos2d::CCSprite* sprite);
    static void beginFrame();
};

#endif
