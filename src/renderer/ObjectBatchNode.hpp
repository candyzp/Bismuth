#pragma once

#include <common.hpp>

#include "ObjectBatch.hpp"
#include "ObjectUtils.hpp"

class ObjectBatchNode : public cocos2d::CCNode {
public:
    inline ObjectBatchNode(const LayerKey& id)
        : layerId(id) {}

    bool init() override;

    void draw() override;

public:
    static inline Ref<ObjectBatchNode> create(const LayerKey& id) {
        auto ret = new ObjectBatchNode(id);
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

private:
    ObjectBatch::LayerDrawCall* drawCall;
    LayerKey layerId;
    cocos2d::CCTexture2D* spriteSheetTexture;
};