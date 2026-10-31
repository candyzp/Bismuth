#ifdef GEODE_IS_IOS

#include "GPUSelfTest.hpp"
#include "../Buffer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace IOSGPUSelfTest {
namespace {

struct GLStateSnapshot {
    GLint framebuffer = 0;
    GLint arrayBuffer = 0;
    GLint currentProgram = 0;
    GLint activeTexture = GL_TEXTURE0;
    GLint texture0 = 0;
    GLint texture1 = 0;
    GLint viewport[4] = {0, 0, 0, 0};

    GLint attribBuffer = 0;
    GLint attribSize = 4;
    GLint attribType = GL_FLOAT;
    GLint attribNormalized = GL_FALSE;
    GLint attribStride = 0;
    GLint attribEnabled = GL_FALSE;
    const void* attribPointer = nullptr;

    GLboolean blend = GL_FALSE;
    GLboolean scissor = GL_FALSE;
    GLboolean depth = GL_FALSE;
    GLboolean cull = GL_FALSE;
    GLboolean dither = GL_FALSE;
    GLboolean colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLfloat clearColor[4] = {0.f, 0.f, 0.f, 0.f};

    GLStateSnapshot() {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glGetIntegerv(GL_VIEWPORT, viewport);

        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
        glActiveTexture(GL_TEXTURE1);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture1);
        glActiveTexture(static_cast<GLenum>(activeTexture));

        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attribBuffer);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attribSize);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attribType);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attribNormalized);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attribStride);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attribEnabled);
        glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attribPointer);

        blend = glIsEnabled(GL_BLEND);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        depth = glIsEnabled(GL_DEPTH_TEST);
        cull = glIsEnabled(GL_CULL_FACE);
        dither = glIsEnabled(GL_DITHER);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
    }

    void restore() const {
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));

        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(attribBuffer));
        glVertexAttribPointer(
            0,
            attribSize,
            static_cast<GLenum>(attribType),
            static_cast<GLboolean>(attribNormalized),
            attribStride,
            attribPointer
        );
        if (attribEnabled)
            glEnableVertexAttribArray(0);
        else
            glDisableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));

        glUseProgram(static_cast<GLuint>(currentProgram));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture0));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture1));
        glActiveTexture(static_cast<GLenum>(activeTexture));

        setEnabled(GL_BLEND, blend);
        setEnabled(GL_SCISSOR_TEST, scissor);
        setEnabled(GL_DEPTH_TEST, depth);
        setEnabled(GL_CULL_FACE, cull);
        setEnabled(GL_DITHER, dither);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
    }

    static void setEnabled(GLenum capability, GLboolean enabled) {
        if (enabled)
            glEnable(capability);
        else
            glDisable(capability);
    }

    ~GLStateSnapshot() {
        restore();
    }
};

struct GLResources {
    GLuint vertexShader = 0;
    GLuint fragmentShader = 0;
    GLuint program = 0;
    GLuint framebuffer = 0;
    GLuint targetTexture = 0;
    GLuint objectStateTexture = 0;
    GLuint spriteStateTexture = 0;
    GLuint vertexBuffer = 0;

    ~GLResources() {
        if (framebuffer)
            glDeleteFramebuffers(1, &framebuffer);
        if (targetTexture)
            glDeleteTextures(1, &targetTexture);
        if (objectStateTexture)
            glDeleteTextures(1, &objectStateTexture);
        if (spriteStateTexture)
            glDeleteTextures(1, &spriteStateTexture);
        if (vertexBuffer)
            glDeleteBuffers(1, &vertexBuffer);
        if (program)
            glDeleteProgram(program);
        if (vertexShader)
            glDeleteShader(vertexShader);
        if (fragmentShader)
            glDeleteShader(fragmentShader);
    }
};

static GLenum takeFirstGLError() {
    const GLenum first = glGetError();
    while (glGetError() != GL_NO_ERROR) {}
    return first;
}

static bool compileShader(
    GLenum type,
    const char* source,
    GLuint& shader,
    std::string& diagnostic
) {
    shader = glCreateShader(type);
    if (!shader) {
        diagnostic = "glCreateShader";
        return false;
    }

    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
        return true;

    GLint logLength = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    if (logLength > 1) {
        std::vector<char> logBuffer(static_cast<usize>(logLength), '\0');
        glGetShaderInfoLog(shader, logLength, nullptr, logBuffer.data());
        diagnostic.assign(logBuffer.data());
    } else {
        diagnostic = "shader compile failed without an info log";
    }
    return false;
}

static bool createFloatDataTexture(
    GLuint& texture,
    const std::array<float, 4>& texel
) {
    glGenTextures(1, &texture);
    if (!texture)
        return false;

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,
        1,
        1,
        0,
        GL_RGBA,
        GL_FLOAT,
        texel.data()
    );
    return takeFirstGLError() == GL_NO_ERROR;
}

} // namespace

