#ifdef GEODE_IS_IOS

#include "../Renderer.hpp"
#include "../Shader.hpp"

#include <Geode/binding/GJGroundLayer.hpp>
#include <Geode/modify/CCSprite.hpp>
#include "Geode/cocos/kazmath/include/kazmath/mat4.h"

using namespace geode::prelude;

namespace {
struct GroundGPUResources {
    Shader* shader = nullptr;
    u32 vao = 0;
    u32 vertexBuffer = 0;
    u32 indexBuffer = 0;

    GLint mvp = -1;
    GLint posBL = -1;
    GLint posBR = -1;
    GLint posTL = -1;
    GLint posTR = -1;
    GLint uvBL = -1;
    GLint uvBR = -1;
    GLint uvTL = -1;
    GLint uvTR = -1;
    GLint color = -1;
    GLint opacityModifyRGB = -1;
    GLint texture = -1;

    bool attemptedInit = false;
    bool announced = false;
    bool failureAnnounced = false;

    // Debug proof is intentionally based only on successful custom GL submits.
    // It never infers floor ownership from renderer state or from a fallback.
    usize successfulDraws = 0;
    usize failedDraws = 0;
    bool ground1Proven = false;
    bool ground2Proven = false;
    bool lineProven = false;
    bool truthAnnounced = false;
};

struct SavedGroundGLState {
    GLint vao = 0;
    GLint arrayBuffer = 0;
    GLint elementBuffer = 0;
    GLint program = 0;
    GLint activeTexture = GL_TEXTURE0;
    GLint texture0 = 0;

    // The ground path changes the blend function. Preserve it exactly so a
    // ground submit cannot leak alpha state into later GD sprites/objects.
    GLboolean blendEnabled = GL_FALSE;
    GLint blendSrcRGB = GL_SRC_ALPHA;
    GLint blendDstRGB = GL_ONE_MINUS_SRC_ALPHA;
    GLint blendSrcAlpha = GL_SRC_ALPHA;
    GLint blendDstAlpha = GL_ONE_MINUS_SRC_ALPHA;
};

GroundGPUResources& groundGPU() {
    static GroundGPUResources state;
    return state;
}

SavedGroundGLState captureGroundGLState() {
    SavedGroundGLState state;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &state.vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &state.arrayBuffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &state.elementBuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &state.program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &state.activeTexture);

    state.blendEnabled = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_BLEND_SRC_RGB, &state.blendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &state.blendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &state.blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &state.blendDstAlpha);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &state.texture0);
    glActiveTexture(static_cast<GLenum>(state.activeTexture));
    return state;
}

void restoreGroundGLState(const SavedGroundGLState& state) {
    glBindVertexArray(static_cast<u32>(state.vao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<u32>(state.arrayBuffer));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<u32>(state.elementBuffer));
    glUseProgram(static_cast<u32>(state.program));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<u32>(state.texture0));
    glActiveTexture(static_cast<GLenum>(state.activeTexture));

    glBlendFuncSeparate(
        static_cast<GLenum>(state.blendSrcRGB),
        static_cast<GLenum>(state.blendDstRGB),
        static_cast<GLenum>(state.blendSrcAlpha),
        static_cast<GLenum>(state.blendDstAlpha)
    );
    if (state.blendEnabled)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
}

bool initGroundGPU() {
    auto& state = groundGPU();
    if (state.attemptedInit)
        return state.shader && state.vao && state.vertexBuffer && state.indexBuffer;
    state.attemptedInit = true;

    static const char* vertexSource = R"(
precision highp float;

// One persistent unit quad. The exact stock Cocos quad corners are supplied as
// uniforms and expanded here, so the GPU performs the repetitive ground-quad
// position/UV math instead of rebuilding a transient vertex array on the CPU.
attribute vec2 a_positionOffset;

uniform mat4 u_mvp;
uniform vec3 u_posBL;
uniform vec3 u_posBR;
uniform vec3 u_posTL;
uniform vec3 u_posTR;
uniform vec2 u_uvBL;
uniform vec2 u_uvBR;
uniform vec2 u_uvTL;
uniform vec2 u_uvTR;
uniform vec4 u_color;
uniform float u_opacityModifyRGB;

varying vec2 t_texCoord;
varying vec4 t_color;

void main() {
    vec3 bottomPosition = mix(u_posBL, u_posBR, a_positionOffset.x);
    vec3 topPosition = mix(u_posTL, u_posTR, a_positionOffset.x);
    vec3 localPosition = mix(bottomPosition, topPosition, a_positionOffset.y);

    vec2 bottomUV = mix(u_uvBL, u_uvBR, a_positionOffset.x);
    vec2 topUV = mix(u_uvTL, u_uvTR, a_positionOffset.x);
    t_texCoord = mix(bottomUV, topUV, a_positionOffset.y);

    vec3 rgb = u_color.rgb;
    if (u_opacityModifyRGB > 0.5)
        rgb *= u_color.a;
    t_color = vec4(rgb, u_color.a);

    gl_Position = u_mvp * vec4(localPosition, 1.0);
}
)";

    static const char* fragmentSource = R"(
