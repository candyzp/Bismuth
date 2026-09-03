#ifdef GEODE_IS_IOS

#include "../Renderer.hpp"
#include "DataTexture.hpp"
#include "../SpriteMeshDictionary.hpp"
#include "../VisibilityManager.hpp"
#include "../ObjectBatchNode.hpp"
#include "../ObjectSorter.hpp"
#include "../GroupManager.hpp"
#include "BProfiler.hpp"
#include "profiler.hpp"

#include "Geode/cocos/CCDirector.h"
#include "Geode/cocos/kazmath/include/kazmath/mat4.h"
#include "Geode/cocos/sprite_nodes/CCSpriteBatchNode.h"
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/RingObject.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>

using namespace geode::prelude;

namespace {

constexpr usize IOS_STATIC_TEXELS_PER_OBJECT = 5;
constexpr usize IOS_RUNTIME_TEXELS_PER_OBJECT = 2;
constexpr usize IOS_GROUP_TEXELS_PER_STATE = 3;
constexpr i32 IOS_STATIC_TEXTURE_UNIT = 1;
constexpr i32 IOS_GROUP_TEXTURE_UNIT = 2;
constexpr i32 IOS_COLOR_TEXTURE_UNIT = 3;

struct IOSRendererState {
    DataTexture* staticDataTexture = nullptr;
    DataTexture* groupDataTexture = nullptr;
    DataTexture* colorDataTexture = nullptr;

    std::vector<glm::vec4> staticTexels;
    std::vector<glm::vec4> runtimeTexels;
    std::vector<glm::vec4> groupTexels;
    std::vector<GameObject*> objects;
    std::vector<glm::vec2> inverseBaseScales;
    usize runtimeDataOffset = 0;
    std::unique_ptr<ColorChannelBuffer> colorChannels = std::make_unique<ColorChannelBuffer>();

