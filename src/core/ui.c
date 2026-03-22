#include <crossos/ui.h>
#include <crossos/input.h>

#include <stdio.h>
#include <string.h>

/* ── Built-in themes ─────────────────────────────────────────────────── */

static const crossos_ui_theme_t k_dark = {
    /* bg              */ {13,  17,  23,  255},
    /* surface         */ {22,  27,  34,  255},
    /* surface_alt     */ {17,  22,  29,  255},
    /* surface_hover   */ {33,  42,  55,  255},
    /* surface_active  */ {48,  60,  80,  255},
    /* accent          */ {37,  99, 235,  255},
    /* accent_hover    */ {59, 130, 246,  255},
    /* accent_dim      */ {30,  60, 110,  255},
    /* text            */ {230, 237, 243, 255},
    /* text_dim        */ {139, 148, 158, 255},
    /* text_disabled   */ {72,  79,  88,  255},
    /* border          */ {48,  54,  61,  255},
    /* success         */ {63, 185, 80,  255},
    /* danger          */ {248, 81,  73,  255},
    /* warning         */ {210, 153, 34,  255},
    /* radius          */ 5,
    /* spacing         */ 8,
};

static const crossos_ui_theme_t k_light = {
    /* bg              */ {246, 248, 250, 255},
    /* surface         */ {255, 255, 255, 255},
    /* surface_alt     */ {240, 242, 244, 255},
    /* surface_hover   */ {234, 238, 242, 255},
    /* surface_active  */ {218, 228, 240, 255},
    /* accent          */ {37,  99, 235,  255},
    /* accent_hover    */ {29,  78, 216,  255},
    /* accent_dim      */ {147, 197, 253, 255},
    /* text            */ {24,  30,  40,  255},
    /* text_dim        */ {101, 109, 118, 255},
    /* text_disabled   */ {179, 186, 194, 255},
    /* border          */ {208, 215, 222, 255},
    /* success         */ {26, 127,  55,  255},
    /* danger          */ {207,  34,  46,  255},
    /* warning         */ {154,  85,   0,  255},
    /* radius          */ 5,
    /* spacing         */ 8,
};

const crossos_ui_theme_t *crossos_ui_theme_dark(void)  { return &k_dark; }
const crossos_ui_theme_t *crossos_ui_theme_light(void) { return &k_light; }

/* ── Context ─────────────────────────────────────────────────────────── */

int crossos_ui_scale_for_surface(const crossos_framebuffer_t *fb)
{
    if (!fb) return 1;
    int m = fb->width < fb->height ? fb->width : fb->height;
    return (m >= 1000) ? 2 : 1;
}

void crossos_ui_begin(crossos_ui_context_t *ui,
                      const crossos_framebuffer_t *fb,
                      const crossos_ui_input_t *input)
{
    if (!ui) return;
    memset(ui, 0, sizeof(*ui));
    ui->fb    = fb;
    ui->scale = crossos_ui_scale_for_surface(fb);
    ui->theme = k_dark;
    if (input) ui->input = *input;
}

void crossos_ui_set_theme(crossos_ui_context_t *ui,
                          const crossos_ui_theme_t *theme)
{
    if (ui && theme) ui->theme = *theme;
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int pt_in(int px, int py, int x, int y, int w, int h)
{
    return px >= x && py >= y && px < x + w && py < y + h;
}

/* Draw centered text inside (x,y,w,h). */
static void draw_centered(crossos_ui_context_t *ui,
                          int x, int y, int w, int h,
                          const char *text, crossos_color_t color)
{
    if (!text) return;
    int tw = crossos_draw_text_width(text, ui->scale);
    int th = crossos_draw_text_height(ui->scale);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - th) / 2;
    crossos_draw_text(ui->fb, tx, ty, text, color, ui->scale);
}

/* Scale a theme dimension (radius, spacing, etc.) by ui->scale. */
static int S(const crossos_ui_context_t *ui, int v)
{
    return v * ui->scale;
}

/* ── Static elements ─────────────────────────────────────────────────── */

