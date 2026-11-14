# Bismuth iOS — renderer method matrix

Experimental Geometry Dash 2.2081 renderer for iOS arm64, based on the
owner-derived scene extraction and OpenGL ES 2 draw path.

This branch adds **20 selectable renderer configurations** to compare five
vertex-delivery paths with four visibility strategies. The active method is
selected through the Geode setting **iOS renderer method**. It includes
persistent, orphaned, SubData, client-array and triple-ring vertex delivery,
combined with vertex-shader, CPU-mask, Metal-mask or disabled camera culling.

The **iOS renderer debug overlay** displays the selected method, draw activity,
submitted geometry, frame cadence and failure reason. The Geode log records a
method-specific first-frame pass only after all active layers have drawn.

Geometry Dash keeps its original Cocos batches visible until the custom renderer
has completed its first full frame. Initialization or draw failures before that
handoff therefore do not intentionally replace the game with a black screen.

See [docs/IOS_RENDER_METHOD_MATRIX.md](docs/IOS_RENDER_METHOD_MATRIX.md) for the
method matrix and comparison procedure.

## Development status

The code and settings are committed to this experimental branch. It has **not**
yet been successfully compiled for iOS arm64 or tested on an iPhone. None of the
20 configurations should be treated as confirmed working until the device
overlay/log records its first complete frame.

<img src="logo.png" alt="Bismuth logo" width="100"/>