    ~IOSRendererState() {
        if (staticDataTexture) DataTexture::destroy(staticDataTexture);
        if (groupDataTexture) DataTexture::destroy(groupDataTexture);
        if (colorDataTexture) DataTexture::destroy(colorDataTexture);
    }
};

static std::unordered_map<Renderer*, std::unique_ptr<IOSRendererState>> g_iosStates;
static Renderer* currentRenderer = nullptr;

static IOSRendererState* iosState(Renderer* renderer) {
    auto it = g_iosStates.find(renderer);
    return it == g_iosStates.end() ? nullptr : it->second.get();
}

static bool hasExtension(const char* extension) {
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    return extensions && std::strstr(extensions, extension) != nullptr;
}

static std::string byteSizeToString(usize size) {
    std::string unit = "B";
    double dsize = size;
    if (dsize > 1000.0) { dsize /= 1000.0; unit = "kB"; }
    if (dsize > 1000.0) { dsize /= 1000.0; unit = "MB"; }
    return fmt::format("{:.3f}{}", dsize, unit);
}

static u32 convertToShaderHSV(const ccHSVValue& hsv) {
    u32 hue = hsv.h + 256.f;
    u32 sat = (hsv.s + (hsv.absoluteSaturation ? 1.0 : 0.0)) * 127.5f;
    u32 val = (hsv.v + (hsv.absoluteBrightness ? 1.0 : 0.0)) * 127.5f;

    u32 ret = (hue & HSV_HUE_MASK) << HSV_HUE_BIT |
              (sat & HSV_SAT_MASK) << HSV_SAT_BIT |
              (val & HSV_VAL_MASK) << HSV_VAL_BIT;
    if (hsv.absoluteSaturation) ret |= HSV_SAT_ADD;
    if (hsv.absoluteBrightness) ret |= HSV_VAL_ADD;
    return ret;
}

struct DecodedHSV {
    float hue = 0.f;
    float sat = 0.f;
    float val = 0.f;
    float satAdd = 0.f;
    float valAdd = 0.f;
};

static DecodedHSV decodeShaderHSV(u32 packed) {
    DecodedHSV ret;
    ret.hue = ((i32)((packed >> HSV_HUE_BIT) & HSV_HUE_MASK) - 256) / 360.f;
    ret.sat = ((packed >> HSV_SAT_BIT) & HSV_SAT_MASK) / 127.5f;
    ret.val = ((packed >> HSV_VAL_BIT) & HSV_VAL_MASK) / 127.5f;
    ret.satAdd = (packed & HSV_SAT_ADD) ? 1.f : 0.f;
    ret.valAdd = (packed & HSV_VAL_ADD) ? 1.f : 0.f;
    return ret;
}

static void packGroupStateTexture(Renderer* renderer) {
    auto state = iosState(renderer);
    if (!state || !state->groupDataTexture)
        return;

    auto states = renderer->getGroupManager().getGroupStates();
    usize needed = std::max<usize>(1, states.size() * IOS_GROUP_TEXELS_PER_STATE);
    state->groupTexels.assign(needed, glm::vec4(0.f));

    for (usize i = 0; i < states.size(); ++i) {
        const auto& value = states[i];
        usize base = i * IOS_GROUP_TEXELS_PER_STATE;
        state->groupTexels[base + 0] = {
            value.positionalTransform[0][0], value.positionalTransform[0][1],
            value.positionalTransform[1][0], value.positionalTransform[1][1]
        };
        state->groupTexels[base + 1] = {
            value.localTransform[0][0], value.localTransform[0][1],
            value.localTransform[1][0], value.localTransform[1][1]
        };
        state->groupTexels[base + 2] = {
            value.offset.x, value.offset.y, value.opacity, 0.f
        };
    }

    state->groupDataTexture->upload(state->groupTexels.data(), state->groupTexels.size());
}

static void packRuntimeObjectStateTexture(Renderer* renderer) {
    auto state = iosState(renderer);
    if (!state || !state->staticDataTexture || state->objects.empty())
        return;

    const usize objectCount = state->objects.size();
    state->runtimeTexels.resize(objectCount * IOS_RUNTIME_TEXELS_PER_OBJECT);

    for (usize i = 0; i < objectCount; ++i) {
        auto object = state->objects[i];
        if (!object) {
            state->runtimeTexels[i * 2 + 0] = { 0.f, 0.f, 0.f, 0.f };
            state->runtimeTexels[i * 2 + 1] = { 0.f, 0.f, 0.f, 0.f };
            continue;
        }

        // Keep the CPU side as a thin state packer. The vertex shader combines
        // the two Area Rotate contributions and converts raw Area Scale offsets
        // into final scale factors. Inverse base scales are precomputed once.
        const glm::vec2 inverseBaseScale = state->inverseBaseScales[i];
        const float runtimeRotationX = object->m_unk2A8;
        const float runtimeRotationY = object->m_unk2B0;
        const float runtimeScaleOffsetX = object->m_unk2BC;
        const float runtimeScaleOffsetY = object->m_unk2C0;

        // 2.2 Area / enter effects keep their temporary visual contribution in
        // these fields. Old Move/Rotate group state remains in GroupManager, so
        // feeding only these deltas avoids applying the normal trigger path twice.
        const bool hasRuntimeVisualState =
            std::abs(object->m_positionXOffset) > 0.001f ||
            std::abs(object->m_positionYOffset) > 0.001f ||
            std::abs(runtimeRotationX) > 0.001f ||
            std::abs(runtimeRotationY) > 0.001f ||
            std::abs(runtimeScaleOffsetX) > 0.0001f ||
            std::abs(runtimeScaleOffsetY) > 0.0001f;

        if (hasRuntimeVisualState)
            renderer->getObjectBatch().trackRuntimeVisualObject(object);

        // r0 = world move XY + raw Area Rotate contributions XY.
        state->runtimeTexels[i * 2 + 0] = {
            object->m_positionXOffset,
            object->m_positionYOffset,
            runtimeRotationX,
            runtimeRotationY
        };
        // r1 = raw Area Scale offsets XY + precomputed inverse base scale XY.
        state->runtimeTexels[i * 2 + 1] = {
            runtimeScaleOffsetX,
            runtimeScaleOffsetY,
            inverseBaseScale.x,
            inverseBaseScale.y
        };
    }

    state->staticDataTexture->uploadRange(
        state->runtimeTexels.data(),
        state->runtimeDataOffset,
        state->runtimeTexels.size()
    );
}

} // namespace

Renderer::~Renderer() {
    terminate();
}

