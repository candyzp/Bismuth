#include <cstring>
#include "BatchRenderer.hpp"
#include <gfx/Programs.hpp>

#include <algorithm>
#include <limits>


namespace {

/// Gives a vector's contents to bgfx without copying (freed once uploaded).
/// Empty vectors become one zeroed uvec4, as buffers can't be empty.
template <class T>
const bgfx::Memory* moveToBgfx(std::vector<T>& vector) {
    if (vector.empty()) {
        const bgfx::Memory* memory = bgfx::alloc(sizeof(bismuth::UVec4));
        std::memset(memory->data, 0, memory->size);
        return memory;
    }

    auto* owned = new std::vector<T>(std::move(vector));
    vector = {};
    return bgfx::makeRef(
        owned->data(), (u32)(owned->size() * sizeof(T)),
        [](void*, void* userData) { delete static_cast<std::vector<T>*>(userData); },
        owned
    );
}

} // namespace

namespace bismuth {

template <typename H>
static void destroyHandle(H& handle) {
    if (bgfx::isValid(handle))
        bgfx::destroy(handle);
    handle = BGFX_INVALID_HANDLE;
}

BatchRenderer::~BatchRenderer() {
    shutdown();
}

bool BatchRenderer::failResource(const char* message) {
    if (resourceError.empty()) {
        resourceError = message;
        logf(LogLevel::Error, "Bismuth: %s", message);
    }
    return false;
}

bool BatchRenderer::init(StaticBatchData staticData, gfx::CullingPath cullingPath, bgfx::RendererType::Enum renderer) {
    shutdown();
    resourceError.clear();

    data = std::move(staticData);
    path = cullingPath;

    const bool gpu = path == gfx::CullingPath::Gpu;
    const auto profile = gfx::shaderProfileFor(renderer, gpu);

    uvec4Layout.begin()
        .add(bgfx::Attrib::TexCoord0, 4, bgfx::AttribType::Float)
        .end();

    cpuVertexLayout.begin()
        .add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Uint8, true)
        .end();

    u_gameState   = bgfx::createUniform("u_gameState", bgfx::UniformType::Vec4, GAME_STATE_VEC4S);
    u_mvp         = bgfx::createUniform("u_bismuthMvp", bgfx::UniformType::Mat4);
    s_spriteSheet = bgfx::createUniform("s_spriteSheet", bgfx::UniformType::Sampler);

