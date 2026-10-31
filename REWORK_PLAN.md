# Bismuth iOS GPU Rework

## Baseline

- Rework branch: `rework/ios-gpu-v2`
- Baseline: `candyzp/Bismuth` `master` at `d31115ee3851655c6671233bc13b1a42e0d7e157`
- Target: Geometry Dash 2.2081 on iOS / arm64
- The existing renderer remains intact as a reference until the replacement is proven.

## Keep

- iOS build and Geode integration
- Existing debugger / GPU diagnostics and BProfiler
- Sprite metadata and mesh data that remain valid
- PlayLayer lifecycle and stock-renderer integration hooks
- Existing CPU rendering fallback and the old implementation for comparison

The diagnostics should be updated to report confirmed GPU submissions, not merely registered ownership or attempted draws.

## Rebuild boundary

Rework the GPU rendering implementation in isolation. Do not rewrite gameplay, triggers, collision, object activation, physics, or simulation timing. Geometry Dash remains authoritative for the final resolved visual state; Bismuth may move render-only work to the GPU.

The upstream renderer is the algorithmic reference, not a source tree to copy wholesale. Its current desktop path uses OpenGL 4.3-era compute / storage-buffer infrastructure and desktop GL loading. The iOS target uses Apple's OpenGLES framework and its own shader / data-upload path. Port algorithms and rendering semantics to the supported iOS API; do not import WGL, desktop-only entry points, compute dispatch, or SSBO assumptions into the iOS backend.

## Milestones

### 0. Preserve and establish a known baseline

- Keep this branch separate from `master`.
- Record the current iOS build result and existing debug counters before changing ownership.
- Do not delete the previous renderer files.

### 1. Prove the GPU path independently

Build a tiny iOS-only render self-test that verifies:
- required vertex texture units are available;
- the state-texture format can be allocated and sampled from a vertex shader;
- the iOS vertex / fragment shaders compile and link;
- a small offscreen draw completes and returns the expected pixel;
- GL state touched by the test is restored.

A failed probe must leave Geometry Dash's stock renderer untouched.

### 2. Rebuild the smallest renderer primitive

- One known sprite, one texture, one draw range.
- Use a known-good sprite quad / UV mapping from Cocos.
- Feed only final resolved transform, visibility, and color state.
- Verify output against the stock sprite before claiming ownership.

### 3. Add batching without changing visual order

- Add multiple sprites sharing one texture and blend state.
- Preserve stock atlas / z-order semantics.
- Add texture and blend transitions only after the single-range path is verified.
- Keep unsupported sprites on stock rendering.

### 4. Add visual features incrementally

Test each separately before combining:
- transforms and camera movement;
- texture crop, rotated frames, and flips;
- opacity and premultiplied-alpha behavior;
- normal and additive blending;
- base/detail colors and HSV;
- glow and pulse effects;
- animated and nested visual trees (remain stock until explicitly supported).

### 5. Reintroduce hybrid ownership and optimization

- Add bounded per-frame GPU work only after measurements justify it.
- Separate candidate eligibility, draw readiness, successful submission, and ownership.
- Preflight all resources and ranges before issuing a mixed GPU pass.
- Never suppress the matching stock draw merely because a sprite was registered as GPU-owned.
- Keep CPU fallback for all unverified or failed candidates.

## Correctness gates

1. **No visual regression:** the GPU result must match the stock result for the tested case.
2. **No false ownership:** attempted or failed GPU draws do not count as successful GPU rendering.
3. **No double rendering:** stock and GPU must not both submit the same sprite.
4. **No missing rendering:** any candidate that cannot be submitted remains on the stock path.
5. **No gameplay changes:** no trigger, collision, physics, or simulation behavior is moved or altered.
6. **Measured performance only:** report CPU time, GPU time, draw calls, upload bytes, and frame pacing on the same level segment. Do not infer FPS gains from sprite counts alone.

## Initial regression matrix

- One static block, one texture
- Several blocks sharing a texture
- Two textures in one stock layer
- Overlapping sprites and z-order
- Moving and rotating groups
- Transparency and additive glow
- Color / HSV and pulse triggers
- Rotated or flipped sprite frames
- Particles, portals, UI, and unsupported objects staying stock
- Restart, checkpoint, pause/resume, and level exit
- Dense level with sustained GPU coverage

## First implementation target

Milestone 1: an isolated iOS GPU render self-test. It provides a trustworthy baseline before the new renderer starts claiming sprites.
