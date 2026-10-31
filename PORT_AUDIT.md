# Bismuth upstream-to-iOS port audit

Audit date: 2026-10-31

Repositories compared:
- Upstream: https://github.com/iliasHDZ/Bismuth, master at `12536eb964022e16985b428228ba3f36923ba3de`
- iOS fork: https://github.com/candyzp/Bismuth, master at `d31115ee3851655c6671233bc13b1a42e0d7e157`
- Port branch: `rework/ios-gpu-v2`

This is a source audit and step-2 port record, not a successful build report. The owner-style CPU modules live under `src/renderer/ios_upstream/`. They are now connected to the new GPU-renderer owner, but the GPU shader/table/draw path remains draft work for step 3 and has not been compiled or device-tested.

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
| `SAOBuffer.hpp` | Data model ported | Owner record meanings are represented in `IOSRenderRecords.hpp`; step 3 serializes them into ES-compatible texture/attribute tables rather than SSBOs. |
| `resources/shaders/headers/*` | CPU record semantics ported | Flags, group state, pulse entries, HSV encoding, game state, color channels, and input-sprite records are represented. The GLSL ES lookup helpers and texture layouts remain step 3. |
| `objectCulling.comp` | Not yet ported | Step 3 translates the per-sprite visibility, transform, audio-scale, group, color, HSV, and pulse calculations into the iOS GPU vertex-processing path. |
| `objectDrawing.vert/.frag` | Not yet ported | Step 3 ports final vertex transform and sprite-sheet sampling to GLSL ES. |
| `Renderer.cpp` | CPU initialization flow ported to `IOSSceneData`; a separate iOS GPU-renderer owner is present | Step 3 audits and completes the ES2 state-table/shader/draw path and checks it against current upstream behavior. |
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
- Source vertex/input arrays remain in their original owner order. The scheduler's remap/pre-culled arrays express draw order; they are not also physically reordered. The port validates that the remap is a complete permutation before step 3.
- iOS-specific direct displayed-tint metadata is retained as a sidecar record; encoded color flags are masked only for blend-channel lookup.
- The owner's blend-state map is fetched during renderer initialization and remains the scheduler key for that level; the iOS port follows that fixed per-level schedule.
- GPU-produced raw-sprite state, culling-index generation, and final draw submission remain step 3.
- The fork's old `VisibilityManager`, section culling and live-VBO-refresh implementation are not the base of this port.

**ObjectUtils**
- The transactional visual-tree validation remains because it prevents invalid Cocos child casts and duplicate sprite records; an unsupported tree rejects the GPU beta build rather than being routed to stock rendering.
- Owner-style sprite order, base/detail/glow classification, black-sprite output type, tag-997 exception, and mesh lookup semantics are retained.
- The iOS direct-tint metadata for legacy/default channel 0 is kept in the high color-channel bits and carried into the new per-sprite record. The scheduler strips those flags only when indexing the blend-state table.

**Renderer / draw nodes**
- `IOSSceneData` ports the owner's CPU initialization/data-refresh sequence: sprite-sheet/mesh setup, full-level GroupManager initialization, ObjectSorter traversal, ObjectBatch build, persistent color-channel table refresh using `m_copyColor`, game/camera/audio state, and draw-time MVP/camera refresh.
- Gameplay hooks mirror move, optimized rotation, follow, toggle and checkpoint state into the active SceneData; Geometry Dash continues to update non-decoration object positions, sections, collision and oriented boxes.
- The CPU scene-data module is now owned by the iOS GPU renderer. The current `IOSGPURenderer`, `IOSGPUStateTables`, and ES2 shaders are step-3 work-in-progress; their presence does not mean the GPU path is validated.
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

The new live capabilities report logs vertex texture units, float/half-float texture support, float/half-float color targets, MRT, and timer-query extensions separately. Step 3 should use vertex texture fetch and CPU-updated data textures for the owner-style records; no desktop SSBO calls should be sent through the iOS buffer shim.

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
- [x] Port owner-style layer sorting, sprite remapping using the initialization-time blend-state map, and work-group range generation.
- [x] Port GroupManager combinations, movement/rotation/toggle state, alpha and pulse data, vanilla-rotation groups and checkpoints.
- [x] Port ObjectBatch object/sprite metadata, mesh construction, texture-rect transforms/flips, source-order buffers, scheduler remapping and lifetime management.
- [x] Port SceneData initialization plus live color-channel/game/camera/audio state capture.
- [x] Port move/optimized-rotation/follow/toggle/checkpoint gameplay-state hooks.
- [x] Match upstream's renderer creation point inside `setupHasCompleted()` / `resetLevel()`.
- [x] Close the stock-render escape paths when GPU-renderer initialization or owner-node creation fails.
- [x] Re-audit blend-flag masking, raw scheduler Z-layer keys, direct-tint channel-0 sorting, sprite-remap bounds, and optimized-group movement.
- [ ] Step 3: finish/audit GPU texture-table serialization, ES shader parity, and ordered layer draw submission. Draft code exists, but is not verified.
- [ ] Step 4: verify visual parity, effect coverage, lifecycle stability, and performance instrumentation on device.
- [ ] Step 5: run the iOS build and address compile/link failures.

**Step 2 status:** Source comparison, the iOS GPU API audit, and the owner-style CPU preparation/lifecycle port are complete on `rework/ios-gpu-v2`. The separate GPU renderer and shaders already present in the branch are the next step, not a completed or tested renderer.

**Build status:** No build has been run yet. Build verification remains reserved for step 5.