bool Renderer::init(PlayLayer* layer) {
    if (currentRenderer)
        return false;

    currentRenderer = this;
    this->layer = layer;

    if (!Mod::get()->getSettingValue<bool>("enabled"))
        return false;

    if (!hasExtension("GL_OES_texture_float")) {
        log::error("Bismuth iOS requires GL_OES_texture_float");
        return false;
    }
    if (!hasExtension("GL_OES_element_index_uint")) {
        log::error("Bismuth iOS requires GL_OES_element_index_uint");
        return false;
    }

    GLint vertexTextureUnits = 0;
    glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &vertexTextureUnits);
    if (vertexTextureUnits < 3) {
        log::error("Bismuth iOS requires at least 3 vertex texture units, device reports {}", vertexTextureUnits);
        return false;
    }

    g_iosStates[this] = std::make_unique<IOSRendererState>();
    auto state = iosState(this);
    colorChannelBuffer = state->colorChannels.get();
    std::memset(colorChannelBuffer, 0, sizeof(ColorChannelBuffer));

    ingameEnableDisable = Mod::get()->getSettingValue<bool>("ingame_enable");
    useIndexCulling = false;

    log::info("Bismuth iOS OpenGL ES version: {}", (const char*)glGetString(GL_VERSION));
    log::info("Bismuth iOS renderer: {}", (const char*)glGetString(GL_RENDERER));

    auto tcache = cocos2d::CCTextureCache::get();
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
    for (auto object : CCArrayExt<GameObject*>(layer->m_objects)) {
        if (object == layer->m_anticheatSpike)
            continue;
        if (object->isTrigger() || object->m_isHide)
            continue;
        sorter.addGameObject(object);
    }
    sorter.finalizeSorting();

    renderedGameObjectCount = 0;
    for (auto it = sorter.iterator(); !it.isEnd(); it.next())
        renderedGameObjectCount++;

    generateStaticRenderingBuffer(sorter);
    if (!state->staticDataTexture)
        return false;

    for (auto it = sorter.iterator(); !it.isEnd(); it.next())
        objectBatch.writeGameObject(it.get());
    objectBatch.finishWriting();
    generateBatchNodes();

    shader = Shader::create("object_ios.vert", "object_ios.frag");
    if (!shader)
        return false;

    usize groupTexelCount = std::max<usize>(1, groupManager.getGroupCombinationCount() * IOS_GROUP_TEXELS_PER_STATE);
    state->groupDataTexture = DataTexture::create("Group state", groupTexelCount, DataTexture::Type::FloatRGBA);
    state->colorDataTexture = DataTexture::create("Color channels", COLOR_CHANNEL_COUNT, DataTexture::Type::ByteRGBA);
    if (!state->groupDataTexture || !state->colorDataTexture)
        return false;

    state->groupTexels.resize(groupTexelCount);
    state->colorDataTexture->upload(colorChannelBuffer, COLOR_CHANNEL_COUNT);

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
    debugTextOutline1->setPosition(0.5, CCDirector::get()->getWinSize().height - 8.5);
    debugTextOutline1->setScale(0.5);
    layer->addChild(debugTextOutline1, 999);

    debugTextOutline2->setAnchorPoint(CCPoint(0, 1));
    debugTextOutline2->setPosition(1.5, CCDirector::get()->getWinSize().height - 7.5);
    debugTextOutline2->setScale(0.5);
    layer->addChild(debugTextOutline2, 999);

    setZOrder(-2);
    reset();
    setEnabled(true);
    rendererStartTime = getTime();

    log::info("Bismuth iOS texture-backed renderer initialized");
    return true;
}

void Renderer::generateBatchNodes() {
    for (const auto& id : objectBatch.getUsedLayerIds()) {
        auto batchNode = ObjectBatchNode::create(id);
        if (!batchNode)
            continue;
        auto node = getSpriteBatchNodeWithLayerId(id);
        if (!node)
            continue;
        layer->m_objectLayer->addChild(batchNode, node->getZOrder());
        batchNodes.push_back(batchNode);
    }
}

void Renderer::terminate() {
    if (shader)
        Shader::destroy(shader);
    shader = nullptr;

    if (basicShader)
        Shader::destroy(basicShader);
    basicShader = nullptr;

    colorChannelBuffer = nullptr;
    colorChannelBufferObject = nullptr;
    srbBuffer = nullptr;
    uniformBuffer = nullptr;

    g_iosStates.erase(this);
    if (currentRenderer == this)
        currentRenderer = nullptr;
}

