#if defined(GEODE_IS_IOS)

#include "IOSMetalRenderer.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/cocos/CCDirector.h>
#include <Geode/cocos/textures/CCTextureCache.h>
#include <Geode/utils/cocos.hpp>
#include <Geode/binding/GameManager.hpp>
#include "../SpriteMeshDictionary.hpp"
#include <common.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <Geode/cocos/kazmath/include/kazmath/mat4.h>
#include <OpenGLES/ES2/gl.h>
#include <OpenGLES/ES2/glext.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

using namespace geode::prelude;

namespace BismuthIOSMetal {

Renderer* Renderer::current = nullptr;

namespace {

constexpr std::uint32_t IOS_COLOR_CHANNEL_BLACK = 1010u;
constexpr std::uint32_t IOS_COLOR_CHANNEL_P1 = 1005u;
constexpr std::uint32_t IOS_COLOR_CHANNEL_P2 = 1006u;
constexpr std::uint32_t IOS_COLOR_CHANNEL_LBG = 1007u;
constexpr std::uint32_t IOS_COLOR_CHANNEL_BG = 1000u;

class GLESStateSnapshot {
public:
    static constexpr GLuint ATTRIBUTE_COUNT = 3;
    static constexpr GLint TEXTURE_UNIT_COUNT = 2;

    GLESStateSnapshot() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        blendEnabled = glIsEnabled(GL_BLEND);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEquationRGB);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEquationAlpha);

        for (GLint unit = 0; unit < TEXTURE_UNIT_COUNT; ++unit) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBindings[unit]);
        }
        glActiveTexture(static_cast<GLenum>(activeTexture));

        for (GLuint index = 0; index < ATTRIBUTE_COUNT; ++index) {
            auto& attribute = attributes[index];
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attribute.enabled);
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attribute.size);
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attribute.type);
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attribute.normalized);
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attribute.stride);
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attribute.buffer);
            glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attribute.pointer);
        }
    }

    ~GLESStateSnapshot() {
        restore();
    }

    void restore() {
        if (restored)
            return;

        glUseProgram(static_cast<GLuint>(program));
        for (GLuint index = 0; index < ATTRIBUTE_COUNT; ++index) {
            const auto& attribute = attributes[index];
            glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(attribute.buffer));
            glVertexAttribPointer(
                index, attribute.size, static_cast<GLenum>(attribute.type),
                static_cast<GLboolean>(attribute.normalized),
                attribute.stride, attribute.pointer
            );
            if (attribute.enabled)
                glEnableVertexAttribArray(index);
            else
                glDisableVertexAttribArray(index);
        }
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));

        for (GLint unit = 0; unit < TEXTURE_UNIT_COUNT; ++unit) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(textureBindings[unit]));
        }
        glActiveTexture(static_cast<GLenum>(activeTexture));

        if (blendEnabled)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
        glBlendEquationSeparate(
            static_cast<GLenum>(blendEquationRGB),
            static_cast<GLenum>(blendEquationAlpha)
        );
        glBlendFuncSeparate(
            static_cast<GLenum>(blendSrcRGB),
            static_cast<GLenum>(blendDstRGB),
            static_cast<GLenum>(blendSrcAlpha),
            static_cast<GLenum>(blendDstAlpha)
        );
        restored = true;
    }

private:
    struct Attribute {
        GLint enabled = GL_FALSE;
        GLint size = 4;
        GLint type = GL_FLOAT;
        GLint normalized = GL_FALSE;
        GLint stride = 0;
        GLint buffer = 0;
        void* pointer = nullptr;
    };

    GLint program = 0;
    GLint arrayBuffer = 0;
    GLint activeTexture = GL_TEXTURE0;
    GLint blendSrcRGB = GL_ONE;
    GLint blendDstRGB = GL_ZERO;
    GLint blendSrcAlpha = GL_ONE;
    GLint blendDstAlpha = GL_ZERO;
    GLint blendEquationRGB = GL_FUNC_ADD;
    GLint blendEquationAlpha = GL_FUNC_ADD;
    GLboolean blendEnabled = GL_FALSE;
    GLint textureBindings[TEXTURE_UNIT_COUNT] {};
    Attribute attributes[ATTRIBUTE_COUNT] {};
    bool restored = false;
};

