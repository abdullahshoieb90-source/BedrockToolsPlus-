#pragma once

#include <GLES3/gl3.h>

// Render-thread only. The caller must use a current GLES 3 context and must
// release resources before destroying that context. Nothing in this class
// hooks Minecraft or captures a screen by itself.
namespace bedrocktools::spatialgui {
class OffscreenTarget {
public:
    OffscreenTarget() = default;
    OffscreenTarget(const OffscreenTarget&) = delete;
    OffscreenTarget& operator=(const OffscreenTarget&) = delete;
    // Explicit release is intentional: a destructor can run after context loss
    // or on another thread and accidentally delete objects in a different context.
    ~OffscreenTarget() = default;

    GLuint texture() const { return m_texture; }
    GLuint framebuffer() const { return m_framebuffer; }
    int width() const { return m_width; }
    int height() const { return m_height; }

    // Transactional resize: on failure the old target remains usable. Refuse to
    // replace a currently bound resource because deleting it would change the
    // caller's GL bindings behind its back.
    bool resize(int width, int height) {
        if (width <= 0 || height <= 0) return false;
        GLint maxTexture = 0, maxRenderbuffer = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture);
        glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRenderbuffer);
        if (width > maxTexture || height > maxTexture || width > maxRenderbuffer || height > maxRenderbuffer) return false;
        if (m_framebuffer && width == m_width && height == m_height) return true;
        GLint draw = 0, read = 0, texture = 0, renderbuffer = 0, unpackBuffer = 0;
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
        if ((m_framebuffer && (static_cast<GLuint>(draw) == m_framebuffer || static_cast<GLuint>(read) == m_framebuffer)) ||
            (m_texture && textureBoundOnAnyUnit(m_texture)) ||
            (m_depth && static_cast<GLuint>(renderbuffer) == m_depth)) return false;

        GLuint fbo = 0, color = 0, depth = 0;
        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &color);
        glGenRenderbuffers(1, &depth);
        bool complete = false;
        if (fbo && color && depth) {
            glBindTexture(GL_TEXTURE_2D, color);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindRenderbuffer(GL_RENDERBUFFER, depth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
            complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
        glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(renderbuffer));
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpackBuffer));
        if (!complete) {
            if (fbo) glDeleteFramebuffers(1, &fbo);
            if (color) glDeleteTextures(1, &color);
            if (depth) glDeleteRenderbuffers(1, &depth);
            return false;
        }
        destroyObjects();
        m_framebuffer = fbo; m_texture = color; m_depth = depth;
        m_width = width; m_height = height;
        return true;
    }

    // Must not be called while one of these objects is bound. Returns false
    // rather than silently changing game state in that case.
    bool release() {
        GLint draw = 0, read = 0, texture = 0, renderbuffer = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
        if ((m_framebuffer && (static_cast<GLuint>(draw) == m_framebuffer || static_cast<GLuint>(read) == m_framebuffer)) ||
            (m_texture && textureBoundOnAnyUnit(m_texture)) ||
            (m_depth && static_cast<GLuint>(renderbuffer) == m_depth)) return false;
        destroyObjects();
        return true;
    }

    // After context loss old names no longer refer to usable resources. Do not
    // issue GL deletes in a newly created context, where names may be reused.
    void forgetAfterContextLoss() { m_framebuffer = m_texture = m_depth = 0; m_width = m_height = 0; }

private:
    static bool textureBoundOnAnyUnit(GLuint name) {
        GLint active = 0, count = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &count);
        bool found = false;
        for (GLint i = 0; i < count; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            GLint bound = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
            if (static_cast<GLuint>(bound) == name) { found = true; break; }
        }
        glActiveTexture(static_cast<GLenum>(active));
        return found;
    }
    void destroyObjects() {
        if (m_framebuffer) glDeleteFramebuffers(1, &m_framebuffer);
        if (m_texture) glDeleteTextures(1, &m_texture);
        if (m_depth) glDeleteRenderbuffers(1, &m_depth);
        forgetAfterContextLoss();
    }
    GLuint m_framebuffer = 0, m_texture = 0, m_depth = 0;
    int m_width = 0, m_height = 0;
};

// This scope changes only framebuffer bindings and viewport; the caller must
// independently preserve any other GL or engine render state it changes.
class ScopedTargetBinding {
public:
    explicit ScopedTargetBinding(const OffscreenTarget& target) {
        if (!target.framebuffer()) return;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_draw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &m_read);
        glGetIntegerv(GL_VIEWPORT, m_viewport);
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer());
        glViewport(0, 0, target.width(), target.height());
        m_active = true;
    }
    ScopedTargetBinding(const ScopedTargetBinding&) = delete;
    ScopedTargetBinding& operator=(const ScopedTargetBinding&) = delete;
    ~ScopedTargetBinding() {
        if (!m_active) return;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(m_draw));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(m_read));
        glViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
    }
    explicit operator bool() const { return m_active; }
private:
    GLint m_draw = 0, m_read = 0, m_viewport[4]{};
    bool m_active = false;
};
} // namespace bedrocktools::spatialgui
