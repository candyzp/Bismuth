# Bismuth upstream-to-iOS port audit

Audit date: 2026-10-31

Repositories compared:
- Upstream: https://github.com/iliasHDZ/Bismuth, master at `12536eb964022e16985b428228ba3f36923ba3de`
- iOS fork baseline: https://github.com/candyzp/Bismuth, master at `d31115ee3851655c6671233bc13b1a42e0d7e157`
- Active port branch: `rework/ios-gpu-v2` (reviewed separately from master; no changes are made to master)
- Branch relation at review start: 194 commits ahead of fork master, 0 behind

This is a source audit and source-port record for steps 2 and 3, not a successful build report. The owner-style CPU modules and the ES2 GPU evaluation/draw path live under `src/renderer/ios_upstream/`. They are connected in the new GPU-only renderer, but have not been compiled or device-tested.

## Findings

### Files and systems already aligned with upstream

- `src/renderer/ObjectSorter.cpp/.hpp`: identical blobs in the audited master branches.
- `src/renderer/SpriteMeshDictionary.cpp/.hpp`: identical blobs.
- `src/BProfiler.cpp/.hpp`: identical blobs.
- The Geode/iOS build workflow and the OpenGLES include shim are fork-specific and should be retained.

### Major missing upstream systems

| Upstream component | Fork state | Required iOS work |
|---|---|---|
| `WorkGroupScheduler.hpp/.cpp` | Ported in `IOSWorkGroupScheduler` | CPU sorting, per-layer ordering, sprite remapping, pre-culled records, and work-group ranges. The desktop OpenCL GPU-core-count heuristic is replaced by a fixed provisional batch size pending iOS measurements. |
| `SAOBuffer.hpp` | Data model and ES2 transport implemented | Owner record meanings are represented in `IOSRenderRecords.hpp`; `IOSGPUStateTables` serializes them into float/byte textures and the VBO carries per-vertex indices instead of SSBOs. |
| `resources/shaders/headers/*` | Record semantics and ES2 shader lookups implemented | Flags, group state, pulse entries, HSV encoding, game state, color channels, and input-sprite records are represented in CPU records and decoded by the GLSL ES 1.00 vertex shader. |
| `objectCulling.comp` | Calculation body ported into `object_es2.vert` | GPU vertex processing evaluates group transforms, camera culling, audio scale, color channels, HSV, pulses, invisible-block fade/glow, and premultiplied opacity. |
| `objectDrawing.vert/.frag` | Ported to `object_es2.vert/.frag` | The vertex shader applies the owner mesh transform and resolved vertex color; the fragment shader samples the sprite sheet and multiplies by that color. |
| `Renderer.cpp` | CPU initialization and ES2 GPU draw owner implemented | `IOSSceneData` prepares owner-style records; `IOSGPURenderer` uploads ordered vertices/state textures, creates layer nodes, binds uniforms, applies the owner blend mode, draws each scheduled range, and restores Cocos GL state. |
| `DifferenceMode`, `OverdrawView` | Not ported | Optional diagnostics; keep BProfiler and the fork's existing debugger. Not required for the core renderer. |
| `GJEffectManager` decomp helpers | Not required by current CPU port | The iOS GroupManager reads the same live pulse/color action data directly. Revisit only if shader parity reveals a binding gap. |
| `GPUTimer` | Capability reporting added | The live context report checks timer-query support; step 4 can add an extension-backed timer without importing desktop-only query assumptions. |

### Shared files that have diverged

**GroupManager**
- Ported in `IOSGroupManager.hpp/.cpp`: explicit empty combination index 0, per-group toggle state, vanilla-rotation group identification, alpha grouping, pulse-entry generation, movement/rotation state, and checkpoint store/restore.
- The iOS port deliberately fixes the upstream alpha-combination filtering typo by filtering the `alphaCombination` copy rather than the unfiltered transform combination.
- Runtime move, optimized rotation, follow, toggle and checkpoint hooks now mirror the owner's state into the active iOS SceneData. The owner-style optimized rotation routine keeps non-decoration physics/oriented-box updates in Geometry Dash while decoration transforms are GPU state.
- The fork's older disabled-group / scaled-state implementation is not used as the new renderer's source of truth.