void crossos_ui_panel(crossos_ui_context_t *ui,
                      int x, int y, int w, int h,
                      crossos_color_t color)
{
    if (!ui || !ui->fb) return;
    int r = S(ui, ui->theme.radius);
    crossos_draw_fill_rounded_rect(ui->fb, x, y, w, h, r, color);
}

void crossos_ui_label(crossos_ui_context_t *ui,
                      int x, int y,
                      const char *text,
                      crossos_color_t color)
{
    if (!ui || !ui->fb || !text) return;
    crossos_draw_text(ui->fb, x, y, text, color, ui->scale);
}

void crossos_ui_label_centered(crossos_ui_context_t *ui,
                                int x, int y, int w, int h,
                                const char *text,
                                crossos_color_t color)
{
    if (!ui || !ui->fb || !text) return;
    draw_centered(ui, x, y, w, h, text, color);
}

void crossos_ui_separator(crossos_ui_context_t *ui, int x, int y, int w)
{
    if (!ui || !ui->fb) return;
    crossos_draw_fill_rect(ui->fb, x, y, w, 1, ui->theme.border);
}

void crossos_ui_badge(crossos_ui_context_t *ui,
                      int x, int y, int count,
                      crossos_color_t color)
{
    if (!ui || !ui->fb) return;
    int r = S(ui, 8);
    crossos_draw_fill_circle(ui->fb, x, y, r, color);
    char s[8]; snprintf(s, sizeof(s), "%d", count < 100 ? count : 99);
    int tw = crossos_draw_text_width(s, 1);
    crossos_draw_text(ui->fb, x - tw/2, y - 3, s,
                      (crossos_color_t){255,255,255,255}, 1);
}

void crossos_ui_spinner(crossos_ui_context_t *ui, int cx, int cy, int size)
{
    if (!ui || !ui->fb) return;
    int r = size / 2;
    /* Draw arc: 6 dots around a circle */
    float pi2 = 6.2831853f;
    for (int i = 0; i < 8; i++) {
        float angle = pi2 * i / 8.0f + (ui->frame * 0.15f);
        int dx = (int)(r * 0.7f * (i < 4 ? 1 : -1));  /* placeholder */
        /* Proper trig approximation */
        float cs = 1.0f, sn = 0.0f;
        /* Use lookup-like approach with integer steps */
        int idx = (i + (ui->frame / 4)) % 8;
        static const float c8[8] = {1.f, 0.7f, 0.f,-0.7f,-1.f,-0.7f, 0.f, 0.7f};
        static const float s8[8] = {0.f, 0.7f, 1.f, 0.7f, 0.f,-0.7f,-1.f,-0.7f};
        (void)angle; (void)dx; (void)cs; (void)sn;
        cs = c8[i]; sn = s8[i];
        int px = cx + (int)(r * cs);
        int py = cy + (int)(r * sn);
        unsigned char a = (unsigned char)(255 - idx * 28);
        crossos_draw_fill_circle(ui->fb, px, py, S(ui, 2),
                                 (crossos_color_t){ui->theme.accent.r,
                                                   ui->theme.accent.g,
                                                   ui->theme.accent.b, a});
    }
}

/* ── Interactive widgets ─────────────────────────────────────────────── */

static int button_impl(crossos_ui_context_t *ui,
                       int x, int y, int w, int h,
                       const char *text,
                       int enabled,
                       crossos_color_t base,
                       crossos_color_t base_hover,
                       crossos_color_t base_disabled)
{
    if (!ui || !ui->fb) return 0;
    int hov = pt_in(ui->input.pointer_x, ui->input.pointer_y, x, y, w, h);
    int r   = S(ui, ui->theme.radius);

    crossos_color_t bg;
    crossos_color_t tc;
    if (!enabled) {
        bg = base_disabled;
        tc = ui->theme.text_disabled;
    } else if (hov && ui->input.pointer_down) {
        bg = crossos_color_darken(base_hover, 15);
        tc = ui->theme.text;
    } else if (hov) {
        bg = base_hover;
        tc = ui->theme.text;
    } else {
        bg = base;
        tc = ui->theme.text;
    }

    crossos_draw_fill_rounded_rect(ui->fb, x, y, w, h, r, bg);
    /* 1px border */
    crossos_draw_stroke_rounded_rect(ui->fb, x, y, w, h, r, 1,
                                     crossos_color_lighten(bg, 20));
    draw_centered(ui, x, y, w, h, text, tc);

    return enabled && hov && ui->input.pointer_pressed;
}

