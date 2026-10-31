#ifdef GEODE_IS_IOS

#include "IOSSceneData.hpp"

#include <Geode/Enums.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/cocos/CCDirector.h>
#include <Geode/cocos/kazmath/include/kazmath/mat4.h>
#include <Geode/cocos/sprite_nodes/CCTextureCache.h>
#include <Geode/utils/cocos.hpp>
#include <ObjectSorter.hpp>
#include <SpriteMeshDictionary.hpp>

#include <algorithm>
#include <vector>

using namespace geode::prelude;

namespace IOSUpstream {

namespace {
SceneData* g_currentSceneData = nullptr;
}

SceneData::~SceneData() {
    if (g_currentSceneData == this)
        g_currentSceneData = nullptr;
}

SceneData* SceneData::getCurrent() {
    return g_currentSceneData;
}

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
            object->m_objectID == IOS_GUIDE_OBJECT_ID)
            continue;

        sorter.addGameObject(object);
    }

    // Match the owner's Renderer::init iteration: consume the ObjectSorter
    // iterator after populating the level's layer lists.
    objects.clear();
    for (auto iterator = sorter.iterator(); !iterator.isEnd(); iterator.next())
        objects.push_back(iterator.get());

    return true;
}

void SceneData::fetchBlendingStates(ColorChannelBlendStates& states) const {
    // blendStates is zero-initialized when SceneData is constructed. Match
    // upstream fetchBlendingStates(): update present action entries without
    // clearing channels whose action pointer is temporarily null.
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

    fetchBlendingStates(blendStates);
    this->blendStates = blendStates;

    const bool batchOK = objectBatch.build(
        objects,
        groupManager,
        this->blendStates,
        [this](SpriteSheet sheet) { return getSpriteSheetTexture(sheet); }
    );
    if (!batchOK) {
        log::error(
            "Bismuth iOS upstream scene data: ObjectBatch build failed; {} objects were sorted",
            objects.size()
        );
        return false;
    }

    gameTimer = 0.f;
    updateColorChannelTable();
    updateGameState();

    log::info(
        "Bismuth iOS upstream scene data prepared: {} objects, {} sprites, {} group combinations, {} work groups, {} render layers",
        objects.size(),
        objectBatch.getSpriteCount(),
        groupManager.getGroupCombinationCount(),
        objectBatch.getSortedBatch().workGroupCount,
        objectBatch.getSortedBatch().layerRanges.size()
    );

    ready = true;
    if (!gpuStateTables.initialize(*this)) {
        log::error("Bismuth iOS upstream scene data: GPU state-table initialization failed");
        ready = false;
        return false;
    }

    g_currentSceneData = this;
    return true;
}

void SceneData::updateColorChannelTable() {
    // ColorChannelTable is initialized to white once with SceneData. Preserve
    // previous resolved channel values when no current action sprite is
    // present, matching the owner's persistent GPU color-channel buffer.
    if (!layer || !layer->m_effectManager)
        return;

    // Match the owner's prepareColorChannelBuffer(): use m_copyColor, which
    // includes the final resolved channel color, not the older m_color value.
    const auto& channelSprites = layer->m_effectManager->m_colorActionSpriteVector;
    for (auto sprite : channelSprites) {
        if (!sprite)
            continue;

        const i32 channelId = sprite->m_colorID;
        if (channelId < 0 || channelId >= static_cast<i32>(colorChannels.size()))
            continue;

        auto& output = colorChannels[static_cast<usize>(channelId)];
        output.r = sprite->m_copyColor.r;
        output.g = sprite->m_copyColor.g;
        output.b = sprite->m_copyColor.b;
        output.a = static_cast<u8>(sprite->m_opacity);
    }

    colorChannels[IOS_COLOR_CHANNEL_BLACK] = {0, 0, 0, 255};
}

