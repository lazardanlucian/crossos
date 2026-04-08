/**
 * examples/film_scanner/main.c
 *
 * CrossOS film scanner application.
 *
 * Provides a minimal but complete UI for scanning 35 mm film with a
 * Plustek OpticFilm scanner (or any SANE-supported device on Linux).
 *
 * Features:
 *   – Scanner device enumeration & selection
 *   – Resolution selector (75 / 300 / 1200 / 2400 / 3600 DPI)
 *   – Colour mode: Color / Grayscale
 *   – Film stock preset selector (18 stocks)
 *   – Exposure compensation slider (−2 to +2 EV in 0.25 steps)
 *   – Low-resolution preview scan (75 DPI, quick)
 *   – Full-resolution scan
 *   – Tone-curve visualiser strip (R/G/B)
 *   – Live preview: scan result scaled to fit the preview pane
 *
 * Build:
 *   cmake -B build -DCROSSOS_BUILD_EXAMPLES=ON && cmake --build build
 *   ./build/examples/film_scanner/film_scanner
 */

#include <crossos/crossos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/select.h>
#include <sys/time.h>
#endif

/* ══════════════════════════════════════════════════════════════════════════
 *  Constants
 * ══════════════════════════════════════════════════════════════════════════ */

#define WIN_W 1100
#define WIN_H 720
#define SIDEBAR_W 280
#define TOPBAR_H 38
#define STATUSBAR_H 26

/* Sidebar layout */
#define S_PAD 12
#define S_ROW_H 26
#define S_LABEL_H 16
#define S_BTN_H 26
#define S_BTN_RADIUS 4

/* Preview pane */
#define PREVIEW_X (SIDEBAR_W)
#define PREVIEW_Y (TOPBAR_H)
#define PREVIEW_W (WIN_W - SIDEBAR_W)
#define PREVIEW_H (WIN_H - TOPBAR_H - STATUSBAR_H)

/* Resolutions available in the UI */
static const int k_resolutions[] = {75, 300, 1200, 2400, 3600};
static const int k_res_count = 5;

/* ══════════════════════════════════════════════════════════════════════════
 *  Colour palette (dark theme with amber accent)
 * ══════════════════════════════════════════════════════════════════════════ */

#define COL_BG ((crossos_color_t){0x18, 0x18, 0x1E, 0xFF})
#define COL_SIDEBAR ((crossos_color_t){0x1E, 0x1E, 0x26, 0xFF})
#define COL_TOPBAR ((crossos_color_t){0x12, 0x12, 0x18, 0xFF})
#define COL_STATUS ((crossos_color_t){0x10, 0x10, 0x14, 0xFF})
#define COL_ACCENT ((crossos_color_t){0xE8, 0x8A, 0x1A, 0xFF})  /* amber   */
#define COL_ACCENT2 ((crossos_color_t){0x3A, 0xBF, 0x7C, 0xFF}) /* green   */
#define COL_BTN ((crossos_color_t){0x2C, 0x2C, 0x38, 0xFF})
#define COL_BTN_SEL ((crossos_color_t){0xE8, 0x8A, 0x1A, 0xFF})
#define COL_BTN_HOT ((crossos_color_t){0x3C, 0x3C, 0x50, 0xFF})
#define COL_TEXT ((crossos_color_t){0xE0, 0xE0, 0xE8, 0xFF})
#define COL_TEXT_DIM ((crossos_color_t){0x80, 0x80, 0x90, 0xFF})
#define COL_TEXT_DARK ((crossos_color_t){0x10, 0x10, 0x10, 0xFF})
#define COL_CURVE_R ((crossos_color_t){0xFF, 0x55, 0x55, 0xFF})
#define COL_CURVE_G ((crossos_color_t){0x55, 0xDD, 0x55, 0xFF})
#define COL_CURVE_B ((crossos_color_t){0x55, 0x88, 0xFF, 0xFF})
#define COL_PREVIEW_BG ((crossos_color_t){0x0A, 0x0A, 0x10, 0xFF})
#define COL_BORDER ((crossos_color_t){0x30, 0x30, 0x40, 0xFF})