    if (gpu) {
        drawProgram     = gfx::loadProgram("vs_object_gpu", "fs_object", profile);
        overdrawProgram = gfx::loadProgram("vs_object_gpu", "fs_overdraw_count", profile);
        cullProgram     = gfx::loadComputeProgram("cs_cull", profile);
        scanProgram     = gfx::loadComputeProgram("cs_scan", profile);
        copyProgram     = gfx::loadComputeProgram("cs_copy", profile);

        if (!bgfx::isValid(cullProgram) || !bgfx::isValid(scanProgram) || !bgfx::isValid(copyProgram)) {
            failResource("BGFX could not create all GPU culling compute programs");
            shutdown();
            return false;
        }

        // BGFX's resource sizes are 32-bit. Validate every large allocation
        // before narrowing sizes or moving the level data into BGFX.
        const u64 staticSize64 = (u64)data.staticBuffer.size() * sizeof(UVec4);
        const u64 vertexSize64 = (u64)data.vertices.size() * sizeof(PackedVertex);
        const u64 rawSpriteElements64 = (u64)std::max<u32>(data.inputSpriteCount, 1) * RAW_SPRITE_STRIDE;
        const u64 triangleBytes64 = (u64)std::max<u32>(data.triangleCount, 1) * sizeof(u32);
        const u64 rawSpriteBytes64 = rawSpriteElements64 * sizeof(UVec4);
        if (staticSize64 > std::numeric_limits<u32>::max()
            || vertexSize64 > std::numeric_limits<u32>::max()
            || rawSpriteElements64 > std::numeric_limits<u32>::max()
            || triangleBytes64 > std::numeric_limits<u32>::max()
            || rawSpriteBytes64 > std::numeric_limits<u32>::max()) {
            failResource("GPU batch exceeds BGFX's 32-bit buffer size limit");
            shutdown();
            return false;
        }

        const u32 staticSize = (u32)staticSize64;
        const u32 vertexSize = (u32)vertexSize64;
        const u32 rawSpriteElements = (u32)rawSpriteElements64;
        const u32 triangles = std::max<u32>(data.triangleCount, 1);

        staticBuffer = bgfx::createVertexBuffer(moveToBgfx(data.staticBuffer), uvec4Layout, BGFX_BUFFER_COMPUTE_READ);
        vertexBuffer = bgfx::createVertexBuffer(moveToBgfx(data.vertices), uvec4Layout, BGFX_BUFFER_COMPUTE_READ);
        culledTris  = bgfx::createDynamicIndexBuffer(triangles, BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ_WRITE);
        compactTris = bgfx::createDynamicIndexBuffer(triangles, BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ_WRITE);
        rawSprites  = bgfx::createDynamicVertexBuffer(rawSpriteElements, uvec4Layout, BGFX_BUFFER_COMPUTE_READ_WRITE);

        if (!bgfx::isValid(staticBuffer) || !bgfx::isValid(vertexBuffer)
            || !bgfx::isValid(culledTris) || !bgfx::isValid(compactTris)
            || !bgfx::isValid(rawSprites)) {
            failResource("BGFX could not allocate every GPU culling buffer for this level");
            shutdown();
            return false;
        }

        staticBytes = staticSize64 + vertexSize64 + triangleBytes64 * 2 + rawSpriteBytes64;

        logf(LogLevel::Info,
            "Bismuth: GPU buffers: objects %.1f MB, vertices %.1f MB, triangles 2 x %.1f MB, sprites %.1f MB",
            staticSize / 1048576.0, vertexSize / 1048576.0, triangles * 4 / 1048576.0,
            (double)rawSpriteBytes64 / 1048576.0);

    } else {
        drawProgram     = gfx::loadProgram("vs_object_cpu", "fs_object", profile);
        overdrawProgram = gfx::loadProgram("vs_object_cpu", "fs_overdraw_count", profile);
        culler = makeUnique<CpuCuller>();
        staticBytes = data.staticBuffer.size() * sizeof(UVec4) + data.vertices.size() * sizeof(PackedVertex);
    }

    if (!bgfx::isValid(u_gameState) || !bgfx::isValid(u_mvp) || !bgfx::isValid(s_spriteSheet)
        || !bgfx::isValid(drawProgram)) {
        failResource("BGFX could not create the renderer programs or uniforms");
        shutdown();
        return false;
    }

    initialized = true;
    return true;
}

void BatchRenderer::shutdown() {
    for (auto& [_, gpuBatch] : gpuBatches) {
        destroyHandle(gpuBatch.buffer);
        destroyHandle(gpuBatch.indirect);
    }
    gpuBatches.clear();

    destroyHandle(u_gameState);
    destroyHandle(u_mvp);
    destroyHandle(s_spriteSheet);
    destroyHandle(drawProgram);
    destroyHandle(overdrawProgram);
    destroyHandle(cullProgram);
    destroyHandle(scanProgram);
    destroyHandle(copyProgram);
    destroyHandle(staticBuffer);
    destroyHandle(vertexBuffer);
    destroyHandle(frameBuffer);
    destroyHandle(culledTris);
    destroyHandle(compactTris);
    destroyHandle(rawSprites);
    destroyHandle(counters);
    destroyHandle(cpuVertexBuffer);
    frameBufferCapacity = 0;
    counterCapacity = 0;
    cpuVertexCapacity = 0;
    culler = nullptr;
    batch = nullptr;
    frame = nullptr;
    initialized = false;
}

//// BATCHES ////

