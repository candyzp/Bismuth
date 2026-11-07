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
struct CCNode;
struct CCArray {
    usize size=0;
    std::vector<CCNode*> nodes;
    usize count(){ return nodes.empty() ? size : nodes.size(); }
};
struct CCTexture2D {
    u32 id=4; float width=1024,height=1024;
    u32 getName(){return id;} float getPixelsWide(){return width;} float getPixelsHigh(){return height;}
};
struct CCNode {
    virtual ~CCNode()=default;
    bool isVisible(){return true;}
    CCNode* parent=nullptr;
    CCArray children;
    int zOrder=0;
    CCNode* getParent(){return parent;}
    CCArray* getChildren(){return &children;}
    int getZOrder(){return zOrder;}
};
struct CCSprite : CCNode {
    bool getDontDraw(){return false;}
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
enum class GameObjectType { Solid, Hazard, AnimatedHazard, Decoration };
enum class GameObjectClassType { Normal, Animated };
struct GameObject : cocos2d::CCSprite {
    virtual ~GameObject()=default;
    GameObjectType m_objectType=GameObjectType::Solid;
    GameObjectClassType m_classType=GameObjectClassType::Normal;
    bool synced=false, dontDraw=false, trigger=false, m_isHide=false, m_isInvisibleBlock=false;
    bool rotate=false, m_usesAudioScale=false;
    int m_groupCount=0;
    bool getHasSyncedAnimation(){return synced;}
    bool getDontDraw(){return dontDraw;}
    bool isTrigger(){return trigger;}
    bool getHasRotateAction(){return rotate;}
    cocos2d::CCSprite *m_glowSprite=nullptr,*m_colorSprite=nullptr;
    bool m_isInvisible=false;
    bool m_colorZLayerRelated=false;
    cocos2d::CCAffineTransform transform;
    std::vector<cocos2d::CCSprite*> unpackedSprites;
    float vertexZ=0;
    cocos2d::CCArray* getChildren(){return &children;}
    cocos2d::CCAffineTransform nodeToParentTransform(){return transform;}
    float getVertexZ(){return vertexZ;}
};
struct PlayLayer {};
struct DataTexture {
    struct Range { usize startTexel, texelCount; };
    bool fail=false; int uploads=0;
    glm::vec2 getSize(){return {1024,16};}
    bool upload(const void*,usize){++uploads;return !fail;}
    bool uploadRanges(const void*,usize,const std::vector<Range>&){++uploads;return !fail;}
};
struct CheckpointGameObject : GameObject {};
struct SpriteUnpackStats { usize nonSpriteChildren=0, duplicateSprites=0; };
struct UnpackedSprite { cocos2d::CCSprite* sprite; };
namespace ObjectUtils {
inline bool isInteractiveVisualObject(GameObject*){return false;}
template<class F> bool unpackObjectIntoSprites(GameObject* o,F callback,SpriteUnpackStats* stats) {
    if (!o)
        return false;

    std::unordered_set<cocos2d::CCSprite*> seen;
    auto emit = [&](cocos2d::CCSprite* sprite) {
        if (!sprite)
            return;
        if (!seen.insert(sprite).second) {
            if (stats) ++stats->duplicateSprites;
            return;
        }
        callback(UnpackedSprite{sprite});
    };

    auto recurse = [&](auto&& self, cocos2d::CCNode* node) -> void {
        if (!node)
            return;
        auto children = node->getChildren();
        if (children) {
            for (auto child : children->nodes) {
                if (!child || child->getZOrder() >= 0)
                    continue;
                if (!dynamic_cast<cocos2d::CCSprite*>(child) && stats)
                    ++stats->nonSpriteChildren;
                self(self, child);
            }
        }
        if (auto sprite = dynamic_cast<cocos2d::CCSprite*>(node))
            emit(sprite);
        if (children) {
            for (auto child : children->nodes) {
                if (!child || child->getZOrder() < 0)
                    continue;
                if (!dynamic_cast<cocos2d::CCSprite*>(child) && stats)
                    ++stats->nonSpriteChildren;
                self(self, child);
            }
        }
    };

    if (o->m_glowSprite)
        recurse(recurse, o->m_glowSprite);
    if (o->m_colorSprite && o->m_colorSprite->getParent() != o && !o->m_colorZLayerRelated)
        recurse(recurse, o->m_colorSprite);
    recurse(recurse, o);
    if (o->m_colorSprite && o->m_colorSprite->getParent() != o && o->m_colorZLayerRelated)
        recurse(recurse, o->m_colorSprite);
    for (auto sprite : o->unpackedSprites)
        emit(sprite);
    return true;
}
}
namespace geode { namespace prelude {
template<class T, class U> T typeinfo_cast(U* o){return dynamic_cast<T>(o);}
template<class T> struct CCArrayExt {
    explicit CCArrayExt(cocos2d::CCArray*) {}
    std::vector<T> values;
    auto begin(){return values.begin();} auto end(){return values.end();}
};
} }
