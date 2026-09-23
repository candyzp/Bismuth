#!/usr/bin/env python3
"""Exercise production iOS Buffer allocation/write behavior against sticky GL errors."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]

def source(path):
    return '\n'.join(
        line for line in (root / path).read_text().splitlines()
        if not line.startswith(('#include', '#pragma once'))
    )

header = source('src/renderer/Buffer.hpp').replace('private:', 'public:')
cpp = source('src/renderer/ios/BufferIOS.cpp')

fixture = r'''
#define GEODE_IS_IOS
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
using usize=std::size_t; using u8=std::uint8_t; using u32=std::uint32_t;
using GLenum=unsigned; using GLint=int; using GLuint=unsigned;
constexpr GLenum GL_NO_ERROR=0, GL_ARRAY_BUFFER=1, GL_ARRAY_BUFFER_BINDING=2;
constexpr GLenum GL_STATIC_DRAW=3, GL_DYNAMIC_DRAW=4;
namespace geode { namespace prelude {
namespace log { template<class... T> void error(T&&...) {} }
}}
GLuint nextId=10, bound=99;
GLenum errorFlag=0;
bool failAllocation=false;
int allocations=0, subUploads=0, deletes=0;
std::vector<u8> gpu(256);
void glGenBuffers(int n, GLuint* out){ while(n--) *out++=nextId++; }
void glDeleteBuffers(int n,const GLuint*){ deletes+=n; }
void glGetIntegerv(GLenum key,GLint* out){ assert(key==GL_ARRAY_BUFFER_BINDING); *out=bound; }
void glBindBuffer(GLenum key,GLuint id){ assert(key==GL_ARRAY_BUFFER); bound=id; }
GLenum glGetError(){ auto e=errorFlag; errorFlag=0; return e; }
void glBufferData(GLenum key,usize bytes,const void*,GLenum){
    assert(key==GL_ARRAY_BUFFER); ++allocations; assert(bytes<=gpu.size());
    if(failAllocation) errorFlag=0x505;
}
void glBufferSubData(GLenum key,usize offset,usize bytes,const void* data){
    assert(key==GL_ARRAY_BUFFER); ++subUploads; assert(offset+bytes<=gpu.size());
    std::memcpy(gpu.data()+offset,data,bytes);
}
'''

cases = r'''
int main(){
    // A stale error predating Bismuth must be ignored.
    errorFlag=0x502;
    Buffer* a=Buffer::create("stale-safe",64,GL_STATIC_DRAW,false);
    assert(a && a->getSize()==64 && bound==99 && allocations==1);

    std::vector<u8> bytes(16);
    for(usize i=0;i<bytes.size();++i) bytes[i]=static_cast<u8>(i+1);
    a->write(bytes.data(),bytes.size(),4);
    assert(bound==99 && subUploads==1);
    for(usize i=0;i<bytes.size();++i) assert(gpu[4+i]==bytes[i]);
    Buffer::destroy(a);

    // A genuine allocation failure still fails cleanly and restores binding.
    failAllocation=true;
    Buffer* bad=Buffer::create("real-fail",32,GL_DYNAMIC_DRAW,false);
    failAllocation=false;
    assert(!bad && bound==99 && deletes>=2);

    // Shadow-backed map/unmap performs one coherent upload.
    Buffer* shadow=Buffer::create("shadow",32,GL_DYNAMIC_DRAW,true);
    assert(shadow);
    auto* mapped=static_cast<u8*>(shadow->mapWriteOnly());
    for(int i=0;i<32;++i) mapped[i]=static_cast<u8>(100+i);
    const int before=subUploads;
    shadow->unmap();
    assert(subUploads==before+1 && bound==99);
    for(int i=0;i<32;++i) assert(gpu[i]==static_cast<u8>(100+i));
    Buffer::destroy(shadow);

    std::cout<<"PASS: stale GL allocation errors ignored, real failures rejected, bindings restored, shadow upload works\n";
}
'''

code='\n'.join([fixture, header, cpp, cases])
with tempfile.TemporaryDirectory(prefix='bismuth-buffer-tests-') as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(code)
    subprocess.run([
        'g++','-std=c++23','-O1','-g','-Wall','-Wextra',
        '-fsanitize=address,undefined','-fno-omit-frame-pointer',
        str(path/'test.cpp'),'-o',str(path/'test')
    ],check=True)
    subprocess.run([str(path/'test')],check=True)