int crossos_ui_button(crossos_ui_context_t *ui,
                      int x, int y, int w, int h,
                      const char *text, int enabled)
{
    return button_impl(ui, x, y, w, h, text, enabled,
                       ui->theme.accent,
                       ui->theme.accent_hover,
                       ui->theme.accent_dim);
}

int crossos_ui_button_danger(crossos_ui_context_t *ui,
                              int x, int y, int w, int h,
                              const char *text, int enabled)
{
    crossos_color_t dh = crossos_color_lighten(ui->theme.danger, 20);
    crossos_color_t dd = crossos_color_darken(ui->theme.danger, 60);
    return button_impl(ui, x, y, w, h, text, enabled,
                       ui->theme.danger, dh, dd);
}

int crossos_ui_button_ghost(crossos_ui_context_t *ui,
                             int x, int y, int w, int h,
                             const char *text, int enabled)
{
    if (!ui || !ui->fb) return 0;
    int hov = pt_in(ui->input.pointer_x, ui->input.pointer_y, x, y, w, h);
    int r   = S(ui, ui->theme.radius);

    crossos_color_t bg = {0, 0, 0, 0};
    crossos_color_t tc = enabled ? ui->theme.accent : ui->theme.text_disabled;
    if (hov && enabled)
        bg = ui->theme.surface_hover;

    if (bg.a > 0)
        crossos_draw_fill_rounded_rect(ui->fb, x, y, w, h, r, bg);
    crossos_draw_stroke_rounded_rect(ui->fb, x, y, w, h, r, 1,
                                     enabled ? ui->theme.accent : ui->theme.border);
    draw_centered(ui, x, y, w, h, text, tc);

    return enabled && hov && ui->input.pointer_pressed;
}

int crossos_ui_selectable(crossos_ui_context_t *ui,
                          int x, int y, int w, int h,
                          const char *text, int selected)
{
    if (!ui || !ui->fb) return 0;
    int hov = pt_in(ui->input.pointer_x, ui->input.pointer_y, x, y, w, h);
    int r   = S(ui, 3);

    crossos_color_t bg;
    if (selected)
        bg = ui->theme.surface_active;
    else if (hov)
        bg = ui->theme.surface_hover;
    else
        bg = ui->theme.surface_alt;

    crossos_draw_fill_rounded_rect(ui->fb, x, y, w, h, r, bg);
    if (selected) {
        /* Left accent stripe */
        crossos_draw_fill_rect(ui->fb, x, y, 3, h, ui->theme.accent);
    }

    int pad = S(ui, ui->theme.spacing);
    crossos_draw_text(ui->fb, x + pad + (selected ? 4 : 0),
                      y + (h - crossos_draw_text_height(ui->scale)) / 2,
                      text ? text : "", ui->theme.text, ui->scale);

    return hov && ui->input.pointer_pressed;
}

int crossos_ui_checkbox(crossos_ui_context_t *ui,
                        int x, int y,
                        const char *label, int *checked)
{
    if (!ui || !ui->fb || !checked) return 0;
    unsigned myid = ++ui->id_counter;

    int bs  = S(ui, 12);  /* box size */
    int pad = S(ui, 6);
    int hov = pt_in(ui->input.pointer_x, ui->input.pointer_y,
                    x, y, bs + pad + crossos_draw_text_width(label, ui->scale), bs);

    crossos_color_t box_bg = *checked ? ui->theme.accent : ui->theme.surface;
    if (hov) box_bg = *checked ? ui->theme.accent_hover : ui->theme.surface_hover;

    crossos_draw_fill_rounded_rect(ui->fb, x, y, bs, bs,
                                   S(ui, 3), box_bg);
    crossos_draw_stroke_rounded_rect(ui->fb, x, y, bs, bs, S(ui, 3), 1,
                                     *checked ? ui->theme.accent_hover : ui->theme.border);

    if (*checked) {
        /* Checkmark: two lines forming a tick */
        crossos_draw_line(ui->fb, x+2, y+bs/2, x+bs/2-1, y+bs-3, ui->theme.text);
        crossos_draw_line(ui->fb, x+bs/2-1, y+bs-3, x+bs-2, y+3, ui->theme.text);
    }

    crossos_draw_text(ui->fb, x + bs + pad,
                      y + (bs - crossos_draw_text_height(ui->scale)) / 2,
                      label ? label : "", ui->theme.text, ui->scale);

    (void)myid;
    if (hov && ui->input.pointer_pressed) {
        *checked = !(*checked);
        return 1;
    }
    return 0;
}

