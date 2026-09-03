#ifdef GEODE_IS_IOS

#include "ResolvedStateLayer.hpp"
#include "../../ObjectUtils.hpp"

#include <Geode/binding/CheckpointGameObject.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace geode::prelude;

namespace {
constexpr usize OBJECT_TEXELS_PER_STATE = 2;
constexpr usize SPRITE_TEXELS_PER_STATE = 3;
constexpr usize MAX_SAFE_SPRITES_PER_OBJECT = 12;

inline bool changedFloat(float a, float b, float epsilon = 0.0001f) {
    return std::abs(a - b) > epsilon;
}

inline bool rectChanged(const cocos2d::CCRect& a, const cocos2d::CCRect& b) {
    return changedFloat(a.origin.x, b.origin.x) ||
           changedFloat(a.origin.y, b.origin.y) ||
           changedFloat(a.size.width, b.size.width) ||
           changedFloat(a.size.height, b.size.height);
}
} // namespace

ResolvedStateLayer::~ResolvedStateLayer() {
    destroyTextures();
}

void ResolvedStateLayer::destroyTextures() {
    if (objectStateTexture)
        DataTexture::destroy(objectStateTexture);
    if (spriteStateTexture)
        DataTexture::destroy(spriteStateTexture);
    objectStateTexture = nullptr;
    spriteStateTexture = nullptr;
}

ResolvedStateLayer::SafetyClass ResolvedStateLayer::classifyObject(
    GameObject* object,
    std::vector<cocos2d::CCSprite*>& outSprites
) const {
    if (!object || object->isTrigger() || object->m_isHide || object->m_isInvisible)
        return SafetyClass::StockOnly;

    // These classes are exactly where the previous replacement renderer became
    // fragile: their child trees, frames, activation state, or interaction art
    // can change independently of the root GameObject.
    if (object->m_classType == GameObjectClassType::Animated)
        return SafetyClass::StockOnly;
    if (ObjectUtils::isInteractiveVisualObject(object))
        return SafetyClass::StockOnly;
    if (typeinfo_cast<CheckpointGameObject*>(object))
        return SafetyClass::StockOnly;
    if (object->getHasSyncedAnimation())
        return SafetyClass::StockOnly;
    if (object->m_isInvisibleBlock)
        return SafetyClass::StockOnly;

    bool invalidSprite = false;
    ObjectUtils::unpackObjectIntoSprites(object, [&](const UnpackedSprite& unpacked) {
        if (!unpacked.sprite || !unpacked.sprite->getTexture()) {
            invalidSprite = true;
            return;
        }
        outSprites.push_back(unpacked.sprite);
    });

    if (invalidSprite || outSprites.empty() || outSprites.size() > MAX_SAFE_SPRITES_PER_OBJECT)
        return SafetyClass::StockOnly;

    // GD remains authoritative either way. "Dynamic" only means this object's
    // resolved root state is expected to change and therefore needs dirty checks
    // once the GPU draw path starts consuming the state texture.
    const bool dynamic =
        object->m_groupCount > 0 ||
        object->getHasRotateAction() ||
        object->m_usesAudioScale;

    return dynamic ? SafetyClass::DynamicSafe : SafetyClass::StaticSafe;
}

ResolvedStateLayer::ObjectState ResolvedStateLayer::captureObjectState(GameObject* object) const {
    ObjectState state;
    if (!object)
        return state;

    const auto pos = object->getPosition();
    state.position = { pos.x, pos.y };
    state.rotation = object->getRotation();
    state.scaleX = object->getScaleX();
    state.scaleY = object->getScaleY();
    state.opacity = (float)object->getOpacity() / 255.f;
    state.visible = object->isVisible() && !object->m_isInvisible;
    return state;
}

ResolvedStateLayer::SpriteState ResolvedStateLayer::captureSpriteState(cocos2d::CCSprite* sprite) const {
    SpriteState state;
    if (!sprite)
        return state;

    state.color = sprite->getColor();
    state.opacity = sprite->getOpacity();
    state.textureRect = sprite->getTextureRect();
    state.visible = sprite->isVisible();
    state.rotated = sprite->isTextureRectRotated();
    state.flipX = sprite->isFlipX();
    state.flipY = sprite->isFlipY();

    if (auto texture = sprite->getTexture()) {
        state.textureId = texture->getName();
        state.textureWidth = std::max(1.f, (float)texture->getPixelsWide());
        state.textureHeight = std::max(1.f, (float)texture->getPixelsHigh());
    }

    return state;
}

void ResolvedStateLayer::packObjectState(usize index, const ObjectState& state, SafetyClass safety) {
    const usize base = index * OBJECT_TEXELS_PER_STATE;
    if (base + 1 >= objectTexels.size())
        return;

    // The future assist vertex shader consumes only final GD-resolved root state.
    // It does not know about Move/Rotate/Follow/Area trigger semantics.
    objectTexels[base + 0] = {
        state.position.x,
        state.position.y,
        state.rotation,
        state.scaleX
    };
    objectTexels[base + 1] = {
        state.scaleY,
        state.opacity,
        state.visible ? 1.f : 0.f,
        safety == SafetyClass::DynamicSafe ? 1.f : 0.f
    };
}

