#pragma once
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
constexpr GLenum GL_MAX_TEXTURE_SIZE=1, GL_MAX_RENDERBUFFER_SIZE=2, GL_DRAW_FRAMEBUFFER_BINDING=3,
 GL_READ_FRAMEBUFFER_BINDING=4, GL_TEXTURE_BINDING_2D=5, GL_RENDERBUFFER_BINDING=6,
 GL_TEXTURE_2D=7, GL_TEXTURE_MIN_FILTER=8, GL_TEXTURE_MAG_FILTER=9, GL_LINEAR=10,
 GL_TEXTURE_WRAP_S=11, GL_TEXTURE_WRAP_T=12, GL_CLAMP_TO_EDGE=13, GL_RGBA8=14,
 GL_RGBA=15, GL_UNSIGNED_BYTE=16, GL_RENDERBUFFER=17, GL_DEPTH_COMPONENT16=18,
 GL_FRAMEBUFFER=19, GL_COLOR_ATTACHMENT0=20, GL_DEPTH_ATTACHMENT=21,
 GL_FRAMEBUFFER_COMPLETE=22, GL_DRAW_FRAMEBUFFER=23, GL_READ_FRAMEBUFFER=24,
 GL_VIEWPORT=25, GL_PIXEL_UNPACK_BUFFER_BINDING=26, GL_PIXEL_UNPACK_BUFFER=27,
 GL_ACTIVE_TEXTURE=28, GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS=29, GL_TEXTURE0=100;
void glGetIntegerv(GLenum, GLint*);
void glGenFramebuffers(GLsizei, GLuint*); void glGenTextures(GLsizei, GLuint*); void glGenRenderbuffers(GLsizei, GLuint*);
void glBindTexture(GLenum, GLuint); void glTexParameteri(GLenum, GLenum, GLint);
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
void glBindRenderbuffer(GLenum, GLuint); void glRenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei);
void glBindFramebuffer(GLenum, GLuint); void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint);
void glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint); GLenum glCheckFramebufferStatus(GLenum);
void glDeleteFramebuffers(GLsizei, const GLuint*); void glDeleteTextures(GLsizei, const GLuint*); void glDeleteRenderbuffers(GLsizei, const GLuint*);
void glViewport(GLint, GLint, GLsizei, GLsizei); void glBindBuffer(GLenum, GLuint); void glActiveTexture(GLenum);
using GLboolean = unsigned char;
using GLfloat = float;
constexpr GLboolean GL_TRUE=1, GL_FALSE=0;
constexpr GLenum GL_VERTEX_SHADER=201, GL_FRAGMENT_SHADER=202, GL_COMPILE_STATUS=203, GL_LINK_STATUS=204,
 GL_CURRENT_PROGRAM=205, GL_VERTEX_ARRAY_BINDING=206, GL_BLEND_SRC_RGB=207, GL_BLEND_DST_RGB=208,
 GL_BLEND_SRC_ALPHA=209, GL_BLEND_DST_ALPHA=210, GL_BLEND_EQUATION_RGB=211, GL_BLEND_EQUATION_ALPHA=212,
 GL_COLOR_WRITEMASK=213, GL_DEPTH_WRITEMASK=214, GL_BLEND=215, GL_DEPTH_TEST=216, GL_CULL_FACE=217,
 GL_SCISSOR_TEST=218, GL_STENCIL_TEST=219, GL_FUNC_ADD=220, GL_SRC_ALPHA=221,
 GL_ONE_MINUS_SRC_ALPHA=222, GL_ONE=223, GL_TRIANGLE_STRIP=224, GL_RASTERIZER_DISCARD=225,
 GL_SAMPLER_BINDING=226;
GLuint glCreateShader(GLenum); void glShaderSource(GLuint,GLsizei,const char* const*,const GLint*);
void glCompileShader(GLuint); void glGetShaderiv(GLuint,GLenum,GLint*); void glDeleteShader(GLuint);
GLuint glCreateProgram(); void glAttachShader(GLuint,GLuint); void glLinkProgram(GLuint);
void glGetProgramiv(GLuint,GLenum,GLint*); void glDeleteProgram(GLuint); GLint glGetUniformLocation(GLuint,const char*);
void glGenVertexArrays(GLsizei,GLuint*); void glDeleteVertexArrays(GLsizei,const GLuint*);
void glGetBooleanv(GLenum,GLboolean*); GLboolean glIsEnabled(GLenum); void glUseProgram(GLuint);
void glBindVertexArray(GLuint); void glUniform1i(GLint,GLint); void glUniform4fv(GLint,GLsizei,const GLfloat*);
void glEnable(GLenum); void glDisable(GLenum); void glBlendEquationSeparate(GLenum,GLenum);
void glBlendFuncSeparate(GLenum,GLenum,GLenum,GLenum); void glDepthMask(GLboolean);
void glColorMask(GLboolean,GLboolean,GLboolean,GLboolean); void glDrawArrays(GLenum,GLint,GLsizei);
void glGetIntegeri_v(GLenum,GLuint,GLint*); void glBindSampler(GLuint,GLuint);
constexpr GLenum GL_COLOR_CLEAR_VALUE=227, GL_DEPTH_CLEAR_VALUE=228;
constexpr unsigned int GL_COLOR_BUFFER_BIT=0x4000, GL_DEPTH_BUFFER_BIT=0x0100;
void glGetFloatv(GLenum,GLfloat*); void glClearColor(GLfloat,GLfloat,GLfloat,GLfloat);
void glClearDepthf(GLfloat); void glClear(unsigned int);
