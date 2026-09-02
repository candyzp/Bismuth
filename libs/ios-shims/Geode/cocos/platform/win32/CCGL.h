#pragma once

// Bismuth's original renderer includes Geode's Win32 GL header directly.
// On iOS Geometry Dash runs through Cocos2d's OpenGL ES 2 context, so provide
// the small compatibility surface the shared batching code needs.
#include <OpenGLES/ES2/gl.h>
#include <OpenGLES/ES2/glext.h>

#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING GL_VERTEX_ARRAY_BINDING_OES
#endif

#ifndef GL_DOUBLE
#define GL_DOUBLE 0x140A
#endif

#define glGenVertexArrays glGenVertexArraysOES
#define glBindVertexArray glBindVertexArrayOES
#define glDeleteVertexArrays glDeleteVertexArraysOES

// These desktop entry points are referenced by generic attribute setup code,
// but iOS changes the affected attributes to floats, so these fallbacks are
// only here to keep the shared source portable.
static inline void bismuth_glVertexAttribIPointer(
    GLuint index, GLint size, GLenum type, GLsizei stride, const GLvoid* pointer
) {
    glVertexAttribPointer(index, size, type, GL_FALSE, stride, pointer);
}
#define glVertexAttribIPointer bismuth_glVertexAttribIPointer

static inline void bismuth_glVertexAttribLPointer(
    GLuint index, GLint size, GLenum, GLsizei stride, const GLvoid* pointer
) {
    glVertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, stride, pointer);
}
#define glVertexAttribLPointer bismuth_glVertexAttribLPointer
