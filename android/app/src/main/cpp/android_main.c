#include <crossos/crossos.h>

#include <android/log.h>
#include <android_native_app_glue.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "CrossOSHello"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Internal entry point exposed by CrossOS Android backend. */
extern void crossos_android_set_app(struct android_app *app);

typedef struct test_result {
    const char *title;
    int ok;
    char detail[128];
} test_result_t;

typedef struct app_state {
    crossos_surface_t *surf;
    int running;
    test_result_t tests[3];
} app_state_t;

static void put_pixel(const crossos_framebuffer_t *fb,
                      int x,
                      int y,
                      unsigned char r,
                      unsigned char g,
                      unsigned char b)
{
    if (x < 0 || y < 0 || x >= fb->width || y >= fb->height) {
        return;
    }

    unsigned char *p = (unsigned char *)fb->pixels + y * fb->stride + x * 4;
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = 255;
}

static void fill_rect(const crossos_framebuffer_t *fb,
                      int x,
                      int y,
                      int w,
                      int h,
                      unsigned char r,
                      unsigned char g,
                      unsigned char b)
{
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            put_pixel(fb, xx, yy, r, g, b);
        }
    }
}

static unsigned char glyph_row(char c, int row)
{
    switch (c) {
    case 'A': { static const unsigned char r[7] = {14,17,17,31,17,17,17}; return r[row]; }
    case 'B': { static const unsigned char r[7] = {30,17,17,30,17,17,30}; return r[row]; }
    case 'C': { static const unsigned char r[7] = {14,17,16,16,16,17,14}; return r[row]; }
    case 'D': { static const unsigned char r[7] = {28,18,17,17,17,18,28}; return r[row]; }
    case 'E': { static const unsigned char r[7] = {31,16,16,30,16,16,31}; return r[row]; }
    case 'F': { static const unsigned char r[7] = {31,16,16,30,16,16,16}; return r[row]; }
    case 'G': { static const unsigned char r[7] = {14,17,16,23,17,17,14}; return r[row]; }
    case 'H': { static const unsigned char r[7] = {17,17,17,31,17,17,17}; return r[row]; }
    case 'I': { static const unsigned char r[7] = {14,4,4,4,4,4,14}; return r[row]; }
    case 'K': { static const unsigned char r[7] = {17,18,20,24,20,18,17}; return r[row]; }
    case 'L': { static const unsigned char r[7] = {16,16,16,16,16,16,31}; return r[row]; }
    case 'M': { static const unsigned char r[7] = {17,27,21,21,17,17,17}; return r[row]; }
    case 'N': { static const unsigned char r[7] = {17,25,21,19,17,17,17}; return r[row]; }
    case 'O': { static const unsigned char r[7] = {14,17,17,17,17,17,14}; return r[row]; }
    case 'P': { static const unsigned char r[7] = {30,17,17,30,16,16,16}; return r[row]; }
    case 'Q': { static const unsigned char r[7] = {14,17,17,17,21,18,13}; return r[row]; }
    case 'R': { static const unsigned char r[7] = {30,17,17,30,20,18,17}; return r[row]; }
    case 'S': { static const unsigned char r[7] = {15,16,16,14,1,1,30}; return r[row]; }
    case 'T': { static const unsigned char r[7] = {31,4,4,4,4,4,4}; return r[row]; }
    case 'U': { static const unsigned char r[7] = {17,17,17,17,17,17,14}; return r[row]; }
    case 'W': { static const unsigned char r[7] = {17,17,17,21,21,21,10}; return r[row]; }
    case 'X': { static const unsigned char r[7] = {17,17,10,4,10,17,17}; return r[row]; }
    case 'Y': { static const unsigned char r[7] = {17,17,10,4,4,4,4}; return r[row]; }
    case '/': { static const unsigned char r[7] = {1,2,4,8,16,0,0}; return r[row]; }
    case '.': { static const unsigned char r[7] = {0,0,0,0,0,12,12}; return r[row]; }
    case ':': { static const unsigned char r[7] = {0,12,12,0,12,12,0}; return r[row]; }
    case '[': { static const unsigned char r[7] = {14,8,8,8,8,8,14}; return r[row]; }
    case ']': { static const unsigned char r[7] = {14,2,2,2,2,2,14}; return r[row]; }
    case '-': { static const unsigned char r[7] = {0,0,0,31,0,0,0}; return r[row]; }
    case '0': { static const unsigned char r[7] = {14,17,17,17,17,17,14}; return r[row]; }
    case '1': { static const unsigned char r[7] = {4,12,4,4,4,4,14}; return r[row]; }
    case '2': { static const unsigned char r[7] = {14,17,1,2,4,8,31}; return r[row]; }
    case '3': { static const unsigned char r[7] = {30,1,1,6,1,1,30}; return r[row]; }
    case '4': { static const unsigned char r[7] = {2,6,10,18,31,2,2}; return r[row]; }
    case '5': { static const unsigned char r[7] = {31,16,16,30,1,1,30}; return r[row]; }
    case '6': { static const unsigned char r[7] = {14,16,16,30,17,17,14}; return r[row]; }
    case '7': { static const unsigned char r[7] = {31,1,2,4,8,8,8}; return r[row]; }
    case '8': { static const unsigned char r[7] = {14,17,17,14,17,17,14}; return r[row]; }
    case '9': { static const unsigned char r[7] = {14,17,17,15,1,1,14}; return r[row]; }
    case ' ': return 0;
    default:  { static const unsigned char r[7] = {31,1,2,4,8,0,8}; return r[row]; }
    }
}

