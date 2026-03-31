#include <crossos/render.h>

#include <stdlib.h>
#include <string.h>

extern void crossos__set_error(const char *fmt, ...);

/* ── Platform OpenGL backend (render_glx.c / render_wgl.c / render_egl.c) ─ */

#if (defined(__linux__) && !defined(__ANDROID__)) || defined(_WIN32)
crossos_result_t render_gl_platform_create(crossos_window_t *win, void **out_ctx);
void             render_gl_platform_destroy(crossos_window_t *win, void *ctx);
crossos_result_t render_gl_platform_make_current(crossos_window_t *win, void *ctx);
crossos_result_t render_gl_platform_present(crossos_window_t *win, void *ctx);
#define CROSSOS_HAS_GL_BACKEND 1
#elif defined(__ANDROID__)
crossos_result_t render_gl_platform_create(crossos_window_t *win, void **out_ctx);
void             render_gl_platform_destroy(crossos_window_t *win, void *ctx);
crossos_result_t render_gl_platform_make_current(crossos_window_t *win, void *ctx);
crossos_result_t render_gl_platform_present(crossos_window_t *win, void *ctx);
#define CROSSOS_HAS_GL_BACKEND 0  /* EGL stub – not yet implemented */
#else
#define CROSSOS_HAS_GL_BACKEND 0
#endif

struct crossos_renderer
{
    crossos_window_t *win;
    crossos_surface_t *surface;
    void *native_target;
    void *gl_context; /* platform GL context (GLXContext / HGLRC / EGLContext) */
    crossos_render_backend_t backend;
    crossos_renderer_caps_t caps;
    int frame_open;
};

static void set_caps_for_backend(crossos_render_backend_t backend,
                                 crossos_renderer_caps_t *caps)
{
    memset(caps, 0, sizeof(*caps));
    switch (backend)
    {
    case CROSSOS_RENDER_BACKEND_SOFTWARE:
        caps->is_hardware_accelerated = 0;
        caps->supports_compute = 0;
        caps->supports_offscreen = 0;
        caps->max_texture_size = 0;
        break;
    case CROSSOS_RENDER_BACKEND_OPENGL:
    case CROSSOS_RENDER_BACKEND_VULKAN:
        caps->is_hardware_accelerated = 1;
        caps->supports_compute = (backend == CROSSOS_RENDER_BACKEND_VULKAN) ? 1 : 0;
        caps->supports_offscreen = 1;
        caps->max_texture_size = 16384;
        break;
    case CROSSOS_RENDER_BACKEND_AUTO:
    default:
        break;
    }
}

int crossos_renderer_backend_is_available(crossos_render_backend_t backend)
{
    switch (backend)
    {
    case CROSSOS_RENDER_BACKEND_SOFTWARE:
        return 1;
    case CROSSOS_RENDER_BACKEND_OPENGL:
        return 1;
    case CROSSOS_RENDER_BACKEND_VULKAN:
        return 0;
    case CROSSOS_RENDER_BACKEND_AUTO:
    default:
        return 1;
    }
}

int crossos_renderer_backend_is_implemented(crossos_render_backend_t backend)
{
    switch (backend)
    {
    case CROSSOS_RENDER_BACKEND_SOFTWARE:
        return 1;
    case CROSSOS_RENDER_BACKEND_OPENGL:
        return CROSSOS_HAS_GL_BACKEND;
    case CROSSOS_RENDER_BACKEND_VULKAN:
    case CROSSOS_RENDER_BACKEND_AUTO:
    default:
        return 0;
    }
}

crossos_render_backend_t crossos_renderer_select_backend(crossos_render_backend_t preferred)
{
    if (preferred == CROSSOS_RENDER_BACKEND_AUTO)
    {
        if (crossos_renderer_backend_is_available(CROSSOS_RENDER_BACKEND_VULKAN))
            return CROSSOS_RENDER_BACKEND_VULKAN;
        if (crossos_renderer_backend_is_available(CROSSOS_RENDER_BACKEND_OPENGL))
            return CROSSOS_RENDER_BACKEND_OPENGL;
        return CROSSOS_RENDER_BACKEND_SOFTWARE;
    }

    if (crossos_renderer_backend_is_available(preferred))
        return preferred;

    return CROSSOS_RENDER_BACKEND_SOFTWARE;
}

