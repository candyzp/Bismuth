#include "VisibilityManager.hpp"
#include "Geode/cocos/sprite_nodes/CCSpriteBatchNode.h"
#include "GroupManager.hpp"
#include "ObjectUtils.hpp"
#include "Renderer.hpp"
#include "common.hpp"
#include "glm/common.hpp"
#include <Geode/binding/AnimatedGameObject.hpp>
#include <Geode/binding/EnhancedGameObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <algorithm>

using namespace geode::prelude;

VisibilityManager::~VisibilityManager() {
    for (auto obj : allObjects)
        delete obj;
    allObjects.clear();
    objectLookup.clear();
    runtimeVisualObjects.clear();
    for (auto layer : visibleSpriteLayers)
        delete layer;
    visibleSpriteLayers.clear();
}

void VisibilityManager::prepareForObject(GameObject* gameObject) {
    // log::info("NEW GAMEOBJECT {}", (void*)gameObject);
    auto object = new Object {
        .gameObject = gameObject,
        .isAnimated = gameObject->m_classType == GameObjectClassType::Animated,
        .groupCombinationIndex = Renderer::get()->getGroupManager().getGroupCombinationIndexForObject(gameObject),
        .destinationLayerIfGlow        = getSpriteLayer(LayerKey::getFromGlowSpriteObject(gameObject)),
        .destinationLayerIfBlending    = getSpriteLayer(LayerKey::getFromObject(gameObject, true)),
        .destinationLayerIfNotBlending = getSpriteLayer(LayerKey::getFromObject(gameObject, false))
    };

    allObjects.push_back(object);
    objectLookup[gameObject] = object;
    currentObject = object;
}

void VisibilityManager::includeCurrentObjectVisualBounds(const glm::vec2& min, const glm::vec2& max) {
    if (!currentObject)
        return;

    if (!currentObject->hasBakedVisualBounds) {
        currentObject->bakedVisualMin = min;
        currentObject->bakedVisualMax = max;
        currentObject->hasBakedVisualBounds = true;
        return;
    }

    currentObject->bakedVisualMin = glm::min(currentObject->bakedVisualMin, min);
    currentObject->bakedVisualMax = glm::max(currentObject->bakedVisualMax, max);
}

void VisibilityManager::finishObject() {
    if (!currentObject)
        return;

    addObjectToSectionStructure(currentObject);
    currentObject = nullptr;
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
        // Keep a modest GPU-side edge band for rotated/scaled corners. Large
        // decoration itself is now indexed by real baked visual bounds instead
        // of relying on this margin to compensate for anchor-only culling.
        constexpr float IOS_GPU_ASSIST_MARGIN = 72.0f;
        min -= glm::vec2(IOS_GPU_ASSIST_MARGIN);
        max += glm::vec2(IOS_GPU_ASSIST_MARGIN);
#endif

        Rect rect = { min, max };

        sectionSet.forEachSectionInRect(rect, [&](const auto& section) {
            for (Object* object : section)
                markObjectAsVisible(object);
        });

        transformId++;
    }

    // 2.2 Area/enter effects can move an individual object independently of
    // its baked section and independently of its normal group transform. Those
    // objects get a second, very small live-position pass so something that was
    // authored off-screen can move into view without being rejected by its old
    // section. The draw itself is still entirely the GPU batch.
    markRuntimeVisualObjects(cameraNormalMin, cameraNormalMax);

    finishAnimatedObjectVisibilities();
}

void VisibilityManager::markAllObjectsVisible() {
    clearObjectVisibilities();

    for (auto object : allObjects)
        markObjectAsVisible(object);

    finishAnimatedObjectVisibilities();
}

void VisibilityManager::trackRuntimeVisualObject(GameObject* gameObject) {
    if (!gameObject)
        return;

    auto it = objectLookup.find(gameObject);
    if (it == objectLookup.end() || !it->second)
        return;

    auto object = it->second;
    if (object->runtimeVisualTracked)
        return;

    object->runtimeVisualTracked = true;
    runtimeVisualObjects.push_back(object);
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
    ++visibilityGeneration;
    if (visibilityGeneration == 0) {
        visibilityGeneration = 1;
        for (auto object : allObjects)
            object->lastVisibleGeneration = 0;
    }

    for (auto layer : visibleSpriteLayers)
        layer->clear();

    // Only the animated objects that were on screen last frame need their
    // visibility marker reset. This keeps the lifecycle work proportional to
    // the camera window rather than the size of the entire level.
    for (auto object : visibleAnimatedObjects)
        object->animatedVisibleThisFrame = false;
    nextVisibleAnimatedObjects.clear();
}

