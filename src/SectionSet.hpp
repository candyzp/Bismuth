#pragma once

#include "glm/fwd.hpp"
#include <algorithm>
#include <common.hpp>

#define DEFAULT_SECTION_SIZE { 50, 50 }

template <typename _Object>
using SectionSetFetchObjectPosition = std::function<glm::vec2(const _Object&)>;

struct Rect {
    glm::vec2 bottomLeft;
    glm::vec2 topRight;
};

inline i32 clampInt(i32 a, i32 min, i32 max) {
    if (a > max) return max;
    if (a < min) return min;
    return a;
}

inline i32 minInt(i32 a, i32 b) {
    return (a < b) ? a : b;
}

inline i32 maxInt(i32 a, i32 b) {
    return (a > b) ? a : b;
}

/*
    This is a set of items (usually GameObjects) that
    are organised based on position in fixed-size sections.

    This is optimized for fast access based on section position.
    Inserting objects on the other hand is slow.
    Removing objects is not possible.
*/
template <typename _Object, glm::vec2 _SectionSize = DEFAULT_SECTION_SIZE>
class SectionSet {
public:
    // const glm::vec2 _SectionSize = DEFAULT_SECTION_SIZE;
    // using _Object = GameObject*;

    using SectionPos = glm::ivec2;
    using Section = std::vector<_Object>;

public:
    inline SectionSet()
        : fetchPositionFunction(nullptr) {}

    inline SectionSet(SectionSetFetchObjectPosition<_Object> func)
        : fetchPositionFunction(func) {}

    inline ~SectionSet() {
        for (auto column : map) {
            if (column == nullptr)
                continue;

            for (auto section : *column) {
                if (section == nullptr) {
                    createCount--;
                    delete section;
                }
            }

            delete column;
            createCount--;
        }

        // geode::log::info("Section Set destroyed with {} left", createCount);
    }

    inline void add(_Object& object) {
        auto sectionPos = getSectionPosition(object);
        // geode::log::info("Add object at section ({}, {})", sectionPos.x, sectionPos.y);
        auto& section = getOrCreateSectionAtPosition(sectionPos);
        section.push_back(object);
    }

    inline Section* getSectionAtPosition(const SectionPos& pos) {
        i32 y = pos.y - sectionYOffset;
        if (y < 0 || y >= map.size())
            return nullptr;

        auto column = map[y];
        if (column == nullptr)
            return nullptr;

        i32 x = pos.x - sectionXOffset;
        if (x < 0 || x >= column->size())
            return nullptr;

        return column->at(x);
    }

    inline void forEachSectionInRect(const Rect& rect, std::function<void(const Section&)> callback) {
        SectionPos min = glm::ivec2(rect.bottomLeft / _SectionSize);
        SectionPos max = glm::ivec2(rect.topRight   / _SectionSize);
        
        // min = (8, 1), max = (13, 4)
        // if (fusk)
        //     geode::log::info("min = ({}, {}), max = ({}, {})", min.x, min.y, max.x, max.y);

        // geode::log::info("minX = {}, maxX = {}, minY = {}, maxY = {}", min.x, max.x, min.y, max.y);

        if (
            max.x < minPos.x || maxPos.x < min.x ||
            max.y < minPos.y || maxPos.y < min.y
        ) {
            return;
        }

        i32 minX = maxInt(min.x - sectionXOffset, 0);
        i32 maxX = max.x - sectionXOffset + 1;

        i32 minY = maxInt(min.y - sectionYOffset, 0);
        i32 maxY = minInt(max.y - sectionYOffset + 1, map.size());

        // geode::log::info("smin = ({}, {}), smax = ({}, {})", minX, minY, maxX, maxY);

        i32 stuff = 0;

        // geode::log::info("Y {} -> {}", minY, maxY);

        for (i32 y = minY; y < maxY; y++) {
            // geode::log::info("Test Y: {}", y);
            auto column = map[y];
            if (column == nullptr) continue;

            i32 maxXClamped = minInt(maxX, column->size());
            // geode::log::info("{} => {}", minX, maxXClamped);
            for (i32 x = minX; x < maxXClamped; x++) {
                // geode::log::info("access column at {} size {}", x, column->size());
                // geode::log::info("Checking section [{}, {}]", x + sectionXOffset, y + sectionYOffset);
                auto section = column->at(x);
                if (section) {
                    // geode::log::info("gottem!");
                    callback(*section);
                    stuff += section->size();
                }
            }
        }
    }

