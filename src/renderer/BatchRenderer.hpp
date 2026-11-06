#pragma once

/*
    GD-independent core of the Bismuth renderer: owns the GPU resources, runs
    culling (GPU compute or CPU) and draws layers. Everything Geometry Dash
    specific (building the data from game objects, deciding when/where layers are
    drawn) lives in Renderer / ObjectBatch / GroupManager.
*/

#include "BatchData.hpp"
#include "CpuCuller.hpp"
#include <gfx/Device.hpp>
#include <bgfx/bgfx.h>
#include <unordered_map>
#include <string>

namespace bismuth {

enum class LayerBlend {
    Normal,     ///< premultiplied: ONE, ONE_MINUS_SRC_ALPHA
    Additive,   ///< SRC_ALPHA, ONE (blending color channels and glow)
};

class BatchRenderer {
public:
    BatchRenderer() = default;
    ~BatchRenderer();

    BatchRenderer(const BatchRenderer&) = delete;
    BatchRenderer& operator=(const BatchRenderer&) = delete;

    bool init(StaticBatchData data, gfx::CullingPath path, bgfx::RendererType::Enum renderer);
    void shutdown();

    gfx::CullingPath getCullingPath() const { return path; }

    /// The batch must stay alive (and unchanged) while it's in use.
    void setSortedBatch(const SortedBatchData* batch);
    void forgetSortedBatch(const SortedBatchData* batch);

    void updateFrame(const FrameBatchData& frame, const GameStateUniforms& gameState);

    /// Culls every sprite of the current batch. Returns false before drawing
    /// if a required buffer, shader or dispatch resource is unavailable.
    bool cull(bgfx::ViewId computeView, bool uploadToBgfx = true);
    const std::string& getResourceError() const { return resourceError; }

    /// CPU path: the last cull's vertices (CpuVertex) and per layer ranges.
    const std::vector<CpuVertex>& getCpuVertices() const { return cpuVertices; }
    LayerVertexRange getCpuLayerRange(u32 layerIndex) const {
        return layerIndex < cpuLayers.size() ? cpuLayers[layerIndex] : LayerVertexRange();
    }

    /// Draws one layer of the current batch.
    /// `toSharedTarget`: drawing into a transparent shared render target (native
    /// backends), which changes how additive alpha is accumulated.
    void drawLayer(
        bgfx::ViewId view,
        u32 layerIndex,
        bgfx::TextureHandle texture,
        LayerBlend blend,
        bool toSharedTarget,
        bool overdrawCount = false
    );

    bool hasLayer(u32 layerIndex) const;

    u64 getStaticBufferBytes() const { return staticBytes; }
    u64 getFrameBufferBytes() const { return frameBytes; }
    u32 getSpriteCount() const { return data.inputSpriteCount; }
    u32 getLastVisibleVertexCount() const { return lastVisibleVertices; }

    static u64 blendState(LayerBlend blend, bool toSharedTarget);

private:
    struct GpuBatch {
        bgfx::VertexBufferHandle buffer = BGFX_INVALID_HANDLE;
        bgfx::IndirectBufferHandle indirect = BGFX_INVALID_HANDLE;
    };

    GpuBatch& getGpuBatch(const SortedBatchData* batch);
    void setGameStateUniform();
    bool ensureFrameBuffer(u32 elements);
    bool ensureCounterBuffer(u32 workGroups);
    bool failResource(const char* message);

    gfx::CullingPath path = gfx::CullingPath::Cpu;
    bool initialized = false;
    StaticBatchData data;
    const SortedBatchData* batch = nullptr;
    GameStateUniforms gameState;
    u32 groupStateOffset = 0;
    u32 pulseOffset = 0;

    bgfx::VertexLayout uvec4Layout;
    bgfx::VertexLayout cpuVertexLayout;

    bgfx::UniformHandle u_gameState = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_mvp = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_spriteSheet = BGFX_INVALID_HANDLE;

    bgfx::ProgramHandle drawProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle overdrawProgram = BGFX_INVALID_HANDLE;

    //// GPU path ////
    bgfx::ProgramHandle cullProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle scanProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle copyProgram = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle staticBuffer = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle vertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::DynamicVertexBufferHandle frameBuffer = BGFX_INVALID_HANDLE;
    u32 frameBufferCapacity = 0;
    bgfx::DynamicIndexBufferHandle culledTris = BGFX_INVALID_HANDLE;
    bgfx::DynamicIndexBufferHandle compactTris = BGFX_INVALID_HANDLE;
    bgfx::DynamicVertexBufferHandle rawSprites = BGFX_INVALID_HANDLE;
    bgfx::DynamicIndexBufferHandle counters = BGFX_INVALID_HANDLE;
    u32 counterCapacity = 0;
    std::unordered_map<const SortedBatchData*, GpuBatch> gpuBatches;
    std::vector<UVec4> frameUpload;

    //// CPU path ////
    UPtr<CpuCuller> culler;
    const FrameBatchData* frame = nullptr;
    std::vector<CpuVertex> cpuVertices;
    std::vector<LayerVertexRange> cpuLayers;
    bgfx::DynamicVertexBufferHandle cpuVertexBuffer = BGFX_INVALID_HANDLE;
    u32 cpuVertexCapacity = 0;
    u32 lastVisibleVertices = 0;

    u64 staticBytes = 0;
    u64 frameBytes = 0;
    std::string resourceError;
};

} // namespace bismuth
