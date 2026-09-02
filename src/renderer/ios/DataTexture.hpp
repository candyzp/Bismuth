#pragma once

#ifdef GEODE_IS_IOS

#include <common.hpp>
#include <vector>

class DataTexture {
public:
    enum class Type {
        FloatRGBA,
        ByteRGBA,
    };

    ~DataTexture();

    static DataTexture* create(const char* name, usize texelCount, Type type);
    static void destroy(DataTexture* texture) { delete texture; }

    bool upload(const void* data, usize texelCount);
    void bind(i32 unit) const;

    inline u32 getId() const { return id; }
    inline glm::vec2 getSize() const { return { (float)width, (float)height }; }
    inline usize getCapacity() const { return capacity; }

private:
    bool init(const char* name, usize texelCount, Type type);

    u32 id = 0;
    i32 width = 0;
    i32 height = 0;
    usize capacity = 0;
    usize bytesPerTexel = 0;
    GLenum pixelType = GL_UNSIGNED_BYTE;
    std::vector<u8> staging;
};

#endif