int crossos_ui_radio(crossos_ui_context_t *ui,
                     int x, int y,
                     const char *label, int selected)
{
    if (!ui || !ui->fb) return 0;
    int r   = S(ui, 6);
    int pad = S(ui, 6);
    int tw  = crossos_draw_text_width(label ? label : "", ui->scale);
    int hov = pt_in(ui->input.pointer_x, ui->input.pointer_y,
                    x, y - r, 2*r + pad + tw, 2*r);

    crossos_color_t ring_col = hov ? ui->theme.accent_hover : ui->theme.border;
    crossos_draw_fill_circle(ui->fb, x + r, y,
                             r, selected ? ui->theme.accent : ui->theme.surface);
    /* Outline via stroke circle (approximated as solid ring) */
    crossos_draw_stroke_rect(ui->fb, x, y - r, 2*r, 2*r, 1, ring_col);

    if (selected)
        crossos_draw_fill_circle(ui->fb, x + r, y, r/2, ui->theme.text);

    crossos_draw_text(ui->fb, x + 2*r + pad,
                      y - crossos_draw_text_height(ui->scale)/2,
                      label ? label : "", ui->theme.text, ui->scale);

    return hov && ui->input.pointer_pressed;
}

int crossos_ui_slider(crossos_ui_context_t *ui,
                      int x, int y, int w, int h,
                      float min_v, float max_v,
                      float *value)
{
    if (!ui || !ui->fb || !value) return 0;
    unsigned myid = ++ui->id_counter;

    float range = max_v - min_v;
    if (range <= 0.0f) range = 1.0f;

    int track_cy = y + h / 2;
    int track_th = S(ui, 3) > 1 ? S(ui, 3) : 3;
    int knob_r   = h / 2 - S(ui, 1);
    if (knob_r < 4) knob_r = 4;
    int track_lx = x + knob_r;
    int track_len = w - knob_r * 2;
    if (track_len < 1) track_len = 1;

    /* Handle drag: activate on press anywhere in the widget */
    int hov = pt_in(ui->input.pointer_x, ui->input.pointer_y, x, y, w, h);
    if (hov && ui->input.pointer_pressed)
        ui->active_id = myid;
    if (!ui->input.pointer_down && ui->active_id == myid)
        ui->active_id = 0;

    int changed = 0;
    if (ui->active_id == myid) {
        float nt = (float)(ui->input.pointer_x - track_lx) / (float)track_len;
        if (nt < 0.0f) nt = 0.0f;
        if (nt > 1.0f) nt = 1.0f;
        float nv = min_v + nt * range;
        if (nv != *value) { *value = nv; changed = 1; }
    }

    float t  = (*value - min_v) / range;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    int knob_x = track_lx + (int)(t * track_len);

    /* Track background */
    crossos_draw_fill_rounded_rect(ui->fb,
        track_lx, track_cy - track_th/2, track_len, track_th,
        track_th/2, ui->theme.border);
    /* Filled portion */
    if (knob_x > track_lx)
        crossos_draw_fill_rounded_rect(ui->fb,
            track_lx, track_cy - track_th/2, knob_x - track_lx, track_th,
            track_th/2, ui->theme.accent);
    /* Knob */
    crossos_color_t kc = (ui->active_id == myid || hov)
                         ? ui->theme.accent_hover : ui->theme.accent;
    crossos_draw_fill_circle(ui->fb, knob_x, track_cy, knob_r, kc);
    crossos_draw_stroke_rect(ui->fb,
        knob_x - knob_r, track_cy - knob_r, knob_r * 2, knob_r * 2,
        1, crossos_color_lighten(kc, 30));

    return changed;
}

