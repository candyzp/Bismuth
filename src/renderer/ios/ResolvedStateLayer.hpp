#pragma once

#ifdef GEODE_IS_IOS

#include "DataTexture.hpp"
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/cocos/sprite_nodes/CCSprite.h>
#include <vector>

class ResolvedStateLayer {
public:
    enum class SafetyClass : u8 {
        StockOnly,
        StaticSafe,
        DynamicSafe,
    };

    struct Stats {
        usize renderableObjects = 0;
        usize stockObjects = 0;
        usize safeObjects = 0;
        usize staticObjects = 0;
        usize dynamicObjects = 0;
        usize safeSprites = 0;

        usize dirtyTransforms = 0;
        usize dirtyAppearance = 0;
        usize dirtyVisibility = 0;
        usize dirtyUVs = 0;
        usize staticObjectsReused = 0;

        usize bytesUploaded = 0;
        usize uploadCalls = 0;
    };

    ResolvedStateLayer() = default;
    ~ResolvedStateLayer();

    bool init(PlayLayer* layer);
    void resync();

    // Detailed probing intentionally runs only for the debug overlay until the
    // new GPU draw path starts consuming these textures. Normal hybrid play pays
    // no per-frame state-walk cost from this layer yet.
    void update(bool detailedProbe);

    inline const Stats& getStats() const { return stats; }
    inline bool isGPUStateReady() const {
        return objectStateTexture != nullptr && spriteStateTexture != nullptr;
    }

    inline DataTexture* getObjectStateTexture() const { return objectStateTexture; }
    inline DataTexture* getSpriteStateTexture() const { return spriteStateTexture; }

private:
    struct ObjectState {
        glm::vec2 position = {0.f, 0.f};
        float rotation = 0.f;
        float scaleX = 1.f;
        float scaleY = 1.f;
        float opacity = 1.f;
        bool visible = true;
    };

    struct SpriteState {
        cocos2d::ccColor3B color = {255, 255, 255};
        u8 opacity = 255;
        cocos2d::CCRect textureRect = {};
        u32 textureId = 0;
        float textureWidth = 1.f;
        float textureHeight = 1.f;
        bool visible = true;
        bool rotated = false;
        bool flipX = false;
        bool flipY = false;
    };

    struct ObjectRecord {
        GameObject* object = nullptr;
        SafetyClass safety = SafetyClass::StockOnly;
        ObjectState state;
        usize firstSprite = 0;
        usize spriteCount = 0;
    };

    struct SpriteRecord {
        cocos2d::CCSprite* sprite = nullptr;
        usize objectIndex = 0;
        SpriteState state;
    };

    SafetyClass classifyObject(GameObject* object, std::vector<cocos2d::CCSprite*>& sprites) const;
    ObjectState captureObjectState(GameObject* object) const;
    SpriteState captureSpriteState(cocos2d::CCSprite* sprite) const;

    void packObjectState(usize index, const ObjectState& state, SafetyClass safety);
    void packSpriteState(usize index, const SpriteState& state, usize objectIndex);

    static bool transformChanged(const ObjectState& a, const ObjectState& b);
    static bool objectAppearanceChanged(const ObjectState& a, const ObjectState& b);
    static bool spriteAppearanceChanged(const SpriteState& a, const SpriteState& b);
    static bool spriteUVChanged(const SpriteState& a, const SpriteState& b);

    void destroyTextures();

private:
    PlayLayer* layer = nullptr;

    std::vector<ObjectRecord> objects;
    std::vector<SpriteRecord> sprites;
    std::vector<glm::vec4> objectTexels;
    std::vector<glm::vec4> spriteTexels;

    DataTexture* objectStateTexture = nullptr;
    DataTexture* spriteStateTexture = nullptr;

    Stats stats;
};

#endif
