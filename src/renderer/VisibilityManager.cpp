#include "VisibilityManager.hpp"
#include "Geode/cocos/sprite_nodes/CCSpriteBatchNode.h"
#include "GroupManager.hpp"
#include "ObjectUtils.hpp"
#include "Renderer.hpp"
#include "SectionSet.hpp"
#include "common.hpp"
#include "glm/common.hpp"
#include <Geode/binding/GameObject.hpp>

using namespace geode::prelude;

VisibilityManager::~VisibilityManager() {
    for (auto obj : allObjects)
        delete obj;
    allObjects.clear();
    for (auto layer : visibleSpriteLayers)
        delete layer;
    visibleSpriteLayers.clear();
}

void VisibilityManager::prepareForObject(GameObject* gameObject) {
    // log::info("NEW GAMEOBJECT {}", (void*)gameObject);
    allObjects.push_back(new Object {
        .gameObject = gameObject,
        .destinationLayerIfGlow        = getSpriteLayer(LayerIdentifier::getFromGlowSpriteObject(gameObject)),
        .destinationLayerIfBlending    = getSpriteLayer(LayerIdentifier::getFromObject(gameObject, true)),
        .destinationLayerIfNotBlending = getSpriteLayer(LayerIdentifier::getFromObject(gameObject, false))
    });
    currentObject = allObjects.back();
    addObjectToSectionStructure(currentObject);
}

void VisibilityManager::addObjectSprite(
    cocos2d::CCSprite* sprite,
    SpriteType type,
    usize indiciesBegin,
    usize indiciesEnd
) {
    if (currentObject == nullptr)
        return;

    currentObject->sprites.push_back({
        .next = nullptr,
        .prev = nullptr,
        .sprite = sprite,
        .type = type,
        .indiciesBegin = indiciesBegin,
        .indiciesEnd = indiciesEnd,
    });
}

inline static float minOf4(float a, float b, float c, float d) {
    if (b < a) a = b;
    if (c < a) a = c;
    if (d < a) a = d;
    return a;
}

inline static float maxOf4(float a, float b, float c, float d) {
    if (b > a) a = b;
    if (c > a) a = c;
    if (d > a) a = d;
    return a;
}

usize darg = 0;
static usize dargz = 0;
static usize dargu = 0;

void VisibilityManager::calculateVisibilitiesForCameraView(const CameraView& view) {
    clearObjectVisibilities();

    GroupManager& groupManager = Renderer::get()->getGroupManager();
    
    for (auto& [transformId, sectionSet] : objectSectionSetPerTransformGroupId) {
        // log::info("TRANSFORM ID {}", transformId);


        GroupCombinationState& state = groupManager.getGroupStates()[groupManager.getFirstGroupIndexOfTransformIndex(transformId)];

        glm::mat2 worldToGroupTransform = state.positionalTransform;

        /*
            If you multiply a matrix with a vector, if the matrix is on the left-hand side,
            it does a transform. If it is on the right-hand side, it does an inverted transform.
        */

        glm::vec2 camBottomLeft  = (view.bottomLeft - state.offset) * state.positionalTransform;
        glm::vec2 camRightVector = view.rightVector * state.positionalTransform;
        glm::vec2 camUpVector    = view.upVector    * state.positionalTransform;
        // glm::vec2 camTopRight    = (view.bottomLeft + view.rightVector + view.upVector - state.offset) * worldToGroupTransform;

        float minX = minOf4(0, camRightVector.x, camUpVector.x, camRightVector.x + camUpVector.x) + camBottomLeft.x;
        float maxX = maxOf4(0, camRightVector.x, camUpVector.x, camRightVector.x + camUpVector.x) + camBottomLeft.x;
        float minY = minOf4(0, camRightVector.y, camUpVector.y, camRightVector.y + camUpVector.y) + camBottomLeft.y;
        float maxY = maxOf4(0, camRightVector.y, camUpVector.y, camRightVector.y + camUpVector.y) + camBottomLeft.y;

        // auto ssize = glm::vec2 DEFAULT_SECTION_SIZE;
        
        // glm::vec2 minc = glm::floor(glm::vec2 { minX, minY } / ssize + glm::vec2(0, 0)) * ssize;
        // glm::vec2 maxc = glm::floor(glm::vec2 { maxX, maxY } / ssize + glm::vec2(1, 1)) * ssize;

        // glm::vec2 bl = state.positionalTransform * glm::vec2(minc.x, minc.y) + state.offset;
        // glm::vec2 br = state.positionalTransform * glm::vec2(maxc.x, minc.y) + state.offset;
        // glm::vec2 tl = state.positionalTransform * glm::vec2(minc.x, maxc.y) + state.offset;
        // glm::vec2 tr = state.positionalTransform * glm::vec2(maxc.x, maxc.y) + state.offset;

        if (transformId == 0) {
            Renderer* ren = Renderer::get();
            // ren->drawLine(bl, br, glm::vec4(1, 0, 1, 1));
            // ren->drawLine(br, tr, glm::vec4(1, 0, 1, 1));
            // ren->drawLine(tr, tl, glm::vec4(1, 0, 1, 1));
            // ren->drawLine(tl, bl, glm::vec4(1, 0, 1, 1));
        }

        Rect rect = { { minX, minY }, { maxX, maxY } };

        sectionSet.forEachSectionInRect(rect, [&](const auto& section) {
            // log::info("KHOWWEDD!!");
            for (Object* object : section) {
                // log::info("DARN IT! {}", (void*)object);
                markObjectAsVisible(object);
            }
        });
    }
}

