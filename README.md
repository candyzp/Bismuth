# Bismuth iOS — GPU-only beta port

Experimental Geometry Dash 2.2081 renderer port for iOS arm64.

The active implementation is being developed on `rework/ios-gpu-v2`, following the current upstream Bismuth renderer while translating its desktop compute/SSBO pipeline to OpenGL ES 2. The new PlayLayer path is **GPU-only**: when Bismuth owns a level, its stock Cocos batches stay hidden. Initialization, shader, state-table, or draw failures are reported; the renderer does not silently fall back to stock rendering.

## Development status

- Step 2: CPU-side iOS port implemented at source level.
- Step 3: ES2 state tables, shaders, ordered VBO, and GPU layer submission implemented at source level.
- Step 4: source-level parity and compatibility review recorded, including runtime blend-order updates, black-sprite channel semantics, GL-state restoration, and atlas-coordinate precision.
- Step 5: not started. The branch has **not** yet been successfully compiled or device-tested. No claim is made that the renderer currently works.

All active port work stays on `rework/ios-gpu-v2`; do not merge these changes into `master`.

<img src="logo.png" alt="Bismuth logo" width="100"/>
