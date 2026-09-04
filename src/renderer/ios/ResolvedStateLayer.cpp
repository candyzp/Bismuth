#ifdef GEODE_IS_IOS

#include "ResolvedStateLayer.hpp"
#include "../../ObjectUtils.hpp"

#include <Geode/binding/CheckpointGameObject.hpp>
#include <Geode/utils/cocos.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

using namespace geode::prelude;

namespace {
constexpr usize OBJECT_TEXELS_PER_STATE = 2;
constexpr usize SPRITE_TEXELS_PER_STATE = 3;

// Merging a tiny gap costs fewer bytes than another GL call, but a distant dirty
// record should never drag the entire data texture across the bus again.
constexpr usize DIRTY_RECORD_MERGE_GAP = 2;

inline bool changedFloat(float a, float b, float epsilon = 0.0001f) {
    return std::abs(a - b) > epsilon;
}

inline bool rectChanged(const cocos2d::CCRect& a, const cocos2d::CCRect& b) {
    return changedFloat(a.origin.x, b.origin.x) ||
           changedFloat(a.origin.y, b.origin.y) ||
           changedFloat(a.size.width, b.size.width) ||
           changedFloat(a.size.height, b.size.height);
}

void uploadDirtyRecordSpans(
    DataTexture* texture,
    const std::vector<glm::vec4>& texels,
    const std::vector<usize>& dirtyRecords,
    usize texelsPerRecord,
    ResolvedStateLayer::Stats& stats
) {
    if (!texture || dirtyRecords.empty() || texelsPerRecord == 0)
        return;

    auto uploadSpan = [&](usize recordStart, usize recordEnd) {
        const usize startTexel = recordStart * texelsPerRecord;
        const usize endTexel = (recordEnd + 1) * texelsPerRecord;
        const usize texelCount = endTexel - startTexel;
        if (startTexel >= texels.size() || endTexel > texels.size())
            return;

        if (texture->uploadRange(
            texels.data() + startTexel,
            startTexel,
            texelCount
        )) {
            stats.bytesUploaded += texelCount * sizeof(glm::vec4);
            ++stats.uploadCalls;
        }
    };

    usize spanStart = dirtyRecords.front();
    usize spanEnd = spanStart;

    for (usize i = 1; i < dirtyRecords.size(); ++i) {
        const usize next = dirtyRecords[i];
        if (next <= spanEnd + 1 + DIRTY_RECORD_MERGE_GAP) {
            spanEnd = next;
            continue;
        }

        uploadSpan(spanStart, spanEnd);
        spanStart = spanEnd = next;
    }

    uploadSpan(spanStart, spanEnd);
}
} // namespace

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
    std::vector<cocos2d::CCSprite*>& outSprites,
    CollectionDiagnostics& diagnostics
) const {
    if (!object || object->isTrigger() || object->m_isHide || object->m_isInvisible)
        return SafetyClass::StockOnly;

    // These classes keep the exact stock path. Their sprite trees/frames or
    // interaction visuals can change independently of simple resolved root
    // state, which is precisely where the old replacement renderer broke.
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
    SpriteUnpackStats unpackStats;
    const bool collectionSafe = ObjectUtils::unpackObjectIntoSprites(
        object,
        [&](const UnpackedSprite& unpacked) {
            if (!unpacked.sprite || !unpacked.sprite->getTexture()) {
                invalidSprite = true;
                ++diagnostics.invalidSprites;
                return;
            }
            outSprites.push_back(unpacked.sprite);
        },
        &unpackStats
    );

    diagnostics.invalidChildNodes += unpackStats.nonSpriteChildren;
    diagnostics.duplicateSprites += unpackStats.duplicateSprites;

    if (!collectionSafe || invalidSprite) {
        outSprites.clear();
        diagnostics.unsafeCollection = true;
        return SafetyClass::StockOnly;
    }

    // Child transforms and separately attached visual parts can change without
    // changing the root state. Persistent root geometry cannot represent them.
    if (outSprites.size() != 1 || outSprites.front() != object ||
        object->m_glowSprite || object->m_colorSprite ||
        (object->getChildren() && object->getChildren()->count() != 0))
        return SafetyClass::StockOnly;

    // DynamicSafe is still GD-owned state. It only means the final transform is
    // expected to change, so the state texture will receive dirty updates while
    // the A15 performs the final per-vertex transform math.
    const bool dynamic =
        object->m_groupCount > 0 ||
        object->getHasRotateAction() ||
        object->m_usesAudioScale;

    return dynamic ? SafetyClass::DynamicSafe : SafetyClass::StaticSafe;
}

