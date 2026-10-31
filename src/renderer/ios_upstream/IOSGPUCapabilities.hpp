#pragma once

#ifdef GEODE_IS_IOS

#include <string>
#include <vector>

namespace IOSGPU {

struct Capabilities {
    std::string version;
    std::string shadingLanguage;
    std::string vendor;
    std::string renderer;
    std::vector<std::string> extensions;

    int apiMajor = 0;
    int apiMinor = 0;
    int maxVertexAttribs = 0;
    int maxVertexUniformVectors = 0;
    int maxVertexTextureUnits = 0;
    int maxFragmentTextureUnits = 0;
    int maxCombinedTextureUnits = 0;
    int maxTextureSize = 0;
    int maxRenderbufferSize = 0;
    int fragmentHighpRangeMin = 0;
    int fragmentHighpRangeMax = 0;
    int fragmentHighpPrecision = 0;

    bool isOpenGLES = false;
    bool hasVertexTextureSampling = false;
    bool hasFragmentHighpPrecision = false;
    bool hasVertexArrayObject = false;
    bool hasUIntElementIndices = false;
    bool hasFloatTexture = false;
    bool hasHalfFloatTexture = false;
    bool hasFloatColorBuffer = false;
    bool hasHalfFloatColorBuffer = false;
    bool hasDrawBuffers = false;
    bool hasDisjointTimerQuery = false;
    bool hasComputeShaders = false;
    bool hasShaderStorageBuffers = false;
    bool hasIndirectMultiDraw = false;
};

bool queryCapabilities(Capabilities& out);
bool hasExtension(const Capabilities& caps, const char* extension);
void logCapabilities(const Capabilities& caps);

} // namespace IOSGPU

#endif
