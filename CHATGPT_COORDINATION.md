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

- Found a concrete stock-color regression in `src/ObjectUtils.hpp`: commit `af3aa051` changed the color sanitizer from upstream behavior to treat channel `0` as invalid and force it to `COLOR_CHANNEL_WHITE`. Bismuth's color table is indexed from `0`, and upstream intentionally forwards channel `0`, so valid stock objects could be remapped to the wrong tint/blending path.
- Patched only the safe color-classification file in commit `b959f0826e67c93ce669b2ef102b6e86d3dfee7c`: `sanitizeColorChannel()` now rejects only negative IDs and IDs `>= COLOR_CHANNEL_COUNT`, while preserving channel `0`.
- Kept the newer base/detail subtree black-classification logic intact for now. Reverting that wholesale would likely reintroduce the layered coin/orb child-sprite bug it was trying to solve.
- Read-only check of `Renderer::prepareColorChannelBuffer()` found that the renderer explicitly initializes BLACK but relies on GD's color-action vector for other slots. This makes remapping ordinary objects to WHITE especially risky and reinforces preserving valid channel `0` instead of using WHITE as a catch-all.
- Did not touch Assistant A's claimed transform/culling/renderer/shader files.

## Assistant A current implementation status

Implemented on `master`:
- `75cf361f4058a399b0389385b8fc483ffc16e1a2` restores `updateEnterEffects`, `processAreaVisualActions`, and particles in the optimized visibility path without restoring stock CPU/Cocos drawing.
- `8adb3705671cb6a1305df2d13d2da5a369481060` + `f41adb7687f75213273bd6961016850a8cb3a02f` add partial iOS data-texture uploads.
- `8958820b518a8d9e3991423ef4ab0ac5eeb225bc` + follow-ups feed per-object 2.2 Move/Rotate/Scale runtime state into the existing object data texture rather than adding another vertex texture unit.
- `9e2e491f369588f5e710e7cfb53115f76453c29f` applies those runtime transforms in `object_ios.vert`, keeping the heavy vertex work on the GPU.
- `dc61e835a33441722b6f48b5211b72fbef87527c` adds live-position culling for runtime-effect objects and widens the normal iOS GPU assist edge band to reduce cutoffs.
- `e8c7ef886472fbe47ddc226987ccbaf951d86edd` + `c11688215d9a6c0038299cde3fe31433846e1ead` explicitly prevent stale stock Cocos opacity from becoming a GPU visibility/culling source.

Important safety/correctness rule for follow-up work:
- **Do not use `GameObject::getOpacity()` as generic runtime GPU opacity.** Bismuth skips GD's normal stock visibility loop, so ordinary baked sprites may have stale display opacity/visibility. Area Fade needs a dedicated tracked value or hook before it is sent to the GPU. For now Move/Rotate/Scale are the supported runtime bridge; Area Fade is intentionally not consumed by the shader.
- Do not re-enable full `preUpdateVisibility()` or stock Cocos rendering as a shortcut. The goal is CPU effect simulation + GPU geometry/rendering.

Validation status:
- Source/field audit done against Geode 2.2081 bindings for the runtime transform fields.
- No GitHub check run exists on the current head, and the manual workflow was intentionally not started. Device/build validation is still required.

## Assistant B active ownership — 50/50 split

Assistant A's transform/culling work above is considered complete for now. Assistant B is now actively owning the appearance/runtime-effects half:
- color channel correctness and stock-level color parity
- HSV and blending correctness
- Area Fade runtime state
- Area Tint runtime state
- glow/particle visual mismatches that are independent of Move/Rotate/Scale geometry
- iOS appearance-side GPU data plumbing needed for those effects

Assistant B may now edit `RendererIOS.cpp`, the iOS shaders, and shared renderer structures when the change is specifically for the appearance/runtime-effects half. Do not overwrite or redesign Assistant A's Move/Rotate/Scale/culling implementation unless a direct compatibility bug is proven.

Assistant B will preserve the existing goal: CPU simulates GD effect logic, GPU performs the visual rendering/transform work. No full stock `preUpdateVisibility()` or Cocos fallback.

## Assistant A active ownership — GPU transform half

User explicitly requested a 50/50 split and a slightly more GPU-heavy renderer. Assistant A owns only the Move/Rotate/Scale transform and culling half unless the coordination file is updated again.

Current transform files:
- `src/renderer/ios/RendererIOS.cpp`
- `resources/shaders/object_ios.vert`
- `src/renderer/VisibilityManager.cpp/.hpp` and `src/renderer/ObjectBatch.hpp` only when a transform/culling bug requires them

Commit `92fecebf48c9a1b1edd9d23f29230251a88970ac` shifts more runtime transform arithmetic from CPU to the iOS vertex shader:
- CPU packs raw position offsets, both Area Rotate contributions, raw Area Scale offsets, and precomputed inverse base scales.
- GPU combines Area Rotate and converts Area Scale offsets into final per-vertex scale factors.
- Removed the ignored generic `GameObject::getOpacity()` read from the transform packer.
- Debug overlay reports `Runtime transform math: GPU`.

Runtime transform texel contract after `92fecebf`:
- `r0.xy` = runtime world position offsets
- `r0.zw` = raw Area Rotate X/Y contributions
- `r1.xy` = raw Area Scale X/Y offsets
- `r1.zw` = precomputed inverse base scale X/Y

**Assistant B: do not reuse `r0.w` or any `r1` component for Area Fade/Tint.** Those two texels are now fully owned by transform data. Appearance state should use a separate appearance texel/texture or another explicitly coordinated layout. Preserve `AreaVisualState.hpp/.cpp` work from `d967b708` and `ad4d50be`; Assistant A will not modify those files.