void Renderer::prepareShaderUniforms() {
    kmMat4 matrixP;
    kmMat4 matrixMV;
    kmMat4 matrixMVP;
    kmGLGetMatrix(KM_GL_PROJECTION, &matrixP);
    kmGLGetMatrix(KM_GL_MODELVIEW, &matrixMV);
    kmMat4Multiply(&matrixMVP, &matrixP, &matrixMV);

    (kmMat4&)uniforms.u_mvp = matrixMVP;
    uniforms.u_timer = gameTimer;
    uniforms.u_cameraPosition = ccPointToGLM(layer->m_gameState.m_cameraPosition2);
    uniforms.u_cameraViewSize = glm::vec2(layer->m_cameraWidth, layer->m_cameraHeight);
    auto winsize = CCDirector::get()->getWinSize();
    uniforms.u_winSize = glm::vec2(winsize.width, winsize.height);
    uniforms.u_screenRight = CCDirector::get()->getScreenRight();
    uniforms.u_cameraUnzoomedX = layer->m_cameraUnzoomedX;
    ccColor3B specialLightBG = GameToolbox::transformColor(
        layer->m_effectManager->activeColorForIndex(COLOR_CHANNEL_BG), 0.0, -0.2, 0.2
    );
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
}

void Renderer::prepareColorChannelBuffer() {
    auto timer = BProfiler::start("Generate color channel buffer");
    auto effectManager = layer->m_effectManager;

    for (usize i = 0; i < effectManager->m_colorActionSpriteVector.size(); i++) {
        auto sprite = effectManager->m_colorActionSpriteVector[i];
        if (!sprite)
            continue;
        auto id = sprite->m_colorID;
        if (id < 0 || id >= COLOR_CHANNEL_COUNT)
            continue;
        auto& channelColor = colorChannelBuffer->u_channelColors[id];
        channelColor.r = sprite->m_color.r;
        channelColor.g = sprite->m_color.g;
        channelColor.b = sprite->m_color.b;
        channelColor.a = (u8)sprite->m_opacity;
    }

    colorChannelBuffer->u_channelColors[COLOR_CHANNEL_BLACK] = {0, 0, 0, 255};
    timer.end();
}

void Renderer::generateStaticRenderingBuffer(ObjectSorter& sorter) {
    std::vector<StaticObjectInfo> objectInfos;
    objectInfos.resize(renderedGameObjectCount);

    auto state = iosState(this);
    state->objects.clear();
    state->inverseBaseScales.clear();
    state->objects.reserve(renderedGameObjectCount);
    state->inverseBaseScales.reserve(renderedGameObjectCount);

    usize index = 0;
    for (auto it = sorter.iterator(); !it.isEnd(); it.next()) {
        auto object = it.get();
        auto info = &objectInfos[index];

        info->startPosition = ccPointToGLM(object->m_startPosition);
        info->rotationSpeed = object->getHasRotateAction()
            ? ((EnhancedGameObject*)object)->m_rotationDelta
            : 0.0f;
        info->flags = 0;

        if (object->m_usesAudioScale) {
            info->flags |= OBJECT_FLAG_USES_AUDIO_SCALE;
            if (object->m_customAudioScale) {
                info->flags |= OBJECT_FLAG_CUSTOM_AUDIO_SCALE;
                info->audioScaleMin = object->m_minAudioScale;
                info->audioScaleMax = object->m_maxAudioScale;
            }
            if (typeinfo_cast<RingObject*>(object))
                info->flags |= OBJECT_FLAG_IS_ORB;
        }

        if (object->m_baseColor && object->m_baseColor->m_usesHSV) {
            info->flags |= OBJECT_FLAG_HAS_BASE_HSV;
            info->baseHSV = convertToShaderHSV(object->m_baseColor->m_hsv);
        }
        if (object->m_detailColor && object->m_detailColor->m_usesHSV) {
            info->flags |= OBJECT_FLAG_HAS_DETAIL_HSV;
            info->detailHSV = convertToShaderHSV(object->m_detailColor->m_hsv);
        }

        info->opacity = object->m_opacityMod2 > 0.0 ? object->m_opacityMod2 : 1.0;
        info->groupCombinationIndex = groupManager.getGroupCombinationIndexForObject(object);
        if (object->m_isInvisibleBlock) info->flags |= OBJECT_FLAG_IS_INVISIBLE_BLOCK;
        if (object->m_customGlowColor) info->flags |= OBJECT_FLAG_SPECIAL_GLOW_COLOR;
        if (object->m_objectType == GameObjectType::Solid) info->flags |= OBJECT_FLAG_IS_STATIC_OBJECT;
        info->fadeMargin = object->m_fadeMargin;

        objectSRBIndicies[object] = index;
        state->objects.push_back(object);
        state->inverseBaseScales.push_back({
            std::abs(object->m_scaleX) > 0.0001f ? 1.f / object->m_scaleX : 0.f,
            std::abs(object->m_scaleY) > 0.0001f ? 1.f / object->m_scaleY : 0.f
        });
        index++;
    }

    const usize staticTexelCount = objectInfos.size() * IOS_STATIC_TEXELS_PER_OBJECT;
    const usize runtimeTexelCount = objectInfos.size() * IOS_RUNTIME_TEXELS_PER_OBJECT;
    const usize texelCount = std::max<usize>(1, staticTexelCount + runtimeTexelCount);
    state->runtimeDataOffset = staticTexelCount;
    state->staticTexels.assign(texelCount, glm::vec4(0.f));
    state->runtimeTexels.assign(runtimeTexelCount, glm::vec4(0.f));

    for (usize i = 0; i < objectInfos.size(); ++i) {
        const auto& info = objectInfos[i];
        auto baseHSV = decodeShaderHSV(info.baseHSV);
        auto detailHSV = decodeShaderHSV(info.detailHSV);
        usize base = i * IOS_STATIC_TEXELS_PER_OBJECT;

        state->staticTexels[base + 0] = {
            info.startPosition.x, info.startPosition.y, info.rotationSpeed, info.opacity
        };
        state->staticTexels[base + 1] = {
            info.audioScaleMin, info.audioScaleMax, info.fadeMargin, (float)info.groupCombinationIndex
        };
        state->staticTexels[base + 2] = {
            (float)info.flags, baseHSV.hue, baseHSV.sat, baseHSV.val
        };
        state->staticTexels[base + 3] = {
            baseHSV.satAdd, baseHSV.valAdd, detailHSV.hue, detailHSV.sat
        };
        state->staticTexels[base + 4] = {
            detailHSV.val, detailHSV.satAdd, detailHSV.valAdd, 0.f
        };

        const usize runtimeBase = state->runtimeDataOffset + i * IOS_RUNTIME_TEXELS_PER_OBJECT;
        state->staticTexels[runtimeBase + 0] = { 0.f, 0.f, 0.f, 0.f };
        state->staticTexels[runtimeBase + 1] = { 0.f, 0.f, 0.f, 0.f };
    }

    state->staticDataTexture = DataTexture::create(
        "Static + runtime object data", texelCount, DataTexture::Type::FloatRGBA
    );
    if (state->staticDataTexture)
        state->staticDataTexture->upload(state->staticTexels.data(), state->staticTexels.size());
}

