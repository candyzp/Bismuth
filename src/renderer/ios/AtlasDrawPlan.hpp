#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct AtlasDrawRun {
    std::size_t firstSlot;
    std::size_t slotCount;
    std::size_t firstIndex;
};

template <class Owner>
void buildAtlasDrawPlan(
    const std::vector<Owner>& owners,
    std::vector<AtlasDrawRun>& runs,
    std::vector<std::uint32_t>& indices
) {
    runs.clear();
    indices.clear();
    for (std::size_t slot = 0; slot < owners.size(); ++slot) {
        const auto& owner = owners[slot];
        if (runs.empty() || !owner.sameOwner(owners[runs.back().firstSlot]))
            runs.push_back({slot, 0, indices.size()});
        ++runs.back().slotCount;
        if (owner.empty())
            continue;
        const auto base = owner.baseVertex;
        for (std::uint32_t corner : {0u, 2u, 3u, 0u, 3u, 1u})
            indices.push_back(static_cast<std::uint32_t>(base + corner));
    }
}