void VisibilityManager::markAllObjectsVisible() {
    clearObjectVisibilities();

    for (auto object : allObjects)
        markObjectAsVisible(object);
}

VisibilityManager::Layer VisibilityManager::getLayer(const LayerIdentifier& id) {
    for (auto layer : visibleSpriteLayers) {
        if (layer->id == id)
            return layer;
    }
    return nullptr;
}

void VisibilityManager::forEachVisibleSpriteIndexRangeInLayer(Layer layerRaw, std::function<void(usize, usize)> func) {
    auto layer = (VisibleSpriteLayer*)layerRaw;
    if (layer == nullptr)
        return;

    for (auto& [zorder, list] : layer->visibleSpritesPerZOrder) {
        if (list.head == nullptr)
            continue;

        ObjectSprite* sprite = list.head;
        do {
            func(sprite->indiciesBegin, sprite->indiciesEnd);
            sprite = sprite->next;
        } while (sprite != list.head);
    }
}

std::vector<LayerIdentifier> VisibilityManager::getUsedLayerIds() {
    std::vector<LayerIdentifier> ids;
    for (auto layer : visibleSpriteLayers)
        ids.push_back(layer->id);
    return ids;
}

void VisibilityManager::clearObjectVisibilities() {
    for (auto layer : visibleSpriteLayers) {
        layer->clear();
    }
}

void VisibilityManager::markObjectAsVisible(Object* object) {
    // log::info("MARK OBJECT VISIBLE {}", (void*)object);

    // bool present = false;
    // for (auto obj : allObjects) {
    //     if (obj == object) {
    //         present = true;
    //         break;
    //     }
    // }
    // if (!present) {
    //     log::info("NOT PRESENT!!!!!");
    //     return;
    // }

    for (auto& sprite : object->sprites) {
        // log::info("SPRITZE!");
        // log::info("GAME OBJECT {}", (void*)object->gameObject);

        i32 zorder = object->gameObject->getObjectZOrder();

        i32 colorChannel = ObjectUtils::getSpriteColorChannel(sprite.type, object->gameObject, sprite.sprite);

        if (sprite.type == SpriteType::GLOW) {
            // log::info("MARK GLOW");
            if (object->destinationLayerIfGlow)
                object->destinationLayerIfGlow->push(&sprite, zorder);
        } else if (Renderer::get()->isColorChannelBlending(colorChannel)) {
            // log::info("MARK BLENDING");
            if (object->destinationLayerIfBlending)
                object->destinationLayerIfBlending->push(&sprite, zorder);
        } else {
            // log::info("MARK NOT BLENDING {}", (void*)object->destinationLayerIfNotBlending);
            if (object->destinationLayerIfNotBlending)
                object->destinationLayerIfNotBlending->push(&sprite, zorder);
        }
    }
}

VisibilityManager::VisibleSpriteLayer* VisibilityManager::getSpriteLayer(LayerIdentifier layerId) {
    for (auto layer : visibleSpriteLayers) {
        if (layer->id == layerId)
            return layer;
    }

    CCSpriteBatchNode* batchNode = Renderer::get()->getSpriteBatchNodeWithLayerId(layerId);
    if (!batchNode)
        return nullptr;

    i32 nodeZOrder = batchNode->getZOrder();

    i32 dstIndex = 0;
    for (; dstIndex < visibleSpriteLayers.size(); dstIndex++) {
        if (visibleSpriteLayers[dstIndex]->zorder > nodeZOrder)
            break;
    }

    auto newLayer = new VisibleSpriteLayer { layerId, nodeZOrder };

    visibleSpriteLayers.insert(visibleSpriteLayers.begin() + dstIndex, newLayer);
    return visibleSpriteLayers[dstIndex];
}

glm::vec2 VisibilityManager::returnObjectStartPosition(Object* object) {
    return ccPointToGLM(object->gameObject->m_startPosition);
}

void VisibilityManager::addObjectToSectionStructure(Object* object) {
    i32 transformId = Renderer::get()->getGroupManager().getTransformCombinationIndexForObject(object->gameObject);

    // log::info("OBJ TID {}", transformId);

    SectionSet* sectionSet = nullptr;

    auto it = objectSectionSetPerTransformGroupId.find(transformId);
    if (it == objectSectionSetPerTransformGroupId.end()) {
        objectSectionSetPerTransformGroupId[transformId] = SectionSet(returnObjectStartPosition);
        // log::info("CREATE FOR {}", transformId);
        sectionSet = &objectSectionSetPerTransformGroupId[transformId];
    } else {
        sectionSet = &it->second;
    }

    assert(sectionSet != nullptr);

    // log::info("ADD!");
    sectionSet->add(object);
}