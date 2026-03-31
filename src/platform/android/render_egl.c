/**
 * platform/android/render_egl.c  –  OpenGL ES context management via EGL (stub).
 *
 * This file provides the Android OpenGL ES / EGL backend for the CrossOS
 * renderer.  It currently returns CROSSOS_ERR_UNSUPPORT from all entry
 * points.
 *
 * A full implementation would follow this pipeline:
 *   1.  eglGetDisplay(EGL_DEFAULT_DISPLAY)
 *   2.  eglInitialize(dpy, &major, &minor)
 *   3.  eglChooseConfig(dpy, attribs, &cfg, 1, &n)  – RGBA8 + depth24 + ES2
 *   4.  eglCreateWindowSurface(dpy, cfg, (ANativeWindow*)native_handle, NULL)
 *   5.  static const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
 *       eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs)
 *   6.  eglMakeCurrent(dpy, surface, surface, ctx)
 *   7.  Each frame: eglSwapBuffers(dpy, surface)
 *   8.  Teardown: eglDestroyContext / eglDestroySurface / eglTerminate
 *
 * Requires linking: EGL, GLESv2
 * AndroidManifest.xml:
 *   <uses-feature android:glEsVersion="0x00020000" android:required="true" />
 */

#if defined(__ANDROID__)

#include <crossos/render.h>
#include <crossos/window.h>

crossos_result_t render_gl_platform_create(crossos_window_t *win, void **out_ctx)
{
    (void)win;
    (void)out_ctx;
    return CROSSOS_ERR_UNSUPPORT;
}

void render_gl_platform_destroy(crossos_window_t *win, void *ctx)
{
    (void)win;
    (void)ctx;
}

crossos_result_t render_gl_platform_make_current(crossos_window_t *win, void *ctx)
{
    (void)win;
    (void)ctx;
    return CROSSOS_ERR_UNSUPPORT;
}

crossos_result_t render_gl_platform_present(crossos_window_t *win, void *ctx)
{
    (void)win;
    (void)ctx;
    return CROSSOS_ERR_UNSUPPORT;
}

#endif /* __ANDROID__ */