static void draw_char(const crossos_framebuffer_t *fb,
                      int x,
                      int y,
                      char c,
                      unsigned char r,
                      unsigned char g,
                      unsigned char b,
                      int scale)
{
    for (int row = 0; row < 7; row++) {
        unsigned char bits = glyph_row(c, row);
        for (int col = 0; col < 5; col++) {
            if ((bits >> (4 - col)) & 1) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        put_pixel(fb,
                                  x + col * scale + sx,
                                  y + row * scale + sy,
                                  r, g, b);
                    }
                }
            }
        }
    }
}

static void draw_text(const crossos_framebuffer_t *fb,
                      int x,
                      int y,
                      const char *text,
                      unsigned char r,
                      unsigned char g,
                      unsigned char b,
                      int scale)
{
    for (int i = 0; text[i] != '\0'; i++) {
        char c = text[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        draw_char(fb, x + i * (6 * scale), y, c, r, g, b, scale);
    }
}

static void draw_background(const crossos_framebuffer_t *fb)
{
    unsigned char *row = (unsigned char *)fb->pixels;
    for (int y = 0; y < fb->height; y++) {
        unsigned char *px = row;
        for (int x = 0; x < fb->width; x++) {
            px[0] = (unsigned char)(40 + x * 60 / (fb->width > 0 ? fb->width : 1));
            px[1] = (unsigned char)(50 + y * 90 / (fb->height > 0 ? fb->height : 1));
            px[2] = 0x18;
            px[3] = 0xFF;
            px += 4;
        }
        row += fb->stride;
    }
}

static void run_tests(app_state_t *state)
{
    state->tests[0].title = "GET HTTP://EXAMPLE.COM";
    state->tests[1].title = "DING SOUND";
    state->tests[2].title = "LIST DIR /SYSTEM/BIN";

    crossos_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    crossos_result_t rc = crossos_http_get("http://example.com", 5000, &resp);
    if (rc == CROSSOS_OK && resp.status_code >= 200 && resp.status_code < 400) {
        state->tests[0].ok = 1;
        snprintf(state->tests[0].detail, sizeof(state->tests[0].detail),
                 "HTTP %ld, %zu BYTES", resp.status_code, resp.body_size);
    } else {
        state->tests[0].ok = 0;
        snprintf(state->tests[0].detail, sizeof(state->tests[0].detail),
                 "FAIL: %s", crossos_get_error());
    }
    crossos_http_response_free(&resp);

    rc = crossos_sound_play_file("tone:ding");
    if (rc == CROSSOS_OK) {
        state->tests[1].ok = 1;
        snprintf(state->tests[1].detail, sizeof(state->tests[1].detail),
                 "PLAYED NOTIFICATION TONE");
    } else {
        state->tests[1].ok = 0;
        snprintf(state->tests[1].detail, sizeof(state->tests[1].detail),
                 "FAIL: %s", crossos_get_error());
    }

    DIR *d = opendir("/system/bin");
    if (!d) {
        state->tests[2].ok = 0;
        snprintf(state->tests[2].detail, sizeof(state->tests[2].detail),
                 "FAIL: CANNOT OPEN DIR");
    } else {
        struct dirent *de;
        int count = 0;
        char first[32] = "";
        char second[32] = "";
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') {
                continue;
            }
            if (count == 0) {
                snprintf(first, sizeof(first), "%s", de->d_name);
            } else if (count == 1) {
                snprintf(second, sizeof(second), "%s", de->d_name);
            }
            count++;
        }
        closedir(d);
        state->tests[2].ok = (count > 0);
        snprintf(state->tests[2].detail, sizeof(state->tests[2].detail),
                 "%d FILES. %s %s", count, first, second);
    }
}