/* ══════════════════════════════════════════════════════════════════════════
 *  Application state
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct
{
    /* Window/surface */
    crossos_window_t *win;
    crossos_surface_t *surf;
    crossos_typeface_t *font;
    int running;

    /* Scanner devices */
    crossos_scanner_info_t devices[CROSSOS_SCANNER_MAX_DEVS];
    int device_count;
    int selected_device;        /* -1 = none */
    crossos_scanner_t *scanner; /* currently open handle */

    /* Scan parameters */
    int res_index; /* index into k_resolutions */
    crossos_scanner_color_mode_t color_mode;
    crossos_film_stock_t film_stock;
    float exposure; /* EV stops, -2..+2 */

    /* Film curve */
    crossos_film_curve_t curve;

    /* Scan results */
    crossos_scan_result_t raw_scan;  /* last raw result (no curve)   */
    crossos_scan_result_t proc_scan; /* processed with curve+exposure*/

    /* UI state */
    int scanning; /* 1 = scan in progress */
    char status_msg[128];

    /* Mouse */
    float mouse_x, mouse_y;
    int mouse_down;

    /* Scroll in film-stock list */
    int stock_scroll; /* first visible stock index */
} app_t;

/* ══════════════════════════════════════════════════════════════════════════
 *  Utility helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static int rect_hit(float mx, float my,
                    int x, int y, int w, int h)
{
    return mx >= x && mx < x + w && my >= y && my < y + h;
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

static void draw_label(const crossos_framebuffer_t *fb,
                       crossos_typeface_t *font,
                       int x, int y, const char *text,
                       crossos_color_t col)
{
    if (font)
        crossos_typeface_draw_text(fb, font, x, y, text, 13.0f, col);
    else
        crossos_draw_text(fb, x, y, text, col, 1);
}

/* Draw a rounded button; returns 1 if mouse is hovering over it. */
static int draw_button(const crossos_framebuffer_t *fb,
                       crossos_typeface_t *font,
                       int x, int y, int w, int h,
                       const char *label,
                       int selected, int hovered)
{
    crossos_color_t bg = selected  ? COL_BTN_SEL
                         : hovered ? COL_BTN_HOT
                                   : COL_BTN;
    crossos_color_t txt = selected ? COL_TEXT_DARK : COL_TEXT;

    crossos_draw_fill_rounded_rect(fb, x, y, w, h, S_BTN_RADIUS, bg);
    crossos_draw_stroke_rounded_rect(fb, x, y, w, h, S_BTN_RADIUS, 1, COL_BORDER);

    /* Centre the label */
    int tw = font ? (int)strlen(label) * 7 : crossos_draw_text_width(label, 1);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - (font ? 13 : 7)) / 2;
    draw_label(fb, font, tx, ty, label, txt);
    (void)hovered;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Scan processing
 * ══════════════════════════════════════════════════════════════════════════ */

static void free_scan(crossos_scan_result_t *s)
{
    crossos_scanner_free_result(s);
}

static void reprocess(app_t *app)
{
    free_scan(&app->proc_scan);
    if (!app->raw_scan.pixels)
        return;

    int n = app->raw_scan.width * app->raw_scan.height * 4;
    app->proc_scan.pixels = malloc((size_t)n);
    if (!app->proc_scan.pixels)
        return;

    memcpy(app->proc_scan.pixels, app->raw_scan.pixels, (size_t)n);
    app->proc_scan.width = app->raw_scan.width;
    app->proc_scan.height = app->raw_scan.height;
    app->proc_scan.stride = app->raw_scan.stride;
    app->proc_scan.resolution_dpi = app->raw_scan.resolution_dpi;
    app->proc_scan.bits_per_channel = app->raw_scan.bits_per_channel;
    app->proc_scan.raw16 = NULL; /* not duplicated */

    crossos_film_apply_curve(&app->proc_scan, &app->curve, app->exposure);
}

static void update_curve(app_t *app)
{
    crossos_film_curve_get_preset(app->film_stock, &app->curve);
    reprocess(app);
}