GLuint compileShader(GLenum type, const std::string& source, std::string& error) {
    GLuint shader = glCreateShader(type);
    if (!shader) {
        error = "glCreateShader returned zero";
        return 0;
    }

    const GLchar* sourcePointer = source.c_str();
    const GLint sourceLength = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &sourcePointer, &sourceLength);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
        return shader;

    GLint logLength = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<std::size_t>(std::max(1, logLength)), '\0');
    GLsizei written = 0;
    glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), &written, log.data());
    error = type == GL_VERTEX_SHADER
        ? "iOS Metal-state vertex shader compilation failed: "
        : "iOS Metal-state fragment shader compilation failed: ";
    error.append(log.data(), static_cast<std::size_t>(std::max<GLsizei>(0, written)));
    glDeleteShader(shader);
    return 0;
}

class LayerDrawNode final : public cocos2d::CCNode {
public:
    explicit LayerDrawNode(LayerKey key) : key(key) {}
    void draw() override {
        auto renderer = Renderer::getCurrent();
        if (renderer)
            renderer->drawLayer(key);
    }
private:
    LayerKey key {};
};

class FinishDrawNode final : public cocos2d::CCNode {
public:
    void draw() override {
        auto renderer = Renderer::getCurrent();
        if (renderer)
            renderer->finishDrawFrame();
    }
};

double elapsedMilliseconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start
    ).count();
}

std::uint32_t packRGBA(cocos2d::ccColor3B color, std::uint8_t alpha) {
    return static_cast<std::uint32_t>(color.r) |
        (static_cast<std::uint32_t>(color.g) << 8) |
        (static_cast<std::uint32_t>(color.b) << 16) |
        (static_cast<std::uint32_t>(alpha) << 24);
}

} // namespace

Renderer* Renderer::create(PlayLayer* playLayer, cocos2d::CCArray* objects) {
    auto renderer = new Renderer();
    if (!renderer->initWithLevel(playLayer, objects)) {
        std::string failure = renderer->initializationError.empty()
            ? "Initialization stopped before a reason was recorded"
            : renderer->initializationError;
        delete renderer;

        // Keep the fallback visible. A failed Metal setup must never look
        // indistinguishable from a successful renderer activation.
        if (playLayer &&
            Mod::get()->getSettingValue<bool>("ios_metal_debug_overlay")) {
            if (failure.size() > 112)
                failure.resize(112);
            const std::string hudText = fmt::format(
                "BISMUTH iOS METAL DEBUG\n"
                "RENDER OWNER: VANILLA DRAW\n"
                "STATUS: INIT FAILED (METAL INACTIVE)\n"
                "CAUSE: {}\n"
                "The game is using its original renderer.",
                failure
            );
            auto label = cocos2d::CCLabelBMFont::create(
                hudText.c_str(), "chatFont.fnt"
            );
            if (label) {
                const auto winSize = cocos2d::CCDirector::get()->getWinSize();
                label->setID("iliashdz.bismuth/ios-metal-init-failure-overlay");
                label->setAnchorPoint({0.f, 1.f});
                label->setPosition({8.f, winSize.height - 8.f});
                label->setScale(0.42f);
                label->setColor({255, 110, 110});
                playLayer->addChild(label, 1000000);
            } else {
                log::error("Bismuth could not create the iOS Metal initialization failure HUD");
            }
        }
        return nullptr;
    }
    renderer->autorelease();
    return renderer;
}

Renderer* Renderer::getCurrent() {
    return current;
}

Renderer::~Renderer() {
    reset();
    if (current == this)
        current = nullptr;
}