    inline SectionPos getSectionPosition(const _Object& object) const {
        return fetchPositionFunction(object) / _SectionSize;
    }
    
private:
    inline Section& getOrCreateSectionAtPosition(const SectionPos& pos) {
        if (pos.x > maxPos.x) maxPos.x = pos.x;
        if (pos.y > maxPos.y) maxPos.y = pos.y;
        if (pos.x < minPos.x) minPos.x = pos.x;
        if (pos.y < minPos.y) minPos.y = pos.y;

        if (map.size() == 0) {
            sectionYOffset = pos.y;
            sectionXOffset = pos.x;
        }

        // geode::log::info("sectionYOffset = {}", sectionYOffset);

        if (pos.y < sectionYOffset) {
            i32 increase = sectionYOffset - pos.y;
            i32 oldSize  = map.size();
            map.resize(oldSize + increase);
            std::move(map.data(), map.data() + oldSize, map.data() + increase);
            std::fill(map.data(), map.data() + increase, nullptr);
            sectionYOffset = pos.y;
        } else if (pos.y - sectionYOffset >= map.size()) {
            i32 oldSize = map.size();
            map.resize(pos.y - sectionYOffset + 1);
            // geode::log::info("resize Y to {}", pos.y - sectionYOffset + 1);
            std::fill(map.begin() + oldSize, map.end(), nullptr);
        }

        auto& columnPtr = map[pos.y - sectionYOffset];
        if (columnPtr == nullptr) {
            // geode::log::info("New column at {} ({} + {})", pos.y, pos.y - sectionYOffset, sectionYOffset);
            columnPtr = new std::vector<Section*>;
            createCount++;
        }
        auto& column = *columnPtr;

        // geode::log::info("sectionXOffset = {}", sectionXOffset);

        if (pos.x < sectionXOffset) {
            setSectionXOffset(pos.x);
        } else if (pos.x - sectionXOffset >= column.size()) {
            i32 oldSize = column.size();
            // geode::log::info("resize X of {} to {}", pos.y, pos.x - sectionXOffset + 1);
            column.resize(pos.x - sectionXOffset + 1);
            std::fill(column.begin() + oldSize, column.end(), nullptr);
        }

        auto& sectionPtr = column[pos.x - sectionXOffset];
        if (sectionPtr == nullptr) {
            // geode::log::info("New section at Y={}, X={} ({} + {})", pos.y, pos.x, pos.x - sectionXOffset, sectionXOffset);
            sectionPtr = new Section;
            createCount++;
        }
        return *sectionPtr;
    }

    inline void setSectionXOffset(i32 newX) {
        // geode::log::info("setSectionXOffset {} {}", newX, (void*)this);
        if (newX >= sectionXOffset) {}
            // geode::log::info("PROBLEM!");

        assert(newX < sectionXOffset);
        i32 increase = sectionXOffset - newX;

        for (auto column : map) {
            if (column == nullptr)
                continue;

            i32 oldSize = column->size();
            column->resize(oldSize + increase);
            std::move(column->data(), column->data() + oldSize, column->data() + increase);
            std::fill(column->data(), column->data() + increase, nullptr);
        }
        sectionXOffset = newX;
    }

private:
    i32 createCount = 0;

    SectionSetFetchObjectPosition<_Object> fetchPositionFunction;
    i32 sectionXOffset = 0;
    i32 sectionYOffset = 0;
    std::vector<std::vector<Section*>*> map;

    SectionPos minPos;
    SectionPos maxPos;
};