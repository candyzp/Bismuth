# iOS renderer regression checks

Run from the repository root with Python 3 and a C++23-capable GCC:

```sh
python3 tests/run_ios_atlas_tests.py
python3 tests/run_ios_state_tests.py
python3 tests/run_ios_lifecycle_tests.py
python3 tests/run_ios_upload_tests.py
python3 tests/run_ios_geometry_tests.py
python3 tests/run_ios_shader_tests.py
```

The C++ tests compile production functions against small fake Cocos/GL APIs with
AddressSanitizer and UndefinedBehaviorSanitizer. In containers that cannot run
LeakSanitizer, use `ASAN_OPTIONS=detect_leaks=0`. The shader test uses a real
headless Mesa GLES context through libEGL; it is not an Apple driver test.

Coverage:

- 10,000 shuffled sprites, repeated reorderings, unchanged index-buffer reuse,
  spatial buffers exceeding 32,000 sprites, u16 limits, and exact mixed draw order.
- Direct dirty-atlas VBO upload with no masked full-atlas rendering pass; failed
  uploads remain failures. GL state and Cocos' VAO cache survive mixed draws.
- Failed owned atlas draws do not switch to a full stock redraw.
- Exact affine transforms, detached visibility, stock premultiplied color for
  all 65,536 byte/opacity pairs, and one validation epoch per displayed frame.
- Overlapping PlayLayer setup/exit, pause/resume, explicit disable, and one state
  capture per displayed frame despite multiple simulation updates.
- Data-texture row boundaries, overflow rejection, one binding scope per texture,
  coalescing 4,000 scattered dirty records into 16 transfers in the fixture, and
  retaining/retrying failed dirty records even when CPU state stops changing.
- Live crop/UV/offset and child affine changes, unchanged geometry reuse, root
  motion without a geometry reupload, and retry after a vertex-upload failure.
- Actual GLES shader compilation/linking and colored/black background pixel output.

The PR workflows run the host regressions and build the iOS mod. Passing these
checks does not establish Geometry Dash FPS, Apple driver behavior, or all Geode
hook interactions on a device.

For device validation, compare the same WHAT segment, same refresh/FPS cap and
settings, at a similar device temperature, with GPU debug disabled. Compare
Bismuth enabled/disabled over repeated runs and inspect frame-time spikes, not
just the capped FPS number. Separately turn diagnostics on to check background
submissions and errors. Check color/background changes, resets, practice restart,
fades, moving decorations, glow overlap, and exits/re-entry.
