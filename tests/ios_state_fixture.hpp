#pragma once
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
using usize=std::size_t;
using u8=std::uint8_t;
using u32=std::uint32_t;
namespace glm { struct vec2 {float x,y;}; struct vec4 {float x,y,z,w;}; }
namespace cocos2d {
struct CCPoint {float x=0,y=0;};
struct CCSize {float width=0,height=0;};
struct CCRect {CCPoint origin; CCSize size;};
struct ccColor3B {u8 r=255,g=255,b=255;};
struct CCAffineTransform {float a=1,b=0,c=0,d=1,tx=0,ty=0;};
inline CCAffineTransform CCAffineTransformMakeIdentity(){return {};}
struct CCArray { usize size=0; usize count(){return size;} };
struct CCTexture2D {
    u32 id=4; float width=1024,height=1024;
    u32 getName(){return id;} float getPixelsWide(){return width;} float getPixelsHigh(){return height;}
};
struct CCSprite {
    ccColor3B color; u8 opacity=255;
    bool visible=true, rotated=false, flipX=false, flipY=false, premultiplied=false;
    CCRect rect{{0,0},{30,30}}; CCPoint offset;
    CCTexture2D tex;
    ccColor3B getDisplayedColor(){return color;} u8 getDisplayedOpacity(){return opacity;}
    CCRect getTextureRect(){return rect;} CCPoint getOffsetPosition(){return offset;}
    bool isVisible(){return visible;} bool isTextureRectRotated(){return rotated;}
    bool isFlipX(){return flipX;} bool isFlipY(){return flipY;} bool isOpacityModifyRGB(){return premultiplied;}
    CCTexture2D* getTexture(){return &tex;}
};
}
struct GameObject : cocos2d::CCSprite {
    cocos2d::CCArray children;
    cocos2d::CCSprite *m_glowSprite=nullptr,*m_colorSprite=nullptr;
    bool m_isInvisible=false;
    cocos2d::CCAffineTransform transform;
    float vertexZ=0;
    GameObject* parent=nullptr;
    cocos2d::CCArray* getChildren(){return &children;}
    GameObject* getParent(){return parent;}
    cocos2d::CCAffineTransform nodeToParentTransform(){return transform;}
    float getVertexZ(){return vertexZ;}
};
struct PlayLayer {};
struct DataTexture {};
namespace geode { namespace prelude {} }