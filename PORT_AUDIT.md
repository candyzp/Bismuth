# Bismuth upstream-to-iOS port audit

Audit date: 2026-10-31

Repositories compared:
- Upstream: https://github.com/iliasHDZ/Bismuth, master at `12536eb964022e16985b428228ba3f36923ba3de`
- iOS fork: https://github.com/candyzp/Bismuth, master at `d31115ee3851655c6671233bc13b1a42e0d7e157`
- Port branch: `rework/ios-gpu-v2`

This is a source audit, not a successful build report. The new port modules are separate from the old renderer until the new path is ready to replace it.

## Findings

### Files and systems already aligned with upstream

- `src/renderer/ObjectSorter.cpp/.hpp`: identical blobs in the audited master branches.
- `src/renderer/SpriteMeshDictionary.cpp/.hpp`: identical blobs.
- `src/BProfiler.cpp/.hpp`: identical blobs.
- The Geode/iOS build workflow and the OpenGLES include shim are fork-specific and should be retained.

### Major missing upstream systems

| Upstream component | Fork state | Required iOS work |
|---|---|---|
| `WorkGroupScheduler.hpp/.cpp` | Missing | Port CPU sorting, layer grouping, sprite remapping and draw-range generation. Remove the desktop OpenCL GPU-core-count dependency. |
| `SAOBuffer.hpp` | Missing | Port the owner data model, but serialize the records into ES-compatible texture/attribute tables rather than SSBOs. |
| `resources/shaders/headers/*` | Missing | Port the record definitions, flags, HSV and pulse semantics into an ES shader data layout. |
| `objectCulling.comp` | Missing | Translate the per-sprite visibility, transform, audio-scale and color calculations into the iOS GPU pipeline. |
| `objectDrawing.vert/.frag` | Missing | Port the final per-vertex transform and sprite-sheet sampling to GLSL ES. |
| `Renderer.cpp` | Missing | Replace the legacy `RendererIOS.cpp` architecture with the owner-style object/group/batch lifecycle. |
| `DifferenceMode`, `OverdrawView` | Missing | Optional debug facilities; not a blocker for the core renderer. |
| `GJEffectManager` decomp helpers | Missing | Needed for the owner's pulse/color parity helpers where direct bindings don't expose the required methods. |
| `GPUTimer` | Missing | Use an iOS-supported timer extension only when advertised; retain the existing CPU profiler independently. |

### Shared files that have diverged

**GroupManager**
- Upstream adds explicit empty group-combination index 0, per-group toggle state, vanilla-rotation group identification, and a pulse-entry table.
- Upstream updates group alpha and pulse state every frame and stores/restores group state at checkpoints.
- The fork has an older disabled-group / scaled-state implementation with different rotation handling. This must not be used as the new renderer's source of truth.

**ObjectBatch**
- Upstream builds static vertices plus per-sprite `InputSprite` records, and uses the work-group scheduler to order sprite processing by Z layer, sprite sheet and blending state.
- It adds GPU-produced `RawSprite` state and culling indices.
- The fork's implementation adds `VisibilityManager`, section culling and per-frame live-sprite VBO refresh for the former hybrid renderer. Those additions are not the owner's current architecture and are not the base of this port.

**ObjectUtils**
- The fork contains extra transactional sprite-tree validation added for hybrid ownership and CPU fallback.
- The owner uses a simpler unpacking path consistent with its static object/sprite input model. Port the owner's current object/sprite semantics; keep any proven iOS safety checks only where they do not change ownership or silently demote a GPU draw to stock.

**Renderer / draw nodes**
- The fork uses `RendererIOS.cpp`, `ResolvedStateLayer`, `AssistShadowBatch`, `StandaloneAssistBatch`, `AtlasInterleave`, and Cocos stock-batch hooks.
- The owner uses a single renderer lifecycle: collect and sort objects; initialize group/object data; prepare per-layer batches; update uniforms and group state; evaluate GPU state; draw ordered layer ranges.
- The old iOS system is retained only as historical reference. The new renderer must not depend on its ownership registry or hybrid fallback.

**Shader and buffer APIs**
- The fork's `ShaderIOS.cpp` compiles simple vertex/fragment shaders only.
- The owner uses desktop GLSL 4.5 / 4.6 preprocessing and OpenGL 4.3 compute shaders.
- The fork's `Buffer.hpp` has iOS no-op implementations for uniform/storage-buffer binding. Its `BufferIOS.cpp` is not a working SSBO backend.
- Desktop `src/gl.*`, `src/cl.*`, WGL loading, OpenCL/GL interop and GPU-core-count discovery are deliberately not ported.

### Game lifecycle integration missing from the iOS hooks

The current iOS hooks call Geometry Dash's original move, rotation and follow methods, but do not mirror the resulting owner-style group state into the GPU group-state table. The upstream integration also:
- tracks move, rotate, follow and toggle group state;
- stores/restores group state at checkpoints;
- identifies groups whose objects must continue rotating under vanilla logic;
- removes GPU-rendered decorations from vanilla movement arrays once the GPU renderer owns them.

These hooks must be ported carefully so gameplay, collision and physics remain Geometry Dash-owned while the renderer receives the matching visual state.

## iOS GPU API research

### Actual project context

The fork's `libs/ios-shims/Geode/cocos/platform/win32/CCGL.h` includes Apple's `OpenGLES/ES2/gl.h` and `glext.h`. This establishes an ES2-compatible compile surface, but does not prove the exact runtime context version; `IOSGPUCapabilities` queries the live context at gameplay initialization.

### Native OpenGL ES techniques

Apple's OpenGL ES documentation describes:
- runtime capability queries such as `GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS`;
- vertex-shader texture lookups for transform data;
- render-to-texture using framebuffer objects, including GPU-produced data used by a later vertex-processing stage;
- texture-unit reuse to avoid unnecessary extra rendering passes.

The port should use the live capabilities report to decide which data-table formats are legal. In particular, texture sampling support and renderable float/half-float color targets are separate capabilities and are now logged separately.

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
- [x] Inspect upstream GroupManager, ObjectBatch, WorkGroupScheduler, shader records and renderer flow.
- [x] Inspect the fork's OpenGLES ES2 shim and iOS shader/buffer implementations.
- [x] Add live GL capability reporting, including vertex texture units, texture formats, color-target formats, MRT and timer-query support.
- [ ] Port owner data records to an ES-compatible iOS representation.
- [ ] Port owner-style CPU layer sorting and work scheduling.
- [ ] Port GroupManager state, checkpoint and pulse semantics.
- [ ] Port ObjectBatch input/mesh construction.
- [ ] Port group/move/rotation/follow/toggle/checkpoint hooks.
- [ ] Build and verify the step-2 iOS target before starting step 3 shader translation.