void Renderer::draw() {
    profiler::functionPush("Renderer::draw");
    auto timer = BProfiler::start("Renderer::draw");

    storeGLStates();
    prepareShaderUniforms();

    auto state = iosState(this);
    if (!isPaused() && state) {
        prepareColorChannelBuffer();
        groupManager.prepareGroupStateBuffer();
        packGroupStateTexture(this);
        packRuntimeObjectStateTexture(this);
        state->colorDataTexture->upload(colorChannelBuffer, COLOR_CHANNEL_COUNT);

        CameraView view = {
            ccPointToGLM(layer->m_gameState.m_cameraPosition2),
            glm::vec2(layer->m_cameraWidth, 0),
            glm::vec2(0, layer->m_cameraHeight)
        };
        constexpr float cullScale = 1.3f;
        view.bottomLeft += (view.rightVector + view.upVector) * 0.5f;
        view.rightVector *= cullScale;
        view.upVector *= cullScale;
        view.bottomLeft -= (view.rightVector + view.upVector) * 0.5f;
        objectBatch.predraw(view);
    }

    restoreGLStates();
    timer.end();

    if (debugText->isVisible())
        updateDebugText();

    profiler::functionPop();
}

void Renderer::updateDebugText() {
    std::string text;
    if (!enabled) {
        text = "Bismuth iOS renderer is disabled\n";
    } else if (debugTextEnabled) {
        auto screenSize = CCDirector::get()->getWinSizeInPixels();
        auto state = iosState(this);
        const usize objectDataSize = state && state->staticDataTexture
            ? state->staticDataTexture->getCapacity() * sizeof(glm::vec4)
            : 0;

        text += fmt::format("Bismuth iOS {}\n", Mod::get()->getVersion().toVString());
        text += fmt::format("OpenGL ES {}\n", (const char*)glGetString(GL_VERSION));
        text += fmt::format("{}\n", (const char*)glGetString(GL_RENDERER));
        text += fmt::format("Window: {}x{}\n", screenSize.width, screenSize.height);
        text += fmt::format("Vertex buffer: {}\n", byteSizeToString(objectBatch.getVertexBufferSize()));
        text += fmt::format("Object data: {}\n", byteSizeToString(objectDataSize));
        text += fmt::format("Group data: {}\n", byteSizeToString(groupManager.getGroupStateBufferSize()));
        text += fmt::format("Color data: {}\n", byteSizeToString(sizeof(ColorChannelBuffer)));
        text += "Runtime transform math: GPU\n";
        text += "\n" + BProfiler::toString();
    }

    debugText->setString(text.c_str());
    debugTextOutline1->setString(text.c_str());
    debugTextOutline2->setString(text.c_str());
}

