# iOS renderer method matrix

This branch contains **20 selectable rendering configurations**. Each configuration
uses the actual Bismuth scene extraction, ES2 shaders, layer nodes, state tables,
and draw submission path. The selector changes how vertex data reaches OpenGL ES
and how each sprite's camera visibility is determined.

These are five vertex-delivery strategies crossed with four culling strategies:

| Vertex delivery strategy | Shader culling | CPU visibility mask | Metal visibility mask | No camera culling |
|---|---:|---:|---:|---:|
| Persistent VBO | 01 | 02 | 03 | 04 |
| Orphaned VBO | 05 | 06 | 07 | 08 |
| SubData VBO | 09 | 10 | 11 | 12 |
| Client-side vertex arrays | 13 | 14 | 15 | 16 |
| Triple VBO ring | 17 | 18 | 19 | 20 |

## What each axis changes

**Vertex delivery**

- **Persistent VBO:** keep one GL buffer allocation and update dirty vertex ranges.
- **Orphaned VBO:** orphan the buffer storage and upload the complete current stream when the ordered geometry changes.
- **SubData VBO:** allocate capacity and update existing storage with `glBufferSubData`.
- **Client arrays:** pass the ordered CPU vertex memory directly to the ES2 vertex attributes; no VBO is used for the object stream.
- **Triple VBO ring:** rotate across three VBOs, orphaning and filling the selected slot.

**Visibility**

- **Shader culling:** the vertex shader tests the sprite's transformed object anchor against the camera bounds.
- **CPU visibility mask:** the CPU calculates visibility and uploads a packed RGBA mask; the vertex shader samples it.
- **Metal visibility mask:** Metal computes the mask, exposes it to the existing GLES renderer through the shared Core Video texture, and the vertex shader samples it.
- **No camera culling:** every sprite reaches the remaining shader and rasterization stages; other visibility rules such as sprite appearance and opacity still apply.

## How to compare methods

1. Set **iOS renderer method** to the configuration to test.
2. Exit the current level and enter a new level so Bismuth creates a renderer using the new selection.
3. Leave **iOS renderer debug overlay** enabled.
4. Record the result shown by the overlay and Geode log.

A method is reported as a successful first-frame result only after all active Bismuth layer ranges have completed a GL draw in the same frame. Merely creating a shader, VBO, CPU mask, or Metal device is not counted as a successful render.

The overlay shows the method number and name, vertex-delivery strategy, culling strategy, level sprite/object/group counts, frame cadence, draw count, submitted vertices/triangles, upload bytes, missing layer nodes, and any failure reason. The Geode log records a **PASSED FIRST FRAME** line or a method-specific **FAILED** line.

If initialization or a later draw operation fails before ownership has transferred, Geometry Dash's original Cocos batches remain visible. If a method fails after Bismuth has taken ownership, Bismuth restores the saved stock-batch visibility state.

## Important limits

The 20 entries are 20 real end-to-end configurations built from five vertex-delivery implementations and four culling implementations. They are not 20 unrelated graphics APIs or 20 independently rewritten renderers.

The triple-buffer option rotates three VBOs; it does not use explicit GPU fences. The Metal options still depend on the existing Metal/OpenGL ES shared-texture implementation. A crash in a method can still terminate the app before a final log entry is written.

**Build and device status:** repository changes are source-level only until the iOS arm64 Geode build is compiled and tested on-device. No configuration is marked as working until a device run records its first complete frame.
