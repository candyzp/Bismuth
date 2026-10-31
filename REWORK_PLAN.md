# Bismuth iOS GPU Rework

## Direction

This is a GPU-only beta port of the current upstream Bismuth renderer. The implementation must follow the upstream renderer's data flow and rendering model, not extend the previous iOS `ResolvedStateLayer` / `AssistShadowBatch` experiment.

- Upstream reference: `iliasHDZ/Bismuth` master at `12536eb964022e16985b428228ba3f36923ba3de` (October 31, 2026)
- iOS repository: `candyzp/Bismuth`
- Work branch: `rework/ios-gpu-v2`
- Fork baseline: `d31115ee3851655c6671233bc13b1a42e0d7e157`
- Target game: Geometry Dash 2.2081, iOS arm64

## GPU-only beta rule

There is no stock-render fallback in the new renderer.

If the GPU backend, shaders, buffers, work scheduling, or a draw fails, the beta reports the failure and the affected render does not get silently handed back to Cocos. A broken GPU build is a failed beta build; it is not disguised as a successful hybrid renderer.

Do not preserve the old hybrid ownership/fallback design in the new path. Keep the old code only as an archived reference until the upstream port replaces it. Do not allow both old and new renderers to claim the same draw.

## Preserve from the iOS fork

- Geode/iOS build plumbing and device target
- BProfiler and profiler diagnostics
- Existing debug UI/settings that are independent of the old renderer
- Sprite-sheet resources and valid mesh/object metadata
- Required game-version bindings and iOS lifecycle integration

Debugger counters must describe actual GPU work and completed submissions, not registered ownership or intended draws.

## Port the upstream architecture

Follow the owner's current source structure and concepts:

1. `ObjectSorter`: enumerate and organize supported GameObjects and their visual sprites.
2. `GroupManager`: build group-combination state for move/rotate/alpha/toggle/pulse behavior.
3. `ObjectBatch`: construct ordered sprite geometry and input sprite records.
4. `WorkGroupScheduler`: sort by Z layer, sprite sheet, and blending state; partition work into work groups; prepare sprite remaps and draw ranges.
5. GPU culling/evaluation pass: process group transforms, camera culling, audio scale, color channels, HSV, pulse entries, opacity, and raw sprite output.
6. GPU drawing pass: use the prebuilt vertex/index data and the GPU-produced sprite state to render in the exact layer order.

The upstream sources to use as the reference are:
- `src/renderer/ObjectSorter.cpp`
- `src/renderer/ObjectBatch.cpp`
- `src/renderer/GroupManager.cpp`
- `src/renderer/WorkGroupScheduler.cpp`
- `src/renderer/SAOBuffer.hpp`
- `src/renderer/Renderer.cpp`
- `resources/shaders/objectCulling.comp`
- `resources/shaders/objectDrawing.vert`
- `resources/shaders/objectDrawing.frag`
- `resources/shaders/headers/shared.h` and its buffer headers

Port the owner's algorithms and semantics. Do not copy Windows-only API glue, OpenCL core-count discovery, WGL function loading, or desktop-only shader directives as if they were iOS-compatible.

## iOS graphics API constraint

Apple documents the iOS OpenGLES framework as implementing OpenGL ES 1.1, 2.0, and 3.0. The fork's own iOS shim explicitly includes `OpenGLES/ES2/gl.h` / `glext.h` and documents Geometry Dash as using a Cocos OpenGL ES 2 context. Treat ES2 as the actual target until the runtime capability report proves otherwise.

The upstream renderer requires desktop OpenGL 4.3 features including compute shaders, shader-storage buffers, and indirect multi-draw. Those APIs are not available in the game's documented/current ES2 path. Preserve the owner's data preparation, work-group scheduling, ordering, and per-sprite calculations; translate the culling/transform/color evaluation into the ES2 GPU vertex path, using GPU-readable state tables only after confirming the context's vertex-texture support. CPU scheduling issues the corresponding precomputed `glDrawArrays` ranges over the expanded triangle-list vertex stream. This is an API translation of the owner's renderer, not a continuation of the old hybrid assist design.

Before implementation, use the new runtime capability report to verify the live context version, GLSL ES version, texture limits, vertex texture units, extensions, and supported formats. Do not assume ES 3.0 merely because the framework can provide it.

## Build sequence

