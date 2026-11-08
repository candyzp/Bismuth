#include "CorporateCatcher.hpp"

#include <Geode/Geode.hpp>
#include <array>
#include <cstdint>
#include <cstring>

using namespace geode::prelude;

namespace CorporateCatcher {
namespace {

constexpr std::size_t kMaxTrackedHooks = 64;
constexpr std::size_t kWorstHookCount = 5;
constexpr double kLagThresholdFps = 60.0;

constexpr const char* kInstrumentedHooks[] = {
    "CCDirector::purgeDirector",
    "PlayLayer::setupHasCompleted",
    "PlayLayer::resetLevel",
    "PlayLayer::updateVisibility",
    "GJBaseGameLayer::update",
    "GJBaseGameLayer::processMoveActions",
    "GJBaseGameLayer::processRotationActions",
    "GJBaseGameLayer::processFollowActions",
    "GJBaseGameLayer::toggleGroup",
    "GJBaseGameLayer::optimizeMoveGroups",
    "GameObject::activateObject",
    "GameObject::deactivateObject",
    "CCSprite::updateTransform",
    "CCSpriteBatchNode::draw",
    "CCDisplayLinkDirector::mainLoop",
    "PlayLayer::onEnterTransitionDidFinish",
    "PlayLayer::onExit",
    "PauseLayer::onResume",
    "CCSprite::draw",
    "CCSprite::visit"
};

struct HookRecord {
    const char* name = nullptr;
    std::uint64_t frameNs = 0;
    std::uint64_t windowNs = 0;
    std::uint32_t frameCalls = 0;
    std::uint32_t windowCalls = 0;
};

struct WorstHook {
    const char* name = nullptr;
    std::uint64_t timeNs = 0;
    std::uint32_t calls = 0;
};

std::array<HookRecord, kMaxTrackedHooks> g_hooks{};
std::array<WorstHook, kWorstHookCount> g_worstHooks{};
std::size_t g_hookCount = 0;

bool g_enabled = false;
bool g_hasFrameStart = false;
bool g_inventoryLogged = false;

std::chrono::steady_clock::time_point g_lastFrameStart{};
std::chrono::steady_clock::time_point g_reportStart{};

std::uint64_t g_windowFrames = 0;
std::uint64_t g_windowLagFrames = 0;
double g_windowMinFps = 100000.0;
double g_worstFrameMs = 0.0;
std::uint64_t g_overflowedHookNames = 0;

bool settingEnabled() noexcept {
    auto mod = Mod::get();
    return mod && mod->getSettingValue<bool>("corporate_catcher_enabled");
}

void clearFrameSamples() noexcept {
    for (auto& hook : g_hooks) {
        hook.frameNs = 0;
        hook.frameCalls = 0;
    }
}

void clearWindowSamples() noexcept {
    for (auto& hook : g_hooks) {
        hook.windowNs = 0;
        hook.windowCalls = 0;
    }
    g_windowFrames = 0;
    g_windowLagFrames = 0;
    g_windowMinFps = 100000.0;
    g_worstFrameMs = 0.0;
    g_overflowedHookNames = 0;
    g_worstHooks = {};
}

void recordHookTime(const char* name, std::uint64_t elapsedNs) noexcept {
    if (!name || elapsedNs == 0)
        return;

    HookRecord* record = nullptr;
    for (std::size_t i = 0; i < g_hookCount; ++i) {
        if (g_hooks[i].name == name || std::strcmp(g_hooks[i].name, name) == 0) {
            record = &g_hooks[i];
            break;
        }
    }

    if (!record) {
        if (g_hookCount >= g_hooks.size()) {
            ++g_overflowedHookNames;
            return;
        }
        record = &g_hooks[g_hookCount++];
        record->name = name;
    }

    record->frameNs += elapsedNs;
    ++record->frameCalls;
}

void captureWorstFrameHooks() noexcept {
    g_worstHooks = {};

    for (std::size_t i = 0; i < g_hookCount; ++i) {
        const auto& hook = g_hooks[i];
        if (!hook.frameNs)
            continue;

        for (std::size_t rank = 0; rank < g_worstHooks.size(); ++rank) {
            if (hook.frameNs > g_worstHooks[rank].timeNs) {
                for (std::size_t move = g_worstHooks.size() - 1; move > rank; --move)
                    g_worstHooks[move] = g_worstHooks[move - 1];

                g_worstHooks[rank] = {hook.name, hook.frameNs, hook.frameCalls};
                break;
            }
        }
    }
}

void addFrameSamplesToWindow() noexcept {
    for (auto& hook : g_hooks) {
        hook.windowNs += hook.frameNs;
        hook.windowCalls += hook.frameCalls;
    }
}

void logWindowHookRanking() {
    std::array<bool, kMaxTrackedHooks> used{};

    for (std::size_t rank = 0; rank < kWorstHookCount; ++rank) {
        std::size_t bestIndex = kMaxTrackedHooks;
        std::uint64_t bestTime = 0;

        for (std::size_t i = 0; i < g_hookCount; ++i) {
            if (!used[i] && g_hooks[i].windowNs > bestTime) {
                bestTime = g_hooks[i].windowNs;
                bestIndex = i;
            }
        }

        if (bestIndex == kMaxTrackedHooks)
            break;

        used[bestIndex] = true;
        const auto& hook = g_hooks[bestIndex];
        const double totalMs = static_cast<double>(hook.windowNs) / 1000000.0;
        const double averageMs = hook.windowCalls
            ? totalMs / static_cast<double>(hook.windowCalls)
            : 0.0;

        log::info(
            "[Corporate Catcher] hook #{} {}: {:.3f}ms total, {:.4f}ms/call, {} calls",
            rank + 1, hook.name, totalMs, averageMs, hook.windowCalls
        );
    }
}

void reportWindow(double elapsedSeconds) {
    if (elapsedSeconds <= 0.0 || g_windowFrames == 0)
        return;

    const double averageFps = static_cast<double>(g_windowFrames) / elapsedSeconds;
    const double worstFps = g_worstFrameMs > 0.0 ? 1000.0 / g_worstFrameMs : averageFps;

    log::info(
        "[Corporate Catcher] {:.1f} FPS average | min {:.1f} FPS | {} measured frames | {} below 60 FPS | worst {:.2f}ms | {}/{} hook names observed",
        averageFps,
        g_windowMinFps,
        g_windowFrames,
        g_windowLagFrames,
        g_worstFrameMs,
        g_hookCount,
        sizeof(kInstrumentedHooks) / sizeof(kInstrumentedHooks[0])
    );

    if (g_windowLagFrames > 0) {
        log::warn(
            "[Corporate Catcher] LAG SPIKES: {} frame(s) under 60 FPS; worst frame {:.2f}ms ({:.1f} FPS)",
            g_windowLagFrames, g_worstFrameMs, worstFps
        );

        for (const auto& hook : g_worstHooks) {
            if (!hook.name || !hook.timeNs)
                continue;

            log::warn(
                "[Corporate Catcher] worst-frame hook {}: {:.3f}ms across {} call(s)",
                hook.name,
                static_cast<double>(hook.timeNs) / 1000000.0,
                hook.calls
            );
        }
    }

    logWindowHookRanking();

    if (g_overflowedHookNames > 0) {
        log::warn(
            "[Corporate Catcher] hook sample capacity reached; {} additional hook-name sample(s) were not recorded",
            g_overflowedHookNames
        );
    }
}

} // namespace

HookScope::HookScope(const char* name) noexcept
    : m_name(name), m_active(g_enabled) {
    if (m_active)
        m_start = std::chrono::steady_clock::now();
}

HookScope::~HookScope() noexcept {
    if (!m_active)
        return;

    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - m_start
    ).count();

