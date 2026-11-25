#pragma once

#include "common.hpp"

#define DECLARE_HOTKEY(K, F) static HotKey GEODE_CONCAT(__HOTKEY_, __LINE__)(K, []() F);

struct HotKey {
    cocos2d::enumKeyCodes key;
    std::function<void()> pressed;
    HotKey* next = nullptr;

    static HotKey* hotKeysHead;
    static HotKey* hotKeysTail;

    inline HotKey(cocos2d::enumKeyCodes key, std::function<void()> pressed)
        : key(key), pressed(pressed)
    {
        if (hotKeysTail == nullptr)
            hotKeysHead = this;
        else
            hotKeysTail->next = this;
        hotKeysTail = this;
    }
};