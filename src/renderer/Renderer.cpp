#include "Renderer.hpp"
#include "Geode/cocos/CCDirector.h"
#include "Geode/cocos/kazmath/include/kazmath/mat4.h"
#include "Geode/cocos/platform/win32/CCGL.h"
#include "Geode/cocos/robtop/keyboard_dispatcher/CCKeyboardDelegate.h"
#include "Geode/cocos/sprite_nodes/CCSpriteBatchNode.h"
#include "GroupManager.hpp"
#include "HotKey.hpp"
#include "ObjectBatchNode.hpp"
#include "SpriteMeshDictionary.hpp"
#include "ccTypes.h"
#include "common.hpp"
#include "glm/fwd.hpp"
#include <Geode/Enums.hpp>
#include <Geode/binding/AdvancedFollowTriggerObject.hpp>
#include <Geode/binding/EnterEffectObject.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/KeyframeAnimTriggerObject.hpp>
#include <Geode/binding/KeyframeGameObject.hpp>
#include <Geode/binding/TransformTriggerGameObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/RingObject.hpp>
#include "profiler.hpp"
#include "ObjectSorter.hpp"

using namespace geode::prelude;

static Renderer* currentRenderer;
static PlayLayer* currentRendererLayer;

PackedHSV packHSV(const ccHSVValue& hsv) {
    PackedHSV ret;

    ret.hueAndFlags = glm::packHalf2x16({hsv.h, 0}) & 0xffff;

    if (hsv.absoluteSaturation) ret.hueAndFlags |= HSV_SAT_ADD;
    if (hsv.absoluteBrightness) ret.hueAndFlags |= HSV_VAL_ADD;

    ret.satAndVal = glm::packHalf2x16({hsv.s, hsv.v});

    return ret;
}

Renderer::~Renderer() { terminate(); }

class FinishNode : public CCNode {
public:
    void draw() {
        if (currentRenderer)
            currentRenderer->finish();        
    }
};

/*
    These trigger types change objects in ways that can't be expressed as a
    per-group state (they animate or transform individual objects over time).
    Levels that contain them are still rendered by Kyanite; the objects these
    triggers affect just keep their initial transform. The renderer is never
    disabled for a whole level because of them, otherwise Kyanite would do
    nothing at all on most modern levels.
*/
static bool levelUsesUnsupportedTriggers(cocos2d::CCArray* objects) {
    if (!objects)
        return false;

    for (auto object : CCArrayExt<GameObject*, false>(objects)) {
        if (!object)
            continue;

        if (typeinfo_cast<EnterEffectObject*>(object) ||
            typeinfo_cast<TransformTriggerGameObject*>(object) ||
            typeinfo_cast<AdvancedFollowTriggerObject*>(object) ||
            typeinfo_cast<KeyframeGameObject*>(object) ||
            typeinfo_cast<KeyframeAnimTriggerObject*>(object))
            return true;

        // Scale trigger (2067), area effects (3006-3015), advanced follow
        // (3016) and enter effects (3017+) all change individual objects
        // instead of a whole group, which the group state can't represent.
        i32 id = object->m_objectID;
        if (id == 2067 || (id >= 3006 && id <= 3100))
            return true;
    }

    return false;
}

bool Renderer::init(PlayLayer* layer, CCArray* levelObjects) {
    log::info("Creating renderer for a level with {} object(s)", levelObjects ? levelObjects->count() : 0);

    if (currentRenderer) {
        // A renderer for the same level is already alive. Don't create a
        // second one.
        if (currentRendererLayer == layer) {
            log::warn("A renderer already exists for this layer, keeping it");
            return false;
        }

        // A renderer from a previous level is still alive (this can happen
        // while a level transition is in progress). Drop it so it cannot
        // keep applying its state to the wrong PlayLayer.
        auto stale = currentRenderer;
        currentRenderer = nullptr;
        currentRendererLayer = nullptr;

        if (stale->getParent())
            stale->removeFromParentAndCleanup(true);
    }

    currentRenderer = this;
    currentRendererLayer = layer;

    if (!initInternal(layer, levelObjects)) {
        // create() deletes this renderer when init fails, so the global
        // pointer must never keep aiming at it.
        if (currentRenderer == this) {
            currentRenderer = nullptr;
            currentRendererLayer = nullptr;
        }
        return false;
    }

    return true;
}

