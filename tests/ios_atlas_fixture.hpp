#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <numeric>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using usize = std::size_t;
using GLuint = u32;
using GLenum = u32;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
constexpr int GL_FALSE=0, GL_TRUE=1, GL_NO_ERROR=0;
enum { GL_TEXTURE0=100, GL_TEXTURE_2D=200, GL_ARRAY_BUFFER,
    GL_ARRAY_BUFFER_BINDING, GL_ELEMENT_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER_BINDING,
    GL_VERTEX_ARRAY_BINDING, GL_CURRENT_PROGRAM, GL_ACTIVE_TEXTURE,
    GL_TEXTURE_BINDING_2D, GL_COLOR_WRITEMASK, GL_DEPTH_WRITEMASK,
    GL_STENCIL_WRITEMASK, GL_STENCIL_BACK_WRITEMASK, GL_FRONT, GL_BACK,
    GL_DYNAMIC_DRAW, GL_TRIANGLES, GL_UNSIGNED_SHORT, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA };
namespace fixture {
inline GLuint vao=99, program=77, arrayBuffer=88, nextBuffer=1000;
inline GLenum activeTexture=GL_TEXTURE0;
inline std::array<GLint, 3> textures{11,22,33};
inline std::array<GLboolean,4> colorMask{1,1,1,1};
inline GLboolean depthMask=1;
inline GLint frontMask=7, backMask=13;
inline GLenum error=0;
inline bool failUpload=false, failAtlasSync=false;
inline int uploads=0, stockDraws=0, gpuDraws=0, stockTransforms=0;
inline std::vector<int> pixels;
inline std::unordered_map<GLuint, GLuint> elements;
inline std::unordered_map<GLuint, std::vector<u16>> buffers;
inline std::unordered_map<GLuint, std::vector<int>> vaoSpriteIDs;
inline bool writes() { return colorMask[0]||colorMask[1]||colorMask[2]||colorMask[3]; }
inline void clearFrame() { pixels.clear(); stockDraws=gpuDraws=stockTransforms=0; }
}
inline void glGetIntegerv(GLenum key, GLint* out) {
    using namespace fixture;
    switch(key) {
        case GL_VERTEX_ARRAY_BINDING: *out=vao; break;
        case GL_CURRENT_PROGRAM: *out=program; break;
        case GL_ARRAY_BUFFER_BINDING: *out=arrayBuffer; break;
        case GL_ELEMENT_ARRAY_BUFFER_BINDING: *out=elements[vao]; break;
        case GL_ACTIVE_TEXTURE: *out=activeTexture; break;
        case GL_TEXTURE_BINDING_2D: *out=textures.at(activeTexture-GL_TEXTURE0); break;
        case GL_STENCIL_WRITEMASK: *out=frontMask; break;
        case GL_STENCIL_BACK_WRITEMASK: *out=backMask; break;
        default: assert(false);
    }
}
inline void glGetBooleanv(GLenum key, GLboolean* out) {
    if(key==GL_COLOR_WRITEMASK) std::copy(fixture::colorMask.begin(),fixture::colorMask.end(),out);
    else { assert(key==GL_DEPTH_WRITEMASK); *out=fixture::depthMask; }
}
inline void glColorMask(GLboolean a,GLboolean b,GLboolean c,GLboolean d) { fixture::colorMask={a,b,c,d}; }
inline void glDepthMask(GLboolean b) { fixture::depthMask=b; }
inline void glStencilMask(u32 b) { fixture::frontMask=fixture::backMask=b; }
inline void glStencilMaskSeparate(GLenum face,u32 b) { (face==GL_FRONT ? fixture::frontMask : fixture::backMask)=b; }
inline void glActiveTexture(GLenum x) { fixture::activeTexture=x; }
inline void glBindTexture(GLenum,GLuint x) { fixture::textures.at(fixture::activeTexture-GL_TEXTURE0)=x; }
inline void glBindVertexArray(GLuint x) { fixture::vao=x; }
inline void glUseProgram(GLuint x) { fixture::program=x; }
inline void glBindBuffer(GLenum target,GLuint x) {
    if(target==GL_ARRAY_BUFFER) fixture::arrayBuffer=x;
    else { assert(target==GL_ELEMENT_ARRAY_BUFFER); fixture::elements[fixture::vao]=x; }
}
inline void glGenBuffers(int n,GLuint* out) { while(n--) *out++=fixture::nextBuffer++; }
inline void glDeleteBuffers(int n,const GLuint* values) { while(n--) fixture::buffers.erase(*values++); }
inline GLenum glGetError() { auto e=fixture::error; fixture::error=0; return e; }
inline void glBufferData(GLenum target,usize bytes,const void* data,GLenum) {
    assert(target==GL_ARRAY_BUFFER);
    ++fixture::uploads;
    if(fixture::failUpload) { fixture::error=1; return; }
    auto first=static_cast<const u16*>(data);
    fixture::buffers[fixture::arrayBuffer].assign(first,first+bytes/sizeof(u16));
}
inline void glDrawElements(GLenum mode,GLsizei count,GLenum type,const void* offset) {
    assert(mode==GL_TRIANGLES && type==GL_UNSIGNED_SHORT && count%6==0);
    ++fixture::gpuDraws;
    auto first=reinterpret_cast<std::uintptr_t>(offset)/sizeof(u16);
    auto& indices=fixture::buffers.at(fixture::elements.at(fixture::vao));
    assert(first+count<=indices.size());
    auto& ids=fixture::vaoSpriteIDs.at(fixture::vao);
    for(usize i=first;i<first+count;i+=6) {
        auto base=indices[i];
        assert(base%4==0);
        assert(indices[i+1]==base+2 && indices[i+2]==base+3);
        assert(indices[i+3]==base && indices[i+4]==base+3 && indices[i+5]==base+1);
        if(fixture::writes()) fixture::pixels.push_back(ids.at(base/4));
    }
}
struct kmMat4 { float mat[16]{}; };
constexpr int KM_GL_PROJECTION=0, KM_GL_MODELVIEW=1;
inline void kmGLGetMatrix(int,kmMat4*) {}
inline void kmMat4Multiply(kmMat4*,const kmMat4*,const kmMat4*) {}
enum class GameObjectClassType { Normal, Animated };
namespace cocos2d {
struct CCNode;
struct CCArray {
    std::vector<CCNode*> nodes;
    u32 count() const { return nodes.size(); }
    CCNode* objectAtIndex(u32 i) { return nodes.at(i); }
    u32 indexOfObject(CCNode* n) { auto it=std::find(nodes.begin(),nodes.end(),n); return it==nodes.end()?UINT_MAX:it-nodes.begin(); }
};
struct CCAffineTransform { float a=1,b=0,c=0,d=1,tx=0,ty=0; };
struct CCNode {
    virtual ~CCNode()=default;
    CCArray children;
    CCNode* parent=nullptr;
    bool visible=true;
    CCAffineTransform transform;
    CCArray* getChildren() { return &children; }
    CCNode* getParent() { return parent; }
    bool isVisible() const { return visible; }
    CCAffineTransform nodeToParentTransform() { return transform; }
    virtual void draw() {}
};
struct CCTexture2D { u32 name=5; u32 getName() { return name; } };
struct CCTextureAtlas {
    std::vector<int> quads;
    bool dirty=true;
    bool isDirty() { return dirty; }
    u32 getTotalQuads() { return quads.size(); }
    void drawNumberOfQuads(u32 n,u32 first) {
        assert(first+n<=quads.size());
        if(!fixture::writes()) { if(!fixture::failAtlasSync) dirty=false; return; }
        dirty=false; ++fixture::stockDraws;
        fixture::pixels.insert(fixture::pixels.end(),quads.begin()+first,quads.begin()+first+n);
    }
    void drawQuads() { drawNumberOfQuads(quads.size(),0); }
};
struct CCSpriteBatchNode;
struct CCSprite : CCNode {
    int id=0;
    bool dirty=true;
    u32 slot=0;
    CCSpriteBatchNode* batch=nullptr;
    CCTexture2D* texture=nullptr;
    GameObjectClassType m_classType=GameObjectClassType::Normal;
    bool interactive=false, synced=false, m_isInvisibleBlock=false;
    CCSprite *m_glowSprite=nullptr, *m_colorSprite=nullptr;
    bool getHasSyncedAnimation() { return synced; }
    CCSpriteBatchNode* getBatchNode() { return batch; }
    u32 getAtlasIndex() { return slot; }
    CCTexture2D* getTexture() { return texture; }
    void setDirty(bool d) { dirty=d; }
    virtual void updateTransform();
};
struct ccBlendFunc { u32 src=GL_SRC_ALPHA, dst=GL_ONE_MINUS_SRC_ALPHA; };
struct CCSpriteBatchNode : CCNode {
    CCTextureAtlas atlas;
    CCTexture2D texture;
    CCArray descendants;
    int stockCalls=0;
    CCTextureAtlas* getTextureAtlas() { return &atlas; }
    CCTexture2D* getTexture() { return &texture; }
    CCArray* getDescendants() { return &descendants; }
    ccBlendFunc getBlendFunc() { return {}; }
    void draw() override {
        ++stockCalls;
        for(auto child:children.nodes) if(auto sprite=dynamic_cast<CCSprite*>(child)) sprite->updateTransform();
        atlas.drawQuads();
    }
};
inline void CCSprite::updateTransform() {
    ++fixture::stockTransforms;
    if(batch && dirty) { batch->atlas.quads.at(slot)=id; batch->atlas.dirty=true; dirty=false; }
    for(auto child:children.nodes) if(auto sprite=dynamic_cast<CCSprite*>(child)) sprite->updateTransform();
}
}
using GameObject=cocos2d::CCSprite;
constexpr u32 CCSpriteIndexNotInitialized=UINT_MAX;
namespace ObjectUtils { inline bool isInteractiveVisualObject(GameObject* o) { return o->interactive; } }
namespace geode {
template<class T> struct Ref { T* value; Ref(T* p):value(p){} T* data(){return value;} T* operator->(){return value;} explicit operator bool()const{return value!=nullptr;} };
namespace prelude {
using namespace cocos2d;
template<class T> T typeinfo_cast(CCNode* n) { return dynamic_cast<T>(n); }
template<class T> struct CCArrayExt {
    std::vector<T> nodes;
    CCArrayExt(CCArray* a) { if(a) for(auto n:a->nodes) nodes.push_back(static_cast<T>(n)); }
    auto begin(){return nodes.begin();} auto end(){return nodes.end();}
};
namespace log { template<class... T> void error(T&&...) {} }
inline void ccGLBlendFunc(u32,u32) {}
}}
struct PlayLayer { cocos2d::CCArray* m_batchNodes=nullptr; };
struct DataTexture {
    void bind(int unit) { glActiveTexture(GL_TEXTURE0+unit); glBindTexture(GL_TEXTURE_2D,600+unit); }
    std::array<float,2> getSize() { return {1024,1}; }
};
struct ResolvedStateLayer {
    bool ready=true;
    DataTexture objectTexture,spriteTexture;
    bool isGPUStateReady() { return ready; }
    DataTexture* getObjectStateTexture() { return &objectTexture; }
    DataTexture* getSpriteStateTexture() { return &spriteTexture; }
};
struct Shader {
    void use(){glUseProgram(500);}
    void setMatrix4(const char*,const float*){}
    void setInt(const char*,int){}
    void setVec2(const char*,std::array<float,2>){}
    void setTexture(const char*,int unit,u32 id){glActiveTexture(GL_TEXTURE0+unit);glBindTexture(GL_TEXTURE_2D,id);}
};
struct Buffer {};
struct Renderer {
    inline static Renderer* current=nullptr;
    bool enabled=true;
    PlayLayer layer;
    std::unordered_set<cocos2d::CCSprite*> owned;
    static geode::Ref<Renderer> get(){return current;}
    bool isEnabled(){return enabled;}
    PlayLayer* getPlayLayer(){return &layer;}
    bool isGPUOwnedSprite(cocos2d::CCSprite* s) const {return enabled && owned.contains(s);}
    bool prepareGPUOwnedSprite(cocos2d::CCSprite* s);
    bool isGPUInterleavedBatch(cocos2d::CCSpriteBatchNode*) const;
    bool drawGPUInterleavedBatch(cocos2d::CCSpriteBatchNode*);
};
struct BatchStats { bool ready=true; usize drawCallsLastFrame=0,indicesLastFrame=0; };
struct AssistShadowBatch : cocos2d::CCNode {
    BatchStats stats;
    cocos2d::CCSpriteBatchNode* stockBatch=nullptr;
    std::vector<cocos2d::CCSprite*> ownedSprites;
    ResolvedStateLayer* resolvedState=nullptr;
    Shader* shader=nullptr;
    u32 vao=0;
    Buffer* indexBuffer=nullptr;
};
struct StandaloneAssistBatch : cocos2d::CCNode {
    BatchStats stats;
    bool rootAddressable=false;
    std::vector<cocos2d::CCSprite*> ownedSprites;
    ResolvedStateLayer* resolvedState=nullptr;
    Shader* shader=nullptr;
    u32 vao=0;
    Buffer* indexBuffer=nullptr;
};
#define CC_NODE_DRAW_SETUP() do {} while(0)
#define $modify(Name, Base) Name : public Base
