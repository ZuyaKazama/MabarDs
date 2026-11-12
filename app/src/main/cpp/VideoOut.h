// EmuNDS - EGL / OpenGL ES presenter, owned by the emulation thread.
// The same GL context is used by the melonDS OpenGL renderer, so its hi-res
// output texture is drawn straight to the window with no copies.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <EGL/egl.h>
#include <android/native_window.h>
#include <atomic>
#include <cstdint>
#include <mutex>

class VideoOut
{
public:
    static VideoOut& Get();

    // ---- emulation thread only ----
    bool InitEGL();
    void ShutdownEGL();
    bool HasGLES32() const { return gles32; }
    void SetWindow(ANativeWindow* win);       // takes ownership of one reference
    bool HasWindow() const { return winSurface != EGL_NO_SURFACE; }

    // Software renderer: two 256x192 BGRA buffers.
    void PresentSoft(const uint32_t* top, const uint32_t* bottom);
    // OpenGL renderer: 2-layer RGBA texture array (layer 0 = top, 1 = bottom).
    void PresentTexture(unsigned tex);
    // Black frame (no game loaded).
    void PresentBlank();

    // ---- any thread ----
    // Rects are in window pixels, origin top-left: x, y, w, h. w <= 0 hides that screen.
    void SetLayout(const int top[4], const int bottom[4]);
    void SetFilter(int filter); // 0 nearest, 1 linear
    bool ConsumeRedraw() { return needRedraw.exchange(false); }
    void RequestRedraw() { needRedraw = true; }

private:
    VideoOut() = default;
    bool MakeCurrent();
    void EnsureGLObjects();
    void Draw(unsigned tex, bool swapRB);

    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface pbuffer = EGL_NO_SURFACE;
    EGLSurface winSurface = EGL_NO_SURFACE;
    ANativeWindow* window = nullptr;
    bool gles32 = false;

    unsigned prog = 0, vao = 0, softTex = 0;
    int locRect = -1, locSwap = -1, locTex = -1;

    std::mutex layoutLock;
    int topRect[4] = {0, 0, 0, 0};
    int botRect[4] = {0, 0, 0, 0};
    std::atomic<int> filter{0};
    std::atomic<bool> needRedraw{false};
};
