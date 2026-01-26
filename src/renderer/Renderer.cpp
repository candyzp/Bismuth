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
#include "VisibilityManager.hpp"
#include "ccTypes.h"
#include "common.hpp"
#include "glm/fwd.hpp"
#include "math/ConvexPolygon.hpp"
#include "math/Line.hpp"
#include <Geode/Enums.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/RingObject.hpp>
#include <BProfiler.hpp>
#include "profiler.hpp"

using namespace geode::prelude;

static Renderer* currentRenderer;

Renderer::~Renderer() { terminate(); }

static std::string byteSizeToString(usize size) {
    std::string unit = "B";
    double dsize = size;
    if (dsize > 1000.0) { dsize /= 1000.0; unit = "kB"; }
    if (dsize > 1000.0) { dsize /= 1000.0; unit = "MB"; }
    return fmt::format("{:.3f}{}", dsize, unit);
}

bool Renderer::init(PlayLayer* layer) {
    if (currentRenderer)
        return false;
    currentRenderer = this;

    this->layer = layer;
    
    auto size = CCDirector::get()->getWinSize();

    if (!Mod::get()->getSettingValue<bool>("enabled"))
        return false;

    ingameEnableDisable = Mod::get()->getSettingValue<bool>("ingame_enable");
    useIndexCulling     = Mod::get()->getSettingValue<bool>("index_culling");

    log::info("OpenGL Version: {}", (const char*)glGetString(GL_VERSION));

    auto tcache = cocos2d::CCTextureCache::get();
    // TODO: Add text sheet
    spriteSheets[(i32)SpriteSheet::GAME_1]   = tcache->addImage("GJ_GameSheet.png", false);
    spriteSheets[(i32)SpriteSheet::GAME_2]   = tcache->addImage("GJ_GameSheet02.png", false);
    spriteSheets[(i32)SpriteSheet::PARTICLE] = tcache->addImage("GJ_ParticleSheet.png", false);
    spriteSheets[(i32)SpriteSheet::GLOW]     = tcache->addImage("GJ_GameSheetGlow.png", false);
    spriteSheets[(i32)SpriteSheet::FIRE]     = tcache->addImage("FireSheet_01.png", false);
    spriteSheets[(i32)SpriteSheet::PIXEL]    = tcache->addImage("PixelSheet_01.png", false);

    SpriteMeshDictionary::loadFromFile("spriteMeshes.json");

    groupManager.initWithObjects(layer->m_objects);

    ObjectSorter sorter;

    sorter.initForGameLayer(layer);
    log::info("Level contains {} object(s)", layer->m_objects->count());
    log::info("Sorting objects...");
    for (auto object : CCArrayExt<GameObject*>(layer->m_objects)) {
        if (object == layer->m_anticheatSpike) {
            DEBUG_LOG("- anti-cheat spike");
            continue;
        }

        if (object->isTrigger() || object->m_isHide)
            continue;

        DEBUG_LOG("- {}, id: {}", (void*)object, object->m_objectID);
        DEBUG_LOG("  - zlayer: {}", (i32)object->getObjectZLayer());
        DEBUG_LOG("  - blending: {}", object->m_baseOrDetailBlending);
        DEBUG_LOG("  - spritesheet: {}", object->getParentMode());
        DEBUG_LOG("  - glowColorIsLBG: {}", object->m_glowColorIsLBG);
        DEBUG_LOG("  - customGlowColor: {}", object->m_customGlowColor);
        DEBUG_LOG("  - opacityMod: {}", object->m_opacityMod);
        DEBUG_LOG("  - isDecoration2: {}", object->m_isDecoration2);
        if (object->m_baseColor && object->m_baseColor->m_usesHSV)
            DEBUG_LOG("  - baseHSV: {} {} {} {} {}", object->m_baseColor->m_hsv.h, object->m_baseColor->m_hsv.s, object->m_baseColor->m_hsv.v, object->m_baseColor->m_hsv.absoluteSaturation, object->m_baseColor->m_hsv.absoluteBrightness);
        if (object->m_detailColor && object->m_detailColor->m_usesHSV)
            DEBUG_LOG("  - detailHSV: {} {} {} {} {}", object->m_detailColor->m_hsv.h, object->m_detailColor->m_hsv.s, object->m_detailColor->m_hsv.v, object->m_detailColor->m_hsv.absoluteSaturation, object->m_detailColor->m_hsv.absoluteBrightness);
        if (object->getHasRotateAction())
            DEBUG_LOG("  - rotationDelta: {}", ((EnhancedGameObject*)object)->m_rotationDelta);

        sorter.addGameObject(object);
    }
    sorter.finalizeSorting();

    log::info("Generating static rendering buffer....");

    renderedGameObjectCount = 0;
    for (auto it = sorter.iterator(); !it.isEnd(); it.next())
        renderedGameObjectCount++;
    
    generateStaticRenderingBuffer(sorter);

    u32 groupCombCount = groupManager.getGroupCombinationCount();
    log::info("{} group combinations detected", groupCombCount);

    log::info("Generating vertex buffer...");

    for (auto it = sorter.iterator(); !it.isEnd(); it.next())
        objectBatch.writeGameObject(it.get());
    objectBatch.finishWriting();
    generateBatchNodes();

    log::info("Compiling shaders...");

    std::map<std::string, std::string> shaderMacroVariables;

    shaderMacroVariables["TOTAL_OBJECT_COUNT"] = std::to_string(renderedGameObjectCount == 0 ? 1 : renderedGameObjectCount);
    shaderMacroVariables["GROUP_ID_LIMIT"]     = std::to_string(groupCombCount);

    shader = Shader::create("object.vert", "object.frag", shaderMacroVariables);
    if (!shader)
        return false;

    colorChannelBufferObject = Buffer::createDynamicDraw("Color channel buffer", sizeof(ColorChannelBuffer));
    if (!colorChannelBufferObject)
        return false;
    colorChannelBuffer = (ColorChannelBuffer*)colorChannelBufferObject->mapWriteOnly();

    uniformBuffer = Buffer::createDynamicDraw("Uniform buffer", sizeof(RendererUniformBuffer));
    if (!uniformBuffer)
        return false;

    debugText = CCLabelBMFont::create("", "chatFont.fnt");
    debugTextOutline1 = CCLabelBMFont::create("", "chatFont.fnt");
    debugTextOutline2 = CCLabelBMFont::create("", "chatFont.fnt");

    debugTextOutline1->setColor({0, 0, 0});
    debugTextOutline1->setOpacity(200);
    debugTextOutline2->setColor({0, 0, 0});
    debugTextOutline2->setOpacity(200);

    debugText->setAnchorPoint(CCPoint(0, 1));
    debugText->setPosition(1, CCDirector::get()->getWinSize().height - 8);
    debugText->setScale(0.5);
    layer->addChild(debugText, 1000);

    debugTextOutline1->setAnchorPoint(CCPoint(0, 1));
    debugTextOutline1->setPosition(1 - 0.5, CCDirector::get()->getWinSize().height - 8 - 0.5);
    debugTextOutline1->setScale(0.5);
    layer->addChild(debugTextOutline1, 999);

    debugTextOutline2->setAnchorPoint(CCPoint(0, 1));
    debugTextOutline2->setPosition(1 + 0.5, CCDirector::get()->getWinSize().height - 8 + 0.5);
    debugTextOutline2->setScale(0.5);
    layer->addChild(debugTextOutline2, 999);

    setZOrder(-2);
    setEnabled(true);

    rendererStartTime = getTime();

    reset();

    log::info("Renderer initialized");
    return true;
}