static int text_input_impl(crossos_ui_context_t *ui,
                           int x, int y, int w, int h,
                           crossos_ui_text_buf_t *buf,
                           const char *placeholder,
                           int password)
{
    if (!ui || !ui->fb || !buf) return 0;
    unsigned myid = ++ui->id_counter;

    int hov = pt_in(ui->input.pointer_x, ui->input.pointer_y, x, y, w, h);
    if (hov && ui->input.pointer_pressed)
        ui->focus_id = myid;

    int focused = (ui->focus_id == myid);
    buf->focused = focused;

    /* Process keyboard input when focused */
    if (focused) {
        unsigned cp = ui->input.char_input;
        if (cp >= 32 && cp < 127 && buf->len < CROSSOS_UI_TEXT_MAX - 1) {
            /* Insert char at cursor */
            memmove(buf->buf + buf->cursor + 1,
                    buf->buf + buf->cursor,
                    (size_t)(buf->len - buf->cursor + 1));
            buf->buf[buf->cursor] = (char)cp;
            buf->len++;
            buf->cursor++;
        }

        int kp = ui->input.key_pressed;
        if (kp == CROSSOS_KEY_BACKSPACE && buf->cursor > 0) {
            memmove(buf->buf + buf->cursor - 1,
                    buf->buf + buf->cursor,
                    (size_t)(buf->len - buf->cursor + 1));
            buf->len--;
            buf->cursor--;
        } else if (kp == CROSSOS_KEY_DELETE && buf->cursor < buf->len) {
            memmove(buf->buf + buf->cursor,
                    buf->buf + buf->cursor + 1,
                    (size_t)(buf->len - buf->cursor));
            buf->len--;
        } else if (kp == CROSSOS_KEY_LEFT && buf->cursor > 0) {
            buf->cursor--;
        } else if (kp == CROSSOS_KEY_RIGHT && buf->cursor < buf->len) {
            buf->cursor++;
        } else if (kp == CROSSOS_KEY_HOME) {
            buf->cursor = 0;
        } else if (kp == CROSSOS_KEY_END) {
            buf->cursor = buf->len;
        }
        buf->buf[buf->len] = '\0';
    }

    /* Draw field background */
    int r  = S(ui, ui->theme.radius);
    int pad = S(ui, 4);
    crossos_color_t bg  = focused ? ui->theme.surface_active : ui->theme.surface;
    crossos_color_t brd = focused ? ui->theme.accent       : ui->theme.border;
    crossos_draw_fill_rounded_rect(ui->fb, x, y, w, h, r, bg);
    crossos_draw_stroke_rounded_rect(ui->fb, x, y, w, h, r, 1, brd);
    if (focused) {
        /* Subtle glow: second border one px out */
        crossos_draw_stroke_rounded_rect(ui->fb, x-1, y-1, w+2, h+2, r+1, 1,
                                         crossos_color_lerp(bg, ui->theme.accent, 80));
    }

    /* Draw text or placeholder */
    int th = crossos_draw_text_height(ui->scale);
    int ty = y + (h - th) / 2;
    if (buf->len > 0) {
        if (password) {
            /* Show bullet for each char */
            char dots[CROSSOS_UI_TEXT_MAX];
            int dl = buf->len < CROSSOS_UI_TEXT_MAX - 1 ? buf->len : CROSSOS_UI_TEXT_MAX - 1;
            for (int i = 0; i < dl; i++) dots[i] = '*';
            dots[dl] = '\0';
            /* Clip to field width */
            crossos_draw_push_clip(x + pad, y, w - pad*2, h);
            crossos_draw_text(ui->fb, x + pad, ty, dots, ui->theme.text, ui->scale);
            crossos_draw_pop_clip();
        } else {
            crossos_draw_push_clip(x + pad, y, w - pad*2, h);
            crossos_draw_text(ui->fb, x + pad, ty, buf->buf, ui->theme.text, ui->scale);
            crossos_draw_pop_clip();
        }
    } else if (placeholder) {
        crossos_draw_push_clip(x + pad, y, w - pad*2, h);
        crossos_draw_text(ui->fb, x + pad, ty, placeholder,
                          ui->theme.text_dim, ui->scale);
        crossos_draw_pop_clip();
    }

    /* Cursor blink: show every other 30 frames */
    if (focused && (ui->frame / 30) % 2 == 0) {
        int cursor_x;
        if (password) {
            cursor_x = x + pad + buf->cursor * 6 * ui->scale;
        } else {
            /* Approximate: count chars before cursor */
            char tmp[CROSSOS_UI_TEXT_MAX];
            int copy_len = buf->cursor < CROSSOS_UI_TEXT_MAX ? buf->cursor : CROSSOS_UI_TEXT_MAX - 1;
            memcpy(tmp, buf->buf, (size_t)copy_len);
            tmp[copy_len] = '\0';
            cursor_x = x + pad + crossos_draw_text_width(tmp, ui->scale);
        }
        crossos_draw_fill_rect(ui->fb, cursor_x, ty, 1, th, ui->theme.text);
    }

    return focused;
}

