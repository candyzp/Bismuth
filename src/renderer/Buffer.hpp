#pragma once

#include "Geode/cocos/platform/win32/CCGL.h"
#include <common.hpp>
#include <vector>

#ifndef GEODE_IS_IOS
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#endif

class Buffer {
public:
    ~Buffer();

    inline usize getSize() const { return size; }
    inline u32 getId() const { return id; }

    void read(void* data, usize size, usize offset = 0);
    void write(void* data, usize size, usize offset = 0);

    inline void bindAs(GLenum binding) {
        glBindBuffer(binding, id);
    }

    inline void bindAsUniformBuffer(u32 binding) {
#ifdef GEODE_IS_IOS
        (void)binding;
#else
        glBindBufferBase(GL_UNIFORM_BUFFER, binding, id);
#endif
    }

    inline void bindAsStorageBuffer(u32 binding) {
#ifdef GEODE_IS_IOS
        (void)binding;
#else
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, id);
#endif
    }

    inline void* mapWriteOnly() {
#ifdef GEODE_IS_IOS
        mapped = true;
        return shadowData.data();
#else
        bindAs(GL_ARRAY_BUFFER);
        return glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
#endif
    }

    inline void* mapReadWrite() {
#ifdef GEODE_IS_IOS
        mapped = true;
        return shadowData.data();
#else
        bindAs(GL_ARRAY_BUFFER);
        return glMapBuffer(GL_ARRAY_BUFFER, GL_READ_WRITE);
#endif
    }

    inline void unmap() {
#ifdef GEODE_IS_IOS
        if (mapped) {
            uploadShadow();
            mapped = false;
        }
#else
        bindAs(GL_ARRAY_BUFFER);
        glUnmapBuffer(GL_ARRAY_BUFFER);
#endif
    }

public:
    static Buffer* create(const char* name, usize size, GLenum usage);

    inline static void destroy(Buffer* buffer) {
        delete buffer;
    }

    inline static Buffer* createStaticDraw(const char* name, void* data, usize size) {
        auto ret = create(name, size, GL_STATIC_DRAW);
        ret->write(data, size);
        return ret;
    }

    inline static Buffer* createDynamicDraw(const char* name, usize size) {
        return create(name, size, GL_DYNAMIC_DRAW);
    }

    inline static Buffer* createDynamicCopy(const char* name, usize size) {
#ifdef GEODE_IS_IOS
        return create(name, size, GL_DYNAMIC_DRAW);
#else
        return create(name, size, GL_DYNAMIC_COPY);
#endif
    }

#ifdef GEODE_IS_IOS
    void uploadShadow();
#endif

private:
    usize size = 0;
    u32 id = 0;

#ifdef GEODE_IS_IOS
    std::vector<u8> shadowData;
    bool mapped = false;
#endif
};
