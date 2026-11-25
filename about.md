# Kyanite

<cr>**Alpha.** Levels that use per-object effects (Scale trigger, area/enter
effects, advanced follow, keyframes) render, but the objects those effects move
stay at their start transform.</c>

Kyanite draws level objects on the GPU. Group transforms, triggers and
visibility are resolved per group in a compute shader every frame, off-screen
objects are culled before they are drawn, and a level is no longer rebuilt into
per-object vertices each frame. Shaders, UI and particles are untouched.

Group triggers run on the GPU: move, rotate, follow, toggle, alpha and pulse.
Hitbox viewers keep working without turning the optimizations off.

Kyanite is **especially** effective with high object co