static void render_screen(app_state_t *app)
{
    if (!app->running || !app->surf) {
        return;
    }

    crossos_framebuffer_t fb;
    if (crossos_surface_lock(app->surf, &fb) != CROSSOS_OK) {
        return;
    }

    draw_background(&fb);

    int panel_x = 24;
    int panel_y = 24;
    int panel_w = fb.width - 48;
    int panel_h = fb.height - 48;
    if (panel_w < 120) panel_w = fb.width;
    if (panel_h < 120) panel_h = fb.height;

    fill_rect(&fb, panel_x, panel_y, panel_w, panel_h, 14, 22, 35);

    draw_text(&fb, panel_x + 16, panel_y + 16,
              "CROSSOS SELF TEST DASHBOARD", 230, 238, 245, 2);
    draw_text(&fb, panel_x + 16, panel_y + 42,
              "RUNS: WEB, AUDIO, FILE SYSTEM", 160, 190, 215, 1);

    int y = panel_y + 72;
    for (int i = 0; i < 3; i++) {
        unsigned char rr = app->tests[i].ok ? 36 : 180;
        unsigned char gg = app->tests[i].ok ? 180 : 45;
        unsigned char bb = app->tests[i].ok ? 76 : 45;

        fill_rect(&fb, panel_x + 16, y + 2, 12, 12, rr, gg, bb);
        draw_text(&fb, panel_x + 36, y,
                  app->tests[i].ok ? "[OK]" : "[FAIL]",
                  rr, gg, bb, 1);
        draw_text(&fb, panel_x + 80, y, app->tests[i].title, 232, 236, 240, 1);
        draw_text(&fb, panel_x + 36, y + 14, app->tests[i].detail, 170, 190, 208, 1);
        y += 34;
    }

    draw_text(&fb, panel_x + 16, panel_y + panel_h - 18,
              "TOUCH OR BACK TO EXIT", 150, 170, 188, 1);

    crossos_surface_unlock(app->surf);
    crossos_surface_present(app->surf);
}

static int wait_for_native_window(struct android_app *app)
{
    while (!app->window) {
        int events;
        struct android_poll_source *source = NULL;
        if (ALooper_pollAll(-1, NULL, &events, (void **)&source) >= 0) {
            if (source) {
                source->process(app, source);
            }
        }
        if (app->destroyRequested) {
            LOGE("destroy requested before window was initialized");
            return 0;
        }
    }
    return 1;
}

static void on_event(const crossos_event_t *ev, void *ud)
{
    app_state_t *app = (app_state_t *)ud;

    if (ev->type == CROSSOS_EVENT_QUIT || ev->type == CROSSOS_EVENT_WINDOW_CLOSE) {
        LOGI("quit requested");
        app->running = 0;
        crossos_quit();
    }

    if (ev->type == CROSSOS_EVENT_KEY_DOWN && ev->key.keycode == CROSSOS_KEY_ESCAPE) {
        app->running = 0;
        crossos_quit();
    }

    if (app->running) {
        render_screen(app);
    }
}

void android_main(struct android_app *app)
{
    crossos_android_set_app(app);

    if (!wait_for_native_window(app)) {
        return;
    }

    if (crossos_init() != CROSSOS_OK) {
        LOGE("crossos_init failed: %s", crossos_get_error());
        return;
    }

    crossos_window_t *win = crossos_window_create("CrossOS - Hello", 0, 0, 0);
    if (!win) {
        LOGE("crossos_window_create failed: %s", crossos_get_error());
        crossos_shutdown();
        return;
    }

    crossos_window_show(win);

    app_state_t state;
    memset(&state, 0, sizeof(state));
    state.surf = crossos_surface_get(win);
    state.running = 1;
    run_tests(&state);

    render_screen(&state);

    crossos_run_loop(win, on_event, &state);

    crossos_sound_stop();
    crossos_window_destroy(win);
    crossos_shutdown();
}
