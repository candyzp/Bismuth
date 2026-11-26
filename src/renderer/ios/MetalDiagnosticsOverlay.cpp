#include "MetalDiagnosticsOverlay.hpp"
#include "MetalBackend.hpp"

#include <Geode/Enums.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/cocos/CCDirector.h>
#include <Geode/cocos/cocoa/CCAffineTransform.h>
#include <Geode/cocos/sprite_nodes/CCSprite.h>
#import <OpenGLES/ES3/gl.h>
#import <OpenGLES/ES3/glext.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace geode::prelude;


namespace {
constexpr float DEG_TO_RAD = 0.01745329251994329577f;
constexpr std::uint32_t MAX_GROUP_ID = 9999;
constexpr int IOS_GUIDE_OBJECT_ID = 2016;
constexpr int IOS_KEYFRAME_OBJECT_ID = 3032;

bool shouldIncludeObject(PlayLayer* layer, GameObject* object) {
    if (!layer || !object)
        return false;
    if (object == layer->m_anticheatSpike || object == layer->m_endPortal)
        return false;
    if (object->m_isHide)
        return false;
    if (object->m_objectID == IOS_GUIDE_OBJECT_ID ||
        object->m_objectID == IOS_KEYFRAME_OBJECT_ID)
        return false;
    return true;
}

struct ExtractedSprite {
    cocos2d::CCSprite* sprite = nullptr;
    GameObject* object = nullptr;
    cocos2d::CCAffineTransform transform{};
    std::uint32_t objectIndex = 0;
    std::int32_t zLayer = 0;
    std::int32_t zOrder = 0;
    std::uint64_t sequence = 0;
    bool additive = false;
};

using SpriteReceiver = std::function<void(cocos2d::CCSprite*, const cocos2d::CCAffineTransform&, int)>;

void unpackSpriteTree(
    GameObject* object,
    cocos2d::CCSprite* sprite,
    cocos2d::CCAffineTransform transform,
    int type,
    const SpriteReceiver& receive
) {
    if (!object || !sprite)
        return;

    const float savedRotationX = object->getRotationX();
    const float savedRotationY = object->getRotationY();
    object->setRotationX(object->m_startRotationX);
    object->setRotationY(object->m_startRotationY);
    transform = CCAffineTransformConcat(sprite->nodeToParentTransform(), transform);
    object->setRotationX(savedRotationX);
    object->setRotationY(savedRotationY);

    if (sprite == object->m_colorSprite)
        type = 1;
    if (sprite == object->m_glowSprite)
        type = 3;

    std::vector<cocos2d::CCSprite*> spriteChildren;
    for (auto child : CCArrayExt<cocos2d::CCNode*, false>(sprite->getChildren())) {
        if (auto childSprite = typeinfo_cast<cocos2d::CCSprite*>(child))
            spriteChildren.push_back(childSprite);
    }

    for (auto child : spriteChildren) {
        if (child->getZOrder() < 0)
            unpackSpriteTree(object, child, transform, type, receive);
    }

    if (!sprite->getDontDraw())
        receive(sprite, transform, type);

    for (auto child : spriteChildren) {
        if (child->getZOrder() >= 0)
            unpackSpriteTree(object, child, transform, type, receive);
    }
}

void unpackGameObject(GameObject* object, const SpriteReceiver& receive) {
    if (!object)
        return;

    const auto identity = CCAffineTransformMakeIdentity();
    auto* colorSprite = object->m_colorSprite;
    const bool separateColorSprite =
        colorSprite && colorSprite->getParent() != object;
    const bool colorSpriteInFront = object->m_colorZLayerRelated;

    if (object->m_glowSprite)
        unpackSpriteTree(object, object->m_glowSprite, identity, 3, receive);
    if (separateColorSprite && !colorSpriteInFront)
        unpackSpriteTree(object, colorSprite, identity, 1, receive);
    unpackSpriteTree(object, object, identity, 0, receive);
    if (separateColorSprite && colorSpriteInFront)
        unpackSpriteTree(object, colorSprite, identity, 1, receive);
}

bool readTexturePixels(cocos2d::CCTexture2D* texture, MetalTextureInput& output) {
    if (!texture || !texture->getName())
        return false;

    const auto width = static_cast<std::uint32_t>(texture->getPixelsWide());
    const auto height = static_cast<std::uint32_t>(texture->getPixelsHigh());
    if (width == 0 || height == 0)
        return false;

    GLint previousFramebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);

    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    if (framebuffer == 0)
        return false;

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        texture->getName(),
        0
    );
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
        glDeleteFramebuffers(1, &framebuffer);
        return false;
    }

    output.width = width;
    output.height = height;
    output.rgba.resize(static_cast<std::size_t>(width) * height * 4u);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(
        0, 0,
        static_cast<GLsizei>(width),
        static_cast<GLsizei>(height),
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        output.rgba.data()
    );
    const GLenum readError = glGetError();

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
    glDeleteFramebuffers(1, &framebuffer);

    return readError == GL_NO_ERROR;
}
} // namespace