static void do_scan(app_t *app, int preview)
{
    if (!app->scanner)
        return;

    app->scanning = 1;
    snprintf(app->status_msg, sizeof(app->status_msg),
             "%s...", preview ? "Preview scan" : "Scanning");

    free_scan(&app->raw_scan);
    free_scan(&app->proc_scan);

    crossos_scanner_params_t p;
    crossos_scanner_get_default_params(app->scanner, &p);
    p.resolution = k_resolutions[app->res_index];
    p.color_mode = app->color_mode;
    p.bit_depth = CROSSOS_SCANNER_DEPTH_8;
    p.preview = preview;
    if (preview)
        p.resolution = 75;

    crossos_result_t rc = crossos_scanner_scan(app->scanner, &p,
                                               &app->raw_scan);
    app->scanning = 0;

    if (rc == CROSSOS_OK)
    {
        reprocess(app);
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "%s complete: %d x %d @ %d DPI",
                 preview ? "Preview" : "Scan",
                 app->raw_scan.width, app->raw_scan.height,
                 app->raw_scan.resolution_dpi);
    }
    else
    {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Scan failed (error %d)", (int)rc);
    }
}

static void open_scanner(app_t *app, int idx)
{
    if (app->scanner)
    {
        crossos_scanner_close(app->scanner);
        app->scanner = NULL;
    }
    if (idx < 0 || idx >= app->device_count)
    {
        app->selected_device = -1;
        return;
    }
    crossos_result_t rc = crossos_scanner_open(idx, &app->scanner);
    if (rc == CROSSOS_OK)
    {
        app->selected_device = idx;
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Opened: %s", app->devices[idx].name);
    }
    else
    {
        app->selected_device = -1;
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Failed to open device (error %d)", (int)rc);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Rendering
 * ══════════════════════════════════════════════════════════════════════════ */

static void draw_curve_strip(const crossos_framebuffer_t *fb,
                             const crossos_film_curve_t *curve,
                             int x, int y, int w, int h)
{
    crossos_draw_fill_rect(fb, x, y, w, h,
                           (crossos_color_t){0x10, 0x10, 0x14, 0xFF});
    crossos_draw_stroke_rect(fb, x, y, w, h, 1, COL_BORDER);

    if (!curve)
        return;

    int prev_ry = -1, prev_gy = -1, prev_by = -1;
    for (int i = 0; i < 256; i++)
    {
        int px = x + i * w / 256;
        int ry = y + h - 1 - (int)(curve->r[i] * (h - 1) / 255);
        int gy = y + h - 1 - (int)(curve->g[i] * (h - 1) / 255);
        int by = y + h - 1 - (int)(curve->b[i] * (h - 1) / 255);

        if (i > 0)
        {
            int ppx = x + (i - 1) * w / 256;
            crossos_draw_line(fb, ppx, prev_ry, px, ry, COL_CURVE_R);
            crossos_draw_line(fb, ppx, prev_gy, px, gy, COL_CURVE_G);
            crossos_draw_line(fb, ppx, prev_by, px, by, COL_CURVE_B);
        }
        prev_ry = ry;
        prev_gy = gy;
        prev_by = by;
    }
}

static void draw_preview(const crossos_framebuffer_t *fb,
                         const crossos_scan_result_t *scan)
{
    crossos_draw_fill_rect(fb, PREVIEW_X, PREVIEW_Y,
                           PREVIEW_W, PREVIEW_H, COL_PREVIEW_BG);

    if (!scan || !scan->pixels)
    {
        /* Show placeholder text */
        const char *msg = "No scan yet. Press Preview or Scan.";
        int tw = crossos_draw_text_width(msg, 1);
        int tx = PREVIEW_X + (PREVIEW_W - tw) / 2;
        int ty = PREVIEW_Y + PREVIEW_H / 2 - 4;
        crossos_draw_text(fb, tx, ty, msg, COL_TEXT_DIM, 1);
        return;
    }

    /* Scale image to fit the preview pane while keeping aspect ratio. */
    float scale_x = (float)PREVIEW_W / (float)scan->width;
    float scale_y = (float)PREVIEW_H / (float)scan->height;
    float scale = scale_x < scale_y ? scale_x : scale_y;

    int dw = (int)(scan->width * scale);
    int dh = (int)(scan->height * scale);
    int ox = PREVIEW_X + (PREVIEW_W - dw) / 2;
    int oy = PREVIEW_Y + (PREVIEW_H - dh) / 2;

    /* Simple nearest-neighbour blit */
    unsigned char *dst_row = (unsigned char *)fb->pixels + oy * fb->stride + ox * 4;

    for (int dy = 0; dy < dh; dy++)
    {
        int sy = (int)(dy / scale);
        if (sy >= scan->height)
            sy = scan->height - 1;
        unsigned char *src_row = scan->pixels + sy * scan->stride;
        unsigned char *dst_px = dst_row;

        for (int dx = 0; dx < dw; dx++)
        {
            int sx = (int)(dx / scale);
            if (sx >= scan->width)
                sx = scan->width - 1;
            unsigned char *src_px = src_row + sx * 4;
            /* RGBA → framebuffer layout (assume BGRA on Linux/Windows) */
            dst_px[0] = src_px[2]; /* B = scan B */
            dst_px[1] = src_px[1]; /* G */
            dst_px[2] = src_px[0]; /* R */
            dst_px[3] = 0xFF;
            dst_px += 4;
        }
        dst_row += fb->stride;
    }
}

static void render(app_t *app, const crossos_framebuffer_t *fb)
{
    /* ── Background ──────────────────────────────────────────────── */
    crossos_draw_clear(fb, COL_BG);

    /* ── Top bar ─────────────────────────────────────────────────── */
    crossos_draw_fill_rect(fb, 0, 0, WIN_W, TOPBAR_H, COL_TOPBAR);
    draw_label(fb, app->font, 12, (TOPBAR_H - 14) / 2,
               "CrossOS Film Scanner", COL_ACCENT);
    if (app->selected_device >= 0)
    {
        char dev_str[80];
        snprintf(dev_str, sizeof(dev_str), "Device: %s",
                 app->devices[app->selected_device].name);
        draw_label(fb, app->font, SIDEBAR_W + 12, (TOPBAR_H - 14) / 2,
                   dev_str, COL_TEXT_DIM);
    }

    /* ── Sidebar background ──────────────────────────────────────── */
    crossos_draw_fill_rect(fb, 0, TOPBAR_H, SIDEBAR_W,
                           WIN_H - TOPBAR_H, COL_SIDEBAR);
    crossos_draw_line(fb, SIDEBAR_W - 1, TOPBAR_H,
                      SIDEBAR_W - 1, WIN_H, COL_BORDER);

    int sy = TOPBAR_H + S_PAD;
    int sx = S_PAD;
    int sw = SIDEBAR_W - S_PAD * 2;

    /* ── Scanner list ─────────────────────────────────────────── */
    draw_label(fb, app->font, sx, sy, "SCANNER DEVICES", COL_TEXT_DIM);
    sy += S_LABEL_H + 4;

    if (app->device_count == 0)
    {
        draw_label(fb, app->font, sx + 4, sy + 6, "No scanners found",
                   COL_TEXT_DIM);
        sy += S_ROW_H + 4;
    }
    else
    {
        for (int i = 0; i < app->device_count && i < 3; i++)
        {
            int by = sy + i * (S_BTN_H + 2);
            int hover = rect_hit(app->mouse_x, app->mouse_y,
                                 sx, by, sw, S_BTN_H);
            int sel = (i == app->selected_device);
            char label[48];
            snprintf(label, sizeof(label), "%s", app->devices[i].model[0] ? app->devices[i].model : app->devices[i].name);
            draw_button(fb, app->font, sx, by, sw, S_BTN_H, label, sel, hover);
        }
        sy += app->device_count * (S_BTN_H + 2) + 6;
    }

    /* ── Divider ─────────────────────────────────────────────────── */
    crossos_draw_line(fb, sx, sy, sx + sw, sy, COL_BORDER);
    sy += 8;

    /* ── Resolution ──────────────────────────────────────────────── */
    draw_label(fb, app->font, sx, sy, "RESOLUTION", COL_TEXT_DIM);
    sy += S_LABEL_H + 4;
    {
        int bw = (sw - (k_res_count - 1) * 2) / k_res_count;
        for (int i = 0; i < k_res_count; i++)
        {
            int bx = sx + i * (bw + 2);
            int hover = rect_hit(app->mouse_x, app->mouse_y,
                                 bx, sy, bw, S_BTN_H);
            char label[16];
            if (k_resolutions[i] >= 1000)
                snprintf(label, sizeof(label), "%dk", k_resolutions[i] / 1000);
            else
                snprintf(label, sizeof(label), "%d", k_resolutions[i]);
            draw_button(fb, app->font, bx, sy, bw, S_BTN_H, label,
                        i == app->res_index, hover);
        }
    }
    sy += S_BTN_H + 8;

    /* ── Colour mode ─────────────────────────────────────────────── */
    draw_label(fb, app->font, sx, sy, "COLOUR MODE", COL_TEXT_DIM);
    sy += S_LABEL_H + 4;
    {
        int bw2 = (sw - 2) / 2;
        const char *modes[2] = {"Color", "Gray"};
        for (int i = 0; i < 2; i++)
        {
            int bx = sx + i * (bw2 + 2);
            int hover = rect_hit(app->mouse_x, app->mouse_y,
                                 bx, sy, bw2, S_BTN_H);
            draw_button(fb, app->font, bx, sy, bw2, S_BTN_H, modes[i],
                        (int)app->color_mode == i, hover);
        }
    }
    sy += S_BTN_H + 8;

    /* ── Film stock ──────────────────────────────────────────────── */
    crossos_draw_line(fb, sx, sy, sx + sw, sy, COL_BORDER);
    sy += 8;
    draw_label(fb, app->font, sx, sy, "FILM STOCK PRESET", COL_TEXT_DIM);
    sy += S_LABEL_H + 4;

    /* Show scrollable list of all presets (5 visible) */
    int visible_stocks = 6;
    int stock_bh = S_BTN_H - 2;
    for (int i = 0; i < visible_stocks; i++)
    {
        int si = i + app->stock_scroll;
        if (si >= (int)CROSSOS_FILM_STOCK_COUNT)
            break;
        int by = sy + i * (stock_bh + 2);
        int hover = rect_hit(app->mouse_x, app->mouse_y, sx, by, sw, stock_bh);
        draw_button(fb, app->font, sx, by, sw, stock_bh,
                    crossos_film_stock_name((crossos_film_stock_t)si),
                    (int)app->film_stock == si, hover);
    }
    sy += visible_stocks * (stock_bh + 2) + 4;

    /* Scroll indicator */
    int total_stocks = (int)CROSSOS_FILM_STOCK_COUNT;
    if (total_stocks > visible_stocks)
    {
        draw_label(fb, app->font, sx, sy,
                   "(scroll wheel to see more)", COL_TEXT_DIM);
        sy += S_LABEL_H + 4;
    }

    /* ── Exposure ────────────────────────────────────────────────── */
    crossos_draw_line(fb, sx, sy, sx + sw, sy, COL_BORDER);
    sy += 8;
    {
        char exp_label[32];
        snprintf(exp_label, sizeof(exp_label), "EXPOSURE: %+.2f EV",
                 app->exposure);
        draw_label(fb, app->font, sx, sy, exp_label, COL_TEXT_DIM);
        sy += S_LABEL_H + 4;

        /* Slider track */
        int track_y = sy + S_BTN_H / 2 - 2;
        crossos_draw_fill_rect(fb, sx, track_y, sw, 4, COL_BTN);
        /* Slider fill (0 EV = centre) */
        float t = (app->exposure + 2.0f) / 4.0f; /* 0..1 */
        int fill_w = (int)(t * sw);
        crossos_draw_fill_rect(fb, sx, track_y, fill_w, 4, COL_ACCENT);
        /* Thumb */
        int thumb_x = sx + fill_w - 6;
        crossos_draw_fill_circle(fb, thumb_x, track_y + 2, 6, COL_ACCENT);
        crossos_draw_stroke_rect(fb, sx, sy, sw, S_BTN_H, 1, COL_BORDER);
        sy += S_BTN_H + 8;
    }

    /* ── Curve strip ─────────────────────────────────────────────── */
    crossos_draw_line(fb, sx, sy, sx + sw, sy, COL_BORDER);
    sy += 6;
    draw_label(fb, app->font, sx, sy, "TONE CURVE", COL_TEXT_DIM);
    sy += S_LABEL_H + 2;
    int curve_h = 50;
    draw_curve_strip(fb, &app->curve, sx, sy, sw, curve_h);
    sy += curve_h + 8;

    /* ── Action buttons ──────────────────────────────────────────── */
    crossos_draw_line(fb, sx, sy, sx + sw, sy, COL_BORDER);
    sy += 8;
    int abw = (sw - 4) / 2;
    /* Preview */
    {
        int hover = rect_hit(app->mouse_x, app->mouse_y, sx, sy, abw, 30);
        crossos_color_t bg = hover ? COL_BTN_HOT : COL_BTN;
        crossos_draw_fill_rounded_rect(fb, sx, sy, abw, 30, S_BTN_RADIUS, bg);
        crossos_draw_stroke_rounded_rect(fb, sx, sy, abw, 30, S_BTN_RADIUS, 1,
                                         COL_ACCENT2);
        draw_label(fb, app->font,
                   sx + (abw - 48) / 2, sy + (30 - 13) / 2,
                   "Preview", COL_ACCENT2);
    }
    /* Full Scan */
    {
        int bx = sx + abw + 4;
        int hover = rect_hit(app->mouse_x, app->mouse_y, bx, sy, abw, 30);
        crossos_color_t bg = hover ? COL_BTN_HOT : COL_BTN;
        crossos_draw_fill_rounded_rect(fb, bx, sy, abw, 30, S_BTN_RADIUS, bg);
        crossos_draw_stroke_rounded_rect(fb, bx, sy, abw, 30, S_BTN_RADIUS, 1,
                                         COL_ACCENT);
        draw_label(fb, app->font,
                   bx + (abw - 35) / 2, sy + (30 - 13) / 2,
                   "Scan", COL_ACCENT);
    }

    /* ── Preview pane ────────────────────────────────────────────── */
    draw_preview(fb, &app->proc_scan);

    /* ── Status bar ──────────────────────────────────────────────── */
    int sby = WIN_H - STATUSBAR_H;
    crossos_draw_fill_rect(fb, 0, sby, WIN_W, STATUSBAR_H, COL_STATUS);
    crossos_draw_line(fb, 0, sby, WIN_W, sby, COL_BORDER);

    const char *status = app->scanning ? "Scanning — please wait..."
                                       : (app->status_msg[0]
                                              ? app->status_msg
                                              : "Ready");
    draw_label(fb, app->font, 10, sby + (STATUSBAR_H - 13) / 2,
               status, app->scanning ? COL_ACCENT : COL_TEXT_DIM);

    /* Scan info on the right side of status bar */
    if (app->proc_scan.pixels)
    {
        char info[64];
        snprintf(info, sizeof(info), "%d × %d px | %d DPI",
                 app->proc_scan.width, app->proc_scan.height,
                 app->proc_scan.resolution_dpi);
        int iw = crossos_draw_text_width(info, 1);
        crossos_draw_text(fb, WIN_W - iw - 10, sby + (STATUSBAR_H - 7) / 2,
                          info, COL_TEXT_DIM, 1);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Hit testing for interactive controls
 * ══════════════════════════════════════════════════════════════════════════ */

static int sidebar_action_y_for_device(int idx)
{
    int sy = TOPBAR_H + S_PAD;
    sy += S_LABEL_H + 4; /* "SCANNER DEVICES" label */
    return sy + idx * (S_BTN_H + 2);
}

static int exposure_slider_hit(app_t *app, float mx, float my, float *out_ev)
{
    /* Need to recompute sy for the exposure slider — fragile but simple */
    int sx = S_PAD;
    int sw = SIDEBAR_W - S_PAD * 2;
    int sy = TOPBAR_H + S_PAD;

    sy += S_LABEL_H + 4; /* SCANNER DEVICES label */
    int dev_rows = app->device_count > 0 ? app->device_count : 1;
    if (dev_rows > 3)
        dev_rows = 3;
    sy += dev_rows * (S_BTN_H + 2) + 6;
    sy += 8;             /* divider */
    sy += S_LABEL_H + 4; /* RESOLUTION label */
    sy += S_BTN_H + 8;
    sy += S_LABEL_H + 4; /* COLOUR MODE label */
    sy += S_BTN_H + 8;
    sy += 8;                         /* divider */
    sy += S_LABEL_H + 4;             /* FILM STOCK label */
    sy += 6 * (S_BTN_H - 2 + 2) + 4; /* stock list */
    if ((int)CROSSOS_FILM_STOCK_COUNT > 6)
        sy += S_LABEL_H + 4; /* scroll hint */
    sy += 8;                 /* divider */
    sy += S_LABEL_H + 4;     /* EXPOSURE label */

    if (rect_hit(mx, my, sx, sy, sw, S_BTN_H))
    {
        float t = (mx - sx) / (float)sw;
        if (t < 0.0f)
            t = 0.0f;
        if (t > 1.0f)
            t = 1.0f;
        *out_ev = t * 4.0f - 2.0f;
        /* Snap to 0.25 steps */
        *out_ev = (float)(int)(*out_ev * 4.0f + 0.5f) / 4.0f;
        return 1;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Event callback
 * ══════════════════════════════════════════════════════════════════════════ */

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

    case CROSSOS_EVENT_KEY_DOWN:
        if (ev->key.keycode == CROSSOS_KEY_ESCAPE)
        {
            app->running = 0;
            crossos_quit();
        }
        break;

    case CROSSOS_EVENT_POINTER_MOVE:
        app->mouse_x = ev->pointer.x;
        app->mouse_y = ev->pointer.y;
        break;

    case CROSSOS_EVENT_POINTER_DOWN:
        app->mouse_x = ev->pointer.x;
        app->mouse_y = ev->pointer.y;
        app->mouse_down = 1;
        {
            float mx = ev->pointer.x, my = ev->pointer.y;
            int sx = S_PAD, sw = SIDEBAR_W - S_PAD * 2;

            /* ── Device selection ─────────────────────────────── */
            for (int i = 0; i < app->device_count && i < 3; i++)
            {
                int by = sidebar_action_y_for_device(i);
                if (rect_hit(mx, my, sx, by, sw, S_BTN_H))
                {
                    open_scanner(app, i);
                    break;
                }
            }

            /* ── Resolution buttons ───────────────────────────── */
            {
                int bsy = TOPBAR_H + S_PAD;
                int dev_rows = app->device_count > 0
                                   ? (app->device_count < 3 ? app->device_count : 3)
                                   : 1;
                bsy += S_LABEL_H + 4;
                bsy += dev_rows * (S_BTN_H + 2) + 6;
                bsy += 8;
                bsy += S_LABEL_H + 4;

                int bw = (sw - (k_res_count - 1) * 2) / k_res_count;
                for (int i = 0; i < k_res_count; i++)
                {
                    int bx = sx + i * (bw + 2);
                    if (rect_hit(mx, my, bx, bsy, bw, S_BTN_H))
                    {
                        app->res_index = i;
                        break;
                    }
                }

                bsy += S_BTN_H + 8;
                bsy += S_LABEL_H + 4;

                /* ── Colour mode ──────────────────────────────── */
                int bw2 = (sw - 2) / 2;
                for (int i = 0; i < 2; i++)
                {
                    int bx = sx + i * (bw2 + 2);
                    if (rect_hit(mx, my, bx, bsy, bw2, S_BTN_H))
                    {
                        app->color_mode = (crossos_scanner_color_mode_t)i;
                        break;
                    }
                }

                bsy += S_BTN_H + 8;
                bsy += 8;
                bsy += S_LABEL_H + 4;

                /* ── Film stock ───────────────────────────────── */
                int stock_bh = S_BTN_H - 2;
                for (int i = 0; i < 6; i++)
                {
                    int si = i + app->stock_scroll;
                    if (si >= (int)CROSSOS_FILM_STOCK_COUNT)
                        break;
                    int by = bsy + i * (stock_bh + 2);
                    if (rect_hit(mx, my, sx, by, sw, stock_bh))
                    {
                        app->film_stock = (crossos_film_stock_t)si;
                        update_curve(app);
                        break;
                    }
                }

                bsy += 6 * (stock_bh + 2) + 4;
                if ((int)CROSSOS_FILM_STOCK_COUNT > 6)
                    bsy += S_LABEL_H + 4;
                bsy += 8;
                bsy += S_LABEL_H + 4;

                /* ── Exposure slider ──────────────────────────── */
                float new_ev = 0.0f;
                if (rect_hit(mx, my, sx, bsy, sw, S_BTN_H))
                {
                    float t = (mx - sx) / (float)sw;
                    if (t < 0.0f)
                        t = 0.0f;
                    if (t > 1.0f)
                        t = 1.0f;
                    new_ev = t * 4.0f - 2.0f;
                    new_ev = (float)(int)(new_ev * 4.0f + 0.5f) / 4.0f;
                    app->exposure = new_ev;
                    reprocess(app);
                }
                bsy += S_BTN_H + 8;
                bsy += 8; /* divider */
                bsy += S_LABEL_H + 2;
                bsy += 50 + 8; /* curve strip */
                bsy += 8;      /* divider */
                bsy += 8;      /* padding */

                /* ── Preview & Scan buttons ───────────────────── */
                int abw = (sw - 4) / 2;
                if (rect_hit(mx, my, sx, bsy, abw, 30))
                {
                    do_scan(app, 1); /* preview */
                }
                else if (rect_hit(mx, my, sx + abw + 4, bsy, abw, 30))
                {
                    do_scan(app, 0); /* full scan */
                }
            }
        }
        break;

    case CROSSOS_EVENT_POINTER_UP:
        app->mouse_down = 0;
        break;

    case CROSSOS_EVENT_POINTER_SCROLL:
        /* Scroll the film stock list */
        if (ev->pointer.x < SIDEBAR_W)
        {
            int delta = ev->pointer.scroll_y < 0 ? -1 : 1;
            app->stock_scroll += delta;
            int max_scroll = (int)CROSSOS_FILM_STOCK_COUNT - 6;
            if (app->stock_scroll < 0)
                app->stock_scroll = 0;
            if (app->stock_scroll > max_scroll)
                app->stock_scroll = max_scroll;
        }
        break;

    default:
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Entry point
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    app_t app;
    memset(&app, 0, sizeof(app));
    app.selected_device = -1;
    app.res_index = 2; /* 1200 DPI default */
    app.color_mode = CROSSOS_SCANNER_COLOR_MODE_COLOR;
    app.film_stock = CROSSOS_FILM_STOCK_KODAK_PORTRA_400;
    app.exposure = 0.0f;
    app.running = 1;
    strncpy(app.status_msg, "Initialising scanner subsystem...",
            sizeof(app.status_msg) - 1);

    /* ── SDK init ──────────────────────────────────────────────────── */
    crossos_init();

    app.win = crossos_window_create("CrossOS Film Scanner",
                                    WIN_W, WIN_H, 0);
    app.surf = crossos_surface_get(app.win);
    crossos_window_show(app.win);

    /* Try to load a system font */
    crossos_typeface_load_system("Sans",
                                 CROSSOS_TYPEFACE_STYLE_NORMAL,
                                 &app.font);
    if (!app.font)
        crossos_typeface_load_builtin(&app.font);

    /* ── Scanner init ─────────────────────────────────────────────── */
    crossos_result_t sc_rc = crossos_scanner_init();
    if (sc_rc == CROSSOS_OK)
    {
        app.device_count = crossos_scanner_enumerate(app.devices,
                                                     CROSSOS_SCANNER_MAX_DEVS);
        if (app.device_count > 0)
        {
            snprintf(app.status_msg, sizeof(app.status_msg),
                     "Found %d scanner(s). Select a device.", app.device_count);
            /* Auto-open the first device */
            open_scanner(&app, 0);
        }
        else
        {
            snprintf(app.status_msg, sizeof(app.status_msg),
                     "No scanners found. Connect a scanner and restart.");
        }
    }
    else
    {
        snprintf(app.status_msg, sizeof(app.status_msg),
                 "Scanner subsystem unavailable (error %d).", (int)sc_rc);
    }

    /* Load initial curve preset */
    update_curve(&app);

    /* ── Main loop ────────────────────────────────────────────────── */
    while (app.running)
    {
        crossos_framebuffer_t fb;
        if (crossos_surface_lock(app.surf, &fb) == CROSSOS_OK)
        {
            render(&app, &fb);
            crossos_surface_unlock(app.surf);
            crossos_surface_present(app.surf);
        }

        crossos_event_t ev;
        while (crossos_poll_event(&ev))
            on_event(&ev, &app);

        sleep_ms(16); /* ~60 fps */
    }

    /* ── Cleanup ──────────────────────────────────────────────────── */
    free_scan(&app.raw_scan);
    free_scan(&app.proc_scan);
    if (app.scanner)
        crossos_scanner_close(app.scanner);
    crossos_scanner_shutdown();

    if (app.font)
        crossos_typeface_destroy(app.font);
    crossos_window_destroy(app.win);
    crossos_shutdown();
    return 0;
}
