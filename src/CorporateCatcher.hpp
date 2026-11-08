#pragma once

#include <chrono>

namespace CorporateCatcher {

class HookScope final {
public:
    explicit HookScope(const char* name) noexcept;
    ~HookScope() noexcept;

    HookScope(const HookScope&) = delete;
    HookScope& operator=(const HookScope&) = delete;

private:
    const char* m_name = nullptr;
    std::chrono::steady_clock::time_point m_start{};
    bool m_active = false;
};

// Call once at the beginning of each display-link frame. The interval between
// calls is the measured frame time; any interval below 60 FPS is a lag event.
void beginFrame() noexcept;

} // namespace CorporateCatcher

#define CORPORATE_CATCHER_PROFILE_HOOK(name) \
    ::CorporateCatcher::HookScope corporateCatcherHookScope(name)
