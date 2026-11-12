// EmuNDS - OpenGL ES 3.2 compatibility layer for the melonDS OpenGL renderer.
// SPDX-License-Identifier: GPL-3.0-or-later
#define EMUNDS_GLES_NO_WRAP
#include "emunds_gles.h"

#include <cstdint>
#include <cstring>
#include <regex>
#include <vector>

#include "gles_shaders.inc"

// ---------------------------------------------------------------------------
// DS 15-bit colour <-> GLES formats

static inline uint16_t Rev1555To5551(uint16_t v)
{
    uint16_t r = v & 0x1F, g = (v >> 5) & 0x1F, b = (v >> 10) & 0x1F, a = v >> 15;
    return (uint16_t)((r << 11) | (g << 6) | (b << 1) | a);
}

static thread_local std::vector<uint16_t> tConv;

static const void* Convert(GLenum& type, const void* pixels, size_t count)
{
    if (type != GL_UNSIGNED_SHORT_1_5_5_5_REV) return pixels;
    type = GL_UNSIGNED_SHORT_5_5_5_1;
    if (!pixels) return nullptr;
    tConv.resize(count);
    const uint16_t* src = static_cast<const uint16_t*>(pixels);
    uint16_t* dst = tConv.data();
    for (size_t i = 0; i < count; i++) dst[i] = Rev1555To5551(src[i]);
    return dst;
}

extern "C" {

void emunds_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                         GLint border, GLenum format, GLenum type, const void* pixels)
{
    const void* p = Convert(type, pixels, (size_t)width * height);
    glTexImage2D(target, level, internalformat, width, height, border, format, type, p);
}

void emunds_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                            GLenum format, GLenum type, const void* pixels)
{
    const void* p = Convert(type, pixels, (size_t)width * height);
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, p);
}

void emunds_glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth,
                         GLint border, GLenum format, GLenum type, const void* pixels)
{
    const void* p = Convert(type, pixels, (size_t)width * height * depth);
    glTexImage3D(target, level, internalformat, width, height, depth, border, format, type, p);
}

void emunds_glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                            GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels)
{
    const void* p = Convert(type, pixels, (size_t)width * height * depth);
    glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, p);
}

void emunds_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels)
{
    if (type != GL_UNSIGNED_SHORT_1_5_5_5_REV)
    {
        glReadPixels(x, y, width, height, format, type, pixels);
        return;
    }
    // GLES only guarantees RGBA8 readback: read that and pack to DS 15-bit
    static thread_local std::vector<uint8_t> tmp;
    size_t n = (size_t)width * height;
    tmp.resize(n * 4);
    GLint oldAlign = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &oldAlign);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
    glPixelStorei(GL_PACK_ALIGNMENT, oldAlign);
    uint16_t* dst = static_cast<uint16_t*>(pixels);
    const uint8_t* s = tmp.data();
    for (size_t i = 0; i < n; i++, s += 4)
    {
        dst[i] = (uint16_t)((s[0] >> 3) | ((s[1] >> 3) << 5) | ((s[2] >> 3) << 10) | ((s[3] >= 128) << 15));
    }
}

void emunds_glDrawBuffer(GLenum buf)
{
    GLint fb = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    if (fb == 0 && buf != GL_NONE) buf = GL_BACK;
    glDrawBuffers(1, &buf);
}

void* emunds_glMapBuffer(GLenum target, GLenum)
{
    GLint size = 0;
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    if (size <= 0) return nullptr;
    return glMapBufferRange(target, 0, size, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
}

} // extern "C"

// ---------------------------------------------------------------------------
// GLSL 1.40 -> GLSL ES 3.20

static const char* kPrecision =
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp sampler2D;\n"
    "precision highp sampler2DArray;\n"
    "precision highp sampler3D;\n"
    "precision highp usampler2D;\n"
    "precision highp usampler2DArray;\n"
    "precision highp isampler2D;\n"
    "precision highp samplerBuffer;\n"
    "precision highp usamplerBuffer;\n"
    "precision highp isamplerBuffer;\n";

static uint64_t Fnv1a64(const std::string& s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ULL; }
    return h;
}

std::string emunds_TranslateShader(const std::string& src, bool fragment, const EmundsFragOut* outs, int numOuts)
{
    // Known melonDS shaders were cross-compiled offline (glslang + SPIRV-Cross),
    // which fixes the implicit int/float conversions GLSL ES does not allow.
    std::string s;
    uint64_t h = Fnv1a64(src);
    for (const auto& e : kGlesShaders)
    {
        if (e.Hash == h) { s = e.Source; break; }
    }
    if (s.empty()) s = src; // unknown shader: best-effort textual translation below

    // version directive (must stay the first statement)
    size_t v = s.find("#version 320 es") == std::string::npos ? s.find("#version") : std::string::npos;
    std::string header = "#version 320 es\n";
    header += kPrecision;
    if (v != std::string::npos)
    {
        size_t eol = s.find('\n', v);
        s.replace(v, (eol == std::string::npos ? s.size() : eol + 1) - v, header);
    }
    else if (s.find("#version") == std::string::npos)
    {
        s = header + s;
    }

    if (fragment && numOuts > 0)
    {
        // "out vec4 oColor;" -> "layout(location = N) out vec4 oColor;"
        for (int i = 0; i < numOuts; i++)
        {
            std::regex re("(^|\\n)([ \\t]*)out((?:[ \\t]+\\w+)+?[ \\t]+)" + std::string(outs[i].Name) + "([ \\t]*;)");
            s = std::regex_replace(s, re, "$1$2layout(location = " + std::to_string(outs[i].Location) + ") out$3" +
                                              outs[i].Name + "$4");
        }
    }
    return s;
}