bool Renderer::initInternal(PlayLayer* layer, CCArray* levelObjects) {
    if (!isOpenGLAtLeastVersion(4, 3)) {
        Notification::create(
            "Your device doesn't support the required OpenGL 4.3\nPlease update your drivers",
            NotificationIcon::Error,
            5.0f
        )->show();

        const char* versionString = (const char*)glGetString(GL_VERSION);
        log::error("Device doesn't support OpenGL 4.3. OpenGL Version: {}", versionString ? versionString : "unknown");
        return false;
    }

    if (!hasLoadedNewerOpenGLFunctions())
        loadNewerOpenGLFunctions();

    this->layer = layer;
    
    auto size = CCDirector::get()->getWinSize();

    if (!Mod::get()->getSettingValue<bool>("enabled")) {
        log::warn("Kyanite is disabled in the mod settings, not creating a renderer");
        return false;
    }

    debugToolsEnabled = Mod::get()->getSettingValue<bool>("debug_tools");
    disableObjectPulse = Mod::get()->getSettingValue<bool>("disable_object_pulse");
    frameTiming.setEnabled(Mod::get()->getSettingValue<bool>("frame_timing"));

    if (levelUsesUnsupportedTriggers(levelObjects)) {
        log::warn("Level uses area, transform or keyframe triggers, which the optimized renderer doesn't support yet; those objects keep their initial transform");
    }

    log::info("OpenGL Version: {}", (const char*)glGetString(GL_VERSION));

    i64 value = 0;
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &value);
    log::info("GL_MAX_SHADER_STORAGE_BLOCK_SIZE: {}", byteSizeToString(value));
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &value);
    log::info("GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS: {}", value);

    const char* fontTexture = GameManager::get()->getFontTexture(GameManager::get()->m_loadedFont);

    auto tcache = cocos2d::CCTextureCache::get();
    if (!tcache) {
        log::error("Failed to get the texture cache");
        return false;
    }
    // TODO: Add text sheet
    spriteSheets[(i32)SpriteSheet::GAME_1]   = tcache->addImage("GJ_GameSheet.png", false);
    spriteSheets[(i32)SpriteSheet::GAME_2]   = tcache->addImage("GJ_GameSheet02.png", false);
    spriteSheets[(i32)SpriteSheet::PARTICLE] = tcache->addImage("GJ_ParticleSheet.png", false);
    spriteSheets[(i32)SpriteSheet::GLOW]     = tcache->addImage("GJ_GameSheetGlow.png", false);
    spriteSheets[(i32)SpriteSheet::FIRE]     = tcache->addImage("FireSheet_01.png", false);
    spriteSheets[(i32)SpriteSheet::PIXEL]    = tcache->addImage("PixelSheet_01.png", false);

    if (fontTexture)
        spriteSheets[(i32)SpriteSheet::TEXT] = tcache->addImage(fontTexture, false);

    usize unsupportedSheetObjectCount = 0;

    SpriteMeshDictionary::loadFromFile("spriteMeshes.json");

    groupManager.initWithObjects(levelObjects);

    ObjectSorter sorter;
    sorter.initForGameLayer(layer);

    std::vector<GameObject*> objects;

    log::info("Level contains {} object(s)", levelObjects->count());
    log::info("Sorting objects...");
    for (auto object : CCArrayExt<GameObject*>(levelObjects)) {
        if (object == layer->m_anticheatSpike) {
            DEBUG_LOG("- anti-cheat spike");
            continue;
        }

        if (
            object->isTrigger() ||
            object->m_isHide ||
            object->m_objectID == GUIDE_OBJECT_ID ||
            object->m_objectID == KEYFRAME_OBJECT_ID
        ) {
            continue;
        }

        if (object == layer->m_endPortal)
            continue;

        DEBUG_LOG("- {}, id: {}", (void*)object, object->m_objectID);
        DEBUG_LOG("  - rotation: {}, {}", object->m_startRotationX, object->m_startRotationY);
        DEBUG_LOG("  - scale: {}, {}", object->m_startScaleX, object->m_startScaleY);
        DEBUG_LOG("  - flip: {}, {}", object->m_startFlipX, object->m_startFlipX);
        DEBUG_LOG("  - zlayer: {}", zLayerAsString(object->getObjectZLayer()));
        DEBUG_LOG("  - zorder: {}", object->getObjectZOrder());
        DEBUG_LOG("  - blending: {}", object->m_baseOrDetailBlending);
        DEBUG_LOG("  - spritesheet: {}", object->getParentMode());
        DEBUG_LOG("  - customGlowColor: {}", object->m_customGlowColor);
        DEBUG_LOG("  - glowColorIsLBG: {}", object->m_glowColorIsLBG);
        DEBUG_LOG("  - cantColorGlow: {}", object->m_cantColorGlow);
        DEBUG_LOG("  - glowSprite.color: {}", object->m_glowSprite ? object->m_glowSprite->getColor() : ccColor3B {0, 0, 0});
        DEBUG_LOG("  - opacityMod: {}", object->m_opacityMod);
        DEBUG_LOG("  - isDecoration2: {}", object->m_isDecoration2);
        DEBUG_LOG("  - colorSprite: {}", object->m_colorSprite);
        DEBUG_LOG("  - glowSprite: {}", object->m_glowSprite);
        DEBUG_LOG("  - isObjectBlack: {}", object->m_isObjectBlack);
        DEBUG_LOG("  - isObjectChildrenBlack: {}", object->m_isColorSpriteBlack);
        DEBUG_LOG("  - blackChildOpacity: {}", object->m_blackChildOpacity);
        DEBUG_LOG("  - blackChildOpacityLocked: {}", object->m_blackChildOpacityLocked);
        DEBUG_LOG("  - activeDetailColorID: {}", object->m_activeDetailColorID);
        DEBUG_LOG("  - childCount: {}", object->getChildrenCount());
        if (object->m_baseColor && object->m_baseColor->m_usesHSV)
            DEBUG_LOG("  - baseHSV: {} {} {} {} {}", object->m_baseColor->m_hsv.h, object->m_baseColor->m_hsv.s, object->m_baseColor->m_hsv.v, object->m_baseColor->m_hsv.absoluteSaturation, object->m_baseColor->m_hsv.absoluteBrightness);
        if (object->m_detailColor && object->m_detailColor->m_usesHSV)
            DEBUG_LOG("  - detailHSV: {} {} {} {} {}", object->m_detailColor->m_hsv.h, object->m_detailColor->m_hsv.s, object->m_detailColor->m_hsv.v, object->m_detailColor->m_hsv.absoluteSaturation, object->m_detailColor->m_hsv.absoluteBrightness);
        if (object->getHasRotateAction())
            DEBUG_LOG("  - rotationDelta: {}", ((EnhancedGameObject*)object)->m_rotationDelta);

        // Objects from sheets the renderer has no texture for are skipped
        // (this is how they were handled before), but a single one of them
        // must not turn off the renderer for the whole level.
        auto spriteSheet = (SpriteSheet)object->getParentMode();
        if (spriteSheet == SpriteSheet::SPECIAL || spriteSheet == SpriteSheet::_UNK) {
            unsupportedSheetObjectCount++;
            continue;
        }

        sorter.addGameObject(object);
    }

    if (unsupportedSheetObjectCount > 0)
        log::warn("Skipped {} object(s) whose sprite sheet can't be drawn by the renderer", unsupportedSheetObjectCount);

    DEBUG_LOG("m_lightBGColor: {}", layer->m_lightBGColor);

    for (auto it = sorter.iterator(); !it.isEnd(); it.next())
        objects.push_back(it.get());

    log::info("Generating static rendering buffer....");

    renderedGameObjectCount = objects.size();
    
    generateObjectInfoBuffer(objects);

    u32 groupCombCount = groupManager.getGroupCombinationCount();
    log::info("{} group combinations detected", groupCombCount);

    log::info("Generating vertex buffer...");

    for (auto& value : colorChannelsIsBlending)
        value = false;

    fetchBlendingStates();

    for (auto object : objects)
        objectBatch.writeGameObject(object);
    objectBatch.finishWriting();
    generateBatchNodes();

    log::info("Sprite count: {}", objectBatch.getSpriteCount());

    std::map<std::string, std::string> shaderMacroVariables;

    log::info("Compiling shaders...");
    shader = Shader::create("objectDrawing.vert", "objectDrawing.frag", shaderMacroVariables);
    if (!shader)
        return false;

    log::info("Compiled objectDrawing shader");

    cullingComputeShader = Shader::createCompute("objectCulling.comp", shaderMacroVariables);
    if (!cullingComputeShader)
        return false;

    log::info("Compiled objectCulling shader");

    colorChannelBufferObject = Buffer::createDynamicDraw("ColorChannelBuffer", sizeof(ColorChannelBuffer));
    if (!colorChannelBufferObject) {
        log::error("Failed to create the color channel buffer");
        return false;
    }

    colorChannelBuffer = (ColorChannelBuffer*)colorChannelBufferObject->mapWriteOnly();
    if (!colorChannelBuffer) {
        log::error("Failed to map the color channel buffer");
        return false;
    }

    gameStateBuffer = Buffer::createDynamicDraw("GameStateBuffer", sizeof(GameStateBuffer));
    if (!gameStateBuffer) {
        log::error("Failed to create the game state buffer");
        return false;
    }

    debugText = DebugText::create();
    layer->addChild(debugText, 1000);

    if (useOptimizations()) {
        for (auto object : CCArrayExt<GameObject*, false>(layer->m_objects)) {
            if (object && object != layer->m_endPortal)
                object->deactivateObject(true);
        }
    }

    setZOrder(-99999);
    setEnabled(true);

    rendererStartTime = getTime();

    auto finishNode = new FinishNode;
    finishNode->autorelease();
    layer->m_objectLayer->addChild(finishNode, 99999);

    reset();

    if (debugToolsEnabled) {
        Notification::create(
            "Debug tools removes ALL optimizations!\nDisable it for better performance.",
            NotificationIcon::Warning,
            5.0f
        )->show();
    }

    setID("iliashdz.kyanite/renderer");

    log::info("Renderer initialized");
    return true;
}