void VisibilityManager::finishAnimatedObjectVisibilities() {
    // GD normally performs this transition from preUpdateVisibility(). Bismuth
    // deliberately skips that full CPU path, so stop only animations that have
    // just left Bismuth's own camera window.
    for (auto object : visibleAnimatedObjects) {
        if (!object->animatedVisibleThisFrame && object->gameObject)
            object->gameObject->deactivateObject(true);
    }

    visibleAnimatedObjects.swap(nextVisibleAnimatedObjects);
    nextVisibleAnimatedObjects.clear();
}

void VisibilityManager::updateVisibleAnimatedObject(Object* object) {
    if (!object->isAnimated || object->animatedVisibleThisFrame)
        return;

    // visibleAnimatedObjects still contains last frame's set until the end of
    // calculateVisibilitiesForCameraView(). That lets us distinguish a true
    // offscreen -> visible transition from an object that is simply continuing
    // to animate, without storing another per-object lifecycle flag.
    const bool wasVisibleLastFrame = std::find(
        visibleAnimatedObjects.begin(), visibleAnimatedObjects.end(), object
    ) != visibleAnimatedObjects.end();

    object->animatedVisibleThisFrame = true;
    nextVisibleAnimatedObjects.push_back(object);

    auto animatedObject = static_cast<AnimatedGameObject*>(object->gameObject);

    // AnimatedGameObject has a dedicated animation setup path. Calling only
    // GameObject::activateObject() is not enough for objects whose Cocos part
    // animation was never started because Bismuth skipped preUpdateVisibility.
    // Start/setup it once when the object enters the GPU camera set, not every
    // frame, so we preserve the animation timeline and keep CPU overhead tiny.
    if (!wasVisibleLastFrame)
        animatedObject->updateObjectAnimation();

    object->gameObject->activateObject();

    auto renderer = Renderer::get();
    auto playLayer = renderer ? renderer->getPlayLayer() : nullptr;

    // Synced animations are driven explicitly from the level clock in GD's
    // visibility loop rather than only by Cocos actions.
    if (playLayer && object->gameObject->getHasSyncedAnimation())
        animatedObject->updateSyncedAnimation(playLayer->m_gameState.m_totalTime, -1);

    // Some animated children use the brightened background color. Preserve the
    // same update that GD applies to active AnimatedGameObjects.
    if (playLayer && playLayer->m_background && object->gameObject->m_unk367) {
        auto brightBGColor = GameToolbox::transformColor(
            playLayer->m_background->getColor(), 0.0, -0.3, 0.4
        );
        animatedObject->updateChildSpriteColor(brightBGColor);
    }
}

void VisibilityManager::markRuntimeVisualObjects(const glm::vec2& cameraMin, const glm::vec2& cameraMax) {
    if (runtimeVisualObjects.empty())
        return;

    auto renderer = Renderer::get();
    if (!renderer)
        return;

    auto& groupManager = renderer->getGroupManager();
    auto groupStates = groupManager.getGroupStates();

    constexpr float RUNTIME_EDGE_PADDING = 96.0f;

    for (auto object : runtimeVisualObjects) {
        if (!object || !object->gameObject || object->gameObject->m_isInvisible)
            continue;
        if (object->groupCombinationIndex >= groupStates.size())
            continue;

        auto gameObject = object->gameObject;
        const auto& state = groupStates[object->groupCombinationIndex];

        // Match object_ios.vert: normal group transform first, then the 2.2
        // per-object world-space Area Move offset.
        glm::vec2 center = state.positionalTransform * ccPointToGLM(gameObject->m_startPosition) + state.offset;
        center += glm::vec2(gameObject->m_positionXOffset, gameObject->m_positionYOffset);

        // The CPU object rectangle is useful for its current visual size even
        // though its center may not contain Bismuth's GPU-only group transform.
        // Add a fixed edge pad for glow, sprite children, and rotated corners.
        auto liveRect = gameObject->getObjectRect();
        const float extentX = std::max(32.0f, liveRect.size.width * 0.5f) + RUNTIME_EDGE_PADDING;
        const float extentY = std::max(32.0f, liveRect.size.height * 0.5f) + RUNTIME_EDGE_PADDING;

        if (
            center.x + extentX < cameraMin.x ||
            center.x - extentX > cameraMax.x ||
            center.y + extentY < cameraMin.y ||
            center.y - extentY > cameraMax.y
        ) {
            continue;
        }

        markObjectAsVisible(object);
    }
}

