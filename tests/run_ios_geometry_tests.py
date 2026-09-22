#!/usr/bin/env python3
"""Compile LiveGeometry itself and check changing frames/transforms/upload failure."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
header='\n'.join(l for l in (root/'src/renderer/ios/LiveGeometry.hpp').read_text().splitlines() if not l.startswith(('#include','#pragma once')))
code=r'''
#define GEODE_IS_IOS
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
using usize=std::size_t; using u32=std::uint32_t; using GLint=int;
constexpr int GL_ARRAY_BUFFER=1,GL_ARRAY_BUFFER_BINDING=2,GL_NO_ERROR=0;
namespace glm {struct vec2{float x=0,y=0;}; vec2 operator+(vec2 a,vec2 b){return {a.x+b.x,a.y+b.y};}}
namespace cocos2d {
struct CCPoint{float x=0,y=0;}; struct CCSize{float width=30,height=30;}; struct CCRect{CCPoint origin;CCSize size;};
struct CCAffineTransform{float a=1,b=0,c=0,d=1,tx=0,ty=0;};
CCAffineTransform CCAffineTransformMakeIdentity(){return {};}
CCAffineTransform CCAffineTransformConcat(CCAffineTransform a,CCAffineTransform b){
return {a.a*b.a+a.b*b.c,a.a*b.b+a.b*b.d,a.c*b.a+a.d*b.c,a.c*b.b+a.d*b.d,a.tx*b.a+a.ty*b.c+b.tx,a.tx*b.b+a.ty*b.d+b.ty};}
CCPoint CCPointApplyAffineTransform(CCPoint p,CCAffineTransform a){return {p.x*a.a+p.y*a.c+a.tx,p.x*a.b+p.y*a.d+a.ty};}
struct CCNode {
CCNode* parent=nullptr; CCAffineTransform transform;
CCNode* getParent(){return parent;} CCAffineTransform nodeToParentTransform(){return transform;}
CCAffineTransform nodeToWorldTransform(){return parent?CCAffineTransformConcat(transform,parent->nodeToWorldTransform()):transform;}
CCAffineTransform worldToNodeTransform(){auto t=nodeToWorldTransform();float d=t.a*t.d-t.b*t.c;return {t.d/d,-t.b/d,-t.c/d,t.a/d,(t.c*t.ty-t.d*t.tx)/d,(t.b*t.tx-t.a*t.ty)/d};}
};
struct UV{float u=0,v=0;}; struct Vertex{UV texCoords;}; struct Quad{Vertex bl{{0,0}},br{{1,0}},tl{{0,1}},tr{{1,1}};};
struct Texture{};
struct CCSprite:CCNode {CCRect rect;CCPoint offset;Quad quad;Texture texture;CCNode* batchNode=nullptr;
Texture* getTexture(){return &texture;} CCNode* getBatchNode(){return batchNode;}
CCRect getTextureRect(){return rect;} CCPoint getOffsetPosition(){return offset;} Quad getQuad(){return quad;}};
}
struct GameObject:cocos2d::CCSprite{};
struct ResolvedStateLayer{struct ShadowCandidate{GameObject* object; cocos2d::CCSprite* sprite;usize objectStateIndex,spriteStateIndex;};};
int bound=99,calls=0,error=0;bool fail=false;std::vector<unsigned char> gpu(4096);
void glGetIntegerv(int,GLint* v){*v=bound;} void glBindBuffer(int,u32 id){bound=id;}
int glGetError(){int e=error;error=0;return e;}
void glBufferSubData(int,usize offset,usize bytes,const void* data){++calls;assert(bound==7);if(fail){error=1;return;}std::memcpy(gpu.data()+offset,data,bytes);}
'''+header+r'''
int main(){
GameObject root;root.transform.tx=100;
AssistVertex quad[4]{};LiveGeometry geometry;geometry.add({&root,&root,0,0},quad);
assert(geometry.refresh(0)&&geometry.flush(7));assert(calls==1&&bound==99);
auto get=[](int i){AssistVertex v;std::memcpy(&v,gpu.data()+i*sizeof(v),sizeof(v));return v;};
assert(get(3).localPosition.x==30&&get(3).localPosition.y==30);
assert(geometry.refresh(0)&&geometry.flush(7));assert(calls==1);
root.rect.size.width=60;root.offset={3,4};root.quad.bl.texCoords={0.5f,0.75f};
fail=true;assert(geometry.refresh(0)&&!geometry.flush(7));assert(bound==99);
fail=false;assert(geometry.refresh(0)&&geometry.flush(7));
assert(get(0).texCoord.x==0.5f&&get(0).texCoord.y==0.75f);
assert(get(3).localPosition.x==63&&get(3).localPosition.y==34);
root.offset.x=4;error=0x501;assert(geometry.refresh(0)&&geometry.flush(7));
assert(get(3).localPosition.x==64&&get(3).localPosition.y==34);
const int before=calls;root.transform.tx=10000;assert(geometry.refresh(0)&&geometry.flush(7));assert(calls==before);
cocos2d::CCNode batch;root.parent=&batch;root.batchNode=&batch;
cocos2d::CCSprite child;child.parent=&root;child.batchNode=&batch;child.transform={0,1,-1,0,7,8};
LiveGeometry children;children.add({&root,&child,0,1},quad);
assert(children.canUseBatch(0,&batch));
assert(children.refresh(0)&&children.flush(7));
// Batched sprites stay in sprite-local geometry. Cocos' authoritative
// m_transformToBatch is streamed separately through sprite state and applied
// by the vertex shader, so child/parent affine changes must not be baked here.
assert(get(0).localPosition.x==0&&get(0).localPosition.y==0);
assert(get(3).localPosition.x==30&&get(3).localPosition.y==30);
const int childBefore=calls;
child.transform.tx=9;
assert(children.refresh(0)&&children.flush(7));
assert(calls==childBefore);
assert(get(0).localPosition.x==0&&get(3).localPosition.x==30);
std::cout<<"PASS: live crop, UV, offset, nested batch ownership, sprite-local atlas geometry, unchanged geometry reuse, stale GL isolation, failed vertex upload retry\n";
}
'''
with tempfile.TemporaryDirectory(prefix='bismuth-geometry-') as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++23','-O1','-g','-Wall','-Wextra','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