void Renderer::generateBatchNodes() {
    for (const auto& id : objectBatch.getUsedLayerIds()) {
        auto batchNode = ObjectBatchNode::create(id);
        batchNode->setID(id.asID());

        if (!batchNode) continue;

        if (id.spriteSheet == SpriteSheet::PARTICLE && isSuperLayer(id.zlayer))
            continue;
        
        auto node = getSpriteBatchNodeWithLayerId(id);

        if (!node) continue;

        vanillaBatchNodes.insert(node);

        int zorder = node->getZOrder();

        layer->m_objectLayer->addChild(batchNode, node->getZOrder());
        batchNodes.push_back(batchNode);
    }

    if (layer->m_batchNodes) {
        for (auto node : CCArrayExt<CCNode*, false>(layer->m_batchNodes)) {
            if (auto batchNode = typeinfo_cast<CCSpriteBatchNode*>(node))
                vanillaBatchNodes.insert(batchNode);
        }
    }
}

void Renderer::terminate() {
    if (currentRenderer == this) {
        currentRenderer = nullptr;
        currentRendererLayer = nullptr;
    }

    if (debugText)
        debugText = nullptr;

    batchNodes.clear();
    vanillaBatchNodes.clear();

    log::info("Renderer terminated");
}

void Renderer::prepareShaderUniforms() {
    kmMat4 matrixP;
	kmMat4 matrixMV;
	kmMat4 matrixMVP;
	
	kmGLGetMatrix(KM_GL_PROJECTION, &matrixP);
	kmGLGetMatrix(KM_GL_MODELVIEW, &matrixMV);
	
	kmMat4Multiply(&matrixMVP, &matrixP, &matrixMV);

    (kmMat4&)gameState.u_mvp = matrixMVP;
    gameState.u_timer = gameTimer;
    gameState.u_cameraViewSize = glm::vec2(layer->m_cameraWidth, layer->m_cameraHeight);
    auto winsize = CCDirector::get()->getWinSize();
    gameState.u_winSize = glm::vec2(winsize.width, winsize.height);
    gameState.u_screenRight = CCDirector::get()->getScreenRight();
    gameState.u_cameraUnzoomedX = layer->m_cameraUnzoomedX;
    ccColor3B specialLightBG = GameToolbox::transformColor(layer->m_effectManager->activeColorForIndex(COLOR_CHANNEL_BG), 0.0, -0.2, 0.2);
    gameState.u_specialLightBGColor = ccColor3BToGLM(specialLightBG);

    auto winSize = CCDirector::get()->getWinSize();

    vec2 cameraBottomLeft  = ccPointToGLM(layer->m_objectLayer->convertToNodeSpace({0, 0}));
    vec2 cameraBottomRight = ccPointToGLM(layer->m_objectLayer->convertToNodeSpace({winSize.width, 0}));
    vec2 cameraTopLeft     = ccPointToGLM(layer->m_objectLayer->convertToNodeSpace({0, winSize.height}));

    vec2 cameraRight = cameraBottomRight - cameraBottomLeft;
    vec2 cameraUp    = cameraTopLeft     - cameraBottomLeft;

    gameState.u_cameraPosition = cameraBottomLeft;

    gameState.u_cameraRight = glm::normalize(cameraRight);
    gameState.u_cameraUp    = glm::normalize(cameraUp);

    gameState.u_cameraSize = glm::vec2 { glm::length(cameraRight), glm::length(cameraUp) };

    float flipProgress = layer->m_gameState.m_levelFlipProgress;

    if (layer->m_cameraFlip == -1.0f)
        flipProgress = 1.0f - flipProgress;

    gameState.u_gameStateCameraPositionX = layer->m_gameState.m_cameraPosition.x;
    gameState.u_gameStateCameraZoom = layer->m_gameState.m_cameraZoom;
    gameState.u_winSizeWidth = CCDirector::get()->getWinSize().width;
    gameState.u_levelFlipProgress = flipProgress;

    gameState.u_gameStateFlags = 0;
    if (layer->m_player1->m_isDead)
        gameState.u_gameStateFlags |= GAME_STATE_IS_PLAYER_DEAD;

    if (disableObjectPulse)
        gameState.u_audioScale = 0.5;
    else {
        if (layer->m_skipAudioStep)
            gameState.u_audioScale = FMODAudioEngine::sharedEngine()->getMeteringValue();
        else
            gameState.u_audioScale = layer->m_audioEffectsLayer->m_audioScale;
        
        if (layer->m_isSilent || (layer->m_isPracticeMode && !layer->m_practiceMusicSync))
            gameState.u_audioScale = 0.5;
    }

    gameStateBuffer->write(&gameState, sizeof(gameState));
}

