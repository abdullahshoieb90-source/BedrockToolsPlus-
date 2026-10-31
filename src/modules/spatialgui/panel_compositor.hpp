#pragma once

#include "panel_projection.hpp"
#include <GLES3/gl3.h>
#include <array>
#include <cmath>

namespace bedrocktools::spatialgui {
// Clip-space coordinates in triangle-strip order: top-left, top-right,
// bottom-left, bottom-right. Supply clip x/y/z/w BEFORE perspective divide;
// retaining w lets GLES interpolate the UI texture perspective-correctly.

// Render-thread-only GLES 3 helper. It does not obtain the game's camera,
// capture its UI, or alter its input. Uses the current framebuffer/viewport.
// release() must run in the owning current GL context; after context loss use
// forgetAfterContextLoss() rather than deleting names in a new context.
class PanelCompositor {
public:
    PanelCompositor() = default;
    PanelCompositor(const PanelCompositor&) = delete;
    PanelCompositor& operator=(const PanelCompositor&) = delete;
    ~PanelCompositor() = default;

    bool initialize() {
        if (m_program && m_vao) return true;
        static constexpr const char* vs = R"(#version 300 es
precision highp float;
uniform vec4 corners[4];
out vec2 texCoord;
void main() {
    int i = gl_VertexID;
    gl_Position = corners[i];
    texCoord = vec2(float(i & 1), 1.0 - float(i >> 1));
}
)";
        static constexpr const char* fs = R"(#version 300 es
precision mediump float;
uniform sampler2D image;
in vec2 texCoord;
out vec4 outputColor;
void main() { outputColor = texture(image, texCoord); }
)";
        GLuint vertex = compile(GL_VERTEX_SHADER, vs);
        if (!vertex) return false;
        GLuint fragment = compile(GL_FRAGMENT_SHADER, fs);
        if (!fragment) { glDeleteShader(vertex); return false; }
        GLuint program = glCreateProgram();
        if (!program) { glDeleteShader(vertex); glDeleteShader(fragment); return false; }
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) { glDeleteProgram(program); return false; }
        GLint corners = glGetUniformLocation(program, "corners[0]");
        GLint image = glGetUniformLocation(program, "image");
        if (corners < 0 || image < 0) { glDeleteProgram(program); return false; }
        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        if (!vao) { glDeleteProgram(program); return false; }
        m_program = program; m_vao = vao; m_corners = corners; m_image = image;
        return true;
    }

    // Draws a transparent UI texture; expects straight (not premultiplied)
    // alpha. The texture must not be attached to the currently bound draw
    // framebuffer (GL feedback is undefined). Disable this path until the capture's alpha convention has been
    // confirmed. No depth/occlusion is applied: this is an overlay compositor.
    bool draw(GLuint texture, const ClipQuad& quad) const {
        if (!m_program || !m_vao || !texture) return false;
        for (const auto& v : quad)
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
                !std::isfinite(v.w) || v.w <= 1e-6f) return false;
        GLint program=0, vao=0, active=0, unit0Texture=0, sampler=0;
        GLint srcRgb=0, dstRgb=0, srcAlpha=0, dstAlpha=0, eqRgb=0, eqAlpha=0;
        GLboolean colorMask[4]{}, depthMask=GL_FALSE;
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &unit0Texture);
        glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &sampler);
        glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb); glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcAlpha); glGetIntegerv(GL_BLEND_DST_ALPHA, &dstAlpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &eqRgb); glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eqAlpha);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask); glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        const bool blend=glIsEnabled(GL_BLEND), depth=glIsEnabled(GL_DEPTH_TEST),
                   cull=glIsEnabled(GL_CULL_FACE), scissor=glIsEnabled(GL_SCISSOR_TEST),
                   stencil=glIsEnabled(GL_STENCIL_TEST), discard=glIsEnabled(GL_RASTERIZER_DISCARD);
        glUseProgram(m_program); glBindVertexArray(m_vao);
        glBindTexture(GL_TEXTURE_2D, texture); glBindSampler(0, 0);
        glUniform1i(m_image, 0);
        // Copy explicitly instead of assuming struct packing.
        GLfloat values[16];
        for (int i=0; i<4; ++i) {
            values[i*4]=quad[i].x; values[i*4+1]=quad[i].y;
            values[i*4+2]=quad[i].z; values[i*4+3]=quad[i].w;
        }
        glUniform4fv(m_corners, 4, values);
        glEnable(GL_BLEND); glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
        glDisable(GL_RASTERIZER_DISCARD);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        glDepthMask(depthMask);
        setEnabled(GL_BLEND, blend); setEnabled(GL_DEPTH_TEST, depth); setEnabled(GL_CULL_FACE, cull);
        setEnabled(GL_SCISSOR_TEST, scissor); setEnabled(GL_STENCIL_TEST, stencil);
        setEnabled(GL_RASTERIZER_DISCARD, discard);
        glBlendEquationSeparate(static_cast<GLenum>(eqRgb), static_cast<GLenum>(eqAlpha));
        glBlendFuncSeparate(static_cast<GLenum>(srcRgb), static_cast<GLenum>(dstRgb),
                            static_cast<GLenum>(srcAlpha), static_cast<GLenum>(dstAlpha));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(unit0Texture));
        glBindSampler(0, static_cast<GLuint>(sampler));
        glActiveTexture(static_cast<GLenum>(active));
        glBindVertexArray(static_cast<GLuint>(vao));
        glUseProgram(static_cast<GLuint>(program));
        return true;
    }

    // Refuse to delete currently bound state, which would change the caller's
    // GL bindings. The owner must unbind it and call on the render thread.
    bool release() {
        GLint current=0, vao=0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &current);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        if ((m_program && static_cast<GLuint>(current)==m_program) ||
            (m_vao && static_cast<GLuint>(vao)==m_vao)) return false;
        if (m_program) glDeleteProgram(m_program);
        if (m_vao) glDeleteVertexArrays(1, &m_vao);
        forgetAfterContextLoss();
        return true;
    }
    void forgetAfterContextLoss() { m_program=0; m_vao=0; m_corners=-1; m_image=-1; }
private:
    static GLuint compile(GLenum type, const char* source) {
        GLuint shader=glCreateShader(type);
        if (!shader) return 0;
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint status=0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
        if (!status) { glDeleteShader(shader); return 0; }
        return shader;
    }
    static void setEnabled(GLenum capability, bool enabled) {
        if (enabled) glEnable(capability); else glDisable(capability);
    }
    GLuint m_program=0, m_vao=0;
    GLint m_corners=-1, m_image=-1;
};
}
