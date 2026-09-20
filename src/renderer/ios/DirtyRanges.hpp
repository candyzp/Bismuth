#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

// Keep nearby dirty records together, including disjoint records on the same
// texture row. A dense row becomes one transfer, not one driver call per sprite.
template <class Range, class Index>
void buildDirtyRanges(std::vector<Index>& records, Index stride,
    std::size_t rowWidth, std::vector<Range>& ranges) {
    ranges.clear();
    if (records.empty() || !stride || !rowWidth)
        return;
    if (!std::is_sorted(records.begin(), records.end()))
        std::sort(records.begin(), records.end());
    for (auto record : records) {
        const auto start = record * stride;
        const auto end = start + stride;
        if (!ranges.empty()) {
            auto& last = ranges.back();
            const auto lastEnd = last.startTexel + last.texelCount;
            if (start <= lastEnd + stride * 2 || start / rowWidth == (lastEnd - 1) / rowWidth) {
                last.texelCount = std::max(lastEnd, end) - last.startTexel;
                continue;
            }
        }
        ranges.push_back({start, stride});
    }
}
