#pragma once

#include <algorithm>
#include <cstddef>

// One indexed buffer can address this many four-vertex sprites with u16
// indices. This is a per-buffer format boundary, not a level-wide sprite cap.
inline constexpr std::size_t MAX_ASSIST_SPRITES_PER_BUFFER = 16383;

template <class SpriteContainer, class Callback>
void forEachAssistBufferChunk(const SpriteContainer& sprites, Callback&& callback) {
    std::size_t offset = 0;
    while (offset < sprites.size()) {
        const std::size_t count = std::min(
            MAX_ASSIST_SPRITES_PER_BUFFER,
            sprites.size() - offset
        );
        callback(offset, count);
        offset += count;
    }
}
