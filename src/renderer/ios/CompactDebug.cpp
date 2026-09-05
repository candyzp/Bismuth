#ifdef GEODE_IS_IOS

#include <Geode/modify/CCLabelBMFont.hpp>
#include <cstdlib>
#include <string>

using namespace geode::prelude;

namespace {
bool readMetric(const std::string& text, const char* marker, usize& value) {
    const auto pos = text.find(marker);
    if (pos == std::string::npos)
        return false;

    const char* start = text.c_str() + pos + std::char_traits<char>::length(marker);
    char* end = nullptr;
    const auto parsed = std::strtoull(start, &end, 10);
    if (end == start)
        return false;

    value = static_cast<usize>(parsed);
    return true;
}
}

class $modify(BismuthCompactDebugLabel, cocos2d::CCLabelBMFont) {
    void setString(const char* newString) {
        if (!newString) {
            cocos2d::CCLabelBMFont::setString(newString);
            return;
        }

        const std::string text(newString);
        if (!text.starts_with("Bismuth iOS GPU Assist")) {
            cocos2d::CCLabelBMFont::setString(newString);
            return;
        }

        usize gpuCalls = 0;
        usize indices = 0;
        usize cpuSaved = 0;
        readMetric(text, "GPU draws: ", gpuCalls);
        readMetric(text, "indices: ", indices);
        readMetric(text, "CPU skipped last frame: ", cpuSaved);

        const usize gpuSprites = indices / 6;
        const auto compact = fmt::format(
            "Bismuth GPU\n"
            "GPU Draw: {} sprites/frame\n"
            "Calls: {} | CPU Saved: {}",
            gpuSprites,
            gpuCalls,
            cpuSaved
        );

        cocos2d::CCLabelBMFont::setString(compact.c_str());
    }
};

#endif