void SceneData::updateGameState() {
    if (!layer || !layer->m_objectLayer)
        return;

    kmMat4 projection;
    kmMat4 modelView;
    kmMat4 modelViewProjection;
    kmGLGetMatrix(KM_GL_PROJECTION, &projection);
    kmGLGetMatrix(KM_GL_MODELVIEW, &modelView);
    kmMat4Multiply(&modelViewProjection, &projection, &modelView);
    reinterpret_cast<kmMat4&>(gameState.mvp) = modelViewProjection;

    gameState.timer = gameTimer;
    gameState.cameraViewSize = glm::vec2(
        layer->m_cameraWidth,
        layer->m_cameraHeight
    );

    const auto windowSize = CCDirector::get()->getWinSize();
    gameState.windowSize = glm::vec2(windowSize.width, windowSize.height);
    gameState.screenRight = CCDirector::get()->getScreenRight();
    gameState.cameraUnzoomedX = layer->m_cameraUnzoomedX;

    const cocos2d::ccColor3B specialLightBackground =
        GameToolbox::transformColor(
            layer->m_effectManager->activeColorForIndex(IOS_COLOR_CHANNEL_BG),
            0.0,
            -0.2,
            0.2
        );
    gameState.specialLightBGColor = ccColor3BToGLM(specialLightBackground);

    const glm::vec2 cameraBottomLeft = ccPointToGLM(
        layer->m_objectLayer->convertToNodeSpace({0.f, 0.f})
    );
    const glm::vec2 cameraBottomRight = ccPointToGLM(
        layer->m_objectLayer->convertToNodeSpace({windowSize.width, 0.f})
    );
    const glm::vec2 cameraTopLeft = ccPointToGLM(
        layer->m_objectLayer->convertToNodeSpace({0.f, windowSize.height})
    );

    const glm::vec2 cameraRight = cameraBottomRight - cameraBottomLeft;
    const glm::vec2 cameraUp = cameraTopLeft - cameraBottomLeft;
    gameState.cameraPosition = cameraBottomLeft;

    const float rightLength = glm::length(cameraRight);
    const float upLength = glm::length(cameraUp);
    gameState.cameraRight = rightLength > 0.f
        ? cameraRight / rightLength : glm::vec2(1.f, 0.f);
    gameState.cameraUp = upLength > 0.f
        ? cameraUp / upLength : glm::vec2(0.f, 1.f);
    gameState.cameraSize = glm::vec2(rightLength, upLength);

    gameState.gameStateFlags = 0;
    if (layer->m_player1 && layer->m_player1->m_isDead)
        gameState.gameStateFlags |= IOS_GAME_STATE_IS_PLAYER_DEAD;

    if (layer->m_skipAudioStep)
        gameState.audioScale = FMODAudioEngine::sharedEngine()->getMeteringValue();
    else if (layer->m_audioEffectsLayer)
        gameState.audioScale = layer->m_audioEffectsLayer->m_audioScale;
    else
        gameState.audioScale = 0.5f;

    if (layer->m_isSilent ||
        (layer->m_isPracticeMode && !layer->m_practiceMusicSync)) {
        gameState.audioScale = 0.5f;
    }
}

void SceneData::reset() {
    if (!ready)
        return;

    // The owner resets group transforms/toggles but keeps the level's
    // initialization-time blend schedule. Refresh the mutable tables now so
    // the next owned draw sees the reset state immediately.
    groupManager.resetGroupStates();
    updateColorChannelTable();
    objectBatch.refreshSpriteAppearances();

    if (!gpuStateTables.upload(*this))
        log::error("Bismuth iOS upstream scene data: GPU state-table upload failed after reset");
}

void SceneData::prepareForDraw() {
    if (ready)
        updateGameState();
}

bool SceneData::update(float dt) {
    if (!ready)
        return false;

    gameTimer += dt;

    // Match Renderer::draw(): refresh channel blend flags every frame, even
    // while paused. The owner only runs ObjectBatch::predraw() when unpaused,
    // so a new sorted variant is requested on the first active frame.
    ColorChannelBlendStates currentBlendStates = blendStates;
    fetchBlendingStates(currentBlendStates);
    blendStates = currentBlendStates;

    if (layer->m_isPaused)
        return true;

    if (!objectBatch.refreshBlendOrder(blendStates)) {
        log::error("Bismuth iOS upstream scene data: failed to apply a refreshed blend-state schedule");
        return false;
    }

    updateColorChannelTable();
    groupManager.updateGroupStateBuffer();
    objectBatch.refreshSpriteAppearances();

    if (!gpuStateTables.upload(*this)) {
        log::error("Bismuth iOS upstream scene data: GPU state-table upload failed during gameplay");
        return false;
    }
    return true;
}

} // namespace IOSUpstream

#endif