BatchRenderer::GpuBatch& BatchRenderer::getGpuBatch(const SortedBatchData* sorted) {
    auto it = gpuBatches.find(sorted);
    if (it != gpuBatches.end())
        return it->second;

    GpuBatch& gpuBatch = gpuBatches[sorted];
    const u64 size64 = (u64)sorted->buffer.size() * sizeof(UVec4);
    if (size64 > std::numeric_limits<u32>::max() || sorted->layerCount > std::numeric_limits<u16>::max()) {
        failResource("Sorted GPU batch exceeds BGFX buffer or indirect-draw limits");
        return gpuBatch;
    }
    const u32 size = (u32)size64;
    const bgfx::Memory* sortedMemory = nullptr;
    if (size == 0) {
        sortedMemory = bgfx::alloc(sizeof(UVec4));
        std::memset(sortedMemory->data, 0, sortedMemory->size);
    } else {
        sortedMemory = bgfx::copy(sorted->buffer.data(), size);
    }
    gpuBatch.buffer = bgfx::createVertexBuffer(sortedMemory, uvec4Layout, BGFX_BUFFER_COMPUTE_READ);
    gpuBatch.indirect = bgfx::createIndirectBuffer(std::max<u32>(sorted->layerCount, 1));
    if (!bgfx::isValid(gpuBatch.buffer) || !bgfx::isValid(gpuBatch.indirect))
        failResource("BGFX could not allocate a sorted sprite batch or indirect draw buffer");
    return gpuBatch;
}

void BatchRenderer::setSortedBatch(const SortedBatchData* sorted) {
    batch = sorted;
    if (batch && path == gfx::CullingPath::Gpu) {
        getGpuBatch(batch);
        ensureCounterBuffer(batch->workGroupCount);
    }
}

void BatchRenderer::forgetSortedBatch(const SortedBatchData* sorted) {
    auto it = gpuBatches.find(sorted);
    if (it != gpuBatches.end()) {
        destroyHandle(it->second.buffer);
        destroyHandle(it->second.indirect);
        gpuBatches.erase(it);
    }
    if (batch == sorted)
        batch = nullptr;
}

bool BatchRenderer::hasLayer(u32 layerIndex) const {
    return batch && layerIndex < batch->layerCount;
}

//// FRAME ////

bool BatchRenderer::ensureFrameBuffer(u32 elements) {
    if (bgfx::isValid(frameBuffer) && frameBufferCapacity >= elements)
        return true;
    destroyHandle(frameBuffer);
    const u64 capacity64 = std::max<u64>((u64)elements + elements / 2u, 1024u);
    if (capacity64 * sizeof(UVec4) > std::numeric_limits<u32>::max())
        return failResource("Per-frame GPU state buffer exceeds BGFX's size limit");
    frameBufferCapacity = (u32)capacity64;
    frameBuffer = bgfx::createDynamicVertexBuffer(frameBufferCapacity, uvec4Layout, BGFX_BUFFER_COMPUTE_READ);
    return bgfx::isValid(frameBuffer) || failResource("BGFX could not allocate the per-frame GPU state buffer");
}

bool BatchRenderer::ensureCounterBuffer(u32 workGroups) {
    const u64 needed64 = (u64)std::max<u32>(workGroups, 1) * WORK_GROUP_COUNTER_STRIDE;
    if (needed64 * sizeof(u32) > std::numeric_limits<u32>::max())
        return failResource("GPU workgroup counter buffer exceeds BGFX's size limit");
    const u32 needed = (u32)needed64;
    if (bgfx::isValid(counters) && counterCapacity >= needed)
        return true;
    destroyHandle(counters);
    counterCapacity = needed;
    counters = bgfx::createDynamicIndexBuffer(counterCapacity, BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ_WRITE);
    return bgfx::isValid(counters) || failResource("BGFX could not allocate the GPU workgroup counter buffer");
}

void BatchRenderer::updateFrame(const FrameBatchData& frameData, const GameStateUniforms& state) {
    gameState = state;
    groupStateOffset = frameData.groupStateOffset;
    pulseOffset = frameData.pulseOffset;
    frameBytes = frameData.buffer.size() * sizeof(UVec4);

    if (path == gfx::CullingPath::Gpu) {
        const u64 elements64 = std::max<u64>((u64)frameData.buffer.size(), 1u);
        const u64 bytes64 = elements64 * sizeof(UVec4);
        if (bytes64 > std::numeric_limits<u32>::max() || !ensureFrameBuffer((u32)elements64)) {
            failResource("Per-frame GPU state upload is too large or unavailable");
            return;
        }
        bgfx::update(frameBuffer, 0, bgfx::copy(frameData.buffer.data(), (u32)bytes64));
    } else {
        frame = &frameData;
    }
}

