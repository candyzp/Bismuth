# iOS renderer regression checks

Run from the repository root with Python 3 and a C++23-capable GCC:

```sh
python tests/run_ios_atlas_tests.py
python tests/run_ios_state_tests.py
glslang -l resources/shaders/assist_ios.vert resources/shaders/assist_ios.frag
```

The atlas test compiles the production registry and the production CCSprite / CCSpriteBatchNode hook bodies against a small fake Cocos/GL API. The state test compiles the production state structures and capture, packing, comparison, geometry eligibility, and current-state lifetime functions. Both use AddressSanitizer and UndefinedBehaviorSanitizer. In containers where LeakSanitizer cannot inspect `/proc`, use `ASAN_OPTIONS=detect_leaks=0`; address and undefined-behavior checks remain enabled.

Coverage includes:

- Ordered mixed stock/GPU output, followed by a complete stock draw when complex decoration enters the batch.
- 10,000 shuffled sprites, repeated reorderings, no redundant index upload for an unchanged order, and u16 index boundaries.
- Failed index uploads, failed atlas synchronization, unavailable GPU state, unrelated/disabled batches, and deferred sprites migrating between batches.
- Preservation of texture/program bindings, each VAO's original element buffer, color/depth write masks, and separate front/back stencil masks.
- Exact stock affine coefficients, including skew/separate-axis rotation, small transform changes and vertex Z.
- Rejection of stale texture/crop/flip/offset geometry and newly attached child/glow parts.
- All 65,536 color-byte/opacity combinations against Cocos' premultiplied-color expression.
- Clearing the active resolved-state pointer on destruction.

These are host regression tests, not an iOS build or a Geometry Dash performance benchmark. They do not measure Future Funk FPS, Apple driver behavior, or runtime Geode hook ABI compatibility. The workflow remains manual-only and was not run for this change.

For device validation, build the new master and compare the same Future Funk sections with Bismuth enabled and disabled. Check overlapping decoration and fades, reset/practice restart, and exiting/re-entering the level. Use the GPU debug display to inspect actual draw counts, then turn it off for the frame-rate comparison. Multipart or changing visual geometry remains stock; GPU ownership alone is not evidence that a particular frame used GPU assist.
