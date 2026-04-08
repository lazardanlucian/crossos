/**
 * applications/easy_raw/main.c
 *
 * EasyRaw — a non-destructive RAW photo editor built on the CrossOS SDK.
 *
 * Inspired by RawTherapee and RapidRAW, EasyRaw provides a minimal but
 * complete editing workflow:
 *
 *   • Open RAW files (PPM P6 / 10-bit Bayer .raw) via native file picker
 *   • Live preview with pan & zoom
 *   • Exposure, White Balance, Tone (Blacks/Shadows/Midtones/Highlights/Whites)
 *   • Contrast, Saturation, Vibrance
 *   • Per-channel tone curves (Luma / R / G / B)
 *   • HSL colour mixer (6 colour slices)
 *   • Sharpening & Noise Reduction
 *   • Export processed image as PPM P6
 *
 * Layout:
 *   ┌──────────────────────────────────────────────────────────┐
 *   │  Toolbar (open / export / zoom / fit)                    │ TOPBAR_H
 *   ├──────────────┬───────────────────────────────────────────┤
 *   │              │                                           │
 *   │  Adjustment  │           Preview pane                    │
 *   │  panel       │                                           │
 *   │  (PANEL_W)   │                                           │
 *   │              │                                           │
 *   ├──────────────┴───────────────────────────────────────────┤
 *   │  Status bar                                              │ STATUSBAR_H
 *   └──────────────────────────────────────────────────────────┘
 *
 * Build:
 *   cmake -B build -DCROSSOS_BUILD_EXAMPLES=ON && cmake --build build
 *   ./build/easy_raw
 */

#include <crossos/crossos.h>

#include "raw_decode.h"
#include "raw_process.h"

#define ER_PNG_WRITE_IMPLEMENTATION
#include "../../src/thirdparty/er_png_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/select.h>
#include <sys/time.h>
#include <pthread.h>
#endif

/* ══════════════════════════════════════════════════════════════════════════
 *  Layout constants
 * ══════════════════════════════════════════════════════════════════════════ */

#define WIN_W 1280
#define WIN_H 800
#define PANEL_W 310
#define TOPBAR_H 40
#define STATUSBAR_H 24

#define PREVIEW_X PANEL_W
#define PREVIEW_Y TOPBAR_H
#define PREVIEW_W (WIN_W - PANEL_W)
#define PREVIEW_H (WIN_H - TOPBAR_H - STATUSBAR_H)

/* Panel layout */
#define P_PAD 12
#define P_ROW_H 22
#define P_LABEL_H 14
#define P_BTN_H 24
#define P_SLIDER_H 16
#define P_RADIUS 4

/* ══════════════════════════════════════════════════════════════════════════
 *  Colour palette  (dark photo-editing theme, blue-grey tint)
 * ══════════════════════════════════════════════════════════════════════════ */

#define COL_BG ((crossos_color_t){0x1A, 0x1C, 0x22, 0xFF})
#define COL_PANEL ((crossos_color_t){0x20, 0x22, 0x2A, 0xFF})
#define COL_TOPBAR ((crossos_color_t){0x14, 0x16, 0x1E, 0xFF})
#define COL_STATUS ((crossos_color_t){0x12, 0x14, 0x1A, 0xFF})
#define COL_BORDER ((crossos_color_t){0x34, 0x36, 0x46, 0xFF})
#define COL_ACCENT ((crossos_color_t){0x4A, 0xA0, 0xE8, 0xFF})  /* sky-blue */
#define COL_ACCENT2 ((crossos_color_t){0x64, 0xC8, 0x80, 0xFF}) /* green    */
#define COL_BTN ((crossos_color_t){0x2C, 0x2E, 0x3A, 0xFF})
#define COL_BTN_SEL ((crossos_color_t){0x4A, 0xA0, 0xE8, 0xFF})
#define COL_BTN_HOT ((crossos_color_t){0x3C, 0x3E, 0x52, 0xFF})
#define COL_TEXT ((crossos_color_t){0xDC, 0xDE, 0xEA, 0xFF})
#define COL_TEXT_DIM ((crossos_color_t){0x70, 0x72, 0x88, 0xFF})
#define COL_TEXT_DARK ((crossos_color_t){0x10, 0x10, 0x18, 0xFF})
#define COL_SLIDER_BG ((crossos_color_t){0x2C, 0x2E, 0x3A, 0xFF})
#define COL_SLIDER_FG ((crossos_color_t){0x4A, 0xA0, 0xE8, 0xFF})
#define COL_CURVE_R ((crossos_color_t){0xFF, 0x55, 0x55, 0xFF})
#define COL_CURVE_G ((crossos_color_t){0x55, 0xCC, 0x55, 0xFF})
#define COL_CURVE_B ((crossos_color_t){0x55, 0x88, 0xFF, 0xFF})
#define COL_CURVE_L ((crossos_color_t){0xCC, 0xCC, 0xCC, 0xFF})
#define COL_PREVIEW_BG ((crossos_color_t){0x0E, 0x0E, 0x14, 0xFF})

/* ══════════════════════════════════════════════════════════════════════════
 *  Panel sections (tabs)
 * ══════════════════════════════════════════════════════════════════════════ */

typedef enum
{
    TAB_EXPOSURE = 0,
    TAB_COLOR,
    TAB_DETAIL,
    TAB_CURVES,
    TAB_HSL,
    TAB_COUNT
} tab_t;

static const char *k_tab_labels[TAB_COUNT] = {
    "Exposure", "Color", "Detail", "Curves", "HSL"};

/* ══════════════════════════════════════════════════════════════════════════
 *  Background processing worker
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct
{
    pthread_t tid;
    pthread_mutex_t mu;
    pthread_cond_t cond;
    int shutdown;
    int request_id;            /**< bumped by main for each new job    */
    int result_id;             /**< set by worker when a job completes */
    er_params_t params;        /**< latest params snapshot             */
    const er_raw_image_t *raw; /**< pointer to raw image (owned by main) */
    er_output_t output;        /**< latest result buffer               */
} worker_t;

/* ══════════════════════════════════════════════════════════════════════════
 *  Curve editor interaction context  (set each frame during render)
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct
{
    int visible;       /**< 1 = editor was drawn this frame        */
    int x, y, w, h;    /**< pixel bbox of the curve editor widget  */
    er_curve_t *curve; /**< which curve is being edited            */
    int drag_pt;       /**< index of grabbed control point, or -1 */
} curve_ctx_t;

static curve_ctx_t g_curve_ctx;

/* ══════════════════════════════════════════════════════════════════════════
 *  Application state
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct
{
    /* Platform */
    crossos_window_t *win;
    crossos_surface_t *surf;
    crossos_typeface_t *font;
    crossos_typeface_t *font_small;
    int running;

    /* RAW image */
    er_raw_image_t raw; /* decoded float planes           */
    int has_image;
    char filename[256];

    /* Processing */
    er_params_t params;
    er_output_t processed; /* last processed RGBA8888 output */
    int dirty;             /* 1 = needs reprocess            */

    /* Preview */
    float zoom; /* 1.0 = fit,  2.0 = 200%, etc. */
    int pan_x;  /* pixel offset of image origin in preview pane */
    int pan_y;
    int panning; /* 1 = RMB drag in progress */
    float pan_start_x, pan_start_y;
    int pan_origin_x, pan_origin_y;

    /* UI state */
    tab_t active_tab;
    int dragging_slider; /* 1 = a slider is being dragged  */
    float *drag_target;  /* pointer to the float being changed */
    float drag_min, drag_max;
    int drag_slider_x; /* x-coord of slider track start  */
    int drag_slider_w; /* width of slider track          */

    /* Active curve editor */
    int curve_channel;     /* 0=Luma 1=R 2=G 3=B             */
    int dragging_curve_pt; /* index of grabbed control point  */

    /* Mouse */
    float mouse_x, mouse_y;
    int mouse_down; /* LMB                             */
    int rmb_down;   /* RMB                             */

    /* Status */
    char status_msg[128];

    /* Panel scroll */
    int panel_scroll;

    /* Export progress */
    int exporting;

    /* Background processing worker */
    worker_t worker;
    int worker_running;
    int last_result_id; /**< result_id of the buffer in app->processed */
    int processing;     /**< 1 = worker has an in-flight job           */

    /* ── Undo / redo ── */
#define UNDO_SIZE 32
    er_params_t undo_stack[UNDO_SIZE];
    int undo_head;  /**< index of the current (latest) entry              */
    int undo_count; /**< number of valid entries in the ring              */
    int undo_pos;   /**< position relative to head (0 = tip, 1 = one back) */

    /* ── Before / after ── */
    er_params_t params_before; /**< snapshot taken at load time              */
    int show_before;           /**< 1 = preview with params_before           */

    /* ── Preset ── */
    char last_preset_path[512]; /**< last saved/loaded preset file path       */
} app_t;

