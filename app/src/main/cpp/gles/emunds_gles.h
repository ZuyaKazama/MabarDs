// EmuNDS - OpenGL ES 3.2 compatibility layer for the melonDS OpenGL renderer.
// Used as MELONDS_GL_HEADER: everything melonDS needs from desktop GL 3.2/4.3
// that is missing in GLES 3.2 is shimmed here.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef EMUNDS_GLES_H
#define EMUNDS_GLES_H

#include <GLES3/gl32.h>

#ifdef __cplusplus
#include <string>
#include <initializer_list>
extern "C" {
#endif

// Desktop-only pixel type used for DS 15-bit colour (R in the low bits, A in bit 15).
// GLES only has GL_UNSIGNED_SHORT_5_5_5_1 (R in the high bits); the wrappers convert.
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#ifndef GL_WRITE_ONLY
#define GL_WRITE_ONLY 0x88B9
#endif

void emunds_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                         GLint border, GLenum format, GLenum type, const void* pixels);
void emunds_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                            GLenum format, GLenum type, const void* pixels);
void emunds_glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth,
                         GLint border, GLenum format, GLenum type, const void* pixels);
void emunds_glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                            GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels);
void emunds_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
void emunds_glDrawBuffer(GLenum buf);
void* emunds_glMapBuffer(GLenum target, GLenum access);
// Locations are baked into the fragment shader source instead (see emunds_TranslateShader).
static inline void emunds_glBindFragDataLocation(GLuint, GLuint, const GLchar*) {}

#ifdef __cplusplus
}

struct EmundsFragOut { const char* Name; unsigned Location; };
// Rewrites desktop GLSL 1.40 / 4.30 into GLSL ES 3.20.
std::string emunds_TranslateShader(const std::string& src, bool fragment,
                                   const EmundsFragOut* outs, int numOuts);
#endif

#ifndef EMUNDS_GLES_NO_WRAP
#define glTexImage2D emunds_glTexImage2D
#define glTexSubImage2D emunds_glTexSubImage2D
#define glTexImage3D emunds_glTexImage3D
#define glTexSubImage3D emunds_glTexSubImage3D
#define glReadPixels emunds_glReadPixels
#define glDrawBuffer emunds_glDrawBuffer
#define glMapBuffer emunds_glMapBuffer
#define glBindFragDataLocation emunds_glBindFragDataLocation
#define glClearDepth glClearDepthf
#define glDepthRange glDepthRangef
#endif

#endif