void Renderer::prepareColorChannelBuffer() {
    PROFILER_SCOPE("Renderer::prepareColorChannelBuffer");

    auto effectManager = layer->m_effectManager;

    // The color channel buffer is only uploaded when a color actually changed.
    // Colors stay constant for most frames, so this removes a buffer upload
    // (and its driver overhead) from those frames.
    bool changed = !colorChannelBufferInitialized;
    colorChannelBufferInitialized = true;

    for (usize i = 0; i < effectManager->m_colorActionSpriteVector.size(); i++) {
        auto sprite = effectManager->m_colorActionSpriteVector[i];
        if (sprite == nullptr)
            continue;

        auto id = sprite->m_colorID;

        auto& channelColor = colorChannelBuffer->u_channelColors[id];

        u8 r = sprite->m_copyColor.r;
        u8 g = sprite->m_copyColor.g;
        u8 b = sprite->m_copyColor.b;
        u8 a = (u8)sprite->m_opacity;

        if (channelColor.r == r && channelColor.g == g && channelColor.b == b && channelColor.a == a)
            continue;

        channelColor.r = r;
        channelColor.g = g;
        channelColor.b = b;
        channelColor.a = a;
        changed = true;
    }

    auto& blackChannel = colorChannelBuffer->u_channelColors[COLOR_CHANNEL_BLACK];
    if (blackChannel.r != 0 || blackChannel.g != 0 || blackChannel.b != 0 || blackChannel.a != 255) {
        blackChannel = { 0, 0, 0, 255 };
        changed = true;
    }

    if (changed)
        colorChannelBufferObject->flush(0, colorChannelBufferObject->getSize());
}

