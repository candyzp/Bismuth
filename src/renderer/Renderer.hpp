#pragma once

#include <Geode/binding/GameObject.hpp>
#include <common.hpp>
#include "Geode/cocos/sprite_nodes/CCSpriteBatchNode.h"
#include "ObjectBatch.hpp"
#include "ObjectUtils.hpp"
#include "OverdrawView.hpp"
#include <array>
#include <deque>
#include <unordered_map>
#include <unordered_set>

#include "BatchRenderer.hpp"
#include "DifferenceMode.hpp"
#include "GroupManager.hpp"
#include "ObjectBatchNode.hpp"
#include <gfx/Device.hpp>
#include <gfx/HostTextures.hpp>

using namespace geode;

u32 convertToShaderHSV(const cocos2d::ccHSVValue& hsv);

/// Initializes (or re-initializes after a settings change / context loss) the
/// bgfx device. Must be called on the GL thread. `report` lists failed backends.
bool ensureBismuthDevice(std::string& report);

/// bgfx views used by the renderer (lower ids are rendered first).
namespace RendererViews {
    constexpr bgfx::ViewId Compute     = 0;
    constexpr bgfx::ViewId HostDraw    = 1;
    constexpr bgfx::ViewId OverdrawCount = 2;
    constexpr bgfx::ViewId GroupFirst  = 3;   // one per composite group (native backends)
    constexpr u32          MaxGroups   = 24;
    constexpr bgfx::ViewId Overlay     = GroupFirst + MaxGroups;
}

/*
    A run of Bismuth layers with no other cocos2d node drawn in between: they can
    be rendered together and, with Vulkan/Metal, composited as one texture.
*/
struct CompositeGroup {
    std::vector<ObjectBatchNode*> nodes;
};

class Renderer : public cocos2d::CCNode {
private:
    inline Renderer()
        : objectBatch(*this), groupManager(*this), differenceMode(*this),
          overdrawView(*this) {}
    ~Renderer() override;

    bool init(PlayLayer* layer, cocos2d::CCArray* objects);

    void generateBatchNodes();

    void terminate();

    void prepareGameState();

    void prepareColorChannels();

    void generateStaticRenderingBuffer(std::vector<GameObject*>& objects, bismuth::StaticBatchData& out);

    void draw() override;

    void finish();

    void updateDebugText();

    void fetchBlendingStates();

    void computeCompositeGroups();

    bool ensureSharedTargets(u32 count, u32 width, u32 height);
    void releaseSharedTargets();

    void drawGroupLayers(const CompositeGroup& group, bgfx::ViewId view, bool toSharedTarget);

    /// Stops rendering with Bismuth for the rest of the level (vanilla rendering takes over).
    void deactivate(const char* reason);

    friend class FinishNode;
    friend class ObjectBatch;
    friend class ObjectBatchNode;
    friend class GroupManager;
    friend class OverdrawView;

public:
    void update(float dt) override;

    /// Called by every ObjectBatchNode at its place in the draw order.
    void drawLayerNode(ObjectBatchNode* node);

    cocos2d::CCSpriteBatchNode* getSpriteBatchNodeWithLayerId(LayerKey id);

    inline bool isColorChannelBlending(i32 channel) {
        return colorChannelsIsBlending[channel];
    }

    inline cocos2d::CCTexture2D* getSpriteSheetTexture(SpriteSheet sheet) {
        if ((i32)sheet < 0 || (i32)sheet >= (i32)SpriteSheet::COUNT)
            return nullptr;
        return spriteSheets[(i32)sheet];
    }

    inline bgfx::TextureHandle getSpriteSheetHandle(SpriteSheet sheet) {
        if ((i32)sheet < 0 || (i32)sheet >= (i32)SpriteSheet::COUNT)
            return BGFX_INVALID_HANDLE;
        return spriteSheetHandles[(i32)sheet];
    }

    inline PlayLayer* getPlayLayer() {
        return layer;
    }

    inline usize getObjectSRBIndex(GameObject* object) {
        return objectSRBIndicies[object];
    }

    inline bool isEnabled() { return enabled; }

    /// False once Bismuth gave up for this level (e.g. the GL context was lost).
    inline bool isActive() { return active; }

    inline DifferenceMode* getDifferenceMode() {
        if (!differenceModeEnabled) return nullptr;
        return &differenceMode;
    }

    inline void toggleDebugText() {
        debugTextEnabled = !debugTextEnabled;
    }

