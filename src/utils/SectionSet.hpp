#pragma once

#include "VectorMap.hpp"
#include "glm/fwd.hpp"
#include <algorithm>
#include <common.hpp>
#include <unordered_map>

#define DEFAULT_SECTION_SIZE { 100, 100 }

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

private:
    using Section = std::vector<_Object>;

    struct FastColumn {
        _Object* array;
        VectorMap<_Object*> sections;

        inline ~FastColumn() { if (array) delete[] array; }
    };

public:
    inline SectionSet()
        : fetchPositionFunction(nullptr) {}

    inline SectionSet(SectionSetFetchObjectPosition<_Object> func)
        : fetchPositionFunction(func) {}

    inline void add(_Object& object) {
        auto sectionPos = getSectionPosition(object);
        boundRect.include(sectionPos);
        writeSections[sectionPos.y][sectionPos.x].push_back(object);
    }

    inline void generateFastStructure() {
        for (auto& [y, _] : writeSections)
            fastMap.expectKey(y);

        fastMap.allocate();
    
        for (auto& [y, section] : writeSections)
            generateFastColumn(fastMap[y], section);
    }

    inline void forEachSectionInRect(const Rect& rect, std::function<void(const std::span<_Object>&)> callback) {
        IRect srect = {
            glm::ivec2(rect.bottomLeft / _SectionSize),
            glm::ivec2(rect.topRight   / _SectionSize) + glm::ivec2(1, 1)
        };

        // IRect uses half-open [min, max) ranges, while boundRect is built from
        // discrete occupied section coordinates. Expand the stored max by one
        // so the highest occupied row/column is not accidentally treated as
        // outside the set. Sparse transform groups can otherwise lose whole
        // chunks of decoration at their section boundary.
        IRect occupiedBounds = boundRect;
        occupiedBounds.max += glm::ivec2(1, 1);

        if (!srect.intersects(occupiedBounds))
            return;

        IRange rangeY = fastMap.getRange().intersection(srect.rangeY());

        for (i32 y : rangeY) {
            auto& column = fastMap[y];
            
            auto rangeX = column.sections.getRange();
            rangeX.max--;
            rangeX = rangeX.intersection(srect.rangeX());

            if (rangeX.isEmpty())
                continue;

            _Object* begin = column.sections[rangeX.min];
            _Object* end   = column.sections[rangeX.max];

            callback({ begin, end });
        }
    }

    inline SectionPos getSectionPosition(const _Object& object) const {
        return fetchPositionFunction(object) / _SectionSize;
    }
    
private:
    inline void generateFastColumn(FastColumn& fastColumn, const std::unordered_map<i32, Section>& column) {
        usize columnObjectCount = 0;

        for (auto& [x, section] : column) {
            columnObjectCount += section.size();
            fastColumn.sections.expectKey(x);
        }
        auto range = fastColumn.sections.getRange();

        fastColumn.sections.expectKey(range.max);
        fastColumn.sections.allocate();

        _Object* ptr = new _Object[columnObjectCount];
        fastColumn.array = ptr;

        for (i32 x : range) {
            fastColumn.sections[x] = ptr;

            const Section* section = nullptr;
            if (column.find(x) != column.end())
                section = &column.at(x);

            if (section) {
                std::copy(section->begin(), section->end(), ptr);
                ptr += section->size();
            }
        }

        fastColumn.sections[range.max] = ptr;
    }

private:
    i32 createCount = 0;

    SectionSetFetchObjectPosition<_Object> fetchPositionFunction;
    i32 sectionXOffset = 0;
    i32 sectionYOffset = 0;
    std::vector<std::vector<Section*>*> map;

    IRect boundRect;
    std::unordered_map<i32, std::unordered_map<i32, Section>> writeSections;
    VectorMap<FastColumn> fastMap;

    SectionPos minPos;
    SectionPos maxPos;
};