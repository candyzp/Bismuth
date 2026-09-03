# Bismuth assistant coordination

Active branch: `master`
Platform focus: Geometry Dash 2.2081 / Geode / iOS OpenGL ES 2.0

## Assistant A (current ChatGPT) — claimed work

I am actively working on the GPU visual-state / visibility path for these bugs:
- blocks disappearing or being cut off
- 2.2 / Dash-style moving visual effects not rendering correctly
- runtime visual transforms not reaching the GPU renderer
- keeping GPU rendering dominant instead of falling back to full stock CPU/Cocos drawing

Primary files I may edit:
- `src/decomp/PlayLayer.cpp`
- `src/renderer/VisibilityManager.cpp/.hpp`
- `src/renderer/Renderer.cpp/.hpp`
- `src/renderer/ios/RendererIOS.cpp`
- `resources/shaders/object.vert`
- `resources/shaders/object_ios.vert`
- possibly shared renderer structs if needed

Please avoid editing those same areas until this note is updated, unless the user explicitly asks us to merge approaches.

## Findings already confirmed

1. `optimized_updateVisibility()` currently only processes colors/audio and skips most GD visual-state work. In the full path, GD also runs enter effects, area visual actions, particles, per-object visual updates, etc.
2. Bismuth's `GroupManager` handles old Move/Rotate/Follow group transforms, but 2.2 area/enter effects have additional per-object runtime state.
3. Geode 2.2081 bindings confirm area-move-style runtime displacement is represented through object offsets such as `m_positionXOffset` / `m_positionYOffset`; scale has `m_scaleXOffset` / related scale state. This is a good bridge for sending GD-calculated visual state to the GPU without redrawing through stock Cocos.
4. Current iOS renderer already uploads object/group/color state through float/byte data textures, so extending GPU-readable runtime state is feasible.
5. Current culling uses baked `m_startPosition` section placement plus group transforms. Runtime per-object visual movement can therefore cause cutoffs unless culling gets a conservative path for those dynamic objects.

## Good parallel tasks for Assistant B

Safe areas to investigate without colliding with my current work:
- wrong color classification / blending / HSV behavior on Stereo Madness and other stock levels
- identify which exact 2.2 effect types Dash uses around its moving sections (object IDs / area effect types)
- inspect particle / shader / glow discrepancies that are independent of runtime transform plumbing
- build/test failures after my commits, without rewriting the transform/culling design
- compare Bismuth output against stock GD for object color channel selection

Please append findings below rather than rewriting the claimed-work section.

## Assistant B findings

- (append here)
