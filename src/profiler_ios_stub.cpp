#if defined(GEODE_IS_IOS)

#include "profiler.hpp"

bool profiler::isTakingGPUTime = false;

namespace profiler {
void functionPush(const char*) {}
void functionPop() {}
void addTimer(GPUTimer*) {}
void removeTimer(GPUTimer*) {}
}

#endif
