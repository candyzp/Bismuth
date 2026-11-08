#!/usr/bin/env python3
"""Verify the active iOS GPU-only renderer's level and frame lifecycle."""
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

def body(source, signature, next_signature):
    start = source.index(signature)
    end = source.index(next_signature, start + len(signature))
    return source[start:end]

play = body(hooks, "    void resetLevel()", "    void updateVisibility")
play_ios = play.split("#ifdef GEODE_IS_IOS", 1)[1].split("#else", 1)[0]
require("IOSUpstream::IOSGPURenderer::create(this)" in play_ios,
        "new PlayLayer must create the GPU-only renderer")
require("prepareForLevelReset()" in play_ios and
        "enforceGPUOwnership()" in play_ios,
        "level reset must retire old layer nodes and reassert GPU ownership")
require("auto renderer = Renderer::create(this);" not in play_ios,
        "iOS reset must not instantiate the legacy hybrid Renderer")

visibility = body(hooks, "    void updateVisibility(float dt)", "\n};")
visibility_ios = visibility.split("#ifdef GEODE_IS_IOS", 1)[1].split("#else", 1)[0]
require("optimized_updateVisibility(dt)" in visibility_ios and
        "refreshBlendOrder()" in visibility_ios,
        "iOS visibility must preserve GD effects while refreshing GPU draw ordering")

require("renderer->updateScene(dt)" in hooks,
        "simulation updates must advance GPU scene state")
require("renderer->beginFrame()" in hooks and
        "finishedRenderer->finishFrame()" in hooks,
        "display-link frame boundaries must reach the active GPU renderer")
require("optimized_processRotationActions(" in hooks and
        "renderer->getGroupManager()" in hooks,
        "rotation triggers must use the GPU-owned group transforms")
require("renderer->getGroupManager().moveGroup(" in hooks,
        "move/follow triggers must update GPU group state")
require("pruneGPUOwnedDecorations(this)" in hooks,
        "GPU-owned decorations must be removed from CPU transform lists")

move = body(hooks, "    void processMoveActions()", "    void processRotationActions")
move_ios = move.split("#ifdef GEODE_IS_IOS", 1)[1].split("#else", 1)[0]
require("getStaticGroup(groupId)" in move_ios and
        "getOptimizedGroup(groupId)" not in move_ios,
        "CPU movement must not process the GPU optimized-decoration lane")

require("void IOSGPURenderer::prepareForLevelReset()" in gpu and
        "layerNodeKeys.clear()" in gpu,
        "GPU draw nodes must be recreated when PlayLayer rebuilds Z-order containers")
require("bool IOSGPURenderer::updateScene(float dt)" in gpu and
        "sceneData.update(dt)" in gpu,
        "GPU scene state must update each simulation tick")
require("bool IOSGPURenderer::refreshBlendOrder()" in gpu and
        "sceneData.refreshBlendOrder()" in gpu,
        "blend ordering must refresh before layer draws")
require("GroupManager& getGroupManager()" in header,
        "the GPU renderer must expose its active group manager")
require("updateObjectParticles();" in scene,
        "per-object particle caches must remain alive in the GPU-only path")
require("IOSUpstream::GroupManager& groupManager" in decomp,
        "GPU transforms must retain the optimized physics-safe rotation implementation")

failure = gpu[gpu.index("void IOSGPURenderer::fail"):gpu.index("void IOSGPURenderer::hideStockBatches")]
require("hideStockBatches();" in failure,
        "renderer failure must remain strict GPU-only, without stock batch fallback")

print("PASS: GPU-only iOS PlayLayer lifecycle, frame updates, scene state, blend ordering, group transforms, and no hybrid fallback")