void Renderer::generateBatchNodes() {
    for (const auto& id : objectBatch.getUsedLayerIds()) {
        auto batchNode = ObjectBatchNode::create(id);
        if (batchNode) {
            auto node = getSpriteBatchNodeWithLayerId(id);

            layer->m_objectLayer->addChild(batchNode, node->getZOrder());
            batchNodes.push_back(batchNode);
        }
    }
}

void Renderer::terminate() {
    if (shader)
        Shader::destroy(shader);
    shader = nullptr;

    if (basicShader)
        Shader::destroy(basicShader);
    basicShader = nullptr;

    if (colorChannelBufferObject) {
        colorChannelBufferObject->unmap();
        Buffer::destroy(colorChannelBufferObject);
    }

    if (srbBuffer)
        Buffer::destroy(srbBuffer);

    if (uniformBuffer)
        Buffer::destroy(uniformBuffer);

    currentRenderer = nullptr;
    log::info("Renderer terminated");
}

void Renderer::prepareShaderUniforms() {
    kmMat4 matrixP;
	kmMat4 matrixMV;
	kmMat4 matrixMVP;
	
	kmGLGetMatrix(KM_GL_PROJECTION, &matrixP);
	kmGLGetMatrix(KM_GL_MODELVIEW, &matrixMV);
	
	kmMat4Multiply(&matrixMVP, &matrixP, &matrixMV);

    // shader->setTextureArray("u_spriteSheets", (i32)SpriteSheet::COUNT, spriteSheets);

    (kmMat4&)uniforms.u_mvp = matrixMVP;
    uniforms.u_timer = gameTimer;
    uniforms.u_cameraPosition = ccPointToGLM(layer->m_gameState.m_cameraPosition2);
    uniforms.u_cameraViewSize = glm::vec2(layer->m_cameraWidth, layer->m_cameraHeight);
    auto winsize = CCDirector::get()->getWinSize();
    uniforms.u_winSize = glm::vec2(winsize.width, winsize.height);
    uniforms.u_screenRight = CCDirector::get()->getScreenRight();
    uniforms.u_cameraUnzoomedX = layer->m_cameraUnzoomedX;
    ccColor3B specialLightBG = GameToolbox::transformColor(layer->m_effectManager->activeColorForIndex(COLOR_CHANNEL_BG), 0.0, -0.2, 0.2);
    uniforms.u_specialLightBGColor = ccColor3BToGLM(specialLightBG);

    uniforms.u_gameStateFlags = 0;
    if (layer->m_player1->m_isDead)
        uniforms.u_gameStateFlags |= GAME_STATE_IS_PLAYER_DEAD;

    if (layer->m_skipAudioStep)
        uniforms.u_audioScale = FMODAudioEngine::sharedEngine()->getMeteringValue();
    else
        uniforms.u_audioScale = layer->m_audioEffectsLayer->m_audioScale;
    
    if (layer->m_isSilent || (layer->m_isPracticeMode && !layer->m_practiceMusicSync))
        uniforms.u_audioScale = 0.5;

    uniformBuffer->write(&uniforms, sizeof(RendererUniformBuffer));
    uniformBuffer->bindAsUniformBuffer(RENDERER_UNIFORM_BUFFER_BINDING);
}

