# iOS Metal / OpenGL ES method matrix

This branch adds an isolated probe selector for iOS. It is intentionally separate
from the full Bismuth scene renderer.

## How to use it

1. Leave **iOS interop probe mode** enabled.
2. Choose **00 - Run all 20 probes** to execute the complete matrix in one level
   entry, or choose an individual numbered method to isolate it.
3. Enter a level. Probes run after Geometry Dash finishes its normal level reset.
   The batch option runs methods 01–20 sequentially and logs each PASS/FAIL.
4. Read the Geode log for the individual results and the underlying API errors.

Probe mode does not create `IOSGPURenderer`, hide stock Cocos batches, replace
Geometry Dash's `updateVisibility`, or remove stock decorations. The game keeps
its normal rendering path while the selected API test runs. Change the selector
and enter another level to try a different probe; no rebuild is required. The
selector defaults to the low-risk host-context check (method 01); the full matrix
is opt-in through method 00.

Turn probe mode off only when you are ready to test the full Bismuth renderer.
The full renderer now waits until all active GPU layers have submitted
successfully before hiding the original batches. If initialization or a later
GPU operation fails, it restores the previous batch-visibility state.

## Probe list

Choose method 00 to run the following twenty probes sequentially.

| # | Probe | What it establishes |
|---:|---|---|
| 01 | Host EAGL context | Geometry Dash's current OpenGL ES context is available on the level-start thread. |
| 02 | Metal device creation | `MTLCreateSystemDefaultDevice` returns a device. |
| 03 | Metal command queue | A Metal command queue can be created. |
| 04 | Metal shader compilation | The probe's minimal Metal shader source compiles. |
| 05 | Metal compute pipeline | A compute pipeline can be created from the compiled function. |
| 06 | Metal-only CVPixelBuffer | Core Video can allocate a BGRA pixel buffer with Metal compatibility. |
| 07 | GLES-only CVPixelBuffer | Core Video can allocate a BGRA pixel buffer with OpenGL ES compatibility. |
| 08 | Shared BGRA CVPixelBuffer | One pixel buffer can be created with both compatibility flags. |
| 09 | CVMetalTextureCache | Core Video can create a Metal texture cache. |
| 10 | CVOpenGLESTextureCache | Core Video can create an OpenGL ES texture cache for the host context. |
| 11 | CVPixelBuffer to Metal texture | The shared pixel buffer can be exposed as a Metal texture. |
| 12 | CVPixelBuffer to GLES texture | The shared pixel buffer can be exposed as a bindable OpenGL ES texture. |
| 13 | Shared Metal + GLES texture binding | Both texture views can be created from the same pixel buffer and the GLES view can be bound. |
| 14 | Metal compute + blocking completion | Metal can write the shared texture, wait for completion, and expose the result to CPU readback. |
| 15 | Metal compute + completion handler | The same write succeeds when completion is observed through a command-buffer completion handler. |
| 16 | Metal compute + MTLSharedEvent | The compute write can be synchronized to CPU readback through a shared event. |
| 17 | Shared MTLBuffer + CPU readback | Metal can write a shared-storage buffer that the CPU can read. |
| 18 | Shared MTLBuffer + GLES texture upload | CPU-visible Metal output can be copied into a temporary GLES texture. |
| 19 | GLES render into shared texture | OpenGL ES can render into the shared pixel-buffer texture through an FBO, with the result visible in the shared backing. |
| 20 | Full current interop self-test | The existing Metal/GLES shared-texture culling path can initialize and pass its integrated test. |

## Interpreting results

A PASS confirms only the operation listed for that probe. For example, passing
method 13 does not prove that the full level renderer is correct; it proves that
Core Video produced two valid views of the same shared pixel buffer and that
OpenGL ES could bind its view. Method 20 is the integrated check of the current
shared-texture culling implementation.

A FAIL should include the failing API stage or a specific validation message.
A probe failure must not disable Geometry Dash's normal renderer.
