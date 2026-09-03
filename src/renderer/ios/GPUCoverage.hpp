#pragma once

#ifdef GEODE_IS_IOS

struct IOSGPUCoverage {
    // RendererIOS still applies its original 1.3x camera expansion after the
    // CameraView is constructed. Keep a very wide deterministic pre-scale so
    // large decoration is submitted long before its anchor reaches screen.
    float preViewScale;
    float preEdgeMargin;
    const char* label;
};

// No density tiers and no fallback mode. If Bismuth is active on iOS, it keeps
// the same GPU-heavy render-ahead policy for every level. The CPU may still do
// gameplay/trigger bookkeeping and build submissions, but drawing remains on
// Bismuth's GPU batch path. This deliberately trades extra off-screen GPU work
// for a much larger decoration runway, reducing late section activation/cuts.
inline constexpr IOSGPUCoverage getIOSGPUCoverage() {
    return { 1.85f, 320.0f, "fixed-gpu-ultra-wide" };
}

#endif
