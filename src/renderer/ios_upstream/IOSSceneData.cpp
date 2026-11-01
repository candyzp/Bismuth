#ifdef GEODE_IS_IOS

#include "IOSSceneData.hpp"
#include "IOSCrashDiagnostics.hpp"

#include <Geode/Enums.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/AnimatedGameObject.hpp>
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/EnhancedGameObject.hpp>
#include <Geode/cocos/CCDirector.h>
#include <Geode/cocos/kazmath/include/kazmath/mat4.h>
#include <Geode/cocos/textures/CCTextureCache.h>
#include <Geode/utils/cocos.hpp>
#include <ObjectSorter.hpp>
#include <SpriteMeshDictionary.hpp>

#include <algorithm>
#include <vector>
#include <os/proc.h>

using namespace geode::prelude;

namespace IOSUpstream {

namespace {
SceneData* g_currentSceneData = nullptr;

// Record the OS-provided available-memory estimate at each expensive stage.
// It is advisory and deliberately does not reject a level using an arbitrary
// per-object estimate; the log should tell us where memory actually drops.
void logSceneMemorySnapshot(usize objectCount, const char* stage) {
    const auto availableBytes = os_proc_available_memory();
    crashBreadcrumb(fmt::format(
        "SceneData memory snapshot stage={} objects={} availableMiB={}",
        stage,
        objectCount,
        static_cast<u64>(availableBytes) / (1024u * 1024u)
    ));
}

bool getObjectLayerCameraBounds(
    PlayLayer* layer,
    float padding,
    float& minX,
    float& maxX,
    float& minY,
    float& maxY
) {
    auto director = cocos2d::CCDirector::get();
    if (!layer || !layer->m_objectLayer || !director)
        return false;

    const auto windowSize = director->getWinSize();
    const cocos2d::CCPoint corners[] = {
        layer->m_objectLayer->convertToNodeSpace({0.f, 0.f}),
        layer->m_objectLayer->convertToNodeSpace({windowSize.width, 0.f}),
        layer->m_objectLayer->convertToNodeSpace({0.f, windowSize.height}),
        layer->m_objectLayer->convertToNodeSpace({windowSize.width, windowSize.height})
    };

    minX = maxX = corners[0].x;
    minY = maxY = corners[0].y;
    for (const auto& corner : corners) {
        minX = std::min(minX, corner.x);
        maxX = std::max(maxX, corner.x);
        minY = std::min(minY, corner.y);
        maxY = std::max(maxY, corner.y);
    }

    minX -= padding;
    maxX += padding;
    minY -= padding;
    maxY += padding;
    return true;
}
}

SceneData::~SceneData() {
    releaseObjectParticles();
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

void SceneData::initializeLiveObjectTracking() {
    animatedObjects.clear();
    particleObjects.clear();
    lastAnimatedObjectsUpdated = 0;
    claimedParticleCount = 0;

    if (!layer || !layer->m_objects)
        return;

    animatedObjects.reserve(64);
    particleObjects.reserve(32);
    for (auto object : CCArrayExt<GameObject*>(layer->m_objects)) {
        if (!object)
            continue;

        if (object->m_hasParticles && !object->m_hasNoParticles &&
            !object->m_isHide && !object->m_particleString.empty()) {
            particleObjects.push_back({object, false});
        }

        const bool syncedAnimation = object->getHasSyncedAnimation();
        const bool rotateAction = object->getHasRotateAction();
        const bool updateChildColor = object->m_unk367;
        const bool interactiveHover =
            object->getType() == GameObjectType::Collectible;
        if (syncedAnimation || rotateAction || updateChildColor || interactiveHover) {
            animatedObjects.push_back({
                object,
                syncedAnimation,
                rotateAction,
                updateChildColor,
                interactiveHover
            });
        }
    }

    crashBreadcrumb(fmt::format(
        "SceneData live object tracking READY animated={} particleObjects={}",
        animatedObjects.size(),
        particleObjects.size()
    ));
}

void SceneData::updateObjectParticles() {
    if (!ready || !layer || layer->m_isPaused || particleObjects.empty())
        return;

    float minX = 0.f;
    float maxX = 0.f;
    float minY = 0.f;
    float maxY = 0.f;
    if (!getObjectLayerCameraBounds(layer, 150.f, minX, maxX, minY, maxY))
        return;

    for (auto& entry : particleObjects) {
        auto object = entry.object;
        if (!object)
            continue;

        bool wanted = !object->m_isGroupDisabled;
        if (wanted) {
            const auto position = object->getRealPosition();
            wanted = position.x >= minX && position.x <= maxX &&
                position.y >= minY && position.y <= maxY;
        }

        if (wanted && !entry.claimed) {
            // Match the upstream renderer: claim an object's cached particle
            // only while it is near the camera, rather than activating every
            // GameObject through the stock visibility loop.
            if (!object->m_particle)
                object->claimParticle();
            // If the layer has no cached system available yet, retry on the
            // next frame instead of recording a particle that was never claimed.
            entry.claimed = object->m_particle != nullptr;
            if (entry.claimed)
                ++claimedParticleCount;
        } else if (!wanted && entry.claimed) {
            if (object->m_particle)
                object->unclaimParticle();
            entry.claimed = false;
            if (claimedParticleCount > 0)
                --claimedParticleCount;
        }
    }
}

void SceneData::releaseObjectParticles() {
    for (auto& entry : particleObjects) {
        if (!entry.claimed)
            continue;
        if (entry.object && entry.object->m_particle)
            entry.object->unclaimParticle();
        entry.claimed = false;
    }
    claimedParticleCount = 0;
}

void SceneData::updateAnimatedObjects(float dt) {
    lastAnimatedObjectsUpdated = 0;
    if (!ready || !layer || layer->m_isPaused || animatedObjects.empty())
        return;

    float minX = 0.f;
    float maxX = 0.f;
    float minY = 0.f;
    float maxY = 0.f;
    if (!getObjectLayerCameraBounds(layer, 150.f, minX, maxX, minY, maxY))
        return;

    const auto backgroundColor = layer->m_background
        ? layer->m_background->getColor()
        : cocos2d::ccColor3B{255, 255, 255};
    const auto brightBackgroundColor =
        GameToolbox::transformColor(backgroundColor, 0.0, -0.3, 0.4);
    const float totalTime = layer->m_gameState.m_totalTime;
    const float hoverY = layer->m_hoverNode
        ? layer->m_hoverNode->getPosition().y
        : 0.f;

    for (const auto& entry : animatedObjects) {
        auto object = entry.object;
        if (!object || object->m_isGroupDisabled)
            continue;

        const auto position = object->getRealPosition();
        const bool nearCamera =
            position.x >= minX && position.x <= maxX &&
            position.y >= minY && position.y <= maxY;
        if (!nearCamera)
            continue;

        // These are the object-specific visual updates skipped by
        // optimized_updateVisibility(). Keep them on the CPU before the GPU
        // batch captures the new sprite frame/appearance below.
        if (entry.syncedAnimation) {
            static_cast<EnhancedGameObject*>(object)->updateSyncedAnimation(totalTime, -1);
        }
        if (entry.rotateAction) {
            static_cast<EnhancedGameObject*>(object)->updateRotateAction(dt);
        }
        if (entry.updateChildColor) {
            static_cast<AnimatedGameObject*>(object)->updateChildSpriteColor(
                brightBackgroundColor
            );
        }
        if (entry.interactiveHover && layer->m_hoverNode) {
            static_cast<EffectGameObject*>(object)->updateInteractiveHover(hoverY);
        }

        ++lastAnimatedObjectsUpdated;
    }
}

bool SceneData::buildSortedObjectList(std::vector<GameObject*>& objects) const {
    if (!layer || !layer->m_objects)
        return false;

    const bool diagnosticsEnabled = crashDiagnosticsEnabled();
    usize visited = 0;
    ObjectSorter sorter;
    sorter.initForGameLayer(layer);

    for (auto object : CCArrayExt<GameObject*>(layer->m_objects)) {
        ++visited;
        if (diagnosticsEnabled && visited % 50000u == 0) {
            crashBreadcrumb(fmt::format(
                "SceneData::buildSortedObjectList progress visited={}/{}",
                visited,
                layer->m_objects->count()
            ));
        }
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
    crashBreadcrumb("SceneData::buildSortedObjectList sorter input complete; materializing sorted object vector");
    objects.clear();
    for (auto iterator = sorter.iterator(); !iterator.isEnd(); iterator.next())
        objects.push_back(iterator.get());
    crashBreadcrumb(fmt::format("SceneData::buildSortedObjectList COMPLETE sortedObjects={}", objects.size()));

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
    releaseObjectParticles();
    ready = false;
    failureReason.clear();
    layer = playLayer;
    initializeLiveObjectTracking();
    spriteSheets.fill(nullptr);
    if (!layer || !layer->m_objects || !layer->m_effectManager) {
        failureReason = "PlayLayer, level objects, or effect manager is missing";
        return false;
    }

    const usize objectCount = static_cast<usize>(layer->m_objects->count());
    crashBreadcrumb(fmt::format(
        "SceneData::init BEGIN layer={:p} objects={}",
        static_cast<void*>(layer),
        objectCount
    ));
    logSceneMemorySnapshot(objectCount, "before GroupManager");

    auto textureCache = cocos2d::CCTextureCache::get();
    if (!textureCache) {
        failureReason = "Cocos texture cache is unavailable";
        return false;
    }

    // Match the exact texture set currently used by the owner's Renderer.
    // TEXT/SPECIAL remain unassigned in upstream (TODO there).
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

    crashBreadcrumb("SceneData::init GroupManager BEGIN");
    if (!groupManager.initWithObjects(layer, layer->m_objects)) {
        failureReason = "GroupManager could not build the level's group-state tables";
        log::error("Bismuth iOS upstream scene data: {}", failureReason);
        return false;
    }
    crashBreadcrumb(fmt::format("SceneData::init GroupManager PASS combinations={} maxGroupId={}",
        groupManager.getGroupCombinationCount(), groupManager.getMaxGroupId()));
    logSceneMemorySnapshot(objectCount, "after GroupManager / before sorting");

    std::vector<GameObject*> objects;
    crashBreadcrumb("SceneData::init sorted object extraction BEGIN");
    if (!buildSortedObjectList(objects)) {
        failureReason = "ObjectSorter could not build the level's sorted object list";
        log::error("Bismuth iOS upstream scene data: {}", failureReason);
        return false;
    }
    logSceneMemorySnapshot(objectCount, "after sorting / before ObjectBatch");

    fetchBlendingStates(blendStates);
    this->blendStates = blendStates;

    crashBreadcrumb(fmt::format("SceneData::init ObjectBatch BEGIN sortedObjects={}", objects.size()));
    const bool batchOK = objectBatch.build(
        objects,
        groupManager,
        this->blendStates,
        [this](SpriteSheet sheet) { return getSpriteSheetTexture(sheet); }
    );
    if (!batchOK) {
        failureReason =
            "ObjectBatch build failed; check the preceding object ID, sprite sheet, or mesh diagnostic";
        log::error(
            "Bismuth iOS upstream scene data: {}; {} objects were sorted",
            failureReason,
            objects.size()
        );
        return false;
    }
    crashBreadcrumb(fmt::format("SceneData::init ObjectBatch PASS sprites={} sourceVertices={} orderedVertices={}",
        objectBatch.getSpriteCount(), objectBatch.getVertices().size(), objectBatch.getOrderedVertices().size()));
    logSceneMemorySnapshot(objectCount, "after ObjectBatch");

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
    crashBreadcrumb("SceneData::init GPU state-table initialization BEGIN");
    logSceneMemorySnapshot(objectCount, "before GPU state-table upload");
    if (!gpuStateTables.initialize(*this)) {
        failureReason = "GPU state-table initialization or texture upload failed";
        log::error("Bismuth iOS upstream scene data: {}", failureReason);
        ready = false;
        return false;
    }

    crashBreadcrumb("SceneData::init COMPLETE; GPU state tables ready");
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

bool SceneData::refreshBlendOrder() {
    if (!ready || !layer)
        return false;

    // Match Renderer::draw(): refresh the live blend flags at render time,
    // before selecting the layer range and uploading the sorted vertex stream.
    ColorChannelBlendStates currentBlendStates = blendStates;
    fetchBlendingStates(currentBlendStates);
    blendStates = currentBlendStates;

    // The owner refreshes the map while paused but does not run predraw until
    // the next active frame.
    if (layer->m_isPaused)
        return true;

    if (!objectBatch.refreshBlendOrder(blendStates)) {
        log::error("Bismuth iOS upstream scene data: failed to apply a refreshed blend-state schedule");
        return false;
    }
    return true;
}

void SceneData::prepareForDraw() {
    if (ready)
        updateGameState();
}

bool SceneData::update(float dt) {
    if (!ready)
        return false;

    gameTimer += dt;

    if (layer->m_isPaused)
        return true;

    updateAnimatedObjects(dt);
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
