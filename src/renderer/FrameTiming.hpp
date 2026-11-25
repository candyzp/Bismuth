#pragma once

#include <algorithm>
#include <common.hpp>

/*
    Measures how long each part of a rendered frame takes and writes an
    average breakdown to the Geode log every few seconds.

    The stages are exclusive, so "other" is everything the renderer doesn't
    account for (game update, UI, swap, ...).
*/
struct FrameTiming {
    bool enabled = true;

    //// TOP LEVEL STAGES (nanoseconds accumulated over the logging window) ////
    u64 frameTime = 0;
    u64 uniforms = 0;
    u64 colorChannels = 0;
    u64 groupStates = 0;
    u64 culling = 0;
    u64 draw = 0;

    //// BREAKDOWN OF groupStates AND culling ////
    u64 groupStatesAlpha = 0;
    u64 groupStatesPulse = 0;
    u64 groupStatesUpload = 0;
    u64 sortedBatch = 0;

    inline void setEnabled(bool value) {
        enabled = value;
    }

    inline void countSortedBatchGeneration() {
        sortedBatchGenerations++;
    }

    /*
        Called at the start of every frame. The time between two calls is the
        frame time, which is what the FPS in the log is calculated from.
    */
    inline void beginFrame() {
        u64 now = getTime();

        if (lastFrameStart != 0)
            frameTime += now - lastFrameStart;

        lastFrameStart = now;
        frameCount++;
    }

    /*
        Called once the object layer finished drawing. Logs the average of the
        current window when enough time passed.
    */
    inline void endFrame() {
        if (!enabled || lastFrameStart == 0)
            return;

        u64 now = getTime();

        if (windowStart == 0) {
            windowStart = now;
            return;
        }

        if ((double)(now - windowStart) / 1e9 < LOG_INTERVAL_SECONDS)
            return;

        double frames = (double)frameCount;

        if (frames < 1.0) {
            windowStart = now;
            return;
        }

        double frameMs = (double)frameTime / frames / 1e6;
        double fps = frameMs > 0.0 ? 1000.0 / frameMs : 0.0;

        u64 measured = uniforms + colorChannels + groupStates + culling + draw;
        double otherUs = (double)(frameTime > measured ? frameTime - measured : 0) / frames / 1000.0;

        geode::log::info(
            "Frame timing: {:.0f} FPS ({:.2f} ms/frame) | uniforms {:.0f} us"
            " | color channels {:.0f} us | group states {:.0f} us (alpha {:.0f}, pulse {:.0f}, upload {:.0f})"
            " | culling {:.0f} us (sorted batch {:.0f}, {} regen) | draw {:.0f} us | other {:.0f} us",
            fps,
            frameMs,
            toUs(uniforms, frames),
            toUs(colorChannels, frames),
            toUs(groupStates, frames),
            toUs(groupStatesAlpha, frames),
            toUs(groupStatesPulse, frames),
            toUs(groupStatesUpload, frames),
            toUs(culling, frames),
            toUs(sortedBatch, frames),
            sortedBatchGenerations,
            toUs(draw, frames),
            otherUs
        );

        frameTime = 0;
        uniforms = 0;
        colorChannels = 0;
        groupStates = 0;
        culling = 0;
        draw = 0;
        groupStatesAlpha = 0;
        groupStatesPulse = 0;
        groupStatesUpload = 0;
        sortedBatch = 0;
        frameCount = 0;
        sortedBatchGenerations = 0;
        windowStart = now;
    }

private:
    static constexpr double LOG_INTERVAL_SECONDS = 5.0;

    inline static double toUs(u64 total, double frames) {
        return (double)total / frames / 1000.0;
    }

    u64 lastFrameStart = 0;
    u64 windowStart = 0;
    u32 frameCount = 0;
    u32 sortedBatchGenerations = 0;
};

/*
    Adds the time spent in a scope to a FrameTiming counter.
*/
class ScopedFrameTimer {
public:
    explicit inline ScopedFrameTimer(u64& target)
        : target(target), startTime(getTime()) {}

    inline ~ScopedFrameTimer() {
        target += getTime() - startTime;
    }

private:
    u64& target;
    u64 startTime;
};
