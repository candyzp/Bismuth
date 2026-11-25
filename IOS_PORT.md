# Kyanite iOS Metal port

This branch is based on the upstream ELTXZU/Kyanite source tree. It is kept
separate from the original Bismuth renderer and preserves the upstream source
attribution and license.

## Target

- Geometry Dash: 2.2091
- Geode: 6.0.0
- Platform: iOS arm64
- Experimental GPU API: Apple Metal

## Implemented on iOS

The iOS target is isolated from Kyanite's desktop OpenGL 4.3 renderer. It now:

- initializes the system Metal device and command queue;
- compiles a startup compute probe and a separate object-visibility compute
  pipeline;
- submits a one-time 256-thread Metal self-test;
- reads level object metadata once after level setup and uploads bounds,
  initial position/rotation, and group IDs;
- deduplicates objects into sorted group combinations, so objects sharing the
  same group set reference the same transform/visibility state;
- stores one compact transform, visibility and alpha state per group
  combination, following Kyanite's group-combination architecture rather than
  independently composing per-group transforms in the shader;
- mirrors move, rotate, follow, toggle, and alpha updates into every affected
  group combination without taking ownership of the game's actual object
  positions or physics;
- updates a camera-space rectangle and dispatches one compute thread per
  uploaded object;
- applies the accumulated group-combination transform and alpha/visibility
  state, tests a conservative rotated object bound against the camera, and
  compacts visible object indices into a GPU-written buffer;
- checks the output indices only after the command buffer completes and reports
  object count, combination count, visible/culled counts, completed frames,
  skipped frames, group-state updates, and GPU timing in the on-screen overlay.

Immutable object data is uploaded once per level. Group state is copied to the
GPU only when a transform, visibility, or alpha state has changed. The camera
bounds and output counter are prepared for each submitted frame. If a prior
dispatch is still in flight, the next one is skipped rather than overwriting a
buffer the GPU may still be reading.

## Important renderer boundary

**This is not yet the complete Kyanite sprite renderer.** The visible-index list
is diagnostic output and is not consumed by a Metal sprite draw pass. Geometry
Dash's stock renderer continues to draw every level sprite and owns the visible
frame. This GPU visibility workload therefore does not yet reduce the game's
actual draw calls or guarantee an FPS improvement.

A complete renderer still needs:

1. Convert Kyanite's per-sprite vertices, texture coordinates, atlas pages,
   blending modes, z-layer ordering, color/pulse state, and object flags into
   Metal-compatible buffers.
2. Upload the required game sprite atlases to Metal textures.
3. Add Metal render and indirect-command pipelines that consume the culling
   output while preserving Kyanite's draw ordering and blending behavior.
4. Composite Metal output into the iOS Cocos2d/OpenGL ES frame without breaking
   the game's other layers, particles, portals, checkpoints, or UI.
5. Validate restart/checkpoint behavior, transform semantics, audio scale,
   color channels, unsupported sheets, and safe fallback.
6. Add Scale, Area/Enter, Advanced Follow, and Keyframe trigger behavior before
   claiming full feature parity.

## Safety and compatibility rules

- The stock Geometry Dash renderer remains enabled as the safe fallback.
- This port does not alter gameplay physics, hitboxes, collision checks, or the
  game's authoritative object transforms.
- The overlay reports culling as complete only after Metal's command-buffer
  completion handler confirms success.
- Visible-index output is range-checked before the diagnostics report it.
- If Metal initialization or a compute dispatch fails, the overlay reports the
  failure and the stock game renderer remains active.
- GPU timings are marked unavailable when Metal does not provide valid GPU
  start/end timestamps.
- Object bounds are a conservative broad-phase approximation, not Kyanite's
  final per-sprite geometry.

## Validation

The iOS workflow builds the target for validation only. It does not upload or
publish a binary. CI runs that never receive a GitHub-hosted runner are not
compile results and must not be reported as passing.

## Upstream license notice

See LICENSE. The upstream Kyanite license includes an additional restriction
on distributing binaries of AI-modified versions. This branch's workflow is
for compilation validation only and does not upload or publish a binary.
