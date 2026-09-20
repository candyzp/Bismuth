# v0.1.2-alpha

- Upload dirty atlas buffers directly, removing the hidden full-atlas render pass.
- Coalesce state texture transfers and retry failed uploads.
- Refresh live owned decoration geometry and preserve nested child updates.
- Draw the real background through a persistent GPU quad, including black/color changes.
- Report owned GPU draw failures without silently redrawing those sprites through stock Cocos.
- Add iOS build and rendering regression checks for pull requests.

# 0.1.0-alpha
- Added rotate trigger support
- Added follow trigger support
- Added shader sprite optimization
- Added seperate batch node optimization
- Added index culling optimization
- Fixed Intel (and AMD?) GPU crashes

# 0.0.1-alpha.1
- Added renderer
- Added color trigger support
- Added alpha trigger support
- Added move trigger support
- Added toggle trigger support
- Added object HSV
- Objects now rotate
- Pulsating objects now pulse to audio scale