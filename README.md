# Bismuth iOS

Experimental Geometry Dash 2.2081 rendering optimization project for iOS.

The current architecture keeps Geometry Dash authoritative for gameplay, triggers,
animations, colors, visibility, and sprite hierarchy. Bismuth observes the final
resolved visual state and is being rebuilt to move safe render-only work to the GPU:

- translation / rotation / scale math
- camera transforms
- persistent GPU state
- dirty-state uploads
- conservative safe-object batching

Correctness comes first. Complex or uncertain objects stay on Geometry Dash's stock
renderer instead of being reimplemented inside Bismuth.

<img src="logo.png" alt="Bismuth logo" width="100"/>