void Renderer::generateObjectInfoBuffer(std::vector<GameObject*>& objects) {
    std::vector<ObjectInfo> objectInfos;
    objectInfos.resize(renderedGameObjectCount);

    usize index = 0;

    for (auto object : objects) {
        auto objectInfo = &objectInfos[index];

        if (object->getHasRotateAction())
            objectInfo->rotationSpeed = ((EnhancedGameObject*)object)->m_rotationDelta; // 'rotationDelta' is incorrect naming
        else
            objectInfo->rotationSpeed = 0.0;

        objectInfo->flags = 0;
        if (object->m_usesAudioScale && !object->m_hasNoAudioScale) {
            objectInfo->flags |= OBJECT_FLAG_USES_AUDIO_SCALE;
        
            if (object->m_customAudioScale) {
                objectInfo->flags |= OBJECT_FLAG_CUSTOM_AUDIO_SCALE;
                objectInfo->audioScaleMin = object->m_minAudioScale;
                objectInfo->audioScaleMax = object->m_maxAudioScale;
            }
            
            if (typeinfo_cast<RingObject*>(object))
                objectInfo->flags |= OBJECT_FLAG_IS_ORB;
        }

        objectInfo->baseColorChannelId = object->m_activeMainColorID;
        objectInfo->detailColorChannelId = object->m_activeDetailColorID;

        if (object->m_baseColor && object->m_baseColor->m_usesHSV) {
            objectInfo->flags |= OBJECT_FLAG_HAS_BASE_HSV;
            objectInfo->baseHSV = packHSV(object->m_baseColor->m_hsv);
        }

        if (object->m_detailColor && object->m_detailColor->m_usesHSV) {
            objectInfo->flags |= OBJECT_FLAG_HAS_DETAIL_HSV;
            objectInfo->detailHSV = packHSV(object->m_detailColor->m_hsv);
        }

        float opacity = (object->m_opacityMod2 > 0.0) ? object->m_opacityMod2 : 1.0;
        float glowOpacity = object->m_opacityMod;
        float blackChildOpacity = 1.0f;

        if (object->m_hasCustomChild && !object->m_blackChildOpacityLocked && object->m_blackChildOpacity > 0.0f)
            blackChildOpacity = object->m_blackChildOpacity;

        objectInfo->opacityValues = glm::packUnorm4x8({opacity, blackChildOpacity, glowOpacity, 0});

        if (object->m_cantColorGlow && object->m_glowSprite) {
            auto color = object->m_glowSprite->getColor();

            objectInfo->flags |= OBJECT_FLAG_HAS_STATIC_GLOW_COLOR;
            objectInfo->staticGlowColor = {color.r, color.g, color.b, 255};
        }

        if (object->m_isInvisibleBlock) objectInfo->flags |= OBJECT_FLAG_IS_INVISIBLE_BLOCK;
        if (object->m_customGlowColor)  objectInfo->flags |= OBJECT_FLAG_SPECIAL_GLOW_COLOR;
        if (object->m_objectType == GameObjectType::Solid)
            objectInfo->flags |= OBJECT_FLAG_IS_STATIC_OBJECT;
        objectInfo->fadeMargin = object->m_fadeMargin;

        objectIndicies[object] = index;
        index++;
    }

    objectInfoBuffer = Buffer::createStaticDraw("ObjectInfoBuffer", objectInfos.data(), objectInfos.size() * sizeof(ObjectInfo));

    if (!objectInfoBuffer)
        log::error("Failed to create the object info buffer");
};