void ResolvedStateLayer::packSpriteState(usize index, const SpriteState& state, usize objectIndex) {
    const usize base = index * SPRITE_TEXELS_PER_STATE;
    if (base + 2 >= spriteTexels.size())
        return;

    spriteTexels[base + 0] = {
        (float)state.color.r / 255.f,
        (float)state.color.g / 255.f,
        (float)state.color.b / 255.f,
        (float)state.opacity / 255.f
    };
    spriteTexels[base + 1] = {
        state.textureRect.origin.x,
        state.textureRect.origin.y,
        state.textureRect.size.width,
        state.textureRect.size.height
    };

    u32 flags = 0;
    if (state.visible) flags |= 1u;
    if (state.rotated) flags |= 2u;
    if (state.flipX) flags |= 4u;
    if (state.flipY) flags |= 8u;

    spriteTexels[base + 2] = {
        (float)flags,
        (float)objectIndex,
        state.textureWidth,
        state.textureHeight
    };
}

bool ResolvedStateLayer::transformChanged(const ObjectState& a, const ObjectState& b) {
    return changedFloat(a.position.x, b.position.x) ||
           changedFloat(a.position.y, b.position.y) ||
           changedFloat(a.rotation, b.rotation) ||
           changedFloat(a.scaleX, b.scaleX) ||
           changedFloat(a.scaleY, b.scaleY);
}

bool ResolvedStateLayer::objectAppearanceChanged(const ObjectState& a, const ObjectState& b) {
    return changedFloat(a.opacity, b.opacity);
}

bool ResolvedStateLayer::spriteAppearanceChanged(const SpriteState& a, const SpriteState& b) {
    return a.color.r != b.color.r ||
           a.color.g != b.color.g ||
           a.color.b != b.color.b ||
           a.opacity != b.opacity;
}

bool ResolvedStateLayer::spriteUVChanged(const SpriteState& a, const SpriteState& b) {
    return rectChanged(a.textureRect, b.textureRect) ||
           a.textureId != b.textureId ||
           a.rotated != b.rotated ||
           a.flipX != b.flipX ||
           a.flipY != b.flipY;
}

bool ResolvedStateLayer::init(PlayLayer* playLayer) {
    destroyTextures();
    layer = playLayer;
    objects.clear();
    sprites.clear();
    objectTexels.clear();
    spriteTexels.clear();
    stats = {};

    if (!layer || !layer->m_objects)
        return false;

    for (auto object : CCArrayExt<GameObject*>(layer->m_objects)) {
        if (!object || object == layer->m_anticheatSpike || object->isTrigger() || object->m_isHide)
            continue;

        ++stats.renderableObjects;

        std::vector<cocos2d::CCSprite*> objectSprites;
        const SafetyClass safety = classifyObject(object, objectSprites);
        if (safety == SafetyClass::StockOnly) {
            ++stats.stockObjects;
            continue;
        }

        ObjectRecord record;
        record.object = object;
        record.safety = safety;
        record.firstSprite = sprites.size();
        record.spriteCount = objectSprites.size();
        record.state = captureObjectState(object);

        const usize objectIndex = objects.size();
        objects.push_back(record);

        ++stats.safeObjects;
        if (safety == SafetyClass::StaticSafe)
            ++stats.staticObjects;
        else
            ++stats.dynamicObjects;

        for (auto sprite : objectSprites) {
            SpriteRecord spriteRecord;
            spriteRecord.sprite = sprite;
            spriteRecord.objectIndex = objectIndex;
            spriteRecord.state = captureSpriteState(sprite);
            sprites.push_back(spriteRecord);
            ++stats.safeSprites;
        }
    }

    if (objects.empty() || sprites.empty()) {
        log::info(
            "Bismuth iOS state layer: no conservative GPU-safe objects ({} stock)",
            stats.stockObjects
        );
        return true;
    }

    objectTexels.resize(objects.size() * OBJECT_TEXELS_PER_STATE);
    spriteTexels.resize(sprites.size() * SPRITE_TEXELS_PER_STATE);

    objectStateTexture = DataTexture::create(
        "Resolved object state",
        objectTexels.size(),
        DataTexture::Type::FloatRGBA
    );
    spriteStateTexture = DataTexture::create(
        "Resolved sprite state",
        spriteTexels.size(),
        DataTexture::Type::FloatRGBA
    );

    if (!objectStateTexture || !spriteStateTexture) {
        log::warn("Bismuth iOS resolved-state textures unavailable; stock GD rendering remains active");
        destroyTextures();
        return false;
    }

    resync();

    log::info(
        "Bismuth iOS state layer: {} safe objects ({} static, {} dynamic), {} stock, {} sprite records",
        stats.safeObjects,
        stats.staticObjects,
        stats.dynamicObjects,
        stats.stockObjects,
        stats.safeSprites
    );
    return true;
}

