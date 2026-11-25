#include "Buffer.hpp"

using namespace geode::prelude;

Buffer::~Buffer() {
    if (mappedData) {
        if (shouldMap) {
            bindAs(GL_ARRAY_BUFFER);
            glUnmapBuffer(GL_ARRAY_BUFFER);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        } else
            delete[] mappedData;
    }

    mappedData = nullptr;

    if (id != 0)
        glDeleteBuffers(1, &id);
}

void Buffer::read(void* data, usize size, usize offset) {
    if (!data || size == 0 || offset > this->size || size > this->size - offset)
        return;

    i32 previouslyBoundBuffer;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previouslyBoundBuffer);

    glBindBuffer(GL_ARRAY_BUFFER, id);
    glGetBufferSubData(GL_ARRAY_BUFFER, offset, size, data);

    glBindBuffer(GL_ARRAY_BUFFER, previouslyBoundBuffer);
}

void Buffer::write(void* data, usize size, usize offset) {
    if (!data || size == 0 || offset > this->size || size > this->size - offset)
        return;

    /*
        While the buffer is persistently mapped, glBufferSubData is not
        allowed. Write through the mapping instead and let the caller flush.

        The previous binding is not queried anymore: an ARRAY_BUFFER binding
        is not part of vertex array object state, and every cocos draw binds
        the buffers it needs, so leaving this at 0 skips a synchronous
        glGetIntegerv on every upload.
    */
    if (mappedData && shouldMap) {
        std::memcpy(mappedData + offset, data, size);
        glBindBuffer(GL_ARRAY_BUFFER, id);
        glFlushMappedBufferRange(GL_ARRAY_BUFFER, offset, size);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return;
    }

    glBindBuffer(GL_ARRAY_BUFFER, id);
    glBufferSubData(GL_ARRAY_BUFFER, offset, size, data);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void* Buffer::map(i32 type) {
    if (mappedData) return nullptr;

    if (shouldMap) {
        bindAs(GL_ARRAY_BUFFER);
        mappedData = (u8*)glMapBuffer(GL_ARRAY_BUFFER, type);

        if (!mappedData)
            log::error("Failed to map buffer '{}' ({})", name, byteSizeToString(size));
    } else {
        mappedData = new u8[size];
    }
    return mappedData;
}

void Buffer::unmap() {
    if (!mappedData) return;

    if (shouldMap) {
        bindAs(GL_ARRAY_BUFFER);
        glUnmapBuffer(GL_ARRAY_BUFFER);
    } else {
        delete[] mappedData;
    }
    mappedData = nullptr;
}

void Buffer::flush(usize offset, usize size) {
    if (!mappedData || offset > this->size || size > this->size - offset) return;

    if (shouldMap) {
        bindAs(GL_ARRAY_BUFFER);
        glFlushMappedBufferRange(GL_ARRAY_BUFFER, offset, size);
    } else {
        write(mappedData + offset, size, offset);
    }
}

UPtr<Buffer> Buffer::create(const char* name, usize size, GLenum usage) {
    u32 buffer = 0;
    glGenBuffers(1, &buffer);

    if (buffer == 0) {
        log::error("Failed to create buffer '{}'", name);
        return nullptr;
    }

    glObjectLabel(GL_BUFFER, buffer, strlen(name), name);

    i32 previouslyBoundBuffer;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previouslyBoundBuffer);

    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, size, NULL, usage);

    glBindBuffer(GL_ARRAY_BUFFER, previouslyBoundBuffer);

    auto ret = std::make_unique<Buffer>();
    ret->name = name;
    ret->id = buffer;
    ret->size = size;
    ret->shouldMap = Mod::get()->getSettingValue<bool>("use_mapped_buffers");

    return ret;
}