bool VisibilityManager::isAnimatedSpriteVisible(Object* object, cocos2d::CCSprite* sprite) {
    if (!sprite || !sprite->isVisible())
        return false;

    // CCPartAnimSprite hides unused CCSpriteParts with local visibility flags.
    // First determine whether this is actually in the GameObject's child tree.
    // Detached color/glow sprites live under GD batch nodes, whose visibility
    // Bismuth intentionally disables, so their external ancestors are ignored.
    bool belongsToObjectTree = false;
    for (auto node = static_cast<cocos2d::CCNode*>(sprite); node; node = node->getParent()) {
        if (node == object->gameObject) {
            belongsToObjectTree = true;
            break;
        }
    }

    if (!belongsToObjectTree)
        return true;

    // For real child parts, stop at the GameObject and never inspect its hidden
    // external batch parent.
    for (auto node = static_cast<cocos2d::CCNode*>(sprite); node; node = node->getParent()) {
        if (!node->isVisible())
            return false;
        if (node == object->gameObject)
            return true;
    }

    return true;
}

void VisibilityManager::markObjectAsVisible(Object* object) {
    if (!object || !object->gameObject || object->gameObject->m_isInvisible)
        return;

    // One object can now intentionally live in several visual-bound sections.
    // The generation marker makes those duplicate section references free at
    // submission time and also protects the intrusive sprite lists.
    if (object->lastVisibleGeneration == visibilityGeneration)
        return;
    object->lastVisibleGeneration = visibilityGeneration;

    updateVisibleAnimatedObject(object);

    // Geometry Dash's normal active-object loop calls activateObject() for all
    // active visual objects, not just orbs/portals. Bismuth bypasses that loop,
    // which leaves ordinary level decoration animations in their startup state.
    // Restore only the lifecycle tick for objects Bismuth already decided are
    // visible. Rendering and transforms still stay entirely in the GPU batch.
    if (!object->isAnimated)
        object->gameObject->activateObject();

    // Some non-AnimatedGameObject classes still use GD's synced animation path.
    // The stock visibility loop advances it explicitly from total level time,
    // so mirror that small visual-only tick for the Bismuth-visible set.
    auto renderer = Renderer::get();
    auto playLayer = renderer ? renderer->getPlayLayer() : nullptr;
    if (!object->isAnimated && playLayer && object->gameObject->getHasSyncedAnimation()) {
        static_cast<EnhancedGameObject*>(object->gameObject)->updateSyncedAnimation(
            playLayer->m_gameState.m_totalTime, -1
        );
    }

    for (auto& sprite : object->sprites) {
        // Bismuth's optimized PlayLayer::updateVisibility bypasses GD's normal
        // activate/deactivate loop. As a result, stock sprites outside the
        // initial active window can remain locally hidden even when Bismuth's
        // own section culling says their baked geometry is on screen. Do not
        // use Cocos visibility, displayed opacity, or parent state as a global
        // submission gate here. Per-part animation visibility needs a separate
        // live-state path that does not inherit the stock object's culling state.
        if (!sprite.sprite)
            continue;

        // This gate is intentionally restricted to AnimatedGameObject. Those
        // objects were activated above, so their local CCSpritePart state is
        // meaningful. Applying stock visibility to every baked object caused
        // the entire level to disappear as GD's skipped culling loop left
        // ordinary stock sprites inactive.
        if (object->isAnimated && !isAnimatedSpriteVisible(object, sprite.sprite))
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

    while (transformId >= objectSectionSetPerTransformGroupId.size())
        objectSectionSetPerTransformGroupId.push_back(SectionSet(returnObjectStartPosition));

    auto& sectionSet = objectSectionSetPerTransformGroupId[transformId];
    if (object->hasBakedVisualBounds) {
        // A tiny pad covers texture filtering/rounding at exact section edges.
        constexpr float BOUNDS_PAD = 2.0f;
        sectionSet.addInRect(object, {
            object->bakedVisualMin - glm::vec2(BOUNDS_PAD),
            object->bakedVisualMax + glm::vec2(BOUNDS_PAD)
        });
    } else {
        sectionSet.add(object);
    }
}