void Renderer::draw() {
    PROFILER_SCOPE("Renderer::draw");

    if (!shader || !cullingComputeShader)
        return;

    frameTiming.beginFrame();

    {
        ScopedFrameTimer timer(frameTiming.colorChannels);

        fetchBlendingStates();
        prepareColorChannelBuffer();
    }

    {
        ScopedFrameTimer timer(frameTiming.uniforms);

        prepareShaderUniforms();
    }

    if (!isPaused()) {
        {
            ScopedFrameTimer timer(frameTiming.groupStates);

            groupManager.updateGroupStateBuffer();
        }

        {
            ScopedFrameTimer timer(frameTiming.culling);

            objectBatch.predraw();
        }
    }

    renderingTimer.start();

    // The layer nodes draw between here and finish(), which is where the
    // drawing time is accumulated. Timing every layer draw separately cost a
    // pair of clock reads per layer for a number that only matters as a sum.
    drawStartTime = getTime();
}

void Renderer::finish() {
    renderingTimer.stop();

    if (drawStartTime != 0) {
        frameTiming.draw += getTime() - drawStartTime;
        drawStartTime = 0;
    }

    updateDebugText();
    frameTiming.endFrame();
}

void Renderer::updateDebugText() {
    if (!debugText)
        return;

    std::string text = "";

    if (!enabled) {
        if (differenceModeEnabled) {
            text += "Difference mode is enabled (F9 to disable)\n";
        } else
            text += "Kyanite renderer is disabled (F8 to enable)\n";
    } else {
        if (debugTextEnabled) {
            auto screenSize = CCDirector::get()->getWinSizeInPixels();

            text += fmt::format("Kyanite renderer {}\n", Mod::get()->getVersion().toVString());
            text += fmt::format("OpenGL {}\n", (const char*)glGetString(GL_VERSION));
            text += fmt::format("{}\n", (const char*)glGetString(GL_RENDERER));
            text += "\n";
            text += "Press F3 to hide this screen\n";
        }
    }

    debugText->setText(text);
}

