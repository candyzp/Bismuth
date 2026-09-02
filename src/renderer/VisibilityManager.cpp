#include "VisibilityManager.hpp"
#include "Geode/cocos/sprite_nodes/CCSpriteBatchNode.h"
#include "GroupManager.hpp"
#include "ObjectUtils.hpp"
#include "Renderer.hpp"
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
        .destinationLayerIfGlow        = getSpriteLayer(LayerKey::getFromGlowSpriteObject(gameObject)),
        .destinationLayerIfBlending    = getSpriteLayer(LayerKey::getFromObject(gameObject, true)),
        .destinationLayerIfNotBlending = getSpriteLayer(LayerKey::getFromObject(gameObject, false))
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

void VisibilityManager::generateFastStructures() {
    for (auto& set : objectSectionSetPerTransformGroupId)
        set.generateFastStructure();
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

// volatile float minX;
// volatile float minY;
// volatile float maxX;
// volatile float maxY;

void VisibilityManager::calculateVisibilitiesForCameraView(const CameraView& view) {
    clearObjectVisibilities();

    GroupManager& groupManager = Renderer::get()->getGroupManager();
    auto groupStates = groupManager.getGroupStates();

    glm::vec2 cameraNormalMin = glm::vec2 {
        minOf4(0, view.rightVector.x, view.upVector.x, view.rightVector.x + view.upVector.x),
        minOf4(0, view.rightVector.y, view.upVector.y, view.rightVector.y + view.upVector.y)
    } + view.bottomLeft;
    glm::vec2 cameraNormalMax = glm::vec2 {
        maxOf4(0, view.rightVector.x, view.upVector.x, view.rightVector.x + view.upVector.x),
        maxOf4(0, view.rightVector.y, view.upVector.y, view.rightVector.y + view.upVector.y)
    } + view.bottomLeft;
    
    i32 transformId = 0;
    for (auto& sectionSet : objectSectionSetPerTransformGroupId) {
        i32 groupIndex = groupManager.getFirstGroupIndexOfTransformIndex(transformId);

        GroupCombinationState& state = groupStates[groupIndex];

        glm::vec2 min, max;
        
        if (groupManager.hasGroupStateScaled(groupIndex)) {
            /*
                If you multiply a matrix with a vector, if the matrix is on the left-hand side,
                it does a transform. If it is on the right-hand side, it does an inverted transform.
            */

            glm::vec2 camBottomLeft    = (view.bottomLeft - state.offset) * state.positionalTransform;
            glm::vec2 camRightVector   = view.rightVector * state.positionalTransform;
            glm::vec2 camUpVector      = view.upVector    * state.positionalTransform;
            glm::vec2 camRightUpVector = camRightVector + camUpVector;

            min = glm::vec2 {
                minOf4(0, camRightVector.x, camUpVector.x, camRightUpVector.x),
                minOf4(0, camRightVector.y, camUpVector.y, camRightUpVector.y)
            } + camBottomLeft;
            max = glm::vec2 {
                maxOf4(0, camRightVector.x, camUpVector.x, camRightUpVector.x),
                maxOf4(0, camRightVector.y, camUpVector.y, camRightUpVector.y)
            } + camBottomLeft;
        } else {
            min = cameraNormalMin - state.offset;
            max = cameraNormalMax - state.offset;
        }

#ifdef GEODE_IS_IOS
        // Keep CPU culling coarse on iOS and let the GPU render a small edge band.
        // This slightly shifts rendering work toward the GPU and makes fast-moving
        // or transformed decoration less likely to pop out at the camera boundary.
        constexpr float IOS_GPU_ASSIST_MARGIN = 18.0f;
        min -= glm::vec2(IOS_GPU_ASSIST_MARGIN);
        max += glm::vec2(IOS_GPU_ASSIST_MARGIN);
#endif

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

//        minX = min.x;
//        minY = min.y;
//        maxX = max.x;
//        maxY = max.y;

        Rect rect = { min, max };

        sectionSet.forEachSectionInRect(rect, [&](const auto& section) {
            for (Object* object : section) {
                markObjectAsVisible(object);
            }
        });

        transformId++;
    }
}

void VisibilityManager::markAllObjectsVisible() {
    clearObjectVisibilities();

    for (auto object : allObjects)
        markObjectAsVisible(object);
}

VisibilityManager::Layer VisibilityManager::getLayer(const LayerKey& id) {
    for (auto layer : visibleSpriteLayers)
        if (layer->id == id)
            return layer;
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

std::vector<LayerKey> VisibilityManager::getUsedLayerIds() {
    std::vector<LayerKey> ids;
    for (auto layer : visibleSpriteLayers)
        ids.push_back(layer->id);
    return ids;
}

void VisibilityManager::clearObjectVisibilities() {
    for (auto layer : visibleSpriteLayers) {
        layer->clear();
    }
}

static bool isLiveSpriteVisible(GameObject* object, cocos2d::CCSprite* sprite) {
    if (!object || !sprite || !sprite->isVisible() || sprite->getDisplayedOpacity() == 0)
        return false;

    // Animated objects can hide a parent sprite-part while its child keeps its
    // own local visible flag. Only walk the hierarchy when it actually reaches
    // this GameObject; detached color/glow sprites may live under GD's batch
    // nodes, which Bismuth intentionally hides.
    bool belongsToObjectTree = false;
    for (auto node = static_cast<cocos2d::CCNode*>(sprite); node; node = node->getParent()) {
        if (node == object) {
            belongsToObjectTree = true;
            break;
        }
    }

    if (!belongsToObjectTree)
        return true;

    for (auto node = static_cast<cocos2d::CCNode*>(sprite); node; node = node->getParent()) {
        if (!node->isVisible())
            return false;
        if (node == object)
            return object->getDisplayedOpacity() != 0;
    }

    return true;
}

void VisibilityManager::markObjectAsVisible(Object* object) {
    if (!object || !object->gameObject || object->gameObject->m_isInvisible)
        return;

    for (auto& sprite : object->sprites) {
        // Runtime animations, coins and platformer checkpoints can toggle
        // individual sprite parts after Bismuth has baked the level VBO.
        if (!isLiveSpriteVisible(object->gameObject, sprite.sprite))
            continue;

        i32 zorder = object->gameObject->getObjectZOrder();

        i32 colorChannel = ObjectUtils::getSpriteColorChannel(sprite.type, object->gameObject, sprite.sprite);

        if (sprite.type == SpriteType::GLOW) {
            if (object->destinationLayerIfGlow)
                object->destinationLayerIfGlow->push(&sprite, zorder);
        } else if (Renderer::get()->isColorChannelBlending(colorChannel)) {
            if (object->destinationLayerIfBlending)
                object->destinationLayerIfBlending->push(&sprite, zorder);
        } else {
            if (object->destinationLayerIfNotBlending)
                object->destinationLayerIfNotBlending->push(&sprite, zorder);
        }
    }
}

VisibilityManager::VisibleSpriteLayer* VisibilityManager::getSpriteLayer(LayerKey layerId) {
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

    SectionSet* sectionSet = nullptr;

    while (transformId >= objectSectionSetPerTransformGroupId.size())
        objectSectionSetPerTransformGroupId.push_back(SectionSet(returnObjectStartPosition));

    objectSectionSetPerTransformGroupId[transformId].add(object);
}
