#pragma once

#include <cstdint>

namespace bismuth::diag {
void start();
void heartbeat();
void updatePhase(const char* phase);
void checkpoint(const char* phase);
void note(const char* message);
void setRendererState(const char* backend, const char* culling);
void setLevelStats(std::uint32_t objects, std::uint32_t sprites);
} // namespace bismuth::diag
