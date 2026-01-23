#include "ObjectBatchNode.hpp"
#include "Geode/cocos/platform/win32/CCGL.h"
#include "Renderer.hpp"
#include <profiler.hpp>

using namespace geode::prelude;

bool ObjectBatchNode::init() {
    auto renderer = Renderer::get();
    spriteSheetTexture = renderer->getSpriteSheetTexture(layerId.spriteSheet);
    drawCall = renderer->getObjectBatch().getDrawCall(layerId);
    return true;
}

void ObjectBatchNode::draw() {
    profiler::functionPush("ObjectBatchNode::draw");
    auto renderer = Renderer::get();

    auto shader = renderer->prepareDraw();
    shader->setUInt("u_spriteSheet", (u32)layerId.spriteSheet);
    shader->setTexture("u_spriteSheetTexture", 0, spriteSheetTexture);

    glEnable(GL_BLEND);
    if (layerId.blending)
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    else
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    renderer->getObjectBatch().draw(drawCall);

    renderer->finishDraw();
    profiler::functionPop();
}