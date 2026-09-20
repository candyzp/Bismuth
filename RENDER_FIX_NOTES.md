# iOS rendering changes

The existing renderer submitted an invisible full-atlas draw to synchronize dirty
stock geometry, then drew the actual mixed stock/GPU output. That duplicated
vertex work on dense scenes. It now uploads the atlas VBO directly, from slot zero,
without Cocos' unsafe partial dirty-draw copy or a hidden rendering pass.

Dirty object/sprite state now shares one texture-binding scope, merges changes
within a texture row, and retains failed transfers for retry. Draw readiness is
false while state is stale. This avoids a silent stale-state success.

Owned sprite geometry can refresh live crop, UV, offset and local transforms;
unchanged geometry stays resident. Previously forced decorations accepted frame
changes while continuing to draw the original baked geometry.

The actual PlayLayer background sprite and sprite descendants use a persistent
unit-quad shader at their existing draw position in the scene. It consumes GD's
current corners, UVs, color, opacity and parent/camera matrices, including a black
background. Bismuth does not replace the background with a synthetic color layer.
The background is usually only a few quads: this is coverage/correctness work,
not a promise of a large background-only speedup.

GPU-owned atlas/root and background draw failures suppress stock redraw and show
an error even with detailed diagnostics disabled. Explicitly disabling Bismuth
still restores normal rendering. This is strict behavior for claimed draws; it
is not a claim that all GD rendering is implemented by Bismuth. Portals, particles,
ground, UI and unsupported visual trees still have engine-owned paths. Gameplay,
collision, triggers, audio and simulation timing remain GD-authoritative.

Validation: host sanitizer regressions plus Mesa GLES compile/link and background
pixel checks. iOS compilation and device results are tracked on the pull request.
No WHAT FPS improvement is claimed before measuring the same segment on-device.
