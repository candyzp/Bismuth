#ifdef GEODE_IS_IOS
#include "BackgroundGPU.hpp"
#include <Geode/cocos/kazmath/include/kazmath/mat4.h>

BackgroundGPU::~BackgroundGPU() {
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (shader) Shader::destroy(shader);
}

bool BackgroundGPU::init() {
    if (attempted)
        return shader && vao && vbo;
    attempted = true;
    shader = Shader::create("background_ios.vert", "assist_ios.frag");
    if (!shader)
        return false;
    const float corners[] = {0, 0, 1, 0, 0, 1, 1, 1};
    GLint previousVAO = 0, previousVBO = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVAO);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousVBO);
    // Ignore sticky errors left by stock rendering or another mod. Only
    // errors produced by this allocation should decide whether the persistent
    // background GPU path is available.
    while (glGetError() != GL_NO_ERROR) {}

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    if (vao && vbo) {
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, nullptr);
        glEnableVertexAttribArray(0);
    }
    bool allocationOK = true;
    while (glGetError() != GL_NO_ERROR)
        allocationOK = false;
    glBindVertexArray(static_cast<u32>(previousVAO));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<u32>(previousVBO));
    if (!allocationOK || !vao || !vbo) {
        if (vao) glDeleteVertexArrays(1, &vao);
        if (vbo) glDeleteBuffers(1, &vbo);
        vao = vbo = 0;
        return false;
    }
    projection = shader->location("u_projection"); modelView = shader->location("u_modelView");
    positions = shader->location("u_positions[0]"); uvs = shader->location("u_uvs[0]");
    colors = shader->location("u_colors[0]"); sampler = shader->location("u_spriteSheetTexture");
    return true;
}

bool BackgroundGPU::draw(cocos2d::CCSprite* sprite) {
    if (!sprite || sprite->getBatchNode() || !sprite->getTexture() ||
        !sprite->getTexture()->getName() || !init())
        return false;
    const auto quad = sprite->getQuad();
    const cocos2d::ccV3F_C4B_T2F corners[] = {quad.bl, quad.br, quad.tl, quad.tr};
    float positionData[12], uvData[8], colorData[16];
    for (usize i = 0; i < 4; ++i) {
        const auto& v = corners[i];
        positionData[i*3] = v.vertices.x; positionData[i*3+1] = v.vertices.y; positionData[i*3+2] = v.vertices.z;
        uvData[i*2] = v.texCoords.u; uvData[i*2+1] = v.texCoords.v;
        colorData[i*4] = v.colors.r / 255.f; colorData[i*4+1] = v.colors.g / 255.f;
        colorData[i*4+2] = v.colors.b / 255.f; colorData[i*4+3] = v.colors.a / 255.f;
    }
    kmMat4 matrixP, matrixMV;
    kmGLGetMatrix(KM_GL_PROJECTION, &matrixP);
    kmGLGetMatrix(KM_GL_MODELVIEW, &matrixMV);
    // Raw binds are restored without changing Cocos' VAO/program/texture caches.
    GLint previousVAO = 0, previousProgram = 0, previousUnit = 0, previousTexture = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVAO);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousUnit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    // A stale error must not turn a valid strict background submission into
    // a false failure (which would suppress the stock draw for this frame).
    while (glGetError() != GL_NO_ERROR) {}

    const auto blend = sprite->getBlendFunc();
    cocos2d::ccGLBlendFunc(blend.src, blend.dst);
    shader->use();
    glUniformMatrix4fv(projection, 1, GL_FALSE, matrixP.mat);
    glUniformMatrix4fv(modelView, 1, GL_FALSE, matrixMV.mat);
    glUniform3fv(positions, 4, positionData); glUniform2fv(uvs, 4, uvData);
    glUniform4fv(colors, 4, colorData); glUniform1i(sampler, 0);
    glBindTexture(GL_TEXTURE_2D, sprite->getTexture()->getName());
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    bool drawOK = true;
    while (glGetError() != GL_NO_ERROR)
        drawOK = false;
    glBindVertexArray(static_cast<u32>(previousVAO));
    glUseProgram(static_cast<u32>(previousProgram));
    glBindTexture(GL_TEXTURE_2D, static_cast<u32>(previousTexture));
    glActiveTexture(static_cast<GLenum>(previousUnit));
    return drawOK;
}
#endif
