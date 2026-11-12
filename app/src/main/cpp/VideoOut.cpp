// EmuNDS - EGL / OpenGL ES presenter, owned by the emulation thread.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "VideoOut.h"

#define EMUNDS_GLES_NO_WRAP
#include "gles/emunds_gles.h"
#include <android/log.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "EmuNDS-Video", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "EmuNDS-Video", __VA_ARGS__)

static const char* kVS = R"(#version 300 es
uniform vec4 uRect; // NDC: x0, y0, x1, y1
out vec2 vUV;
void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vUV = vec2(c.x, 1.0 - c.y);
    gl_Position = vec4(mix(uRect.xy, uRect.zw, c), 0.0, 1.0);
}
)";

// layer 0/1 is selected through the 3rd texture coordinate
static const char* kFS = R"(#version 300 es
precision mediump float;
precision mediump sampler2DArray;
uniform sampler2DArray uTex;
uniform vec2 uSwap; // x = layer, y = 1 to swap R/B (software renderer outputs BGRA)
in vec2 vUV;
out vec4 oColor;
void main() {
    vec4 c = texture(uTex, vec3(vUV, uSwap.x));
    oColor = vec4(uSwap.y > 0.5 ? c.bgr : c.rgb, 1.0);
}
)";

static GLuint Compile(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("presenter shader: %s", log);
    }
    return s;
}

VideoOut& VideoOut::Get()
{
    static VideoOut v;
    return v;
}

bool VideoOut::InitEGL()
{
    if (ctx != EGL_NO_CONTEXT) return true;

    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr))
    {
        LOGE("eglInitialize failed: 0x%x", eglGetError());
        return false;
    }

    const EGLint cfgAttr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
        EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgAttr, &config, 1, &n) || n < 1)
    {
        LOGE("eglChooseConfig failed");
        return false;
    }

    // Prefer GLES 3.2 (needed by the hi-res OpenGL renderer), fall back to 3.0.
    const EGLint ctx32[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    const EGLint ctx30[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx32);
    gles32 = ctx != EGL_NO_CONTEXT;
    if (!gles32) ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx30);
    if (ctx == EGL_NO_CONTEXT)
    {
        LOGE("eglCreateContext failed: 0x%x", eglGetError());
        return false;
    }

    const EGLint pbAttr[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    pbuffer = eglCreatePbufferSurface(dpy, config, pbAttr);
    if (!MakeCurrent()) return false;

    LOGI("GL: %s / %s (ES 3.2: %d)", glGetString(GL_RENDERER), glGetString(GL_VERSION), gles32);
    EnsureGLObjects();
    return true;
}

void VideoOut::ShutdownEGL()
{
    if (dpy == EGL_NO_DISPLAY) return;
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (winSurface != EGL_NO_SURFACE) eglDestroySurface(dpy, winSurface);
    if (pbuffer != EGL_NO_SURFACE) eglDestroySurface(dpy, pbuffer);
    if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
    if (window) ANativeWindow_release(window);
    eglTerminate(dpy);
    winSurface = pbuffer = EGL_NO_SURFACE;
    ctx = EGL_NO_CONTEXT;
    dpy = EGL_NO_DISPLAY;
    window = nullptr;
}

bool VideoOut::MakeCurrent()
{
    EGLSurface s = winSurface != EGL_NO_SURFACE ? winSurface : pbuffer;
    if (eglGetCurrentSurface(EGL_DRAW) == s && eglGetCurrentContext() == ctx) return true;
    if (!eglMakeCurrent(dpy, s, s, ctx))
    {
        LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
        return false;
    }
    return true;
}

void VideoOut::SetWindow(ANativeWindow* win)
{
    if (dpy == EGL_NO_DISPLAY) { if (win) ANativeWindow_release(win); return; }

    if (winSurface != EGL_NO_SURFACE)
    {
        eglMakeCurrent(dpy, pbuffer, pbuffer, ctx);
        eglDestroySurface(dpy, winSurface);
        winSurface = EGL_NO_SURFACE;
    }
    if (window) ANativeWindow_release(window);
    window = win;

    if (window)
    {
        winSurface = eglCreateWindowSurface(dpy, config, window, nullptr);
        if (winSurface == EGL_NO_SURFACE) LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
    }
    MakeCurrent();
    if (winSurface != EGL_NO_SURFACE)
    {
        // never block the emulation thread on vsync; pacing comes from the audio clock
        eglSwapInterval(dpy, 0);
    }
    needRedraw = true;
}

void VideoOut::EnsureGLObjects()
{
    if (prog) return;
    GLuint vs = Compile(GL_VERTEX_SHADER, kVS);
    GLuint fs = Compile(GL_FRAGMENT_SHADER, kFS);
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    locRect = glGetUniformLocation(prog, "uRect");
    locSwap = glGetUniformLocation(prog, "uSwap");
    locTex = glGetUniformLocation(prog, "uTex");

    glGenVertexArrays(1, &vao);

    glGenTextures(1, &softTex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, softTex);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 256, 192, 2);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void VideoOut::SetLayout(const int top[4], const int bottom[4])
{
    {
        std::lock_guard<std::mutex> lk(layoutLock);
        for (int i = 0; i < 4; i++) { topRect[i] = top[i]; botRect[i] = bottom[i]; }
    }
    needRedraw = true;
}

void VideoOut::SetFilter(int f)
{
    filter.store(f);
    needRedraw = true;
}

void VideoOut::Draw(unsigned tex, bool swapRB)
{
    if (winSurface == EGL_NO_SURFACE || !MakeCurrent()) return;

    EGLint w = 1, h = 1;
    eglQuerySurface(dpy, winSurface, EGL_WIDTH, &w);
    eglQuerySurface(dpy, winSurface, EGL_HEIGHT, &h);

    // The melonDS GL renderer leaves arbitrary state behind: reset what we rely on.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, w, h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(prog);
    glBindVertexArray(vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    GLint mode = filter.load() ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, mode);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, mode);
    glUniform1i(locTex, 0);

    int t[4], b[4];
    {
        std::lock_guard<std::mutex> lk(layoutLock);
        for (int i = 0; i < 4; i++) { t[i] = topRect[i]; b[i] = botRect[i]; }
    }
    const int* rects[2] = {t, b};
    for (int layer = 0; layer < 2; layer++)
    {
        const int* r = rects[layer];
        if (r[2] <= 0 || r[3] <= 0) continue;
        float x0 = (float)r[0] / w * 2.f - 1.f;
        float x1 = (float)(r[0] + r[2]) / w * 2.f - 1.f;
        float y0 = 1.f - (float)(r[1] + r[3]) / h * 2.f;
        float y1 = 1.f - (float)r[1] / h * 2.f;
        glUniform4f(locRect, x0, y0, x1, y1);
        glUniform2f(locSwap, (float)layer, swapRB ? 1.f : 0.f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    eglSwapBuffers(dpy, winSurface);
}

void VideoOut::PresentSoft(const uint32_t* top, const uint32_t* bottom)
{
    if (winSurface == EGL_NO_SURFACE || !MakeCurrent()) return;
    glBindTexture(GL_TEXTURE_2D_ARRAY, softTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 256, 192, 1, GL_RGBA, GL_UNSIGNED_BYTE, top);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, 256, 192, 1, GL_RGBA, GL_UNSIGNED_BYTE, bottom);
    Draw(softTex, true);
}

void VideoOut::PresentTexture(unsigned tex)
{
    Draw(tex, false);
}

void VideoOut::PresentBlank()
{
    if (winSurface == EGL_NO_SURFACE || !MakeCurrent()) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(dpy, winSurface);
}
