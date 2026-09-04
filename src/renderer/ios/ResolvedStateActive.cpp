#ifdef GEODE_IS_IOS

#include "ResolvedStateLayer.hpp"

#include <algorithm>

using namespace geode::prelude;

namespace {
ResolvedStateLayer* g_currentResolvedState = nullptr;

static void insertSortedUnique(std::vector<usize>& values, usize value) {
    auto it = std::lower_bound(values.begin(), values.end(), value);
    if (it == values.end() || *it != value)
        values.insert(it, value);
}

static void eraseSorted(std::vector<usize>& values, usize value) {
    auto it = std::lower_bound(values.begin(), values.end(), value);
    if (it != values.end() && *it == value)
        values.erase(it);
}
} // namespace

ResolvedStateLayer::ResolvedStateLayer() {
    g_currentResolvedState = this;
}

ResolvedStateLayer::~ResolvedStateLayer() {
    if (g_currentResolvedState == this)
        g_currentResolvedState = nullptr;
    destroyTextures();
}

ResolvedStateLayer* ResolvedStateLayer::getCurrent() {
    return g_currentResolvedState;
}

void ResolvedStateLayer::ensureEventOwnership() {
    if (eventOwnershipReady)
        return;

    // setGPUOwnedSprites() initially stores every owned record in these vectors.
    // Compile that complete ownership once, then repurpose the vectors below for
    // only the objects that stock GD actually has active.
    gpuOwnedObjectMask.assign(objects.size(), false);
    gpuOwnedSpriteMask.assign(sprites.size(), false);

    for (usize objectIndex : activeObjectIndices) {
        if (objectIndex < gpuOwnedObjectMask.size())
            gpuOwnedObjectMask[objectIndex] = true;
    }
    for (usize spriteIndex : activeSpriteIndices) {
        if (spriteIndex < gpuOwnedSpriteMask.size())
            gpuOwnedSpriteMask[spriteIndex] = true;
    }

    objectIndexByPointer.clear();
    objectIndexByPointer.reserve(objects.size());
    for (usize objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        if (objects[objectIndex].object)
            objectIndexByPointer.emplace(objects[objectIndex].object, objectIndex);
    }

    activeObjectMask.assign(objects.size(), false);
    activeSpriteMask.assign(sprites.size(), false);
    pendingDeactivateMask.assign(objects.size(), false);
    pendingDeactivateIndices.clear();
    pendingDeactivateIndices.reserve(64);

    eventOwnershipReady = true;
}

void ResolvedStateLayer::reseedActiveFromStock() {
    ensureEventOwnership();
    if (!eventOwnershipReady)
        return;

    activeObjectIndices.clear();
    activeSpriteIndices.clear();
    std::fill(activeObjectMask.begin(), activeObjectMask.end(), false);
    std::fill(activeSpriteMask.begin(), activeSpriteMask.end(), false);
    std::fill(pendingDeactivateMask.begin(), pendingDeactivateMask.end(), false);
    pendingDeactivateIndices.clear();

    stats.activeGPUObjects = 0;
    stats.activeGPUSprites = 0;
    stats.activeStaticObjects = 0;

    // This is a one-time/reset seed only. The per-frame path never scans the
    // whole level. Parent + visible are stock-resolved lifecycle facts here.
    for (usize objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        if (objectIndex >= gpuOwnedObjectMask.size() || !gpuOwnedObjectMask[objectIndex])
            continue;

        const auto& objectRecord = objects[objectIndex];
        auto object = objectRecord.object;
        if (!object || !object->getParent() || !object->isVisible())
            continue;

        activeObjectMask[objectIndex] = true;
        activeObjectIndices.push_back(objectIndex);
        ++stats.activeGPUObjects;
        if (objectRecord.safety == SafetyClass::StaticSafe)
            ++stats.activeStaticObjects;

        const usize spriteEnd = std::min<usize>(
            static_cast<usize>(sprites.size()),
            objectRecord.firstSprite + objectRecord.spriteCount
        );
        for (usize spriteIndex = objectRecord.firstSprite; spriteIndex < spriteEnd; ++spriteIndex) {
            if (spriteIndex >= gpuOwnedSpriteMask.size() || !gpuOwnedSpriteMask[spriteIndex])
                continue;
            activeSpriteMask[spriteIndex] = true;
            activeSpriteIndices.push_back(spriteIndex);
            ++stats.activeGPUSprites;
        }
    }

    stats.staticObjectsReused = stats.activeStaticObjects;

    log::info(
        "Bismuth iOS stock-active seed: {} / {} GPU object(s), {} GPU sprite(s) in the hot path",
        stats.activeGPUObjects,
        std::count(gpuOwnedObjectMask.begin(), gpuOwnedObjectMask.end(), true),
        stats.activeGPUSprites
    );
}

