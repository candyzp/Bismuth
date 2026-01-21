#include "BProfiler.hpp"
#include "common.hpp"
#include <fmt/format.h>

static std::map<BProfilerCategory*, u64> categoryTimes;
static BProfilerCategory* currentCategory = nullptr;
static u64 lastTime = 0;

static bool shouldUseAverages = false;
static usize averageCount = 0;

void BProfiler::start() {
    if (shouldUseAverages)
        averageCount++;
    else
        categoryTimes.clear();
    currentCategory = nullptr;
}

void BProfiler::end() {
    u64 diff = getTime() - lastTime;
    if (currentCategory == nullptr)
        return;
    auto it = categoryTimes.find(currentCategory);
    if (it != categoryTimes.end())
        it->second += diff;
    else
        categoryTimes[currentCategory] = diff;
    currentCategory = nullptr;
}

void BProfiler::category(BProfilerCategory* category) {
    end();
    currentCategory = category;
    lastTime = getTime();
}

bool BProfiler::isUsingAverages() {
    return shouldUseAverages;
}

void BProfiler::useAverages(bool cond) {
    categoryTimes.clear();
    shouldUseAverages = cond;
    averageCount = 0;
}

std::string BProfiler::toString() {
    std::string ret = "";
    for (auto& [cat, time] : categoryTimes) {
        auto dtime = (double)time;
        if (shouldUseAverages)
            dtime /= averageCount;
        ret += fmt::format("{}: {:.5f}ms\n", cat->name, dtime / 1000000.0);
    }
    return ret;
}