    if (elapsed > 0)
        recordHookTime(m_name, static_cast<std::uint64_t>(elapsed));
}

void beginFrame() noexcept {
    g_enabled = settingEnabled();
    if (!g_enabled) {
        g_hasFrameStart = false;
        g_inventoryLogged = false;
        g_hookCount = 0;
        g_hooks = {};
        clearWindowSamples();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!g_inventoryLogged) {
        log::info(
            "[Corporate Catcher] scanning {} active Bismuth hook points; lag threshold is below 60 FPS",
            sizeof(kInstrumentedHooks) / sizeof(kInstrumentedHooks[0])
        );
        g_inventoryLogged = true;
    }

    if (!g_hasFrameStart) {
        g_lastFrameStart = now;
        g_reportStart = now;
        g_hasFrameStart = true;
        clearFrameSamples();
        return;
    }

    const double elapsedMs = std::chrono::duration<double, std::milli>(
        now - g_lastFrameStart
    ).count();
    g_lastFrameStart = now;

    if (elapsedMs > 0.0) {
        const double fps = 1000.0 / elapsedMs;
        ++g_windowFrames;
        if (fps < g_windowMinFps)
            g_windowMinFps = fps;

        if (fps < kLagThresholdFps) {
            ++g_windowLagFrames;
            if (elapsedMs > g_worstFrameMs) {
                g_worstFrameMs = elapsedMs;
                captureWorstFrameHooks();
            }
        }
    }

    addFrameSamplesToWindow();
    clearFrameSamples();

    const double reportElapsedSeconds = std::chrono::duration<double>(
        now - g_reportStart
    ).count();

    if (reportElapsedSeconds >= 1.0) {
        reportWindow(reportElapsedSeconds);
        clearWindowSamples();
        g_reportStart = now;
    }
}

} // namespace CorporateCatcher
