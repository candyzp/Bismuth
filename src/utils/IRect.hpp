#pragma once

#include <common.hpp>

struct IRange {
    i32 min = INT32_MAX;
    i32 max = INT32_MIN;

    inline i32 length() const { return max - min; }

    inline bool isEmpty() const { return min >= max; }

    inline void include(i32 v) {
        if (v >= max) max = v + 1;
        if (v <  min) min = v;
    }

    inline bool contains(i32 v) const { return v >= min && v < max; }

    inline IRange intersection(const IRange& range) const {
        return {
            (min > range.min) ? min : range.min,
            (max < range.max) ? max : range.max,
        };
    }

    struct Iterator {
        i32 index;

        inline i32 operator*() const { return index; }

        inline bool operator==(const Iterator& o) const { return index == o.index; }
        inline bool operator!=(const Iterator& o) const { return index != o.index; }

        inline Iterator& operator++() { index++; return *this; }
        inline Iterator& operator++(int) { auto& t = *this; index++; return t; }
    };

    Iterator begin() const { return { min }; }
    Iterator end() const { return { max }; }
};

struct IRect {
    glm::ivec2 min = { INT32_MAX, INT32_MAX };
    glm::ivec2 max = { INT32_MIN, INT32_MIN };

    inline IRect() {}

    inline IRect(const glm::ivec2& min, const glm::ivec2& max)
        : min(min), max(max) {}

    inline IRect(const IRange& rangeX, const IRange& rangeY)
        : min(rangeX.min, rangeY.min), max(rangeX.max, rangeY.max) {}

    inline IRange rangeX() const { return { min.x, max.x }; }
    inline IRange rangeY() const { return { min.y, max.y }; }

    inline void include(const glm::ivec2& p) {
        if (p.x < min.x) min.x = p.x;
        if (p.x > max.x) max.x = p.x;
        if (p.y < min.y) min.y = p.y;
        if (p.y > max.y) max.y = p.y;
    }

    inline bool intersects(const IRect& o) const {
        return max.x > o.min.x && o.max.x > min.x &&
               max.y > o.min.y && o.max.y > min.y;
    }
};