// GL state simulation for the off-screen target; does not exercise a real GPU.
#include "modules/spatialgui/offscreen_target.hpp"
#include <algorithm>
#include <cstdio>
using namespace bedrocktools::spatialgui;
namespace fakeEgl { EGLDisplay display = reinterpret_cast<void*>(1); EGLContext context = reinterpret_cast<void*>(2); }
EGLDisplay eglGetCurrentDisplay() {return fakeEgl::display;}
EGLContext eglGetCurrentContext() {return fakeEgl::context;}
namespace fake {
GLuint next=1000, draw=17, read=18, rb=19, unpack=20, active=0, textures[4]={21,22,23,24};
GLint viewport[4]={4,5,640,480};
int deletes=0; bool complete=true, unpackWasZero=false;
}
void glGetIntegerv(GLenum p, GLint* v) {
    using namespace fake;
    switch (p) {
    case GL_MAX_TEXTURE_SIZE: case GL_MAX_RENDERBUFFER_SIZE: *v=1024; break;
    case GL_DRAW_FRAMEBUFFER_BINDING: *v=draw; break; case GL_READ_FRAMEBUFFER_BINDING: *v=read; break;
    case GL_TEXTURE_BINDING_2D: *v=textures[active]; break; case GL_RENDERBUFFER_BINDING: *v=rb; break;
    case GL_PIXEL_UNPACK_BUFFER_BINDING: *v=unpack; break; case GL_ACTIVE_TEXTURE: *v=GL_TEXTURE0+active; break;
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *v=4; break;
    case GL_VIEWPORT: std::copy(viewport, viewport+4, v); break;
    }
}
void glGenFramebuffers(GLsizei, GLuint* p) {*p=fake::next++;}
void glGenTextures(GLsizei, GLuint* p) {*p=fake::next++;}
void glGenRenderbuffers(GLsizei, GLuint* p) {*p=fake::next++;}
void glBindTexture(GLenum, GLuint n) {fake::textures[fake::active]=n;}
void glTexParameteri(GLenum, GLenum, GLint) {}
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) {fake::unpackWasZero=fake::unpack==0;}
void glBindRenderbuffer(GLenum, GLuint n) {fake::rb=n;}
void glRenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei) {}
void glBindFramebuffer(GLenum type, GLuint n) {if (type != GL_READ_FRAMEBUFFER) fake::draw=n; if (type != GL_DRAW_FRAMEBUFFER) fake::read=n;}
void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint) {}
void glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint) {}
GLenum glCheckFramebufferStatus(GLenum) {return fake::complete ? GL_FRAMEBUFFER_COMPLETE : 0;}
void glDeleteFramebuffers(GLsizei, const GLuint*) {++fake::deletes;}
void glDeleteTextures(GLsizei, const GLuint*) {++fake::deletes;}
void glDeleteRenderbuffers(GLsizei, const GLuint*) {++fake::deletes;}
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {fake::viewport[0]=x; fake::viewport[1]=y; fake::viewport[2]=w; fake::viewport[3]=h;}
void glBindBuffer(GLenum, GLuint n) {fake::unpack=n;}
void glActiveTexture(GLenum n) {fake::active=n-GL_TEXTURE0;}
int main() {
    int failures=0;
    auto check=[&](bool ok,const char* label){std::printf("%s %s\n",ok?"ok":"FAIL",label); failures+=!ok;};
    OffscreenTarget target;
    fakeEgl::context = EGL_NO_CONTEXT;
    check(!target.resize(200,100), "missing EGL context rejects allocation");
    fakeEgl::context = reinterpret_cast<void*>(2);
    check(!target.resize(0, 100) && !target.resize(2000,100), "invalid dimensions rejected");
    check(target.resize(200,100), "target created");
    auto fbo=target.framebuffer(), tex=target.texture();
    fakeEgl::context = reinterpret_cast<void*>(3);
    check(!target.resize(200,100) && !target.release(), "different EGL context cannot reuse or delete target");
    { ScopedTargetBinding wrong(target); check(!wrong, "wrong EGL context cannot bind target"); }
    fakeEgl::context = reinterpret_cast<void*>(2);
    fakeEgl::display = reinterpret_cast<void*>(4);
    check(!target.release(), "different EGL display cannot delete target");
    fakeEgl::display = reinterpret_cast<void*>(1);
    check(fbo && tex && target.width()==200 && target.height()==100, "target properties");
    check(fake::draw==17 && fake::read==18 && fake::textures[0]==21 && fake::rb==19 && fake::unpack==20 && fake::unpackWasZero, "bindings restored and pixel buffer disabled during allocation");
    {
        ScopedTargetBinding binding(target);
        check(static_cast<bool>(binding) && fake::draw==fbo && fake::read==fbo && fake::viewport[2]==200 && fake::viewport[3]==100, "scope binds target and viewport");
        check(!target.release() && !target.resize(300,100), "bound target cannot be replaced or released");
    }
    check(fake::draw==17 && fake::read==18 && fake::viewport[0]==4 && fake::viewport[1]==5 && fake::viewport[2]==640 && fake::viewport[3]==480, "scope restores distinct framebuffers and viewport");
    fake::textures[2]=tex;
    check(!target.resize(300,100) && !target.release() && fake::active==0, "texture bound on another unit prevents deletion and active unit is restored");
    fake::textures[2]=23;
    fake::complete=false; int before=fake::deletes;
    check(!target.resize(300,100) && target.framebuffer()==fbo && target.texture()==tex && fake::deletes==before+3, "failed resize preserves old target and deletes temporary objects");
    fake::complete=true;
    check(target.resize(300,100) && target.framebuffer()!=fbo && target.width()==300, "successful resize replaces target");
    check(target.release() && !target.framebuffer(), "release clears resources");
    { ScopedTargetBinding empty(target); check(!empty, "empty binding is inert"); }
    check(target.resize(32,32), "recreate after release");
    before=fake::deletes; target.forgetAfterContextLoss();
    check(!target.framebuffer() && fake::deletes==before, "context loss forgets names without deleting");
    return failures?1:0;
}
