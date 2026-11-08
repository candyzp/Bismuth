#ifdef GEODE_IS_IOS

#include "ResolvedStateLayer.hpp"
#include "../GroupManager.hpp"
#include "DirtyRanges.hpp"
#include "../../ObjectUtils.hpp"

#include <Geode/binding/CheckpointGameObject.hpp>
#include <Geode/utils/cocos.hpp>
#include <algorithm>
#include <cmath>
#include <vector>
#include <unordered_set>

using namespace geode::prelude;

namespace {
constexpr usize OBJECT_TEXELS_PER_STATE = 3;
constexpr usize SPRITE_TEXELS_PER_STATE = 2;

bool isSimpleSpikeRoot(GameObject* object) {
    // Only the root quad is GPU-owned. Separate glow/detail nodes keep their
    // stock draws and lifecycle; nested or animated visuals remain stock.
    return object && object->m_objectType == GameObjectType::Hazard &&
        object->m_classType != GameObjectClassType::Animated &&
        !object->getHasSyncedAnimation() && !object->getDontDraw() &&
        object->getTexture() &&
        (!object->getChildren() || object->getChildren()->count() == 0) &&
        object->m_glowSprite != object && object->m_colorSprite != object;
}

bool isDescendantOf(cocos2d::CCNode* node, cocos2d::CCNode* ancestor) {
    if (!node || !ancestor)
        return false;
    for (auto current = node; current; current = current->getParent()) {
        if (current == ancestor)
            return true;
    }
    return false;
}

// The legacy shader expects each GameObject transform in its stock atlas' local
// space. A GameObject can sit below an ordinary CCNode container, so its
// nodeToParentTransform() alone is not always the complete transform.
cocos2d::CCAffineTransform getObjectTransformToBatch(GameObject* object) {
    if (!object)
        return cocos2d::CCAffineTransformMakeIdentity();

    const auto local = object->nodeToParentTransform();
    auto batch = object->getBatchNode();
    if (!batch)
        return local;

    auto transform = cocos2d::CCAffineTransformMakeIdentity();
    auto current = static_cast<cocos2d::CCNode*>(object);
    while (current && current != batch) {
        transform = cocos2d::CCAffineTransformConcat(
            transform, current->nodeToParentTransform()
        );
        current = current->getParent();
    }

    // Detached/deferred roots do not have a batch yet. Keep their local matrix
    // until Geometry Dash inserts them into their final stock atlas.
    return current == batch ? transform : local;
}

inline bool changedFloat(float a, float b, float epsilon = 0.0001f) {
    return std::abs(a - b) > epsilon;
}

inline bool rectChanged(const cocos2d::CCRect& a, const cocos2d::CCRect& b) {
    return changedFloat(a.origin.x, b.origin.x) ||
           changedFloat(a.origin.y, b.origin.y) ||
           changedFloat(a.size.width, b.size.width) ||
           changedFloat(a.size.height, b.size.height);
}

bool uploadDirtyRecordSpans(
    DataTexture* texture, const std::vector<glm::vec4>& texels,
    std::vector<usize>& dirtyRecords, usize texelsPerRecord,
    ResolvedStateLayer::Stats& stats
) {
    if (dirtyRecords.empty())
        return true;
    if (!texture || !texelsPerRecord)
        return false;
    std::vector<DataTexture::Range> ranges;
    buildDirtyRanges(dirtyRecords, texelsPerRecord,
        static_cast<usize>(texture->getSize().x), ranges);
    if (!texture->uploadRanges(texels.data(), texels.size(), ranges))
        return false; // Retain the records: a failed upload must retry next frame.
    for (const auto& range : ranges) {
        stats.bytesUploaded += range.texelCount * sizeof(glm::vec4);
        ++stats.uploadCalls;
    }
    dirtyRecords.clear();
    return true;
}

} // namespace

void ResolvedStateLayer::destroyTextures() {
    if (objectStateTexture)
        DataTexture::destroy(objectStateTexture);
    if (spriteStateTexture)
        DataTexture::destroy(spriteStateTexture);
    if (groupStateTexture)
        DataTexture::destroy(groupStateTexture);
    objectStateTexture = nullptr;
    spriteStateTexture = nullptr;
    groupStateTexture = nullptr;
    groupStateTexels.clear();
    groupStateUploadCurrent = false;
    uploadsCurrent = false;
}

