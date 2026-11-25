#pragma once

#include <common.hpp>

struct IRange {
    i32 min = INT32_MAX;
    i32 max = INT32_MIN;

    GEODE_INLINE i32 length() const { return max - min; }

    GEODE_INLINE bool isEmpty() const { return min >= max; }

    GEODE_INLINE void include(i32 v) {
        if (v >= max) max = v + 1;
        if (v <  min) min = v;
    }

    GEODE_INLINE bool contains(i32 v) const { return v >= min && v < max; }

    GEODE_INLINE IRange intersection(const IRange& range) const {
        return {
            (min > range.min) ? min : range.min,
            (max < range.max) ? max : range.max,
        };
    }

    struct Iterator {
        i32 index;

        GEODE_INLINE i32 operator*() const { return index; }

        GEODE_INLINE bool operator==(const Iterator& o) const { return index == o.index; }
        GEODE_INLINE bool operator!=(const Iterator& o) const { return index != o.index; }

        GEODE_INLINE Iterator& operator++() { index++; return *this; }
        GEODE_INLINE Iterator& operator++(int) { auto& t = *this; index++; return t; }
    };

    Iterator begin() const { return { min }; }
    Iterator end() const { return { max }; }
};

struct IRect {
    glm::ivec2 min = { INT32_MAX, INT32_MAX };
    glm::ivec2 max = { INT32_MIN, INT32_MIN };

    GEODE_INLINE IRect() {}

    GEODE_INLINE IRect(const glm::ivec2& min, const glm::ivec2& max)
        : min(min), max(max) {}

    GEODE_INLINE IRect(const IRange& rangeX, const IRange& rangeY)
        : min(rangeX.min, rangeY.min), max(rangeX.max, rangeY.max) {}

    GEODE_INLINE IRange rangeX() const { return { min.x, max.x }; }
    GEODE_INLINE IRange rangeY() const { return { min.y, max.y }; }

    GEODE_INLINE void include(const glm::ivec2& p) {
        if (p.x < min.x) min.x = p.x;
        if (p.x > max.x) max.x = p.x;
        if (p.y < min.y) min.y = p.y;
        if (p.y > max.y) max.y = p.y;
    }

    GEODE_INLINE bool intersects(const IRect& o) const {
        return max.x > o.min.x && o.max.x > min.x &&
               max.y > o.min.y && o.max.y > min.y;
    }
};