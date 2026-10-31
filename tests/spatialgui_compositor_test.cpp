// Host GL-state simulation only. A real GLES driver is still required for validation.
#include "modules/spatialgui/panel_compositor.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>
using namespace bedrocktools::spatialgui;
namespace g {
GLuint next=100, program=5, vao=6, active=2, tex[3]={7,8,9}, sampler=10;
GLint blend[6]={11,12,13,14,15,16};
GLboolean color[4]={0,1,0,1}, depthMask=1, enabled[300]{};
bool shaderOK=true, linkOK=true; int draws=0, deletes=0; GLfloat vertices[16]{};
}
GLuint glCreateShader(GLenum){return g::next++;} void glShaderSource(GLuint,GLsizei,const char* const*,const GLint*){}
void glCompileShader(GLuint){} void glGetShaderiv(GLuint,GLenum,GLint* p){*p=g::shaderOK;}
void glDeleteShader(GLuint){++g::deletes;} GLuint glCreateProgram(){return g::next++;}
void glAttachShader(GLuint,GLuint){} void glLinkProgram(GLuint){} void glGetProgramiv(GLuint,GLenum,GLint* p){*p=g::linkOK;}
void glDeleteProgram(GLuint){++g::deletes;} GLint glGetUniformLocation(GLuint,const char*){return 1;}
void glGenVertexArrays(GLsizei,GLuint* p){*p=g::next++;} void glDeleteVertexArrays(GLsizei,const GLuint*){++g::deletes;}
void glGetIntegerv(GLenum n,GLint* p){
 switch(n){case GL_CURRENT_PROGRAM:*p=g::program;break;case GL_VERTEX_ARRAY_BINDING:*p=g::vao;break;
 case GL_ACTIVE_TEXTURE:*p=GL_TEXTURE0+g::active;break;case GL_TEXTURE_BINDING_2D:*p=g::tex[g::active];break;
 case GL_BLEND_SRC_RGB:*p=g::blend[0];break;case GL_BLEND_DST_RGB:*p=g::blend[1];break;
 case GL_BLEND_SRC_ALPHA:*p=g::blend[2];break;case GL_BLEND_DST_ALPHA:*p=g::blend[3];break;
 case GL_BLEND_EQUATION_RGB:*p=g::blend[4];break;case GL_BLEND_EQUATION_ALPHA:*p=g::blend[5];break;}
}
void glGetIntegeri_v(GLenum,GLuint,GLint* p){*p=g::sampler;}
void glGetBooleanv(GLenum n,GLboolean* p){if(n==GL_COLOR_WRITEMASK)std::copy(g::color,g::color+4,p);else *p=g::depthMask;}
GLboolean glIsEnabled(GLenum n){return g::enabled[n];} void glUseProgram(GLuint n){g::program=n;}
void glBindVertexArray(GLuint n){g::vao=n;} void glActiveTexture(GLenum n){g::active=n-GL_TEXTURE0;}
void glBindTexture(GLenum,GLuint n){g::tex[g::active]=n;} void glBindSampler(GLuint,GLuint n){g::sampler=n;}
void glUniform1i(GLint,GLint){} void glUniform4fv(GLint,GLsizei,const GLfloat* v){std::copy(v,v+16,g::vertices);}
void glEnable(GLenum n){g::enabled[n]=1;} void glDisable(GLenum n){g::enabled[n]=0;}
void glBlendEquationSeparate(GLenum a,GLenum b){g::blend[4]=a;g::blend[5]=b;}
void glBlendFuncSeparate(GLenum a,GLenum b,GLenum c,GLenum d){g::blend[0]=a;g::blend[1]=b;g::blend[2]=c;g::blend[3]=d;}
void glDepthMask(GLboolean b){g::depthMask=b;} void glColorMask(GLboolean a,GLboolean b,GLboolean c,GLboolean d){g::color[0]=a;g::color[1]=b;g::color[2]=c;g::color[3]=d;}
void glDrawArrays(GLenum,GLint,GLsizei){++g::draws;}
int main(){
 int errors=0; auto check=[&](bool b,const char* s){std::printf("%s %s\n",b?"ok":"FAIL",s);errors+=!b;};
 PanelCompositor panel; ClipQuad quad{{{-1,1,0,1},{1,1,0,1},{-1,-1,0,1},{1,-1,0,2}}};
 check(!panel.draw(2,quad),"uninitialized draw rejected");
 g::shaderOK=false; check(!panel.initialize(),"shader compile failure rejected");
 g::shaderOK=true;g::linkOK=false;check(!panel.initialize(),"program link failure rejected");
 g::linkOK=true;check(panel.initialize(),"compositor initialized");
 auto bad=quad;bad[0].w=0;check(!panel.draw(2,bad) && g::draws==0,"invalid clip vertex rejected");
 g::enabled[GL_DEPTH_TEST]=1;g::enabled[GL_SCISSOR_TEST]=1;g::enabled[GL_RASTERIZER_DISCARD]=1;
 check(panel.draw(44,quad) && g::draws==1 && g::vertices[15]==2,"quad submitted with perspective w");
 check(g::program==5 && g::vao==6 && g::active==2 && g::tex[0]==7 && g::sampler==10,"program, vao, texture unit and sampler restored");
 check(g::blend[0]==11 && g::blend[5]==16 && g::color[0]==0 && g::color[1]==1 && g::depthMask==1,"blend, write masks restored");
 check(!g::enabled[GL_BLEND] && g::enabled[GL_DEPTH_TEST] && g::enabled[GL_SCISSOR_TEST] && g::enabled[GL_RASTERIZER_DISCARD],"enable states restored");
 check(panel.release(),"released when not bound");
 return errors?1:0;
}