int crossos_ui_text_input(crossos_ui_context_t *ui,
                          int x, int y, int w, int h,
                          crossos_ui_text_buf_t *buf,
                          const char *placeholder)
{
    return text_input_impl(ui, x, y, w, h, buf, placeholder, 0);
}

int crossos_ui_password_input(crossos_ui_context_t *ui,
                               int x, int y, int w, int h,
                               crossos_ui_text_buf_t *buf,
                               const char *placeholder)
{
    return text_input_impl(ui, x, y, w, h, buf, placeholder, 1);
}

/* ── Progress & status ───────────────────────────────────────────────── */

void crossos_ui_progress_bar(crossos_ui_context_t *ui,
                             int x, int y, int w, int h,
                             float percent,
                             const char *label)
{
    if (!ui || !ui->fb) return;
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 100.0f) percent = 100.0f;

    int r = S(ui, 3);
    crossos_draw_fill_rounded_rect(ui->fb, x, y, w, h, r, ui->theme.surface);
    int fill = (int)((float)(w) * (percent / 100.0f));
    if (fill > 0)
        crossos_draw_fill_rounded_rect(ui->fb, x, y, fill, h, r, ui->theme.success);
    crossos_draw_stroke_rounded_rect(ui->fb, x, y, w, h, r, 1, ui->theme.border);

    if (label) {
        int th = crossos_draw_text_height(ui->scale);
        int ty = y + (h - th) / 2;
        crossos_draw_push_clip(x + 4, y, w - 8, h);
        crossos_draw_text(ui->fb, x + 4, ty, label,
                          (crossos_color_t){230, 236, 240, 255}, ui->scale);
        crossos_draw_pop_clip();
    }
}

/* ── Containers ──────────────────────────────────────────────────────── */

int crossos_ui_dropdown_header(crossos_ui_context_t *ui,
                               int x, int y, int w, int h,
                               const char *label, int opened)
{
    if (!ui || !ui->fb) return 0;
    char line[256];
    snprintf(line, sizeof(line), "%s %s", opened ? "[-]" : "[+]",
             label ? label : "");

    int hov = pt_in(ui->input.pointer_x, ui->input.pointer_y, x, y, w, h);
    int r   = S(ui, ui->theme.radius);
    crossos_color_t bg = hov ? ui->theme.surface_hover : ui->theme.surface;
    crossos_draw_fill_rounded_rect(ui->fb, x, y, w, h, r, bg);
    crossos_draw_stroke_rounded_rect(ui->fb, x, y, w, h, r, 1, ui->theme.border);

    int pad = S(ui, ui->theme.spacing);
    int th  = crossos_draw_text_height(ui->scale);
    crossos_draw_text(ui->fb, x + pad, y + (h - th) / 2,
                      line, ui->theme.text, ui->scale);

    return hov && ui->input.pointer_pressed;
}