bool Renderer::initWithLevel(PlayLayer* playLayer, cocos2d::CCArray* objects) {
    initializationError.clear();
    if (current) {
        initializationError = "Another iOS Metal renderer is already active";
        log::error("Bismuth iOS Metal renderer could not initialize: {}", initializationError);
        return false;
    }
    if (!CCNode::init()) {
        initializationError = "Cocos node initialization failed";
        log::error("Bismuth iOS Metal renderer could not initialize: {}", initializationError);
        return false;
    }
    if (!playLayer || !objects ||
        !playLayer->m_objectLayer || !playLayer->m_effectManager) {
        initializationError = "PlayLayer, object layer, effect manager, or object list is missing";
        log::error("Bismuth iOS Metal renderer could not initialize: {}", initializationError);
        return false;
    }

    layer = playLayer;
    current = this;

    if (!loadSpriteSheets()) {
        initializationError = "Original Geometry Dash sprite sheets could not be loaded";
        log::error("Bismuth iOS Metal renderer could not load the original sprite sheets");
        reset();
        return false;
    }

    SpriteMeshDictionary::loadFromFile("spriteMeshes.json");

    if (!groupManager.initialize(layer, objects)) {
        initializationError = "Group-combination state extraction failed";
        log::error("Bismuth iOS Metal renderer could not build group-combination state");
        reset();
        return false;
    }

    BlendStates initialBlendStates {};
    fetchBlendStates(initialBlendStates);

    std::vector<GameObject*> levelObjects;
    levelObjects.reserve(objects->count());
    for (auto object : CCArrayExt<GameObject*>(objects)) {
        if (object)
            levelObjects.push_back(object);
    }

    const auto groupResolver = [this](GameObject* object) {
        return groupManager.getGroupCombinationIndexForObject(object);
    };
    const auto textureResolver = [this](SpriteSheet sheet) -> cocos2d::CCTexture2D* {
        const auto index = static_cast<std::int32_t>(sheet);
        if (index < 0 || index >= static_cast<std::int32_t>(spriteSheets.size()))
            return nullptr;
        return spriteSheets[static_cast<std::size_t>(index)];
    };

    const auto objectBuildStart = std::chrono::steady_clock::now();
    if (!objectBatch.build(
            layer, levelObjects, groupResolver, initialBlendStates, textureResolver)) {
        initializationError =
            "Static level mesh extraction failed (see preceding object-batch log)";
        log::error("Bismuth iOS Metal renderer could not extract static level meshes");
        reset();
        return false;
    }
    cpuLevelBuildMs = elapsedMilliseconds(objectBuildStart);

    std::string error;
    const auto& workItems = objectBatch.getWorkItems();
    const auto& sprites = objectBatch.getInputSprites();
    const auto& objectsInfo = objectBatch.getObjectInfos();
    const auto metalInitializeStart = std::chrono::steady_clock::now();
    if (!metalCompute.initializeLevel(
            workItems.data(), static_cast<std::uint32_t>(workItems.size()),
            sprites.data(), static_cast<std::uint32_t>(sprites.size()),
            objectsInfo.data(), static_cast<std::uint32_t>(objectsInfo.size()),
            error)) {
        initializationError = error.empty()
            ? "Metal compute initialization returned false without an error"
            : error;
        log::error("Bismuth iOS Metal compute initialization failed: {}", initializationError);
        reset();
        return false;
    }
    cpuMetalStaticUploadMs = elapsedMilliseconds(metalInitializeStart);

    if (!createGLESProgram()) {
        initializationError = "OpenGL ES 2 state-sampling shader setup failed";
        reset();
        return false;
    }
    if (!createLayerDrawNodes()) {
        initializationError = "Custom OpenGL ES layer draw-node setup failed";
        reset();
        return false;
    }

    colorChannels.fill(0xffffffffu);
    colorChannels[IOS_COLOR_CHANNEL_BLACK] = 0xff000000u;
    setID("iliashdz.bismuth/ios-metal-compute-renderer");
    ready = true;
    createDebugOverlay();
    updateDebugOverlay();
    return true;
}

bool Renderer::loadSpriteSheets() {
    const char* fontTexture = GameManager::get()->getFontTexture(
        GameManager::get()->m_loadedFont
    );
    auto cache = cocos2d::CCTextureCache::get();
    if (!cache)
        return false;

    spriteSheets[static_cast<std::size_t>(SpriteSheet::GAME_1)] =
        cache->addImage("GJ_GameSheet.png", false);
    spriteSheets[static_cast<std::size_t>(SpriteSheet::GAME_2)] =
        cache->addImage("GJ_GameSheet02.png", false);
    spriteSheets[static_cast<std::size_t>(SpriteSheet::PARTICLE)] =
        cache->addImage("GJ_ParticleSheet.png", false);
    spriteSheets[static_cast<std::size_t>(SpriteSheet::GLOW)] =
        cache->addImage("GJ_GameSheetGlow.png", false);
    spriteSheets[static_cast<std::size_t>(SpriteSheet::FIRE)] =
        cache->addImage("FireSheet_01.png", false);
    spriteSheets[static_cast<std::size_t>(SpriteSheet::PIXEL)] =
        cache->addImage("PixelSheet_01.png", false);
    spriteSheets[static_cast<std::size_t>(SpriteSheet::TEXT)] =
        fontTexture ? cache->addImage(fontTexture, false) : nullptr;

    for (std::size_t i = 0; i < spriteSheets.size(); ++i) {
        if (i == static_cast<std::size_t>(SpriteSheet::SPECIAL) ||
            i == static_cast<std::size_t>(SpriteSheet::_UNK))
            continue;
        if (!spriteSheets[i])
            return false;
    }
    return true;
}