**ObjectBatch**
- Ported in `IOSObjectBatch.hpp/.cpp`: owner-style object metadata, sprite-tree expansion, rotated/flipped texture-rect mapping, convex mesh triangulation, six-vertex quads, per-sprite input records, source-sprite lifetime pins, and scheduler-produced layer ranges.
- Source vertex/input arrays remain in their original owner order. The scheduler's remap/pre-culled arrays express draw order; they are not also physically reordered. The port validates a complete permutation and checks contiguous layer/work-group coverage before accepting the GPU draw batch.
- iOS-specific direct displayed-tint metadata is retained as a sidecar record; encoded color flags are masked only for blend-channel lookup.
- The owner's blend-state map is refreshed every frame before rendering. The iOS port now requests the matching `WorkGroupScheduler` variant once in `PlayLayer::updateVisibility`, before Cocos visits the per-layer GPU nodes; it keeps the last valid order while a new variant is generated asynchronously, and switches the ordered VBO before the frame's layer ranges are consumed.
- ES2 has no upstream compute/SSBO/indirect-draw equivalents, so the owner culling calculations run in the GPU vertex shader and the CPU submits the scheduler's contiguous triangle-list ranges with `glDrawArrays`. This combines the owner's two GPU stages without moving final transforms/colors to the CPU.
- The fork's old `VisibilityManager`, section culling and live-VBO-refresh implementation are not the base of this port.

**ObjectUtils**
- The transactional visual-tree validation remains because it prevents invalid Cocos child casts and duplicate sprite records; an unsupported tree rejects the GPU beta build rather than being routed to stock rendering.
- Owner-style sprite order, base/detail/glow classification, root-versus-child black flags, tag-997 exception, and mesh lookup semantics are retained. The channel resolver follows upstream's `sprite == object` test for `m_isObjectBlack` versus child `m_isColorSpriteBlack`; the iOS direct-tint path remains a separate channel-0 adaptation.
- The iOS direct-tint metadata for legacy/default channel 0 is kept in the high color-channel bits and carried into the new per-sprite record. The scheduler strips those flags only when indexing the blend-state table.

**Renderer / draw nodes**
- `IOSSceneData` ports the owner's CPU initialization/data-refresh sequence: sprite-sheet/mesh setup, full-level GroupManager initialization, ObjectSorter traversal, ObjectBatch build, persistent color-channel table refresh using `m_copyColor`, game/camera/audio state, and draw-time MVP/camera refresh.
- Gameplay hooks mirror move, optimized rotation, follow, toggle and checkpoint state into the active SceneData; Geometry Dash continues to update non-decoration object positions, sections, collision and oriented boxes.
- The CPU scene-data module is owned by `IOSGPURenderer`. The renderer uses three vertex-readable state textures, a source-indexed ordered VBO, and GPU layer nodes keyed by the upstream scheduler. Runtime blend changes now trigger the owner's asynchronous sorted-batch variant selection once before GPU layer traversal, so all layer nodes in a frame share one stable range map. The active PlayLayer's stock Cocos batches stay hidden; shader, table, or draw failures stop the GPU path and do not fall back.
- The legacy `RendererIOS.cpp`, `ResolvedStateLayer`, `AssistShadowBatch`, `StandaloneAssistBatch`, and `AtlasInterleave` are not dependencies of the new renderer path. When the new GPU renderer exists, the iOS batch hook explicitly blocks legacy Cocos batch draws. Initialization and draw failures keep stock batches hidden; they do not route to stock rendering.


**Shader and buffer APIs**
- The fork's `ShaderIOS.cpp` compiles simple vertex/fragment shaders only.
- The owner uses desktop GLSL 4.5 / 4.6 preprocessing and OpenGL 4.3 compute shaders.
- The fork's `Buffer.hpp` has iOS no-op implementations for uniform/storage-buffer binding. Its `BufferIOS.cpp` is not a working SSBO backend.
- Desktop `src/gl.*`, `src/cl.*`, WGL loading, OpenCL/GL interop and GPU-core-count discovery are deliberately not ported.

### Game lifecycle integration now present in the iOS hooks

The iOS hooks mirror the owner's state split when an `IOSSceneData` instance is active:
- move deltas are accumulated into GroupManager while vanilla non-decoration object data is advanced;
- optimized rotation uses the owner's `vanillaRotateGroups` set, `claimRotationAction` / `claimMoveAction`, object-section updates, and oriented-box maintenance;
- move processing still advances both remaining static and optimized non-decoration objects in Geometry Dash after GPU decorations have been removed from those arrays;
- follow deltas and group toggles are mirrored into GroupManager;
- checkpoint group states are stored, restored and removed with checkpoint lifetime;
- decorations are removed from vanilla move groups only after the new SceneData renderer is active.
- if GPU-renderer construction fails before an owner node exists, the PlayLayer hook explicitly hides stock batch nodes; the active GPU renderer also blocks legacy Cocos batch draws.

