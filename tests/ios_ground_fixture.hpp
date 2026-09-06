#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>
using usize=std::size_t; using u32=std::uint32_t; using u16=std::uint16_t;
using GLuint=u32; using GLenum=u32; using GLint=int; using GLsizei=int;
using GLfloat=float; using GLboolean=unsigned char;
constexpr int GL_FALSE=0,GL_TRUE=1,GL_NO_ERROR=0;
enum { GL_VERTEX_ARRAY_BINDING=100,GL_ARRAY_BUFFER_BINDING,GL_ELEMENT_ARRAY_BUFFER_BINDING,
    GL_CURRENT_PROGRAM,GL_ACTIVE_TEXTURE,GL_TEXTURE_BINDING_2D,GL_BLEND,GL_BLEND_SRC_RGB,
    GL_BLEND_DST_RGB,GL_BLEND_SRC_ALPHA,GL_BLEND_DST_ALPHA,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,
    GL_TEXTURE0,GL_TEXTURE_2D,GL_ARRAY_BUFFER,GL_ELEMENT_ARRAY_BUFFER,GL_STATIC_DRAW,GL_FLOAT,
    GL_TRIANGLES,GL_UNSIGNED_SHORT };
namespace fixture {
inline GLuint vao=9,arrayBuffer=8,program=7,next=100;
inline GLenum activeTexture=GL_TEXTURE0,error=GL_NO_ERROR;
inline std::map<GLuint,GLuint> elements{{9,6}};
inline GLuint texture=5;
inline std::array<GLint,4> blend{1,2,3,4};
inline bool blending=false,failSetup=false,failDraw=false;
inline int stockDraws=0,gpuDraws=0,proofs=0,failures=0;
inline std::map<std::string,GLint> locations;
inline std::map<GLint,std::array<float,4>> uniforms;
inline std::vector<u16> indices;
}
inline void glGetIntegerv(GLenum key,GLint* out) {
    using namespace fixture;
    switch(key) {
    case GL_VERTEX_ARRAY_BINDING:*out=vao;break;
    case GL_ARRAY_BUFFER_BINDING:*out=arrayBuffer;break;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING:*out=elements[vao];break;
    case GL_CURRENT_PROGRAM:*out=program;break;
    case GL_ACTIVE_TEXTURE:*out=activeTexture;break;
    case GL_TEXTURE_BINDING_2D:*out=texture;break;
    case GL_BLEND_SRC_RGB:*out=blend[0];break;
    case GL_BLEND_DST_RGB:*out=blend[1];break;
    case GL_BLEND_SRC_ALPHA:*out=blend[2];break;
    case GL_BLEND_DST_ALPHA:*out=blend[3];break;
    default:assert(false);
    }
}
inline GLboolean glIsEnabled(GLenum x){assert(x==GL_BLEND);return fixture::blending;}
inline void glEnable(GLenum){fixture::blending=true;}
inline void glDisable(GLenum){fixture::blending=false;}
inline void glActiveTexture(GLenum x){fixture::activeTexture=x;}
inline void glBindTexture(GLenum,GLuint x){fixture::texture=x;}
inline void glBindVertexArray(GLuint x){fixture::vao=x;}
inline void glBindBuffer(GLenum t,GLuint x){if(t==GL_ARRAY_BUFFER)fixture::arrayBuffer=x;else fixture::elements[fixture::vao]=x;}
inline void glUseProgram(GLuint x){fixture::program=x;}
inline void glBlendFunc(GLenum s,GLenum d){fixture::blend={int(s),int(d),int(s),int(d)};}
inline void glBlendFuncSeparate(GLenum a,GLenum b,GLenum c,GLenum d){fixture::blend={int(a),int(b),int(c),int(d)};}
inline void glGenVertexArrays(int n,GLuint* out){while(n--)*out++=fixture::next++;}
inline void glGenBuffers(int n,GLuint* out){while(n--)*out++=fixture::next++;}
inline void glBufferData(GLenum t,usize bytes,const void* p,GLenum){
    if(fixture::failSetup)fixture::error=1;
    if(t==GL_ELEMENT_ARRAY_BUFFER){auto q=static_cast<const u16*>(p);fixture::indices.assign(q,q+bytes/sizeof(u16));}
}
inline void glVertexAttribPointer(int,int,GLenum,GLboolean,usize,const void*){}
inline void glEnableVertexAttribArray(int){}
inline GLenum glGetError(){auto e=fixture::error;fixture::error=0;return e;}
inline void glUniformMatrix4fv(GLint,int,GLboolean,const float*){}
inline void glUniform1i(GLint,int){}
inline void glUniform2f(GLint loc,float a,float b){fixture::uniforms[loc]={a,b,0,0};}
inline void glUniform3f(GLint loc,float a,float b,float c){fixture::uniforms[loc]={a,b,c,0};}
inline void glUniform4f(GLint loc,float a,float b,float c,float d){fixture::uniforms[loc]={a,b,c,d};}
inline void glDrawElements(GLenum,GLsizei n,GLenum,const void*){
    assert(n==6 && fixture::blending); ++fixture::gpuDraws;
    if(fixture::failDraw)fixture::error=1;
}
struct kmMat4{float mat[16]{};}; constexpr int KM_GL_PROJECTION=0,KM_GL_MODELVIEW=1;
inline void kmGLGetMatrix(int,kmMat4*){} inline void kmMat4Multiply(kmMat4*,const kmMat4*,const kmMat4*){}
namespace glm { struct vec2{float x,y;};struct vec3{float x,y,z;}; }
namespace cocos2d {
struct CCNode;
struct CCArray {
    std::vector<CCNode*> nodes;
    u32 count() const { return nodes.size(); }
    CCNode* objectAtIndex(u32 i) { return nodes.at(i); }
};
struct CCNode {CCNode* parent=nullptr;virtual ~CCNode()=default;CCNode* getParent(){return parent;}virtual void draw(){}};
struct ccColor4B{std::uint8_t r=255,g=255,b=255,a=255;};
struct ccV3F_C4B_T2F{struct{float x=0,y=0,z=0;}vertices;ccColor4B colors;struct{float u=0,v=0;}texCoords;};
struct ccV3F_C4B_T2F_Quad{ccV3F_C4B_T2F tl,bl,tr,br;};
struct CCTexture2D{GLuint id=77;GLuint getName(){return id;}};
struct CCSpriteBatchNode : CCNode {
    CCArray descendants;
    CCTexture2D texture;
    struct Blend{GLenum src=GL_SRC_ALPHA,dst=GL_ONE_MINUS_SRC_ALPHA;};
    CCArray* getDescendants(){return &descendants;}
    CCTexture2D* getTexture(){return &texture;}
    Blend getBlendFunc(){return {};}
    void draw()override{++fixture::stockDraws;}
};
struct CCSprite : CCNode {
    bool dontDraw=false;CCTexture2D texture;CCSpriteBatchNode* batch=nullptr;
    struct Rect{struct{float width=30,height=30;}size;}rect;
    ccV3F_C4B_T2F_Quad quad;
    CCSpriteBatchNode* getBatchNode(){return batch;}
    CCTexture2D* getTexture(){return &texture;}
    ccV3F_C4B_T2F_Quad getQuad(){return quad;}
    Rect getTextureRect(){return rect;}
    bool getDontDraw(){return dontDraw;}
    struct Blend{GLenum src=GL_SRC_ALPHA,dst=GL_ONE_MINUS_SRC_ALPHA;};
    Blend getBlendFunc(){return {};}
    void draw()override{++fixture::stockDraws;}
};
}
struct GJGroundLayer:cocos2d::CCNode{cocos2d::CCSprite *m_ground1Sprite=nullptr,*m_ground2Sprite=nullptr,*m_lineSprite=nullptr;};
struct PlayLayer:cocos2d::CCNode{};
namespace geode {
template<class T>struct Ref{T* p;T* data(){return p;}T* operator->(){return p;}operator bool(){return p!=nullptr;}};
namespace prelude {
template<class T>T typeinfo_cast(cocos2d::CCNode* n){return dynamic_cast<T>(n);}
namespace log{template<class... T>void error(T&&...){}template<class... T>void info(T&&...) {}}
}}
struct Renderer{inline static Renderer* current=nullptr;bool enabled=true;PlayLayer layer;
    static geode::Ref<Renderer> get(){return {current};}bool isEnabled(){return enabled;}PlayLayer* getPlayLayer(){return &layer;}};
struct ShaderSources{std::string vertexSource,fragmentSource;};
struct Shader{static Shader* create(const ShaderSources&){static Shader s;return &s;}
    void use(){glUseProgram(55);}u32 location(const char* name){auto [i,_]=fixture::locations.emplace(name,fixture::locations.size());return i->second;}};
struct Mod{static Mod* get(){static Mod m;return &m;}template<class T>T getSettingValue(const char*){return false;}};
namespace GPUTruth {
inline void recordGroundSuccess(Renderer*,GJGroundLayer*,cocos2d::CCSprite*){++fixture::proofs;}
inline void recordGroundFailure(Renderer*,GJGroundLayer*,cocos2d::CCSprite*){++fixture::failures;}
}
#define $modify(Name,Base) Name : public Base
