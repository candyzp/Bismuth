# iOS: Metal compute with OpenGL ES drawing

## Architecture target

This branch is based directly on the official upstream Bismuth revision, not
on the earlier iOS-port experiment.

The intended pipeline follows Bismuth's own design:

1. Extract immutable object, sprite, group-combination, and sprite-mesh data
   once when a level loads.
2. Upload those static arrays to Metal once.
3. Each frame, provide only compact changing data: group-combination state,
   color channels, pulse entries, and camera/game uniforms.
4. Run per-sprite transforms, camera visibility, audio scaling, trigger
   transforms, pulse/HSV, opacity, and glow calculations in Metal.
5. Let OpenGL ES draw the existing level layers and sprite sheets, sampling
   the Metal-produced per-sprite state texture.

There must be no CPU loop over all sprites to calculate visibility or final
positions each frame. There must be no per-frame rebuilding of the mesh
vertex stream.

## Why a shared texture is used

The iOS OpenGL ES path cannot use Bismuth's desktop GL 4.3 shader-storage
buffers or indirect multi-draw calls. Apple's documented Metal/OpenGL ES
interoperability path uses a Core Video pixel buffer with both Metal and
OpenGL ES texture views. The Metal kernel writes per-sprite state into that
shared texture; the GLES vertex shader samples it while drawing the static
mesh VBO.

The texture stores three RGBA32Float texels per source sprite:
- world position and the first local-transform column
- second local-transform column and red/green color
- blue/alpha color and a visibility flag

The GLES vertex shader uses the vertex's source sprite index to fetch this
record. Invisible sprites are moved outside clip space. The original sprite
mesh remains static, so concave sprite meshes continue to reduce overdraw.

## CPU work allowed

- One-time level extraction, object metadata, sprite mesh construction, and
  blend-order scheduling when blend states actually change.
- Compact group-combination state updates caused by move/rotate/follow/alpha
  triggers.
- The active color-channel table, pulse entries, and one camera/game-state
  constant block per frame.

## CPU work rejected

- Per-sprite camera visibility checks.
- Per-sprite current-position or transform generation.
- Per-sprite vertex rebuilding or full vertex-stream upload every frame.
- Copying one transform/culling input record for every sprite on every frame.
- Waiting synchronously for a Metal command buffer to finish.

## Non-blocking resource ownership

The compute renderer uses multiple shared output textures. A Metal completion
handler marks each result ready. OpenGL ES places a zero-timeout
GL_APPLE_sync fence after its last read. A texture slot is reused only when
Metal has finished writing and the previous GLES read fence has signalled.
When no slot is available, the compute dispatch is skipped and the previous
completed result remains in use. The game thread never waits for Metal.

## Current implementation boundary

The data contract and GLES shader are added first. The next integration step
must wire immutable level extraction, compact frame-state updates, Metal
dispatch, and the existing OpenGL ES layer draws into the Geode PlayLayer
lifecycle. Until that integration is built and tested on an iPhone, this
branch is an architecture implementation in progress, not an installable
replacement for the current mod.
