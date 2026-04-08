/**
 * crossos/render.h  –  Cross-platform rendering backend abstraction.
 *
 * This API lets applications target a single CrossOS renderer object while
 * the SDK selects the best backend per platform (software today; hardware
 * backends such as OpenGL/Vulkan can be plugged in later without changing
 * application code).
 */

#ifndef CROSSOS_RENDER_H
#define CROSSOS_RENDER_H

#include "types.h"
#include "window.h"
#include "display.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum crossos_render_backend
    {
        CROSSOS_RENDER_BACKEND_AUTO = 0,
        CROSSOS_RENDER_BACKEND_SOFTWARE,
        CROSSOS_RENDER_BACKEND_OPENGL,
        CROSSOS_RENDER_BACKEND_VULKAN,
    } crossos_render_backend_t;

    typedef struct crossos_renderer_caps
    {
        int is_hardware_accelerated;
        int supports_compute;
        int supports_offscreen;
        int max_texture_size;
    } crossos_renderer_caps_t;

    typedef struct crossos_renderer crossos_renderer_t;

    int crossos_renderer_backend_is_available(crossos_render_backend_t backend);

    int crossos_renderer_backend_is_implemented(crossos_render_backend_t backend);

    crossos_render_backend_t crossos_renderer_select_backend(crossos_render_backend_t preferred);

    crossos_result_t crossos_renderer_create(crossos_window_t *win,
                                             crossos_render_backend_t preferred,
                                             crossos_renderer_t **out_renderer);

    void crossos_renderer_destroy(crossos_renderer_t *renderer);

    crossos_render_backend_t crossos_renderer_backend(const crossos_renderer_t *renderer);

    crossos_result_t crossos_renderer_get_caps(const crossos_renderer_t *renderer,
                                               crossos_renderer_caps_t *out_caps);

    void *crossos_renderer_get_native_target(const crossos_renderer_t *renderer);

    crossos_result_t crossos_renderer_begin_software_frame(crossos_renderer_t *renderer,
                                                           crossos_framebuffer_t *out_fb);

    crossos_result_t crossos_renderer_end_software_frame(crossos_renderer_t *renderer);

    /**
     * Make the renderer's OpenGL context current on the calling thread.
     * Must be called before issuing any OpenGL draw calls.
     * Returns CROSSOS_ERR_UNSUPPORT if this is not an OpenGL renderer.
     */
    crossos_result_t crossos_renderer_make_current(crossos_renderer_t *renderer);

    /**
     * Present the rendered frame:
     *   - OpenGL : calls SwapBuffers / glXSwapBuffers (double-buffered).
     *   - Software: equivalent to crossos_renderer_end_software_frame().
     *               Only valid after a successful begin_software_frame() call.
     */
    crossos_result_t crossos_renderer_present(crossos_renderer_t *renderer);

    /**
     * Return the platform-specific OpenGL context handle:
     *   Linux   → GLXContext
     *   Windows → HGLRC
     *   Android → EGLContext (stub: NULL until EGL backend is implemented)
     * Returns NULL if this is not an OpenGL renderer.
     */
    void *crossos_renderer_get_gl_context(const crossos_renderer_t *renderer);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_RENDER_H */
