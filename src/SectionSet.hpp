#pragma once

#include "glm/fwd.hpp"
#include <algorithm>
#include <common.hpp>

#define DEFAULT_SECTION_SIZE { 200, 200 }

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
    SectionSet(SectionSetFetchObjectPosition<_Object> func)
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

        geode::log::info("Section Set destroyed with {} left", createCount);
    }

    inline void add(_Object& object) {
        auto sectionPos = getSectionPosition(object);
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

        if (
            max.x < minPos.x || maxPos.x < min.x ||
            max.y < minPos.y || maxPos.y < min.y
        ) {
            return;
        }

        i32 minX = maxInt(min.x - sectionXOffset, 0);
        i32 maxX = max.x - sectionXOffset;

        i32 minY = maxInt(min.y - sectionYOffset, 0);
        i32 maxY = minInt(max.y - sectionYOffset, map.size());

        for (i32 y = minY; y <= maxY; y++) {
            auto column = map[y];
            if (column == nullptr) continue;

            i32 maxXClamped = minInt(maxX, column->size());
            for (i32 x = minX; x <= maxXClamped; x++) {
                auto section = column->at(x);
                if (section)
                    callback(*section);
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

        if (map.size() == 0)
            sectionYOffset = pos.y;

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
            std::fill(map.begin() + oldSize, map.end(), nullptr);
        }

        auto& columnPtr = map[pos.y - sectionYOffset];
        if (columnPtr == nullptr) {
            columnPtr = new std::vector<Section*>;
            createCount++;
        }
        auto& column = *columnPtr;

        if (column.size() == 0)
            sectionXOffset = pos.x;

        if (pos.x < sectionXOffset) {
            setSectionXOffset(pos.x);
        } else if (pos.x - sectionXOffset >= column.size()) {
            i32 oldSize = column.size();
            column.resize(pos.x - sectionXOffset + 1);
            std::fill(column.begin() + oldSize, column.end(), nullptr);
        }

        auto& sectionPtr = column[pos.y - sectionXOffset];
        if (sectionPtr == nullptr) {
            sectionPtr = new Section;
            createCount++;
        }
        return *sectionPtr;
    }

    inline void setSectionXOffset(i32 newX) {
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