bool Renderer::createGLESProgram() {
    auto vertex = readResourceFile("ios_metal_object_es2.vert");
    auto fragment = readResourceFile("ios_metal_object_es2.frag");
    if (!vertex.has_value() || !fragment.has_value()) {
        log::error("Bismuth iOS Metal ES2 shader resources are missing");
        return false;
    }

    std::string error;
    GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertex.value(), error);
    if (!vertexShader) {
        log::error("{}", error);
        return false;
    }
    GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragment.value(), error);
    if (!fragmentShader) {
        glDeleteShader(vertexShader);
        log::error("{}", error);
        return false;
    }

    GLuint program = glCreateProgram();
    if (!program) {
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return false;
    }

    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glBindAttribLocation(program, 0, "a_positionOffset");
    glBindAttribLocation(program, 1, "a_texCoord");
    glBindAttribLocation(program, 2, "a_spriteIndex");
    glLinkProgram(program);

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint logLength = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
        std::string logText(static_cast<std::size_t>(std::max(1, logLength)), '\0');
        GLsizei written = 0;
        glGetProgramInfoLog(program, static_cast<GLsizei>(logText.size()), &written, logText.data());
        log::error("Bismuth iOS Metal ES2 shader link failed: {}", logText);
        glDeleteProgram(program);
        return false;
    }

    glesProgram = program;
    stateTextureUniform = glGetUniformLocation(program, "u_metalSpriteStateTexture");
    stateTextureSizeUniform = glGetUniformLocation(program, "u_metalSpriteStateTextureSize");
    mvpUniform = glGetUniformLocation(program, "u_mvp");
    spriteSheetUniform = glGetUniformLocation(program, "u_spriteSheetTexture");

    if (stateTextureUniform < 0 || stateTextureSizeUniform < 0 ||
        mvpUniform < 0 || spriteSheetUniform < 0) {
        log::error("Bismuth iOS Metal ES2 shader is missing a required uniform");
        glDeleteProgram(glesProgram);
        glesProgram = 0;
        return false;
    }
    return true;
}

bool Renderer::createLayerDrawNodes() {
    drawLayerKeys.clear();
    vanillaBatchNodes.clear();

    for (const auto& range : objectBatch.getLayerRanges()) {
        cocos2d::CCNode* vanillaNode = layer->parentForZLayer(
            static_cast<std::int32_t>(range.key.zlayer),
            range.key.blending,
            static_cast<std::int32_t>(range.key.spriteSheet),
            false
        );
        auto vanillaBatch = typeinfo_cast<cocos2d::CCSpriteBatchNode*>(vanillaNode);
        if (!vanillaBatch) {
            log::error(
                "Bismuth iOS Metal renderer has no original Cocos batch for layer {}",
                range.key
            );
            return false;
        }

        auto drawNode = new LayerDrawNode(range.key);
        drawNode->setID(range.key.asID() + "-metal");
        layer->m_objectLayer->addChild(drawNode, vanillaBatch->getZOrder());
        drawNode->release();

        drawLayerKeys.push_back(range.key);
        if (std::find(vanillaBatchNodes.begin(), vanillaBatchNodes.end(), vanillaBatch) ==
            vanillaBatchNodes.end()) {
            vanillaBatchNodes.push_back(vanillaBatch);
        }
    }

    if (drawLayerKeys.empty()) {
        log::error("Bismuth iOS Metal renderer extracted no drawable layers");
        return false;
    }

    auto finishNode = new FinishDrawNode();
    finishNode->setID("iliashdz.bismuth/ios-metal-finish");
    layer->m_objectLayer->addChild(finishNode, 999999);
    finishNode->release();
    return true;
}

void Renderer::fetchBlendStates(BlendStates& output) const {
    output.fill(false);
    if (!layer || !layer->m_effectManager)
        return;

    const auto& blending = layer->m_effectManager->m_colorActionVector;
    const std::size_t count = std::min<std::size_t>(output.size(), blending.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (blending[i])
            output[i] = blending[i]->m_blending;
    }

    output[IOS_COLOR_CHANNEL_P1] = true;
    output[IOS_COLOR_CHANNEL_P2] = true;
    output[IOS_COLOR_CHANNEL_LBG] = true;
}

