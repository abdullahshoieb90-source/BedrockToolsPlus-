#include "diagnostics.hpp"
#include "container_tracker.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <atomic>
#include <cstdint>

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#endif

namespace {
using Render = void (*)(void*, void*);
Render original = nullptr;
std::atomic<bool> g_enabled{false};
bedrocktools::hooks::Handle g_hook = nullptr;
std::atomic<unsigned> samples{0};
bedrocktools::spatialgui::ContainerTracker containers;

void observeScreen(bedrocktools::events::ScreenStateEvent& event) {
    using namespace bedrocktools::events;
    if (!g_enabled.load(std::memory_order_acquire) || event.screen != ScreenKind::Container) return;
    if (event.phase == ScreenPhase::Opened) {
        containers.opened(event.controller);
        samples.store(0, std::memory_order_release);
    } else containers.closed(event.controller);
}

// The existing Android candidate signature is unique in the supplied
// 1.26.60.04 binary; upstream LeviLamina describes ScreenView::render as
// void(ScreenView*, UIRenderContext*), but the Android ABI and function
// identity still need on-device verification. This opt-in hook never changes
// render targets, reads screen object fields, cancels input or draws overlays.
void renderHook(void* view, void* context) {
#if defined(__ANDROID__)
    const bool shouldSample = g_enabled.load(std::memory_order_acquire) && containers.hasOpen() &&
                              samples.fetch_add(1, std::memory_order_relaxed) < 8 &&
                              eglGetCurrentContext() != EGL_NO_CONTEXT;
    GLint beforeDraw = 0, afterDraw = 0, viewport[4]{};
    if (shouldSample) {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &beforeDraw);
        glGetIntegerv(GL_VIEWPORT, viewport);
    }
#endif
    if (original) original(view, context);
#if defined(__ANDROID__)
    if (shouldSample) {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &afterDraw);
        // These are generic screen-render samples while any container is open;
        // they do not prove the sampled view belongs to that container.
        __android_log_print(ANDROID_LOG_INFO, "BTP-SpatialDiag",
                            "screen render: viewport=%dx%d draw_fbo_changed=%d (view not identified)",
                            viewport[2], viewport[3], beforeDraw != afterDraw);
    }
#endif
}
}

SpatialGuiDiagnostics::SpatialGuiDiagnostics()
    : Module("Spatial GUI Diagnostics", "Read-only screen-render diagnostics; not a spatial inventory.") {
    hideInHudEditor = true;
}
SpatialGuiDiagnostics::~SpatialGuiDiagnostics() {
    onDisable();
    bedrocktools::events::bus().unsubscribe(m_screenSubscription);
}
void SpatialGuiDiagnostics::onInit() {
    using bedrocktools::memory::SignatureId;
    m_target = reinterpret_cast<void*>(bedrocktools::memory::resolve(SignatureId::ScreenViewRender));
    if (!m_screenSubscription)
        m_screenSubscription = bedrocktools::events::bus().subscribe<bedrocktools::events::ScreenStateEvent>(observeScreen);
}
void SpatialGuiDiagnostics::onEnable() {
    containers.clear();
    samples.store(0, std::memory_order_release);
    if (!g_hook && m_target)
        g_hook = bedrocktools::hooks::install(m_target, reinterpret_cast<void*>(&renderHook),
                                            reinterpret_cast<void**>(&original));
    // A hook without a callable trampoline would suppress vanilla screens.
    if (g_hook && !original) {
        bedrocktools::hooks::remove(g_hook);
        g_hook = nullptr;
    }
    m_installed = g_hook != nullptr;
    g_enabled.store(m_installed, std::memory_order_release);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "BTP-SpatialDiag", "diagnostics %s; vanilla UI unchanged",
                        m_installed ? "enabled" : "unavailable: screen-render hook not installed");
#endif
}
void SpatialGuiDiagnostics::onDisable() {
    g_enabled.store(false, std::memory_order_release);
    containers.clear();
    samples.store(0, std::memory_order_release);
}
