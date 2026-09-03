#pragma once

#include "Geode/cocos/sprite_nodes/CCSprite.h"
#include "ObjectUtils.hpp"
#include <utils/SectionSet.hpp>
#include <common.hpp>
#include <unordered_map>

struct CameraView {
    // Bottom-left position of the camera
    glm::vec2 bottomLeft;
    // Vector from the bottom-left corner to the bottom-right corner
    glm::vec2 rightVector;
    // Vector from the bottom-left corner to the top-left corner
    glm::vec2 upVector;
};


/*
    This manages the visibility of objects in
    a very efficient way.
    In other words, it manages which objects
    are on screen and which are invisible.
*/
class VisibilityManager {
private:
    struct ObjectSprite {
        ObjectSprite* next;
        ObjectSprite* prev;
        cocos2d::CCSprite* sprite;
        SpriteType type;

        // This is important for ObjectBatch
        usize indiciesBegin;
        usize indiciesEnd;
    };

    struct VisibleSpriteLayer;

    struct Object {
        GameObject* gameObject;
        std::vector<ObjectSprite> sprites;

        bool isAnimated;
        bool animatedVisibleThisFrame = false;
        bool runtimeVisualTracked = false;
        u32 lastVisibleGeneration = 0;
        u32 groupCombinationIndex = 0;

        VisibleSpriteLayer* destinationLayerIfGlow;
        VisibleSpriteLayer* destinationLayerIfBlending;
        VisibleSpriteLayer* destinationLayerIfNotBlending;
    };

    /*
        Linked list containing visible sprites.
    */
    struct VisibleSpriteList {
        ObjectSprite* head = nullptr;

        inline void push(ObjectSprite* sprite) {
            if (head == nullptr) {
                sprite->next = sprite;
                sprite->prev = sprite;
                head = sprite;
            } else {
                sprite->prev = head->prev;
                sprite->next = head;
                head->prev->next = sprite;
                head->prev = sprite;
            }
        }

        inline void clear() {
            usize dcount = 0;
            if (head) {
                auto sprite = head;
                do {
                    dcount++;
                    sprite = sprite->next;
                } while (sprite != head);
            }
            head = nullptr;
        }
    };

    struct VisibleSpriteLayer {
        LayerKey id;
        i32 zorder;

        std::map<i32, VisibleSpriteList> visibleSpritesPerZOrder;

        inline void push(ObjectSprite* sprite, i32 zorder) {
            auto it = visibleSpritesPerZOrder.find(zorder);
            if (it == visibleSpritesPerZOrder.end()) {
                visibleSpritesPerZOrder[zorder] = {};
                visibleSpritesPerZOrder[zorder].push(sprite);
            } else {
                it->second.push(sprite);
            }
        }

        inline void clear() {
            for (auto& [zorder, list] : visibleSpritesPerZOrder) {
                list.clear();
            }
        }
    };

public:
    ~VisibilityManager();

    void prepareForObject(GameObject* object);

    void addObjectSprite(
        cocos2d::CCSprite* sprite,
        SpriteType type,
        usize indiciesBegin,
        usize indiciesEnd
    );

    void generateFastStructures();

    void calculateVisibilitiesForCameraView(const CameraView& view);

    void markAllObjectsVisible();

    void trackRuntimeVisualObject(GameObject* object);

    using Layer = void*;

    Layer getLayer(const LayerKey& id);

    void forEachVisibleSpriteIndexRangeInLayer(Layer layer, std::function<void(usize, usize)> func);

    std::vector<LayerKey> getUsedLayerIds();

private:
    void clearObjectVisibilities();

    void finishAnimatedObjectVisibilities();

    void updateVisibleAnimatedObject(Object* object);

    void markRuntimeVisualObjects(const glm::vec2& cameraMin, const glm::vec2& cameraMax);

    static bool isAnimatedSpriteVisible(Object* object, cocos2d::CCSprite* sprite);

    void markObjectAsVisible(Object* object);

    VisibleSpriteLayer* getSpriteLayer(LayerKey labelayerId);

    static glm::vec2 returnObjectStartPosition(Object* object);

    void addObjectToSectionStructure(Object* object);

private:
    std::vector<Object*> allObjects;
    std::unordered_map<GameObject*, Object*> objectLookup;
    std::vector<VisibleSpriteLayer*> visibleSpriteLayers;
    std::vector<Object*> visibleAnimatedObjects;
    std::vector<Object*> nextVisibleAnimatedObjects;
    std::vector<Object*> runtimeVisualObjects;
    u32 visibilityGeneration = 0;

    using SectionSet = SectionSet<Object*>;

    /*
        This is the most important structure for finding which
        objects are on screen.
        Objects here are put into equal-sized sections based
        on position like how Geometry Dash does it.
        
        However, objects can move. The way we fix this is
        by further organising these sections based on
        which transforming group id the objects belong to.

        Objects with the same transform group (see GroupManager.hpp)
        always transform the same way. And so the idea is to
        have the sections the objects belong to also
        transform with the objects so that we don't have to
        move the objects from section to section.
    */
    std::vector<SectionSet> objectSectionSetPerTransformGroupId;

    Object* currentObject = nullptr;
};
