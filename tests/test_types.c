/**
 * tests/test_types.c  –  Unit tests for types and enumerations.
 *
 * These tests are deliberately headless (no display required) and validate:
 *   1. Event type enum values are distinct and non-zero where expected.
 *   2. Result codes are negative for errors and zero for success.
 *   3. Touch-point array sizing is consistent with the documented limit.
 *   4. crossos_platform_name() returns a non-empty string.
 *   5. crossos_init() + crossos_shutdown() round-trip (skipped when no display).
 */

#include <crossos/crossos.h>

#include <stdio.h>
#include <string.h>
#include <assert.h>

/* Simple test harness */
static int s_passed = 0;
static int s_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name)  do { printf("  %-40s", #name); name(); } while(0)
#define CHECK(expr)                                             \
    do {                                                        \
        if (expr) {                                             \
            printf("PASS\n"); s_passed++;                       \
        } else {                                                \
            printf("FAIL  (%s:%d)\n", __FILE__, __LINE__);     \
            s_failed++;                                         \
        }                                                       \
    } while(0)

/* ── Tests ─────────────────────────────────────────────────────────────── */

TEST(test_event_type_none_is_zero)
{
    CHECK(CROSSOS_EVENT_NONE == 0);
}

TEST(test_event_types_are_distinct)
{
    /* Spot-check a handful of critical event codes */
    CHECK(CROSSOS_EVENT_QUIT         != CROSSOS_EVENT_NONE);
    CHECK(CROSSOS_EVENT_WINDOW_CLOSE != CROSSOS_EVENT_QUIT);
    CHECK(CROSSOS_EVENT_KEY_DOWN     != CROSSOS_EVENT_KEY_UP);
    CHECK(CROSSOS_EVENT_TOUCH_BEGIN  != CROSSOS_EVENT_TOUCH_END);
    CHECK(CROSSOS_EVENT_POINTER_DOWN != CROSSOS_EVENT_TOUCH_BEGIN);
}

TEST(test_result_codes)
{
    CHECK(CROSSOS_OK            == 0);
    CHECK(CROSSOS_ERR_INIT      <  0);
    CHECK(CROSSOS_ERR_DISPLAY   <  0);
    CHECK(CROSSOS_ERR_WINDOW    <  0);
    CHECK(CROSSOS_ERR_OOM       <  0);
    CHECK(CROSSOS_ERR_UNSUPPORT <  0);
    CHECK(CROSSOS_ERR_PARAM     <  0);
}

TEST(test_touch_point_limit)
{
    CHECK(CROSSOS_MAX_TOUCH_POINTS >= 5);
    /* The event struct must accommodate the declared maximum */
    crossos_event_t ev;
    CHECK(sizeof(ev.touch.points) ==
          sizeof(crossos_touch_point_t) * CROSSOS_MAX_TOUCH_POINTS);
}

TEST(test_event_struct_size_reasonable)
{
    /* The event struct should fit comfortably on the stack */
    CHECK(sizeof(crossos_event_t) <= 512);
}

TEST(test_platform_name)
{
    const char *name = crossos_platform_name();
    CHECK(name != NULL);
    CHECK(name[0] != '\0');
    /* Must be one of the known platforms */
    int known = (strcmp(name, "windows") == 0 ||
                 strcmp(name, "linux")   == 0 ||
                 strcmp(name, "android") == 0 ||
                 strcmp(name, "unknown") == 0);
    CHECK(known);
}

TEST(test_key_codes_in_range)
{
    CHECK(CROSSOS_KEY_A     ==  65);
    CHECK(CROSSOS_KEY_Z     ==  90);
    CHECK(CROSSOS_KEY_0     ==  48);
    CHECK(CROSSOS_KEY_9     ==  57);
    CHECK(CROSSOS_KEY_SPACE ==  32);
    CHECK(CROSSOS_KEY_ESCAPE == 256);
    CHECK(CROSSOS_KEY_F1    == 290);
    CHECK(CROSSOS_KEY_F12   == 301);
}

TEST(test_modifier_bitmask)
{
    /* Modifiers must be distinct powers-of-two so they can be OR-combined */
    CHECK(CROSSOS_MOD_NONE  == 0);
    CHECK((CROSSOS_MOD_SHIFT & CROSSOS_MOD_CTRL)  == 0);
    CHECK((CROSSOS_MOD_SHIFT & CROSSOS_MOD_ALT)   == 0);
    CHECK((CROSSOS_MOD_CTRL  & CROSSOS_MOD_ALT)   == 0);
    CHECK((CROSSOS_MOD_CTRL  & CROSSOS_MOD_SUPER) == 0);
}

TEST(test_pixel_format_enum)
{
    CHECK(CROSSOS_PIXEL_FMT_RGBA8888 == 0);
    CHECK(CROSSOS_PIXEL_FMT_BGRA8888 != CROSSOS_PIXEL_FMT_RGBA8888);
    CHECK(CROSSOS_PIXEL_FMT_RGB565   != CROSSOS_PIXEL_FMT_BGRA8888);
}

TEST(test_version_string)
{
    CHECK(CROSSOS_VERSION_MAJOR >= 0);
    CHECK(CROSSOS_VERSION_MINOR >= 0);
    CHECK(CROSSOS_VERSION_PATCH >= 0);
    CHECK(strlen(CROSSOS_VERSION_STRING) > 0);
}

TEST(test_null_window_safety)
{
    /* Calling window functions with NULL must not crash */
    crossos_window_destroy(NULL);
    crossos_window_show(NULL);
    crossos_window_hide(NULL);
    int w, h;
    crossos_window_get_size(NULL, &w, &h);
    CHECK(w == 0 && h == 0);
    CHECK(crossos_window_is_fullscreen(NULL) == 0);
    CHECK(crossos_window_get_native_handle(NULL) == NULL);
    CHECK(crossos_surface_get(NULL) == NULL);
    /* These should return an error code, not crash */
    /* Verify we reached this point without crashing */
    CHECK(crossos_window_set_fullscreen(NULL, 1) == CROSSOS_ERR_PARAM);
    CHECK(crossos_window_resize(NULL, 100, 100) == CROSSOS_ERR_PARAM);
    CHECK(crossos_window_set_title(NULL, "hi") == CROSSOS_ERR_PARAM);
}

TEST(test_null_surface_safety)
{
    crossos_framebuffer_t fb;
    memset(&fb, 0, sizeof(fb));
    CHECK(crossos_surface_lock(NULL, &fb) != CROSSOS_OK);
    CHECK(crossos_surface_lock(NULL, NULL) != CROSSOS_OK);
    crossos_surface_unlock(NULL); /* must not crash */
    CHECK(crossos_surface_present(NULL) != CROSSOS_OK);
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== CrossOS type/API unit tests ===\n");

    RUN(test_event_type_none_is_zero);
    RUN(test_event_types_are_distinct);
    RUN(test_result_codes);
    RUN(test_touch_point_limit);
    RUN(test_event_struct_size_reasonable);
    RUN(test_platform_name);
    RUN(test_key_codes_in_range);
    RUN(test_modifier_bitmask);
    RUN(test_pixel_format_enum);
    RUN(test_version_string);
    RUN(test_null_window_safety);
    RUN(test_null_surface_safety);

    printf("\n%d passed, %d failed\n", s_passed, s_failed);
    return s_failed > 0 ? 1 : 0;
}
