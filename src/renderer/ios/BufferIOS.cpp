#ifdef GEODE_IS_IOS

#include "../Buffer.hpp"
#include <cstring>

using namespace geode::prelude;

Buffer::~Buffer() {
    if (id)
        glDeleteBuffers(1, &id);
}

void Buffer::read(void* data, usize readSize, usize offset) {
    assert(data != nullptr);
    assert((offset + readSize) <= size);

    if (shadowData.empty()) {
        log::error("Attempted to read an iOS GPU buffer without a CPU shadow");
        std::memset(data, 0, readSize);
        return;
    }

    std::memcpy(data, shadowData.data() + offset, readSize);
}

void Buffer::write(void* data, usize writeSize, usize offset) {
    assert(data != nullptr);
    assert((offset + writeSize) <= size);

    const void* uploadData = data;
    if (!shadowData.empty()) {
        if (data != shadowData.data() + offset)
            std::memcpy(shadowData.data() + offset, data, writeSize);
        uploadData = shadowData.data() + offset;
    }

    GLint previouslyBoundBuffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previouslyBoundBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, id);
    glBufferSubData(GL_ARRAY_BUFFER, offset, writeSize, uploadData);
    glBindBuffer(GL_ARRAY_BUFFER, previouslyBoundBuffer);
}

void Buffer::uploadShadow() {
    if (!id || shadowData.empty())
        return;

    GLint previouslyBoundBuffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previouslyBoundBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, id);
    glBufferSubData(GL_ARRAY_BUFFER, 0, size, shadowData.data());
    glBindBuffer(GL_ARRAY_BUFFER, previouslyBoundBuffer);
}

Buffer* Buffer::create(const char* name, usize size, GLenum usage, bool keepShadow) {
    u32 buffer = 0;
    glGenBuffers(1, &buffer);
    if (!buffer)
        return nullptr;

    GLint previouslyBoundBuffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previouslyBoundBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, size, nullptr, usage);
    const GLenum allocationError = glGetError();
    glBindBuffer(GL_ARRAY_BUFFER, previouslyBoundBuffer);

    if (allocationError != GL_NO_ERROR) {
        log::error("Failed to allocate iOS GPU buffer '{}' ({} bytes, GL error 0x{:X})", name, size, (u32)allocationError);
        glDeleteBuffers(1, &buffer);
        return nullptr;
    }

    auto ret = new Buffer();
    ret->id = buffer;
    ret->size = size;
    if (keepShadow)
        ret->shadowData.resize(size);
    return ret;
}

#endif