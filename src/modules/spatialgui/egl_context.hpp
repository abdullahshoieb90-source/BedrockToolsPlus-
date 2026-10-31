#pragma once
#include <EGL/egl.h>

namespace bedrocktools::spatialgui {
// EGL handles are meaningful only with their display. Equality does not detect
// driver handle reuse after context destruction: the owner still must call
// forgetAfterContextLoss on lifecycle notifications, before recreating objects.
struct EglContextIdentity {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    static EglContextIdentity current() {
        return {eglGetCurrentDisplay(), eglGetCurrentContext()};
    }
    bool valid() const { return display != EGL_NO_DISPLAY && context != EGL_NO_CONTEXT; }
    bool matchesCurrent() const {
        const auto now = current();
        return valid() && now.valid() && display == now.display && context == now.context;
    }
};
}