The hooks continue to call Geometry Dash's gameplay/physics routines for the non-decoration objects. SceneData update and draw-time uniform refresh are separate so the renderer can capture the correct camera/MVP state immediately before GPU submission.

## iOS GPU API research

### Actual project context

The fork's `libs/ios-shims/Geode/cocos/platform/win32/CCGL.h` includes Apple's `OpenGLES/ES2/gl.h` and `glext.h`. This establishes an ES2-compatible compile surface, but does not prove the exact runtime context version; `IOSGPUCapabilities` queries the live context at gameplay initialization.

### Native OpenGL ES techniques already present in the fork

The existing iOS backend provides useful low-level patterns, even though its old hybrid renderer is not the architecture being continued:

- `src/renderer/ios/DataTexture.cpp` allocates one-row/multi-row RGBA textures, uses `GL_NEAREST` and `GL_CLAMP_TO_EDGE`, and supports `GL_FLOAT` or `GL_UNSIGNED_BYTE` texels.
- `DataTexture::uploadRange/uploadRanges` updates changed rows through `glTexSubImage2D`, preserving the current texture binding and splitting a range across partial/full texture rows.
- `src/renderer/ios/ShaderIOS.cpp` compiles and links an ES vertex/fragment pair, pins attribute locations with `glBindAttribLocation`, and binds texture units/uniforms.
- `resources/shaders/object_ios.vert` demonstrates vertex-stage state-table sampling with `texture2D`; its legacy table layout is a reference for ES2 techniques only, not the new renderer architecture.
- `src/renderer/ios/BufferIOS.cpp` uses ordinary `GL_ARRAY_BUFFER` plus `glBufferSubData` and optional CPU shadow memory. Its no-op SSBO binding methods are not a usable implementation of the desktop owner's storage-buffer API.

Apple's OpenGL ES documentation also describes:
- runtime capability queries such as `GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS`;
- vertex-shader texture lookups for transform data;
- render-to-texture using framebuffer objects, including GPU-produced data used by a later vertex-processing stage;
- texture-unit reuse to avoid unnecessary extra rendering passes.

The new live capabilities report logs vertex texture units, fragment highp support/range, float/half-float texture support, float/half-float color targets, MRT, and timer-query extensions separately. The ES2 renderer requires three vertex texture units, four combined texture units, five attributes, fragment highp for atlas coordinates, and float texture sampling; unsupported contexts fail GPU initialization. No desktop SSBO calls are sent through the iOS buffer shim. Vertex-attrib state restoration now restores both VBO-backed and client-memory descriptors, as ES2 permits vertex data from application memory as well as buffer objects.

### Metal option

Apple documents Metal/OpenGL ES interoperation using a shared `CVPixelBuffer` with both Metal and OpenGL ES compatibility, exposed to each API through its texture cache. Metal provides true compute encoders, GPU buffers and thread dispatch.

This is a viable alternative backend to investigate, but it is not a drop-in replacement for Cocos' current GL draw path. It would require an Objective-C++ Metal layer, resource import for the sprite sheets, per-layer output surfaces, and explicit cross-API synchronization. The first port should avoid introducing this second backend before the existing context's ES capabilities are measured.

## Chosen first iOS translation

Keep the owner's CPU preparation and ordering model, but translate the owner's compute-stage work into the iOS GL shader pipeline:

1. CPU: owner-style object/sprite collection, `ObjectInfo`, `InputSprite`, `PreCulledSprite`, `GroupCombinationState`, color-channel and pulse records.
2. CPU: owner-style layer sorting by Z order, sprite sheet and blending state.
3. GPU: ES-compatible shader reads the input/state tables and performs group transforms, camera culling, audio scale, color selection, HSV and pulse evaluation.
4. GPU: vertex stage applies the computed transforms to the owner's mesh data; fragment stage samples the sprite sheet and applies resolved color.
5. CPU: submits only ordered per-layer draw ranges. It does not calculate final sprite transforms or final colors.

