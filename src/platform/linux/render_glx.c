/**
 * platform/linux/render_glx.c  –  OpenGL context management via GLX.
 *
 * Creates and manages a GLX rendering context for a CrossOS window.
 * Uses GLX 1.3 FBConfigs for context creation so that the context is
 * compatible with any window regardless of its visual.
 *
 * Requires:
 *   • libGL      (-lGL)
 *   • X11 display initialised (s_display / s_screen from x11_internal.h)
 */

#if defined(__linux__) && !defined(__ANDROID__)

#include <crossos/render.h>
#include <crossos/window.h>

#include "x11_internal.h"

#include <GL/glx.h>

#include <stdlib.h>
#include <stdint.h>

extern void crossos__set_error(const char *fmt, ...);

/* ── Internal context handle ─────────────────────────────────────────── */

typedef struct
{
    GLXContext ctx;
} glx_ctx_t;

/* ── Platform API ────────────────────────────────────────────────────── */

crossos_result_t render_gl_platform_create(crossos_window_t *win, void **out_ctx)
{
    (void)win;

    if (!s_display)
    {
        crossos__set_error("GLX: X11 display not initialised");
        return CROSSOS_ERR_DISPLAY;
    }

    /* Select an FBConfig with RGBA, double-buffered, depth-24 */
    static const int attribs[] = {
        GLX_RENDER_TYPE,   GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_DOUBLEBUFFER,  True,
        GLX_RED_SIZE,      8,
        GLX_GREEN_SIZE,    8,
        GLX_BLUE_SIZE,     8,
        GLX_ALPHA_SIZE,    8,
        GLX_DEPTH_SIZE,    24,
        None
    };

    int n = 0;
    GLXFBConfig *fbc = glXChooseFBConfig(s_display, s_screen, attribs, &n);
    if (!fbc || n == 0)
    {
        crossos__set_error("GLX: glXChooseFBConfig failed – OpenGL may not be available");
        return CROSSOS_ERR_DISPLAY;
    }

    /* Create a direct-rendering context (falls back to indirect if needed) */
    GLXContext ctx = glXCreateNewContext(s_display, fbc[0], GLX_RGBA_TYPE, NULL, GL_TRUE);
    XFree(fbc);

    if (!ctx)
    {
        crossos__set_error("GLX: glXCreateNewContext failed");
        return CROSSOS_ERR_DISPLAY;
    }

    glx_ctx_t *gc = (glx_ctx_t *)malloc(sizeof(*gc));
    if (!gc)
    {
        glXDestroyContext(s_display, ctx);
        return CROSSOS_ERR_OOM;
    }

    gc->ctx  = ctx;
    *out_ctx = gc;
    return CROSSOS_OK;
}

void render_gl_platform_destroy(crossos_window_t *win, void *ctx)
{
    (void)win;
    if (!ctx || !s_display)
        return;

    glx_ctx_t *gc = (glx_ctx_t *)ctx;

    /* Release context before destruction */
    glXMakeCurrent(s_display, None, NULL);
    glXDestroyContext(s_display, gc->ctx);
    free(gc);
}

crossos_result_t render_gl_platform_make_current(crossos_window_t *win, void *ctx)
{
    if (!ctx || !s_display)
        return CROSSOS_ERR_PARAM;

    glx_ctx_t *gc  = (glx_ctx_t *)ctx;
    Window     xwin = (Window)(uintptr_t)crossos_window_get_native_handle(win);

    if (!glXMakeCurrent(s_display, xwin, gc->ctx))
    {
        crossos__set_error("GLX: glXMakeCurrent failed");
        return CROSSOS_ERR_DISPLAY;
    }
    return CROSSOS_OK;
}

crossos_result_t render_gl_platform_present(crossos_window_t *win, void *ctx)
{
    (void)ctx;
    if (!s_display)
        return CROSSOS_ERR_PARAM;

    Window xwin = (Window)(uintptr_t)crossos_window_get_native_handle(win);
    glXSwapBuffers(s_display, xwin);
    return CROSSOS_OK;
}

#endif /* __linux__ && !__ANDROID__ */
