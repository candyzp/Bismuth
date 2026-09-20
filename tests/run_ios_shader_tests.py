#!/usr/bin/env python3
"""Compile/link production GLES shaders and render colored/black backgrounds on Mesa.
Requires libEGL plus a GLES-capable Mesa driver; this is not an Apple-driver test.
"""
import ctypes as C
import os
from pathlib import Path
os.environ.setdefault('EGL_PLATFORM', 'surfaceless')
os.environ.setdefault('LIBGL_ALWAYS_SOFTWARE', '1')
egl=C.CDLL('libEGL.so.1')
def api(lib,name,result,args):
    fn=getattr(lib,name);fn.restype=result;fn.argtypes=args;return fn
ptr=C.c_void_p; integer=C.c_int; uint=C.c_uint; floating=C.c_float
getDisplay=api(egl,'eglGetDisplay',ptr,[ptr]); initialize=api(egl,'eglInitialize',uint,[ptr,ptr,ptr])
choose=api(egl,'eglChooseConfig',uint,[ptr,ptr,ptr,integer,ptr]);bind=api(egl,'eglBindAPI',uint,[uint])
createSurface=api(egl,'eglCreatePbufferSurface',ptr,[ptr,ptr,ptr]);createContext=api(egl,'eglCreateContext',ptr,[ptr,ptr,ptr,ptr])
makeCurrent=api(egl,'eglMakeCurrent',uint,[ptr,ptr,ptr,ptr]);getProc=api(egl,'eglGetProcAddress',ptr,[C.c_char_p])
def ints(*values): return (integer*len(values))(*values)
def floats(values): return (floating*len(values))(*values)
display=getDisplay(None);assert initialize(display,None,None)
config=ptr();count=integer()
assert choose(display,ints(0x3033,1,0x3040,4,0x3024,8,0x3023,8,0x3022,8,0x3021,8,0x3038),C.byref(config),1,C.byref(count)) and count.value
assert bind(0x30A0)
surface=createSurface(display,config,ints(0x3057,16,0x3056,16,0x3038))
context=createContext(display,config,None,ints(0x3098,2,0x3038));assert context
assert makeCurrent(display,surface,surface,context)
def gl(name,result,*args):
    address=getProc(name.encode());assert address,name
    return C.CFUNCTYPE(result,*args)(address)
createShader=gl('glCreateShader',uint,uint);shaderSource=gl('glShaderSource',None,uint,integer,ptr,ptr)
compileShader=gl('glCompileShader',None,uint);getShader=gl('glGetShaderiv',None,uint,uint,ptr)
shaderLog=gl('glGetShaderInfoLog',None,uint,integer,ptr,ptr)
createProgram=gl('glCreateProgram',uint);attach=gl('glAttachShader',None,uint,uint)
link=gl('glLinkProgram',None,uint);getProgram=gl('glGetProgramiv',None,uint,uint,ptr)
programLog=gl('glGetProgramInfoLog',None,uint,integer,ptr,ptr)
root=Path(__file__).resolve().parents[1]/'resources/shaders'
def program(vertex):
    p=createProgram()
    for path,kind in ((vertex,0x8B31),('assist_ios.frag',0x8B30)):
        s=createShader(kind);source=C.c_char_p((root/path).read_bytes())
        shaderSource(s,1,C.byref(source),None);compileShader(s)
        ok=integer();getShader(s,0x8B81,C.byref(ok))
        log=C.create_string_buffer(4096);shaderLog(s,4096,None,log)
        assert ok.value,(path,log.value.decode());attach(p,s)
    link(p);ok=integer();getProgram(p,0x8B82,C.byref(ok));log=C.create_string_buffer(4096);programLog(p,4096,None,log)
    assert ok.value,log.value.decode();return p
program('assist_ios.vert');p=program('background_ios.vert')
gl('glUseProgram',None,uint)(p)
loc=gl('glGetUniformLocation',integer,uint,C.c_char_p)
def uniform(name,fn,n,values):gl(fn,None,integer,integer,ptr)(loc(p,name.encode()),n,floats(values))
identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
for name in ('u_projection','u_modelView'):
    gl('glUniformMatrix4fv',None,integer,integer,uint,ptr)(loc(p,name.encode()),1,0,floats(identity))
uniform('u_positions[0]','glUniform3fv',4,[-1,-1,0,1,-1,0,-1,1,0,1,1,0])
uniform('u_uvs[0]','glUniform2fv',4,[0,0,1,0,0,1,1,1])
uniform('u_colors[0]','glUniform4fv',4,[1]*16)
vbo=uint();gl('glGenBuffers',None,integer,ptr)(1,C.byref(vbo));gl('glBindBuffer',None,uint,uint)(0x8892,vbo)
vertices=floats([0,0,1,0,0,1,1,1]);gl('glBufferData',None,uint,C.c_ssize_t,ptr,uint)(0x8892,C.sizeof(vertices),vertices,0x88E4)
attribute=gl('glGetAttribLocation',integer,uint,C.c_char_p)(p,b'a_localPosition')
gl('glVertexAttribPointer',None,uint,integer,uint,uint,integer,ptr)(attribute,2,0x1406,0,8,None)
gl('glEnableVertexAttribArray',None,uint)(attribute)
texture=uint();gl('glGenTextures',None,integer,ptr)(1,C.byref(texture));gl('glBindTexture',None,uint,uint)(0x0DE1,texture)
for key in (0x2800,0x2801):gl('glTexParameteri',None,uint,uint,integer)(0x0DE1,key,0x2600)
gl('glUniform1i',None,integer,integer)(loc(p,b'u_spriteSheetTexture'),0)
gl('glViewport',None,integer,integer,integer,integer)(0,0,16,16)
for color in ((220,70,120,255),(0,0,0,255)):
    data=(C.c_ubyte*4)(*color)
    gl('glTexImage2D',None,uint,integer,integer,integer,integer,integer,uint,uint,ptr)(0x0DE1,0,0x1908,1,1,0,0x1908,0x1401,data)
    gl('glDrawArrays',None,uint,integer,integer)(0x0005,0,4)
    pixel=(C.c_ubyte*4)();gl('glReadPixels',None,integer,integer,integer,integer,uint,uint,ptr)(8,8,1,1,0x1908,0x1401,pixel)
    assert tuple(pixel)==color,(tuple(pixel),color)
assert gl('glGetError',uint)()==0
print('PASS: assist/background GLES shaders compile and link; colored and black backgrounds render correctly on Mesa')