/* Forward declarations for P3 helpers (defined later, used in draw_panel_*) */
static void undo_push(app_t *app);
static void handle_undo(app_t *app);
static void handle_redo(app_t *app);
static void toggle_before(app_t *app, int on);
static void handle_save_preset(app_t *app);
static void handle_load_preset(app_t *app);

/* ══════════════════════════════════════════════════════════════════════════
 *  Utility
 * ══════════════════════════════════════════════════════════════════════════ */

static int rect_hit(float mx, float my, int x, int y, int w, int h)
{
    return mx >= (float)x && mx < (float)(x + w) &&
           my >= (float)y && my < (float)(y + h);
}

static void sleep_ms(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    select(0, NULL, NULL, NULL, &tv);
#endif
}

static void draw_label(const crossos_framebuffer_t *fb, crossos_typeface_t *font,
                       int x, int y, const char *text, crossos_color_t col)
{
    if (font)
        crossos_typeface_draw_text(fb, font, x, y, text, 12.0f, col);
    else
        crossos_draw_text(fb, x, y, text, col, 1);
}

static void draw_label_sm(const crossos_framebuffer_t *fb, crossos_typeface_t *font,
                          int x, int y, const char *text, crossos_color_t col)
{
    if (font)
        crossos_typeface_draw_text(fb, font, x, y, text, 10.0f, col);
    else
        crossos_draw_text(fb, x, y, text, col, 1);
}

/* Draw a toolbar / panel button. Returns 1 if hovered. */
static int draw_button(const crossos_framebuffer_t *fb, crossos_typeface_t *font,
                       int x, int y, int w, int h, const char *label,
                       int selected, int hovered)
{
    crossos_color_t bg = selected  ? COL_BTN_SEL
                         : hovered ? COL_BTN_HOT
                                   : COL_BTN;
    crossos_color_t txt = selected ? COL_TEXT_DARK : COL_TEXT;
    crossos_draw_fill_rounded_rect(fb, x, y, w, h, P_RADIUS, bg);
    crossos_draw_stroke_rounded_rect(fb, x, y, w, h, P_RADIUS, 1, COL_BORDER);
    int lw = font ? (int)(strlen(label) * 7) : crossos_draw_text_width(label, 1);
    int tx = x + (w - lw) / 2;
    int ty = y + (h - (font ? 12 : 7)) / 2;
    draw_label(fb, font, tx, ty, label, txt);
    (void)hovered;
    return 0;
}

/* ── Slider ─────────────────────────────────────────────────────────── */
/*
 * Draws a horizontal slider for value *val in [vmin, vmax].
 * Returns 1 if LMB was pressed inside the slider (start drag).
 */
static int draw_slider(const crossos_framebuffer_t *fb, crossos_typeface_t *font,
                       int x, int y, int w,
                       float val, float vmin, float vmax,
                       const char *label, float mx, float my, int lmb_pressed)
{
    /* Label + value bubble */
    draw_label(fb, font, x, y, label, COL_TEXT_DIM);

    char val_buf[16];
    snprintf(val_buf, sizeof(val_buf), "%.1f", (double)val);
    int vw = font ? (int)(strlen(val_buf) * 7) : crossos_draw_text_width(val_buf, 1);
    draw_label(fb, font, x + w - vw, y, val_buf, COL_TEXT);

    /* Track */
    int ty = y + P_LABEL_H + 2;
    crossos_draw_fill_rounded_rect(fb, x, ty, w, P_SLIDER_H, P_SLIDER_H / 2, COL_SLIDER_BG);

    /* Filled portion */
    float t = (vmax > vmin) ? (val - vmin) / (vmax - vmin) : 0.0f;
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    int fill_w = (int)(t * (float)w);
    if (fill_w > 0)
        crossos_draw_fill_rounded_rect(fb, x, ty, fill_w, P_SLIDER_H, P_SLIDER_H / 2, COL_SLIDER_FG);

    /* Thumb */
    int thumb_x = x + fill_w;
    crossos_draw_fill_circle(fb, thumb_x, ty + P_SLIDER_H / 2, P_SLIDER_H / 2 + 1, COL_ACCENT);

    /* Hit test */
    int hovered = rect_hit(mx, my, x, ty - 4, w, P_SLIDER_H + 8);
    if (lmb_pressed && hovered)
        return 1;
    return 0;
}

/* ── Histogram strip ────────────────────────────────────────────────── */

static void draw_histogram(const crossos_framebuffer_t *fb,
                           const er_output_t *img,
                           int x, int y, int w, int h)
{
    if (!img || !img->pixels)
    {
        crossos_draw_fill_rect(fb, x, y, w, h, COL_SLIDER_BG);
        return;
    }

    uint32_t hist_r[256] = {0}, hist_g[256] = {0}, hist_b[256] = {0};
    int n = img->width * img->height;
    for (int i = 0; i < n; i++)
    {
        hist_r[img->pixels[i * 4 + 0]]++;
        hist_g[img->pixels[i * 4 + 1]]++;
        hist_b[img->pixels[i * 4 + 2]]++;
    }

    /* Find max across all channels for scaling */
    uint32_t peak = 1;
    for (int b = 1; b < 255; b++)
    { /* skip 0 and 255 – often spike */
        if (hist_r[b] > peak)
            peak = hist_r[b];
        if (hist_g[b] > peak)
            peak = hist_g[b];
        if (hist_b[b] > peak)
            peak = hist_b[b];
    }

    crossos_draw_fill_rect(fb, x, y, w, h, COL_SLIDER_BG);

    /* Draw one column per bucket, blend RGB */
    for (int bk = 0; bk < 256; bk++)
    {
        int cx = x + bk * w / 256;
        int cw = w / 256 + 1;

        float fr = (float)hist_r[bk] / (float)peak;
        float fg = (float)hist_g[bk] / (float)peak;
        float fb2 = (float)hist_b[bk] / (float)peak;
        if (fr > 1.0f)
            fr = 1.0f;
        if (fg > 1.0f)
            fg = 1.0f;
        if (fb2 > 1.0f)
            fb2 = 1.0f;

        if (fr > 0.001f)
        {
            int bar_h = (int)(fr * (float)h);
            crossos_draw_fill_rect(fb, cx, y + h - bar_h, cw, bar_h,
                                   (crossos_color_t){0xFF, 0x55, 0x55, 0x88});
        }
        if (fg > 0.001f)
        {
            int bar_h = (int)(fg * (float)h);
            crossos_draw_fill_rect(fb, cx, y + h - bar_h, cw, bar_h,
                                   (crossos_color_t){0x55, 0xCC, 0x55, 0x88});
        }
        if (fb2 > 0.001f)
        {
            int bar_h = (int)(fb2 * (float)h);
            crossos_draw_fill_rect(fb, cx, y + h - bar_h, cw, bar_h,
                                   (crossos_color_t){0x55, 0x88, 0xFF, 0x88});
        }
    }

    crossos_draw_stroke_rect(fb, x, y, w, h, 1, COL_BORDER);
}

/* ── Tone curve editor ──────────────────────────────────────────────── */