void Renderer::prepareColorChannelBuffer() {
    auto timer = BProfiler::start("Generate color channel buffer");

    auto effectManager = layer->m_effectManager;

    for (usize i = 0; i < effectManager->m_colorActionSpriteVector.size(); i++) {
        auto sprite = effectManager->m_colorActionSpriteVector[i];
        if (sprite == nullptr)
            continue;

        auto id = sprite->m_colorID;

        auto& channelColor = colorChannelBuffer->u_channelColors[id];
        channelColor.r = sprite->m_color.r;
        channelColor.g = sprite->m_color.g;
        channelColor.b = sprite->m_color.b;
        channelColor.a = (u8)sprite->m_opacity;
    }

    colorChannelBuffer->u_channelColors[COLOR_CHANNEL_BLACK] = { 0, 0, 0, 255 };

    timer.end();
}

static u32 convertToShaderHSV(const ccHSVValue& hsv) {
    u32 hue = hsv.h + 256.f;
    u32 sat = ( hsv.s + (hsv.absoluteSaturation ? 1.0 : 0.0) ) * 127.5f;
    u32 val = ( hsv.v + (hsv.absoluteBrightness ? 1.0 : 0.0) ) * 127.5f;

    u32 ret = (hue & HSV_HUE_MASK) << HSV_HUE_BIT |
              (sat & HSV_SAT_MASK) << HSV_SAT_BIT |
              (val & HSV_VAL_MASK) << HSV_VAL_BIT;

    if (hsv.absoluteSaturation) ret |= HSV_SAT_ADD;
    if (hsv.absoluteBrightness) ret |= HSV_VAL_ADD;
    return ret;
}

