#pragma once

#ifdef GEODE_IS_IOS

struct IOSGPUCoverage {
    // RendererIOS still applies its original 1.3x camera expansion after the
    // CameraView is constructed. This fixed pre-scale makes the effective view
    // roughly 1.75x without choosing a different path for dense levels.
    float preViewScale;
    float preEdgeMargin;
    const char* label;
};

// No density tiers and no fallback mode. If Bismuth is active on iOS, it keeps
// the same GPU-heavy render-ahead policy for every level. The CPU may still do
// gameplay/trigger bookkeeping and build submissions, but drawing remains on
// Bismuth's GPU batch path.
inline constexpr IOSGPUCoverage getIOSGPUCoverage() {
    return { 1.35f, 64.0f, "fixed-gpu-wide" };
}

#endif