static void draw_curve_editor(const crossos_framebuffer_t *fb, crossos_typeface_t *font,
                              er_curve_t *curve, crossos_color_t line_col,
                              int x, int y, int w, int h,
                              float mx, float my)
{
    /* Background */
    crossos_draw_fill_rect(fb, x, y, w, h, COL_SLIDER_BG);
    crossos_draw_stroke_rect(fb, x, y, w, h, 1, COL_BORDER);

    /* Grid */
    crossos_color_t grid_col = {0x34, 0x36, 0x46, 0xFF};
    for (int q = 1; q < 4; q++)
    {
        int gx = x + q * w / 4;
        int gy = y + q * h / 4;
        crossos_draw_line(fb, gx, y, gx, y + h, grid_col);
        crossos_draw_line(fb, x, gy, x + w, gy, grid_col);
    }

    /* Identity diagonal */
    crossos_draw_line(fb, x, y + h, x + w, y, COL_BORDER);

    /* Curve segments */
    if (curve->count >= 2)
    {
        for (int seg = 0; seg < curve->count - 1; seg++)
        {
            int x0c = x + (int)(curve->pts[seg].in * (float)w);
            int y0c = y + h - (int)(curve->pts[seg].out * (float)h);
            int x1c = x + (int)(curve->pts[seg + 1].in * (float)w);
            int y1c = y + h - (int)(curve->pts[seg + 1].out * (float)h);
            /* Draw 8 line segments approximating this piece */
            for (int s = 0; s < 8; s++)
            {
                float t0 = (float)s / 8.0f, t1 = (float)(s + 1) / 8.0f;
                crossos_draw_line(fb,
                                  x0c + (int)(t0 * (x1c - x0c)), y0c + (int)(t0 * (y1c - y0c)),
                                  x0c + (int)(t1 * (x1c - x0c)), y0c + (int)(t1 * (y1c - y0c)),
                                  line_col);
            }
        }
    }

    /* Control points */
    for (int i = 0; i < curve->count; i++)
    {
        int px = x + (int)(curve->pts[i].in * (float)w);
        int py = y + h - (int)(curve->pts[i].out * (float)h);
        int is_drag = (g_curve_ctx.drag_pt == i);
        crossos_color_t pt_col = is_drag ? COL_TEXT : COL_ACCENT;
        crossos_draw_fill_circle(fb, px, py, is_drag ? 7 : 5, pt_col);
        crossos_draw_stroke_rect(fb, px - (is_drag ? 7 : 5), py - (is_drag ? 7 : 5),
                                 (is_drag ? 15 : 11), (is_drag ? 15 : 11), 1, COL_BORDER);
    }
    /* Expose bbox to event handler */
    g_curve_ctx.visible = 1;
    g_curve_ctx.x = x;
    g_curve_ctx.y = y;
    g_curve_ctx.w = w;
    g_curve_ctx.h = h;
    g_curve_ctx.curve = curve;
    (void)font;
    (void)mx;
    (void)my;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Panel drawing helpers
 * ══════════════════════════════════════════════════════════════════════════ */

/* Draw one labelled slider; returns updated y. */
static int panel_slider(const crossos_framebuffer_t *fb, crossos_typeface_t *font,
                        int x, int y, int w,
                        float val, float vmin, float vmax,
                        const char *label, float mx, float my, int pressed)
{
    draw_slider(fb, font, x, y, w, val, vmin, vmax, label, mx, my, pressed);
    return y + P_LABEL_H + 2 + P_SLIDER_H + P_PAD;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Panel rendering (per tab)
 * ══════════════════════════════════════════════════════════════════════════ */

static void draw_panel_exposure(const crossos_framebuffer_t *fb,
                                crossos_typeface_t *font, app_t *app,
                                int x, int y, int w,
                                float mx, float my, int pressed)
{
    int slw = w - P_PAD * 2;

    y = panel_slider(fb, font, x + P_PAD, y, slw,
                     app->params.exposure_ev, -5.0f, 5.0f,
                     "Exposure (EV)", mx, my, pressed);
    y = panel_slider(fb, font, x + P_PAD, y, slw,
                     app->params.blacks, -100.0f, 100.0f,
                     "Blacks", mx, my, pressed);
    y = panel_slider(fb, font, x + P_PAD, y, slw,
                     app->params.shadows, -100.0f, 100.0f,
                     "Shadows", mx, my, pressed);
    y = panel_slider(fb, font, x + P_PAD, y, slw,
                     app->params.midtones, -100.0f, 100.0f,
                     "Midtones", mx, my, pressed);
    y = panel_slider(fb, font, x + P_PAD, y, slw,
                     app->params.highlights, -100.0f, 100.0f,
                     "Highlights", mx, my, pressed);
    y = panel_slider(fb, font, x + P_PAD, y, slw,
                     app->params.whites, -100.0f, 100.0f,
                     "Whites", mx, my, pressed);
    y = panel_slider(fb, font, x + P_PAD, y, slw,
                     app->params.contrast, -100.0f, 100.0f,
                     "Contrast", mx, my, pressed);

    /* Histogram */
    draw_label(fb, font, x + P_PAD, y, "Histogram", COL_TEXT_DIM);
    y += P_LABEL_H + 2;
    draw_histogram(fb, &app->processed, x + P_PAD, y, slw, 60);
}

static void draw_panel_color(const crossos_framebuffer_t *fb,
                             crossos_typeface_t *font, app_t *app,
                             int x, int y, int w,
                             float mx, float my, int pressed)
{
    int slw = w - P_PAD * 2;
    y = panel_slider(fb, font, x + P_PAD, y, slw,
                     app->params.wb_temp, 2000.0f, 10000.0f,
                     "WB Temperature (K)", mx, my, pressed);
    y = panel_slider(fb, font, x + P_PAD, y, slw,
                     app->params.wb_tint, -100.0f, 100.0f,
                     "WB Tint", mx, my, pressed);
    y = panel_slider(fb, font, x + P_PAD, y, slw,
                     app->params.vibrance, -100.0f, 100.0f,
                     "Vibrance", mx, my, pressed);
    y = panel_slider(fb, font, x + P_PAD, y, slw,
                     app->params.saturation, -100.0f, 100.0f,
                     "Saturation", mx, my, pressed);
}

static void draw_panel_detail(const crossos_framebuffer_t *fb,
                              crossos_typeface_t *font, app_t *app,
                              int x, int y, int w,
                              float mx, float my, int pressed)
{
    int slw = w - P_PAD * 2;
    y = panel_slider(fb, font, x + P_PAD, y, slw,
                     app->params.highlight_recovery, 0.0f, 1.0f,
                     "Highlight Recovery", mx, my, pressed);
    y = panel_slider(fb, font, x + P_PAD, y, slw,
                     app->params.sharpening, 0.0f, 1.0f,
                     "Sharpening", mx, my, pressed);
    y = panel_slider(fb, font, x + P_PAD, y, slw,
                     app->params.noise_reduction, 0.0f, 1.0f,
                     "Noise Reduction", mx, my, pressed);
    (void)y;
}

static void draw_panel_curves(const crossos_framebuffer_t *fb,
                              crossos_typeface_t *font, app_t *app,
                              int x, int y, int w,
                              float mx, float my, int pressed)
{
    int slw = w - P_PAD * 2;

    /* Channel selector */
    static const char *ch_labels[4] = {"Luma", "Red", "Green", "Blue"};
    int btn_w = slw / 4 - 2;
    for (int c = 0; c < 4; c++)
    {
        int bx = x + P_PAD + c * (btn_w + 2);
        int hov = rect_hit(mx, my, bx, y, btn_w, P_BTN_H);
        if (hov && pressed)
            app->curve_channel = c;
        draw_button(fb, font, bx, y, btn_w, P_BTN_H,
                    ch_labels[c], app->curve_channel == c, hov);
    }
    y += P_BTN_H + P_PAD;

    er_curve_t *cv = (app->curve_channel == 0)   ? &app->params.curve_luma
                     : (app->curve_channel == 1) ? &app->params.curve_r
                     : (app->curve_channel == 2) ? &app->params.curve_g
                                                 : &app->params.curve_b;
    crossos_color_t col = (app->curve_channel == 0)   ? COL_CURVE_L
                          : (app->curve_channel == 1) ? COL_CURVE_R
                          : (app->curve_channel == 2) ? COL_CURVE_G
                                                      : COL_CURVE_B;
    draw_curve_editor(fb, font, cv, col,
                      x + P_PAD, y, slw, slw,
                      mx, my);
    y += slw + P_PAD;

    /* Reset curve button */
    int rbx = x + P_PAD;
    int hov = rect_hit(mx, my, rbx, y, 80, P_BTN_H);
    if (hov && pressed)
    {
        undo_push(app);
        cv->count = 2;
        cv->pts[0].in = 0.0f;
        cv->pts[0].out = 0.0f;
        cv->pts[1].in = 1.0f;
        cv->pts[1].out = 1.0f;
        app->dirty = 1;
    }
    draw_button(fb, font, rbx, y, 80, P_BTN_H, "Reset", 0, hov);
    (void)slw;
    (void)pressed;
}

static const char *k_hsl_slices[6] = {"Red", "Orange", "Yellow", "Green", "Cyan", "Blue"};

static void draw_panel_hsl(const crossos_framebuffer_t *fb,
                           crossos_typeface_t *font, app_t *app,
                           int x, int y, int w,
                           float mx, float my, int pressed)
{
    int slw = w - P_PAD * 2;
    for (int c = 0; c < 6; c++)
    {
        /* Section header */
        draw_label(fb, font, x + P_PAD, y, k_hsl_slices[c], COL_ACCENT);
        y += P_LABEL_H + 4;
        y = panel_slider(fb, font, x + P_PAD, y, slw,
                         app->params.hsl_hue_shift[c], -180.0f, 180.0f,
                         "Hue", mx, my, pressed);
        y = panel_slider(fb, font, x + P_PAD, y, slw,
                         app->params.hsl_sat_shift[c], -100.0f, 100.0f,
                         "Sat", mx, my, pressed);
        y = panel_slider(fb, font, x + P_PAD, y, slw,
                         app->params.hsl_lum_shift[c], -100.0f, 100.0f,
                         "Lum", mx, my, pressed);
        y += 2;
        crossos_draw_line(fb, x + P_PAD, y, x + w - P_PAD, y, COL_BORDER);
        y += P_PAD;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Slider hit-testing for adjustment interaction
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * Check whether a slider is being grabbed.  Matches layout in draw_panel_*.
 * When a grab is detected, app->dragging_slider is set along with
 * drag_target / drag_min / drag_max / drag_slider_x / drag_slider_w.
 *
 * This is a minimal implementation: we track the position of each slider
 * drawn and test against the current mouse position.
 */

typedef struct
{
    float *target;
    float vmin, vmax;
    int x, y, w;
} slider_desc_t;

#define SLIDER_DESCS_MAX 30

typedef struct
{
    slider_desc_t descs[SLIDER_DESCS_MAX];
    int count;
    int panel_x;
    int slider_w;
} slider_registry_t;

static slider_registry_t g_sliders;

/* Register a slider for hit-testing. */
static void reg_slider(float *target, float vmin, float vmax,
                       int x, int y, int w)
{
    if (g_sliders.count >= SLIDER_DESCS_MAX)
        return;
    slider_desc_t *d = &g_sliders.descs[g_sliders.count++];
    d->target = target;
    d->vmin = vmin;
    d->vmax = vmax;
    d->x = x;
    d->y = y + P_LABEL_H + 2;
    d->w = w;
}

/* Test all registered sliders; activate the first hit. */
static int test_sliders(app_t *app, float mx, float my)
{
    for (int i = 0; i < g_sliders.count; i++)
    {
        slider_desc_t *d = &g_sliders.descs[i];
        int ty = d->y;
        if (rect_hit(mx, my, d->x - 4, ty - 4, d->w + 8, P_SLIDER_H + 8))
        {
            app->dragging_slider = 1;
            app->drag_target = d->target;
            app->drag_min = d->vmin;
            app->drag_max = d->vmax;
            app->drag_slider_x = d->x;
            app->drag_slider_w = d->w;
            return 1;
        }
    }
    return 0;
}

/* Update drag_target value based on mouse x. */
static void update_slider_drag(app_t *app, float mx)
{
    float t = ((float)mx - (float)app->drag_slider_x) / (float)app->drag_slider_w;
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    *app->drag_target = app->drag_min + t * (app->drag_max - app->drag_min);
    app->dirty = 1;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Preview rendering
 * ══════════════════════════════════════════════════════════════════════════ */

static void draw_preview(const crossos_framebuffer_t *fb, app_t *app)
{
    /* Fill preview area background */
    crossos_draw_fill_rect(fb, PREVIEW_X, PREVIEW_Y, PREVIEW_W, PREVIEW_H,
                           COL_PREVIEW_BG);

    if (!app->has_image || !app->processed.pixels)
    {
        /* Drop-zone hint */
        const char *hint = "Open a RAW file to begin";
        int tw = crossos_draw_text_width(hint, 1);
        crossos_draw_text(fb,
                          PREVIEW_X + (PREVIEW_W - tw) / 2,
                          PREVIEW_Y + PREVIEW_H / 2,
                          hint, COL_TEXT_DIM, 1);
        return;
    }

    er_output_t *img = &app->processed;
    int iw = img->width, ih = img->height;

    /* Fit zoom if pan_x/pan_y haven't been set yet */
    float fit_scale = (float)PREVIEW_W / (float)iw;
    if ((float)ih * fit_scale > (float)PREVIEW_H)
        fit_scale = (float)PREVIEW_H / (float)ih;
    float scale = app->zoom * fit_scale;

    int disp_w = (int)((float)iw * scale);
    int disp_h = (int)((float)ih * scale);

    /* Centre + apply pan */
    int origin_x = PREVIEW_X + (PREVIEW_W - disp_w) / 2 + app->pan_x;
    int origin_y = PREVIEW_Y + (PREVIEW_H - disp_h) / 2 + app->pan_y;

    /* Clip to preview area and blit with nearest-neighbour scaling — direct fb write */
    int fb_w = fb->width, fb_h = fb->height;
    int is_bgra = (fb->format == CROSSOS_PIXEL_FMT_BGRA8888);
    uint8_t *fb_pix = (uint8_t *)fb->pixels;
    int fb_stride = fb->stride; /* bytes per row */
    int clip_x0 = PREVIEW_X, clip_y0 = PREVIEW_Y;
    int clip_x1 = PREVIEW_X + PREVIEW_W, clip_y1 = PREVIEW_Y + PREVIEW_H;
    for (int py = 0; py < disp_h; py++)
    {
        int fy = py + origin_y;
        if (fy < clip_y0 || fy >= clip_y1 || fy < 0 || fy >= fb_h)
            continue;
        int src_y = (int)((float)py / scale);
        if (src_y >= ih)
            src_y = ih - 1;
        for (int px = 0; px < disp_w; px++)
        {
            int fx = px + origin_x;
            if (fx < clip_x0 || fx >= clip_x1 || fx < 0 || fx >= fb_w)
                continue;
            int src_x = (int)((float)px / scale);
            if (src_x >= iw)
                src_x = iw - 1;
            const uint8_t *sp = img->pixels + (src_y * iw + src_x) * 4;
            uint8_t *dp = fb_pix + fy * fb_stride + fx * 4;
            if (is_bgra)
            {
                dp[0] = sp[2];
                dp[1] = sp[1];
                dp[2] = sp[0];
                dp[3] = sp[3];
            }
            else
            {
                dp[0] = sp[0];
                dp[1] = sp[1];
                dp[2] = sp[2];
                dp[3] = sp[3];
            }
        }
    }
    (void)fb_w;
    (void)fb_h;

    /* Image border */
    crossos_draw_stroke_rect(fb,
                             origin_x - 1, origin_y - 1,
                             disp_w + 2, disp_h + 2,
                             1, COL_BORDER);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Full frame render
 * ══════════════════════════════════════════════════════════════════════════ */

static void render(app_t *app)
{
    crossos_framebuffer_t fb;
    if (crossos_surface_lock(app->surf, &fb) != CROSSOS_OK)
        return;

    /* Reset per-frame curve context */
    g_curve_ctx.visible = 0;

    /* Background */
    crossos_draw_clear(&fb, COL_BG);

    /* ── Topbar ── */
    crossos_draw_fill_rect(&fb, 0, 0, WIN_W, TOPBAR_H, COL_TOPBAR);
    crossos_draw_line(&fb, 0, TOPBAR_H - 1, WIN_W, TOPBAR_H - 1, COL_BORDER);

    /* App title */
    draw_label(&fb, app->font, P_PAD, (TOPBAR_H - (app->font ? 14 : 7)) / 2,
               "EasyRaw", COL_TEXT);

    /* Toolbar buttons */
    int bx = P_PAD + 80;
    int by = (TOPBAR_H - P_BTN_H) / 2;
    int b_hov;

    /* Open */
    b_hov = rect_hit(app->mouse_x, app->mouse_y, bx, by, 70, P_BTN_H);
    draw_button(&fb, app->font, bx, by, 70, P_BTN_H, "Open", 0, b_hov);
    bx += 76;

    /* Export */
    b_hov = rect_hit(app->mouse_x, app->mouse_y, bx, by, 70, P_BTN_H);
    draw_button(&fb, app->font, bx, by, 70, P_BTN_H, "Export", 0, b_hov);
    bx += 76;

    /* Undo */
    b_hov = rect_hit(app->mouse_x, app->mouse_y, bx, by, 50, P_BTN_H);
    draw_button(&fb, app->font, bx, by, 50, P_BTN_H, "Undo",
                0, b_hov && app->undo_count >= 2 && app->undo_pos < app->undo_count - 1);
    bx += 56;

    /* Redo */
    b_hov = rect_hit(app->mouse_x, app->mouse_y, bx, by, 50, P_BTN_H);
    draw_button(&fb, app->font, bx, by, 50, P_BTN_H, "Redo",
                0, b_hov && app->undo_pos > 0);
    bx += 56;

    /* Before/After */
    b_hov = rect_hit(app->mouse_x, app->mouse_y, bx, by, 56, P_BTN_H);
    draw_button(&fb, app->font, bx, by, 56, P_BTN_H,
                app->show_before ? "After" : "Before",
                app->show_before, b_hov);
    bx += 62;

    /* Save Preset */
    b_hov = rect_hit(app->mouse_x, app->mouse_y, bx, by, 72, P_BTN_H);
    draw_button(&fb, app->font, bx, by, 72, P_BTN_H, "Save Preset", 0, b_hov);
    bx += 78;

    /* Load Preset */
    b_hov = rect_hit(app->mouse_x, app->mouse_y, bx, by, 72, P_BTN_H);
    draw_button(&fb, app->font, bx, by, 72, P_BTN_H, "Load Preset", 0, b_hov);
    bx += 78;

    /* Fit */
    b_hov = rect_hit(app->mouse_x, app->mouse_y, bx, by, 50, P_BTN_H);
    draw_button(&fb, app->font, bx, by, 50, P_BTN_H, "Fit", app->zoom == 1.0f, b_hov);
    bx += 56;

    /* 100% */
    b_hov = rect_hit(app->mouse_x, app->mouse_y, bx, by, 50, P_BTN_H);
    draw_button(&fb, app->font, bx, by, 50, P_BTN_H, "100%", app->zoom == (1.0f / /* will be divided by fit */ 1.0f), b_hov);
    bx += 56;

    /* Zoom level label */
    char zoom_buf[24];
    snprintf(zoom_buf, sizeof(zoom_buf), "%d%%", (int)(app->zoom * 100.0f));
    draw_label(&fb, app->font, bx + 4, by + 4, zoom_buf, COL_TEXT_DIM);

    /* ── Panel background ── */
    crossos_draw_fill_rect(&fb, 0, TOPBAR_H, PANEL_W, WIN_H - TOPBAR_H, COL_PANEL);
    crossos_draw_line(&fb, PANEL_W, TOPBAR_H, PANEL_W, WIN_H - STATUSBAR_H, COL_BORDER);

    /* ── Tab bar ── */
    int tab_y = TOPBAR_H + 4;
    int tab_w = PANEL_W / TAB_COUNT;
    for (int t = 0; t < TAB_COUNT; t++)
    {
        int tx = t * tab_w;
        int hov = rect_hit(app->mouse_x, app->mouse_y, tx, tab_y, tab_w, P_BTN_H);
        draw_button(&fb, app->font_small, tx, tab_y, tab_w, P_BTN_H,
                    k_tab_labels[t], app->active_tab == (tab_t)t, hov);
    }
    int content_y = tab_y + P_BTN_H + P_PAD;
    int draw_y = content_y - app->panel_scroll;

    /* ── Reset sliders registry for this frame ── */
    memset(&g_sliders, 0, sizeof(g_sliders));

    /* ── Panel content ── */
    crossos_draw_push_clip(0, content_y, PANEL_W, WIN_H - STATUSBAR_H - content_y);
    switch (app->active_tab)
    {
    case TAB_EXPOSURE:
        draw_panel_exposure(&fb, app->font_small, app, 0, draw_y,
                            PANEL_W, app->mouse_x, app->mouse_y, 0);
        reg_slider(&app->params.exposure_ev, -5.0f, 5.0f,
                   P_PAD, draw_y, PANEL_W - P_PAD * 2);
        draw_y += P_LABEL_H + 2 + P_SLIDER_H + P_PAD;
        reg_slider(&app->params.blacks, -100.0f, 100.0f,
                   P_PAD, draw_y, PANEL_W - P_PAD * 2);
        draw_y += P_LABEL_H + 2 + P_SLIDER_H + P_PAD;
        reg_slider(&app->params.shadows, -100.0f, 100.0f,
                   P_PAD, draw_y, PANEL_W - P_PAD * 2);
        draw_y += P_LABEL_H + 2 + P_SLIDER_H + P_PAD;
        reg_slider(&app->params.midtones, -100.0f, 100.0f,
                   P_PAD, draw_y, PANEL_W - P_PAD * 2);
        draw_y += P_LABEL_H + 2 + P_SLIDER_H + P_PAD;
        reg_slider(&app->params.highlights, -100.0f, 100.0f,
                   P_PAD, draw_y, PANEL_W - P_PAD * 2);
        draw_y += P_LABEL_H + 2 + P_SLIDER_H + P_PAD;
        reg_slider(&app->params.whites, -100.0f, 100.0f,
                   P_PAD, draw_y, PANEL_W - P_PAD * 2);
        draw_y += P_LABEL_H + 2 + P_SLIDER_H + P_PAD;
        reg_slider(&app->params.contrast, -100.0f, 100.0f,
                   P_PAD, draw_y, PANEL_W - P_PAD * 2);
        break;
    case TAB_COLOR:
        draw_panel_color(&fb, app->font_small, app, 0, draw_y,
                         PANEL_W, app->mouse_x, app->mouse_y, 0);
        reg_slider(&app->params.wb_temp, 2000.0f, 10000.0f,
                   P_PAD, draw_y, PANEL_W - P_PAD * 2);
        draw_y += P_LABEL_H + 2 + P_SLIDER_H + P_PAD;
        reg_slider(&app->params.wb_tint, -100.0f, 100.0f,
                   P_PAD, draw_y, PANEL_W - P_PAD * 2);
        draw_y += P_LABEL_H + 2 + P_SLIDER_H + P_PAD;
        reg_slider(&app->params.vibrance, -100.0f, 100.0f,
                   P_PAD, draw_y, PANEL_W - P_PAD * 2);
        draw_y += P_LABEL_H + 2 + P_SLIDER_H + P_PAD;
        reg_slider(&app->params.saturation, -100.0f, 100.0f,
                   P_PAD, draw_y, PANEL_W - P_PAD * 2
        break;
    case TAB_DETAIL:
        draw_panel_detail(&fb, app->font_small, app, 0, draw_y,
                          PANEL_W, app->mouse_x, app->mouse_y, 0);
        reg_slider(&app->params.highlight_recovery, 0.0f, 1.0f,
                   P_PAD, draw_y, PANEL_W - P_PAD * 2);
        draw_y += P_LABEL_H + 2 + P_SLIDER_H + P_PAD;
        reg_slider(&app->params.sharpening, 0.0f, 1.0f,
                   P_PAD, draw_y, PANEL_W - P_PAD * 2);
        draw_y += P_LABEL_H + 2 + P_SLIDER_H + P_PAD;
        reg_slider(&app->params.noise_reduction, 0.0f, 1.0f,
                   P_PAD, draw_y, PANEL_W - P_PAD * 2);
        break;
    case TAB_CURVES:
        draw_panel_curves(&fb, app->font_small, app, 0, draw_y,
                          PANEL_W, app->mouse_x, app->mouse_y, 0);
        break;
    case TAB_HSL:
        draw_panel_hsl(&fb, app->font_small, app, 0, draw_y,
                       PANEL_W, app->mouse_x, app->mouse_y, 0);
        break;
    default:
        break;
    }
    crossos_draw_pop_clip();

    /* ── Preview ── */
    draw_preview(&fb, app);

    /* ── Status bar ── */
    crossos_draw_fill_rect(&fb, 0, WIN_H - STATUSBAR_H, WIN_W, STATUSBAR_H, COL_STATUS);
    crossos_draw_line(&fb, 0, WIN_H - STATUSBAR_H, WIN_W, WIN_H - STATUSBAR_H, COL_BORDER);
    if (app->processing)
    {
        draw_label_sm(&fb, app->font_small, P_PAD, WIN_H - STATUSBAR_H + 5,
                      "Processing...", COL_ACCENT);
    }
    else
    {
        draw_label_sm(&fb, app->font_small, P_PAD, WIN_H - STATUSBAR_H + 5,
                      app->status_msg, COL_TEXT_DIM);
    }

    if (app->has_image)
    {
        char info[80];
        snprintf(info, sizeof(info), "%s  %d×%d",
                 app->filename, app->raw.width, app->raw.height);
        int iw2 = crossos_draw_text_width(info, 1);
        draw_label_sm(&fb, app->font_small,
                      WIN_W - iw2 - P_PAD, WIN_H - STATUSBAR_H + 5,
                      info, COL_TEXT_DIM);
    }

    crossos_surface_unlock(app->surf);
    crossos_surface_present(app->surf);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  File export (PPM P6 + PNG)
 * ══════════════════════════════════════════════════════════════════════════ */

static int export_ppm(const er_output_t *img, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", img->width, img->height);
    int n = img->width * img->height;
    for (int i = 0; i < n; i++)
    {
        uint8_t rgb[3] = {img->pixels[i * 4], img->pixels[i * 4 + 1],
                          img->pixels[i * 4 + 2]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}

static int export_png(const er_output_t *img, const char *path)
{
    return er_png_write_rgba(path, img->pixels,
                             img->width, img->height, img->stride);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Worker thread implementation
 * ══════════════════════════════════════════════════════════════════════════ */

static void worker_init(worker_t *w)
{
    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cond, NULL);
}

static void *worker_thread_fn(void *arg)
{
    worker_t *w = (worker_t *)arg;
    int my_id = 0;
    for (;;)
    {
        pthread_mutex_lock(&w->mu);
        while (w->request_id == my_id && !w->shutdown)
            pthread_cond_wait(&w->cond, &w->mu);
        if (w->shutdown)
        {
            pthread_mutex_unlock(&w->mu);
            break;
        }
        my_id = w->request_id;
        er_params_t params = w->params;
        const er_raw_image_t *raw = w->raw;
        pthread_mutex_unlock(&w->mu);

        if (!raw)
            continue;

        er_output_t out = {0};
        er_process(raw, &params, &out);

        pthread_mutex_lock(&w->mu);
        if (w->request_id == my_id && !w->shutdown)
        {
            er_output_free(&w->output);
            w->output = out;
            w->result_id = my_id;
        }
        else
        {
            er_output_free(&out);
        }
        pthread_mutex_unlock(&w->mu);
    }
    return NULL;
}

static void worker_start(worker_t *w)
{
    w->shutdown = 0;
    pthread_create(&w->tid, NULL, worker_thread_fn, w);
}

static void worker_stop(worker_t *w)
{
    pthread_mutex_lock(&w->mu);
    w->shutdown = 1;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mu);
    pthread_join(w->tid, NULL);
}

static void worker_trigger(worker_t *w,
                           const er_raw_image_t *raw,
                           const er_params_t *params)
{
    pthread_mutex_lock(&w->mu);
    w->request_id++;
    w->raw = raw;
    w->params = *params;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mu);
}

/* Swap the latest worker result into *dest.  Returns 1 if fresh. */
static int worker_poll(worker_t *w, int *last_id, er_output_t *dest)
{
    pthread_mutex_lock(&w->mu);
    int fresh = (w->result_id != *last_id);
    if (fresh)
    {
        er_output_t tmp = *dest;
        *dest = w->output;
        w->output = tmp; /* give worker the old buffer to reuse */
        *last_id = w->result_id;
    }
    pthread_mutex_unlock(&w->mu);
    return fresh;
}

static int worker_is_busy(worker_t *w)
{
    pthread_mutex_lock(&w->mu);
    int busy = (w->request_id != w->result_id);
    pthread_mutex_unlock(&w->mu);
    return busy;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Undo / redo
 * ══════════════════════════════════════════════════════════════════════════ */

/* Push the CURRENT params onto the undo stack (call before any edit). */
static void undo_push(app_t *app)
{
    /* Advance head into the ring, discarding any redo future */
    app->undo_head = (app->undo_head + 1) % UNDO_SIZE;
    app->undo_stack[app->undo_head] = app->params;
    if (app->undo_count < UNDO_SIZE)
        app->undo_count++;
    app->undo_pos = 0; /* tip */
}

static void handle_undo(app_t *app)
{
    if (!app->has_image)
        return;
    /* We need at least 2 entries (current + one before) */
    if (app->undo_count < 2 || app->undo_pos >= app->undo_count - 1)
    {
        snprintf(app->status_msg, sizeof(app->status_msg), "Nothing to undo.");
        return;
    }
    app->undo_pos++;
    int idx = ((app->undo_head - app->undo_pos) % UNDO_SIZE + UNDO_SIZE) % UNDO_SIZE;
    app->params = app->undo_stack[idx];
    app->dirty = 1;
    snprintf(app->status_msg, sizeof(app->status_msg), "Undo (%d left)", app->undo_pos);
}

static void handle_redo(app_t *app)
{
    if (!app->has_image || app->undo_pos == 0)
    {
        snprintf(app->status_msg, sizeof(app->status_msg), "Nothing to redo.");
        return;
    }
    app->undo_pos--;
    int idx = ((app->undo_head - app->undo_pos) % UNDO_SIZE + UNDO_SIZE) % UNDO_SIZE;
    app->params = app->undo_stack[idx];
    app->dirty = 1;
    snprintf(app->status_msg, sizeof(app->status_msg),
             app->undo_pos == 0 ? "Redo (at tip)" : "Redo (%d forward)", app->undo_pos);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Before / after
 * ══════════════════════════════════════════════════════════════════════════ */

static void toggle_before(app_t *app, int on)
{
    if (!app->has_image)
        return;
    if (on == app->show_before)
        return;
    app->show_before = on;
    /* Temporarily swap params so the worker produces the right frame */
    if (on)
    {
        worker_trigger(&app->worker, &app->raw, &app->params_before);
    }
    else
    {
        worker_trigger(&app->worker, &app->raw, &app->params);
    }
    app->processing = 1;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Preset save / load
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * Simple key=value text format.  One parameter per line.
 * Only scalar float fields are serialised; curves use a dedicated format
 *   curve_luma = N pt0_in pt0_out pt1_in pt1_out ...
 */
static int preset_save(const er_params_t *p, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;

#define WF(name) fprintf(f, #name " = %g\n", (double)p->name)
    WF(exposure_ev);
    WF(wb_temp);
    WF(wb_tint);
    WF(blacks);
    WF(shadows);
    WF(midtones);
    WF(highlights);
    WF(whites);
    WF(contrast);
    WF(vibrance);
    WF(saturation);
    WF(highlight_recovery);
    WF(sharpening);
    WF(noise_reduction);
    WF(output_gamma);
#undef WF
    for (int c = 0; c < 6; c++)
        fprintf(f, "hsl_hue_shift_%d = %g\n", c, (double)p->hsl_hue_shift[c]);
    for (int c = 0; c < 6; c++)
        fprintf(f, "hsl_sat_shift_%d = %g\n", c, (double)p->hsl_sat_shift[c]);
    for (int c = 0; c < 6; c++)
        fprintf(f, "hsl_lum_shift_%d = %g\n", c, (double)p->hsl_lum_shift[c]);
    /* Curves */
    const er_curve_t *curves[4] = {
        &p->curve_luma, &p->curve_r, &p->curve_g, &p->curve_b};
    const char *cnames[4] = {"curve_luma", "curve_r", "curve_g", "curve_b"};
    for (int ci = 0; ci < 4; ci++)
    {
        fprintf(f, "%s = %d", cnames[ci], curves[ci]->count);
        for (int k = 0; k < curves[ci]->count; k++)
            fprintf(f, " %g %g",
                    (double)curves[ci]->pts[k].in,
                    (double)curves[ci]->pts[k].out);
        fprintf(f, "\n");
    }
    fclose(f);
    return 0;
}

static int preset_load(er_params_t *p, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char key[64];
        double val;
        if (sscanf(line, "%63s = %lf", key, &val) == 2)
        {
#define RF(name)                 \
    if (strcmp(key, #name) == 0) \
    {                            \
        p->name = (float)val;    \
        continue;                \
    }
            RF(exposure_ev)
            RF(wb_temp) RF(wb_tint)
                RF(blacks) RF(shadows) RF(midtones) RF(highlights) RF(whites)
                    RF(contrast) RF(vibrance) RF(saturation)
                        RF(highlight_recovery) RF(sharpening) RF(noise_reduction)
                            RF(output_gamma)
#undef RF
                                for (int c = 0; c < 6; c++)
            {
                char buf[32];
                snprintf(buf, sizeof(buf), "hsl_hue_shift_%d", c);
                if (strcmp(key, buf) == 0)
                {
                    p->hsl_hue_shift[c] = (float)val;
                    break;
                }
                snprintf(buf, sizeof(buf), "hsl_sat_shift_%d", c);
                if (strcmp(key, buf) == 0)
                {
                    p->hsl_sat_shift[c] = (float)val;
                    break;
                }
                snprintf(buf, sizeof(buf), "hsl_lum_shift_%d", c);
                if (strcmp(key, buf) == 0)
                {
                    p->hsl_lum_shift[c] = (float)val;
                    break;
                }
            }
        }
        else
        {
            /* Curve line: "curve_X = N in0 out0 in1 out1 ..." */
            char ckey[32];
            int cnt;
            if (sscanf(line, "%31s = %d", ckey, &cnt) == 2 && cnt >= 2)
            {
                er_curve_t *cv = NULL;
                if (strcmp(ckey, "curve_luma") == 0)
                    cv = &p->curve_luma;
                else if (strcmp(ckey, "curve_r") == 0)
                    cv = &p->curve_r;
                else if (strcmp(ckey, "curve_g") == 0)
                    cv = &p->curve_g;
                else if (strcmp(ckey, "curve_b") == 0)
                    cv = &p->curve_b;
                if (cv)
                {
                    if (cnt > ER_CURVE_MAX_PTS)
                        cnt = ER_CURVE_MAX_PTS;
                    /* Find past the count field to read pairs */
                    const char *p2 = line;
                    /* skip key, '=', count */
                    while (*p2 && *p2 != '=')
                        p2++;
                    if (*p2 == '=')
                        p2++;
                    long count_tmp;
                    char *endp;
                    count_tmp = strtol(p2, &endp, 10);
                    (void)count_tmp;
                    p2 = endp;
                    cv->count = 0;
                    for (int k = 0; k < cnt; k++)
                    {
                        double in_v, out_v;
                        if (sscanf(p2, " %lf %lf", &in_v, &out_v) != 2)
                            break;
                        cv->pts[k].in = (float)in_v;
                        cv->pts[k].out = (float)out_v;
                        cv->count++;
                        /* advance past the two values */
                        char *ep1, *ep2;
                        strtod(p2, &ep1);
                        p2 = ep1;
                        strtod(p2, &ep2);
                        p2 = ep2;
                    }
                }
            }
        }
    }
    fclose(f);
    return 0;
}

static void handle_save_preset(app_t *app)
{
    if (!app->has_image)
    {
        snprintf(app->status_msg, sizeof(app->status_msg), "No image loaded.");
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s.erpreset",
             app->filename[0] ? app->filename : "easyraw");
    if (preset_save(&app->params, path) == 0)
    {
        snprintf(app->last_preset_path, sizeof(app->last_preset_path), "%s", path);
        snprintf(app->status_msg, sizeof(app->status_msg), "Preset saved: %s", path);
    }
    else
    {
        snprintf(app->status_msg, sizeof(app->status_msg), "Preset save failed.");
    }
}

static void handle_load_preset(app_t *app)
{
    crossos_dialog_file_list_t files;
    memset(&files, 0, sizeof(files));
    if (crossos_dialog_pick_files("Load Preset", 0, &files) != CROSSOS_OK)
        return;
    if (files.count == 0)
    {
        crossos_dialog_file_list_free(&files);
        return;
    }

    er_params_t tmp = app->params;
    if (preset_load(&tmp, files.items[0]) == 0)
    {
        undo_push(app);
        app->params = tmp;
        app->dirty = 1;
        snprintf(app->last_preset_path, sizeof(app->last_preset_path),
                 "%.511s", files.items[0]);
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Preset loaded: %.100s", files.items[0]);
    }
    else
    {
        snprintf(app->status_msg, sizeof(app->status_msg), "Preset load failed.");
    }
    crossos_dialog_file_list_free(&files);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Image loading
 * ══════════════════════════════════════════════════════════════════════════ */

static void load_image(app_t *app, const char *path)
{
    /* Stop in-flight processing before touching the raw planes */
    if (app->worker_running)
    {
        worker_stop(&app->worker);
        app->worker_running = 0;
    }
    er_output_free(&app->processed);
    er_raw_image_free(&app->raw);
    memset(&app->raw, 0, sizeof(app->raw));
    app->has_image = 0;

    snprintf(app->status_msg, sizeof(app->status_msg), "Loading...");

    int rc = er_raw_image_load(path, &app->raw);
    if (rc != 0)
    {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Failed to load: %.120s", path);
        return;
    }

    const char *slash = strrchr(path, '/');
    if (!slash)
        slash = strrchr(path, '\\');
    snprintf(app->filename, sizeof(app->filename), "%.255s",
             slash ? slash + 1 : path);

    app->has_image = 1;
    app->pan_x = 0;
    app->pan_y = 0;
    app->zoom = 1.0f;
    app->panel_scroll = 0;
    app->last_result_id = 0;
    app->worker.result_id = 0;
    app->worker.request_id = 0;
    er_params_init(&app->params);
    app->params_before = app->params; /* baseline "before" snapshot */
    app->undo_head = 0;
    app->undo_count = 0;
    app->undo_pos = 0;
    app->show_before = 0;
    undo_push(app); /* push initial state as bottom of stack */

    worker_start(&app->worker);
    app->worker_running = 1;
    worker_trigger(&app->worker, &app->raw, &app->params);
    app->processing = 1;
    app->dirty = 0;

    snprintf(app->status_msg, sizeof(app->status_msg),
             "Loaded %.200s  (%d x %d)",
             app->filename, app->raw.width, app->raw.height);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Event handling
 * ══════════════════════════════════════════════════════════════════════════ */

static void handle_open(app_t *app)
{
    crossos_dialog_file_list_t files;
    memset(&files, 0, sizeof(files));
    if (crossos_dialog_pick_files("Open RAW Image", 0, &files) != CROSSOS_OK)
        return;
    if (files.count > 0)
        load_image(app, files.items[0]);
    crossos_dialog_file_list_free(&files);
}

static void handle_export(app_t *app)
{
    if (!app->has_image)
    {
        snprintf(app->status_msg, sizeof(app->status_msg), "No image loaded.");
        return;
    }
    if (app->worker_running && worker_is_busy(&app->worker))
    {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Still processing, please wait...");
        return;
    }
    char out_path[256];
    snprintf(out_path, sizeof(out_path), "/tmp/easyraw_export.ppm");
    snprintf(app->status_msg, sizeof(app->status_msg), "Exporting...");
    render(app);
    if (export_ppm(&app->processed, out_path) == 0)
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Exported: %s", out_path);
    else
        snprintf(app->status_msg, sizeof(app->status_msg), "Export failed.");
}

/* Toolbar button hit testing */
static void handle_toolbar_click(app_t *app, float mx, float my)
{
    int by = (TOPBAR_H - P_BTN_H) / 2;
    int bx = P_PAD + 80;

    /* Open */
    if (rect_hit(mx, my, bx, by, 70, P_BTN_H))
    {
        handle_open(app);
        return;
    }
    bx += 76;
    /* Export */
    if (rect_hit(mx, my, bx, by, 70, P_BTN_H))
    {
        handle_export(app);
        return;
    }
    bx += 76;
    /* Undo */
    if (rect_hit(mx, my, bx, by, 50, P_BTN_H))
    {
        handle_undo(app);
        return;
    }
    bx += 56;
    /* Redo */
    if (rect_hit(mx, my, bx, by, 50, P_BTN_H))
    {
        handle_redo(app);
        return;
    }
    bx += 56;
    /* Before/After */
    if (rect_hit(mx, my, bx, by, 56, P_BTN_H))
    {
        toggle_before(app, !app->show_before);
        return;
    }
    bx += 62;
    /* Save Preset */
    if (rect_hit(mx, my, bx, by, 72, P_BTN_H))
    {
        handle_save_preset(app);
        return;
    }
    bx += 78;
    /* Load Preset */
    if (rect_hit(mx, my, bx, by, 72, P_BTN_H))
    {
        handle_load_preset(app);
        return;
    }
    bx += 78;
    /* Fit */
    if (rect_hit(mx, my, bx, by, 50, P_BTN_H))
    {
        app->zoom = 1.0f;
        app->pan_x = 0;
        app->pan_y = 0;
        return;
    }
    bx += 56;
    /* 100% */
    if (rect_hit(mx, my, bx, by, 50, P_BTN_H))
    {
        app->zoom = 2.0f;
        app->pan_x = 0;
        app->pan_y = 0;
        return;
    }
}

static void handle_tab_click(app_t *app, float mx, float my)
{
    int tab_y = TOPBAR_H + 4;
    int tab_w = PANEL_W / TAB_COUNT;
    for (int t = 0; t < TAB_COUNT; t++)
    {
        int tx = t * tab_w;
        if (rect_hit(mx, my, tx, tab_y, tab_w, P_BTN_H))
        {
            app->active_tab = (tab_t)t;
            app->panel_scroll = 0;
            return;
        }
    }
}

static void on_event(const crossos_event_t *ev, void *ud)
{
    app_t *app = (app_t *)ud;

    switch (ev->type)
    {
    case CROSSOS_EVENT_QUIT:
    case CROSSOS_EVENT_WINDOW_CLOSE:
        app->running = 0;
        crossos_quit();
        break;

    case CROSSOS_EVENT_POINTER_MOVE:
        app->mouse_x = ev->pointer.x;
        app->mouse_y = ev->pointer.y;

        if (app->dragging_slider)
        {
            update_slider_drag(app, app->mouse_x);
        }
        else if (g_curve_ctx.drag_pt >= 0)
        {
            /* Drag active curve point */
            er_curve_t *cv = g_curve_ctx.curve;
            float nx = (float)(app->mouse_x - g_curve_ctx.x) / (float)g_curve_ctx.w;
            float ny = 1.0f - (float)(app->mouse_y - g_curve_ctx.y) / (float)g_curve_ctx.h;
            if (nx < 0.0f)
                nx = 0.0f;
            if (nx > 1.0f)
                nx = 1.0f;
            if (ny < 0.0f)
                ny = 0.0f;
            if (ny > 1.0f)
                ny = 1.0f;
            int dp = g_curve_ctx.drag_pt;
            /* Clamp in to neighbours */
            if (dp > 0 && nx <= cv->pts[dp - 1].in)
                nx = cv->pts[dp - 1].in + 0.001f;
            if (dp < cv->count - 1 && nx >= cv->pts[dp + 1].in)
                nx = cv->pts[dp + 1].in - 0.001f;
            cv->pts[dp].in = nx;
            cv->pts[dp].out = ny;
            app->dirty = 1;
        }
        if (app->panning)
        {
            app->pan_x = app->pan_origin_x + (int)(app->mouse_x - app->pan_start_x);
            app->pan_y = app->pan_origin_y + (int)(app->mouse_y - app->pan_start_y);
        }
        break;

    case CROSSOS_EVENT_POINTER_DOWN:
        app->mouse_x = ev->pointer.x;
        app->mouse_y = ev->pointer.y;

        if (ev->pointer.button == 1)
        {
            app->mouse_down = 1;

            /* Check topbar */
            if (app->mouse_y < TOPBAR_H)
            {
                handle_toolbar_click(app, app->mouse_x, app->mouse_y);
                break;
            }
            /* Check tab bar */
            if (app->mouse_y < TOPBAR_H + 4 + P_BTN_H + P_PAD &&
                app->mouse_x < PANEL_W)
            {
                handle_tab_click(app, app->mouse_x, app->mouse_y);
                break;
            }
            /* Check curve editor hit */
            if (g_curve_ctx.visible &&
                rect_hit(app->mouse_x, app->mouse_y,
                         g_curve_ctx.x, g_curve_ctx.y,
                         g_curve_ctx.w, g_curve_ctx.h))
            {
                er_curve_t *cv = g_curve_ctx.curve;
                float nx = (float)(app->mouse_x - g_curve_ctx.x) / (float)g_curve_ctx.w;
                float ny = 1.0f - (float)(app->mouse_y - g_curve_ctx.y) / (float)g_curve_ctx.h;
                if (nx < 0.0f)
                    nx = 0.0f;
                if (nx > 1.0f)
                    nx = 1.0f;
                if (ny < 0.0f)
                    ny = 0.0f;
                if (ny > 1.0f)
                    ny = 1.0f;
                /* Find nearest existing point */
                int nearest = -1;
                float nearest_dist = 1e9f;
                for (int i = 0; i < cv->count; i++)
                {
                    float dx = (cv->pts[i].in - nx) * (float)g_curve_ctx.w;
                    float dy = (cv->pts[i].out - ny) * (float)g_curve_ctx.h;
                    float d = dx * dx + dy * dy;
                    if (d < nearest_dist)
                    {
                        nearest_dist = d;
                        nearest = i;
                    }
                }
                if (nearest >= 0 && nearest_dist <= 100.0f)
                { /* 10px radius */
                    g_curve_ctx.drag_pt = nearest;
                }
                else if (cv->count < ER_CURVE_MAX_PTS)
                {
                    /* Insert sorted by in */
                    int ins = cv->count;
                    for (int i = 0; i < cv->count; i++)
                    {
                        if (nx < cv->pts[i].in)
                        {
                            ins = i;
                            break;
                        }
                    }
                    undo_push(app);
                    for (int i = cv->count; i > ins; i--)
                        cv->pts[i] = cv->pts[i - 1];
                    cv->pts[ins].in = nx;
                    cv->pts[ins].out = ny;
                    cv->count++;
                    g_curve_ctx.drag_pt = ins;
                    app->dirty = 1;
                }
                break;
            }
            /* Check panel sliders */
            if (app->mouse_x < PANEL_W)
            {
                if (test_sliders(app, app->mouse_x, app->mouse_y))
                {
                    update_slider_drag(app, app->mouse_x);
                }
            }
        }
        else if (ev->pointer.button == 3)
        {
            /* Right button — delete curve point if over editor, else pan */
            if (g_curve_ctx.visible &&
                rect_hit(app->mouse_x, app->mouse_y,
                         g_curve_ctx.x, g_curve_ctx.y,
                         g_curve_ctx.w, g_curve_ctx.h))
            {
                er_curve_t *cv = g_curve_ctx.curve;
                if (cv->count > 2)
                {
                    float nx = (float)(app->mouse_x - g_curve_ctx.x) / (float)g_curve_ctx.w;
                    float ny = 1.0f - (float)(app->mouse_y - g_curve_ctx.y) / (float)g_curve_ctx.h;
                    int nearest = -1;
                    float nearest_dist = 1e9f;
                    for (int i = 0; i < cv->count; i++)
                    {
                        float dx = (cv->pts[i].in - nx) * (float)g_curve_ctx.w;
                        float dy = (cv->pts[i].out - ny) * (float)g_curve_ctx.h;
                        float d = dx * dx + dy * dy;
                        if (d < nearest_dist)
                        {
                            nearest_dist = d;
                            nearest = i;
                        }
                    }
                    if (nearest >= 0 && nearest_dist <= 100.0f)
                    {
                        undo_push(app);
                        for (int i = nearest; i < cv->count - 1; i++)
                            cv->pts[i] = cv->pts[i + 1];
                        cv->count--;
                        app->dirty = 1;
                    }
                }
                break;
            }
            app->rmb_down = 1;
            app->panning = 1;
            app->pan_start_x = ev->pointer.x;
            app->pan_start_y = ev->pointer.y;
            app->pan_origin_x = app->pan_x;
            app->pan_origin_y = app->pan_y;
        }
        break;

    case CROSSOS_EVENT_POINTER_UP:
        if (ev->pointer.button == 1)
        {
            /* Push undo snapshot when finishing a slider drag */
            if (app->dragging_slider)
                undo_push(app);
            app->mouse_down = 0;
            app->dragging_slider = 0;
            /* Push undo snapshot when finishing a curve drag */
            if (g_curve_ctx.drag_pt >= 0)
                undo_push(app);
            g_curve_ctx.drag_pt = -1;
        }
        else if (ev->pointer.button == 3)
        {
            app->rmb_down = 0;
            app->panning = 0;
        }
        break;

    case CROSSOS_EVENT_POINTER_SCROLL:
        if (app->mouse_x > PANEL_W)
        {
            /* Scroll in preview = adjust zoom */
            app->zoom += ev->pointer.scroll_y * 0.1f;
            if (app->zoom < 0.1f)
                app->zoom = 0.1f;
            if (app->zoom > 8.0f)
                app->zoom = 8.0f;
        }
        else
        {
            /* Scroll in panel = scroll panel content */
            app->panel_scroll -= (int)(ev->pointer.scroll_y * 30);
            if (app->panel_scroll < 0)
                app->panel_scroll = 0;
            if (app->panel_scroll > 1000)
                app->panel_scroll = 1000;
        }
        break;

    case CROSSOS_EVENT_DROP_FILES:
        if (ev->drop.count > 0)
            load_image(app, ev->drop.paths[0]);
        break;

    case CROSSOS_EVENT_KEY_DOWN:
        switch (ev->key.keycode)
        {
        case CROSSOS_KEY_ESCAPE:
            app->running = 0;
            crossos_quit();
            break;
        case CROSSOS_KEY_Z:
            if (ev->key.mods & CROSSOS_MOD_CTRL)
            {
                if (ev->key.mods & CROSSOS_MOD_SHIFT)
                    handle_redo(app);
                else
                    handle_undo(app);
            }
            break;
        case CROSSOS_KEY_Y:
            if (ev->key.mods & CROSSOS_MOD_CTRL)
                handle_redo(app);
            break;
        case CROSSOS_KEY_SPACE:
            toggle_before(app, 1);
            break;
        case CROSSOS_KEY_R:
            /* Reset all parameters */
            undo_push(app);
            er_params_init(&app->params);
            app->dirty = 1;
            snprintf(app->status_msg, sizeof(app->status_msg), "Parameters reset.");
            break;
        case CROSSOS_KEY_F:
            /* Fit to view */
            app->zoom = 1.0f;
            app->pan_x = 0;
            app->pan_y = 0;
            break;
        case CROSSOS_KEY_O:
            handle_open(app);
            break;
        case CROSSOS_KEY_E:
            handle_export(app);
            break;
        default:
            break;
        }
        break;

    case CROSSOS_EVENT_KEY_UP:
        if (ev->key.keycode == CROSSOS_KEY_SPACE)
            toggle_before(app, 0);
        break;

    default:
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Entry point
 * ══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    app_t app;
    memset(&app, 0, sizeof(app));

    crossos_init();

    app.win = crossos_window_create("EasyRaw", WIN_W, WIN_H,
                                    CROSSOS_WINDOW_RESIZABLE);
    if (!app.win)
    {
        crossos_shutdown();
        return 1;
    }

    app.surf = crossos_surface_get(app.win);
    if (!app.surf)
    {
        crossos_window_destroy(app.win);
        crossos_shutdown();
        return 1;
    }

    /* Load typefaces */
    crossos_typeface_load_builtin(&app.font);
    crossos_typeface_load_builtin(&app.font_small);

    crossos_window_show(app.win);

    er_params_init(&app.params);
    app.running = 1;
    app.zoom = 1.0f;
    app.active_tab = TAB_EXPOSURE;
    g_curve_ctx.drag_pt = -1;
    worker_init(&app.worker);
    snprintf(app.status_msg, sizeof(app.status_msg),
             "Welcome to EasyRaw — open a file (O) or drag and drop one here.");

    /* Optional: auto-load from argv[1] */
    if (argc > 1)
        load_image(&app, argv[1]);

    /* Main loop */
    while (app.running)
    {
        crossos_event_t ev;
        while (crossos_poll_event(&ev))
            on_event(&ev, &app);

        /* Trigger re-process if any parameter changed (not while previewing before) */
        if (app.dirty && app.has_image && app.worker_running && !app.show_before)
        {
            worker_trigger(&app.worker, &app.raw, &app.params);
            app.processing = 1;
            app.dirty = 0;
        }

        /* Poll for finished result */
        if (app.worker_running &&
            worker_poll(&app.worker, &app.last_result_id, &app.processed))
        {
            if (!worker_is_busy(&app.worker))
                app.processing = 0;
        }

        render(&app);
        sleep_ms(16); /* ~60 fps */
    }

    /* Cleanup */
    if (app.worker_running)
        worker_stop(&app.worker);
    er_output_free(&app.processed);
    er_raw_image_free(&app.raw);
    if (app.font)
        crossos_typeface_destroy(app.font);
    if (app.font_small)
        crossos_typeface_destroy(app.font_small);
    crossos_window_destroy(app.win);
    crossos_shutdown();
    return 0;
}
