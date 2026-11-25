# Kyanite iOS Metal port

This branch is based on the exact upstream ELTXZU/Kyanite source tree. It is
separate from the existing Bismuth renderer work and preserves Kyanite's
upstream source attribution and license.

## Target

- Geometry Dash: 2.2091
- Geode: 6.0.0
- Platform: iOS arm64
- Graphics API for the experimental backend: Apple Metal

## Implemented on iOS

The iOS target is isolated from Kyanite's desktop OpenGL 4.3 renderer. It now:

- initializes the system Metal device and command queue;
- compiles separate Metal compute pipelines for the startup probe and object
  visibility pass;
- submits a one-time 256-thread Metal compute self-test;
- reads level object metadata once after \`PlayLayer::setupHasCompleted\`;
- uploads static object bounds, start transforms, and up to 10 group IDs per
  object into a shared Metal buffer;
- keeps compact per-group position/local transforms, visibility, and opacity
  state in a separate Metal buffer;
- mirrors move, rotate, follow, toggle, and alpha changes into those group
  states without changing the game's own object positions or physics;
- updates a camera-space rectangle and dispatches one compute thread per object;
- computes transformed object bounds, rejects invisible/transparent groups,
  culls objects outside the camera rectangle, and compacts visible object
  indices into a GPU-written index buffer;
- checks the visible-index output after the command buffer completes and
  reports object, visible, culled, completed-frame, skipped-frame, and GPU-time
  counters in the on-screen diagnostics.

The object list is uploaded once per level. Per-frame work copies only the
compact group state and camera bounds, then dispatches the GPU visibility
kernel. If a previous dispatch is still in flight, the next dispatch is
skipped rather than writing buffers the GPU may still be reading.

## Important renderer boundary

**This is a real Metal object-metadata and GPU visibility workload, but it is
not yet the complete Kyanite sprite renderer.** The visible-index list is
currently diagnostic output; it is not consumed by a Metal sprite draw pass.
Geometry Dash's stock renderer continues to draw every level sprite and owns
the visible frame. The current GPU culling therefore does not yet reduce the
game's actual draw calls or guarantee an FPS improvement.

A complete renderer still needs:

1. Convert Kyanite's per-sprite vertices, texture coordinates, atlas pages,
   blending modes, z-layer ordering, color/pulse state, and object flags into
   Metal-compatible buffers.
2. Upload the required game sprite atlases to Metal textures.
3. Add Metal render and indirect-command pipelines that consume the culling
   output and preserve Kyanite's draw ordering and blending behavior.
4. Composite the Metal output into the iOS Cocos2d/OpenGL ES frame without
   breaking the game's other layers, particles, portals, checkpoints, or UI.
5. Validate restart/checkpoint behavior, group transform semantics, audio
   scale, color channels, unsupported sheets, and compatibility fallback.
6. Add unsupported Scale, Area/Enter, Advanced Follow, and Keyframe trigger
   behavior before claiming complete feature parity.

## Safety and compatibility rules

- The stock Geometry Dash renderer remains enabled as the safe fallback.
- This port does not alter gameplay physics, hitboxes, collision checks, or the
  game's authoritative object transforms.
- A completed compute command buffer is required before the overlay reports a
  culling frame as complete.
- Visible-index output is range-checked before the diagnostics report it.
- If Metal initialization or a compute dispatch fails, the overlay reports the
  failure and the stock game renderer remains active.
- GPU timings are marked unavailable when Metal does not provide valid GPU
  start/end timestamps.
- The camera-space object bounds are a broad-phase approximation. They are
  not yet the exact sprite meshes used by Kyanite's desktop renderer.

## Validation

\`.github/workflows/ios.yml\` builds the iOS target for validation only. It does
not upload or publish a binary.

## Upstream license notice

See \`LICENSE\`. The upstream Kyanite license includes an additional restriction
on distributing binaries of AI-modified versions. This branch's workflow is
for compilation validation only and does not upload or publish a binary.
