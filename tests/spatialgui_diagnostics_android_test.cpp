#define __ANDROID__ 1
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <cassert>
#include <cstring>
namespace bedrocktools::events { EventBus& bus() { static EventBus b; return b; } }
namespace bedrocktools::memory { std::uintptr_t resolve(SignatureId id) { return id==SignatureId::ScreenViewRender ? 0x1000 : 0; } }
#include "modules/spatialgui/diagnostics.cpp"
namespace { int samplesLogged=0, hooksLogged=0, eventsLogged=0, framesLogged=0, originalCalls=0; bool hasContext=true; }
EGLContext eglGetCurrentContext() {return hasContext ? reinterpret_cast<void*>(1) : EGL_NO_CONTEXT;}
void glGetIntegerv(GLenum name, GLint* out) {if (name==GL_VIEWPORT) {out[0]=0;out[1]=0;out[2]=800;out[3]=600;} else *out=2;}
int __android_log_print(int,const char* tag,const char* fmt,...) {
    assert(std::strcmp(tag,"BTP-SpatialDiag")==0);
    if (std::strncmp(fmt,"screen render",13)==0) ++samplesLogged;
    if (std::strncmp(fmt,"screen hook called",18)==0) ++hooksLogged;
    if (std::strncmp(fmt,"container event",15)==0) ++eventsLogged;
    if (std::strncmp(fmt,"egl swap frame",14)==0) ++framesLogged;
    return 0;
}
void fakeOriginal(void*,void*) {++originalCalls;}
int main() {
    SpatialGuiDiagnostics mod;mod.onInit();mod.setMasterEnabled(true);original=fakeOriginal;
    int controller=0;
    bedrocktools::events::ScreenStateEvent event{bedrocktools::events::ScreenKind::Container,bedrocktools::events::ScreenPhase::Opened,&controller};
    bedrocktools::events::FrameEvent frame;
    for (int i=0;i<4;++i) bedrocktools::events::bus().publish(frame);
    assert(framesLogged==2);
    for (int i=0;i<6;++i) renderHook(nullptr,nullptr);
    assert(originalCalls==6 && hooksLogged==4 && eventsLogged==0 && samplesLogged==0);
    bedrocktools::events::bus().publish(event);
    for (int i=0;i<4;++i) bedrocktools::events::bus().publish(frame);
    assert(framesLogged==4);
    hasContext=false; renderHook(nullptr,nullptr);
    assert(originalCalls==7 && hooksLogged==5 && eventsLogged==1 && samplesLogged==0);
    hasContext=true;
    for (int i=0;i<12;++i) renderHook(nullptr,nullptr);
    assert(originalCalls==19 && hooksLogged==8 && eventsLogged==1 && samplesLogged==8);
    event.phase=bedrocktools::events::ScreenPhase::Closed;bedrocktools::events::bus().publish(event);
    renderHook(nullptr,nullptr);assert(originalCalls==20 && samplesLogged==8 && eventsLogged==2);
    mod.setMasterEnabled(false);bedrocktools::events::bus().publish(frame);
    renderHook(nullptr,nullptr);assert(originalCalls==21 && samplesLogged==8 && hooksLogged==8 && framesLogged==4);
}
