#pragma once

#include "IRect.hpp"
#include <common.hpp>

template <typename T>
class VectorMap {
public:
    GEODE_INLINE void expectKey(i32 key) {
        range.include(key);
    }

    GEODE_INLINE void allocate() {
        KYANITE_ASSERT(vector.size() == 0);
        vector.resize(range.length());
    }

    GEODE_INLINE T& at(i32 key) {
        KYANITE_ASSERT(range.contains(key));
        return vector[key - range.min];
    }
    GEODE_INLINE const T& at(i32 key) const {
        KYANITE_ASSERT(range.contains(key));
        return vector[key - range.min];
    }

    GEODE_INLINE T& operator[](i32 key) { return at(key); }
    GEODE_INLINE const T& operator[](i32 key) const { return at(key); }

    GEODE_INLINE const IRange& getRange() const { return range; }

private:
    IRange range;
    std::vector<T> vector;
};