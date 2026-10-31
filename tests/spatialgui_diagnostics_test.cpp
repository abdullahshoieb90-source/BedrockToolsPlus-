// Host fake hook test: verifies opt-in and non-invasive behavior, not Android ABI.
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <cassert>
namespace bedrocktools::events { EventBus& bus() { static EventBus b; return b; } }
namespace bedrocktools::memory { std::uintptr_t resolve(SignatureId id) { return id==SignatureId::ScreenViewRender ? 0x1000 : 0; } }
#include "modules/spatialgui/diagnostics.cpp"
namespace { int calls=0; void* lastView=nullptr; void* lastContext=nullptr;
void fakeOriginal(void* view,void* context){++calls;lastView=view;lastContext=context;} }
int main() {
    SpatialGuiDiagnostics module;
    module.onInit();
    assert(!g_enabled.load());
    pl::memory::failTrampoline=true;
    module.setMasterEnabled(true);
    assert(!g_enabled.load() && pl::memory::unhookCalls==1);
    module.setMasterEnabled(false);
    pl::memory::failTrampoline=false;
    module.setMasterEnabled(true);
    assert(g_enabled.load());
    original=fakeOriginal;
    int controller=0, view=0, context=0;
    bedrocktools::events::ScreenStateEvent open{bedrocktools::events::ScreenKind::Container,bedrocktools::events::ScreenPhase::Opened,&controller};
    bedrocktools::events::bus().publish(open);
    assert(containers.hasOpen());
    renderHook(&view,&context);
    assert(calls==1 && lastView==&view && lastContext==&context);
    auto close=open; close.phase=bedrocktools::events::ScreenPhase::Closed;
    bedrocktools::events::bus().publish(close);
    assert(!containers.hasOpen());
    module.setMasterEnabled(false);
    assert(!g_enabled.load());
    renderHook(&view,&context); // persistent hook must still call vanilla
    assert(calls==2);
    bedrocktools::events::bus().publish(open);
    assert(!containers.hasOpen());
    return 0;
}