void Renderer::generateStaticRenderingBuffer(ObjectSorter& sorter) {
    std::vector<StaticObjectInfo> objectInfos;
    objectInfos.resize(renderedGameObjectCount);

    usize index = 0;

    for (auto it = sorter.iterator(); !it.isEnd(); it.next()) {
        auto object = it.get();
        auto objectInfo = &objectInfos[index];

        objectInfo->startPosition = ccPointToGLM(object->m_startPosition);
        if (object->getHasRotateAction())
            objectInfo->rotationSpeed = ((EnhancedGameObject*)object)->m_rotationDelta; // 'rotationDelta' is incorrect naming
        else
            objectInfo->rotationSpeed = 0.0;

        objectInfo->flags = 0;
        if (object->m_usesAudioScale) {
            objectInfo->flags |= OBJECT_FLAG_USES_AUDIO_SCALE;
        
            if (object->m_customAudioScale) {
                objectInfo->flags |= OBJECT_FLAG_CUSTOM_AUDIO_SCALE;
                objectInfo->audioScaleMin = object->m_minAudioScale;
                objectInfo->audioScaleMax = object->m_maxAudioScale;
            }
            
            if (typeinfo_cast<RingObject*>(object))
                objectInfo->flags |= OBJECT_FLAG_IS_ORB;
        }

        if (object->m_baseColor && object->m_baseColor->m_usesHSV) {
            objectInfo->flags |= OBJECT_FLAG_HAS_BASE_HSV;
            objectInfo->baseHSV = convertToShaderHSV(object->m_baseColor->m_hsv);
        }

        if (object->m_detailColor && object->m_detailColor->m_usesHSV) {
            objectInfo->flags |= OBJECT_FLAG_HAS_DETAIL_HSV;
            objectInfo->detailHSV = convertToShaderHSV(object->m_detailColor->m_hsv);
        }

        objectInfo->opacity = (object->m_opacityMod2 > 0.0) ? object->m_opacityMod2 : 1.0;

        objectInfo->groupCombinationIndex = groupManager.getGroupCombinationIndexForObject(object);

        if (object->m_isInvisibleBlock) objectInfo->flags |= OBJECT_FLAG_IS_INVISIBLE_BLOCK;
        if (object->m_customGlowColor)  objectInfo->flags |= OBJECT_FLAG_SPECIAL_GLOW_COLOR;
        if (object->m_objectType == GameObjectType::Solid)
            objectInfo->flags |= OBJECT_FLAG_IS_STATIC_OBJECT;
        objectInfo->fadeMargin = object->m_fadeMargin;

        objectSRBIndicies[object] = index;
        index++;
    }

    srbBuffer = Buffer::createStaticDraw("Static rendering buffer", objectInfos.data(), objectInfos.size() * sizeof(StaticObjectInfo));
};

static glm::vec2 linePoint = { 100, 200 };
static float     lineAngle = 0;

static ConvexPolygon polygon;

static isize triangleIndex = 0;

