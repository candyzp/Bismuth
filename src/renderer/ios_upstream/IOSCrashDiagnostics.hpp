#pragma once

#ifdef GEODE_IS_IOS

#include <Geode/Geode.hpp>
#include <string>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>

namespace IOSUpstream {

inline bool crashDiagnosticsEnabled() {
    auto mod = geode::Mod::get();
    return mod && mod->getSettingValue<bool>("crash_diagnostics");
}

inline void crashBreadcrumb(const std::string& message) {
    if (!crashDiagnosticsEnabled())
        return;

    // Keep the normal Geode log entry for the native crash report, but also
    // persist a small, ordered breadcrumb trail in this mod's save directory.
    // Flush every record: the last completed stage should survive a jetsam or
    // native crash even when Geode cannot finish writing its own log.
    static std::mutex fileMutex;
    static std::ofstream breadcrumbFile;
    static bool fileOpenAttempted = false;
    static std::uint64_t sequence = 0;
    static const auto sessionStart = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(fileMutex);
    const auto recordNumber = ++sequence;
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - sessionStart
    ).count();

    geode::log::info("[Bismuth crash diagnostics #{:06}] {}", recordNumber, message);

    if (!fileOpenAttempted) {
        fileOpenAttempted = true;
        if (auto mod = geode::Mod::get()) {
            const auto directory = mod->getSaveDir();
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (!error) {
                const auto breadcrumbPath = directory / "crash-breadcrumbs.log";
                breadcrumbFile.open(
                    breadcrumbPath,
                    std::ios::out | std::ios::app
                );
                if (breadcrumbFile.is_open())
                    geode::log::info("Bismuth crash breadcrumb file: {}", breadcrumbPath.string());
            }
        }

        if (!breadcrumbFile.is_open())
            geode::log::warn("Bismuth crash diagnostics could not open crash-breadcrumbs.log in the mod save directory");
    }

    if (breadcrumbFile.is_open()) {
        breadcrumbFile << elapsedMs << "ms #" << recordNumber << " " << message << '\n';
        breadcrumbFile.flush();
    }
}

} // namespace IOSUpstream

#endif
