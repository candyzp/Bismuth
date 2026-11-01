#pragma once

#ifdef GEODE_IS_IOS

#include <Geode/Geode.hpp>
#include <string>

namespace IOSUpstream {

inline bool crashDiagnosticsEnabled() {
    auto mod = geode::Mod::get();
    return mod && mod->getSettingValue<bool>("crash_diagnostics");
}

inline void crashBreadcrumb(const std::string& message) {
    if (!crashDiagnosticsEnabled())
        return;

    // Geode's native iOS crash reporter captures recent loader/mod log records
    // alongside the signal details and stack trace. This helper records only
    // context; it deliberately does not install a competing signal handler.
    geode::log::info("[Bismuth crash diagnostics] {}", message);
}

} // namespace IOSUpstream

#endif