static void drawLine(Renderer* ren, const Line& line, const glm::vec4& color) {
    glm::vec2 npp = { line.normal.y, -line.normal.x };

    glm::vec2 p1 = line.firstPoint() + npp *  10000.f;
    glm::vec2 p2 = line.firstPoint() + npp * -10000.f;

    ren->drawLine(p1, p2, color);
}

static void drawCross(Renderer* ren, const glm::vec2& point, const glm::vec4& color) {
    ren->drawLine(point + glm::vec2(-5, -5), point + glm::vec2( 5,  5), color);
    ren->drawLine(point + glm::vec2(-5,  5), point + glm::vec2( 5, -5), color);
}

float CAMERA_CULL_RECT_SCALE = 1.3;

void Renderer::draw() {
    profiler::functionPush("Renderer::draw");

    auto timer = BProfiler::start("Renderer::draw");

    u64 prevTime = getTime();
    storeGLStates();
    prepareShaderUniforms();
    if (!isPaused()) {
        prepareColorChannelBuffer();
    
        auto timer = BProfiler::start("Generate group state buffer");
        groupManager.prepareGroupStateBuffer();
        timer.end();

        CameraView view = {
            ccPointToGLM(layer->m_gameState.m_cameraPosition2),
            glm::vec2(layer->m_cameraWidth, 0),
            glm::vec2(0, layer->m_cameraHeight)
        };

        view.bottomLeft  += (view.rightVector + view.upVector) * 0.5f;
        view.rightVector *= CAMERA_CULL_RECT_SCALE;
        view.upVector    *= CAMERA_CULL_RECT_SCALE;
        view.bottomLeft  -= (view.rightVector + view.upVector) * 0.5f;

        // glm::vec2 bl = view.bottomLeft;
        // glm::vec2 br = view.bottomLeft + view.rightVector;
        // glm::vec2 tl = view.bottomLeft + view.upVector;
        // glm::vec2 tr = view.bottomLeft + view.rightVector + view.upVector;

        // drawLine(bl, br, glm::vec4(1, 1, 0, 1));
        // drawLine(br, tr, glm::vec4(1, 1, 0, 1));
        // drawLine(tr, tl, glm::vec4(1, 1, 0, 1));
        // drawLine(tl, bl, glm::vec4(1, 1, 0, 1));

        objectBatch.predraw(view);
    }

    timer.end();

    /*
    glm::vec2 normal { glm::cos(glm::radians(lineAngle)), glm::sin(glm::radians(lineAngle)) };

    drawLine(linePoint, linePoint + normal * 10.f, glm::vec4(0, 1, 0, 1));

    Line line { linePoint, normal };

    ::drawLine(this, line, glm::vec4(1, 0, 0, 1));

    for (const auto& mline : polygon.getLines()) {
        ::drawLine(this, mline, glm::vec4(0.5, 0.5, 0.5, 1));
    
        auto inter = line.intersectionWith(mline);
        if (inter)
            drawCross(this, inter.value(), glm::vec4(0, 0, 1, 1));
    }

    std::vector<glm::vec2> verticies;

    polygon.triangulate([&](const auto& p1, const auto& p2, const auto& p3) {
        verticies.push_back(p1);
        verticies.push_back(p2);
        verticies.push_back(p3);
    });

    if (triangleIndex >= (verticies.size() / 3))
        triangleIndex = 0;

    if (verticies.size() != 0) {
        auto p1 = verticies[triangleIndex * 3 + 0];
        auto p2 = verticies[triangleIndex * 3 + 1];
        auto p3 = verticies[triangleIndex * 3 + 2];

        drawLine(p1, p2, glm::vec4(0, 1, 0, 1));
        drawLine(p2, p3, glm::vec4(0, 1, 0, 1));
        drawLine(p3, p1, glm::vec4(0, 1, 0, 1));
    }
    */

    restoreGLStates();

    /*
    
    if (debugTextEnabled) {
        // glBeginQuery(GL_TIME_ELAPSED, 50);
    }

    prepareDraw();

    spritesOnScreen = objectBatch.draw();

    finishDraw();

    restoreGLStates();
    
    if (debugTextEnabled) {
        renderTime = 0;
        // glEndQuery(GL_TIME_ELAPSED);
        // glGetQueryObjecti64v(50, GL_QUERY_RESULT, &renderTime);
    }
    */

    if (debugText->isVisible())
        updateDebugText();

    profiler::functionPop();
}

