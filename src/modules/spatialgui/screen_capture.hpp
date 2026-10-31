#pragma once

#include "offscreen_target.hpp"
#include <utility>

namespace bedrocktools::spatialgui {
// Captures ONE renderer invocation into an off-screen texture. This class does
// not choose a Minecraft screen, hook Bedrock or suppress vanilla UI. The
// caller must verify the engine can render to a GLES framebuffer and must
// handle its own engine render-target/state synchronization.
//
// Render thread only. A texture is published only after the callback reports a
// complete frame and leaves this framebuffer bound. No previous frame may be
// shown after failure, resize or context loss.
class ScreenCapture {
public:
    ScreenCapture() = default;
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;
    ~ScreenCapture() = default;

    template <class Render>
    bool capture(int width, int height, Render&& render) {
        m_ready = false;
        if (!m_target.resize(width, height)) return false;
        GLint draw = 0, read = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
        // This capture needs a different destination to return to; also avoids
        // recursive capture/feedback if accidentally invoked inside itself.
        if (static_cast<GLuint>(draw) == m_target.framebuffer() ||
            static_cast<GLuint>(read) == m_target.framebuffer()) return false;
        ScopedTargetBinding binding(m_target);
        if (!binding) return false;
        clearTransparent();
        // A throwing callback unwinds the binding and leaves m_ready false.
        if (!std::forward<Render>(render)()) return false;
        if (!m_target.currentContextMatches()) return false;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        if (static_cast<GLuint>(draw) != m_target.framebuffer()) return false;
        if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
        m_ready = true;
        return true;
    }
    GLuint texture() const { return m_ready && m_target.currentContextMatches() ? m_target.texture() : 0; }
    int width() const { return m_ready ? m_target.width() : 0; }
    int height() const { return m_ready ? m_target.height() : 0; }
    bool release() {
        m_ready = false;
        return m_target.release();
    }
    void forgetAfterContextLoss() { m_ready = false; m_target.forgetAfterContextLoss(); }
private:
    static void clearTransparent() {
        GLfloat color[4]{}, depth = 1;
        GLboolean mask[4]{}, depthMask = GL_TRUE;
        glGetFloatv(GL_COLOR_CLEAR_VALUE, color);
        glGetFloatv(GL_DEPTH_CLEAR_VALUE, &depth);
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(0, 0, 0, 0);
        glClearDepthf(1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glClearColor(color[0], color[1], color[2], color[3]);
        glClearDepthf(depth);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
        glDepthMask(depthMask);
        if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    }
    OffscreenTarget m_target;
    bool m_ready = false;
};
}