Result run() {
    Result result;

    GLint vertexTextureUnits = 0;
    glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &vertexTextureUnits);
    result.vertexTextureUnits = vertexTextureUnits;
    if (vertexTextureUnits < 2) {
        result.stage = "vertex-texture-units";
        return result;
    }

    // Resource destruction runs after GL state restoration.
    GLResources resources;
    GLStateSnapshot savedState;

    // Ignore errors left by the game or another mod; every check below belongs
    // to this self-test transaction.
    while (glGetError() != GL_NO_ERROR) {}

    const char* vertexSource = R"GLSL(
        precision highp float;
        attribute vec2 a_position;
        uniform sampler2D u_objectState;
        uniform sampler2D u_spriteState;
        varying vec4 v_color;

        void main() {
            vec4 objectState = texture2D(u_objectState, vec2(0.5, 0.5));
            vec4 spriteState = texture2D(u_spriteState, vec2(0.5, 0.5));
            vec2 position = a_position * objectState.xy + objectState.zw;
            gl_Position = vec4(position, 0.0, 1.0);
            v_color = spriteState;
        }
    )GLSL";

    const char* fragmentSource = R"GLSL(
        precision mediump float;
        varying vec4 v_color;
        void main() {
            gl_FragColor = v_color;
        }
    )GLSL";

    std::string shaderDiagnostic;
    if (!compileShader(GL_VERTEX_SHADER, vertexSource, resources.vertexShader, shaderDiagnostic)) {
        result.stage = "vertex-shader-compile: " + shaderDiagnostic;
        result.glError = takeFirstGLError();
        return result;
    }
    if (!compileShader(GL_FRAGMENT_SHADER, fragmentSource, resources.fragmentShader, shaderDiagnostic)) {
        result.stage = "fragment-shader-compile: " + shaderDiagnostic;
        result.glError = takeFirstGLError();
        return result;
    }

    resources.program = glCreateProgram();
    if (!resources.program) {
        result.stage = "program-create";
        result.glError = takeFirstGLError();
        return result;
    }
    glAttachShader(resources.program, resources.vertexShader);
    glAttachShader(resources.program, resources.fragmentShader);
    glBindAttribLocation(resources.program, 0, "a_position");
    glLinkProgram(resources.program);

    GLint linked = GL_FALSE;
    glGetProgramiv(resources.program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint logLength = 0;
        glGetProgramiv(resources.program, GL_INFO_LOG_LENGTH, &logLength);
        if (logLength > 1) {
            std::vector<char> logBuffer(static_cast<usize>(logLength), '\0');
            glGetProgramInfoLog(resources.program, logLength, nullptr, logBuffer.data());
            result.stage = "program-link: ";
            result.stage += logBuffer.data();
        } else {
            result.stage = "program-link";
        }
        result.glError = takeFirstGLError();
        return result;
    }

    glGenFramebuffers(1, &resources.framebuffer);
    glGenTextures(1, &resources.targetTexture);
    if (!resources.framebuffer || !resources.targetTexture) {
        result.stage = "framebuffer-create";
        result.glError = takeFirstGLError();
        return result;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, resources.targetTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    if (takeFirstGLError() != GL_NO_ERROR) {
        result.stage = "render-target-allocation";
        result.glError = takeFirstGLError();
        return result;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, resources.framebuffer);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        resources.targetTexture,
        0
    );
    const GLenum framebufferStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (framebufferStatus != GL_FRAMEBUFFER_COMPLETE) {
        result.stage = "framebuffer-incomplete";
        result.glError = framebufferStatus;
        return result;
    }

    if (!createFloatDataTexture(
            resources.objectStateTexture,
            std::array<float, 4>{1.f, 1.f, 0.f, 0.f})) {
        result.stage = "object-state-float-texture";
        result.glError = takeFirstGLError();
        return result;
    }

    glActiveTexture(GL_TEXTURE1);
    if (!createFloatDataTexture(
            resources.spriteStateTexture,
            std::array<float, 4>{0.25f, 0.50f, 0.75f, 1.f})) {
        result.stage = "sprite-state-float-texture";
        result.glError = takeFirstGLError();
        return result;
    }

    const GLint objectSampler = glGetUniformLocation(resources.program, "u_objectState");
    const GLint spriteSampler = glGetUniformLocation(resources.program, "u_spriteState");
    if (objectSampler < 0 || spriteSampler < 0) {
        result.stage = "sampler-uniform-location";
        return result;
    }

    const GLfloat vertices[] = {
        -1.f, -1.f,
         1.f, -1.f,
        -1.f,  1.f,
         1.f,  1.f
    };
    glGenBuffers(1, &resources.vertexBuffer);
    if (!resources.vertexBuffer) {
        result.stage = "vertex-buffer-create";
        result.glError = takeFirstGLError();
        return result;
    }

    glBindBuffer(GL_ARRAY_BUFFER, resources.vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glUseProgram(resources.program);
    glUniform1i(objectSampler, 0);
    glUniform1i(spriteSampler, 1);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, nullptr);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, resources.objectStateTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, resources.spriteStateTexture);

    glViewport(0, 0, 4, 4);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const GLenum drawError = takeFirstGLError();
    if (drawError != GL_NO_ERROR) {
        result.stage = "offscreen-draw";
        result.glError = drawError;
        return result;
    }

    glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, result.pixel.data());
    const GLenum readError = takeFirstGLError();
    if (readError != GL_NO_ERROR) {
        result.stage = "pixel-readback";
        result.glError = readError;
        return result;
    }

    const auto near = [](unsigned char value, int expected) {
        return std::abs(static_cast<int>(value) - expected) <= 6;
    };
    if (!near(result.pixel[0], 64) ||
        !near(result.pixel[1], 128) ||
        !near(result.pixel[2], 191) ||
        !near(result.pixel[3], 255)) {
        result.stage = "pixel-validation";
        return result;
    }

    result.ok = true;
    result.stage = "complete";
    return result;
}

} // namespace IOSGPUSelfTest

#endif