void BatchRenderer::setGameStateUniform() {
    const auto& gs = gameState;
    float values[GAME_STATE_VEC4S * 4] = {
        gs.timer, gs.audioScale, gs.cameraViewSize.x, gs.cameraViewSize.y,
        gs.cameraPosition.x, gs.cameraPosition.y, gs.cameraRight.x, gs.cameraRight.y,
        gs.cameraUp.x, gs.cameraUp.y, gs.cameraSize.x, gs.cameraSize.y,
        gs.winSize.x, gs.winSize.y, gs.screenRight, gs.cameraUnzoomedX,
        gs.specialLightBGColor.x, gs.specialLightBGColor.y, gs.specialLightBGColor.z, float(gs.flags),
        float(groupStateOffset), float(pulseOffset), float(data.inputSpriteOffset), float(batch ? batch->workGroupOffset : 0),
        float(batch ? batch->layerOffset : 0), float(batch ? batch->workGroupCount : 0), float(batch ? batch->layerCount : 0), 0.f,
    };
    bgfx::setUniform(u_gameState, values, GAME_STATE_VEC4S);
}

bool BatchRenderer::cull(bgfx::ViewId computeView, bool uploadToBgfx) {
    if (!resourceError.empty() || !initialized || !batch)
        return false;

    if (path == gfx::CullingPath::Cpu) {
        if (!frame)
            return failResource("CPU culling has no current frame data");

        culler->run(data, *batch, *frame, gameState, cpuVertices, cpuLayers);
        lastVisibleVertices = (u32)cpuVertices.size();
        if (!uploadToBgfx)
            return true;

        const u64 count64 = std::max<u64>((u64)cpuVertices.size(), 1u);
        const u64 bytes64 = count64 * sizeof(CpuVertex);
        if (count64 > std::numeric_limits<u32>::max() || bytes64 > std::numeric_limits<u32>::max())
            return failResource("CPU-culler vertex upload exceeds BGFX's size limit");
        const u32 count = (u32)count64;
        if (!bgfx::isValid(cpuVertexBuffer) || cpuVertexCapacity < count) {
            destroyHandle(cpuVertexBuffer);
            const u64 capacity64 = std::max<u64>(count64 + count64 / 2u, 4096u);
            if (capacity64 * sizeof(CpuVertex) > std::numeric_limits<u32>::max())
                return failResource("CPU-culler vertex buffer capacity exceeds BGFX's size limit");
            cpuVertexCapacity = (u32)capacity64;
            cpuVertexBuffer = bgfx::createDynamicVertexBuffer(cpuVertexCapacity, cpuVertexLayout);
        }
        if (!bgfx::isValid(cpuVertexBuffer))
            return failResource("BGFX could not allocate the CPU-culler vertex upload buffer");
        if (!cpuVertices.empty()) {
            // The vector stays untouched until the next cull, after this frame is submitted.
            bgfx::update(cpuVertexBuffer, 0, bgfx::makeRef(cpuVertices.data(), (u32)bytes64));
        }
        return true;
    }

    GpuBatch& gpuBatch = getGpuBatch(batch);
    if (!resourceError.empty() || !bgfx::isValid(gpuBatch.buffer) || !bgfx::isValid(gpuBatch.indirect))
        return failResource("GPU culling batch resources are unavailable");
    if (!ensureCounterBuffer(batch->workGroupCount))
        return false;

    if (batch->workGroupCount == 0)
        return true;
    if (!bgfx::isValid(staticBuffer) || !bgfx::isValid(vertexBuffer)
        || !bgfx::isValid(frameBuffer) || !bgfx::isValid(culledTris)
        || !bgfx::isValid(compactTris) || !bgfx::isValid(rawSprites)
        || !bgfx::isValid(counters) || !bgfx::isValid(cullProgram)
        || !bgfx::isValid(scanProgram) || !bgfx::isValid(copyProgram))
        return failResource("GPU culling stopped because one or more compute resources are invalid");

    // Pass 1: parallel cull + transform (one sprite per GPU lane).
    setGameStateUniform();
    bgfx::setBuffer(0, staticBuffer, bgfx::Access::Read);
    bgfx::setBuffer(1, gpuBatch.buffer, bgfx::Access::Read);
    bgfx::setBuffer(2, frameBuffer, bgfx::Access::Read);
    bgfx::setBuffer(3, culledTris, bgfx::Access::Write);
    bgfx::setBuffer(4, rawSprites, bgfx::Access::Write);
    bgfx::setBuffer(5, counters, bgfx::Access::Write);
    bgfx::dispatch(computeView, cullProgram, batch->workGroupCount, 1, 1);

    // Pass 2: per-layer prefix sums + indirect draw commands.
    setGameStateUniform();
    bgfx::setBuffer(0, gpuBatch.buffer, bgfx::Access::Read);
    bgfx::setBuffer(1, counters, bgfx::Access::ReadWrite);
    bgfx::setBuffer(2, gpuBatch.indirect, bgfx::Access::Write);
    bgfx::dispatch(computeView, scanProgram, std::max<u32>(batch->layerCount, 1), 1, 1);

    // Pass 3: compaction into per-layer contiguous index ranges.
    setGameStateUniform();
    bgfx::setBuffer(0, gpuBatch.buffer, bgfx::Access::Read);
    bgfx::setBuffer(1, counters, bgfx::Access::Read);
    bgfx::setBuffer(2, culledTris, bgfx::Access::Read);
    bgfx::setBuffer(3, compactTris, bgfx::Access::Write);
    bgfx::dispatch(computeView, copyProgram, batch->workGroupCount, 1, 1);
    return true;
}

