#include "../src/diagnostics/Diagnostics.hpp"

// The standalone rendering-core tests do not load Geode or exercise file
// diagnostics. These no-op definitions satisfy Device.cpp's instrumentation
// hooks while keeping the tests independent of the Geode SDK.
namespace bismuth::diag {

void start() {}
void heartbeat() {}
void updatePhase(const char*) {}
void checkpoint(const char*) {}
void note(const char*) {}
void setRendererState(const char*, const char*) {}
void setLevelStats(std::uint32_t, std::uint32_t) {}
void setRenderMonitoringEnabled(bool) {}
void renderHeartbeat() {}

} // namespace bismuth::diag