int crossos_ui_tree_header(crossos_ui_context_t *ui,
                           int x, int y, int w, int h,
                           const char *label, int expanded, int depth)
{
    char line[256];
    int  off = depth * S(ui, 10);
    snprintf(line, sizeof(line), "%s %s", expanded ? "v" : ">",
             label ? label : "");
    return crossos_ui_selectable(ui, x + off, y, w - off, h, line, 0);
}

int crossos_ui_tab(crossos_ui_context_t *ui,
                   int x, int y, int w, int h,
                   const char *label, int active)
{
    if (!ui || !ui->fb) return 0;
    int hov = pt_in(ui->input.pointer_x, ui->input.pointer_y, x, y, w, h);

    crossos_color_t bg = active ? ui->theme.surface_active
                                : (hov ? ui->theme.surface_hover : ui->theme.surface);
    crossos_draw_fill_rect(ui->fb, x, y, w, h, bg);

    if (active) {
        /* Bottom accent line */
        crossos_draw_fill_rect(ui->fb, x, y + h - 2, w, 2, ui->theme.accent);
    }

    crossos_color_t tc = active ? ui->theme.text : ui->theme.text_dim;
    draw_centered(ui, x, y, w, h, label ? label : "", tc);

    return hov && ui->input.pointer_pressed;
}

/* ── Drop zone ───────────────────────────────────────────────────────── */

int crossos_ui_drop_zone(crossos_ui_context_t *ui,
                         int x, int y, int w, int h,
                         const char *label, int dragging_over)
{
    if (!ui || !ui->fb) return 0;
    int hov = pt_in(ui->input.pointer_x, ui->input.pointer_y, x, y, w, h);
    int r   = S(ui, ui->theme.radius + 2);

    crossos_color_t bg;
    crossos_color_t brd;
    if (dragging_over) {
        bg  = crossos_color_lerp(ui->theme.surface, ui->theme.accent, 30);
        brd = ui->theme.accent;
    } else if (hov) {
        bg  = ui->theme.surface_hover;
        brd = ui->theme.accent_hover;
    } else {
        bg  = ui->theme.surface_alt;
        brd = ui->theme.border;
    }

    crossos_draw_fill_rounded_rect(ui->fb, x, y, w, h, r, bg);

    /* Dashed border when idle */
    if (!dragging_over) {
        int seg = 8, gap = 5, total = seg + gap;
        /* Top */
        for (int px = x; px < x + w; px += total)
            crossos_draw_fill_rect(ui->fb, px, y, seg < (x+w-px) ? seg : x+w-px, 1, brd);
        /* Bottom */
        for (int px = x; px < x + w; px += total)
            crossos_draw_fill_rect(ui->fb, px, y+h-1, seg < (x+w-px) ? seg : x+w-px, 1, brd);
        /* Left */
        for (int py = y; py < y + h; py += total)
            crossos_draw_fill_rect(ui->fb, x, py, 1, seg < (y+h-py) ? seg : y+h-py, brd);
        /* Right */
        for (int py = y; py < y + h; py += total)
            crossos_draw_fill_rect(ui->fb, x+w-1, py, 1, seg < (y+h-py) ? seg : y+h-py, brd);
    } else {
        crossos_draw_stroke_rounded_rect(ui->fb, x, y, w, h, r, 2, brd);
    }

    /* Centre icon (plus sign) */
    int ic = S(ui, 10);
    int cx = x + w/2, cy = y + h/2 - S(ui, 10);
    crossos_draw_fill_rect(ui->fb, cx - ic/2, cy - 1, ic, 2,
                           dragging_over ? ui->theme.accent : ui->theme.text_dim);
    crossos_draw_fill_rect(ui->fb, cx - 1, cy - ic/2, 2, ic,
                           dragging_over ? ui->theme.accent : ui->theme.text_dim);

    /* Label */
    if (label) {
        int th = crossos_draw_text_height(ui->scale);
        int tw = crossos_draw_text_width(label, ui->scale);
        crossos_draw_text(ui->fb, x + (w - tw) / 2,
                          y + h/2 + S(ui, 4),
                          label,
                          dragging_over ? ui->theme.text : ui->theme.text_dim,
                          ui->scale);
        (void)th;
    }

    return hov && ui->input.pointer_pressed;
}