### 0. Isolate the new implementation
- Keep all work on `rework/ios-gpu-v2`.
- Retain the existing profiler/debug UI.
- Stop modifying the legacy `RendererIOS.cpp` pipeline.
- Build the new renderer in a separate iOS backend area and wire it in only when its first milestone is ready.

### 1. Establish the actual iOS GPU contract
- Query and log the current GL and GLSL ES versions.
- Query required limits and extensions.
- Verify shader compilation, required texture/buffer formats, framebuffer support, and draw submission.
- Fail the GPU beta initialization on unsupported requirements; do not switch to stock rendering.

### 2. Port upstream data structures and CPU preparation
- Port ObjectInfo, InputSprite, GroupCombinationState, WorkGroup, and draw-range concepts.
- Port ObjectSorter, GroupManager, ObjectBatch, and WorkGroupScheduler in upstream order.
- Keep CPU work to scene enumeration, sorting, scheduling, and game-state capture that upstream already performs on the CPU.

### 3. Port GPU evaluation and drawing
- Port the logic of `objectCulling.comp` without changing game semantics.
- Port the logic of `objectDrawing.vert` / `objectDrawing.frag`.
- Replace unsupported desktop API mechanisms only where the iOS graphics API requires it; keep the upstream record layouts, indexing, ordering, and shader calculations aligned.
- No CPU rasterization or stock Cocos draw substitution.

### 4. Validate visual and gameplay parity
- Compare against upstream behavior for movement, rotation, blending, color, HSV, pulse, glow, audio scale, and camera culling.
- Geometry Dash remains authoritative for gameplay, triggers, physics, collision, and simulation timing.
- The GPU renderer owns rendering in this beta.

### 5. Optimize after parity
- Measure CPU preparation time, GPU evaluation time, GPU drawing time, upload volume, draw calls, and frame pacing.
- Tune work-group size using real iOS measurements rather than desktop GPU core-count heuristics.
- Preserve the owner's work-group scheduling and batching intent.

## Beta correctness gates

1. GPU rendering is the only renderer in the new beta path.
2. A failed GPU draw is reported as a failure; it is never silently replaced by a stock draw.
3. No duplicate GPU submissions for the same sprite.
4. Z order, sprite-sheet selection, blend state, transforms, visibility, and color must match the supported upstream behavior.
5. No changes to gameplay, collision, physics, trigger execution, or simulation timing.
6. Performance claims must come from measured frame and GPU timings, not sprite counts alone.

## Current execution status

### Step 2 — upstream audit and CPU-side iOS port
Completed on `rework/ios-gpu-v2` at source level:
- Audited the current upstream renderer, data records, shaders, ObjectSorter, ObjectBatch, GroupManager, WorkGroupScheduler, and draw-layer ordering.
- Audited the fork's ES2 context shim plus DataTexture, ShaderIOS, BufferIOS, and runtime capability reporting.
- Ported owner-style CPU records, group/pulse/checkpoint state, object/sprite mesh construction, scheduler remaps, SceneData initialization, color/game/camera/audio capture, and gameplay-state hooks.
- Closed the stock-render escape paths when the new GPU renderer owns the PlayLayer or initialization fails.

### Step 3 — GPU evaluation and drawing
In progress on `rework/ios-gpu-v2`:
- Added texture-backed object/group/color/HSV/pulse/sprite-appearance tables for ES2.
- Ported the owner culling/evaluation calculations into a vertex shader and sprite-sheet sampling into the fragment shader.
- Added ordered VBO uploads, one GPU draw node per active owner layer range, upstream blend functions, and Cocos GL-state restoration.
- Reconciled shader byte packing, direct-tint handling, pulse sentinels/loop limits, Z-layer keys, empty scenes, and renderer failure propagation against upstream source.

Still required before step 3 can be called complete:
- Build the shader and renderer with the iOS toolchain and fix compiler/linker errors.
- Verify texture-table contents, shader attribute/uniform bindings, and actual draw submissions on device.
- Compare visual output for color, HSV, pulses, glow, invisible blocks, rotation, audio scale, culling, and sprite-sheet ordering.

This source work is not yet compiled or device-tested. No Cocos fallback has been added.

Do not start from or extend the old `AssistShadowBatch`, `ResolvedStateLayer`, or atlas-interleaving renderer.
