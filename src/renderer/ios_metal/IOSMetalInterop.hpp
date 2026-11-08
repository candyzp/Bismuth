#pragma once
#ifdef GEODE_IS_IOS
#include <cstdint>
#include <string>
namespace IOSUpstream {
struct IOSMetalCullSprite {
    float startX=0.f, startY=0.f;
    float transformColumn0X=1.f, transformColumn0Y=0.f;
    float transformColumn1X=0.f, transformColumn1Y=1.f;
    float offsetX=0.f, offsetY=0.f;
};
struct alignas(16) IOSMetalCullParameters {
    float cameraPositionX=0.f, cameraPositionY=0.f;
    float cameraRightX=1.f, cameraRightY=0.f;
    float cameraUpX=0.f, cameraUpY=1.f;
    float cameraWidth=0.f, cameraHeight=0.f;
    std::uint32_t spriteCount=0;
    std::uint32_t padding[3] {};
};
static_assert(sizeof(IOSMetalCullSprite)==32);
static_assert(sizeof(IOSMetalCullParameters)==48);
class IOSMetalInteropTexture {
public:
    IOSMetalInteropTexture()=default;
    ~IOSMetalInteropTexture();
    IOSMetalInteropTexture(const IOSMetalInteropTexture&)=delete;
    IOSMetalInteropTexture& operator=(const IOSMetalInteropTexture&)=delete;
    bool initialize(std::uint32_t width,std::uint32_t height,std::string& error);
    bool dispatchCulling(const IOSMetalCullSprite* sprites,std::uint32_t spriteCount,
                         const IOSMetalCullParameters& parameters,std::string& error);
    bool runSelfTest(std::string& error);
    void reset();
    bool isReady() const { return m_impl!=nullptr && m_glTextureName!=0; }
    bool selfTestPassed() const { return m_selfTestPassed; }
    std::uint32_t openGLESTextureName() const { return m_glTextureName; }
    std::uint32_t textureWidth() const { return m_width; }
    std::uint32_t textureHeight() const { return m_height; }
private:
    void* m_impl=nullptr;
    std::uint32_t m_glTextureName=0, m_width=0, m_height=0;
    bool m_selfTestPassed=false;
};
} // namespace IOSUpstream
#endif
