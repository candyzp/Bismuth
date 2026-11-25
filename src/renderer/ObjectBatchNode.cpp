#include "ObjectBatchNode.hpp"
#include "Renderer.hpp"
#include <profiler.hpp>

using namespace geode::prelude;

void ObjectBatchNode::draw() {
    auto renderer = Renderer::get();
    if (!renderer)
        return;

    renderer->getObjectBatch().draw(layerId);
}