void Renderer::updateDebugText() {
    std::string text = "";

    text += overdrawView.getDebugText();

    if (!enabled) {
        text += "Bismuth renderer is disabled\n";
        text += "Press F8 to enable\n";
    } else {
        if (debugTextEnabled) {
            auto screenSize = CCDirector::get()->getWinSizeInPixels();

            text += fmt::format("Bismuth renderer {}\n", Mod::get()->getVersion().toVString());
            text += fmt::format("OpenGL {}\n", (const char*)glGetString(GL_VERSION));
            text += fmt::format("{}\n", (const char*)glGetString(GL_RENDERER));
            text += fmt::format("Window: {}x{}\n", screenSize.width, screenSize.height);
            text += fmt::format("Render time: {}ms\n", (double)renderTime / 1000000.0);
            text += fmt::format("Vertex buffer size: {}\n", byteSizeToString(objectBatch.getVertexBufferSize()));
            text += fmt::format("Sprites on screen: {}\n", spritesOnScreen);
            text += fmt::format("Static rendering buffer size: {}\n", byteSizeToString(srbBuffer->getSize()));
            text += fmt::format("Group state buffer size: {}\n", byteSizeToString(groupManager.getGroupStateBufferSize()));
            text += fmt::format("Color channel buffer size: {}\n", byteSizeToString(sizeof(ColorChannelBuffer)));
            text += "\n";
            text += BProfiler::toString();
            text += "\n";
            text += "Press F3 to hide this screen\n";
        } else if (differenceModeEnabled)
            text += "_\n";
    }

    debugText->setString(text.c_str());
    debugTextOutline1->setString(text.c_str());
    debugTextOutline2->setString(text.c_str());
}

Shader* Renderer::prepareDraw() {
    storeGLStates();

    shader->use();

    uniformBuffer->bindAsUniformBuffer(RENDERER_UNIFORM_BUFFER_BINDING);
    srbBuffer->bindAsStorageBuffer(STATIC_RENDERING_BUFFER_BINDING);
    colorChannelBufferObject->bindAsStorageBuffer(COLOR_CHANNEL_BUFFER_BINDING);
    groupManager.bindGroupStateBuffer();

    return shader;
}

void Renderer::finishDraw() {
    restoreGLStates();
}

void Renderer::update(float dt) {
    gameTimer += dt;
    
    float audioScale;
    if (layer->m_skipAudioStep)
        audioScale = FMODAudioEngine::sharedEngine()->getMeteringValue();
    else
        audioScale = layer->m_audioEffectsLayer->m_audioScale;
    
    if (layer->m_isSilent || (layer->m_isPracticeMode && !layer->m_practiceMusicSync))
        audioScale = 0.5;

    if (debugText->isVisible())
        updateDebugText();

    auto& gstate = layer->m_gameState;
    cameraCenterPos = ccPointToGLM(gstate.m_cameraPosition2 + CCPoint(layer->m_cameraWidth * 0.5, layer->m_cameraHeight * 0.5));
}

bool Renderer::isColorChannelBlending(i32 channel) {
    if (
        channel == COLOR_CHANNEL_P1 ||
        channel == COLOR_CHANNEL_P2 ||
        channel == COLOR_CHANNEL_LBG
    ) {
        return true;
    }

    return layer->shouldBlend(channel);
}

