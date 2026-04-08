/**
 * platform/windows/render_wgl.c  –  OpenGL context management via WGL.
 *
 * Creates and manages a WGL rendering context for a CrossOS window.
 * Uses ChoosePixelFormat / SetPixelFormat / wglCreateContext for
 * a double-buffered RGBA context with a 24-bit depth buffer.
 *
 * Requires:
 *   • opengl32.lib  (-lopengl32)
 */

#ifdef _WIN32

#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>

#include <crossos/render.h>
#include <crossos/window.h>

#include <stdlib.h>

extern void crossos__set_error(const char *fmt, ...);

/* ── Internal context handle ─────────────────────────────────────────── */

typedef struct
{
    HGLRC hglrc;
    HDC   hdc;
    HWND  hwnd;
} wgl_ctx_t;

/* ── Platform API ────────────────────────────────────────────────────── */

crossos_result_t render_gl_platform_create(crossos_window_t *win, void **out_ctx)
{
    HWND hwnd = (HWND)crossos_window_get_native_handle(win);
    if (!hwnd)
    {
        crossos__set_error("WGL: invalid window handle");
        return CROSSOS_ERR_PARAM;
    }

    HDC hdc = GetDC(hwnd);
    if (!hdc)
    {
        crossos__set_error("WGL: GetDC failed (error %lu)", GetLastError());
        return CROSSOS_ERR_DISPLAY;
    }

    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize        = sizeof(pfd);
    pfd.nVersion     = 1;
    pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType   = PFD_TYPE_RGBA;
    pfd.cColorBits   = 32;
    pfd.cRedBits     = 8;
    pfd.cGreenBits   = 8;
    pfd.cBlueBits    = 8;
    pfd.cAlphaBits   = 8;
    pfd.cDepthBits   = 24;
    pfd.iLayerType   = PFD_MAIN_PLANE;

    int pf = ChoosePixelFormat(hdc, &pfd);
    if (!pf)
    {
        crossos__set_error("WGL: ChoosePixelFormat failed (error %lu)", GetLastError());
        ReleaseDC(hwnd, hdc);
        return CROSSOS_ERR_DISPLAY;
    }

    if (!SetPixelFormat(hdc, pf, &pfd))
    {
        crossos__set_error("WGL: SetPixelFormat failed (error %lu)", GetLastError());
        ReleaseDC(hwnd, hdc);
        return CROSSOS_ERR_DISPLAY;
    }

    HGLRC hglrc = wglCreateContext(hdc);
    if (!hglrc)
    {
        crossos__set_error("WGL: wglCreateContext failed (error %lu)", GetLastError());
        ReleaseDC(hwnd, hdc);
        return CROSSOS_ERR_DISPLAY;
    }

    wgl_ctx_t *wc = (wgl_ctx_t *)malloc(sizeof(*wc));
    if (!wc)
    {
        wglDeleteContext(hglrc);
        ReleaseDC(hwnd, hdc);
        return CROSSOS_ERR_OOM;
    }

    wc->hglrc = hglrc;
    wc->hdc   = hdc;
    wc->hwnd  = hwnd;
    *out_ctx  = wc;
    return CROSSOS_OK;
}

void render_gl_platform_destroy(crossos_window_t *win, void *ctx)
{
    (void)win;
    if (!ctx)
        return;

    wgl_ctx_t *wc = (wgl_ctx_t *)ctx;

    /* Release context before destruction */
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(wc->hglrc);
    ReleaseDC(wc->hwnd, wc->hdc);
    free(wc);
}

crossos_result_t render_gl_platform_make_current(crossos_window_t *win, void *ctx)
{
    (void)win;
    if (!ctx)
        return CROSSOS_ERR_PARAM;

    wgl_ctx_t *wc = (wgl_ctx_t *)ctx;

    if (!wglMakeCurrent(wc->hdc, wc->hglrc))
    {
        crossos__set_error("WGL: wglMakeCurrent failed (error %lu)", GetLastError());
        return CROSSOS_ERR_DISPLAY;
    }
    return CROSSOS_OK;
}

crossos_result_t render_gl_platform_present(crossos_window_t *win, void *ctx)
{
    (void)win;
    if (!ctx)
        return CROSSOS_ERR_PARAM;

    wgl_ctx_t *wc = (wgl_ctx_t *)ctx;
    SwapBuffers(wc->hdc);
    return CROSSOS_OK;
}

#endif /* _WIN32 */
