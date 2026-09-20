#!/usr/bin/env python3
"""Exercise production data-texture transfers and dirty retry with a fake GL driver."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
def source(path):
    return '\n'.join(line for line in (root/path).read_text().splitlines() if not line.startswith(('#include', '#pragma once')))
header = source('src/renderer/ios/DataTexture.hpp').replace('private:', 'public:')
cpp = source('src/renderer/ios/DataTexture.cpp')
state = source('src/renderer/ios/ResolvedStateLayer.cpp')
code = r'''
#define GEODE_IS_IOS
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>
#include "../src/renderer/ios/DirtyRanges.hpp"
using usize=std::size_t; using u8=std::uint8_t; using u32=std::uint32_t; using i32=int;
using GLenum=unsigned; using GLint=int; using GLuint=unsigned; using GLsizei=int;
constexpr int GL_NO_ERROR=0, GL_TEXTURE_BINDING_2D=1, GL_TEXTURE_2D=2, GL_RGBA=3;
constexpr int GL_FLOAT=4, GL_UNSIGNED_BYTE=5, GL_TEXTURE0=6;
namespace glm { struct vec2 {float x,y;}; struct vec4 {float x=0,y=0,z=0,w=0;}; }
namespace geode { namespace prelude {} }
int bound=99, queries=0, transfers=0, error=0; bool fail=false;
std::vector<glm::vec4> gpu(32768);
void glGetIntegerv(GLenum, GLint* value) {++queries; *value=bound;}
void glBindTexture(GLenum, GLuint value) {bound=value;}
void glActiveTexture(GLenum) {}
int glGetError() {int result=error;error=0;return result;}
void glDeleteTextures(int,const GLuint*) {}
void glTexSubImage2D(GLenum,int,GLint x,GLint y,GLsizei w,GLsizei h,GLenum,GLenum,const void* data) {
    assert(bound==7); assert(x>=0 && x+w<=1024 && y>=0 && y+h<=32);
    ++transfers;
    if(fail) {error=1; return;}
    for(int row=0;row<h;++row)
        std::memcpy(gpu.data()+(y+row)*1024+x, static_cast<const glm::vec4*>(data)+row*w, w*sizeof(glm::vec4));
}
struct ResolvedStateLayer { struct Stats {usize bytesUploaded=0,uploadCalls=0;}; };
'''
code += header + '\nDataTexture::~DataTexture() {}\n'
code += cpp[cpp.index('bool DataTexture::upload('):cpp.rindex('#endif')]
code += state[state.index('bool uploadDirtyRecordSpans('):state.index('} // namespace')]
code += r'''
int main() {
    DataTexture texture; texture.id=7; texture.width=1024; texture.height=32;
    texture.capacity=gpu.size(); texture.bytesPerTexel=sizeof(glm::vec4); texture.pixelType=GL_FLOAT;
    std::vector<glm::vec4> cpu(gpu.size());
    for(usize i=0;i<cpu.size();++i) cpu[i]={float(i),2,3,4};
    error=0x502; // unrelated stale driver error must not poison this upload
    assert(texture.uploadRange(cpu.data()+1021,1021,2054));
    assert(transfers==3 && queries==1 && bound==99);
    for(usize i=1021;i<3075;++i) assert(gpu[i].x==float(i));
    assert(!texture.uploadRange(cpu.data(),std::numeric_limits<usize>::max(),2));
    assert(!texture.uploadRanges(cpu.data(),cpu.size(),{{32764,8}}));
    std::vector<usize> records;
    for(usize i=0;i<4000;++i) records.push_back(i*4); // 4,000 separated dirty records.
    ResolvedStateLayer::Stats stats;
    transfers=queries=0; fail=true;
    assert(!uploadDirtyRecordSpans(&texture,cpu,records,2,stats));
    assert(records.size()==4000 && stats.bytesUploaded==0 && bound==99);
    fail=false; transfers=queries=0;
    assert(uploadDirtyRecordSpans(&texture,cpu,records,2,stats));
    assert(records.empty() && queries==1 && transfers<=32 && bound==99);
    for(usize i=0;i<4000;++i) assert(gpu[i*8].x==float(i*8));
    std::cout<<"PASS: 4,000 sparse dirty records: "<<transfers<<" transfers, one binding query; stale GL errors ignored; failed uploads retry; row boundaries and overflow checked\n";
}
'''
with tempfile.TemporaryDirectory(prefix='bismuth-upload-') as d:
    path=Path(d); (path/'test.cpp').write_text(code)
    subprocess.run(['g++','-std=c++23','-O1','-g','-Wall','-Wextra','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(root/'tests'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