This preserves the upstream division of responsibility—CPU builds the scene data and render order; GPU evaluates render state and draws—without trying to call unavailable desktop compute/SSBO functions from ES2.

If vertex texture fetch or the required data formats are unavailable in the live context, the GPU-only beta fails initialization visibly. It must not route that content to Cocos stock rendering.

## Step 2 port checklist

- [x] Compare both repository trees and shared renderer files.
- [x] Identify upstream-only files and fork-only hybrid renderer systems.
- [x] Inspect upstream GroupManager, ObjectBatch, WorkGroupScheduler, shader records, object-culling shader and renderer flow.
- [x] Inspect the fork's OpenGLES ES2 shim, DataTexture, iOS shader compiler and buffer implementation.
- [x] Add live GL capability reporting, including vertex texture units, texture formats, color-target formats, MRT and timer-query support.
- [x] Port owner record meanings into ES-compatible iOS CPU records.
- [x] Port owner-style layer sorting, sprite remapping, initial blend-state map, live blend-state refresh, asynchronous sorted-variant switching, and work-group range generation.
- [x] Port GroupManager combinations, movement/rotation/toggle state, alpha and pulse data, vanilla-rotation groups and checkpoints.
- [x] Port ObjectBatch object/sprite metadata, mesh construction, texture-rect transforms/flips, source-order buffers, scheduler remapping and lifetime management.
- [x] Port SceneData initialization plus live color-channel/game/camera/audio state capture.
- [x] Port move/optimized-rotation/follow/toggle/checkpoint gameplay-state hooks.
- [x] Match upstream's renderer creation point inside `setupHasCompleted()` / `resetLevel()`.
- [x] Close the stock-render escape paths when GPU-renderer initialization or owner-node creation fails.
- [x] Re-audit blend-flag masking, raw scheduler Z-layer keys, direct-tint channel-0 sorting, sprite-remap bounds, and optimized-group movement.
- [x] Step 3 source implementation: serialize owner records into ES2 texture tables, port the culling/evaluation calculations to the vertex shader, add the sprite-sheet fragment shader, upload scheduler-ordered geometry, and submit GPU layer ranges.
- [x] Step 3 source audit: validate byte packing, float-index precision, pulse sentinels/limits, scheduler permutations/range coverage, texture units, vertex attributes, blend functions, and GL-state restoration.
- [x] Step 4 source review: compare the full upstream draw/evaluation path, runtime blending, sprite geometry/atlas transforms, object/group transforms, color/HSV/pulse/opacity/glow/invisible-block math, visibility/culling, camera/MVP/audio updates, lifecycle/reset/cleanup, empty scenes, and ES2 state/precision constraints.
- [x] Step 4 source corrections: restore per-frame blend-state scheduling before layer traversal; correct root-versus-child black color-channel selection; restore all vertex-attrib pointer descriptors; use highp atlas UV varyings and require/query fragment highp support.
- [ ] Step 4 empirical validation: compile/link the ES2 shaders and compare actual visuals/gameplay on an iOS device. This remains unverified until the Step 5 build/device run.
- [ ] Step 5: build the iOS beta, resolve compile/link/shader failures, and optimize only after the source-level parity review.

**Step 2 status:** Upstream comparison, the iOS GPU API audit, and the owner-style CPU preparation/lifecycle port are complete on `rework/ios-gpu-v2`.

**Step 3 status:** The ES2 GPU renderer, texture tables, shader calculations, ordered layer submission, and no-fallback failure handling are implemented at source level. They are not yet compiled, shader-validated on device, or visually tested.

**Step 4 status:** The source-level parity/compatibility review is complete to the extent possible without a compiled iOS binary. The review found and corrected the runtime blend-schedule gap, the black-sprite channel mismatch, incomplete ES2 vertex-attrib state restoration, and mediump atlas-coordinate precision. The source review does not prove shader compilation or visual/gameplay parity on hardware.

**Remaining Step 4 risks:** The current upstream texture map intentionally leaves `TEXT` and `SPECIAL` sheets unassigned; a level requiring an unavailable sheet, or a sprite tree with non-sprite children/duplicate sprite nodes, causes strict GPU initialization to fail. Pulse lists beyond the iOS ES2 shader's fixed loop limit are rejected. Unsupported/failed paths remain visible GPU-beta failures with no stock fallback. These cases require explicit validation against real levels.

**Build status:** No build has been run. Step 5 has not started.
