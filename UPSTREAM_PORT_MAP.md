# Upstream-to-iOS Port Map

Reference: `iliasHDZ/Bismuth` master at `12536eb964022e16985b428228ba3f36923ba3de`.

## Upstream renderer flow

| Upstream component | Role in the owner's renderer | iOS port requirement |
|---|---|---|
| ObjectSorter | Enumerates and organizes the renderable GameObjects | Port the same object ordering and sprite classification |
| GroupManager | Group combination, alpha, transform, pulse, and toggle state | Preserve the owner's state model and update semantics |
| ObjectBatch | Static object/sprite/vertex input data | Preserve the input record and indexed geometry model |
| WorkGroupScheduler | Sort by layer/blend/sheet and partition work for GPU execution | Port sorting, remaps, work-group ranges, and scheduler behavior |
| SAOBuffer | Structure-of-arrays GPU data organization | Preserve the upstream field layout and indexing semantics |
| objectCulling.comp | Per-sprite culling, transforms, audio scale, HSV/pulses, opacity and raw sprite output | Port the calculations, not the desktop-only compute API syntax |
| objectDrawing.vert | Applies GPU-produced sprite state to vertices | Port the output transform and vertex mapping |
| objectDrawing.frag | Samples the sprite sheet and multiplies resolved color | Preserve texture and color output |
| ObjectBatchNode | Submits each ordered render layer | Preserve draw order and blend/sprite-sheet transitions |

## Desktop-only mechanisms that cannot be copied literally

The current upstream renderer checks for desktop OpenGL 4.3 and uses:
- `GL_COMPUTE_SHADER` and `glDispatchCompute`
- `GL_SHADER_STORAGE_BUFFER` / `layout(std430)`
- `glMultiDrawElementsIndirect` and indirect draw command buffers
- `glSpecializeShader` and desktop shader include extensions
- WGL function loading and OpenCL/GL context interop for GPU core count discovery

Apple's documented `OpenGLES.framework` on iOS implements ES 1.1, 2.0, and 3.0. More importantly, this fork's own iOS GL shim includes `OpenGLES/ES2/gl.h` and `OpenGLES/ES2/glext.h`, and documents Geometry Dash as using a Cocos OpenGL ES 2 context. Treat ES2 as the active target until the runtime capability report proves otherwise.

The upstream desktop compute/SSBO/indirect-draw path therefore needs a real iOS backend port. The ES2 translation should preserve the owner's CPU data preparation and ordering, while moving the owner's per-sprite culling/transform/color calculations into the iOS GPU shader path:
- Keep the owner's ObjectSorter, GroupManager, ObjectBatch, and WorkGroupScheduler responsibilities.
- Build GPU vertex/index data in the same upstream layer / sprite-sheet / blend order.
- Carry per-sprite/object/group indices in ES2-compatible vertex attributes and GPU-readable state tables. Confirm vertex texture sampling limits before choosing the state-table representation.
- Translate the calculation body of `objectCulling.comp` into the GPU vertex-processing path. A sprite that is culled is moved outside the clip volume; it is not drawn by Cocos.
- Replace indirect draw commands with the corresponding CPU-scheduled `glDrawElements` ranges because ES2 does not expose the upstream indirect draw mechanism.
- Keep the pixel shader responsible for sampling the selected sprite sheet and applying the GPU-resolved color.

These are API substitutions only. They must not become a return to the old assist/ownership design. The new beta is GPU-only: if its GPU render path fails, report a beta failure; do not draw the affected content through stock Cocos.

## Porting rules

- Keep upstream field names, record meanings, sort keys, group-combination indexing, and per-layer order aligned wherever possible.
- Keep the upstream CPU preparation / work-group scheduling design.
- Keep visibility, per-sprite transform evaluation, and final color calculations on the GPU.
- Use the iOS game's actual current GL context and query capabilities at runtime.
- Do not assume extensions or ES 3.0 features are present without checking.
- No stock-render fallback in the new beta path. A GPU failure is a beta failure and must be visible in diagnostics.

## Old iOS renderer

The prior `ResolvedStateLayer`, `AssistShadowBatch`, `StandaloneAssistBatch`, and `AtlasInterleave` system is not the base of this port. Keep it only as historical reference while the upstream architecture is ported separately.