void Renderer::fetchColorChannels() {
    colorChannels.fill(0xffffffffu);
    if (!layer || !layer->m_effectManager)
        return;

    const auto& activeSprites = layer->m_effectManager->m_colorActionSpriteVector;
    for (auto sprite : activeSprites) {
        if (!sprite)
            continue;
        const auto id = static_cast<std::uint32_t>(sprite->m_colorID);
        if (id >= colorChannels.size())
            continue;
        colorChannels[id] = packRGBA(
            sprite->m_copyColor,
            static_cast<std::uint8_t>(std::clamp<float>(sprite->m_opacity, 0.f, 255.f))
        );
    }
    colorChannels[IOS_COLOR_CHANNEL_BLACK] = 0xff000000u;
}

FrameState Renderer::captureFrameState(glm::mat4& mvp) const {
    FrameState frame;
    if (!layer || !layer->m_objectLayer)
        return frame;

    auto director = cocos2d::CCDirector::get();
    const auto winSize = director->getWinSize();
    const cocos2d::CCPoint bottomLeft =
        layer->m_objectLayer->convertToNodeSpace({0.f, 0.f});
    const cocos2d::CCPoint bottomRight =
        layer->m_objectLayer->convertToNodeSpace({winSize.width, 0.f});
    const cocos2d::CCPoint topLeft =
        layer->m_objectLayer->convertToNodeSpace({0.f, winSize.height});

    glm::vec2 cameraPosition(bottomLeft.x, bottomLeft.y);
    glm::vec2 cameraRight(
        bottomRight.x - bottomLeft.x,
        bottomRight.y - bottomLeft.y
    );
    glm::vec2 cameraUp(
        topLeft.x - bottomLeft.x,
        topLeft.y - bottomLeft.y
    );

    frame.cameraPositionX = cameraPosition.x;
    frame.cameraPositionY = cameraPosition.y;
    const glm::vec2 normalizedRight = glm::normalize(cameraRight);
    const glm::vec2 normalizedUp = glm::normalize(cameraUp);
    frame.cameraRightX = normalizedRight.x;
    frame.cameraRightY = normalizedRight.y;
    frame.cameraUpX = normalizedUp.x;
    frame.cameraUpY = normalizedUp.y;
    frame.cameraWidth = glm::length(cameraRight);
    frame.cameraHeight = glm::length(cameraUp);
    frame.cameraViewWidth = layer->m_cameraWidth;
    frame.cameraViewHeight = layer->m_cameraHeight;
    frame.windowWidth = winSize.width;
    frame.windowHeight = winSize.height;
    frame.timer = gameTimer;

    float audioScale = 0.5f;
    if (layer->m_skipAudioStep)
        audioScale = FMODAudioEngine::sharedEngine()->getMeteringValue();
    else if (layer->m_audioEffectsLayer)
        audioScale = layer->m_audioEffectsLayer->m_audioScale;
    if (layer->m_isSilent || (layer->m_isPracticeMode && !layer->m_practiceMusicSync))
        audioScale = 0.5f;
    frame.audioScale = audioScale;

    frame.gameStateCameraPositionX = layer->m_gameState.m_cameraPosition.x;
    frame.gameStateCameraZoom = layer->m_gameState.m_cameraZoom;
    frame.winSizeWidth = winSize.width;
    float flipProgress = layer->m_gameState.m_levelFlipProgress;
    if (layer->m_cameraFlip == -1.0f)
        flipProgress = 1.0f - flipProgress;
    frame.levelFlipProgress = flipProgress;
    frame.screenRight = director->getScreenRight();
    frame.cameraUnzoomedX = layer->m_cameraUnzoomedX;
    frame.gameStateFlags = layer->m_player1 && layer->m_player1->m_isDead
        ? 1u : 0u;

    if (layer->m_effectManager) {
        const auto background = GameToolbox::transformColor(
            layer->m_effectManager->activeColorForIndex(IOS_COLOR_CHANNEL_BG),
            0.0, -0.2, 0.2
        );
        frame.specialLightBGColor[0] = background.r / 255.f;
        frame.specialLightBGColor[1] = background.g / 255.f;
        frame.specialLightBGColor[2] = background.b / 255.f;
        frame.specialLightBGColor[3] = 1.f;
    }

    kmMat4 projection;
    kmMat4 modelView;
    kmMat4 combined;
    kmGLGetMatrix(KM_GL_PROJECTION, &projection);
    kmGLGetMatrix(KM_GL_MODELVIEW, &modelView);
    kmMat4Multiply(&combined, &projection, &modelView);
    reinterpret_cast<kmMat4&>(mvp) = combined;

    return frame;
}

