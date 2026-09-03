#pragma once

#ifdef GEODE_IS_IOS

#include <common.hpp>

struct IOSGPUCoverage {
    float viewScale;
    float edgeMargin;
    const char* label;
};

// Bismuth keeps the complete baked level geometry resident in the GPU VBO.
// These values only control how much of that geometry is submitted each frame.
// Give the A15 a much wider render-ahead band on ordinary levels, while keeping
// the existing conservative window once a level is already extremely dense.
inline IOSGPUCoverage getIOSGPUCoverage(usize totalIndexCount) {
    if (totalIndexCount <= 300000)
        return { 2.10f, 180.0f, "aggressive" };
    if (totalIndexCount <= 900000)
        return { 1.75f, 140.0f, "wide" };
    if (totalIndexCount <= 1800000)
        return { 1.50f, 100.0f, "balanced" };
    return { 1.30f, 72.0f, "dense-safe" };
}

#endif
