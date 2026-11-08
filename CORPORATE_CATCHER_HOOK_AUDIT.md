# Corporate Catcher — hook coverage

This branch adds observation-only diagnostics to Bismuth. It does not alter renderer selection, gameplay simulation, collision behavior, or the contents of the hooks.

## FPS rule

Frames are timed using the interval between consecutive `CCDisplayLinkDirector::mainLoop` entries. The measured instantaneous rate is `1000 / frame_ms`. Any measured rate strictly below 60 FPS is counted as a lag spike (equivalent to a frame interval greater than 16.6667 ms).

A one-second report includes average FPS, minimum observed FPS, measured frame count, sub-60 frame count, the slowest frame, and the five hooks with the greatest cumulative inclusive time in that reporting window. When a sub-60 frame occurs, the report also includes the five hooks with the greatest inclusive time in that worst frame. Nested hook durations overlap and should not be summed as independent frame costs.

## Instrumented active Bismuth hook points

1. `CCDirector::purgeDirector` — `src/common.cpp`
2. `PlayLayer::setupHasCompleted` — `src/renderer/hooks.cpp`
3. `PlayLayer::resetLevel` — `src/renderer/hooks.cpp`
4. `PlayLayer::updateVisibility` — `src/renderer/hooks.cpp`
5. `GJBaseGameLayer::update` — `src/renderer/hooks.cpp`
6. `GJBaseGameLayer::processMoveActions` — `src/renderer/hooks.cpp`
7. `GJBaseGameLayer::processRotationActions` — `src/renderer/hooks.cpp`
8. `GJBaseGameLayer::processFollowActions` — `src/renderer/hooks.cpp`
9. `GJBaseGameLayer::toggleGroup` — `src/renderer/hooks.cpp`
10. `GJBaseGameLayer::optimizeMoveGroups` — `src/renderer/hooks.cpp`
11. `GameObject::activateObject` — `src/renderer/hooks.cpp`
12. `GameObject::deactivateObject` — `src/renderer/hooks.cpp`
13. `CCSprite::updateTransform` — `src/renderer/hooks.cpp`
14. `CCSpriteBatchNode::draw` — `src/renderer/hooks.cpp`
15. `CCDisplayLinkDirector::mainLoop` — `src/renderer/hooks.cpp`
16. `PlayLayer::onEnterTransitionDidFinish` — `src/renderer/ios/PlayLayerExitGuard.cpp`
17. `PlayLayer::onExit` — `src/renderer/ios/PlayLayerExitGuard.cpp`
18. `PauseLayer::onResume` — `src/renderer/ios/PlayLayerExitGuard.cpp`
19. `CCSprite::draw` — `src/renderer/ios/StandaloneHooks.cpp`
20. `CCSprite::visit` — `src/renderer/ios/StandaloneHooks.cpp`

The coverage is all active hook methods declared by Bismuth in this branch, not every hook installed by Geometry Dash or unrelated third-party Geode mods. The desktop-only generated profiler file is not part of the iOS build and is intentionally not enabled; its generated Windows hook set is not a safe substitute for the actual iOS hook inventory.

## Diagnostic overhead

Each instrumented hook takes two steady-clock timestamps while the setting is enabled. Samples are aggregated in fixed-size arrays and logged once per second to avoid per-call logging and unbounded sample growth. Disable **Corporate Catcher diagnostics** in mod settings to turn the instrumentation off.
