#pragma once

#ifdef GEODE_IS_IOS

#include <cstdint>
#include <string>

namespace IOSUpstream {

// Owns one IOSurface-backed CVPixelBuffer exposed as both an MTLTexture and an
// OpenGL ES texture. The implementation details stay inside Objective-C++.
class IOSMetalInteropTexture {
public:
    IOSMetalInteropTexture() = default;
    ~IOSMetalInteropTexture();

    IOSMetalInteropTexture(const IOSMetalInteropTexture&) = delete;
    IOSMetalInteropTexture& operator=(const IOSMetalInteropTexture&) = delete;

    bool initialize(std::uint32_t width, std::uint32_t height, std::string& error);
    bool runSelfTest(std::string& error);
    void reset();

    bool isReady() const { return m_impl != nullptr && m_glTextureName != 0; }
    bool selfTestPassed() const { return m_selfTestPassed; }
    std::uint32_t openGLESTextureName() const { return m_glTextureName; }

private:
    void* m_impl = nullptr;
    std::uint32_t m_glTextureName = 0;
    bool m_selfTestPassed = false;
};

} // namespace IOSUpstream

#endif
