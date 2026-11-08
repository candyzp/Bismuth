#!/usr/bin/env python3
"""Guard activation and core lifecycle wiring of the iOS GPU-only renderer."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
hooks = (ROOT / "src/renderer/hooks.cpp").read_text()
gpu = (ROOT / "src/renderer/ios_upstream/IOSGPURenderer.cpp").read_text()
header = (ROOT / "src/renderer/ios_upstream/IOSGPURenderer.hpp").read_text()
scene = (ROOT / "src/renderer/ios_upstream/IOSSceneData.cpp").read_text()
decomp = (ROOT / "src/decomp/GJBaseGameLayer.cpp").read_text()

def require(condition, message):
    if not condition:
        raise AssertionError(message)

require("IOSUpstream::IOSGPURenderer::create(this)" in hooks,
        "PlayLayer must create the iOS GPU renderer")
require("existing->prepareForLevelReset()" in hooks and
        "renderer->enforceGPUOwnership()" in hooks,
        "PlayLayer reset must retire stale layer nodes and reassert GPU ownership")
require("optimized_updateVisibility(dt)" in hooks and
        "renderer->refreshBlendOrder()" in hooks,
        "iOS visibility must use the optimized visual-update path and refresh GPU draw order")
require("renderer->updateScene(dt)" in hooks,
        "GJBaseGameLayer updates must advance GPU scene state")
require("optimized_processRotationActions(" in hooks and
        "renderer->getGroupManager()" in hooks,
        "iOS rotation must use the GPU renderer group manager")
require("renderer->getGroupManager().moveGroup(" in hooks,
        "move/follow triggers must update GPU group transforms")
require("pruneGPUOwnedDecorations(this)" in hooks and
        "removeDecoObjects(group)" in hooks,
        "GPU-owned decorations must be removed from CPU transform lists")

start = hooks.index("    void processMoveActions()")
end = hooks.index("    void processRotationActions()", start)
ios_move = hooks[start:end].split("#ifdef GEODE_IS_IOS", 1)[1].split("#else", 1)[0]
require("getStaticGroup(groupId)" in ios_move and
        "getOptimizedGroup(groupId)" not in ios_move,
        "GPU optimized decorations must not be moved again through the CPU optimized-group lane")

require("bool updateScene(float dt);" in header and
        "bool refreshBlendOrder();" in header and
        "GroupManager& getGroupManager()" in header,
        "renderer integration methods are missing from its public API")
require("bool IOSGPURenderer::updateScene(float dt)" in gpu and
        "sceneData.update(dt)" in gpu,
        "renderer must advance SceneData and propagate update failures")
require("bool IOSGPURenderer::refreshBlendOrder()" in gpu and
        "sceneData.refreshBlendOrder()" in gpu,
        "renderer must refresh the current GPU layer schedule")
require("IOSGPURenderer::prepareForLevelReset()" in gpu and
        "layerNodeKeys.clear()" in gpu,
        "old per-layer nodes must be retired on level reset")
fail_body = gpu[gpu.index("void IOSGPURenderer::fail"):gpu.index("void IOSGPURenderer::hideStockBatches")]
require("hideStockBatches();" in fail_body,
        "strict GPU-only failure path must not restore stock batch rendering")
require("updateObjectParticles();" in scene,
        "SceneData must maintain cached object particle ownership")
require("IOSUpstream::GroupManager& groupManager" in decomp,
        "the optimized iOS rotation implementation must remain available")

print("PASS: iOS GPU-only activation, frame state, blend order, group triggers, reset lifecycle, and strict stock-batch ownership")
