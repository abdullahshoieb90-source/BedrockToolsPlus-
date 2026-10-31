// GL state simulation for the off-screen target; does not exercise a real GPU.
#include "modules/spatialgui/screen_capture.hpp"
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
namespace clearstate {
GLfloat color[4]={.2f,.3f,.4f,.5f}, depth=.6f;
GLboolean mask[4]={0,1,0,1}, depthMask=0;
bool scissor=true, cleared=false;
}
void glGetFloatv(GLenum p,GLfloat* out) { if(p==GL_COLOR_CLEAR_VALUE) std::copy(clearstate::color,clearstate::color+4,out); else *out=clearstate::depth; }
void glGetBooleanv(GLenum p,GLboolean* out) { if(p==GL_COLOR_WRITEMASK) std::copy(clearstate::mask,clearstate::mask+4,out); else *out=clearstate::depthMask; }
GLboolean glIsEnabled(GLenum) {return clearstate::scissor;}
void glEnable(GLenum) {clearstate::scissor=true;} void glDisable(GLenum) {clearstate::scissor=false;}
void glColorMask(GLboolean a, GLboolean b, GLboolean c, GLboolean d) {clearstate::mask[0]=a;clearstate::mask[1]=b;clearstate::mask[2]=c;clearstate::mask[3]=d;}
void glDepthMask(GLboolean a) {clearstate::depthMask=a;}
void glClearColor(float a,float b,float c,float d) {clearstate::color[0]=a;clearstate::color[1]=b;clearstate::color[2]=c;clearstate::color[3]=d;}
void glClearDepthf(float v) {clearstate::depth=v;}
void glClear(unsigned int bits) {clearstate::cleared=bits==(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT) && !clearstate::scissor && clearstate::mask[0] && clearstate::depthMask && clearstate::color[3]==0 && clearstate::depth==1;}
int main() {
    int errors=0, calls=0;
    auto check=[&](bool ok,const char* text){std::printf("%s %s\n",ok?"ok":"FAIL",text); errors+=!ok;};
    ScreenCapture capture;
    check(!capture.texture(), "no texture before first capture");
    bool ok=capture.capture(200,100,[&]{++calls; return fake::draw==fake::read && fake::viewport[2]==200 && clearstate::cleared && clearstate::scissor && !clearstate::mask[0] && clearstate::depth==.6f;});
    check(ok && calls==1 && capture.texture() && capture.width()==200 && capture.height()==100,"one renderer call produces capture");
    check(fake::draw==17 && fake::read==18 && fake::viewport[2]==640 && clearstate::color[3]==.5f && !clearstate::depthMask,"framebuffer, viewport and clear state restored");
    fakeEgl::context=reinterpret_cast<void*>(3);
    check(!capture.texture() && !capture.capture(200,100,[]{return true;}) && !capture.release(), "wrong context cannot expose or use capture");
    fakeEgl::context=reinterpret_cast<void*>(2);
    ok=capture.capture(200,100,[&]{++calls;return false;});
    check(!ok && !capture.texture() && calls==2,"failed render hides previous texture");
    ok=capture.capture(200,100,[&]{glBindFramebuffer(GL_DRAW_FRAMEBUFFER,44);return true;});
    check(!ok && !capture.texture() && fake::draw==17,"renderer framebuffer rebind rejected and original restored");
    ok=capture.capture(0,100,[&]{++calls;return true;});
    check(!ok && !capture.texture() && calls==2,"bad dimensions do not invoke renderer");
    bool nested=true;
    ok=capture.capture(200,100,[&]{nested=capture.capture(200,100,[]{return true;}); return true;});
    check(ok && !nested && capture.texture(),"recursive capture is rejected without interfering with outer capture");
    try { capture.capture(200,100,[]{throw 1; return true;}); } catch(int) {}
    check(!capture.texture() && fake::draw==17 && fake::read==18,"exceptions invalidate texture and restore framebuffer");
    ok=capture.capture(200,100,[&]{fakeEgl::context=reinterpret_cast<void*>(3); return true;});
    check(!ok && !capture.texture(),"context changed by callback invalidates frame");
    // Simulate EGL returning to the original context; fake GL is not a real
    // per-context implementation, so restore its fixture bindings explicitly.
    fakeEgl::context=reinterpret_cast<void*>(2); fake::draw=17; fake::read=18;
    capture.forgetAfterContextLoss(); check(!capture.texture(),"context loss invalidates frame");
    return errors?1:0;
}
