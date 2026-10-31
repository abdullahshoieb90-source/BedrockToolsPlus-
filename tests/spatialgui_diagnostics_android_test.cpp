#define __ANDROID__ 1
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <cassert>
#include <cstring>
namespace bedrocktools::events { EventBus& bus() { static EventBus b; return b; } }
namespace bedrocktools::memory { std::uintptr_t resolve(SignatureId id) { return id==SignatureId::ScreenViewRender ? 0x1000 : 0; } }
#include "modules/spatialgui/diagnostics.cpp"
namespace { int samplesLogged=0, originalCalls=0; }
EGLContext eglGetCurrentContext() {return reinterpret_cast<void*>(1);}
void glGetIntegerv(GLenum name, GLint* out) {if (name==GL_VIEWPORT) {out[0]=0;out[1]=0;out[2]=800;out[3]=600;} else *out=2;}
int __android_log_print(int,const char* tag,const char* fmt,...) {
    assert(std::strcmp(tag,"BTP-SpatialDiag")==0);
    if (std::strncmp(fmt,"screen render",13)==0) ++samplesLogged;
    return 0;
}
void fakeOriginal(void*,void*) {++originalCalls;}
int main() {
    SpatialGuiDiagnostics mod;mod.onInit();mod.setMasterEnabled(true);original=fakeOriginal;
    int controller=0;
    bedrocktools::events::ScreenStateEvent event{bedrocktools::events::ScreenKind::Container,bedrocktools::events::ScreenPhase::Opened,&controller};
    bedrocktools::events::bus().publish(event);
    for (int i=0;i<12;++i) renderHook(nullptr,nullptr);
    assert(originalCalls==12 && samplesLogged==8);
    event.phase=bedrocktools::events::ScreenPhase::Closed;bedrocktools::events::bus().publish(event);
    renderHook(nullptr,nullptr);assert(originalCalls==13 && samplesLogged==8);
    mod.setMasterEnabled(false);renderHook(nullptr,nullptr);assert(originalCalls==14 && samplesLogged==8);
}
