#pragma once

#include <Geode/platform/cplatform.h>

#if defined(GEODE_IS_IOS)

#include <cstdint>
#include <string>

namespace BismuthIOSMetal {

// Immutable level-lifetime inputs. These are extracted once when a level is
// loaded, just like Bismuth's InputSprite/ObjectInfo/pre-culled sprite buffers.
struct SpriteWorkItem {
    float startX = 0.f;
    float startY = 0.f;
    std::uint32_t groupCombinationIndex = 0;
    std::uint32_t spriteIndex = 0;
};

struct InputSprite {
    std::uint32_t objectIndex = 0;
    std::uint32_t spriteType = 0;
    std::uint32_t triangleStart = 0;
    std::uint32_t triangleCount = 0;
};

// Field order follows Bismuth's objectInfoBuffer.h / ObjectInfo.
struct ObjectInfo {
    float rotationSpeed = 0.f;
    std::int32_t flags = 0;
    float audioScaleMin = 0.f;
    float audioScaleMax = 0.f;
    float fadeMargin = 0.f;
    std::uint32_t opacityValues = 0xffffffffu;
    std::uint32_t baseColorChannelId = 0;
    std::uint32_t baseHSVHueAndFlags = 0;
    std::uint32_t baseHSVSaturationAndValue = 0;
    std::uint32_t detailColorChannelId = 0;
    std::uint32_t detailHSVHueAndFlags = 0;
    std::uint32_t detailHSVSaturationAndValue = 0;
    std::uint32_t staticGlowColor = 0;
};

// This table is indexed by group-combination, not by object or sprite.
// Trigger updates therefore touch group records, never per-sprite transforms.
struct GroupCombinationState {
    float positionalTransform[4] {1.f, 0.f, 0.f, 1.f};
    float localTransform[4] {1.f, 0.f, 0.f, 1.f};
    float offset[2] {0.f, 0.f};
    float opacity = 1.f;
    std::uint32_t pulseIndex = 0;
};

// Bismuth's pulse-entry GPU record is 12 bytes.
struct PulseEntry {
    std::uint32_t flags = 0;
    std::uint32_t colorOrHSV = 0;
    std::uint32_t hsvSaturationAndValue = 0;
};

// Per-frame data is a compact constant block plus the group/color/pulse tables.
// There is deliberately no sprite count-sized CPU output structure here.
struct alignas(16) FrameState {
    float cameraPositionX = 0.f;
    float cameraPositionY = 0.f;
    float cameraRightX = 1.f;
    float cameraRightY = 0.f;
    float cameraUpX = 0.f;
    float cameraUpY = 1.f;
    float cameraWidth = 0.f;
    float cameraHeight = 0.f;

    float cameraViewWidth = 0.f;
    float cameraViewHeight = 0.f;
    float windowWidth = 0.f;
    float windowHeight = 0.f;

    float timer = 0.f;
    float audioScale = 0.5f;
    float gameStateCameraPositionX = 0.f;
    float gameStateCameraZoom = 1.f;

    float winSizeWidth = 0.f;
    float levelFlipProgress = 0.f;
    float screenRight = 0.f;
    float cameraUnzoomedX = 0.f;

    float specialLightBGColor[4] {0.f, 0.f, 0.f, 1.f};

    std::uint32_t gameStateFlags = 0;
    std::uint32_t objectCount = 0;
    std::uint32_t groupCount = 0;
    std::uint32_t pulseCount = 0;
    std::uint32_t colorChannelCount = 1101;
    std::uint32_t spriteCount = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

static_assert(sizeof(SpriteWorkItem) == 16);
static_assert(sizeof(InputSprite) == 16);
static_assert(sizeof(ObjectInfo) == 52);
static_assert(sizeof(GroupCombinationState) == 48);
static_assert(sizeof(PulseEntry) == 12);
static_assert(sizeof(FrameState) == 128);

struct OutputFrame {
    std::uint32_t textureName = 0;
    std::uint32_t slot = 0;
    std::uint64_t sequence = 0;
    std::uint32_t textureWidth = 0;
    std::uint32_t textureHeight = 0;
    std::uint32_t visibleSpriteCount = 0;
    std::uint32_t totalSpriteCount = 0;
    std::uint32_t groupValidSpriteCount = 0;
    std::uint32_t screenVisibleSpriteCount = 0;
    std::uint32_t threadgroupCount = 0;
    std::uint32_t threadsPerThreadgroup = 0;
    double gpuTimeMilliseconds = 0.0;
    bool hasGpuTiming = false;
    bool stateTextureNeedsYFlip = false;
};

class ComputeRenderer {
public:
    ComputeRenderer() = default;
    ~ComputeRenderer();

    ComputeRenderer(const ComputeRenderer&) = delete;
    ComputeRenderer& operator=(const ComputeRenderer&) = delete;

    // Upload immutable per-level data exactly once.
    bool initializeLevel(
        const SpriteWorkItem* workItems, std::uint32_t workItemCount,
        const InputSprite* sprites, std::uint32_t spriteCount,
        const ObjectInfo* objects, std::uint32_t objectCount,
        std::string& error
    );

    // Upload only compact changing state and enqueue the Metal kernel.
    // This method never waits for the GPU. False means the frame was skipped
    // because all output slots are busy; the caller should keep using its
    // previous completed output.
    bool dispatchFrame(
        const GroupCombinationState* groupStates, std::uint32_t groupCount,
        const std::uint32_t* colorChannels, std::uint32_t colorChannelCount,
        const PulseEntry* pulseEntries, std::uint32_t pulseCount,
        const FrameState& frameState,
        std::string& error
    );

    // Acquire the newest completed texture, or the last completed texture
    // when the newest command is still running.
    bool acquireLatestFrame(OutputFrame& output);

    // Call after the GLES draws that sampled this texture have been queued.
    // A zero-timeout GL_APPLE_sync fence protects the slot from early reuse.
    bool finishOpenGLRead(const OutputFrame& output, std::string& error);

    void reset();
    bool isReady() const;
    std::string getDeviceName() const;
    std::string takeLastError();

private:
    void* m_owner = nullptr;
};

} // namespace BismuthIOSMetal

#endif