precision mediump float;

uniform sampler2D u_spriteSheetTexture;
varying vec2 t_texCoord;
varying vec4 t_color;

void main() {
    gl_FragColor = texture2D(u_spriteSheetTexture, t_texCoord) * t_color;
}
)";

    state.shader = Shader::create({ vertexSource, fragmentSource });
    if (!state.shader) {
        log::error("Bismuth iOS STRICT ground GPU shader unavailable; stock ground fallback is disabled");
        return false;
    }

    state.mvp = static_cast<GLint>(state.shader->location("u_mvp"));
    state.posBL = static_cast<GLint>(state.shader->location("u_posBL"));
    state.posBR = static_cast<GLint>(state.shader->location("u_posBR"));
    state.posTL = static_cast<GLint>(state.shader->location("u_posTL"));
    state.posTR = static_cast<GLint>(state.shader->location("u_posTR"));
    state.uvBL = static_cast<GLint>(state.shader->location("u_uvBL"));
    state.uvBR = static_cast<GLint>(state.shader->location("u_uvBR"));
    state.uvTL = static_cast<GLint>(state.shader->location("u_uvTL"));
    state.uvTR = static_cast<GLint>(state.shader->location("u_uvTR"));
    state.color = static_cast<GLint>(state.shader->location("u_color"));
    state.opacityModifyRGB = static_cast<GLint>(state.shader->location("u_opacityModifyRGB"));
    state.texture = static_cast<GLint>(state.shader->location("u_spriteSheetTexture"));

    const GLfloat corners[] = {
        0.f, 0.f,
        1.f, 0.f,
        0.f, 1.f,
        1.f, 1.f,
    };
    const u16 indices[] = { 0, 2, 3, 0, 3, 1 };

    const auto saved = captureGroundGLState();

    glGenVertexArrays(1, &state.vao);
    glGenBuffers(1, &state.vertexBuffer);
    glGenBuffers(1, &state.indexBuffer);
    if (!state.vao || !state.vertexBuffer || !state.indexBuffer) {
        restoreGroundGLState(saved);
        log::error("Bismuth iOS STRICT ground GPU buffer allocation failed; stock ground fallback is disabled");
        return false;
    }

    glBindVertexArray(state.vao);
    glBindBuffer(GL_ARRAY_BUFFER, state.vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 2, nullptr);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, state.indexBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    const GLenum error = glGetError();
    restoreGroundGLState(saved);
    if (error != GL_NO_ERROR) {
        log::error("Bismuth iOS STRICT ground GPU setup failed with GL error {}; stock ground fallback is disabled", static_cast<u32>(error));
        return false;
    }

    return true;
}

GJGroundLayer* groundOwner(cocos2d::CCSprite* sprite) {
    if (!sprite)
        return nullptr;

    for (auto node = sprite->getParent(); node; node = node->getParent()) {
        auto ground = typeinfo_cast<GJGroundLayer*>(node);
        if (!ground)
            continue;

        if (sprite == ground->m_ground1Sprite ||
            sprite == ground->m_ground2Sprite ||
            sprite == ground->m_lineSprite)
            return ground;
    }
    return nullptr;
}

bool strictGroundTarget(cocos2d::CCSprite* sprite) {
    auto renderer = Renderer::get();
    return renderer && renderer->isEnabled() && groundOwner(sprite);
}

inline glm::vec3 vertexPosition(const cocos2d::ccV3F_C4B_T2F& vertex) {
    return { vertex.vertices.x, vertex.vertices.y, vertex.vertices.z };
}

inline glm::vec2 vertexUV(const cocos2d::ccV3F_C4B_T2F& vertex) {
    return { vertex.texCoords.u, vertex.texCoords.v };
}

void recordGroundProof(GJGroundLayer* ground, cocos2d::CCSprite* sprite) {
    if (!ground || !sprite)
        return;

    auto& state = groundGPU();
    ++state.successfulDraws;

    bool changed = false;
    if (sprite == ground->m_ground1Sprite && !state.ground1Proven) {
        state.ground1Proven = true;
        changed = true;
    }
    if (sprite == ground->m_ground2Sprite && !state.ground2Proven) {
        state.ground2Proven = true;
        changed = true;
    }
    if (sprite == ground->m_lineSprite && !state.lineProven) {
        state.lineProven = true;
        changed = true;
    }

    if (!Mod::get()->getSettingValue<bool>("ios_gpu_debug"))
        return;

    if (!state.truthAnnounced || changed) {
        state.truthAnnounced = true;
        log::info(
            "[Bismuth GPU TRUTH] GPU PROOF=YES | FLOOR GPU={} | GROUND1={} | GROUND2={} | LINE={} | successful={} | failed={}",
            (state.ground1Proven || state.ground2Proven) ? "YES" : "NO",
            state.ground1Proven ? "YES" : "NO",
            state.ground2Proven ? "YES" : "NO",
            state.lineProven ? "YES" : "NO",
            state.successfulDraws,
            state.failedDraws
        );
    }
}