Shader* Renderer::prepareDraw() {
    auto state = iosState(this);
    if (!shader || !state)
        return shader;

    storeGLStates();
    shader->use();

    shader->setMatrix4("u_mvp", (float*)&uniforms.u_mvp);
    shader->setFloat("u_timer", uniforms.u_timer);
    shader->setFloat("u_audioScale", uniforms.u_audioScale);
    shader->setVec2("u_cameraPosition", uniforms.u_cameraPosition);
    shader->setVec2("u_cameraViewSize", uniforms.u_cameraViewSize);
    shader->setVec2("u_winSize", uniforms.u_winSize);
    shader->setFloat("u_screenRight", uniforms.u_screenRight);
    shader->setFloat("u_cameraUnzoomedX", uniforms.u_cameraUnzoomedX);
    shader->setVec3("u_specialLightBGColor", uniforms.u_specialLightBGColor);
    shader->setFloat("u_gameStateFlags", (float)uniforms.u_gameStateFlags);
    shader->setFloat("u_runtimeDataOffset", (float)state->runtimeDataOffset);

    shader->setTexture("u_staticDataTexture", IOS_STATIC_TEXTURE_UNIT, state->staticDataTexture->getId());
    shader->setTexture("u_groupDataTexture", IOS_GROUP_TEXTURE_UNIT, state->groupDataTexture->getId());
    shader->setTexture("u_colorDataTexture", IOS_COLOR_TEXTURE_UNIT, state->colorDataTexture->getId());
    shader->setVec2("u_staticDataTextureSize", state->staticDataTexture->getSize());
    shader->setVec2("u_groupDataTextureSize", state->groupDataTexture->getSize());
    shader->setVec2("u_colorDataTextureSize", state->colorDataTexture->getSize());

    return shader;
}

void Renderer::finishDraw() {
    restoreGLStates();
}

void Renderer::update(float dt) {
    gameTimer += dt;
    if (debugText && debugText->isVisible())
        updateDebugText();

    auto& gstate = layer->m_gameState;
    cameraCenterPos = ccPointToGLM(
        gstate.m_cameraPosition2 + CCPoint(layer->m_cameraWidth * 0.5, layer->m_cameraHeight * 0.5)
    );
}

bool Renderer::isColorChannelBlending(i32 channel) {
    if (channel == COLOR_CHANNEL_P1 || channel == COLOR_CHANNEL_P2 || channel == COLOR_CHANNEL_LBG)
        return true;
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
    return !canEnableDisableIngame();
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

void Renderer::drawLine(const glm::vec2&, const glm::vec2&, const glm::vec4&) {
    // Debug line rendering is intentionally omitted from the ES2 backend.
}

static i32 storedVAO = 0;
static i32 storedVBO = 0;
static i32 storedIBO = 0;
static i32 storedProgram = 0;
static i32 storedBlendSrcAlpha = 0;
static i32 storedBlendSrcRGB = 0;
static i32 storedBlendDstAlpha = 0;
static i32 storedBlendDstRGB = 0;
static i32 storedTextures[4] = {};

void storeGLStates() {
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &storedVAO);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &storedVBO);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &storedIBO);
    glGetIntegerv(GL_CURRENT_PROGRAM, &storedProgram);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &storedBlendSrcAlpha);
    glGetIntegerv(GL_BLEND_SRC_RGB, &storedBlendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &storedBlendDstAlpha);
    glGetIntegerv(GL_BLEND_DST_RGB, &storedBlendDstRGB);

    for (i32 i = 0; i < 4; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &storedTextures[i]);
    }
    glActiveTexture(GL_TEXTURE0);
}

void restoreGLStates() {
    glBindVertexArray(storedVAO);
    glBindBuffer(GL_ARRAY_BUFFER, storedVBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, storedIBO);
    glUseProgram(storedProgram);
    glBlendFuncSeparate(storedBlendSrcRGB, storedBlendDstRGB, storedBlendSrcAlpha, storedBlendDstAlpha);

    for (i32 i = 0; i < 4; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, storedTextures[i]);
    }
    glActiveTexture(GL_TEXTURE0);
}

#endif