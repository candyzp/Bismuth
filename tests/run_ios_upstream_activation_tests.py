#!/usr/bin/env python3
"""Guard activation and core lifecycle wiring of the iOS GPU-only renderer."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
hooks = (ROOT / "src/renderer/hooks.cpp").read_text()
cmake = (ROOT / "CMakeLists.txt").read_text()
metal = (ROOT / "src/renderer/ios_metal/IOSMetalInterop.mm").read_text()
metal_header = (ROOT / "src/renderer/ios_metal/IOSMetalInterop.hpp").read_text()
gpu = (ROOT / "src/renderer/ios_upstream/IOSGPURenderer.cpp").read_text()
header = (ROOT / "src/renderer/ios_upstream/IOSGPURenderer.hpp").read_text()
scene = (ROOT / "src/renderer/ios_upstream/IOSSceneData.cpp").read_text()
batch = (ROOT / "src/renderer/ios_upstream/IOSObjectBatch.cpp").read_text()
batch_header = (ROOT / "src/renderer/ios_upstream/IOSObjectBatch.hpp").read_text()
exit_guard = (ROOT / "src/renderer/ios/PlayLayerExitGuard.cpp").read_text()
decomp = (ROOT / "src/decomp/GJBaseGameLayer.cpp").read_text()

def require(condition, message):
    if not condition:
        raise AssertionError(message)

require("IOSUpstream::IOSGPURenderer::create(this)" in hooks,
        "PlayLayer must create the iOS GPU renderer")
require("enable_language(OBJCXX)" in cmake and
        "IOSMetalInterop.mm" in cmake and
        "-fobjc-arc" in cmake,
        "iOS build must compile the Objective-C++ Metal bridge")
require("METAL_FRAMEWORK" in cmake and "COREVIDEO_FRAMEWORK" in cmake and
        "FOUNDATION_FRAMEWORK" in cmake,
        "iOS target must link Metal, CoreVideo and Foundation")
require("kCVPixelBufferOpenGLESCompatibilityKey" in metal and
        "kCVPixelBufferMetalCompatibilityKey" in metal and
        "kCVPixelBufferIOSurfacePropertiesKey" in metal,
        "shared CVPixelBuffer must be created with both API compatibility flags and IOSurface backing")
require("CVMetalTextureCacheCreateTextureFromImage" in metal and
        "CVOpenGLESTextureCacheCreateTextureFromImage" in metal,
        "one CVPixelBuffer must expose both Metal and OpenGL ES texture views")
require("newComputePipelineStateWithFunction" in metal and
        "dispatchThreadgroups" in metal and "waitUntilCompleted" in metal,
        "Metal compute probe must execute and synchronize before the GL consumer")
require("metalInterop.runSelfTest(metalInteropError)" in gpu and
        "metalInterop.selfTestPassed()" in gpu,
        "the active iOS renderer must initialize and report the interop self-test")
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
scene_dtor = scene[scene.index("SceneData::~SceneData()"):scene.index("SceneData* SceneData::getCurrent()")]
require("releaseObjectParticles()" not in scene_dtor,
        "SceneData destructor must not call into a dying particle manager")
require("suspendForPlayLayerExit()" in exit_guard and
        "resumeAfterPlayLayerExit()" in exit_guard,
        "PlayLayer exit and pause/transition resume must suspend and reclaim GPU ownership")
require("dynamicSpriteIndices" in batch_header and
        "for (const u32 spriteIndex : dynamicSpriteIndices)" in batch,
        "live appearance/geometry checks must iterate only the dynamic sprite list")
require("for (usize i = 0; i < sourceSpritePointers.size(); ++i)" not in batch,
        "the old full-level sprite scan must not return")
require("source.refreshTexture = object->getHasSyncedAnimation();" in batch and
        "source.refreshAppearance =" in batch,
        "direct-tint sprites and known animations must be selectively tracked")

print("PASS: iOS GPU-only activation, frame state, blend order, group triggers, reset lifecycle, and strict stock-batch ownership")