void Renderer::bindCullingComputeShaderBuffers() {
    gameStateBuffer->bindAsUniformBuffer(GAME_STATE_BUFFER_BINDING);
    objectInfoBuffer->bindAsStorageBuffer(OBJECT_INFO_BUFFER_BINDING);
    colorChannelBufferObject->bindAsStorageBuffer(COLOR_CHANNEL_BUFFER_BINDING);
    groupManager.bindGroupStateBuffers();
}

void Renderer::fetchBlendingStates() {
    for (int i = 0; i < colorChannelsIsBlending.size(); i++) {
        auto vec = layer->m_effectManager->m_colorActionVector[i];
        if (vec)
            colorChannelsIsBlending[i] = vec->m_blending;
    }

    colorChannelsIsBlending[COLOR_CHANNEL_P1]  = true;
    colorChannelsIsBlending[COLOR_CHANNEL_P2]  = true;
    colorChannelsIsBlending[COLOR_CHANNEL_LBG] = true;
}

void Renderer::update(float dt) {
    gameTimer += dt;

    if (debugText && debugText->isVisible())
        updateDebugText();

    auto& gstate = layer->m_gameState;
    cameraCenterPos = ccPointToGLM(gstate.m_cameraPosition + CCPoint(layer->m_cameraWidth * 0.5, layer->m_cameraHeight * 0.5));
}

CCSpriteBatchNode* Renderer::getSpriteBatchNodeWithLayerId(LayerKey id) {
    CCNode* node = layer->parentForZLayer((i32)id.zlayer, id.blending, (i32)id.spriteSheet, false);
    
    if (!node || id.spriteSheet == SpriteSheet::SPECIAL || id.spriteSheet == SpriteSheet::_UNK)
        return nullptr;

    return (CCSpriteBatchNode*)node;
}

Ref<Renderer> Renderer::create(PlayLayer* layer, CCArray* objects) {
    auto ren = new Renderer;

    if (ren->init(layer, objects)) {
        ren->autorelease();
        return ren;
    }
    
    log::error("Failed to initialize renderer");
    delete ren;
    return nullptr;
}

Ref<Renderer> Renderer::get() {
    return currentRenderer;
}

bool Renderer::useOptimizations() {
    return !isDebugToolsEnabled();
}

void Renderer::setEnabled(bool enabled) {
    if (useOptimizations() && !enabled)
        return;

    this->enabled = enabled;
    setVisible(enabled);
    for (auto node : batchNodes)
        node->setVisible(enabled);
    for (auto batch : vanillaBatchNodes)
        batch->setVisible(!enabled);

    if (useOptimizations()) {
        // For checkpoints
        layer->m_game2LayerT1->setVisible(true);
        // For end portal
        layer->m_gameBlendingLayerT1->setVisible(true);
        layer->m_gameLayerT1->setVisible(true);
    }

    updateDebugText();
}

void Renderer::reset() {
    gameTimer = 0.0;
    drawStartTime = 0;
    groupManager.clearCheckpoints();
    groupManager.resetGroupStates();
}

DECLARE_HOTKEY(KEY_F3, {
    if (currentRenderer)
        currentRenderer->toggleDebugText();
})

DECLARE_HOTKEY(KEY_F8, {
    if (currentRenderer)
        currentRenderer->setEnabled(!currentRenderer->isEnabled());
})

DECLARE_HOTKEY(KEY_F9, {
    if (currentRenderer)
        currentRenderer->setDifferenceModeEnabled(!currentRenderer->isDifferenceModeEnabled());
})