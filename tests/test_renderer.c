/**
 * tests/test_renderer.c  –  Headless unit tests for the CrossOS renderer API.
 *
 * These tests run without a display (no window is created).  They verify the
 * renderer backend selection and metadata APIs which are always available.
 * Tests that require an OpenGL context (make_current, present) are skipped
 * when no display is available so that they can run in CI.
 */

#include <crossos/render.h>

#include <stdio.h>
#include <assert.h>
#include <string.h>

/* ── helpers ─────────────────────────────────────────────────────────── */

static int s_passed = 0;
static int s_failed = 0;

#define PASS(name) do { printf("  PASS  %s\n", (name)); s_passed++; } while (0)
#define FAIL(name, msg) do { printf("  FAIL  %s – %s\n", (name), (msg)); s_failed++; } while (0)
#define CHECK(name, cond) do { if (cond) { PASS(name); } else { FAIL(name, #cond); } } while (0)

/* ── Tests ───────────────────────────────────────────────────────────── */

static void test_backend_availability(void)
{
    printf("\n[backend_availability]\n");

    /* Software is always available and implemented */
    CHECK("software_is_available",
          crossos_renderer_backend_is_available(CROSSOS_RENDER_BACKEND_SOFTWARE) == 1);
    CHECK("software_is_implemented",
          crossos_renderer_backend_is_implemented(CROSSOS_RENDER_BACKEND_SOFTWARE) == 1);

    /* Vulkan is not yet implemented */
    CHECK("vulkan_not_implemented",
          crossos_renderer_backend_is_implemented(CROSSOS_RENDER_BACKEND_VULKAN) == 0);

#if (defined(__linux__) && !defined(__ANDROID__)) || defined(_WIN32)
    /* OpenGL backend is implemented on Linux and Windows */
    CHECK("opengl_implemented_on_desktop",
          crossos_renderer_backend_is_implemented(CROSSOS_RENDER_BACKEND_OPENGL) == 1);
#else
    /* Not yet implemented on Android */
    CHECK("opengl_not_implemented_on_android",
          crossos_renderer_backend_is_implemented(CROSSOS_RENDER_BACKEND_OPENGL) == 0);
#endif
}

static void test_backend_selection(void)
{
    printf("\n[backend_selection]\n");

    /* Requesting SOFTWARE always returns SOFTWARE */
    crossos_render_backend_t sel =
        crossos_renderer_select_backend(CROSSOS_RENDER_BACKEND_SOFTWARE);
    CHECK("select_software", sel == CROSSOS_RENDER_BACKEND_SOFTWARE);

    /* Requesting an unavailable backend falls back to SOFTWARE */
    sel = crossos_renderer_select_backend(CROSSOS_RENDER_BACKEND_VULKAN);
    CHECK("select_vulkan_falls_back_to_software",
          sel == CROSSOS_RENDER_BACKEND_SOFTWARE ||
          sel == CROSSOS_RENDER_BACKEND_VULKAN); /* only if truly available */

    /* AUTO must return a valid, implemented backend */
    sel = crossos_renderer_select_backend(CROSSOS_RENDER_BACKEND_AUTO);
    int sel_impl = crossos_renderer_backend_is_implemented(sel);
    CHECK("auto_returns_implemented_backend", sel_impl == 1);
}

static void test_null_safety(void)
{
    printf("\n[null_safety]\n");

    CHECK("backend_null",
          crossos_renderer_backend(NULL) == CROSSOS_RENDER_BACKEND_AUTO);
    CHECK("get_caps_null",
          crossos_renderer_get_caps(NULL, NULL) == CROSSOS_ERR_PARAM);
    CHECK("get_native_target_null",
          crossos_renderer_get_native_target(NULL) == NULL);
    CHECK("get_gl_context_null",
          crossos_renderer_get_gl_context(NULL) == NULL);
    CHECK("make_current_null",
          crossos_renderer_make_current(NULL) == CROSSOS_ERR_PARAM);
    CHECK("present_null",
          crossos_renderer_present(NULL) == CROSSOS_ERR_PARAM);
    CHECK("begin_sw_frame_null",
          crossos_renderer_begin_software_frame(NULL, NULL) == CROSSOS_ERR_PARAM);
    CHECK("end_sw_frame_null",
          crossos_renderer_end_software_frame(NULL) == CROSSOS_ERR_PARAM);
    crossos_renderer_destroy(NULL); /* must not crash */
    PASS("destroy_null_no_crash");
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== CrossOS renderer tests ===\n");

    test_backend_availability();
    test_backend_selection();
    test_null_safety();

    printf("\n%d passed, %d failed\n", s_passed, s_failed);
    return s_failed > 0 ? 1 : 0;
}
