# Kyanite iOS Metal port

This branch is based on the exact upstream ELTXZU/Kyanite source tree. It is
separate from the existing Bismuth renderer work and preserves Kyanite's
upstream source attribution and license.

## Target

- Geometry Dash: 2.2091
- Geode: 6.0.0
- Platform: iOS arm64
- Graphics API for the planned backend: Apple Metal

## Current implementation stage

The iOS build is intentionally isolated from Kyanite's desktop OpenGL 4.3
renderer. It currently:

- initializes the system Metal device and command queue;
- compiles a native Metal compute kernel;
- submits a one-time 256-thread compute self-test;
- reports device, queue, pipeline, dispatch completion, verified thread count,
  and GPU execution time when Metal exposes it;
- displays an on-screen diagnostics overlay.

**This is not yet the Kyanite level renderer.** Geometry Dash's own renderer
continues to draw all level objects. The overlay explicitly reports this safe
fallback and reports object upload, culling, and object drawing as not connected.
The probe's timing is not a level-rendering benchmark.

## Port milestones

1. Runtime and truthful diagnostics — current stage.
2. Static object data — define Metal-compatible object, sprite, layer, and
   group buffers; upload immutable level data once.
3. GPU group state — move, rotate, follow, toggle, alpha, and pulse state
   buffers with CPU trigger hooks that update compact state entries.
4. Compute culling and ordering — compact visible sprite indices and build
   per-layer indirect draw arguments on the GPU.
5. Metal object rendering — integrate the Metal render pass with the
   Geometry Dash object-layer order while leaving shaders, UI, particles,
   physics, and hitbox calculations under the game's existing systems.
6. Per-object transforms and compatibility — scale, area/enter effects,
   advanced follow, keyframes, hitboxes, checkpoints, restart, and fallback
   tests.

## Validation rules

- No claim that Kyanite is active unless its Metal draw pass was submitted.
- No claim that a frame's Metal work completed until a command-buffer completion
  callback confirms it.
- Keep device/queue/pipeline failures visible in the overlay and logs.
- If Metal object rendering cannot initialize or a frame submission fails,
  leave the stock Geometry Dash object renderer active.
- GPU timing is marked unavailable when the platform does not provide a valid
  command-buffer GPU start/end time.
- Do not change game physics, hitboxes, object trigger semantics, or
  player-facing gameplay behavior as part of renderer optimization.

## Upstream license notice

See LICENSE. The upstream Kyanite license includes an additional restriction
on distributing binaries of AI-modified versions. This branch's workflow is
for compilation validation only and does not upload or publish a binary.
