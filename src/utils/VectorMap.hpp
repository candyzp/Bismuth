#pragma once

#include "IRect.hpp"
#include <common.hpp>

template <typename T>
class VectorMap {
public:
    inline void expectKey(i32 key) {
        range.include(key);
    }

    inline void allocate() {
        BISMUTH_ASSERT(vector.size() == 0);
        vector.resize(range.length());
    }

    inline T& at(i32 key) {
        BISMUTH_ASSERT(range.contains(key));
        return vector[key - range.min];
    }
    inline const T& at(i32 key) const {
        BISMUTH_ASSERT(range.contains(key));
        return vector[key - range.min];
    }

    inline T& operator[](i32 key) { return at(key); }
    inline const T& operator[](i32 key) const { return at(key); }

    inline const IRange& getRange() const { return range; }

private:
    IRange range;
    std::vector<T> vector;
};