/* ── Scroll area ─────────────────────────────────────────────────────── */

void crossos_ui_scroll_begin(crossos_ui_context_t *ui,
                             int x, int y, int w, int h,
                             crossos_ui_scroll_t *scroll,
                             int content_h)
{
    if (!ui || !scroll) return;

    scroll->view_h    = (float)h;
    scroll->content_h = (float)content_h;

    /* Clamp offset */
    float max_off = (float)(content_h - h);
    if (max_off < 0) max_off = 0;
    if (scroll->offset > max_off) scroll->offset = max_off;
    if (scroll->offset < 0) scroll->offset = 0;

    /* Handle scroll wheel if pointer is inside */
    if (ui->fb && pt_in(ui->input.pointer_x, ui->input.pointer_y, x, y, w, h)) {
        scroll->offset -= ui->input.scroll_dy * S(ui, 20);
        if (scroll->offset < 0) scroll->offset = 0;
        if (scroll->offset > max_off) scroll->offset = max_off;
    }

    /* Push clip to the content area (leaving room for scrollbar on right) */
    int sb_w = (content_h > h) ? S(ui, 8) : 0;
    crossos_draw_push_clip(x, y, w - sb_w, h);
}

void crossos_ui_scroll_end(crossos_ui_context_t *ui,
                           int x, int y, int w, int h,
                           const crossos_ui_scroll_t *scroll)
{
    if (!ui || !scroll) return;
    crossos_draw_pop_clip();

    /* Draw scrollbar if content overflows */
    int content_h = (int)scroll->content_h;
    if (content_h <= h || !ui->fb) return;

    int sb_w  = S(ui, 8);
    int sb_x  = x + w - sb_w;
    float ratio = (float)h / (float)content_h;
    int thumb_h = (int)(ratio * h);
    if (thumb_h < S(ui, 16)) thumb_h = S(ui, 16);
    float max_off = (float)(content_h - h);
    float t = max_off > 0 ? (scroll->offset / max_off) : 0;
    int thumb_y = y + (int)(t * (float)(h - thumb_h));

    crossos_draw_fill_rect(ui->fb, sb_x, y, sb_w, h, ui->theme.surface_alt);
    crossos_draw_fill_rounded_rect(ui->fb, sb_x + 2, thumb_y,
                                   sb_w - 4, thumb_h,
                                   S(ui, 3),
                                   ui->theme.border);
}

/* ── Layout ──────────────────────────────────────────────────────────── */

void crossos_ui_layout_begin_column(crossos_ui_layout_t *layout,
                                    int x, int y, int w, int h, int gap)
{
    if (!layout) return;
    layout->x = x; layout->y = y; layout->w = w; layout->h = h;
    layout->cursor = y; layout->gap = gap; layout->horizontal = 0;
}

int crossos_ui_layout_next(crossos_ui_layout_t *layout,
                           int item_h, crossos_rect_t *out_rect)
{
    if (!layout || !out_rect || item_h <= 0) return 0;
    if (layout->cursor + item_h > layout->y + layout->h) return 0;

    out_rect->x      = layout->x;
    out_rect->y      = layout->cursor;
    out_rect->width  = layout->w;
    out_rect->height = item_h;

    layout->cursor += item_h + layout->gap;
    return 1;
}

void crossos_ui_layout_begin_row(crossos_ui_layout_t *layout,
                                 int x, int y, int w, int h, int gap)
{
    if (!layout) return;
    layout->x = x; layout->y = y; layout->w = w; layout->h = h;
    layout->cursor = x; layout->gap = gap; layout->horizontal = 1;
}

int crossos_ui_layout_row_next(crossos_ui_layout_t *layout,
                               int item_w, crossos_rect_t *out_rect)
{
    if (!layout || !out_rect || item_w <= 0) return 0;
    if (layout->cursor + item_w > layout->x + layout->w) return 0;

    out_rect->x      = layout->cursor;
    out_rect->y      = layout->y;
    out_rect->width  = item_w;
    out_rect->height = layout->h;

    layout->cursor += item_w + layout->gap;
    return 1;
}
