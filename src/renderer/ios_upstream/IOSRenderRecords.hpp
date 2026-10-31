#pragma once

#ifdef GEODE_IS_IOS

#include <common.hpp>
#include <ObjectUtils.hpp>
#include <array>
#include <map>
#include <span>
#include <vector>

// These records retain the owner's meaning and field order. They are not
// std430 structs: the iOS backend serializes them into nearest-filtered ES
// texture tables and ordinary vertex attributes.
namespace IOSUpstream {

constexpr u32 IOS_COLOR_CHANNEL_COUNT = 1101;
constexpr u32 IOS_GROUP_IDS_PER_OBJECT = 10;
constexpr u32 IOS_COLOR_CHANNEL_INDEX_MASK = 0x0fffu;
constexpr u32 IOS_COLOR_CHANNEL_DIRECT_FLAG = 0x2000u;
constexpr u32 IOS_COLOR_CHANNEL_BG = 1000u;
constexpr u32 IOS_COLOR_CHANNEL_P1 = 1005u;
constexpr u32 IOS_COLOR_CHANNEL_P2 = 1006u;
constexpr u32 IOS_COLOR_CHANNEL_LBG = 1007u;
constexpr u32 IOS_COLOR_CHANNEL_BLACK = 1010u;
constexpr u32 IOS_COLOR_CHANNEL_WHITE = 1011u;

constexpr i32 IOS_MOVE_TRIGGER_ID = 901;
constexpr i32 IOS_ALPHA_TRIGGER_ID = 1007;
constexpr i32 IOS_TOGGLE_TRIGGER_ID = 1049;
constexpr i32 IOS_ROTATE_TRIGGER_ID = 1346;
constexpr i32 IOS_FOLLOW_TRIGGER_ID = 1347;
constexpr i32 IOS_GUIDE_OBJECT_ID = 2016;

constexpr u32 IOS_OBJECT_FLAG_USES_AUDIO_SCALE   = 1u << 0;
constexpr u32 IOS_OBJECT_FLAG_CUSTOM_AUDIO_SCALE = 1u << 1;
constexpr u32 IOS_OBJECT_FLAG_IS_ORB             = 1u << 2;
constexpr u32 IOS_OBJECT_FLAG_IS_INVISIBLE_BLOCK = 1u << 3;
constexpr u32 IOS_OBJECT_FLAG_SPECIAL_GLOW_COLOR = 1u << 4;
constexpr u32 IOS_OBJECT_FLAG_IS_GLOW_COLOR_LBG  = 1u << 5;
constexpr u32 IOS_OBJECT_FLAG_HAS_BASE_HSV       = 1u << 6;
constexpr u32 IOS_OBJECT_FLAG_HAS_DETAIL_HSV     = 1u << 7;
constexpr u32 IOS_OBJECT_FLAG_IS_STATIC_OBJECT   = 1u << 8;

constexpr u32 IOS_GAME_STATE_IS_PLAYER_DEAD = 1u << 0;

constexpr u32 IOS_PULSE_FLAG_IS_BASE_ONLY   = 1u << 26;
constexpr u32 IOS_PULSE_FLAG_IS_DETAIL_ONLY = 1u << 27;
constexpr u32 IOS_PULSE_FLAG_IS_HSV         = 1u << 28;
constexpr u32 IOS_PULSE_FLAG_IS_DYNAMIC_HSV = 1u << 29;
constexpr u32 IOS_PULSE_FLAG_IS_ACTIVE      = 1u << 30;

struct ObjectVertex {
    glm::vec2 positionOffset {};
    glm::vec2 texCoord {};
    // ES2 has no integer vertex attributes. Keep this index exactly
    // representable as a float and validate it before uploading.
    float spriteIndex = 0.f;
};

struct InputSprite {
    // Owner fields: preserve their meanings and ordering.
    u32 objectIndex = 0;
    u32 spriteType = 0;
    u32 indexStart = 0;
    u32 indexCount = 0;

    // iOS fork extension: ObjectUtils encodes direct-tint metadata in the high
    // bits of the color channel. Keep it intact for the vertex/shader stage.
    u32 colorChannelFlags = 0;
};

struct SpriteAppearance {
    u32 packedDisplayedColor = 0xffffffffu;
    u32 flags = 0;
};

struct PreCulledSprite {
    u32 groupCombinationIndex = 0;
    glm::vec2 startPosition {};
};

struct WorkGroup {
    u32 inputSpriteStart = 0;
    u32 inputSpriteEnd = 0;
    u32 culledIndexBufferOutputIndex = 0;
};

struct DrawCommand {
    u32 count = 0;
    u32 instanceCount = 1;
    u32 first = 0;
    u32 baseInstance = 0;
};

struct GroupCombinationState {
    glm::mat2 positionalTransform {1.f};
    glm::mat2 localTransform {1.f};
    glm::vec2 offset {};
    float opacity = 1.f;
    u32 pulseIndex = 0;
};

struct ObjectInfo {
    float rotationSpeed = 0.f;
    i32 flags = 0;
    float audioScaleMin = 0.f;
    float audioScaleMax = 0.f;
    float fadeMargin = 0.f;
    float opacity = 1.f;
    u32 baseColorChannelId = 0;
    u32 baseHSV = 0;
    u32 detailColorChannelId = 0;
    u32 detailHSV = 0;
};

struct PulseEntry {
    u32 flags = 0;
    union {
        u32 hsv;
        u32 color;
    };

    PulseEntry() : hsv(0) {}

    static PulseEntry endIndicator() {
        return PulseEntry{};
    }
};

struct GameState {
    glm::mat4 mvp {1.f};
    float timer = 0.f;
    float audioScale = 0.5f;
    glm::vec2 cameraViewSize {};
    glm::vec2 cameraPosition {};
    glm::vec2 cameraRight {1.f, 0.f};
    glm::vec2 cameraUp {0.f, 1.f};
    glm::vec2 cameraSize {};
    glm::vec2 windowSize {};
    float screenRight = 0.f;
    float cameraUnzoomedX = 0.f;
    glm::vec3 specialLightBGColor {};
    u32 gameStateFlags = 0;
};

struct LayerDrawRange {
    LayerKey key {};
    u32 firstInputSprite = 0;
    u32 spriteCount = 0;
    u32 firstVertex = 0;
    u32 vertexCount = 0;
    u32 firstWorkGroup = 0;
    u32 workGroupCount = 0;
};

struct SortedObjectBatch {
    u32 workGroupCount = 0;
    std::map<LayerKey, LayerDrawRange> layerRanges;
    std::vector<u32> inputSpriteIndices;
    std::vector<PreCulledSprite> preCulledSprites;
    std::vector<WorkGroup> workGroups;
};

using ColorChannelBlendStates = std::array<bool, IOS_COLOR_CHANNEL_COUNT>;
struct RGBA8 {
    u8 r = 255;
    u8 g = 255;
    u8 b = 255;
    u8 a = 255;
};
using ColorChannelTable = std::array<RGBA8, IOS_COLOR_CHANNEL_COUNT>;

inline u32 colorChannelForSortKey(u32 colorChannelFlags) {
    const u32 channel = colorChannelFlags & IOS_COLOR_CHANNEL_INDEX_MASK;
    return channel < IOS_COLOR_CHANNEL_COUNT ? channel : IOS_COLOR_CHANNEL_COUNT - 1;
}

} // namespace IOSUpstream

#endif
