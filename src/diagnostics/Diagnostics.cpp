#include "Diagnostics.hpp"

#include <Geode/Geode.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace bismuth::diag {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::int64_t kStallThresholdNs = 4'000'000'000LL;
constexpr std::int64_t kConfirmWindowNs = 1'250'000'000LL;
constexpr auto kPollInterval = std::chrono::milliseconds(400);

std::atomic<bool> g_started{false};
std::atomic<bool> g_freezeReported{false};
std::atomic<std::int64_t> g_lastHeartbeatNs{0};
std::atomic<std::uint32_t> g_objects{0};
std::atomic<std::uint32_t> g_sprites{0};
std::atomic<std::uint32_t> g_freezeIndex{0};

std::array<std::atomic<char>, 160> g_phase{};
std::array<std::atomic<char>, 80> g_backend{};
std::array<std::atomic<char>, 80> g_culling{};

std::filesystem::path g_diagDir;
std::filesystem::path g_sessionPath;
std::filesystem::path g_lastStatePath;
std::mutex g_fileMutex;

std::int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()
    ).count();
}

std::int64_t wallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

template <std::size_t N>
void storeText(std::array<std::atomic<char>, N>& destination, const char* source) {
    for (std::size_t i = 0; i < N; ++i) {
        const char value = source && source[i] ? source[i] : '\0';
        destination[i].store(value, std::memory_order_relaxed);
        if (!value)
            break;
    }
    destination[N - 1].store('\0', std::memory_order_relaxed);
}

template <std::size_t N>
std::string loadText(const std::array<std::atomic<char>, N>& source) {
    std::string value;
    value.reserve(N);
    for (const auto& character : source) {
        const char c = character.load(std::memory_order_relaxed);
        if (!c)
            break;
        value.push_back(c);
    }
    return value;
}

void writeStateFileLocked(const char* event, const char* message) {
    std::ofstream out(g_lastStatePath, std::ios::out | std::ios::trunc);
    if (!out)
        return;

    out << "Bismuth diagnostic state\n";
    out << "timestamp_epoch_ms: " << wallNowMs() << "\n";
    out << "event: " << (event ? event : "state") << "\n";
    out << "message: " << (message ? message : "") << "\n";
    out << "last_phase: " << loadText(g_phase) << "\n";
    out << "backend: " << loadText(g_backend) << "\n";
    out << "culling: " << loadText(g_culling) << "\n";
    out << "level_objects: " << g_objects.load(std::memory_order_relaxed) << "\n";
    out << "level_sprites: " << g_sprites.load(std::memory_order_relaxed) << "\n";
    out.flush();
}

void writeEvent(const char* event, const char* message) {
    if (!g_started.load(std::memory_order_acquire))
        return;

    std::lock_guard lock(g_fileMutex);
    {
        std::ofstream out(g_sessionPath, std::ios::out | std::ios::app);
        if (out) {
            out << "timestamp_epoch_ms=" << wallNowMs()
                << " event=" << (event ? event : "note")
                << " phase=\"" << loadText(g_phase) << "\""
                << " backend=\"" << loadText(g_backend) << "\""
                << " culling=\"" << loadText(g_culling) << "\""
                << " objects=" << g_objects.load(std::memory_order_relaxed)
                << " sprites=" << g_sprites.load(std::memory_order_relaxed)
                << " message=\"" << (message ? message : "") << "\"\n";
            out.flush();
        }
    }
    writeStateFileLocked(event, message);
}

void writeFreezeReport(std::int64_t stalledForNs) {
    const auto index = g_freezeIndex.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto filename = std::string("freeze-stall-")
        + std::to_string(wallNowMs()) + "-"
        + std::to_string(index) + ".txt";
    std::ofstream out(g_diagDir / filename, std::ios::out | std::ios::trunc);
    if (!out)
        return;

    out << "BISMUTH MAIN-LOOP STALL REPORT\n";
    out << "timestamp_epoch_ms: " << wallNowMs() << "\n";
    out << "stall_duration_ms: " << (stalledForNs / 1'000'000) << "\n";
    out << "detection_threshold_ms: " << (kStallThresholdNs / 1'000'000) << "\n";
    out << "last_phase: " << loadText(g_phase) << "\n";
    out << "backend: " << loadText(g_backend) << "\n";
    out << "culling: " << loadText(g_culling) << "\n";
    out << "level_objects: " << g_objects.load(std::memory_order_relaxed) << "\n";
    out << "level_sprites: " << g_sprites.load(std::memory_order_relaxed) << "\n";
    out << "\nDiagnosis:\n";
    out << "- The main game-loop heartbeat stopped advancing while this watchdog thread remained active.\n";
    out << "- This indicates a main-thread stall, blocked graphics/driver call, deadlock, or an unusually long operation.\n";
    out << "- The last_phase/backend/culling fields identify the last recorded Bismuth code path.\n";
    out << "- This report does not prove that Metal itself crashed and does not contain a native stack trace.\n";
    out << "- Match this timestamp with the Geode crash log and the iOS device analytics/crash report.\n";
    out << "- A hard process kill or full-device freeze may prevent any in-process report from being written.\n";
    out.flush();
}

