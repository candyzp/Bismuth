#ifdef GEODE_IS_IOS

#include "IOSSceneData.hpp"

#include <Geode/Enums.hpp>
#include <Geode/binding/GJEffectManager.hpp>
#include <Geode/cocos/sprite_nodes/CCTextureCache.h>
#include <Geode/utils/cocos.hpp>
#include <ObjectSorter.hpp>
#include <SpriteMeshDictionary.hpp>

#include <algorithm>
#include <vector>

using namespace geode::prelude;

namespace IOSUpstream {

cocos2d::CCTexture2D* SceneData::getSpriteSheetTexture(SpriteSheet sheet) const {
    const auto index = static_cast<i32>(sheet);
    if (index < 0 || index >= static_cast<i32>(spriteSheets.size()))
        return nullptr;
    return spriteSheets[static_cast<usize>(index)];
}

bool SceneData::buildSortedObjectList(std::vector<GameObject*>& objects) const {
    if (!layer || !layer->m_objects)
        return false;

    ObjectSorter sorter;
    sorter.initForGameLayer(layer);

    for (auto object : CCArrayExt<GameObject*>(layer->m_objects)) {
        if (!object)
            continue;
        if (object == layer->m_anticheatSpike)
            continue;
        if (object->isTrigger() || object->m_isHide ||
            object->m_objectID == GUIDE_OBJECT_ID)
            continue;

        sorter.addGameObject(object);
    }

    // The owner has an explicit final sort pass. Call it here so object Z
    // ordering is deterministic before ObjectBatch builds its source mesh.
    sorter.finalizeSorting();

    objects.clear();
    for (auto iterator = sorter.iterator(); !iterator.isEnd(); iterator.next())
        objects.push_back(iterator.get());

    return true;
}

void SceneData::fetchBlendingStates(ColorChannelBlendStates& states) const {
    states.fill(false);
    if (!layer || !layer->m_effectManager)
        return;

    const auto& channelActions = layer->m_effectManager->m_colorActionVector;
    const usize count = std::min<usize>(states.size(), channelActions.size());
    for (usize i = 0; i < count; ++i) {
        auto action = channelActions[i];
        if (action)
            states[i] = action->m_blending;
    }

    // These player/background channels use blending in the owner's renderer.
    states[1005] = true; // P1
    states[1006] = true; // P2
    states[1007] = true; // LBG
}

bool SceneData::init(PlayLayer* playLayer) {
    ready = false;
    layer = playLayer;
    spriteSheets.fill(nullptr);
    if (!layer || !layer->m_objects || !layer->m_effectManager)
        return false;

    auto textureCache = cocos2d::CCTextureCache::get();
    if (!textureCache)
        return false;

    // Keep the exact texture set currently loaded by the owner's Renderer.
    // TEXT/SPECIAL remain intentionally unassigned in upstream (TODO there).
    spriteSheets[static_cast<usize>(SpriteSheet::GAME_1)] =
        textureCache->addImage("GJ_GameSheet.png", false);
    spriteSheets[static_cast<usize>(SpriteSheet::GAME_2)] =
        textureCache->addImage("GJ_GameSheet02.png", false);
    spriteSheets[static_cast<usize>(SpriteSheet::PARTICLE)] =
        textureCache->addImage("GJ_ParticleSheet.png", false);
    spriteSheets[static_cast<usize>(SpriteSheet::GLOW)] =
        textureCache->addImage("GJ_GameSheetGlow.png", false);
    spriteSheets[static_cast<usize>(SpriteSheet::FIRE)] =
        textureCache->addImage("FireSheet_01.png", false);
    spriteSheets[static_cast<usize>(SpriteSheet::PIXEL)] =
        textureCache->addImage("PixelSheet_01.png", false);

    SpriteMeshDictionary::loadFromFile("spriteMeshes.json");

    if (!groupManager.initWithObjects(layer, layer->m_objects)) {
        log::error("Bismuth iOS upstream scene data: GroupManager initialization failed");
        return false;
    }

    std::vector<GameObject*> objects;
    if (!buildSortedObjectList(objects)) {
        log::error("Bismuth iOS upstream scene data: object sorting failed");
        return false;
    }

    ColorChannelBlendStates blendStates {};
    fetchBlendingStates(blendStates);

    const bool batchOK = objectBatch.build(
        objects,
        groupManager,
        blendStates,
        [this](SpriteSheet sheet) { return getSpriteSheetTexture(sheet); }
    );
    if (!batchOK) {
        log::error(
            "Bismuth iOS upstream scene data: ObjectBatch build failed; {} objects were sorted",
            objects.size()
        );
        return false;
    }

    log::info(
        "Bismuth iOS upstream scene data prepared: {} objects, {} sprites, {} group combinations, {} work groups, {} render layers",
        objects.size(),
        objectBatch.getSpriteCount(),
        groupManager.getGroupCombinationCount(),
        objectBatch.getSortedBatch().workGroupCount,
        objectBatch.getSortedBatch().layerRanges.size()
    );

    ready = true;
    return true;
}

void SceneData::reset() {
    if (!ready)
        return;
    groupManager.resetGroupStates();
    objectBatch.refreshSpriteAppearances();
}

void SceneData::update() {
    if (!ready)
        return;
    groupManager.updateGroupStateBuffer();
    objectBatch.refreshSpriteAppearances();
}

} // namespace IOSUpstream

#endif