ResolvedStateLayer::SafetyClass ResolvedStateLayer::classifyObject(
    GameObject* object,
    std::vector<cocos2d::CCSprite*>& outSprites,
    CollectionDiagnostics& diagnostics
) const {
    if (!object || object->isTrigger() || object->m_isHide || object->m_isInvisible)
        return SafetyClass::StockOnly;

    // Decoration is a hard GPU-owned category. Complexity, animation, glow,
    // detail sprites, nested sprite trees and non-sprite helper children do not
    // demote it back to stock.
    if (object->m_objectType == GameObjectType::Decoration) {
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

        // Keep decorations GPU-owned, but do not label every decoration dynamic.
        // The legacy helper polls DynamicSafe roots for a fresh Cocos transform on
        // every rendered frame. Most decoration roots are fixed; their child
        // animations and atlas UV changes are already handled by LiveGeometry.
        // Only roots with a real per-frame transform source need that CPU work.
        const auto batch = object->getBatchNode();
        const bool inheritedNodeTransform =
            batch && object->getParent() && object->getParent() != batch;
        const bool groupDriven = object->m_groupCount > 0;
        const bool dynamic =
            object->m_classType == GameObjectClassType::Animated ||
            object->getHasSyncedAnimation() ||
            object->m_usesAudioScale ||
            inheritedNodeTransform ||
            (object->getHasRotateAction() && !groupDriven);

        if (outSprites.empty())
            return SafetyClass::StockOnly;
        return dynamic ? SafetyClass::DynamicSafe : SafetyClass::StaticSafe;
    }

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

    if (isSimpleSpikeRoot(object)) {
        outSprites.push_back(object);

        // Spikes are gameplay-critical visuals. Keep simple spike roots on the
        // GPU path, but always sample their exact live root transform every frame
        // instead of using the StaticSafe transform cache.
        return SafetyClass::DynamicSafe;
    }

    // Explicit GPU visual ownership:
    // - ordinary blocks / objects (Solid)
    // - decorations (Decoration)
    // - simple spike roots, handled above
    //
    // Portal/pad/ring/etc. types are filtered by isInteractiveVisualObject()
    // before this point, and ground is not owned by this object renderer.
    if (object->m_objectType != GameObjectType::Solid)
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

    // The recursive collector already rejects duplicate records and non-sprite
    // child nodes. Keep multi-sprite solids only when every accepted visual is
    // inside the root subtree; separately-homed glow/detail sprites stay stock.
    const bool hasExternalVisual = std::any_of(
        outSprites.begin(), outSprites.end(),
        [object](cocos2d::CCSprite* sprite) {
            return !sprite || !isDescendantOf(sprite, object);
        }
    );
    const bool externalGlow = object->m_glowSprite &&
        !isDescendantOf(object->m_glowSprite, object);
    const bool externalColor = object->m_colorSprite &&
        !isDescendantOf(object->m_colorSprite, object);
    if (outSprites.empty() || hasExternalVisual || externalGlow || externalColor) {
        outSprites.clear();
        return SafetyClass::StockOnly;
    }

    const auto batch = object->getBatchNode();
    const bool inheritedNodeTransform =
        batch && object->getParent() && object->getParent() != batch;
    const bool groupDriven = object->m_groupCount > 0;
    const bool dynamic =
        object->m_usesAudioScale ||
        inheritedNodeTransform ||
        (object->getHasRotateAction() && !groupDriven);

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

    state.transform = getObjectTransformToBatch(object);
    state.vertexZ = object->getVertexZ();
    state.opacity = (float)object->getDisplayedOpacity() / 255.f;
    state.visible = object->getParent() && object->isVisible() &&
        !object->getDontDraw() && !object->m_isHide && !object->m_isInvisible;
    return state;
}

ResolvedStateLayer::ObjectState ResolvedStateLayer::captureFrameObjectState(
    GameObject* object,
    SafetyClass safety,
    const ObjectState& previous
) const {
    ObjectState state = previous;
    if (!object) {
        state.visible = false;
        return state;
    }

    if (safety != SafetyClass::StaticSafe) {
        // The GPU state texture does not consume root opacity. Avoid asking Cocos
        // for displayed opacity on every dynamic object and sample only fields
        // that the vertex shader actually reads.
        state.transform = getObjectTransformToBatch(object);
        state.vertexZ = object->getVertexZ();
    }

    // StaticSafe means this root has no group-driven transform, rotate action or
    // audio scale. Its exact affine matrix/vertex Z remain resident; both classes
    // still mirror stock lifecycle visibility every rendered frame.
    state.visible = object->getParent() && object->isVisible() &&
        !object->getDontDraw() && !object->m_isHide && !object->m_isInvisible;
    return state;
}