    inline bool isDifferenceModeEnabled() const { return differenceModeEnabled; }
    inline void setDifferenceModeEnabled(bool enabled) {
        differenceModeEnabled = enabled;
    }

    inline bool canEnableDisableIngame() const { return ingameEnableDisable; }

    inline bool isPaused() {
        return layer->m_isPaused;
    }

    inline ObjectBatch& getObjectBatch() { return objectBatch; }

    inline bismuth::BatchRenderer& getBatchRenderer() { return batchRenderer; }

    bool useOptimizations();

    void setEnabled(bool enabled);

    inline GroupManager& getGroupManager() { return groupManager; }

    inline const ColorChannelBlendStates& getColorChannelBlendStates() { return colorChannelsIsBlending; }

    inline OverdrawView& getOverdrawView() { return overdrawView; }

    void reset();

    /// Claims / releases the particle systems of objects near the camera.
    /// With optimizations on, GD's own updateVisibility() (which does this when
    /// it activates objects) doesn't run, see RendererPlayLayer::updateVisibility.
    void updateObjectParticles(float dt);

    /// Releases the particles claimed by updateObjectParticles(), before GD's
    /// own updateVisibility() takes over again.
    void releaseObjectParticles();

    /// The composite group `node` belongs to this frame, or -1.
    i32 getGroupOfNode(ObjectBatchNode* node) const;

    static bismuth::LayerBlend blendForLayer(const LayerKey& key);

public:
    static Ref<Renderer> create(PlayLayer* layer, cocos2d::CCArray* objects);

    /// Imports GD's sprite sheets into bgfx ahead of the first level (slow with
    /// Vulkan / Metal, which copy them). Imports are kept for the session.
    static void preloadSpriteSheets();

    /**
     * Returns the current renderer. If the game
     * is not in a PlayLayer. This returns nullptr.
     */
    static Ref<Renderer> get();

private:
    static void importSpriteSheets(cocos2d::CCTexture2D* textures[], bgfx::TextureHandle handles[]);

    PlayLayer* layer;

    bool enabled = false;
    bool active = false;
    bool disabledInSettings = false;
    bool debugTextEnabled = false;
    bool ingameEnableDisable = false;
    bool hasCulledOnce = false;
    bismuth::SortedBatchData* lastSortedBatch = nullptr;

    u64 rendererStartTime = 0;

    std::unordered_map<GameObject*, usize> objectSRBIndicies;

    std::vector<ObjectBatchNode*> batchNodes;

    struct ParticleObject {
        GameObject* object;
        bool claimed;
    };
    std::vector<ParticleObject> particleObjects;
    usize claimedParticleCount = 0;
    std::unordered_set<cocos2d::CCNode*> ownNodes;

    cocos2d::CCTexture2D* spriteSheets[(i32)SpriteSheet::COUNT] = { nullptr };
    bgfx::TextureHandle spriteSheetHandles[(i32)SpriteSheet::COUNT];

    ColorChannelBlendStates colorChannelsIsBlending;

    Ref<cocos2d::CCLabelBMFont> debugText;
    Ref<cocos2d::CCLabelBMFont> debugTextOutline1;
    Ref<cocos2d::CCLabelBMFont> debugTextOutline2;

    GPUTimer renderingTimer { "Rendering time" };

    GroupManager groupManager;

    ObjectBatch objectBatch;
    bismuth::BatchRenderer batchRenderer;

    bismuth::FrameBatchData frameData;
    bismuth::GameStateUniforms gameState;

    //// COMPOSITE GROUPS ////
    std::vector<CompositeGroup> groups;
    std::unordered_map<ObjectBatchNode*, i32> groupOfNode;
    // Interop retains pointers to these entries; deque keeps addresses stable as targets are appended.
    std::deque<bismuth::gfx::SharedTarget> sharedTargets;
    u32 sharedTargetWidth = 0;
    u32 sharedTargetHeight = 0;
    bool nativeFrameSubmitted = false;

    // OpenGL mode with CPU culling: sprites are drawn with gfx::HostSpriteRenderer
    // instead of bgfx (decided every frame, see draw()).
    bool directHostDraw = false;
    bool lastFrameDirectHostDraw = false;
    std::vector<bismuth::gfx::HostSpriteRenderer::Layer> directLayers;
    void drawGroupDirect(const CompositeGroup& group);

    bool differenceModeEnabled = false;
    DifferenceMode differenceMode;
    OverdrawView overdrawView;

    float gameTimer = 0.0;

    i64 renderedGameObjectCount;
};
