#pragma once

#include "common.hpp"

struct BProfilerCategory {
    const char* name;

    inline BProfilerCategory(const char* name)
        : name(name) {}
};

class BProfiler {
public:
    static void start();

    static void end();

    static void category(BProfilerCategory* category);

    static bool isUsingAverages();

    static void useAverages(bool cond);

    static inline void category(BProfilerCategory& cat) { category(&cat); }

    static std::string toString();
};