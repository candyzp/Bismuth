#include "HotKey.hpp"

HotKey* HotKey::hotKeysHead = nullptr;
HotKey* HotKey::hotKeysTail = nullptr;

#include <Geode/modify/CCKeyboardDispatcher.hpp>
class $modify(RendererCCKeyboardDispatcher, cocos2d::CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool keyDown, bool isKeyRepeat) {
        if (keyDown && !isKeyRepeat) {
            HotKey* hotKey = HotKey::hotKeysHead;
            while (hotKey) {
                if (hotKey->key == key)
                    hotKey->pressed();
                hotKey = hotKey->next;
            }
        }

        return cocos2d::CCKeyboardDispatcher::dispatchKeyboardMSG(key, keyDown, isKeyRepeat);
    }
};