bool drawGroundOnGPU(cocos2d::CCSprite* sprite) {
    if (!strictGroundTarget(sprite))
        return false;

    auto ground = groundOwner(sprite);
    if (!ground)
        return false;

    // Ground ownership is strict while Bismuth is enabled. If GD ever moves a
    // literal ground sprite into a stock batch, that is treated as an unsupported
    // state rather than silently returning to the stock renderer.
    if (sprite->getBatchNode())
        return false;

    auto texture = sprite->getTexture();
    if (!texture || !texture->getName() || !initGroundGPU())
        return false;

    auto& state = groundGPU();
    const auto quad = sprite->getQuad();

    kmMat4 projection;
    kmMat4 modelView;
    kmMat4 mvp;
    kmGLGetMatrix(KM_GL_PROJECTION, &projection);
    kmGLGetMatrix(KM_GL_MODELVIEW, &modelView);
    kmMat4Multiply(&mvp, &projection, &modelView);

    const auto displayedColor = sprite->getDisplayedColor();
    const float opacity = static_cast<float>(sprite->getDisplayedOpacity()) / 255.f;
    const glm::vec4 rawColor = {
        static_cast<float>(displayedColor.r) / 255.f,
        static_cast<float>(displayedColor.g) / 255.f,
        static_cast<float>(displayedColor.b) / 255.f,
        opacity,
    };

    const auto saved = captureGroundGLState();

    state.shader->use();
    glUniformMatrix4fv(state.mvp, 1, GL_FALSE, mvp.mat);

    const auto pBL = vertexPosition(quad.bl);
    const auto pBR = vertexPosition(quad.br);
    const auto pTL = vertexPosition(quad.tl);
    const auto pTR = vertexPosition(quad.tr);
    glUniform3f(state.posBL, pBL.x, pBL.y, pBL.z);
    glUniform3f(state.posBR, pBR.x, pBR.y, pBR.z);
    glUniform3f(state.posTL, pTL.x, pTL.y, pTL.z);
    glUniform3f(state.posTR, pTR.x, pTR.y, pTR.z);

    const auto uvBL = vertexUV(quad.bl);
    const auto uvBR = vertexUV(quad.br);
    const auto uvTL = vertexUV(quad.tl);
    const auto uvTR = vertexUV(quad.tr);
    glUniform2f(state.uvBL, uvBL.x, uvBL.y);
    glUniform2f(state.uvBR, uvBR.x, uvBR.y);
    glUniform2f(state.uvTL, uvTL.x, uvTL.y);
    glUniform2f(state.uvTR, uvTR.x, uvTR.y);

    glUniform4f(state.color, rawColor.r, rawColor.g, rawColor.b, rawColor.a);
    glUniform1f(state.opacityModifyRGB, sprite->isOpacityModifyRGB() ? 1.f : 0.f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture->getName());
    glUniform1i(state.texture, 0);

    const auto blend = sprite->getBlendFunc();
    ccGLBlendFunc(blend.src, blend.dst);

    glBindVertexArray(state.vao);
    glBindBuffer(GL_ARRAY_BUFFER, state.vertexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, state.indexBuffer);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);

    const GLenum error = glGetError();
    restoreGroundGLState(saved);
    if (error != GL_NO_ERROR) {
        ++state.failedDraws;
        log::error("Bismuth iOS STRICT ground GPU draw failed with GL error {}; stock ground fallback is disabled", static_cast<u32>(error));
        if (Mod::get()->getSettingValue<bool>("ios_gpu_debug")) {
            log::error(
                "[Bismuth GPU TRUTH] FLOOR GPU=NO | failed submit | successful={} | failed={}",
                state.successfulDraws,
                state.failedDraws
            );
        }
        return false;
    }

    recordGroundProof(ground, sprite);

    if (!state.announced) {
        state.announced = true;
        log::info("Bismuth iOS literal ground GPU assist active: ground01 + ground02 + line use persistent GPU quad expansion");
    }
    return true;
}
} // namespace

class $modify(RendererGroundOwnedCCSprite, cocos2d::CCSprite) {
    void draw() {
        if (!strictGroundTarget(this)) {
            cocos2d::CCSprite::draw();
            return;
        }

        // Deliberately no stock fallback here. A target ground sprite is owned by
        // the GPU path while Bismuth is enabled. If submission fails, the failure
        // remains visible and logged instead of being hidden by stock rendering.
        if (!drawGroundOnGPU(this)) {
            auto& state = groundGPU();
            ++state.failedDraws;
            if (!state.failureAnnounced) {
                state.failureAnnounced = true;
                log::error("Bismuth iOS STRICT ground GPU submission failed; ground stock draw intentionally suppressed");
            }
            if (Mod::get()->getSettingValue<bool>("ios_gpu_debug")) {
                log::error(
                    "[Bismuth GPU TRUTH] FLOOR GPU=NO | target recognized but GPU submit failed | successful={} | failed={}",
                    state.successfulDraws,
                    state.failedDraws
                );
            }
        }
    }
};

#endif