void Renderer::update(float dt) {
    if (!ready)
        return;

    gameTimer += dt;
    if (dt > 0.00001f) {
        const float instantFPS = 1.0f / dt;
        estimatedFPS = estimatedFPS == 0.f
            ? instantFPS
            : estimatedFPS * 0.85f + instantFPS * 0.15f;
    }

    overlayRefreshTimer += dt;
    if (overlayRefreshTimer >= 0.12f) {
        overlayRefreshTimer = 0.f;
        updateDebugOverlay();
    }
}

void Renderer::draw() {
    if (!ready || !metalCompute.isReady())
        return;

    const auto blendStart = std::chrono::steady_clock::now();
    BlendStates blendStates {};
    fetchBlendStates(blendStates);
    if (!objectBatch.refreshBlendOrder(blendStates)) {
        log::error("Bismuth iOS Metal renderer failed to refresh layer order after a blend change");
        frameHasOutput = false;
        dispatchStatus = "BLEND ORDER ERROR";
        lastDispatchError = "Could not rebuild the static layer order";
        return;
    }
    cpuBlendOrderMs = elapsedMilliseconds(blendStart);

    auto stageStart = std::chrono::steady_clock::now();
    fetchColorChannels();
    cpuColorFetchMs = elapsedMilliseconds(stageStart);

    stageStart = std::chrono::steady_clock::now();
    groupManager.update();
    cpuGroupStateMs = elapsedMilliseconds(stageStart);

    stageStart = std::chrono::steady_clock::now();
    FrameState frame = captureFrameState(activeMVP);
    cpuFrameStateMs = elapsedMilliseconds(stageStart);
    const auto groupStates = groupManager.getStates();
    const auto pulses = groupManager.getPulseEntries();
    std::string error;
    stageStart = std::chrono::steady_clock::now();
    const bool submitted = metalCompute.dispatchFrame(
        groupStates.data(), static_cast<std::uint32_t>(groupStates.size()),
        colorChannels.data(), static_cast<std::uint32_t>(colorChannels.size()),
        pulses.data(), static_cast<std::uint32_t>(pulses.size()),
        frame, error
    );
    cpuSubmitMs = elapsedMilliseconds(stageStart);

    if (submitted) {
        ++submittedFrameCount;
        dispatchStatus = "SUBMITTED";
        lastDispatchError.clear();
    } else {
        dispatchStatus = error.find("No free Metal output slot") != std::string::npos
            ? "GPU SLOTS BUSY (SKIPPED)"
            : "DISPATCH FAILED";
        lastDispatchError = error;
    }

    frameHasOutput = metalCompute.acquireLatestFrame(activeOutput);
    const std::string metalError = metalCompute.takeLastError();
    if (!metalError.empty()) {
        dispatchStatus = "METAL COMMAND FAILED";
        lastDispatchError = metalError;
    }
    if (frameHasOutput && submitted && activeOutput.sequence < submittedFrameCount) {
        dispatchStatus = "SUBMITTED / PREVIOUS GPU FRAME DISPLAYED";
    } else if (!frameHasOutput) {
        dispatchStatus = submitted ? "WAITING FOR FIRST GPU FRAME" : dispatchStatus;
    }

    drawsThisFrame = 0;
    drawFailuresThisFrame = 0;
}

