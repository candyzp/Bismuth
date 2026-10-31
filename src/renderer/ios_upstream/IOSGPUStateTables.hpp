#pragma once

#ifdef GEODE_IS_IOS

#include "IOSRenderRecords.hpp"
#include <memory>
#include <vector>

class DataTexture;

namespace IOSUpstream {

class SceneData;

// Texture-backed replacement for the owner's SSBO/SAO bindings. The field
// order and meanings remain owner-compatible; only the transport is changed
// for the iOS OpenGL ES 2 context.
class GPUStateTables {
public:
    struct Layout {
        u32 colorChannelsOffset = 0;
        u32 objectHSVOffset = 0;
        u32 pulseEntriesOffset = 0;
        u32 spriteAppearanceOffset = 0;

        u32 objectCount = 0;
        u32 groupCombinationCount = 0;
        u32 pulseEntryCount = 0;
        u32 spriteCount = 0;

        glm::vec2 objectTextureSize {};
        glm::vec2 groupTextureSize {};
        glm::vec2 byteTextureSize {};
    };

    GPUStateTables();
    ~GPUStateTables();

    GPUStateTables(const GPUStateTables&) = delete;
    GPUStateTables& operator=(const GPUStateTables&) = delete;

    bool initialize(const SceneData& scene);
    bool upload(const SceneData& scene);
    void bind(i32 objectUnit, i32 groupUnit, i32 byteUnit) const;

    bool isReady() const { return ready; }
    const Layout& getLayout() const { return layout; }
    u32 getObjectTexture() const;
    u32 getGroupTexture() const;
    u32 getByteTexture() const;

private:
    bool ensureFloatTexture(
        std::unique_ptr<DataTexture>& texture,
        const char* name,
        usize texelCount
    );
    bool ensureByteTexture(usize texelCount);
    void buildObjectTexels(const SceneData& scene);
    void buildGroupTexels(const SceneData& scene);
    void buildByteTexels(const SceneData& scene);

private:
    std::unique_ptr<DataTexture> objectTexture;
    std::unique_ptr<DataTexture> groupTexture;
    std::unique_ptr<DataTexture> byteTexture;

    std::vector<glm::vec4> objectTexels;
    std::vector<glm::vec4> groupTexels;
    std::vector<RGBA8> byteTexels;

    Layout layout {};
    bool ready = false;
};

} // namespace IOSUpstream

#endif
