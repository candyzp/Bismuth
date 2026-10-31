#ifdef GEODE_IS_IOS

#include "IOSGPUCapabilities.hpp"
#include <Geode/Geode.hpp>
#include <OpenGLES/ES2/gl.h>
#include <OpenGLES/ES2/glext.h>

#include <cstdio>
#include <sstream>
#include <utility>

using namespace geode::prelude;

namespace IOSGPU {
namespace {

std::string glString(GLenum name) {
    const auto* value = glGetString(name);
    return value ? reinterpret_cast<const char*>(value) : std::string{};
}

int queryInteger(GLenum name) {
    GLint value = 0;
    glGetIntegerv(name, &value);
    return static_cast<int>(value);
}

void parseESVersion(const std::string& version, Capabilities& caps) {
    const char* prefix = "OpenGL ES ";
    const auto position = version.find(prefix);
    if (position == std::string::npos)
        return;

    caps.isOpenGLES = true;
    const char* number = version.c_str() + position + std::char_traits<char>::length(prefix);
    int major = 0;
    int minor = 0;
    if (std::sscanf(number, "%d.%d", &major, &minor) == 2) {
        caps.apiMajor = major;
        caps.apiMinor = minor;
    }
}

} // namespace

bool hasExtension(const Capabilities& caps, const char* extension) {
    if (!extension || !*extension)
        return false;

    for (const auto& current : caps.extensions) {
        if (current == extension)
            return true;
    }
    return false;
}

bool queryCapabilities(Capabilities& out) {
    out = {};
    out.version = glString(GL_VERSION);
    out.shadingLanguage = glString(GL_SHADING_LANGUAGE_VERSION);
    out.vendor = glString(GL_VENDOR);
    out.renderer = glString(GL_RENDERER);

    if (out.version.empty())
        return false;

    parseESVersion(out.version, out);

    out.maxVertexAttribs = queryInteger(GL_MAX_VERTEX_ATTRIBS);
    out.maxVertexUniformVectors = queryInteger(GL_MAX_VERTEX_UNIFORM_VECTORS);
    out.maxVertexTextureUnits = queryInteger(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS);
    out.maxFragmentTextureUnits = queryInteger(GL_MAX_TEXTURE_IMAGE_UNITS);
    out.maxCombinedTextureUnits = queryInteger(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS);
    out.maxTextureSize = queryInteger(GL_MAX_TEXTURE_SIZE);
    out.maxRenderbufferSize = queryInteger(GL_MAX_RENDERBUFFER_SIZE);

    GLint fragmentHighpRange[2] = {0, 0};
    GLint fragmentHighpPrecision = 0;
    glGetShaderPrecisionFormat(
        GL_FRAGMENT_SHADER,
        GL_HIGH_FLOAT,
        fragmentHighpRange,
        &fragmentHighpPrecision
    );
    out.fragmentHighpRangeMin = fragmentHighpRange[0];
    out.fragmentHighpRangeMax = fragmentHighpRange[1];
    out.fragmentHighpPrecision = fragmentHighpPrecision;
    out.hasFragmentHighpPrecision = fragmentHighpPrecision > 0;

    const std::string extensionString = glString(GL_EXTENSIONS);
    std::istringstream stream(extensionString);
    std::string extension;
    while (stream >> extension)
        out.extensions.push_back(std::move(extension));

    out.hasVertexTextureSampling = out.maxVertexTextureUnits > 0;
    out.hasVertexArrayObject =
        (out.apiMajor >= 3) ||
        hasExtension(out, "GL_OES_vertex_array_object");
    out.hasUIntElementIndices =
        (out.apiMajor >= 3) ||
        hasExtension(out, "GL_OES_element_index_uint");
    out.hasFloatTexture =
        (out.apiMajor >= 3) ||
        hasExtension(out, "GL_OES_texture_float");
    out.hasHalfFloatTexture =
        (out.apiMajor >= 3) ||
        hasExtension(out, "GL_OES_texture_half_float");
    out.hasFloatColorBuffer =
        hasExtension(out, "GL_EXT_color_buffer_float");
    out.hasHalfFloatColorBuffer =
        hasExtension(out, "GL_EXT_color_buffer_half_float");
    out.hasDrawBuffers =
        (out.apiMajor >= 3) ||
        hasExtension(out, "GL_EXT_draw_buffers");
    out.hasDisjointTimerQuery =
        hasExtension(out, "GL_EXT_disjoint_timer_query");

    // The upstream renderer requires desktop GL 4.3 mechanisms. ES 3.0 does
    // not provide compute shaders, SSBOs, or indirect multi-draw. Do not report
    // these as available merely because a similarly named extension is found.
    out.hasComputeShaders = out.isOpenGLES && (
        out.apiMajor > 3 || (out.apiMajor == 3 && out.apiMinor >= 1)
    );
    out.hasShaderStorageBuffers = out.hasComputeShaders;
    out.hasIndirectMultiDraw =
        hasExtension(out, "GL_EXT_multi_draw_indirect");

    return true;
}

void logCapabilities(const Capabilities& caps) {
    log::info(
        "Bismuth iOS GL: version='{}', GLSL='{}', vendor='{}', renderer='{}'",
        caps.version,
        caps.shadingLanguage,
        caps.vendor,
        caps.renderer
    );
    log::info(
        "Bismuth iOS GL limits: attribs {}, vertex uniforms {}, vertex texture units {}, fragment texture units {}, combined texture units {}, max texture size {}, max renderbuffer size {}, fragment highp range {}..{} precision {}",
        caps.maxVertexAttribs,
        caps.maxVertexUniformVectors,
        caps.maxVertexTextureUnits,
        caps.maxFragmentTextureUnits,
        caps.maxCombinedTextureUnits,
        caps.maxTextureSize,
        caps.maxRenderbufferSize,
        caps.fragmentHighpRangeMin,
        caps.fragmentHighpRangeMax,
        caps.fragmentHighpPrecision
    );
    log::info(
        "Bismuth iOS GL features: vertex textures {}, fragment highp {}, VAO {}, uint indices {}, float textures {}, half-float textures {}, float color targets {}, half-float color targets {}, MRT {}, disjoint timer queries {}, compute {}, SSBO {}, indirect multi-draw {}",
        caps.hasVertexTextureSampling,
        caps.hasFragmentHighpPrecision,
        caps.hasVertexArrayObject,
        caps.hasUIntElementIndices,
        caps.hasFloatTexture,
        caps.hasHalfFloatTexture,
        caps.hasFloatColorBuffer,
        caps.hasHalfFloatColorBuffer,
        caps.hasDrawBuffers,
        caps.hasDisjointTimerQuery,
        caps.hasComputeShaders,
        caps.hasShaderStorageBuffers,
        caps.hasIndirectMultiDraw
    );
}

} // namespace IOSGPU

#endif
