#ifdef GEODE_IS_IOS

#include "DataTexture.hpp"
#include <algorithm>

using namespace geode::prelude;

DataTexture::~DataTexture() {
    if (id)
        glDeleteTextures(1, &id);
}

DataTexture* DataTexture::create(const char* name, usize texelCount, Type type) {
    auto texture = new DataTexture();
    if (!texture->init(name, texelCount, type)) {
        delete texture;
        return nullptr;
    }
    return texture;
}

bool DataTexture::init(const char* name, usize texelCount, Type type) {
    texelCount = std::max<usize>(texelCount, 1);

    GLint maxTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    if (maxTextureSize <= 0)
        return false;

    width = (i32)std::min<usize>(texelCount, std::min<usize>((usize)maxTextureSize, 1024));
    height = (i32)((texelCount + (usize)width - 1) / (usize)width);
    if (height > maxTextureSize) {
        log::error("{} data texture is too large: {} texels", name, texelCount);
        return false;
    }

    capacity = (usize)width * (usize)height;
    pixelType = type == Type::FloatRGBA ? GL_FLOAT : GL_UNSIGNED_BYTE;
    bytesPerTexel = type == Type::FloatRGBA ? sizeof(float) * 4 : sizeof(u8) * 4;

    // OpenGL's error flag is sticky, so drain unrelated errors before this
    // allocation. Also preserve the caller's texture binding because this state
    // layer is side-band to Geometry Dash's stock renderer.
    GLenum staleError = GL_NO_ERROR;
    while ((staleError = glGetError()) != GL_NO_ERROR)
        log::warn("Ignoring pre-existing GL error 0x{:X} before allocating {} data texture", (u32)staleError, name);

    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

    glGenTextures(1, &id);
    if (!id)
        return false;

    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,
        width,
        height,
        0,
        GL_RGBA,
        pixelType,
        nullptr
    );

    const GLenum error = glGetError();
    glBindTexture(GL_TEXTURE_2D, (GLuint)previousTexture);
    glActiveTexture((GLenum)previousActiveTexture);

    if (error != GL_NO_ERROR) {
        log::error(
            "Failed to allocate {} data texture ({}x{}, {} texels, GL error 0x{:X})",
            name,
            width,
            height,
            texelCount,
            (u32)error
        );
        glDeleteTextures(1, &id);
        id = 0;
        return false;
    }

    return true;
}

bool DataTexture::upload(const void* data, usize texelCount) {
    return uploadRange(data, 0, texelCount);
}

bool DataTexture::uploadRange(const void* data, usize startTexel, usize texelCount) {
    if (!id || startTexel + texelCount > capacity || (texelCount > 0 && !data))
        return false;
    if (texelCount == 0)
        return true;

    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

    glBindTexture(GL_TEXTURE_2D, id);

    const usize rowWidth = (usize)width;
    const auto* bytes = static_cast<const u8*>(data);
    usize cursor = startTexel;
    usize remaining = texelCount;
    usize byteOffset = 0;

    // A dirty range may begin/end part-way through a texture row. Keep each
    // logical range to at most three GL uploads without reallocating storage.
    const usize firstX = cursor % rowWidth;
    if (firstX != 0) {
        const usize firstCount = std::min(remaining, rowWidth - firstX);
        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            (GLint)firstX,
            (GLint)(cursor / rowWidth),
            (GLsizei)firstCount,
            1,
            GL_RGBA,
            pixelType,
            bytes
        );
        cursor += firstCount;
        remaining -= firstCount;
        byteOffset += firstCount * bytesPerTexel;
    }

    const usize fullRows = remaining / rowWidth;
    if (fullRows > 0) {
        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            (GLint)(cursor / rowWidth),
            width,
            (GLsizei)fullRows,
            GL_RGBA,
            pixelType,
            bytes + byteOffset
        );
        const usize uploaded = fullRows * rowWidth;
        cursor += uploaded;
        remaining -= uploaded;
        byteOffset += uploaded * bytesPerTexel;
    }

    if (remaining > 0) {
        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            (GLint)(cursor / rowWidth),
            (GLsizei)remaining,
            1,
            GL_RGBA,
            pixelType,
            bytes + byteOffset
        );
    }

    const GLenum error = glGetError();
    glBindTexture(GL_TEXTURE_2D, (GLuint)previousTexture);
    glActiveTexture((GLenum)previousActiveTexture);
    return error == GL_NO_ERROR;
}

void DataTexture::bind(i32 unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, id);
}

#endif