//// DRAWING ////

u64 BatchRenderer::blendState(LayerBlend blend, bool toSharedTarget) {
    u64 state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
    if (blend == LayerBlend::Normal) {
        state |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
    } else if (!toSharedTarget) {
        // Exactly what cocos2d uses: glBlendFunc(GL_SRC_ALPHA, GL_ONE).
        state |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE);
    } else {
        // In a transparent layer target, additive sprites must not change the
        // target's coverage (alpha), so compositing it with ONE, ONE_MINUS_SRC_ALPHA
        // equals drawing the sprites directly.
        state |= BGFX_STATE_BLEND_FUNC_SEPARATE(
            BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE,
            BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_ONE);
    }
    return state;
}

void BatchRenderer::drawLayer(
    bgfx::ViewId view,
    u32 layerIndex,
    bgfx::TextureHandle texture,
    LayerBlend blend,
    bool toSharedTarget,
    bool overdrawCount
) {
    if (!initialized || !hasLayer(layerIndex))
        return;

    bgfx::ProgramHandle program = overdrawCount ? overdrawProgram : drawProgram;
    if (!bgfx::isValid(program))
        return;

    u64 state = overdrawCount
        ? (BGFX_STATE_WRITE_R | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE))
        : blendState(blend, toSharedTarget);

    if (path == gfx::CullingPath::Cpu) {
        if (layerIndex >= cpuLayers.size() || cpuLayers[layerIndex].vertexCount == 0 || !bgfx::isValid(cpuVertexBuffer))
            return;

        const auto& range = cpuLayers[layerIndex];
        bgfx::setUniform(u_mvp, gameState.mvp);
        bgfx::setTexture(0, s_spriteSheet, texture, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        bgfx::setVertexBuffer(0, cpuVertexBuffer, range.firstVertex, range.vertexCount);
        bgfx::setState(state);
        bgfx::submit(view, program);
        return;
    }

    GpuBatch& gpuBatch = getGpuBatch(batch);
    if (!resourceError.empty() || !bgfx::isValid(gpuBatch.buffer) || !bgfx::isValid(gpuBatch.indirect)
        || !bgfx::isValid(compactTris) || !bgfx::isValid(rawSprites) || !bgfx::isValid(vertexBuffer))
        return;

    bgfx::setUniform(u_mvp, gameState.mvp);
    bgfx::setTexture(0, s_spriteSheet, texture, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    bgfx::setBuffer(1, vertexBuffer, bgfx::Access::Read);
    bgfx::setBuffer(2, compactTris, bgfx::Access::Read);
    bgfx::setBuffer(3, rawSprites, bgfx::Access::Read);
    // Attribute-less draw: vertices are pulled from storage buffers.
    bgfx::setVertexCount(3);
    bgfx::setState(state);
    bgfx::submit(view, program, gpuBatch.indirect, (u16)layerIndex, 1);
}

} // namespace bismuth