void watchdogLoop() {
    std::int64_t candidateSinceNs = 0;
    std::int64_t candidateHeartbeatNs = 0;

    for (;;) {
        std::this_thread::sleep_for(kPollInterval);

        const auto heartbeatNs = g_lastHeartbeatNs.load(std::memory_order_acquire);
        if (!heartbeatNs)
            continue;

        const auto nowNs = steadyNowNs();
        const auto stalledForNs = nowNs - heartbeatNs;

        if (stalledForNs < kStallThresholdNs) {
            candidateSinceNs = 0;
            candidateHeartbeatNs = 0;
            continue;
        }

        // Confirm the exact same heartbeat is still stale. The extra window
        // reduces false reports during iOS suspend/resume transitions.
        if (candidateHeartbeatNs != heartbeatNs) {
            candidateHeartbeatNs = heartbeatNs;
            candidateSinceNs = nowNs;
            continue;
        }

        if (candidateSinceNs && nowNs - candidateSinceNs >= kConfirmWindowNs) {
            if (!g_freezeReported.exchange(true, std::memory_order_acq_rel))
                writeFreezeReport(stalledForNs);
            candidateSinceNs = 0;
        }
    }
}

} // namespace

void start() {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    auto* mod = geode::Mod::get();
    if (!mod) {
        g_started.store(false, std::memory_order_release);
        return;
    }

    std::error_code error;
    g_diagDir = mod->getSaveDir() / "diagnostics";
    std::filesystem::create_directories(g_diagDir, error);
    if (error) {
        geode::log::error("Bismuth diagnostics: could not create diagnostics directory ({})", error.message());
        g_started.store(false, std::memory_order_release);
        return;
    }

    const auto sessionId = wallNowMs();
    g_sessionPath = g_diagDir / (std::string("session-") + std::to_string(sessionId) + ".log");
    g_lastStatePath = g_diagDir / "last-state.txt";

    storeText(g_phase, "diagnostic recorder starting");
    storeText(g_backend, "not initialized");
    storeText(g_culling, "not initialized");

    checkpoint("diagnostic recorder started");
    note("Watchdog confirms a main-loop stall after about 5.25 seconds. Files are in this mod's save directory under diagnostics/.");

    // The worker reads atomics and writes a separate freeze report. It never
    // calls Metal, OpenGL, cocos2d, or Geode UI functions.
    std::thread(watchdogLoop).detach();
    geode::log::info("Bismuth diagnostics recorder started");
}

void heartbeat() {
    if (!g_started.load(std::memory_order_acquire))
        start();

    g_lastHeartbeatNs.store(steadyNowNs(), std::memory_order_release);

    if (g_freezeReported.exchange(false, std::memory_order_acq_rel))
        checkpoint("main loop resumed after a detected stall");
}

void updatePhase(const char* phase) {
    storeText(g_phase, phase);
}

void checkpoint(const char* phase) {
    updatePhase(phase);
    writeEvent("checkpoint", phase);
}

void note(const char* message) {
    writeEvent("note", message);
}

void setRendererState(const char* backend, const char* culling) {
    storeText(g_backend, backend);
    storeText(g_culling, culling);

    const auto message = std::string("renderer backend=")
        + (backend ? backend : "unknown")
        + " culling=" + (culling ? culling : "unknown");
    writeEvent("renderer", message.c_str());
}

void setLevelStats(std::uint32_t objects, std::uint32_t sprites) {
    g_objects.store(objects, std::memory_order_relaxed);
    g_sprites.store(sprites, std::memory_order_relaxed);

    const auto message = std::string("level workload objects=")
        + std::to_string(objects)
        + " sprites=" + std::to_string(sprites);
    writeEvent("level", message.c_str());
}

} // namespace bismuth::diag