CCSpriteBatchNode* Renderer::getSpriteBatchNodeWithLayerId(LayerKey id) {
    CCNode* node = layer->parentForZLayer((i32)id.zlayer, id.blending, (i32)id.spriteSheet, false);
    
    if (!node || layer->m_batchNodes->indexOfObject(node) == UINT_MAX)
        return nullptr;

    return (CCSpriteBatchNode*)node;
}

Ref<Renderer> Renderer::create(PlayLayer* layer) {
    auto ren = new Renderer;

    if (ren->init(layer)) {
        ren->autorelease();
        return ren;
    }
    
    delete ren;
    return nullptr;
}

Ref<Renderer> Renderer::get() {
    return currentRenderer;
}

bool Renderer::useOptimizations() {
    if (canEnableDisableIngame())
        return false;

    return true;
}

void Renderer::setEnabled(bool enabled) {
    this->enabled = enabled;
    this->setVisible(enabled);
    for (auto node : batchNodes)
        node->setVisible(enabled);
    for (auto batch : CCArrayExt<CCNode*>(layer->m_batchNodes))
        batch->setVisible(!enabled);
    updateDebugText();
}

void Renderer::reset() {
    groupManager.resetGroupStates();
}

void Renderer::drawLine(const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color) {
    if (!basicShader)
        basicShader = Shader::create("basic.vert", "basic.frag");
    if (!basicShader)
        return;

    glm::vec2 data[2] = { p1, p2 };
    Buffer* buffer = Buffer::createStaticDraw("Line buffer", &data, sizeof(data));
    if (!buffer) return;
    buffer->bindAs(GL_ARRAY_BUFFER);

    u32 vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), 0);
    glEnableVertexAttribArray(0);

    kmMat4 matrixP;
	kmMat4 matrixMV;
	kmMat4 matrixMVP;
	kmGLGetMatrix(KM_GL_PROJECTION, &matrixP);
	kmGLGetMatrix(KM_GL_MODELVIEW, &matrixMV);
	kmMat4Multiply(&matrixMVP, &matrixP, &matrixMV);

    basicShader->use();
    basicShader->setMatrix4("u_mvp", matrixMVP.mat);
    basicShader->setVec4("u_color", color);

    glDrawArrays(GL_LINES, 0, 2);

    glDeleteVertexArrays(1, &vao);
    Buffer::destroy(buffer);
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

DECLARE_HOTKEY(KEY_F6, {
    BProfiler::useAverages(!BProfiler::isUsingAverages()); 
});

static i32 storedVAO, storedVBO, storedIBO, storedProgram;
static i32 storedBlendSrcAlpha, storedBlendSrcRGB;
static i32 storedBlendDstAlpha, storedBlendDstRGB;

static i32 storedTextures[9];

void storeGLStates() {
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &storedVAO);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &storedVBO);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &storedIBO);
    glGetIntegerv(GL_CURRENT_PROGRAM, &storedProgram);
    glGetIntegerv(GL_CURRENT_PROGRAM, &storedProgram);
    
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &storedBlendSrcAlpha);
    glGetIntegerv(GL_BLEND_SRC_RGB,   &storedBlendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &storedBlendDstAlpha);
    glGetIntegerv(GL_BLEND_DST_RGB,   &storedBlendDstRGB);

    for (int i = 0; i < 9; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, storedTextures + i);
    }
    glActiveTexture(GL_TEXTURE0);
}

void restoreGLStates() {
    glBindVertexArray(storedVAO);
    glBindBuffer(GL_ARRAY_BUFFER, storedVBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, storedIBO);
    glUseProgram(storedProgram);

    glBlendFuncSeparate(
        storedBlendSrcRGB,
        storedBlendDstRGB,
        storedBlendSrcAlpha,
        storedBlendDstAlpha
    );

    for (int i = 0; i < 9; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, storedTextures[i]);
    }
    glActiveTexture(GL_TEXTURE0);
}