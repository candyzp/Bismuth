#!/usr/bin/env python3
"""Guard activation and core lifecycle wiring of the selectable iOS renderer."""
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

def body(source, signature, next_signature):
    start = source.index(signature)
    end = source.index(next_signature, start + len(signature))
    return source[start:end]

require("IOSUpstream::IOSGPURenderer::create(this)" in hooks,
        "PlayLayer must create the iOS GPU renderer")
require("enable_language(OBJCXX)" in cmake and
        "IOSMetalInterop.mm" in cmake and
        "-fobjc-arc" in cmake and
        'COMPILE_DEFINITIONS "GEODE_IS_IOS"' in cmake,
        "iOS build must compile the Objective-C++ Metal bridge with its iOS-only API guard")
require("METAL_FRAMEWORK" in cmake and "COREVIDEO_FRAMEWORK" in cmake and
        "FOUNDATION_FRAMEWORK" in cmake,
        "iOS target must link Metal, CoreVideo and Foundation")
require("kCVPixelBufferOpenGLESCompatibilityKey" in metal and
        "kCVPixelBufferMetalCompatibilityKey" in metal and
        "kCVPixelFormatType_32BGRA" in metal and
        "MTLPixelFormatBGRA8Unorm" in metal and
        "GL_BGRA_EXT" in metal and
        "GL_UNSIGNED_INT_8_8_8_8_REV" in metal,
        "shared pixel-buffer/Metal/GL formats must match Apple's iOS interoperability format table")
require("CVMetalTextureCacheCreateTextureFromImage" in metal and
        "CVOpenGLESTextureCacheCreateTextureFromImage" in metal,
        "one CVPixelBuffer must expose both Metal and OpenGL ES texture views")
require("newComputePipelineStateWithFunction" in metal and
        "bismuthCullSprites" in metal and "dispatchCulling(" in metal and
        "dispatchThreadgroups" in metal and "waitUntilCompleted" in metal,
        "Metal must compute per-sprite visibility and synchronize before the GLES consumer")
require("CVOpenGLESTextureCacheFlush" in metal and
        "output.write(float4(value, value, value, 1.0)" in metal,
        "Metal visibility results must be written to the shared pixel buffer")
require("updateMetalCulling()" in gpu and "getPreCulledSprites()" in gpu and
        "getGroupStates()" in gpu and "u_visibilityTexture" in gpu and
        "cullingMethodFor(renderMethod)" in gpu,
        "the selectable renderer must feed group/sprite inputs and bind its selected visibility mask")
metal_cull_start = gpu.index("bool IOSGPURenderer::updateMetalCulling()")
metal_cull_end = gpu.index("void IOSGPURenderer::drawLayer", metal_cull_start)
metal_cull = gpu[metal_cull_start:metal_cull_end]
require("metalInterop.isReady()" in metal_cull and
        "metalInterop.selfTestPassed()" in metal_cull and
        "fail(" in metal_cull and "return false;" in metal_cull and
        "metalInterop.reset()" not in metal_cull and
        "using GLES culling" not in metal_cull,
        "Metal culling must fail closed on initialization, scheduler, and dispatch errors")
require("using GLES culling" not in gpu and "GLES fallback" not in gpu,
        "no runtime code or status label may silently advertise a GLES culling fallback")
selected_culling = body(gpu, "bool IOSGPURenderer::initializeSelectedCulling()", "bool IOSGPURenderer::initializeCPUVisibility()")
require("if (cullingMethod == 0 || cullingMethod == 3)" in selected_culling and
        "metalInterop.initialize(" in selected_culling and
        "metalInterop.runSelfTest(" in selected_culling,
        "Metal must be an optional selected culling method, not a startup requirement")
vertex_shader=(ROOT/"resources/shaders/object_es2.vert").read_text()
require("u_visibilityMode < 0.5" in vertex_shader and
        "u_visibilityMode < 1.5" in vertex_shader and
        "u_visibilityTexture" in vertex_shader and
        "u_cameraPosition" in vertex_shader,
        "the active ES2 shader must support shader, CPU-mask, Metal-mask and no-cull modes")
require("metalInterop.runSelfTest(error)" in gpu and
        "metalInterop.selfTestPassed()" in gpu,
        "the selected Metal method must execute and retain its shared-texture self-test")
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
require("restoreStockBatches();" in fail_body and "hideStockBatches();" not in fail_body,
        "failed rendering must restore Geometry Dash's stock batch visibility")
ownership = body(gpu, "void IOSGPURenderer::finishFrame()", "void IOSGPURenderer::recordGameLayerUpdate")
require("lastFrameDraws >= activeLayerCount" in ownership and
        "PASSED FIRST FRAME" in ownership,
        "GPU rendering must prove a full frame before taking Cocos visual ownership")
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

print("PASS: selectable iOS renderer activation, shader/culling modes, Metal optionality, frame state, and safe stock-batch ownership")