void ResolvedStateLayer::resync() {
    if (!objectStateTexture || !spriteStateTexture)
        return;

    for (usize i = 0; i < objects.size(); ++i) {
        auto& record = objects[i];
        record.state = captureObjectState(record.object);
        packObjectState(i, record.state, record.safety);
    }

    for (usize i = 0; i < sprites.size(); ++i) {
        auto& record = sprites[i];
        record.state = captureSpriteState(record.sprite);
        packSpriteState(i, record.state, record.objectIndex);
    }

    if (!objectTexels.empty())
        objectStateTexture->upload(objectTexels.data(), objectTexels.size());
    if (!spriteTexels.empty())
        spriteStateTexture->upload(spriteTexels.data(), spriteTexels.size());
}

void ResolvedStateLayer::update(bool detailedProbe) {
    stats.dirtyTransforms = 0;
    stats.dirtyAppearance = 0;
    stats.dirtyVisibility = 0;
    stats.dirtyUVs = 0;
    stats.staticObjectsReused = stats.staticObjects;
    stats.bytesUploaded = 0;
    stats.uploadCalls = 0;

    // Until a validated GPU batch consumes the textures, do not add a full
    // per-frame object walk to normal gameplay merely to make GPU statistics
    // look busy. Debug mode performs the expensive proof/measurement pass.
    if (!detailedProbe || !objectStateTexture || !spriteStateTexture)
        return;

    const usize NONE = std::numeric_limits<usize>::max();
    usize objectDirtyMin = NONE;
    usize objectDirtyMax = 0;
    usize spriteDirtyMin = NONE;
    usize spriteDirtyMax = 0;
    std::vector<bool> staticTouched(objects.size(), false);

    for (usize i = 0; i < objects.size(); ++i) {
        auto& record = objects[i];
        const ObjectState next = captureObjectState(record.object);

        const bool transformDirty = transformChanged(record.state, next);
        const bool appearanceDirty = objectAppearanceChanged(record.state, next);
        const bool visibilityDirty = record.state.visible != next.visible;

        if (transformDirty)
            ++stats.dirtyTransforms;
        if (appearanceDirty)
            ++stats.dirtyAppearance;
        if (visibilityDirty)
            ++stats.dirtyVisibility;

        if (transformDirty || appearanceDirty || visibilityDirty) {
            if (record.safety == SafetyClass::StaticSafe)
                staticTouched[i] = true;

            record.state = next;
            packObjectState(i, record.state, record.safety);
            objectDirtyMin = std::min(objectDirtyMin, i);
            objectDirtyMax = std::max(objectDirtyMax, i);
        }
    }

    for (usize i = 0; i < sprites.size(); ++i) {
        auto& record = sprites[i];
        const SpriteState next = captureSpriteState(record.sprite);

        const bool appearanceDirty = spriteAppearanceChanged(record.state, next);
        const bool visibilityDirty = record.state.visible != next.visible;
        const bool uvDirty = spriteUVChanged(record.state, next);

        if (appearanceDirty)
            ++stats.dirtyAppearance;
        if (visibilityDirty)
            ++stats.dirtyVisibility;
        if (uvDirty)
            ++stats.dirtyUVs;

        if (appearanceDirty || visibilityDirty || uvDirty) {
            if (record.objectIndex < objects.size() &&
                objects[record.objectIndex].safety == SafetyClass::StaticSafe) {
                staticTouched[record.objectIndex] = true;
            }

            record.state = next;
            packSpriteState(i, record.state, record.objectIndex);
            spriteDirtyMin = std::min(spriteDirtyMin, i);
            spriteDirtyMax = std::max(spriteDirtyMax, i);
        }
    }

    usize touchedStaticCount = 0;
    for (bool touched : staticTouched)
        if (touched)
            ++touchedStaticCount;
    stats.staticObjectsReused = stats.staticObjects > touchedStaticCount
        ? stats.staticObjects - touchedStaticCount
        : 0;

    if (objectDirtyMin != NONE) {
        const usize startTexel = objectDirtyMin * OBJECT_TEXELS_PER_STATE;
        const usize endTexel = (objectDirtyMax + 1) * OBJECT_TEXELS_PER_STATE;
        const usize texelCount = endTexel - startTexel;
        if (objectStateTexture->uploadRange(
            objectTexels.data() + startTexel,
            startTexel,
            texelCount
        )) {
            stats.bytesUploaded += texelCount * sizeof(glm::vec4);
            ++stats.uploadCalls;
        }
    }

    if (spriteDirtyMin != NONE) {
        const usize startTexel = spriteDirtyMin * SPRITE_TEXELS_PER_STATE;
        const usize endTexel = (spriteDirtyMax + 1) * SPRITE_TEXELS_PER_STATE;
        const usize texelCount = endTexel - startTexel;
        if (spriteStateTexture->uploadRange(
            spriteTexels.data() + startTexel,
            startTexel,
            texelCount
        )) {
            stats.bytesUploaded += texelCount * sizeof(glm::vec4);
            ++stats.uploadCalls;
        }
    }
}

#endif