void Renderer::drawLayer(const LayerKey& key) {
    if (!ready || !frameHasOutput || !glesProgram)
        return;

    const auto ranges = objectBatch.getLayerRanges();
    const auto rangeIt = std::find_if(ranges.begin(), ranges.end(),
        [&](const LayerRange& range) { return range.key == key; });
    if (rangeIt == ranges.end() || rangeIt->vertexCount == 0)
        return;

    const auto sheetIndex = static_cast<std::int32_t>(key.spriteSheet);
    if (sheetIndex < 0 || sheetIndex >= static_cast<std::int32_t>(spriteSheets.size()))
        return;
    auto texture = spriteSheets[static_cast<std::size_t>(sheetIndex)];
    if (!texture)
        return;

    GLESStateSnapshot savedState;
    glUseProgram(glesProgram);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, activeOutput.textureName);
    glUniform1i(stateTextureUniform, 0);
    glUniform2f(
        stateTextureSizeUniform,
        static_cast<float>(activeOutput.textureWidth),
        static_cast<float>(activeOutput.textureHeight)
    );

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, texture->getName());
    glUniform1i(spriteSheetUniform, 1);
    glUniformMatrix4fv(mvpUniform, 1, GL_FALSE, glm::value_ptr(activeMVP));

    glBindBuffer(GL_ARRAY_BUFFER, objectBatch.getVertexBuffer());
    const GLsizei stride = static_cast<GLsizei>(sizeof(Vertex));
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride,
        reinterpret_cast<const void*>(offsetof(Vertex, positionOffset)));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
        reinterpret_cast<const void*>(offsetof(Vertex, texCoord)));
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride,
        reinterpret_cast<const void*>(offsetof(Vertex, spriteIndex)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    if (key.blending || key.spriteSheet == SpriteSheet::GLOW)
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    else
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glDrawArrays(
        GL_TRIANGLES,
        static_cast<GLint>(rangeIt->firstVertex),
        static_cast<GLsizei>(rangeIt->vertexCount)
    );

    savedState.restore();
    const GLenum error = glGetError();
    if (error == GL_NO_ERROR)
        ++drawsThisFrame;
    else {
        ++drawFailuresThisFrame;
        log::warn("Bismuth iOS Metal GLES layer draw failed with GL error 0x{:X}", error);
    }
}


void Renderer::createDebugOverlay() {
    if (!layer || debugOverlay ||
        !Mod::get()->getSettingValue<bool>("ios_metal_debug_overlay"))
        return;

    debugOverlay = cocos2d::CCLabelBMFont::create("", "chatFont.fnt");
    if (!debugOverlay) {
        log::warn("Bismuth iOS Metal debug overlay label could not be created");
        return;
    }

    const auto winSize = cocos2d::CCDirector::get()->getWinSize();
    debugOverlay->setAnchorPoint({0.f, 1.f});
    debugOverlay->setPosition({8.f, winSize.height - 8.f});
    debugOverlay->setScale(0.34f);
    debugOverlay->setColor({255, 255, 255});
    debugOverlay->setZOrder(1000000);
    layer->addChild(debugOverlay, 1000000);
}