bool ResolvedStateLayer::isShadowValidationCandidate(
    GameObject* object,
    SafetyClass safety,
    const std::vector<cocos2d::CCSprite*>& objectSprites
) const {
    if (!object || safety != SafetyClass::StaticSafe)
        return false;

    if (object->m_objectType != GameObjectType::Solid)
        return false;
    if (objectSprites.size() != 1)
        return false;
    if (objectSprites[0] != static_cast<cocos2d::CCSprite*>(object))
        return false;
    if (object->m_groupCount != 0 || object->getHasRotateAction() || object->m_usesAudioScale)
        return false;

    return objectSprites[0]->getTexture() != nullptr;
}

ResolvedStateLayer::ObjectState ResolvedStateLayer::captureObjectState(GameObject* object) const {
    ObjectState state;
    if (!object)
        return state;

    state.transform = object->nodeToParentTransform();
    state.vertexZ = object->getVertexZ();
    state.opacity = (float)object->getDisplayedOpacity() / 255.f;
    state.visible = object->isVisible() && !object->m_isInvisible;
    return state;
}

ResolvedStateLayer::SpriteState ResolvedStateLayer::captureSpriteState(cocos2d::CCSprite* sprite) const {
    SpriteState state;
    if (!sprite)
        return state;

    // Consume Cocos' already-resolved display state. Bismuth does not recreate
    // GD color channels, cascade color, cascade opacity, HSV, or fade semantics.
    state.color = sprite->getDisplayedColor();
    state.opacity = sprite->getDisplayedOpacity();
    state.textureRect = sprite->getTextureRect();
    state.offset = sprite->getOffsetPosition();
    state.opacityModifyRGB = sprite->isOpacityModifyRGB();
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

void ResolvedStateLayer::packObjectState(usize index, const ObjectState& state, SafetyClass) {
    const usize base = index * OBJECT_TEXELS_PER_STATE;
    if (base + 1 >= objectTexels.size())
        return;

    objectTexels[base + 0] = {
        state.transform.a,
        state.transform.b,
        state.transform.c,
        state.transform.d
    };
    objectTexels[base + 1] = {
        state.transform.tx,
        state.transform.ty,
        state.vertexZ,
        state.visible ? 1.f : 0.f
    };
}

void ResolvedStateLayer::packSpriteState(usize index, const SpriteState& state, usize objectIndex) {
    const usize base = index * SPRITE_TEXELS_PER_STATE;
    if (base + 2 >= spriteTexels.size())
        return;

    const auto colorByte = [&](u8 value) -> float {
        // Match CCSprite::updateColor's byte truncation for premultiplied RGB.
        const u8 resolved = state.opacityModifyRGB
            ? static_cast<u8>(value * (state.opacity / 255.f)) : value;
        return static_cast<float>(resolved) / 255.f;
    };
    spriteTexels[base + 0] = {
        colorByte(state.color.r),
        colorByte(state.color.g),
        colorByte(state.color.b),
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
    return a.transform.a != b.transform.a || a.transform.b != b.transform.b ||
           a.transform.c != b.transform.c || a.transform.d != b.transform.d ||
           a.transform.tx != b.transform.tx || a.transform.ty != b.transform.ty ||
           a.vertexZ != b.vertexZ;
}

bool ResolvedStateLayer::objectAppearanceChanged(const ObjectState& a, const ObjectState& b) {
    return changedFloat(a.opacity, b.opacity);
}

bool ResolvedStateLayer::spriteAppearanceChanged(const SpriteState& a, const SpriteState& b) {
    return a.color.r != b.color.r ||
           a.color.g != b.color.g ||
           a.color.b != b.color.b ||
           a.opacity != b.opacity || a.opacityModifyRGB != b.opacityModifyRGB;
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
    spriteIndexByPointer.clear();
    eventOwnershipReady = false;
    shadowCandidates.clear();
    objectTexels.clear();
    spriteTexels.clear();
    activeObjectIndices.clear();
    activeSpriteIndices.clear();
    stats = {};

    if (!layer || !layer->m_objects)
        return false;

    // Retain accepted records only across initial collection -> texture creation
    // -> resync. This closes the init lifetime window without pinning visual parts
    // for the whole level or interfering with normal GD ownership afterward.
    std::vector<Ref<GameObject>> initObjectRetains;
    std::vector<Ref<cocos2d::CCSprite>> initSpriteRetains;
    initObjectRetains.reserve(layer->m_objects->count());

    for (auto object : CCArrayExt<GameObject*>(layer->m_objects)) {
        if (!object || object == layer->m_anticheatSpike || object->isTrigger() || object->m_isHide)
            continue;

        ++stats.renderableObjects;

        std::vector<cocos2d::CCSprite*> objectSprites;
        CollectionDiagnostics diagnostics;
        const SafetyClass safety = classifyObject(object, objectSprites, diagnostics);

        stats.invalidChildNodes += diagnostics.invalidChildNodes;
        stats.duplicateSpriteRecords += diagnostics.duplicateSprites;
        stats.invalidSpriteRecords += diagnostics.invalidSprites;
        if (diagnostics.unsafeCollection)
            ++stats.unsafeCollectionObjects;

        if (safety == SafetyClass::StockOnly) {
            ++stats.stockObjects;
            continue;
        }

        // Convert the freshly type-checked raw pointers into strong refs before
        // committing any state records. Revalidate the texture while retained.
        std::vector<Ref<cocos2d::CCSprite>> objectSpriteRetains;
        objectSpriteRetains.reserve(objectSprites.size());
        bool revalidationFailed = false;
        for (auto sprite : objectSprites) {
            if (!sprite) {
                revalidationFailed = true;
                break;
            }

            objectSpriteRetains.emplace_back(sprite);
            auto retainedSprite = objectSpriteRetains.back().data();
            if (!retainedSprite || !retainedSprite->getTexture()) {
                revalidationFailed = true;
                break;
            }
        }

        if (revalidationFailed || objectSpriteRetains.size() != objectSprites.size()) {
            ++stats.initRevalidationFailures;
            ++stats.stockObjects;
            continue;
        }

        initObjectRetains.emplace_back(object);

        ObjectRecord record;
        record.object = object;
        record.safety = safety;
        record.firstSprite = sprites.size();
        record.spriteCount = objectSprites.size();

        const usize objectIndex = objects.size();
        const usize firstSpriteIndex = sprites.size();
        objects.push_back(record);

        ++stats.safeObjects;
        if (safety == SafetyClass::StaticSafe)
            ++stats.staticObjects;
        else
            ++stats.dynamicObjects;

        for (auto& spriteRef : objectSpriteRetains) {
            auto sprite = spriteRef.data();
            SpriteRecord spriteRecord;
            spriteRecord.sprite = sprite;
            spriteRecord.objectIndex = objectIndex;
            spriteRecord.geometry = captureSpriteState(sprite);
            spriteIndexByPointer.emplace(sprite, sprites.size());
            // Initial state is captured once, inside resync(), while every
            // accepted object and sprite is still strongly retained.
            sprites.push_back(spriteRecord);
            initSpriteRetains.emplace_back(sprite);
            ++stats.safeSprites;
        }

        if (isShadowValidationCandidate(object, safety, objectSprites)) {
            shadowCandidates.push_back({
                object,
                objectSprites[0],
                objectIndex,
                firstSpriteIndex
            });
        }
    }

    stats.retainedInitObjects = initObjectRetains.size();
    stats.retainedInitSprites = initSpriteRetains.size();

    if (objects.empty() || sprites.empty()) {
        log::info(
            "Bismuth iOS state layer: no conservative GPU-safe objects ({} stock); collection rejected {} object(s), {} non-sprite child node(s), {} duplicate sprite(s), {} invalid sprite record(s)",
            stats.stockObjects,
            stats.unsafeCollectionObjects,
            stats.invalidChildNodes,
            stats.duplicateSpriteRecords,
            stats.invalidSpriteRecords
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
        log::warn("Bismuth iOS resolved-state textures unavailable; GPU assist cannot consume state");
        destroyTextures();
        return false;
    }

    // This is the first state capture for accepted records. initObjectRetains and
    // initSpriteRetains remain alive until init() returns, so resync cannot see a
    // sprite that disappeared after classification.
    resync();

    log::info(
        "Bismuth iOS state layer: {} safe objects ({} static, {} dynamic), {} stock, {} sprite records; collection rejected {} object(s) / {} non-sprite child node(s) / {} duplicate sprite(s) / {} invalid sprite record(s); init retained {} object(s) / {} sprite(s), {} revalidation failure(s)",
        stats.safeObjects,
        stats.staticObjects,
        stats.dynamicObjects,
        stats.stockObjects,
        stats.safeSprites,
        stats.unsafeCollectionObjects,
        stats.invalidChildNodes,
        stats.duplicateSpriteRecords,
        stats.invalidSpriteRecords,
        stats.retainedInitObjects,
        stats.retainedInitSprites,
        stats.initRevalidationFailures
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

bool ResolvedStateLayer::canDrawSprite(cocos2d::CCSprite* sprite) const {
    auto it = spriteIndexByPointer.find(sprite);
    if (it == spriteIndexByPointer.end())
        return false;
    const auto& record = sprites[it->second];
    auto object = objects[record.objectIndex].object;
    if (object != sprite || object->m_glowSprite || object->m_colorSprite ||
        (object->getChildren() && object->getChildren()->count() != 0))
        return false;
    const auto current = captureSpriteState(sprite);
    const auto& baked = record.geometry;
    // The persistent VBO contains these exact corners. A later frame/flip/crop
    // change must use stock geometry until the original shape returns.
    return !spriteUVChanged(baked, current) &&
        baked.offset.x == current.offset.x && baked.offset.y == current.offset.y &&
        baked.textureWidth == current.textureWidth && baked.textureHeight == current.textureHeight;
}

void ResolvedStateLayer::setGPUOwnedSprites(
    const std::unordered_set<cocos2d::CCSprite*>& ownedSprites
) {
    activeObjectIndices.clear();
    activeSpriteIndices.clear();

    stats.activeGPUObjects = 0;
    stats.activeGPUSprites = 0;
    stats.activeStaticObjects = 0;

    if (ownedSprites.empty() || objects.empty() || sprites.empty())
        return;

    std::vector<bool> activeObjectMask(objects.size(), false);
    activeSpriteIndices.reserve(std::min(ownedSprites.size(), sprites.size()));

    for (usize spriteIndex = 0; spriteIndex < sprites.size(); ++spriteIndex) {
        const auto& record = sprites[spriteIndex];
        if (!record.sprite || !ownedSprites.contains(record.sprite))
            continue;

        activeSpriteIndices.push_back(spriteIndex);
        if (record.objectIndex < activeObjectMask.size())
            activeObjectMask[record.objectIndex] = true;
    }

    for (usize objectIndex = 0; objectIndex < activeObjectMask.size(); ++objectIndex) {
        if (!activeObjectMask[objectIndex])
            continue;

        activeObjectIndices.push_back(objectIndex);
        if (objects[objectIndex].safety == SafetyClass::StaticSafe)
            ++stats.activeStaticObjects;
    }

    stats.activeGPUObjects = activeObjectIndices.size();
    stats.activeGPUSprites = activeSpriteIndices.size();
    stats.staticObjectsReused = stats.activeStaticObjects;

    log::info(
        "Bismuth iOS active state set: {} GPU objects, {} GPU sprites, {} static GPU objects",
        stats.activeGPUObjects,
        stats.activeGPUSprites,
        stats.activeStaticObjects
    );
}

void ResolvedStateLayer::update(bool detailedProbe) {
    stats.dirtyTransforms = 0;
    stats.dirtyAppearance = 0;
    stats.dirtyVisibility = 0;
    stats.dirtyUVs = 0;
    stats.staticObjectsReused = stats.activeStaticObjects;
    stats.bytesUploaded = 0;
    stats.uploadCalls = 0;

    // The hot path only polls records actually consumed by visible GPU batches.
    // Safe-but-stock sprites are classification information, not renderer input.
    if (!detailedProbe || !objectStateTexture || !spriteStateTexture || activeSpriteIndices.empty())
        return;

    dirtyObjectRecords.clear();
    dirtySpriteRecords.clear();
    staticTouched.resize(objects.size(), false);
    for (usize i : activeObjectIndices) {
        if (i < staticTouched.size())
            staticTouched[i] = false;
    }

    for (usize i : activeObjectIndices) {
        if (i >= objects.size())
            continue;

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
            dirtyObjectRecords.push_back(i);
        }
    }

    for (usize i : activeSpriteIndices) {
        if (i >= sprites.size())
            continue;

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
            dirtySpriteRecords.push_back(i);
        }
    }

    usize touchedStaticCount = 0;
    for (usize objectIndex : activeObjectIndices) {
        if (objectIndex < staticTouched.size() && staticTouched[objectIndex])
            ++touchedStaticCount;
    }
    stats.staticObjectsReused = stats.activeStaticObjects > touchedStaticCount
        ? stats.activeStaticObjects - touchedStaticCount
        : 0;

    uploadDirtyRecordSpans(
        objectStateTexture,
        objectTexels,
        dirtyObjectRecords,
        OBJECT_TEXELS_PER_STATE,
        stats
    );
    uploadDirtyRecordSpans(
        spriteStateTexture,
        spriteTexels,
        dirtySpriteRecords,
        SPRITE_TEXELS_PER_STATE,
        stats
    );
}

#endif