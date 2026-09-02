#ifdef GEODE_IS_IOS

#include "../Buffer.hpp"
#include <cstring>

Buffer::~Buffer() {
    if (id)
        glDeleteBuffers(1, &id);
}

void Buffer::read(void* data, usize readSize, usize offset) {
    assert(data != nullptr);
    assert((offset + readSize) <= size);
    std::memcpy(data, shadowData.data() + offset, readSize);
}

void Buffer::write(void* data, usize writeSize, usize offset) {
    assert(data != nullptr);
    assert((offset + writeSize) <= size);

    if (data != shadowData.data() + offset)
        std::memcpy(shadowData.data() + offset, data, writeSize);

    glBindBuffer(GL_ARRAY_BUFFER, id);
    glBufferSubData(GL_ARRAY_BUFFER, offset, writeSize, shadowData.data() + offset);
}

void Buffer::uploadShadow() {
    if (!id || shadowData.empty())
        return;
    glBindBuffer(GL_ARRAY_BUFFER, id);
    glBufferSubData(GL_ARRAY_BUFFER, 0, size, shadowData.data());
}

Buffer* Buffer::create(const char*, usize size, GLenum usage) {
    u32 buffer = 0;
    glGenBuffers(1, &buffer);
    if (!buffer)
        return nullptr;

    i32 previouslyBoundBuffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previouslyBoundBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, size, nullptr, usage);
    glBindBuffer(GL_ARRAY_BUFFER, previouslyBoundBuffer);

    auto ret = new Buffer();
    ret->id = buffer;
    ret->size = size;
    ret->shadowData.resize(size);
    return ret;
}

#endif