ResolvedStateLayer::SpriteState ResolvedStateLayer::captureSpriteState(cocos2d::CCSprite* sprite) const {
    SpriteState state;
    if (!sprite)
        return state;

    state.color = sprite->getDisplayedColor();
    state.opacity = sprite->getDisplayedOpacity();
    state.textureRect = sprite->getTextureRect();
    state.offset = sprite->getOffsetPosition();
    state.opacityModifyRGB = sprite->isOpacityModifyRGB();
    state.visible = sprite->isVisible() && !sprite->getDontDraw();
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

ResolvedStateLayer::SpriteState ResolvedStateLayer::captureFrameSpriteState(
    cocos2d::CCSprite* sprite,
    const SpriteState& previous
) const {
    SpriteState state = previous;
    if (!sprite) {
        state.visible = false;
        state.opacity = 0;
        return state;
    }

    // Per-frame rendering consumes only final displayed color/alpha and child
    // visibility. UV/frame/offset/texture geometry is baked in the persistent
    // GPU geometry and is validated separately by canDrawSprite() for the
    // conservative solid/spike path. Forced decorations already use persistent
    // geometry, so polling those unused geometry fields thousands of times per
    // frame was pure CPU overhead.
    state.color = sprite->getDisplayedColor();
    state.opacity = sprite->getDisplayedOpacity();
    state.opacityModifyRGB = sprite->isOpacityModifyRGB();
    state.visible = sprite->isVisible() && !sprite->getDontDraw();
    return state;
}

void ResolvedStateLayer::packObjectState(usize index, const ObjectState& state, SafetyClass) {
    const usize base = index * OBJECT_TEXELS_PER_STATE;
    if (index >= objects.size() || base + 2 >= objectTexels.size())
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

    const auto& record = objects[index];
    objectTexels[base + 2] = {
        record.startPosition.x,
        record.startPosition.y,
        static_cast<float>(record.groupCombinationIndex),
        record.groupFlags
    };
}

void ResolvedStateLayer::packSpriteState(usize index, const SpriteState& state, usize objectIndex) {
    const usize base = index * SPRITE_TEXELS_PER_STATE;
    if (base + 1 >= spriteTexels.size())
        return;

    const auto colorByte = [&](u8 value) -> float {
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

    u32 flags = 0;
    if (state.visible) flags |= 1u;
    if (state.rotated) flags |= 2u;
    if (state.flipX) flags |= 4u;
    if (state.flipY) flags |= 8u;

    // The assist shader only consumes flags + object index here. Texture rect
    // and texture dimensions are geometry-validation data, not render-state
    // data, so keeping a third RGBA texel per sprite wasted 33% of this texture.
    spriteTexels[base + 1] = {
        (float)flags,
        (float)objectIndex,
        0.f,
        0.f
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

bool ResolvedStateLayer::init(PlayLayer* playLayer, GroupManager* groups) {
    destroyTextures();
    layer = playLayer;
    groupManager = groups;
    objects.clear();
    sprites.clear();
    spriteIndexByPointer.clear();
    spriteValidationEpoch.clear();
    spriteValidationResult.clear();
    validationEpoch = 0;
    eventOwnershipReady = false;
    shadowCandidates.clear();
    objectTexels.clear();
    spriteTexels.clear();
    activeObjectIndices.clear();
    activeSpriteIndices.clear();
    stats = {};

    if (!layer || !layer->m_objects || !groupManager || !groupManager->isReady())
        return false;

    std::vector<Ref<GameObject>> initObjectRetains;
    std::vector<Ref<cocos2d::CCSprite>> initSpriteRetains;
    initObjectRetains.reserve(static_cast<usize>(layer->m_objects->count()));

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
        record.startPosition = ccPointToGLM(object->m_startPosition);
        record.groupCombinationIndex =
            groupManager->getGroupCombinationIndexForObject(object);
        if (safety == SafetyClass::StaticSafe && object->m_groupCount > 0) {
            // Bit 0 enables shared positional state; bit 1 applies the group-local
            // transform for non-Solid objects, matching the desktop shader.
            record.groupFlags = 1.f;
            if (object->m_objectType != GameObjectType::Solid)
                record.groupFlags += 2.f;
        }
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
    groupStateTexels.resize(
        static_cast<usize>(groupManager->getGroupCombinationCount()) * 3
    );
    spriteValidationEpoch.assign(sprites.size(), 0);
    spriteValidationResult.assign(sprites.size(), 0);

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
    groupStateTexture = DataTexture::create(
        "Resolved group-combination state",
        groupStateTexels.size(),
        DataTexture::Type::FloatRGBA
    );

    if (!objectStateTexture || !spriteStateTexture || !groupStateTexture) {
        log::warn("Bismuth iOS resolved-state textures unavailable; GPU assist cannot consume state");
        destroyTextures();
        return false;
    }

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

    const bool objectsUploaded = objectStateTexture->upload(objectTexels.data(), objectTexels.size());
    const bool spritesUploaded = spriteStateTexture->upload(spriteTexels.data(), spriteTexels.size());
    groupStateUploadCurrent = syncGroupStateTexture();
    uploadsCurrent = objectsUploaded && spritesUploaded && groupStateUploadCurrent;
    fullUploadPending = !objectsUploaded || !spritesUploaded;
    dirtyObjectRecords.clear();
    dirtySpriteRecords.clear();
}

bool ResolvedStateLayer::syncGroupStateTexture() {
    if (!groupManager || !groupStateTexture)
        return groupStateUploadCurrent = false;

    const auto states = groupManager->getGroupStates();
    if (states.size() * 3 != groupStateTexels.size())
        return groupStateUploadCurrent = false;

    const auto& dirtyIndices = groupManager->getDirtyGroupCombinationIndices();
    if (dirtyIndices.empty())
        return groupStateUploadCurrent = true;

    std::vector<u32> sortedIndices(dirtyIndices.begin(), dirtyIndices.end());
    std::sort(sortedIndices.begin(), sortedIndices.end());
    sortedIndices.erase(
        std::unique(sortedIndices.begin(), sortedIndices.end()),
        sortedIndices.end()
    );

    for (u32 groupIndex : sortedIndices) {
        if (groupIndex >= states.size())
            return groupStateUploadCurrent = false;

        const auto& state = states[groupIndex];
        const usize base = static_cast<usize>(groupIndex) * 3;
        groupStateTexels[base + 0] = {
            state.positionalTransform[0][0],
            state.positionalTransform[0][1],
            state.positionalTransform[1][0],
            state.positionalTransform[1][1]
        };
        groupStateTexels[base + 1] = {
            state.localTransform[0][0],
            state.localTransform[0][1],
            state.localTransform[1][0],
            state.localTransform[1][1]
        };
        groupStateTexels[base + 2] = {
            state.offset.x,
            state.offset.y,
            state.opacity,
            0.f
        };
    }

    std::vector<DataTexture::Range> ranges;
    ranges.reserve(std::min<usize>(sortedIndices.size(), 48));
    constexpr usize MAX_GROUP_STATE_RANGES = 48;
    constexpr usize COALESCE_GAP_GROUPS = 2;

    usize rangeStart = static_cast<usize>(sortedIndices.front()) * 3;
    usize rangeEnd = rangeStart + 3;
    for (usize i = 1; i < sortedIndices.size(); ++i) {
        const usize nextStart = static_cast<usize>(sortedIndices[i]) * 3;
        if (nextStart <= rangeEnd + COALESCE_GAP_GROUPS * 3) {
            // The mirror already contains correct values for unchanged groups,
            // so small gaps can be uploaded together to avoid many tiny GL calls.
            rangeEnd = nextStart + 3;
            continue;
        }
        ranges.push_back({rangeStart, rangeEnd - rangeStart});
        rangeStart = nextStart;
        rangeEnd = nextStart + 3;
    }
    ranges.push_back({rangeStart, rangeEnd - rangeStart});

    if (ranges.size() > MAX_GROUP_STATE_RANGES) {
        ranges.clear();
        ranges.push_back({0, groupStateTexels.size()});
    }

    if (!groupStateTexture->uploadRanges(
        groupStateTexels.data(), groupStateTexels.size(), ranges
    ))
        return groupStateUploadCurrent = false;

    groupManager->clearDirtyGroupStateIndices();
    return groupStateUploadCurrent = true;
}

bool ResolvedStateLayer::canDrawSprite(cocos2d::CCSprite* sprite) {
    auto it = spriteIndexByPointer.find(sprite);
    if (it == spriteIndexByPointer.end())
        return false;

    const usize spriteIndex = it->second;
    if (spriteIndex >= sprites.size())
        return false;

    if (spriteValidationEpoch.size() != sprites.size()) {
        spriteValidationEpoch.assign(sprites.size(), 0);
        spriteValidationResult.assign(sprites.size(), 0);
    }

    if (validationEpoch != 0 && spriteValidationEpoch[spriteIndex] == validationEpoch) {
        ++stats.spriteValidationReuses;
        return spriteValidationResult[spriteIndex] != 0;
    }

    ++stats.spriteValidations;
    bool result = false;

    const auto& record = sprites[spriteIndex];
    if (record.objectIndex < objects.size()) {
        const auto& objectRecord = objects[record.objectIndex];
        auto object = objectRecord.object;

        // Ownership was proven when the level state was built. Once a record is
        // GPU-owned, runtime child/glow/detail attachment changes must not demote
        // its root and make an entire atlas batch disappear. LiveGeometry tracks
        // crop/UV/local-transform changes, while newly attached stock children
        // still keep their own normal Cocos draw lifecycle.
        if (object && sprite && sprite->getTexture() &&
            objectRecord.safety != SafetyClass::StockOnly) {
            if (object->m_objectType == GameObjectType::Decoration) {
                result = true;
            } else if (object->m_objectType == GameObjectType::Solid) {
                result = isDescendantOf(sprite, object);
            } else if (object == sprite && object->m_objectType == GameObjectType::Hazard) {
                result = true;
            }
        }
    }

    if (validationEpoch != 0) {
        spriteValidationEpoch[spriteIndex] = validationEpoch;
        spriteValidationResult[spriteIndex] = result ? 1 : 0;
    }
    return result;
}

bool ResolvedStateLayer::isForcedDecorationSprite(cocos2d::CCSprite* sprite) const {
    auto it = spriteIndexByPointer.find(sprite);
    if (it == spriteIndexByPointer.end() || it->second >= sprites.size())
        return false;

    const auto& spriteRecord = sprites[it->second];
    if (spriteRecord.objectIndex >= objects.size())
        return false;

    auto object = objects[spriteRecord.objectIndex].object;
    return object && object->m_objectType == GameObjectType::Decoration;
}

bool ResolvedStateLayer::isGPUGroupManagedObject(GameObject* object) {
    if (!object)
        return false;

    ensureEventOwnership();
    if (!eventOwnershipReady)
        return false;

    const auto found = objectIndexByPointer.find(object);
    if (found == objectIndexByPointer.end() || found->second >= objects.size())
        return false;

    const auto& record = objects[found->second];
    if (record.groupFlags < 0.5f || record.spriteCount == 0 ||
        record.firstSprite > sprites.size() ||
        record.spriteCount > sprites.size() - record.firstSprite ||
        record.firstSprite + record.spriteCount > gpuOwnedSpriteMask.size())
        return false;

    for (usize spriteIndex = record.firstSprite;
         spriteIndex < record.firstSprite + record.spriteCount;
         ++spriteIndex) {
        if (!gpuOwnedSpriteMask[spriteIndex])
            return false;
    }
    return true;
}

void ResolvedStateLayer::beginFrameValidation() {
    stats.spriteValidations = 0;
    stats.spriteValidationReuses = 0;

    // Unsigned wrap is defined. If it ever happens after billions of rendered
    // frames, clear the tiny epoch table so no ancient result can look current.
    ++validationEpoch;
    if (validationEpoch == 0) {
        std::fill(spriteValidationEpoch.begin(), spriteValidationEpoch.end(), 0);
        validationEpoch = 1;
    }
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

    // GPU group transforms are an all-visuals ownership contract. If any
    // sprite belonging to a grouped root remains on stock Cocos, keep the entire
    // root on the CPU transform path so its GPU-owned subset is not moved twice.
    bool adjustedGroupOwnership = false;
    for (usize objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        auto& objectRecord = objects[objectIndex];
        if (objectRecord.groupFlags < 0.5f)
            continue;

        bool fullyOwned = objectRecord.spriteCount > 0 &&
            objectRecord.firstSprite <= sprites.size() &&
            objectRecord.spriteCount <= sprites.size() - objectRecord.firstSprite;
        for (usize localSprite = 0; fullyOwned && localSprite < objectRecord.spriteCount; ++localSprite) {
            const usize spriteIndex = objectRecord.firstSprite + localSprite;
            fullyOwned = sprites[spriteIndex].sprite &&
                ownedSprites.contains(sprites[spriteIndex].sprite);
        }

        if (fullyOwned)
            continue;

        objectRecord.groupFlags = 0.f;
        if (objectRecord.safety == SafetyClass::StaticSafe) {
            objectRecord.safety = SafetyClass::DynamicSafe;
            if (stats.staticObjects)
                --stats.staticObjects;
            ++stats.dynamicObjects;
        }
        packObjectState(objectIndex, objectRecord.state, objectRecord.safety);
        adjustedGroupOwnership = true;
    }

    if (adjustedGroupOwnership && objectStateTexture) {
        const bool uploaded = objectStateTexture->upload(objectTexels.data(), objectTexels.size());
        if (!uploaded) {
            uploadsCurrent = false;
            fullUploadPending = true;
        }
    }

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
    stats.staticTransformBuildsAvoided = 0;
    stats.bytesUploaded = 0;
    stats.uploadCalls = 0;

    if (!detailedProbe || !objectStateTexture || !spriteStateTexture)
        return;
    if (activeSpriteIndices.empty() && dirtyObjectRecords.empty() &&
        dirtySpriteRecords.empty() && !fullUploadPending)
        return;

    // Failed spans remain queued even if the CPU state stops changing.
    staticTouched.resize(objects.size(), false);
    for (usize i : activeObjectIndices) {
        if (i < staticTouched.size())
            staticTouched[i] = false;
    }

    for (usize i : activeObjectIndices) {
        if (i >= objects.size())
            continue;

        auto& record = objects[i];
        const bool staticTransform = record.safety == SafetyClass::StaticSafe;
        const ObjectState next = captureFrameObjectState(
            record.object,
            record.safety,
            record.state
        );

        // StaticSafe's matrix and vertex Z stay resident. Only DynamicSafe pays
        // Cocos' affine rebuild cost on the render hot path.
        const bool transformDirty = !staticTransform && transformChanged(record.state, next);
        const bool visibilityDirty = record.state.visible != next.visible;

        if (staticTransform)
            ++stats.staticTransformBuildsAvoided;
        if (transformDirty)
            ++stats.dirtyTransforms;
        if (visibilityDirty)
            ++stats.dirtyVisibility;

        if (transformDirty || visibilityDirty) {
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
        SpriteState next = captureFrameSpriteState(record.sprite, record.state);
        if (record.objectIndex < objects.size() && record.sprite != objects[record.objectIndex].object) {
            // Root visibility is in object state; nested sprite ancestors must
            // also hide their descendants without leaving stale GPU decoration.
            for (auto parent = record.sprite->getParent(); parent && parent != objects[record.objectIndex].object;
                parent = parent->getParent()) {
                if (!parent->isVisible()) { next.visible = false; break; }
            }
        }

        const bool appearanceDirty = spriteAppearanceChanged(record.state, next);
        const bool visibilityDirty = record.state.visible != next.visible;

        if (appearanceDirty)
            ++stats.dirtyAppearance;
        if (visibilityDirty)
            ++stats.dirtyVisibility;

        if (appearanceDirty || visibilityDirty) {
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

    bool objectsUploaded = uploadDirtyRecordSpans(
        objectStateTexture,
        objectTexels,
        dirtyObjectRecords,
        OBJECT_TEXELS_PER_STATE,
        stats
    );
    bool spritesUploaded = uploadDirtyRecordSpans(
        spriteStateTexture,
        spriteTexels,
        dirtySpriteRecords,
        SPRITE_TEXELS_PER_STATE,
        stats
    );
    if (fullUploadPending) {
        objectsUploaded = objectStateTexture->upload(objectTexels.data(), objectTexels.size());
        spritesUploaded = spriteStateTexture->upload(spriteTexels.data(), spriteTexels.size());
        fullUploadPending = !(objectsUploaded && spritesUploaded);
    }
    uploadsCurrent = objectsUploaded && spritesUploaded && groupStateUploadCurrent;
}

#endif