void ResolvedStateLayer::addActiveObjectIndex(usize objectIndex) {
    if (!eventOwnershipReady || objectIndex >= objects.size() ||
        objectIndex >= gpuOwnedObjectMask.size() || !gpuOwnedObjectMask[objectIndex]) {
        return;
    }

    if (objectIndex < pendingDeactivateMask.size())
        pendingDeactivateMask[objectIndex] = false;

    if (!activeObjectMask[objectIndex]) {
        activeObjectMask[objectIndex] = true;
        insertSortedUnique(activeObjectIndices, objectIndex);
        ++stats.activeGPUObjects;
        if (objects[objectIndex].safety == SafetyClass::StaticSafe)
            ++stats.activeStaticObjects;
    }

    const auto& record = objects[objectIndex];
    const usize spriteEnd = std::min<usize>(
        static_cast<usize>(sprites.size()),
        record.firstSprite + record.spriteCount
    );
    for (usize spriteIndex = record.firstSprite; spriteIndex < spriteEnd; ++spriteIndex) {
        if (spriteIndex >= gpuOwnedSpriteMask.size() || !gpuOwnedSpriteMask[spriteIndex])
            continue;
        if (activeSpriteMask[spriteIndex])
            continue;

        activeSpriteMask[spriteIndex] = true;
        insertSortedUnique(activeSpriteIndices, spriteIndex);
        ++stats.activeGPUSprites;
    }
}

void ResolvedStateLayer::removeActiveObjectIndex(usize objectIndex) {
    if (!eventOwnershipReady || objectIndex >= objects.size() ||
        objectIndex >= activeObjectMask.size() || !activeObjectMask[objectIndex]) {
        return;
    }

    activeObjectMask[objectIndex] = false;
    eraseSorted(activeObjectIndices, objectIndex);
    if (stats.activeGPUObjects)
        --stats.activeGPUObjects;
    if (objects[objectIndex].safety == SafetyClass::StaticSafe && stats.activeStaticObjects)
        --stats.activeStaticObjects;

    const auto& record = objects[objectIndex];
    const usize spriteEnd = std::min<usize>(
        static_cast<usize>(sprites.size()),
        record.firstSprite + record.spriteCount
    );
    for (usize spriteIndex = record.firstSprite; spriteIndex < spriteEnd; ++spriteIndex) {
        if (spriteIndex >= activeSpriteMask.size() || !activeSpriteMask[spriteIndex])
            continue;
        if (spriteIndex >= gpuOwnedSpriteMask.size() || !gpuOwnedSpriteMask[spriteIndex])
            continue;

        activeSpriteMask[spriteIndex] = false;
        eraseSorted(activeSpriteIndices, spriteIndex);
        if (stats.activeGPUSprites)
            --stats.activeGPUSprites;
    }
}

void ResolvedStateLayer::onObjectActivated(GameObject* object) {
    if (!eventOwnershipReady || !object)
        return;

    auto it = objectIndexByPointer.find(object);
    if (it == objectIndexByPointer.end())
        return;

    // Called only AFTER stock GameObject::activateObject(). GD has already made
    // the visibility/parent decision and attached the root to its render home.
    addActiveObjectIndex(it->second);
}

void ResolvedStateLayer::onObjectDeactivated(GameObject* object) {
    if (!eventOwnershipReady || !object)
        return;

    auto it = objectIndexByPointer.find(object);
    if (it == objectIndexByPointer.end())
        return;

    const usize objectIndex = it->second;
    if (objectIndex >= activeObjectMask.size() || !activeObjectMask[objectIndex])
        return;
    if (objectIndex >= pendingDeactivateMask.size() || pendingDeactivateMask[objectIndex])
        return;

    // Keep the record in the hot path through Renderer::update(). Because stock
    // deactivateObject() already ran, that update captures visible=false and
    // uploads the final hide state before we stop polling it.
    pendingDeactivateMask[objectIndex] = true;
    pendingDeactivateIndices.push_back(objectIndex);
}

void ResolvedStateLayer::finishEventFrame() {
    if (!eventOwnershipReady || pendingDeactivateIndices.empty())
        return;

    for (usize objectIndex : pendingDeactivateIndices) {
        if (objectIndex >= pendingDeactivateMask.size() || !pendingDeactivateMask[objectIndex])
            continue;
        pendingDeactivateMask[objectIndex] = false;
        removeActiveObjectIndex(objectIndex);
    }
    pendingDeactivateIndices.clear();

    if (stats.staticObjectsReused > stats.activeStaticObjects)
        stats.staticObjectsReused = stats.activeStaticObjects;
}

#endif