void Renderer::updateDebugOverlay() {
    const bool enabled =
        Mod::get()->getSettingValue<bool>("ios_metal_debug_overlay");

    if (!enabled) {
        if (debugOverlay)
            debugOverlay->setVisible(false);
        return;
    }

    if (!debugOverlay)
        createDebugOverlay();
    if (!debugOverlay)
        return;
    debugOverlay->setVisible(true);

    const auto cpuFrameTotal =
        cpuBlendOrderMs + cpuColorFetchMs + cpuGroupStateMs +
        cpuFrameStateMs + cpuSubmitMs;
    const std::uint64_t culledSprites = frameHasOutput &&
        activeOutput.totalSpriteCount >= activeOutput.visibleSpriteCount
        ? activeOutput.totalSpriteCount - activeOutput.visibleSpriteCount
        : 0u;
    const auto vertexCount = objectBatch.getVertexCount();
    const auto meshBytes =
        static_cast<std::uint64_t>(vertexCount) * sizeof(Vertex);
    const auto compactStateBytes =
        groupManager.getStates().size() * sizeof(GroupCombinationState) +
        colorChannels.size() * sizeof(std::uint32_t) +
        groupManager.getPulseEntries().size() * sizeof(PulseEntry);
    const auto staticMetalInputBytes =
        objectBatch.getWorkItems().size() * sizeof(SpriteWorkItem) +
        objectBatch.getInputSprites().size() * sizeof(InputSprite) +
        objectBatch.getObjectInfos().size() * sizeof(ObjectInfo);
    const auto totalGpuStateBytes =
        static_cast<std::uint64_t>(activeOutput.textureWidth) *
        activeOutput.textureHeight * 16u;

    const char* ownerText = takenOwnership
        ? "METAL COMPUTE + OPENGL ES DRAW"
        : "VANILLA DRAW ACTIVE (SAFE HANDOVER)";

    const std::string gpuTimeText = frameHasOutput && activeOutput.hasGpuTiming
        ? fmt::format("{:.3f} ms", activeOutput.gpuTimeMilliseconds)
        : "not available yet";

    std::string errorText = lastDispatchError;
    if (errorText.size() > 76)
        errorText.resize(76);

    const std::string display = fmt::format(
        "BISMUTH iOS METAL DEBUG\n"
        "RENDER OWNER: {}\n"
        "STATUS: {}\n"
        "FPS: {:.1f}   OUTPUT: {}\n"
        "\n"
        "GPU COMPUTE (REAL COMMAND-BUFFER TIME)\n"
        "Device: {}\n"
        "Metal time: {}\n"
        "Sprites processed: {}   visible: {}   culled: {}\n"
        "Threads: {} groups x {} threads\n"
        "Output state texture: {} x {} ({:.2f} MiB)\n"
        "\n"
        "OPENGL ES DRAW\n"
        "Successful draws: {} / {}   GL failures: {}\n"
        "Static mesh: {} vertices ({:.2f} MiB)\n"
        "\n"
        "CPU WORK (PER FRAME)\n"
        "Per-sprite transform/culling prep: NONE (Metal kernel)\n"
        "Blend/order: {:.3f} ms   colors: {:.3f} ms\n"
        "Group state: {:.3f} ms   frame state: {:.3f} ms\n"
        "Submit + compact copies: {:.3f} ms\n"
        "Measured CPU preparation/submit: {:.3f} ms\n"
        "Compact state sent: {:.2f} KiB/frame\n"
        "\n"
        "CPU WORK (ONCE PER LEVEL)\n"
        "Static mesh extraction/build: {:.2f} ms\n"
        "Metal static-buffer setup/upload: {:.2f} ms\n"
        "Static Metal inputs: {:.2f} MiB (uploaded once)\n"
        "Metal submissions: {}\n"
        "{}",
        ownerText,
        dispatchStatus,
        estimatedFPS,
        frameHasOutput ? activeOutput.sequence : 0,
        metalCompute.getDeviceName(),
        gpuTimeText,
        frameHasOutput ? activeOutput.totalSpriteCount : objectBatch.getSpriteCount(),
        frameHasOutput ? activeOutput.visibleSpriteCount : 0,
        frameHasOutput ? culledSprites : 0,
        frameHasOutput ? activeOutput.threadgroupCount : 0,
        frameHasOutput ? activeOutput.threadsPerThreadgroup : 0,
        activeOutput.textureWidth,
        activeOutput.textureHeight,
        static_cast<double>(totalGpuStateBytes) / (1024.0 * 1024.0),
        drawsThisFrame,
        drawLayerKeys.size(),
        drawFailuresThisFrame,
        vertexCount,
        static_cast<double>(meshBytes) / (1024.0 * 1024.0),
        cpuBlendOrderMs,
        cpuColorFetchMs,
        cpuGroupStateMs,
        cpuFrameStateMs,
        cpuSubmitMs,
        cpuFrameTotal,
        static_cast<double>(compactStateBytes) / 1024.0,
        cpuLevelBuildMs,
        cpuMetalStaticUploadMs,
        static_cast<double>(staticMetalInputBytes) / (1024.0 * 1024.0),
        submittedFrameCount,
        errorText.empty() ? "No dispatch error" : fmt::format("ERROR: {}", errorText)
    );

    debugOverlay->setString(display.c_str());
}

void Renderer::finishDrawFrame() {
    if (!frameHasOutput)
        return;

    std::string error;
    if (!metalCompute.finishOpenGLRead(activeOutput, error))
        log::warn("Bismuth iOS Metal output fence failed: {}", error);

    if (!takenOwnership && drawsThisFrame == drawLayerKeys.size()) {
        for (auto batch : vanillaBatchNodes) {
            if (batch)
                batch->setVisible(false);
        }
        takenOwnership = true;
        log::info(
            "BISMUTH iOS METAL: took OpenGL ES drawing ownership after {} layer draws",
            drawsThisFrame
        );
    }
}

void Renderer::restoreVanillaBatches() {
    for (auto batch : vanillaBatchNodes) {
        if (batch)
            batch->setVisible(true);
    }
    vanillaBatchNodes.clear();
}

void Renderer::reset() {
    restoreVanillaBatches();
    if (debugOverlay) {
        debugOverlay->removeFromParentAndCleanup(true);
        debugOverlay = nullptr;
    }
    if (glesProgram) {
        glDeleteProgram(glesProgram);
        glesProgram = 0;
    }
    metalCompute.reset();
    objectBatch.clear();
    groupManager.reset();

    drawLayerKeys.clear();
    frameHasOutput = false;
    ready = false;
    takenOwnership = false;
    if (current == this)
        current = nullptr;
    layer = nullptr;
}

} // namespace BismuthIOSMetal

#endif
