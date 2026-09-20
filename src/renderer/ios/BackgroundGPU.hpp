#pragma once
#ifdef GEODE_IS_IOS
#include "../Shader.hpp"

// One persistent unit quad. The current GD sprite supplies appearance/geometry;
// its normal visit preserves camera, parent transforms, clipping and draw order.
class BackgroundGPU {
    Shader* shader = nullptr;
    u32 vao = 0, vbo = 0;
    bool attempted = false;
    GLint projection = -1, modelView = -1, positions = -1, uvs = -1, colors = -1, sampler = -1;
    bool init();
public:
    ~BackgroundGPU();
    bool draw(cocos2d::CCSprite* sprite);
};
#endif
