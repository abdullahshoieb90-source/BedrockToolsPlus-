#pragma once
using GLint=int; using GLenum=unsigned;
constexpr GLenum GL_DRAW_FRAMEBUFFER_BINDING=1, GL_VIEWPORT=2;
void glGetIntegerv(GLenum, GLint*);
