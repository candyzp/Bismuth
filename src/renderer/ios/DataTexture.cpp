#ifdef GEODE_IS_IOS

#include "DataTexture.hpp"
#include <algorithm>
#include <cstring>

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
    staging.resize(capacity * bytesPerTexel);

    glGenTextures(1, &id);
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

    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        log::error("Failed to allocate {} data texture (GL error {})", name, (u32)error);
        return false;
    }

    return true;
}

bool DataTexture::upload(const void* data, usize texelCount) {
    if (!id || texelCount > capacity)
        return false;

    const usize usedBytes = texelCount * bytesPerTexel;
    if (usedBytes && data)
        std::memcpy(staging.data(), data, usedBytes);
    if (usedBytes < staging.size())
        std::memset(staging.data() + usedBytes, 0, staging.size() - usedBytes);

    glBindTexture(GL_TEXTURE_2D, id);
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        width,
        height,
        GL_RGBA,
        pixelType,
        staging.data()
    );
    return glGetError() == GL_NO_ERROR;
}

void DataTexture::bind(i32 unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, id);
}

#endif