MetalDiagnosticsOverlay* MetalDiagnosticsOverlay::create(PlayLayer* layer) {
    auto ret = new MetalDiagnosticsOverlay();
    if (ret && ret->init(layer)) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool MetalDiagnosticsOverlay::init(PlayLayer* layer) {
    if (!CCNode::init() || !layer)
        return false;

    m_layer = layer;

    const auto winSize = CCDirector::get()->getWinSize();
    setAnchorPoint({0, 1});
    // Keep diagnostic text below the built-in percentage/attempt HUD.
    setPosition({8.0f, winSize.height - 132.0f});
    setZOrder(100000);

    m_label = CCLabelBMFont::create("", "chatFont.fnt");
    if (!m_label)
        return false;
    m_label->setAnchorPoint({0, 1});
    m_label->setPosition({6.0f, -5.0f});
    m_label->setScale(0.42f);
    addChild(m_label, 1);

    scheduleUpdate();
    MetalBackend::get().initialize();
    uploadLevelObjects();
    if (m_levelUploaded)
        uploadSpriteAssets();
    refreshText();
    return true;
}

bool MetalDiagnosticsOverlay::uploadLevelObjects() {
    if (!m_layer || !m_layer->m_objects)
        return false;

    std::vector<MetalObjectInput> inputs;
    inputs.reserve(m_layer->m_objects->count());

    for (auto object : CCArrayExt<GameObject*, false>(m_layer->m_objects)) {
        if (!shouldIncludeObject(m_layer, object))
            continue;

        const auto size = object->getContentSize();
        MetalObjectInput input;
        input.x = object->m_startPosition.x;
        input.y = object->m_startPosition.y;
        input.halfWidth = std::max(
            1.0f,
            std::abs(size.width * object->m_startScaleX) * 0.5f
        );
        input.halfHeight = std::max(
            1.0f,
            std::abs(size.height * object->m_startScaleY) * 0.5f
        );
        input.rotationRadians = object->m_startRotationX * DEG_TO_RAD;

        const std::uint32_t groupCount = static_cast<std::uint32_t>(
            std::clamp<int>(object->m_groupCount, 0, 10)
        );
        for (std::uint32_t i = 0; i < groupCount; ++i) {
            if (!object->m_groups)
                break;
            const int groupId = (*object->m_groups)[i];
            if (groupId < 0 || static_cast<std::uint32_t>(groupId) > MAX_GROUP_ID)
                continue;

            input.groupIds[input.groupCount++] = static_cast<std::uint32_t>(groupId);
        }

        inputs.push_back(input);
    }

    m_levelUploaded = MetalBackend::get().uploadLevelObjects(inputs);
    m_levelUploadAttempted = m_levelUploaded;
    if (!m_levelUploaded) {
        const auto state = MetalBackend::get().snapshot();
        if (state.error.find("deferred because a culling frame is still in flight") == std::string::npos)
            m_levelUploadAttempted = true;
    }
    const auto uploadedState = MetalBackend::get().snapshot();
    log::info(
        "Kyanite iOS: uploaded {} static object records across {} group combinations",
        inputs.size(),
        uploadedState.groupCombinationCount
    );
    return m_levelUploaded;
}


bool MetalDiagnosticsOverlay::uploadSpriteAssets() {
    m_spriteUploadAttempted = true;
    if (!m_layer || !m_layer->m_objects || !m_levelUploaded)
        return false;

    std::unordered_map<GameObject*, std::uint32_t> objectIndices;
    std::uint32_t nextObjectIndex = 0;
    for (auto object : CCArrayExt<GameObject*, false>(m_layer->m_objects)) {
        if (!shouldIncludeObject(m_layer, object))
            continue;
        objectIndices.emplace(object, nextObjectIndex++);
    }

    const auto backendState = MetalBackend::get().snapshot();
    if (nextObjectIndex != backendState.levelObjectCount) {
        log::error(
            "Kyanite iOS: sprite extraction object order mismatch ({} vs {})",
            nextObjectIndex,
            backendState.levelObjectCount
        );
        return false;
    }

    std::vector<ExtractedSprite> extracted;
    std::uint64_t sequence = 0;
    for (auto object : CCArrayExt<GameObject*, false>(m_layer->m_objects)) {
        auto foundIndex = objectIndices.find(object);
        if (foundIndex == objectIndices.end())
            continue;

        unpackGameObject(object, [&](CCSprite* sprite,
                                     const CCAffineTransform& transform,
                                     int spriteType) {
            if (!sprite || !sprite->getTexture())
                return;
            if (spriteType == 3 && object->m_hasNoGlow)
                return;

            extracted.push_back({
                sprite,
                object,
                transform,
                foundIndex->second,
                static_cast<std::int32_t>(object->getObjectZLayer()),
                object->getObjectZOrder(),
                sequence++,
                spriteType == 3 || object->m_baseOrDetailBlending
            });
        });
    }

    if (extracted.empty()) {
        log::error("Kyanite iOS: no renderable sprites were extracted from this level");
        return false;
    }

    std::stable_sort(extracted.begin(), extracted.end(),
        [](const ExtractedSprite& lhs, const ExtractedSprite& rhs) {
            if (lhs.zLayer != rhs.zLayer)
                return lhs.zLayer < rhs.zLayer;
            if (lhs.zOrder != rhs.zOrder)
                return lhs.zOrder < rhs.zOrder;
            return lhs.sequence < rhs.sequence;
        });

    std::unordered_map<GLuint, std::uint32_t> textureIndices;
    std::vector<MetalTextureInput> textures;
    std::vector<MetalSpriteVertex> vertices;
    std::vector<MetalSpriteBatch> batches;
    vertices.reserve(extracted.size() * 6u);
    textures.reserve(8);

    const float contentScaleFactor = CCDirector::get()->getContentScaleFactor();
    for (const auto& item : extracted) {
        auto* texture = item.sprite->getTexture();
        const GLuint textureName = texture ? texture->getName() : 0;
        if (!texture || textureName == 0)
            continue;

        auto textureIt = textureIndices.find(textureName);
        std::uint32_t textureIndex = 0;
        if (textureIt == textureIndices.end()) {
            MetalTextureInput textureInput;
            if (!readTexturePixels(texture, textureInput)) {
                log::error(
                    "Kyanite iOS: failed to read sprite atlas {}x{} from OpenGL",
                    texture->getPixelsWide(),
                    texture->getPixelsHigh()
                );
                return false;
            }
            textureIndex = static_cast<std::uint32_t>(textures.size());
            textures.push_back(std::move(textureInput));
            textureIndices.emplace(textureName, textureIndex);
        } else {
            textureIndex = textureIt->second;
        }

        const CCRect crop = item.sprite->getTextureRect();
        const auto& transform = item.transform;

        CCPoint positionBottomLeft =
            CCPointApplyAffineTransform(item.sprite->getOffsetPosition(), transform);
        CCPoint positionRight(
            transform.a * crop.size.width,
            transform.b * crop.size.width
        );
        CCPoint positionUp(
            transform.c * crop.size.height,
            transform.d * crop.size.height
        );

        CCPoint texBottomLeft;
        CCPoint texRight;
        CCPoint texUp;
        if (!item.sprite->isTextureRectRotated()) {
            texBottomLeft = {crop.origin.x, crop.origin.y + crop.size.height};
            texRight = {crop.size.width, 0.0f};
            texUp = {0.0f, -crop.size.height};
        } else {
            texBottomLeft = {crop.origin.x, crop.origin.y};
            texRight = {0.0f, crop.size.width};
            texUp = {crop.size.height, 0.0f};
        }

        if (item.sprite->isFlipX()) {
            positionBottomLeft += positionRight;
            positionRight = ccp(-positionRight.x, -positionRight.y);
        }
        if (item.sprite->isFlipY()) {
            positionBottomLeft += positionUp;
            positionUp = ccp(-positionUp.x, -positionUp.y);
        }

        const float texScaleX =
            contentScaleFactor / static_cast<float>(texture->getPixelsWide());
        const float texScaleY =
            contentScaleFactor / static_cast<float>(texture->getPixelsHigh());
        texBottomLeft = ccp(texBottomLeft.x * texScaleX, texBottomLeft.y * texScaleY);
        texRight = ccp(texRight.x * texScaleX, texRight.y * texScaleY);
        texUp = ccp(texUp.x * texScaleX, texUp.y * texScaleY);

        const auto spriteColor = item.sprite->getColor();
        const float red = spriteColor.r / 255.0f;
        const float green = spriteColor.g / 255.0f;
        const float blue = spriteColor.b / 255.0f;
        const float alpha = item.sprite->getOpacity() / 255.0f;

        const CCPoint quad[] = {
            {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f},
            {1.0f, 1.0f}, {0.0f, 1.0f}, {0.0f, 0.0f}
        };

        const bool newBatch = batches.empty() ||
            batches.back().textureIndex != textureIndex ||
            batches.back().additive != item.additive;
        if (newBatch) {
            batches.push_back({
                static_cast<std::uint32_t>(vertices.size()),
                0u,
                textureIndex,
                item.additive
            });
        }

        for (const auto& point : quad) {
            const float worldOffsetX =
                positionBottomLeft.x + positionRight.x * point.x +
                positionUp.x * point.y - item.object->m_startPosition.x;
            const float worldOffsetY =
                positionBottomLeft.y + positionRight.y * point.x +
                positionUp.y * point.y - item.object->m_startPosition.y;
            const float u =
                texBottomLeft.x + texRight.x * point.x + texUp.x * point.y;
            const float v =
                texBottomLeft.y + texRight.y * point.x + texUp.y * point.y;

            vertices.push_back({
                worldOffsetX,
                worldOffsetY,
                u,
                v,
                item.objectIndex,
                red,
                green,
                blue,
                alpha
            });
            batches.back().vertexCount++;
        }
    }

    if (vertices.empty() || textures.empty() || batches.empty()) {
        log::error("Kyanite iOS: sprite geometry or texture atlas upload produced no data");
        return false;
    }

    auto& backend = MetalBackend::get();
    if (!backend.uploadSpriteGeometry(vertices, batches, textures))
        return false;
    if (!backend.attachSpriteView())
        return false;

    if (m_layer->m_objectLayer && m_layer->m_objectLayer->isVisible()) {
        m_originalObjectLayerVisible = true;
        m_layer->m_objectLayer->setVisible(false);
        m_stockLayerHidden = true;
    }

    m_spritesUploaded = true;
    log::info(
        "Kyanite iOS: Metal sprite renderer attached ({} sprites, {} vertices, {} atlases, {} draw batches)",
        extracted.size(),
        vertices.size(),
        textures.size(),
        batches.size()
    );
    return true;
}

void MetalDiagnosticsOverlay::submitVisibilityWork() {
    if (!m_layer || !m_layer->m_objectLayer || !m_levelUploaded || !m_spritesUploaded)
        return;

    const auto winSize = CCDirector::get()->getWinSize();
    auto* objectLayer = m_layer->m_objectLayer;

    const CCPoint corners[] = {
        objectLayer->convertToNodeSpace({0.0f, 0.0f}),
        objectLayer->convertToNodeSpace({winSize.width, 0.0f}),
        objectLayer->convertToNodeSpace({0.0f, winSize.height}),
        objectLayer->convertToNodeSpace({winSize.width, winSize.height})
    };

    float minX = corners[0].x;
    float maxX = corners[0].x;
    float minY = corners[0].y;
    float maxY = corners[0].y;
    for (const auto& point : corners) {
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
    }

    auto& backend = MetalBackend::get();
    // Group opacity is not queried here because the corresponding game
    // method is not exported by the iOS binary. Keep the safe default alpha.
    backend.updateCameraBounds(minX, minY, maxX, maxY);
}

void MetalDiagnosticsOverlay::update(float dt) {
    const bool enabled = Mod::get()->getSettingValue<bool>("enabled");
    const bool runWork = Mod::get()->getSettingValue<bool>("run_metal_probe");
    const bool showOverlay =
        Mod::get()->getSettingValue<bool>("diagnostics_overlay");
    setVisible(showOverlay);

    if (enabled && runWork) {
        if (!m_probeRequested) {
            m_probeRequested = true;
            MetalBackend::get().submitComputeProbeOnce();
        }

        if (!m_levelUploadAttempted)
            uploadLevelObjects();
        if (m_levelUploaded && !m_spriteUploadAttempted)
            uploadSpriteAssets();

        auto& backend = MetalBackend::get();
        if (m_spritesUploaded && !backend.snapshot().spriteViewAttached) {
            if (backend.attachSpriteView() && m_layer && m_layer->m_objectLayer) {
                m_originalObjectLayerVisible = m_layer->m_objectLayer->isVisible();
                m_layer->m_objectLayer->setVisible(false);
                m_stockLayerHidden = true;
            }
        }
        submitVisibilityWork();
    } else {
        if (m_stockLayerHidden && m_layer && m_layer->m_objectLayer) {
            m_layer->m_objectLayer->setVisible(m_originalObjectLayerVisible);
            m_stockLayerHidden = false;
        }
        MetalBackend::get().detachSpriteView();
    }

    m_refreshElapsed += dt;
    if (m_refreshElapsed < 0.20f)
        return;

    m_refreshElapsed = 0.0f;
    if (showOverlay)
        refreshText();
}

void MetalDiagnosticsOverlay::refreshText() {
    const bool enabled = Mod::get()->getSettingValue<bool>("enabled");
    const bool runWork = Mod::get()->getSettingValue<bool>("run_metal_probe");
    const auto state = MetalBackend::get().snapshot();

    std::string text;
    text += "KYANITE / iOS METAL PORT\n";

    if (!enabled) {
        text += "Metal workload: DISABLED IN SETTINGS\n";
    } else if (!runWork) {
        text += "Metal workload: DISABLED IN SETTINGS\n";
    } else {
        text += fmt::format(
            "Metal device: {} | queue: {}\n",
            state.deviceReady ? "READY" : "NOT READY",
            state.commandQueueReady ? "READY" : "NOT READY"
        );
        if (!state.deviceName.empty())
            text += fmt::format("GPU: {}\n", state.deviceName);

        text += fmt::format(
            "Compute pipelines: probe {} / culling {}\n",
            state.computePipelineReady ? "READY" : "NOT READY",
            state.cullingPipelineReady ? "READY" : "NOT READY"
        );

        if (state.probeCompleted) {
            text += fmt::format(
                "Probe: COMPLETE ({}/{})\n",
                state.completedProbeThreads,
                state.expectedProbeThreads
            );
            if (state.completedProbeThreads != state.expectedProbeThreads)
                text += "Probe result: MISMATCH\n";
        } else if (state.probeSubmitted) {
            text += "Probe: SUBMITTED / WAITING\n";
        } else if (state.probeFailed) {
            text += "Probe: FAILED\n";
        } else {
            text += "Probe: NOT SUBMITTED\n";
        }

        if (!m_levelUploadAttempted) {
            text += "Level object data: WAITING\n";
        } else if (!m_levelUploaded || !state.levelDataReady) {
            text += "Level object data: UPLOAD FAILED\n";
        } else {
            text += fmt::format(
                "Static objects: {} | group combinations: {}\n",
                state.levelObjectCount,
                state.groupCombinationCount
            );
            if (state.cullingCompleted) {
                text += fmt::format(
                    "GPU visibility: {} visible / {} culled\n",
                    state.visibleObjectCount,
                    state.culledObjectCount
                );
                text += fmt::format(
                    "Culling frames: {} completed / {} submitted\n",
                    state.completedCullingFrames,
                    state.submittedCullingFrames
                );
                if (state.cullingGpuTimeAvailable) {
                    text += fmt::format(
                        "Culling GPU time: {:.3f} ms\n",
                        state.cullingGpuTimeMilliseconds
                    );
                } else {
                    text += "Culling GPU time: unavailable\n";
                }
            } else if (state.cullingSubmitted) {
                text += "GPU visibility: SUBMITTED / WAITING\n";
            } else if (state.cullingFailed) {
                text += "GPU visibility: FAILED\n";
            } else {
                text += "GPU visibility: NOT SUBMITTED\n";
            }

            text += fmt::format(
                "Group-state updates: {} | skipped busy frames: {}\n",
                state.groupStateUpdates,
                state.skippedBusyFrames
            );
            if (state.invalidVisibleIndices != 0) {
                text += fmt::format(
                    "WARNING: {} invalid GPU indices\n",
                    state.invalidVisibleIndices
                );
            }
        }
    }

    if (!state.error.empty())
        text += fmt::format("Metal error: {}\n", state.error);

    text += fmt::format(
        "Metal sprite pipeline: {} | geometry: {}\n",
        state.spritePipelineReady ? "READY" : "NOT READY",
        state.spriteGeometryReady ? "READY" : "NOT READY"
    );
    text += fmt::format(
        "Metal view: {} | rendered frames: {}\n",
        state.spriteViewAttached ? "ATTACHED" : "NOT ATTACHED",
        state.renderedFrames
    );
    text += m_stockLayerHidden
        ? "Active level renderer: METAL SPRITE PASS\n"
        : "Active level renderer: STOCK GD (fallback)\n";

    m_label->setString(text.c_str());
}
