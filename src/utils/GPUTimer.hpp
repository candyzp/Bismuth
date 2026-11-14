#pragma once

#include <common.hpp>
#include <profiler.hpp>

#if defined(GEODE_IS_IOS)

// Metal command-buffer timestamps are reported by the iOS renderer overlay.
// Do not create desktop GL timer queries in the iOS target.
class GPUTimer {
public:
    inline explicit GPUTimer(const char* name) : name(name) {}
    inline void start() {}
    inline void stop() {}
    inline const char* getName() const { return name; }
    inline i64 getTime() const { return 0; }

private:
    const char* name = nullptr;
};

#else

class GPUTimer {
public:
    inline GPUTimer(const char* name)
        : name(name)
    {
        glGenQueries(1, &queryObject);
        profiler::addTimer(this);
    }

    inline ~GPUTimer() {
        glDeleteQueries(1, &queryObject);
        profiler::removeTimer(this);
    }

    inline void start() {
        if (profiler::isTakingGPUTime)
            glBeginQuery(GL_TIME_ELAPSED, queryObject);
    }

    inline void stop() {
        if (profiler::isTakingGPUTime) {
            glEndQuery(GL_TIME_ELAPSED);
            glGetQueryObjecti64v(queryObject, GL_QUERY_RESULT, &time);
        }
    }

    inline const char* getName() const { return name; }
    inline i64 getTime() const { return time; }

private:
    i64 time;
    const char* name;
    u32 queryObject;
};
#endif
