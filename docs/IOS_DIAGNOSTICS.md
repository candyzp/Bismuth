# iOS diagnostics

Bismuth writes diagnostics in its Geode mod save directory:

`<Geode mod save directory>/diagnostics/`

- `session-<timestamp>.log`: backend choices, initialization checkpoints, and level workload.
- `last-state.txt`: the latest durable checkpoint, kept across an abrupt process exit.
- `freeze-main-loop-stall-<timestamp>-<number>.txt`: a report when the main game loop stops advancing for about 5.25 seconds.
- `freeze-render-stall-<timestamp>-<number>.txt`: a report when an active gameplay renderer stops receiving draw calls for about 3 seconds, even if the main loop continues.

The watchdog runs separately and never calls Metal, OpenGL, cocos2d, or Geode UI functions. It watches both main-loop progress and Bismuth gameplay draw-call progress. It records the last renderer phase and backend, but cannot capture a native thread stack. A renderer-draw report means Bismuth's draw method stopped being visited; it cannot detect a GPU that silently keeps showing an old frame while draw calls continue returning. A hard process kill or full-device freeze may prevent an in-process report from being written; in that case, match `last-state.txt` with Geode's crash log and the iOS device analytics report.

The hot-path heartbeat uses atomics. File writes occur only at named checkpoints and when a stall is confirmed.
