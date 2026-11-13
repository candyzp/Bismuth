#include <algorithm>
#include "Renderer.hpp"
#include "Geode/cocos/CCDirector.h"
#include "Geode/cocos/kazmath/include/kazmath/mat4.h"
#include "Geode/cocos/sprite_nodes/CCSpriteBatchNode.h"
#include "GroupManager.hpp"
#include "HotKey.hpp"
#include "ObjectBatchNode.hpp"
#include "SpriteMeshDictionary.hpp"
#include "common.hpp"
#include "../cl.hpp"
#include <Geode/Enums.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/RingObject.hpp>
#include <BProfiler.hpp>
#include "profiler.hpp"
#include "ObjectSorter.hpp"
#include "../diagnostics/Diagnostics.hpp"

using namespace geode::prelude;
using namespace bismuth;
namespace gfx = bismuth::gfx;

static Renderer* currentRenderer;

u32 convertToShaderHSV(const ccHSVValue& hsv) {
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

//// DEVICE SETUP ////

static gfx::DeviceOptions deviceOptionsFromSettings() {
    gfx::DeviceOptions options;

    auto backend = Mod::get()->getSettingValue<std::string>("renderer_backend");
    if (backend == "OpenGL")
        options.backend = gfx::BackendPreference::OpenGL;
    else if (backend == "Vulkan / Metal")
        options.backend = gfx::BackendPreference::Native;
    else
        options.backend = gfx::BackendPreference::Auto;

    auto culling = Mod::get()->getSettingValue<std::string>("culling_mode");
    if (culling == "GPU")
        options.culling = gfx::CullingPreference::Gpu;
    else if (culling == "CPU")
        options.culling = gfx::CullingPreference::Cpu;
    else
        options.culling = gfx::CullingPreference::Auto;

    return options;
}

/*
    bgfx stays initialized between levels (creating a Vulkan device per level
    would slow down level loading). It's re-created when the settings change or
    when Geometry Dash's GL context was replaced (Android after backgrounding,
    window recreation).
*/
namespace {

/// Logs how long each step of a level's setup takes.
class LoadTimer {
public:
    void step(const char* name) {
        u64 now = getTime();
        log::info("Bismuth: {} took {:.1f} ms", name, (double)(now - last) / 1000000.0);
        last = now;
    }

private:
    u64 last = getTime();
};

const char* spriteSheetFile(SpriteSheet sheet) {
    switch (sheet) {
        case SpriteSheet::GAME_1:   return "GJ_GameSheet.png";
        case SpriteSheet::GAME_2:   return "GJ_GameSheet02.png";
        case SpriteSheet::PARTICLE: return "GJ_ParticleSheet.png";
        case SpriteSheet::GLOW:     return "GJ_GameSheetGlow.png";
        case SpriteSheet::FIRE:     return "FireSheet_01.png";
        case SpriteSheet::PIXEL:    return "PixelSheet_01.png";
        default:                    return nullptr;
    }
}

// Sprite sheets imported into bgfx, kept across levels (see Device::getHostTextures).
// The CCTexture2Ds are retained so their GL names can't be reused by another texture.
struct ImportedSheet {
    Ref<CCTexture2D> texture;
    GLuint glName = 0;
    bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
};
std::vector<ImportedSheet> importedSheets;
u32 importedGeneration = 0;

bgfx::TextureHandle importSpriteSheet(CCTexture2D* texture) {
    auto& device = gfx::Device::get();
    auto& textures = device.getHostTextures();

    for (auto& sheet : importedSheets) {
        if (sheet.texture == texture && sheet.glName == texture->getName())
            return sheet.handle;
    }

    auto handle = textures.import(texture->getName(), texture->getPixelsWide(), texture->getPixelsHigh());
    if (bgfx::isValid(handle))
        importedSheets.push_back({ texture, texture->getName(), handle });
    return handle;
}

} // namespace

void Renderer::importSpriteSheets(CCTexture2D* textures[], bgfx::TextureHandle handles[]) {
    auto& device = gfx::Device::get();
    auto& hostTextures = device.getHostTextures();

    if (importedGeneration != device.getGeneration()) {
        // The device restarted: its textures are gone.
        importedSheets.clear();
        importedGeneration = device.getGeneration();
    }

    // Drop the sheets GD replaced (texture reload, texture packs): only we hold them.
    for (auto it = importedSheets.begin(); it != importedSheets.end();) {
        if (it->texture->retainCount() == 1 || it->texture->getName() != it->glName) {
            hostTextures.release(it->handle);
            it = importedSheets.erase(it);
        } else {
            it++;
        }
    }

    auto tcache = CCTextureCache::get();
    for (i32 i = 0; i < (i32)SpriteSheet::COUNT; i++) {
        textures[i] = nullptr;
        handles[i] = BGFX_INVALID_HANDLE;

        const char* file = spriteSheetFile((SpriteSheet)i);
        if (!file)
            continue;

        textures[i] = tcache->addImage(file, false);
        if (textures[i])
            handles[i] = importSpriteSheet(textures[i]);
    }
}

void Renderer::preloadSpriteSheets() {
    auto& device = gfx::Device::get();
    // In-context OpenGL needs no copy (and a bgfx frame to finish the import).
    if (!device.isInitialized() || device.isHostMode())
        return;

    CCTexture2D* textures[(i32)SpriteSheet::COUNT];
    bgfx::TextureHandle handles[(i32)SpriteSheet::COUNT];

    LoadTimer timer;
    importSpriteSheets(textures, handles);
    timer.step("preloading sprite sheets");
}

bool ensureBismuthDevice(std::string& report) {
    static gfx::DeviceOptions lastOptions;
    auto& device = gfx::Device::get();
    auto options = deviceOptionsFromSettings();

    if (device.isInitialized()) {
        bool changed = options.backend != lastOptions.backend || options.culling != lastOptions.culling;
        if (changed || device.isHostContextLost())
            device.shutdown();
    }

    if (!device.isInitialized()) {
        if (!device.init(options, report))
            return false;
        lastOptions = options;
    }
    return true;
}

Renderer::~Renderer() { terminate(); }

class FinishNode : public CCNode {
public:
    void draw() override {
        if (currentRenderer)
            currentRenderer->finish();
    }
};

bool Renderer::init(PlayLayer* layer, CCArray* levelObjects) {
    if (currentRenderer)
        return false;
    diag::checkpoint("Renderer::init started");
    diag::setLevelStats(levelObjects ? levelObjects->count() : 0, 0);
    currentRenderer = this;

    this->layer = layer;

    for (auto& handle : spriteSheetHandles)
        handle = BGFX_INVALID_HANDLE;

    if (!Mod::get()->getSettingValue<bool>("enabled")) {
        disabledInSettings = true;
        return false;
    }

    std::string report;
    if (!ensureBismuthDevice(report)) {
        Notification::create(
            "Bismuth couldn't start a renderer on this device.\nThe game will render normally.",
            NotificationIcon::Error,
            5.0f
        )->show();
        log::error("Bismuth: no usable renderer:\n{}", report);
        return false;
    }

    auto& device = gfx::Device::get();
    if (!report.empty())
        log::info("Bismuth: renderer fallbacks:\n{}", report);

    ingameEnableDisable = Mod::get()->getSettingValue<bool>("ingame_enable");
    debugTextEnabled = Mod::get()->getSettingValue<bool>("show_debug_overlay");

    log::info("Bismuth: {}", device.describe());

    LoadTimer loadTimer;

    diag::updatePhase("Renderer::init: import Geometry Dash sprite sheets");
    importSpriteSheets(spriteSheets, spriteSheetHandles);
    loadTimer.step("sprite sheets");

    diag::updatePhase("Renderer::init: load sprite mesh dictionary");
    SpriteMeshDictionary::loadFromFile("spriteMeshes.json");
    loadTimer.step("sprite meshes");

    diag::updatePhase("Renderer::init: build group state");
    groupManager.initWithObjects(levelObjects);
    loadTimer.step("groups");

    ObjectSorter sorter;
    sorter.initForGameLayer(layer);

    std::vector<GameObject*> objects;

    log::info("Level contains {} object(s)", levelObjects->count());
    log::info("Sorting objects...");
    for (auto object : CCArrayExt<GameObject*>(levelObjects)) {
        if (object == layer->m_anticheatSpike)
            continue;

        if (object->isTrigger() || object->m_isHide || object->m_objectID == GUIDE_OBJECT_ID)
            continue;

        sorter.addGameObject(object);
    }

    // Objects with a particle system (portals, orbs, pads, coins...). GD only
    // creates their particles when it activates the object, which Bismuth skips.
    for (auto object : CCArrayExt<GameObject*>(levelObjects)) {
        if (object->m_hasParticles && !object->m_hasNoParticles && !object->m_isHide
        &&  !object->m_particleString.empty())
            particleObjects.push_back({ object, false });
    }
    log::info("{} object(s) with particles", particleObjects.size());

    for (auto it = sorter.iterator(); !it.isEnd(); it.next())
        objects.push_back(it.get());
    loadTimer.step("sorting objects");

    log::info("Generating static rendering buffer....");

    renderedGameObjectCount = objects.size();

    bismuth::StaticBatchData staticData;
    diag::updatePhase("Renderer::init: generate static rendering buffer");
    generateStaticRenderingBuffer(objects, staticData);
    loadTimer.step("static rendering buffer");

    u32 groupCombCount = groupManager.getGroupCombinationCount();
    log::info("{} group combinations detected", groupCombCount);

    // Work group sizing for culling.
    if (device.getCullingPath() == gfx::CullingPath::Gpu) {
        isize cores = getGPUCoreCount();
        objectBatch.getScheduler().setMaxWorkGroupCount(cores > 0 ? (isize)(cores * WORK_GROUPS_PER_GPU_CORES) : 8000);
    } else {
        // Small chunks so the CPU culling threads stay balanced.
        objectBatch.getScheduler().setMaxWorkGroupCount(4096);
    }

    log::info("Generating vertex buffer...");

    for (auto& value : colorChannelsIsBlending)
        value = false;

    fetchBlendingStates();

    diag::updatePhase("Renderer::init: generate object vertex batches");
    for (auto object : objects)
        objectBatch.writeGameObject(object);
    objectBatch.finishWriting(staticData);
    generateBatchNodes();
    loadTimer.step("vertex buffer");

    log::info("Sprite count: {}", objectBatch.getSpriteCount());
    diag::setLevelStats(levelObjects ? levelObjects->count() : 0, objectBatch.getSpriteCount());
    diag::checkpoint("Renderer::init: object batches generated");

    diag::updatePhase("Renderer::init: create bgfx shader and batch resources");
    if (!batchRenderer.init(std::move(staticData), device.getCullingPath(), device.getRendererType())) {
        log::error("Bismuth: failed to create GPU resources");
        return false;
    }

    // In-context OpenGL: bgfx must create its texture objects before they can be
    // pointed at cocos2d's textures.
    if (device.isHostMode())
        device.flushHost();
    device.getHostTextures().finishImports();
    loadTimer.step("GPU resources");

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

    setZOrder(-99999);
    active = true;
    setEnabled(true);

    rendererStartTime = getTime();

    auto finishNode = new FinishNode;
    finishNode->autorelease();
    layer->m_objectLayer->addChild(finishNode, 99999);
    ownNodes.insert(finishNode);
    ownNodes.insert(this);

    reset();

    if (Mod::get()->getSettingValue<bool>("overdraw_view"))
        overdrawView.setEnabled(true);

    if (ingameEnableDisable) {
        Notification::create(
            "Debug tools removes ALL optimizations!\nDisable it for better performance.",
            NotificationIcon::Warning,
            5.0f
        )->show();
    }

    log::info("Renderer initialized");
    diag::checkpoint("Renderer::init completed; waiting for first gameplay draw");
    return true;
}

void Renderer::generateBatchNodes() {
    for (const auto& id : objectBatch.getUsedLayerIds()) {
        auto batchNode = ObjectBatchNode::create(id);
        if (batchNode) {
            auto node = getSpriteBatchNodeWithLayerId(id);

            if (!node)
                continue;

            layer->m_objectLayer->addChild(batchNode, node->getZOrder());
            batchNodes.push_back(batchNode);
        }
    }
}

void Renderer::terminate() {
    if (currentRenderer != this)
        return;

    auto interop = gfx::Device::get().getInterop();
    for (auto& target : sharedTargets) {
        if (interop && target.platform)
            interop->destroyTarget(target);
    }
    sharedTargets.clear();

    overdrawView.releaseResources();
    batchRenderer.shutdown();

    currentRenderer = nullptr;
    log::info("Renderer terminated");
}

void Renderer::deactivate(const char* reason) {
    if (!active)
        return;

    log::warn("Bismuth: disabled for this level: {}", reason);
    setEnabled(false);
    active = false;

    // Resources are released with the renderer; with a lost GL context they're
    // abandoned by gfx::Device::shutdown at the next level.
}

void Renderer::prepareGameState() {
    kmMat4 matrixP;
    kmMat4 matrixMV;
    kmMat4 matrixMVP;

    kmGLGetMatrix(KM_GL_PROJECTION, &matrixP);
    kmGLGetMatrix(KM_GL_MODELVIEW, &matrixMV);

    kmMat4Multiply(&matrixMVP, &matrixP, &matrixMV);

    std::memcpy(gameState.mvp, matrixMVP.mat, sizeof(gameState.mvp));
    gameState.timer = gameTimer;
    gameState.cameraViewSize = glm::vec2(layer->m_cameraWidth, layer->m_cameraHeight);
    auto winsize = CCDirector::get()->getWinSize();
    gameState.winSize = glm::vec2(winsize.width, winsize.height);
    gameState.screenRight = CCDirector::get()->getScreenRight();
    gameState.cameraUnzoomedX = layer->m_cameraUnzoomedX;
    ccColor3B specialLightBG = GameToolbox::transformColor(layer->m_effectManager->activeColorForIndex(COLOR_CHANNEL_BG), 0.0, -0.2, 0.2);
    gameState.specialLightBGColor = ccColor3BToGLM(specialLightBG);

    vec2 cameraBottomLeft  = ccPointToGLM(layer->m_objectLayer->convertToNodeSpace({0, 0}));
    vec2 cameraBottomRight = ccPointToGLM(layer->m_objectLayer->convertToNodeSpace({winsize.width, 0}));
    vec2 cameraTopLeft     = ccPointToGLM(layer->m_objectLayer->convertToNodeSpace({0, winsize.height}));

    vec2 cameraRight = cameraBottomRight - cameraBottomLeft;
    vec2 cameraUp    = cameraTopLeft     - cameraBottomLeft;

    gameState.cameraPosition = cameraBottomLeft;
    gameState.cameraRight = glm::normalize(cameraRight);
    gameState.cameraUp    = glm::normalize(cameraUp);
    gameState.cameraSize  = glm::vec2 { glm::length(cameraRight), glm::length(cameraUp) };

    gameState.flags = 0;
    if (layer->m_player1->m_isDead)
        gameState.flags |= GAME_STATE_IS_PLAYER_DEAD;

    if (layer->m_skipAudioStep)
        gameState.audioScale = FMODAudioEngine::sharedEngine()->getMeteringValue();
    else
        gameState.audioScale = layer->m_audioEffectsLayer->m_audioScale;

    if (layer->m_isSilent || (layer->m_isPracticeMode && !layer->m_practiceMusicSync))
        gameState.audioScale = 0.5;
}

void Renderer::prepareColorChannels() {
    PROFILER_PUSH("Renderer::prepareColorChannels");

    // The color channels come first in the frame buffer, 4 packed RGBA8 per uvec4.
    frameData.buffer.assign(COLOR_CHANNEL_UVEC4S, bismuth::UVec4(0));
    auto setChannel = [&](u32 id, u8 r, u8 g, u8 b, u8 a) {
        if (id < COLOR_CHANNEL_COUNT)
            frameData.buffer[id / 4][id % 4] = u32(r) | (u32(g) << 8) | (u32(b) << 16) | (u32(a) << 24);
    };

    auto effectManager = layer->m_effectManager;

    for (usize i = 0; i < effectManager->m_colorActionSpriteVector.size(); i++) {
        auto sprite = effectManager->m_colorActionSpriteVector[i];
        if (sprite == nullptr)
            continue;
        setChannel(sprite->m_colorID, sprite->m_copyColor.r, sprite->m_copyColor.g, sprite->m_copyColor.b, (u8)sprite->m_opacity);
    }

    setChannel(COLOR_CHANNEL_BLACK, 0, 0, 0, 255);

    PROFILER_POP();
}

void Renderer::generateStaticRenderingBuffer(std::vector<GameObject*>& objects, bismuth::StaticBatchData& out) {
    using bismuth::UVec4;
    using bismuth::floatBits;

    out.staticBuffer.resize(objects.size() * OBJECT_INFO_STRIDE);

    usize index = 0;

    for (auto object : objects) {
        float rotationSpeed = 0.0;
        if (object->getHasRotateAction())
            rotationSpeed = ((EnhancedGameObject*)object)->m_rotationDelta; // 'rotationDelta' is incorrect naming

        u32 flags = 0;
        float audioScaleMin = 0.0, audioScaleMax = 0.0;
        if (object->m_usesAudioScale) {
            flags |= OBJECT_FLAG_USES_AUDIO_SCALE;

            if (object->m_customAudioScale) {
                flags |= OBJECT_FLAG_CUSTOM_AUDIO_SCALE;
                audioScaleMin = object->m_minAudioScale;
                audioScaleMax = object->m_maxAudioScale;
            }

            if (typeinfo_cast<RingObject*>(object))
                flags |= OBJECT_FLAG_IS_ORB;
        }

        u32 baseHSV = 0, detailHSV = 0;
        if (object->m_baseColor && object->m_baseColor->m_usesHSV) {
            flags |= OBJECT_FLAG_HAS_BASE_HSV;
            baseHSV = convertToShaderHSV(object->m_baseColor->m_hsv);
        }

        if (object->m_detailColor && object->m_detailColor->m_usesHSV) {
            flags |= OBJECT_FLAG_HAS_DETAIL_HSV;
            detailHSV = convertToShaderHSV(object->m_detailColor->m_hsv);
        }

        float opacity = (object->m_opacityMod2 > 0.0) ? object->m_opacityMod2 : 1.0;

        if (object->m_isInvisibleBlock) flags |= OBJECT_FLAG_IS_INVISIBLE_BLOCK;
        if (object->m_customGlowColor)  flags |= OBJECT_FLAG_SPECIAL_GLOW_COLOR;
        if (object->m_objectType == GameObjectType::Solid)
            flags |= OBJECT_FLAG_IS_STATIC_OBJECT;

        UVec4* info = &out.staticBuffer[index * OBJECT_INFO_STRIDE];
        info[0] = UVec4(floatBits(rotationSpeed), flags, floatBits(audioScaleMin), floatBits(audioScaleMax));
        info[1] = UVec4(floatBits(object->m_fadeMargin), floatBits(opacity), (u32)object->m_activeMainColorID, baseHSV);
        info[2] = UVec4((u32)object->m_activeDetailColorID, detailHSV, 0, 0);

        objectSRBIndicies[object] = index;
        index++;
    }
}

//// COMPOSITE GROUPS ////

LayerBlend Renderer::blendForLayer(const LayerKey& key) {
    return (key.blending || key.spriteSheet == SpriteSheet::GLOW) ? LayerBlend::Additive : LayerBlend::Normal;
}

void Renderer::computeCompositeGroups() {
    groups.clear();
    groupOfNode.clear();

    std::unordered_set<CCNode*> layerNodes(batchNodes.begin(), batchNodes.end());

    CompositeGroup* current = nullptr;
    for (auto child : CCArrayExt<CCNode*>(layer->m_objectLayer->getChildren())) {
        if (layerNodes.contains(child)) {
            auto node = static_cast<ObjectBatchNode*>(child);
            if (!node->isVisible() || !objectBatch.shouldDraw(node->getLayerKey()))
                continue;

            if (!current) {
                if (groups.size() < RendererViews::MaxGroups)
                    groups.emplace_back();
                // Over the limit (extremely unlikely): keep adding to the last group.
                current = &groups.back();
            }
            current->nodes.push_back(node);
            groupOfNode[node] = (i32)groups.size() - 1;
            continue;
        }

        // Any other visible node is drawn by cocos2d between Bismuth's layers.
        if (child->isVisible() && !ownNodes.contains(child) && !overdrawView.isOwnNode(child))
            current = nullptr;
    }
}

i32 Renderer::getGroupOfNode(ObjectBatchNode* node) const {
    auto it = groupOfNode.find(node);
    return it == groupOfNode.end() ? -1 : it->second;
}

bool Renderer::ensureSharedTargets(u32 count, u32 width, u32 height) {
    auto interop = gfx::Device::get().getInterop();
    if (!interop)
        return false;

    if (width != sharedTargetWidth || height != sharedTargetHeight) {
        for (auto& target : sharedTargets)
            interop->destroyTarget(target);
        sharedTargets.clear();
        sharedTargetWidth = width;
        sharedTargetHeight = height;
    }

    while (sharedTargets.size() < count) {
        sharedTargets.emplace_back();
        if (!interop->createTarget(sharedTargets.back(), width, height)) {
            sharedTargets.pop_back();
            return false;
        }
        log::info("Bismuth: created shared render target {} ({}x{})", sharedTargets.size(), width, height);
    }
    return true;
}

void Renderer::drawGroupLayers(const CompositeGroup& group, bgfx::ViewId view, bool toSharedTarget) {
    auto sorted = objectBatch.getSortedBatch();
    if (!sorted)
        return;

    for (auto node : group.nodes) {
        const LayerKey& key = node->getLayerKey();
        auto it = sorted->layerIndexPerKey.find(key);
        if (it == sorted->layerIndexPerKey.end())
            continue;

        auto texture = getSpriteSheetHandle(key.spriteSheet);
        if (!bgfx::isValid(texture))
            continue;

        batchRenderer.drawLayer(view, it->second.index, texture, blendForLayer(key), toSharedTarget);

        if (overdrawView.isEnabled())
            overdrawView.countLayer(it->second.index, texture);
    }
}

void Renderer::drawGroupDirect(const CompositeGroup& group) {
    auto* spriteRenderer = gfx::Device::get().getHostSpriteRenderer();
    auto sorted = objectBatch.getSortedBatch();
    if (!spriteRenderer || !sorted)
        return;

    directLayers.clear();
    for (auto node : group.nodes) {
        const LayerKey& key = node->getLayerKey();
        auto it = sorted->layerIndexPerKey.find(key);
        if (it == sorted->layerIndexPerKey.end())
            continue;

        auto texture = getSpriteSheetTexture(key.spriteSheet);
        auto range = batchRenderer.getCpuLayerRange(it->second.index);
        if (!texture || range.vertexCount == 0)
            continue;

        directLayers.push_back({
            .firstVertex = range.firstVertex,
            .vertexCount = range.vertexCount,
            .texture = texture->getName(),
            .additive = blendForLayer(key) == LayerBlend::Additive,
        });
    }

    spriteRenderer->draw(gameState.mvp, directLayers.data(), (u32)directLayers.size());
}

//// FRAME ////

void Renderer::draw() {
    if (!enabled || !active)
        return;

    PROFILER_PUSH("Renderer::draw");

    auto& device = gfx::Device::get();
    if (device.isHostContextLost()) {
        deactivate("Geometry Dash's OpenGL context was lost");
        PROFILER_POP();
        return;
    }

    // GPU time is only shown in the F3 overlay; measuring it costs a lot on
    // some drivers in OpenGL mode.
    device.setGpuTiming(debugText && debugText->isVisible());

    fetchBlendingStates();
    prepareGameState();

    const bool paused = isPaused();
    if (!paused) {
        prepareColorChannels();
        groupManager.updateGroupStateBuffer();
        groupManager.writeFrameData(frameData);
    }

    diag::updatePhase("Renderer::draw: sort object batches");
    auto sorted = objectBatch.predraw();
    batchRenderer.setSortedBatch(&sorted->data);

    // While paused nothing moves, the previous culling results stay valid.
    const bool needsCulling = !paused || !hasCulledOnce || lastSortedBatch != &sorted->data;
    if (needsCulling)
        batchRenderer.updateFrame(frameData, gameState);

    diag::updatePhase("Renderer::draw: build compositing groups");
    computeCompositeGroups();

    renderingTimer.start();

    if (device.isHostMode()) {
        // Without bgfx when possible: a bgfx frame per composite group costs a
        // lot where GL calls are expensive (Wine + Box64, GL on a separate thread).
        auto* spriteRenderer = (batchRenderer.getCullingPath() == gfx::CullingPath::Cpu && !overdrawView.isEnabled())
            ? device.getHostSpriteRenderer() : nullptr;
        directHostDraw = spriteRenderer != nullptr;

        if (needsCulling) {
            diag::updatePhase("Renderer::draw: host-mode culling");
            // bgfx: submitted with the first group's flush, as bgfx runs the
            // compute view before the draw views.
            auto timer = BProfiler::start("Bismuth culling");
            batchRenderer.cull(RendererViews::Compute, !directHostDraw);
            timer.end();
            if (groups.empty() && !directHostDraw)
                device.flushHost();
        }

        if (directHostDraw && (needsCulling || !lastFrameDirectHostDraw)) {
            const auto& vertices = batchRenderer.getCpuVertices();
            spriteRenderer->setVertices(vertices.data(), (u32)vertices.size());
        }
        lastFrameDirectHostDraw = directHostDraw;

        overdrawView.beginFrame();
    } else {
        gfx::GLint viewport[4] = {};
        gfx::glf().glGetIntegerv(gfx::gl::VIEWPORT, viewport);
        const u32 width = (u32)std::max(viewport[2], 1);
        const u32 height = (u32)std::max(viewport[3], 1);

        // Creating targets submits bgfx frames, so it must happen before any other work.
        diag::updatePhase("Renderer::draw: allocate Metal/OpenGL shared render targets");
        u32 targetCount = (u32)groups.size() + (overdrawView.isEnabled() ? 1 : 0);
        if (!ensureSharedTargets(targetCount, width, height)) {
            deactivate("couldn't create render targets shared with OpenGL");
            renderingTimer.stop();
            PROFILER_POP();
            return;
        }

        if (needsCulling) {
            diag::updatePhase("Renderer::draw: GPU compute culling dispatch");
            auto timer = BProfiler::start("Bismuth culling");
            batchRenderer.cull(RendererViews::Compute);
            timer.end();
        }

        overdrawView.beginFrame(width, height);

        for (u32 g = 0; g < groups.size(); g++) {
            bgfx::ViewId view = RendererViews::GroupFirst + g;
            bgfx::setViewFrameBuffer(view, sharedTargets[g].framebuffer);
            bgfx::setViewRect(view, 0, 0, (u16)width, (u16)height);
            bgfx::setViewClear(view, BGFX_CLEAR_COLOR, 0x00000000);
            bgfx::touch(view);
            drawGroupLayers(groups[g], view, true);
        }

        if (overdrawView.isEnabled())
            overdrawView.renderHeatMap(&sharedTargets[groups.size()], width, height);

        diag::updatePhase("Renderer::draw: submit Metal frame and publish shared target");
        device.submitNative();
        nativeFrameSubmitted = true;
    }

    hasCulledOnce = true;
    lastSortedBatch = &sorted->data;

    diag::updatePhase("Renderer::draw: frame submitted");
    PROFILER_POP();
}

void Renderer::drawLayerNode(ObjectBatchNode* node) {
    if (!enabled || !active)
        return;

    i32 g = getGroupOfNode(node);
    if (g < 0 || groups[g].nodes.front() != node)
        return; // drawn together with the first layer of its group

    PROFILER_PUSH("Renderer::drawLayerNode");

    auto& device = gfx::Device::get();
    diag::updatePhase("Renderer::drawLayerNode: composite gameplay sprites into Geometry Dash");
    if (device.isHostMode() && directHostDraw) {
        auto timer = BProfiler::start("Bismuth drawing");
        drawGroupDirect(groups[g]);
        timer.end();
    } else if (device.isHostMode()) {
        device.setHostViewRect(RendererViews::HostDraw);
        bgfx::setViewFrameBuffer(RendererViews::HostDraw, BGFX_INVALID_HANDLE);
        drawGroupLayers(groups[g], RendererViews::HostDraw, false);
        device.flushHost();
    } else if (nativeFrameSubmitted && (u32)g < sharedTargets.size()) {
        device.getInterop()->beforeHostRead();
        device.getCompositor().draw(sharedTargets[g]);
    }

    PROFILER_POP();
}

void Renderer::finish() {
    diag::updatePhase("Renderer::finish: end gameplay frame");
    renderingTimer.stop();

    auto& device = gfx::Device::get();
    if (active && nativeFrameSubmitted) {
        if (overdrawView.isEnabled() && sharedTargets.size() > groups.size()) {
            device.getInterop()->beforeHostRead();
            device.getCompositor().draw(sharedTargets[groups.size()]);
        }
        device.getInterop()->afterHostRead();
        nativeFrameSubmitted = false;
    }
    device.endGameFrame();

    if (debugText->isVisible())
        updateDebugText();
}

void Renderer::updateDebugText() {
    std::string text = "";

    text += overdrawView.getDebugText();

    if (!active) {
        text += "Bismuth is inactive for this level (see logs)\n";
    } else if (!enabled) {
        text += "Bismuth renderer is disabled\n";
        text += "Press F8 to enable\n";
    } else {
        if (debugTextEnabled) {
            auto& device = gfx::Device::get();
            auto& host = device.getHost();
            auto screenSize = CCDirector::get()->getWinSizeInPixels();

            text += fmt::format("Bismuth renderer {}\n", Mod::get()->getVersion().toVString());
            text += fmt::format("{}\n", device.describe());
            text += fmt::format("Host GL: {}\n", host.version);
            text += fmt::format("{}\n", host.renderer);
            text += fmt::format("Window: {}x{}\n", screenSize.width, screenSize.height);
            text += fmt::format("GPU time: {:.3f}ms{}\n", device.getLastGpuTimeMs(),
                                device.isHostMode() ? " (only measured while this is shown)" : "");
            if (device.isHostMode() && directHostDraw) {
                text += "Drawing: direct OpenGL (no bgfx)\n";
            } else if (device.isHostMode()) {
                const auto& flush = device.getLastHostFlushStats();
                text += fmt::format("GL flushes: {}, save {:.3f}ms, bgfx {:.3f}ms, restore {:.3f}ms\n",
                                    flush.count, flush.saveMs, flush.bgfxMs, flush.restoreMs);
            }
            text += fmt::format("Layers: {} in {} group(s)\n", batchNodes.size(), groups.size());
            text += fmt::format("Vertex buffer size: {}\n", byteSizeToString(objectBatch.getVertexBufferSize()));
            if (batchRenderer.getCullingPath() == gfx::CullingPath::Cpu)
                text += fmt::format("Visible triangles: {}\n", batchRenderer.getLastVisibleVertexCount() / 3);
            text += fmt::format("Static buffers: {}\n", byteSizeToString(batchRenderer.getStaticBufferBytes()));
            text += fmt::format("Frame buffer: {}\n", byteSizeToString(batchRenderer.getFrameBufferBytes()));
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
}

CCSpriteBatchNode* Renderer::getSpriteBatchNodeWithLayerId(LayerKey id) {
    CCNode* node = layer->parentForZLayer((i32)id.zlayer, id.blending, (i32)id.spriteSheet, false);

    if (!node || layer->m_batchNodes->indexOfObject(node) == UINT_MAX)
        return nullptr;

    return (CCSpriteBatchNode*)node;
}

Ref<Renderer> Renderer::create(PlayLayer* layer, CCArray* objects) {
    auto ren = new Renderer;

    if (ren->init(layer, objects)) {
        ren->autorelease();
        return ren;
    }

    if (ren->disabledInSettings)
        log::info("Bismuth is turned off in its settings (\"Bismuth enabled\"), the level renders normally");
    else
        log::error("Failed to initialize renderer, the level renders normally (see the errors above)");
    delete ren;
    return nullptr;
}

Ref<Renderer> Renderer::get() {
    return currentRenderer;
}

bool Renderer::useOptimizations() {
    if (!active || canEnableDisableIngame())
        return false;

    return true;
}

void Renderer::setEnabled(bool enabled) {
    if (!active)
        enabled = false;

    this->enabled = enabled;
    this->setVisible(enabled);
    for (auto node : batchNodes)
        node->setVisible(enabled);
    for (auto batch : CCArrayExt<CCNode*>(layer->m_batchNodes))
        batch->setVisible(!enabled);
    if (debugText)
        updateDebugText();
}

void Renderer::reset() {
    groupManager.resetGroupStates();
}

void Renderer::updateObjectParticles(float dt) {
    if (!particleObjects.empty()) {
        // Camera bounds in object layer space (handles zoom and rotation), with a
        // margin like GD's activation sections so particles start off-screen.
        constexpr float margin = 150.f;

        auto winsize = CCDirector::get()->getWinSize();
        CCPoint corners[4] = {
            layer->m_objectLayer->convertToNodeSpace({ 0, 0 }),
            layer->m_objectLayer->convertToNodeSpace({ winsize.width, 0 }),
            layer->m_objectLayer->convertToNodeSpace({ 0, winsize.height }),
            layer->m_objectLayer->convertToNodeSpace({ winsize.width, winsize.height }),
        };

        float minX = corners[0].x, maxX = corners[0].x;
        float minY = corners[0].y, maxY = corners[0].y;
        for (auto& corner : corners) {
            minX = std::min(minX, corner.x); maxX = std::max(maxX, corner.x);
            minY = std::min(minY, corner.y); maxY = std::max(maxY, corner.y);
        }
        minX -= margin; maxX += margin;
        minY -= margin; maxY += margin;

        for (auto& entry : particleObjects) {
            auto object = entry.object;

            bool wanted = !object->m_isGroupDisabled;
            if (wanted) {
                CCPoint position = object->getRealPosition();
                wanted = position.x >= minX && position.x <= maxX
                      && position.y >= minY && position.y <= maxY;
            }

            if (wanted && !entry.claimed) {
                if (!object->m_particle)
                    object->claimParticle();
                entry.claimed = true;
                claimedParticleCount++;
            } else if (!wanted && entry.claimed) {
                if (object->m_particle)
                    object->unclaimParticle();
                entry.claimed = false;
                claimedParticleCount--;
            }
        }
    }

    layer->updateParticles(dt);
}

void Renderer::releaseObjectParticles() {
    if (claimedParticleCount == 0)
        return;

    for (auto& entry : particleObjects) {
        if (entry.claimed) {
            if (entry.object->m_particle)
                entry.object->unclaimParticle();
            entry.claimed = false;
        }
    }
    claimedParticleCount = 0;
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
