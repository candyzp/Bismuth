#pragma once

#ifdef GEODE_IS_IOS

#include "DataTexture.hpp"
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/cocos/sprite_nodes/CCSprite.h>
#include <unordered_map>
#include <unordered_set>
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

        // Collection/lifetime safety diagnostics. Unsupported visual trees fail
        // closed before any GPU record is committed.
        usize unsafeCollectionObjects = 0;
        usize invalidChildNodes = 0;
        usize duplicateSpriteRecords = 0;
        usize invalidSpriteRecords = 0;
        usize retainedInitObjects = 0;
        usize retainedInitSprites = 0;
        usize initRevalidationFailures = 0;

        // These counters now mean stock-active GPU records, not merely owned
        // records. GD's activate/deactivate lifecycle is the authority.
        usize activeGPUObjects = 0;
        usize activeGPUSprites = 0;
        usize activeStaticObjects = 0;

        usize dirtyTransforms = 0;
        usize dirtyAppearance = 0;
        usize dirtyVisibility = 0;
        usize dirtyUVs = 0;
        usize staticObjectsReused = 0;

        usize bytesUploaded = 0;
        usize uploadCalls = 0;
    };

    struct ShadowCandidate {
        GameObject* object = nullptr;
        cocos2d::CCSprite* sprite = nullptr;
        usize objectStateIndex = 0;
        usize spriteStateIndex = 0;
    };

    ResolvedStateLayer();
    ~ResolvedStateLayer();

    bool init(PlayLayer* layer);
    void resync();
    void update(bool detailedProbe);

    // Called once after renderer ownership is resolved. RendererIOS still builds
    // the complete ownership list; reseedActiveFromStock() then compiles the much
    // smaller set GD actually has active in the scene.
    void setGPUOwnedSprites(const std::unordered_set<cocos2d::CCSprite*>& ownedSprites);

    // iOS visibility observer. These NEVER decide visibility. Stock GD performs
    // activateObject/deactivateObject first, then these methods mirror the final
    // lifecycle result into the GPU polling set.
    static ResolvedStateLayer* getCurrent();
    void reseedActiveFromStock();
    void onObjectActivated(GameObject* object);
    void onObjectDeactivated(GameObject* object);
    void finishEventFrame();

    inline const Stats& getStats() const { return stats; }
    inline const std::vector<ShadowCandidate>& getShadowCandidates() const { return shadowCandidates; }

    inline std::vector<ShadowCandidate> getStaticShadowCandidates() const {
        std::vector<ShadowCandidate> candidates;
        candidates.reserve(stats.safeSprites);

        for (usize objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
            const auto& objectRecord = objects[objectIndex];
            if (!objectRecord.object || objectRecord.safety != SafetyClass::StaticSafe)
                continue;

            for (usize localSprite = 0; localSprite < objectRecord.spriteCount; ++localSprite) {
                const usize spriteIndex = objectRecord.firstSprite + localSprite;
                if (spriteIndex >= sprites.size())
                    break;

                const auto& spriteRecord = sprites[spriteIndex];
                if (!spriteRecord.sprite || !spriteRecord.sprite->getTexture())
                    continue;

                candidates.push_back({
                    objectRecord.object,
                    spriteRecord.sprite,
                    objectIndex,
                    spriteIndex
                });
            }
        }

        return candidates;
    }

    // This is the real iOS assist ownership list. Both StaticSafe and
    // DynamicSafe objects are allowed because Geometry Dash still owns their
    // gameplay, triggers, animation lifecycle, color resolution, visibility,
    // and final root state. Bismuth only consumes that already-resolved state to
    // replace repetitive Cocos quad transform work on the GPU.
    inline std::vector<ShadowCandidate> getGPUCandidates() const {
        std::vector<ShadowCandidate> candidates;
        candidates.reserve(stats.safeSprites);

        for (usize objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
            const auto& objectRecord = objects[objectIndex];
            if (!objectRecord.object || objectRecord.safety == SafetyClass::StockOnly)
                continue;

            for (usize localSprite = 0; localSprite < objectRecord.spriteCount; ++localSprite) {
                const usize spriteIndex = objectRecord.firstSprite + localSprite;
                if (spriteIndex >= sprites.size())
                    break;

                const auto& spriteRecord = sprites[spriteIndex];
                if (!spriteRecord.sprite || !spriteRecord.sprite->getTexture())
                    continue;

                candidates.push_back({
                    objectRecord.object,
                    spriteRecord.sprite,
                    objectIndex,
                    spriteIndex
                });
            }
        }

        return candidates;
    }

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

    struct CollectionDiagnostics {
        usize invalidChildNodes = 0;
        usize duplicateSprites = 0;
        usize invalidSprites = 0;
        bool unsafeCollection = false;
    };

    SafetyClass classifyObject(
        GameObject* object,
        std::vector<cocos2d::CCSprite*>& sprites,
        CollectionDiagnostics& diagnostics
    ) const;
    bool isShadowValidationCandidate(
        GameObject* object,
        SafetyClass safety,
        const std::vector<cocos2d::CCSprite*>& sprites
    ) const;
    ObjectState captureObjectState(GameObject* object) const;
    SpriteState captureSpriteState(cocos2d::CCSprite* sprite) const;

    void packObjectState(usize index, const ObjectState& state, SafetyClass safety);
    void packSpriteState(usize index, const SpriteState& state, usize objectIndex);

    static bool transformChanged(const ObjectState& a, const ObjectState& b);
    static bool objectAppearanceChanged(const ObjectState& a, const ObjectState& b);
    static bool spriteAppearanceChanged(const SpriteState& a, const SpriteState& b);
    static bool spriteUVChanged(const SpriteState& a, const SpriteState& b);

    void destroyTextures();

    // Event-driven active-set helpers. The ownership masks are compiled once
    // from setGPUOwnedSprites()'s initial complete lists, then the hot vectors are
    // kept sorted and screen-scale by stock activation events.
    void ensureEventOwnership();
    void addActiveObjectIndex(usize objectIndex);
    void removeActiveObjectIndex(usize objectIndex);

private:
    PlayLayer* layer = nullptr;

    std::vector<ObjectRecord> objects;
    std::vector<SpriteRecord> sprites;
    std::vector<ShadowCandidate> shadowCandidates;
    std::vector<glm::vec4> objectTexels;
    std::vector<glm::vec4> spriteTexels;

    std::vector<usize> activeObjectIndices;
    std::vector<usize> activeSpriteIndices;

    std::unordered_map<GameObject*, usize> objectIndexByPointer;
    std::vector<bool> gpuOwnedObjectMask;
    std::vector<bool> gpuOwnedSpriteMask;
    std::vector<bool> activeObjectMask;
    std::vector<bool> activeSpriteMask;
    std::vector<bool> pendingDeactivateMask;
    std::vector<usize> pendingDeactivateIndices;
    bool eventOwnershipReady = false;

    DataTexture* objectStateTexture = nullptr;
    DataTexture* spriteStateTexture = nullptr;

    Stats stats;
};

#endif