crossos_result_t crossos_renderer_create(crossos_window_t *win,
                                         crossos_render_backend_t preferred,
                                         crossos_renderer_t **out_renderer)
{
    if (!win || !out_renderer)
        return CROSSOS_ERR_PARAM;

    *out_renderer = NULL;

    crossos_render_backend_t selected = crossos_renderer_select_backend(preferred);

    if (selected != preferred && preferred != CROSSOS_RENDER_BACKEND_AUTO)
    {
        crossos__set_error("renderer backend unavailable; fell back to software");
    }

    if (!crossos_renderer_backend_is_implemented(selected))
    {
        crossos__set_error("selected renderer backend not implemented yet");
        return CROSSOS_ERR_UNSUPPORT;
    }

    crossos_surface_t *surface = NULL;
    if (selected == CROSSOS_RENDER_BACKEND_SOFTWARE)
    {
        surface = crossos_surface_get(win);
        if (!surface)
            return CROSSOS_ERR_DISPLAY;
    }

    crossos_renderer_t *renderer = (crossos_renderer_t *)malloc(sizeof(*renderer));
    if (!renderer)
        return CROSSOS_ERR_OOM;

    renderer->win = win;
    renderer->surface = surface;
    renderer->native_target = crossos_window_get_native_handle(win);
    renderer->gl_context = NULL;
    renderer->backend = selected;
    renderer->frame_open = 0;
    set_caps_for_backend(selected, &renderer->caps);

#if CROSSOS_HAS_GL_BACKEND
    if (selected == CROSSOS_RENDER_BACKEND_OPENGL)
    {
        crossos_result_t rc = render_gl_platform_create(win, &renderer->gl_context);
        if (rc != CROSSOS_OK)
        {
            free(renderer);
            return rc;
        }
    }
#endif

    *out_renderer = renderer;
    return CROSSOS_OK;
}

void crossos_renderer_destroy(crossos_renderer_t *renderer)
{
    if (!renderer)
        return;

    if (renderer->frame_open && renderer->backend == CROSSOS_RENDER_BACKEND_SOFTWARE)
    {
        crossos_surface_unlock(renderer->surface);
        renderer->frame_open = 0;
    }

#if CROSSOS_HAS_GL_BACKEND
    if (renderer->backend == CROSSOS_RENDER_BACKEND_OPENGL && renderer->gl_context)
    {
        render_gl_platform_destroy(renderer->win, renderer->gl_context);
        renderer->gl_context = NULL;
    }
#endif

    free(renderer);
}

crossos_render_backend_t crossos_renderer_backend(const crossos_renderer_t *renderer)
{
    if (!renderer)
        return CROSSOS_RENDER_BACKEND_AUTO;
    return renderer->backend;
}

crossos_result_t crossos_renderer_get_caps(const crossos_renderer_t *renderer,
                                           crossos_renderer_caps_t *out_caps)
{
    if (!renderer || !out_caps)
        return CROSSOS_ERR_PARAM;
    *out_caps = renderer->caps;
    return CROSSOS_OK;
}

void *crossos_renderer_get_native_target(const crossos_renderer_t *renderer)
{
    if (!renderer)
        return NULL;
    return renderer->native_target;
}

crossos_result_t crossos_renderer_begin_software_frame(crossos_renderer_t *renderer,
                                                       crossos_framebuffer_t *out_fb)
{
    if (!renderer || !out_fb)
        return CROSSOS_ERR_PARAM;
    if (renderer->backend != CROSSOS_RENDER_BACKEND_SOFTWARE)
        return CROSSOS_ERR_UNSUPPORT;
    if (renderer->frame_open)
        return CROSSOS_ERR_PARAM;

    crossos_result_t rc = crossos_surface_lock(renderer->surface, out_fb);
    if (rc == CROSSOS_OK)
        renderer->frame_open = 1;
    return rc;
}

crossos_result_t crossos_renderer_end_software_frame(crossos_renderer_t *renderer)
{
    if (!renderer)
        return CROSSOS_ERR_PARAM;
    if (renderer->backend != CROSSOS_RENDER_BACKEND_SOFTWARE)
        return CROSSOS_ERR_UNSUPPORT;
    if (!renderer->frame_open)
        return CROSSOS_ERR_PARAM;

    crossos_surface_unlock(renderer->surface);
    renderer->frame_open = 0;
    return crossos_surface_present(renderer->surface);
}

crossos_result_t crossos_renderer_make_current(crossos_renderer_t *renderer)
{
    if (!renderer)
        return CROSSOS_ERR_PARAM;
    if (renderer->backend != CROSSOS_RENDER_BACKEND_OPENGL)
        return CROSSOS_ERR_UNSUPPORT;
#if CROSSOS_HAS_GL_BACKEND
    return render_gl_platform_make_current(renderer->win, renderer->gl_context);
#else
    return CROSSOS_ERR_UNSUPPORT;
#endif
}

crossos_result_t crossos_renderer_present(crossos_renderer_t *renderer)
{
    if (!renderer)
        return CROSSOS_ERR_PARAM;

    if (renderer->backend == CROSSOS_RENDER_BACKEND_SOFTWARE)
    {
        if (!renderer->frame_open)
            return CROSSOS_ERR_PARAM;
        crossos_surface_unlock(renderer->surface);
        renderer->frame_open = 0;
        return crossos_surface_present(renderer->surface);
    }

    if (renderer->backend == CROSSOS_RENDER_BACKEND_OPENGL)
    {
#if CROSSOS_HAS_GL_BACKEND
        return render_gl_platform_present(renderer->win, renderer->gl_context);
#else
        return CROSSOS_ERR_UNSUPPORT;
#endif
    }

    return CROSSOS_ERR_UNSUPPORT;
}

void *crossos_renderer_get_gl_context(const crossos_renderer_t *renderer)
{
    if (!renderer || renderer->backend != CROSSOS_RENDER_BACKEND_OPENGL)
        return NULL;
    return renderer->gl_context;
}
