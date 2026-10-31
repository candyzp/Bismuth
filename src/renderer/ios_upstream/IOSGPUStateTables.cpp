#ifdef GEODE_IS_IOS

#include "IOSGPUStateTables.hpp"
#include "IOSSceneData.hpp"
#include "IOSObjectBatch.hpp"
#include "../ios/DataTexture.hpp"

#include <algorithm>
#include <limits>

using namespace geode::prelude;

namespace IOSUpstream {

GPUStateTables::~GPUStateTables() = default;

namespace {

RGBA8 unpackU32(u32 value) {
    return {
        static_cast<u8>(value & 0xffu),
        static_cast<u8>((value >> 8) & 0xffu),
        static_cast<u8>((value >> 16) & 0xffu),
        static_cast<u8>((value >> 24) & 0xffu)
    };
}

u32 checkedIndex(usize value, const char* field) {
    if (value > static_cast<usize>(std::numeric_limits<u32>::max())) {
        log::error("Bismuth iOS GPU table '{}' exceeds the 32-bit index range", field);
        return std::numeric_limits<u32>::max();
    }
    return static_cast<u32>(value);
}

} // namespace

bool GPUStateTables::ensureFloatTexture(
    std::unique_ptr<DataTexture>& texture,
    const char* name,
    usize texelCount
) {
    texelCount = std::max<usize>(texelCount, 1);
    if (texture && texture->getCapacity() >= texelCount)
        return true;

    auto replacement = DataTexture::create(
        name,
        texelCount,
        DataTexture::Type::FloatRGBA
    );
    if (!replacement) {
        log::error("Bismuth iOS GPU table: failed to allocate float texture '{}'", name);
        return false;
    }

    texture.reset(replacement);
    return true;
}

bool GPUStateTables::ensureByteTexture(usize texelCount) {
    texelCount = std::max<usize>(texelCount, 1);
    if (byteTexture && byteTexture->getCapacity() >= texelCount)
        return true;

    auto replacement = DataTexture::create(
        "Bismuth packed color/HSV/pulse/sprite table",
        texelCount,
        DataTexture::Type::ByteRGBA
    );
    if (!replacement) {
        log::error("Bismuth iOS GPU table: failed to allocate packed byte texture ({} texels)", texelCount);
        return false;
    }

    byteTexture.reset(replacement);
    return true;
}

void GPUStateTables::buildObjectTexels(const SceneData& scene) {
    const auto& objects = scene.getObjectBatch().getObjectInfos();
    objectTexels.clear();
    objectTexels.reserve(objects.size() * 3);

    for (const auto& object : objects) {
        objectTexels.emplace_back(
            object.rotationSpeed,
            static_cast<float>(object.flags),
            object.audioScaleMin,
            object.audioScaleMax
        );
        objectTexels.emplace_back(
            object.fadeMargin,
            object.opacity,
            static_cast<float>(object.baseColorChannelId),
            static_cast<float>(object.detailColorChannelId)
        );
        // HSV values are stored as exact bytes in the packed byte table. This
        // spare texel remains reserved so the object stride stays three texels.
        objectTexels.emplace_back(0.f, 0.f, 0.f, 0.f);
    }

    layout.objectCount = checkedIndex(objects.size(), "object count");
}

void GPUStateTables::buildGroupTexels(const SceneData& scene) {
    const auto states = scene.getGroupManager().getGroupStates();
    groupTexels.clear();
    groupTexels.reserve(states.size() * 3);

    for (const auto& state : states) {
        // GLM matrices are column-major; preserve the owner's GLSL mat2 layout.
        groupTexels.emplace_back(
            state.positionalTransform[0].x,
            state.positionalTransform[0].y,
            state.positionalTransform[1].x,
            state.positionalTransform[1].y
        );
        groupTexels.emplace_back(
            state.localTransform[0].x,
            state.localTransform[0].y,
            state.localTransform[1].x,
            state.localTransform[1].y
        );
        groupTexels.emplace_back(
            state.offset.x,
            state.offset.y,
            state.opacity,
            static_cast<float>(state.pulseIndex)
        );
    }

    layout.groupCombinationCount = checkedIndex(states.size(), "group combination count");
}

void GPUStateTables::buildByteTexels(const SceneData& scene) {
    const auto& colors = scene.getColorChannels();
    const auto& objects = scene.getObjectBatch().getObjectInfos();
    const auto pulses = scene.getGroupManager().getPulseEntries();
    const auto& appearances = scene.getObjectBatch().getSpriteAppearances();

    byteTexels.clear();

    layout.colorChannelsOffset = 0;
    byteTexels.insert(byteTexels.end(), colors.begin(), colors.end());

    layout.objectHSVOffset = checkedIndex(byteTexels.size(), "object HSV offset");
    for (const auto& object : objects) {
        byteTexels.push_back(unpackU32(object.baseHSV));
        byteTexels.push_back(unpackU32(object.detailHSV));
    }

    layout.pulseEntriesOffset = checkedIndex(byteTexels.size(), "pulse entry offset");
    for (const auto& pulse : pulses) {
        byteTexels.push_back(unpackU32(pulse.flags));
        byteTexels.push_back(unpackU32(pulse.hsv));
    }

    layout.spriteAppearanceOffset = checkedIndex(byteTexels.size(), "sprite appearance offset");
    for (const auto& appearance : appearances) {
        byteTexels.push_back(unpackU32(appearance.packedDisplayedColor));
        byteTexels.push_back({
            static_cast<u8>(appearance.flags & 0xffu),
            0,
            0,
            255
        });
    }

    layout.pulseEntryCount = checkedIndex(pulses.size(), "pulse entry count");
    layout.spriteCount = checkedIndex(appearances.size(), "sprite appearance count");
}

bool GPUStateTables::initialize(const SceneData& scene) {
    ready = false;
    if (!scene.isReady())
        return false;

    buildObjectTexels(scene);
    buildGroupTexels(scene);
    buildByteTexels(scene);

    if (!upload(scene))
        return false;

    ready = true;
    return true;
}

bool GPUStateTables::upload(const SceneData& scene) {
    ready = false;
    if (!scene.isReady())
        return false;

    buildObjectTexels(scene);
    buildGroupTexels(scene);
    buildByteTexels(scene);

    // Keep all shader-indexed values exactly representable by ES2 highp float.
    constexpr usize MAX_EXACT_FLOAT_INTEGER = 1u << 24;
    if (objectTexels.size() > MAX_EXACT_FLOAT_INTEGER ||
        groupTexels.size() > MAX_EXACT_FLOAT_INTEGER ||
        byteTexels.size() > MAX_EXACT_FLOAT_INTEGER) {
        log::error("Bismuth iOS GPU state tables exceed the ES2 exact-index range");
        return false;
    }

    if (!ensureFloatTexture(objectTexture, "Bismuth object info table", objectTexels.size()) ||
        !ensureFloatTexture(groupTexture, "Bismuth group state table", groupTexels.size()) ||
        !ensureByteTexture(byteTexels.size())) {
        return false;
    }

    const bool objectOK = objectTexture->upload(
        objectTexels.empty() ? nullptr : objectTexels.data(),
        objectTexels.size()
    );
    const bool groupOK = groupTexture->upload(
        groupTexels.empty() ? nullptr : groupTexels.data(),
        groupTexels.size()
    );
    const bool byteOK = byteTexture->upload(
        byteTexels.empty() ? nullptr : byteTexels.data(),
        byteTexels.size()
    );

    if (!(objectOK && groupOK && byteOK)) {
        log::error(
            "Bismuth iOS GPU state upload failed (object {}, group {}, packed byte {})",
            objectOK,
            groupOK,
            byteOK
        );
        ready = false;
        return false;
    }

    layout.objectTextureSize = objectTexture->getSize();
    layout.groupTextureSize = groupTexture->getSize();
    layout.byteTextureSize = byteTexture->getSize();
    ready = true;
    return true;
}

void GPUStateTables::bind(i32 objectUnit, i32 groupUnit, i32 byteUnit) const {
    if (!ready || !objectTexture || !groupTexture || !byteTexture)
        return;

    objectTexture->bind(objectUnit);
    groupTexture->bind(groupUnit);
    byteTexture->bind(byteUnit);
}

u32 GPUStateTables::getObjectTexture() const {
    return objectTexture ? objectTexture->getId() : 0;
}

u32 GPUStateTables::getGroupTexture() const {
    return groupTexture ? groupTexture->getId() : 0;
}

u32 GPUStateTables::getByteTexture() const {
    return byteTexture ? byteTexture->getId() : 0;
}

} // namespace IOSUpstream

#endif
