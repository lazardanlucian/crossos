#if defined(__linux__) && !defined(__ANDROID__)

/**
 * Terminal UI Backend for CrossOS
 *
 * Replaces the old pixel framebuffer terminal_backend.c with a character-based
 * rendering approach using Unicode block elements (█) and ANSI truecolor.
 *
 * Window dimensions are defined in character cells rather than pixels.
 * Drawing operations are translated to character grid operations via pixel sampling.
 * Applications continue using the standard crossos_draw_* API unchanged.
 */

#define _POSIX_C_SOURCE 200809L

#include <crossos/crossos.h>
#include "linux_backend.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

extern volatile int crossos__quit_requested;
extern void crossos__set_error(const char *fmt, ...);

/* ─────────────────────────────────────────────────────────────────────────
   Character cell structure - represents one terminal cell with content and colors
 ─────────────────────────────────────────────────────────────────────────── */

typedef struct
{
    uint32_t codepoint; /* Unicode character or 0 for space */
    uint8_t fg_r, fg_g, fg_b;
    uint8_t bg_r, bg_g, bg_b;
} term_cell_t;

/* ─────────────────────────────────────────────────────────────────────────
   Window and surface structures
 ─────────────────────────────────────────────────────────────────────────── */

struct crossos_window
{
    int width;
    int height;
    int visible;
    int is_fullscreen;
    char title[256];
    /* Character grid (width x height cells) */
    term_cell_t *cells;
    /* Real framebuffer: 32-bit BGRA pixels at scaled resolution for drawing compatibility
       Apps draw to this, then we sample it during present() to update cells */
    unsigned char *fb_pixels;
    int fb_stride;
    int fb_width;
    int fb_height;
    struct crossos_surface *surface;
};

struct crossos_surface
{
    crossos_window_t *win;
    int locked;
};

/* ─────────────────────────────────────────────────────────────────────────
   Event queue
 ─────────────────────────────────────────────────────────────────────────── */

#define QUEUE_CAP 256

static crossos_event_t s_queue[QUEUE_CAP];
static int s_head = 0;
static int s_tail = 0;

/* ─────────────────────────────────────────────────────────────────────────
   Terminal state management
 ─────────────────────────────────────────────────────────────────────────── */

static struct termios s_saved_termios;
static int s_termios_saved = 0;
static void (*s_saved_winch_handler)(int) = SIG_DFL;
static int s_saved_winch_valid = 0;
static volatile sig_atomic_t s_resize_pending = 0;
static crossos_window_t *s_primary_window = NULL;
static char s_input_buf[512];
static size_t s_input_len = 0;

/* ─────────────────────────────────────────────────────────────────────────
   Signal handlers
 ─────────────────────────────────────────────────────────────────────────── */

static void on_sigwinch(int signo)
{
    (void)signo;
    s_resize_pending = 1;
}

/* ─────────────────────────────────────────────────────────────────────────
   Event queue management
 ─────────────────────────────────────────────────────────────────────────── */

static void push_event(const crossos_event_t *ev)
{
    int next = (s_head + 1) % QUEUE_CAP;
    if (next == s_tail)
        return;
    s_queue[s_head] = *ev;
    s_head = next;
}

static int pop_event(crossos_event_t *ev)
{
    if (s_tail == s_head)
        return 0;
    *ev = s_queue[s_tail];
    s_tail = (s_tail + 1) % QUEUE_CAP;
    return 1;
}

/* ─────────────────────────────────────────────────────────────────────────
   Terminal utilities
 ─────────────────────────────────────────────────────────────────────────── */

static void terminal_write(const char *bytes)
{
    if (!bytes)
        return;
    ssize_t rc = write(STDOUT_FILENO, bytes, strlen(bytes));
    (void)rc;
}

static void set_terminal_title(const char *title)
{
    char buf[320];
    const char *safe_title = title ? title : "CrossOS";
    int len = snprintf(buf, sizeof(buf), "\033]0;%s\007", safe_title);
    if (len > 0)
    {
        ssize_t rc = write(STDOUT_FILENO, buf, (size_t)len);
        (void)rc;
    }
}

static void query_terminal_cells(int *cols, int *rows)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0)
    {
        if (cols)
            *cols = (int)ws.ws_col - 1; /* Leave margin for safety */
        if (rows)
            *rows = (int)ws.ws_row - 1;
        return;
    }
    if (cols)
        *cols = 78; /* 80 - 2 margin */
    if (rows)
        *rows = 22; /* 24 - 2 margin */
}

/* ─────────────────────────────────────────────────────────────────────────
   UTF-8 encoding utilities
 ─────────────────────────────────────────────────────────────────────────── */

static int utf8_encode(uint32_t codepoint, char *buf, int buf_sz)
{
    if (codepoint < 0x80)
    {
        if (buf_sz >= 1)
        {
            buf[0] = (char)codepoint;
            return 1;
        }
        return 0;
    }
    if (codepoint < 0x800)
    {
        if (buf_sz >= 2)
        {
            buf[0] = (char)(0xC0 | (codepoint >> 6));
            buf[1] = (char)(0x80 | (codepoint & 0x3F));
            return 2;
        }
        return 0;
    }
    if (codepoint < 0x10000)
    {
        if (buf_sz >= 3)
        {
            buf[0] = (char)(0xE0 | (codepoint >> 12));
            buf[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
            buf[2] = (char)(0x80 | (codepoint & 0x3F));
            return 3;
        }
        return 0;
    }
    if (codepoint < 0x110000)
    {
        if (buf_sz >= 4)
        {
            buf[0] = (char)(0xF0 | (codepoint >> 18));
            buf[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
            buf[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
            buf[3] = (char)(0x80 | (codepoint & 0x3F));
            return 4;
        }
        return 0;
    }
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────
   Cell drawing utilities
 ─────────────────────────────────────────────────────────────────────────── */

static void clear_cell(term_cell_t *cell)
{
    cell->codepoint = 0;
    cell->fg_r = cell->fg_g = cell->fg_b = 0;
    cell->bg_r = cell->bg_g = cell->bg_b = 0;
}

static void set_cell(term_cell_t *cell, uint32_t codepoint,
                     uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                     uint8_t bg_r, uint8_t bg_g, uint8_t bg_b)
{
    cell->codepoint = codepoint;
    cell->fg_r = fg_r;
    cell->fg_g = fg_g;
    cell->fg_b = fg_b;
    cell->bg_r = bg_r;
    cell->bg_g = bg_g;
    cell->bg_b = bg_b;
}

static void fill_region(term_cell_t *cells, int stride,
                        int x0, int y0, int x1, int y1,
                        uint32_t codepoint,
                        uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                        uint8_t bg_r, uint8_t bg_g, uint8_t bg_b)
{
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    for (int y = y0; y < y1; y++)
    {
        for (int x = x0; x < x1; x++)
        {
            set_cell(&cells[y * stride + x], codepoint,
                     fg_r, fg_g, fg_b, bg_r, bg_g, bg_b);
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────
   Drawing primitives mapped to character cells
 ─────────────────────────────────────────────────────────────────────────── */

/* Convert pixel coordinates to cell coordinates, scaling down from logical space */
static void pixel_to_cell(int px, int py, int logical_w, int logical_h,
                          int cell_w, int cell_h,
                          int *out_cx, int *out_cy)
{
    *out_cx = (px * cell_w) / logical_w;
    *out_cy = (py * cell_h) / logical_h;
}

/* Sample average color from a rectangle in the pixel framebuffer */
static void sample_pixel_region(const crossos_window_t *win,
                                int px0, int py0, int px1, int py1,
                                uint8_t out_rgb[3])
{
    uint64_t sum_r = 0, sum_g = 0, sum_b = 0;
    uint64_t count = 0;

    if (!win || !win->fb_pixels || win->fb_width <= 0 || win->fb_height <= 0)
    {
        out_rgb[0] = out_rgb[1] = out_rgb[2] = 0;
        return;
    }

    if (px0 < 0)
        px0 = 0;
    if (py0 < 0)
        py0 = 0;
    if (px1 > win->fb_width)
        px1 = win->fb_width;
    if (py1 > win->fb_height)
        py1 = win->fb_height;
    if (px1 <= px0)
        px1 = px0 + 1;
    if (py1 <= py0)
        py1 = py0 + 1;

    for (int y = py0; y < py1; y++)
    {
        const uint8_t *row = win->fb_pixels + y * win->fb_stride;
        for (int x = px0; x < px1; x++)
        {
            /* BGRA format: [B][G][R][A] */
            const uint8_t *px = row + x * 4;
            sum_b += px[0];
            sum_g += px[1];
            sum_r += px[2];
            /* Skip alpha (px[3]) */
            count++;
        }
    }

    if (count == 0)
    {
        out_rgb[0] = out_rgb[1] = out_rgb[2] = 0;
        return;
    }

    out_rgb[0] = (uint8_t)(sum_r / count);
    out_rgb[1] = (uint8_t)(sum_g / count);
    out_rgb[2] = (uint8_t)(sum_b / count);
}

/* Draw a filled rectangle as solid block characters */
static void draw_fill_rect_cells(crossos_window_t *win,
                                 int x0, int y0, int x1, int y1,
                                 uint8_t r, uint8_t g, uint8_t b)
{
    if (x0 >= x1 || y0 >= y1)
        return;

    fill_region(win->cells, win->width, x0, y0, x1, y1, 0x2588,
                r, g, b, r, g, b);
}

/* Draw a rectangle outline using box-drawing characters */
static void draw_stroke_rect_cells(crossos_window_t *win,
                                   int x0, int y0, int x1, int y1,
                                   uint8_t r, uint8_t g, uint8_t b)
{
    if (x0 >= x1 || y0 >= y1)
        return;

    /* Fill horizontal edges */
    for (int x = x0; x < x1; x++)
    {
        if (y0 < win->height)
            set_cell(&win->cells[y0 * win->width + x], 0x2500, r, g, b, 0, 0, 0);
        if (y1 - 1 >= 0 && y1 - 1 < win->height)
            set_cell(&win->cells[(y1 - 1) * win->width + x], 0x2500, r, g, b, 0, 0, 0);
    }

    /* Fill vertical edges */
    for (int y = y0; y < y1; y++)
    {
        if (x0 < win->width)
            set_cell(&win->cells[y * win->width + x0], 0x2502, r, g, b, 0, 0, 0);
        if (x1 - 1 >= 0 && x1 - 1 < win->width)
            set_cell(&win->cells[y * win->width + (x1 - 1)], 0x2502, r, g, b, 0, 0, 0);
    }

    /* Corners */
    if (y0 < win->height && x0 < win->width)
        set_cell(&win->cells[y0 * win->width + x0], 0x250C, r, g, b, 0, 0, 0);
    if (y0 < win->height && x1 - 1 >= 0 && x1 - 1 < win->width)
        set_cell(&win->cells[y0 * win->width + (x1 - 1)], 0x2510, r, g, b, 0, 0, 0);
    if (y1 - 1 >= 0 && y1 - 1 < win->height && x0 < win->width)
        set_cell(&win->cells[(y1 - 1) * win->width + x0], 0x2514, r, g, b, 0, 0, 0);
    if (y1 - 1 >= 0 && y1 - 1 < win->height && x1 - 1 >= 0 && x1 - 1 < win->width)
        set_cell(&win->cells[(y1 - 1) * win->width + (x1 - 1)], 0x2518, r, g, b, 0, 0, 0);
}

/* Draw text as ASCII characters in the cell grid */
static void draw_text_cells(crossos_window_t *win, int x, int y,
                            const char *text,
                            uint8_t r, uint8_t g, uint8_t b)
{
    if (!text || x < 0 || y < 0 || y >= win->height)
        return;

    for (int i = 0; text[i] && x + i < win->width; i++)
    {
        unsigned char c = (unsigned char)text[i];
        uint32_t codepoint = (c >= 32 && c < 127) ? c : '?';
        set_cell(&win->cells[y * win->width + x + i], codepoint, r, g, b, 0, 0, 0);
    }
}

/* ─────────────────────────────────────────────────────────────────────────
   Backend initialization and platform interface
 ─────────────────────────────────────────────────────────────────────────── */

static crossos_result_t term_ui_platform_init(void)
{
    struct termios tm;
    struct sigaction sa;

    /* Set up raw terminal mode */
    if (tcgetattr(STDIN_FILENO, &tm) < 0)
    {
        crossos__set_error("tcgetattr failed");
        return CROSSOS_ERR_INIT;
    }

    s_saved_termios = tm;
    s_termios_saved = 1;

    tm.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tm.c_oflag &= ~OPOST;
    tm.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tm.c_cflag &= ~(CSIZE | PARENB);
    tm.c_cflag |= CS8;

    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &tm) < 0)
    {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &s_saved_termios);
        s_termios_saved = 0;
        crossos__set_error("tcsetattr failed");
        return CROSSOS_ERR_INIT;
    }

    /* Set up SIGWINCH handler for resize events */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigwinch;
    if (sigaction(SIGWINCH, &sa, NULL) < 0)
    {
        crossos__set_error("sigaction failed");
        tcsetattr(STDIN_FILENO, TCSADRAIN, &s_saved_termios);
        s_termios_saved = 0;
        return CROSSOS_ERR_INIT;
    }

    /* Enable alternate buffer and mouse reporting */
    terminal_write("\033[?1049h"); /* Alternate buffer */
    terminal_write("\033[?1002h"); /* Mouse reporting */
    terminal_write("\033[?7l");    /* Disable autowrap */

    return CROSSOS_OK;
}

static void term_ui_platform_shutdown(void)
{
    terminal_write("\033[?7h");    /* Re-enable autowrap */
    terminal_write("\033[?1002l"); /* Disable mouse */
    terminal_write("\033[?1049l"); /* Normal buffer */
    terminal_write("\033[0m");     /* Reset colors */

    if (s_termios_saved)
    {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &s_saved_termios);
        s_termios_saved = 0;
    }
}

/* ─────────────────────────────────────────────────────────────────────────
   Window management
 ─────────────────────────────────────────────────────────────────────────── */

static crossos_window_t *term_ui_window_create(const char *title,
                                               int width, int height,
                                               uint32_t flags)
{
    crossos_window_t *win;
    int cols, rows;

    (void)flags;

    query_terminal_cells(&cols, &rows);

    /* Limit window to terminal size */
    if (width > cols)
        width = cols;
    if (height > rows)
        height = rows;

    win = (crossos_window_t *)malloc(sizeof(crossos_window_t));
    if (!win)
    {
        crossos__set_error("malloc failed");
        return NULL;
    }

    win->width = width;
    win->height = height;
    win->visible = 0;
    win->is_fullscreen = 0;
    strncpy(win->title, title ? title : "", sizeof(win->title) - 1);
    win->title[sizeof(win->title) - 1] = '\0';

    /* Allocate character grid */
    win->cells = (term_cell_t *)calloc(width * height, sizeof(term_cell_t));
    if (!win->cells)
    {
        free(win);
        crossos__set_error("malloc failed for cells");
        return NULL;
    }

    /* Allocate real BGRA framebuffer for drawing compatibility
       Scale up from character cells to provide reasonable drawing resolution */
    win->fb_width = width * 8;    /* 8 pixels per character width */
    win->fb_height = height * 16; /* 16 pixels per character height */
    win->fb_pixels = (unsigned char *)calloc(win->fb_width * win->fb_height * 4, 1);
    if (!win->fb_pixels)
    {
        free(win->cells);
        free(win);
        crossos__set_error("malloc failed for framebuffer");
        return NULL;
    }
    win->fb_stride = win->fb_width * 4;

    win->surface = (struct crossos_surface *)malloc(sizeof(struct crossos_surface));
    if (!win->surface)
    {
        free(win->fb_pixels);
        free(win->cells);
        free(win);
        crossos__set_error("malloc failed for surface");
        return NULL;
    }

    win->surface->win = win;
    win->surface->locked = 0;
    s_primary_window = win;

    return win;
}

static void term_ui_window_destroy(crossos_window_t *win)
{
    if (!win)
        return;
    if (win->surface)
        free(win->surface);
    if (win->cells)
        free(win->cells);
    if (win->fb_pixels)
        free(win->fb_pixels);
    free(win);
    if (s_primary_window == win)
        s_primary_window = NULL;
}

static void term_ui_window_show(crossos_window_t *win)
{
    if (win)
        win->visible = 1;
}

static void term_ui_window_hide(crossos_window_t *win)
{
    if (win)
        win->visible = 0;
}

static crossos_result_t term_ui_window_set_fullscreen(crossos_window_t *win,
                                                      int fullscreen)
{
    if (win)
        win->is_fullscreen = fullscreen ? 1 : 0;
    return CROSSOS_OK;
}

static crossos_result_t term_ui_window_resize(crossos_window_t *win,
                                              int width, int height)
{
    term_cell_t *new_cells;
    unsigned char *new_pixels;
    int cols, rows;
    int new_fb_width, new_fb_height;

    if (!win || width <= 0 || height <= 0)
        return CROSSOS_ERR_PARAM;

    query_terminal_cells(&cols, &rows);
    if (width > cols)
        width = cols;
    if (height > rows)
        height = rows;

    new_cells = (term_cell_t *)calloc(width * height, sizeof(term_cell_t));
    if (!new_cells)
        return CROSSOS_ERR_OOM;

    new_fb_width = width * 8;
    new_fb_height = height * 16;
    new_pixels = (unsigned char *)calloc(new_fb_width * new_fb_height * 4, 1);
    if (!new_pixels)
    {
        free(new_cells);
        return CROSSOS_ERR_OOM;
    }

    if (win->cells)
        free(win->cells);
    if (win->fb_pixels)
        free(win->fb_pixels);

    win->width = width;
    win->height = height;
    win->cells = new_cells;
    win->fb_pixels = new_pixels;
    win->fb_width = new_fb_width;
    win->fb_height = new_fb_height;
    win->fb_stride = new_fb_width * 4;

    return CROSSOS_OK;
}

static crossos_result_t term_ui_window_set_title(crossos_window_t *win,
                                                 const char *title)
{
    if (!win)
        return CROSSOS_ERR_PARAM;
    strncpy(win->title, title ? title : "", sizeof(win->title) - 1);
    win->title[sizeof(win->title) - 1] = '\0';
    set_terminal_title(title);
    return CROSSOS_OK;
}

static void term_ui_window_get_size(const crossos_window_t *win,
                                    int *width, int *height)
{
    if (win)
    {
        if (width)
            *width = win->width;
        if (height)
            *height = win->height;
    }
    else
    {
        if (width)
            *width = 0;
        if (height)
            *height = 0;
    }
}

static int term_ui_window_is_fullscreen(const crossos_window_t *win)
{
    return win ? win->is_fullscreen : 0;
}

static void *term_ui_window_get_native_handle(const crossos_window_t *win)
{
    (void)win;
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────
   Display information
 ─────────────────────────────────────────────────────────────────────────── */

static crossos_result_t term_ui_display_get_size(int display_index,
                                                 int *width, int *height)
{
    if (display_index != 0)
        return CROSSOS_ERR_PARAM;

    query_terminal_cells(width, height);
    return CROSSOS_OK;
}

static int term_ui_display_count(void)
{
    return 1;
}

/* ─────────────────────────────────────────────────────────────────────────
   Surface and framebuffer management
 ─────────────────────────────────────────────────────────────────────────── */

static crossos_surface_t *term_ui_surface_get(crossos_window_t *win)
{
    if (!win)
        return NULL;
    return win->surface;
}

static crossos_result_t term_ui_surface_lock(crossos_surface_t *surf,
                                             crossos_framebuffer_t *fb)
{
    if (!surf || !fb || !surf->win)
        return CROSSOS_ERR_PARAM;

    if (surf->locked)
        return CROSSOS_ERR_WINDOW;

    surf->locked = 1;
    /* Return the real BGRA pixel framebuffer to the app */
    fb->pixels = surf->win->fb_pixels;
    fb->width = surf->win->fb_width;
    fb->height = surf->win->fb_height;
    fb->stride = surf->win->fb_stride;
    fb->format = CROSSOS_PIXEL_FMT_BGRA8888;

    return CROSSOS_OK;
}

static void term_ui_surface_unlock(crossos_surface_t *surf)
{
    if (surf)
        surf->locked = 0;
}

static crossos_result_t term_ui_surface_present(crossos_surface_t *surf)
{
    crossos_window_t *win;
    int cols, rows;
    size_t cap;
    char *out;
    char *p;

    if (!surf || !surf->win)
        return CROSSOS_ERR_PARAM;

    win = surf->win;
    if (!win->visible)
        return CROSSOS_OK;

    query_terminal_cells(&cols, &rows);
    if (cols <= 0 || rows <= 0)
        return CROSSOS_ERR_DISPLAY;

    /* Sample from pixel framebuffer and convert to character cells */
    for (int cy = 0; cy < win->height && cy < rows; cy++)
    {
        /* Map character row to pixel region */
        int py0 = (cy * win->fb_height) / win->height;
        int py1 = ((cy + 1) * win->fb_height) / win->height;
        if (py1 > win->fb_height)
            py1 = win->fb_height;

        for (int cx = 0; cx < win->width && cx < cols; cx++)
        {
            /* Map character column to pixel region */
            int px0 = (cx * win->fb_width) / win->width;
            int px1 = ((cx + 1) * win->fb_width) / win->width;
            if (px1 > win->fb_width)
                px1 = win->fb_width;

            uint8_t rgb[3];
            sample_pixel_region(win, px0, py0, px1, py1, rgb);

            term_cell_t *cell = &win->cells[cy * win->width + cx];
            cell->codepoint = 0x2588; /* Full block */
            cell->fg_r = rgb[0];
            cell->fg_g = rgb[1];
            cell->fg_b = rgb[2];
            cell->bg_r = 0;
            cell->bg_g = 0;
            cell->bg_b = 0;
        }
    }

    /* Now output the character grid via ANSI */
    cap = 32u + (size_t)rows * ((size_t)cols * 48u + 8u);
    out = (char *)malloc(cap);
    if (!out)
        return CROSSOS_ERR_OOM;

    p = out;
    p += snprintf(p, cap, "\033[H"); /* Home */

    /* Output each cell */
    for (int y = 0; y < win->height && y < rows; y++)
    {
        for (int x = 0; x < win->width && x < cols; x++)
        {
            const term_cell_t *cell = &win->cells[y * win->width + x];
            uint32_t cp = cell->codepoint ? cell->codepoint : ' ';
            char utf8_buf[8];
            int utf8_len = utf8_encode(cp, utf8_buf, sizeof(utf8_buf));

            /* Format: \033[38;2;R;G;Bm\033[48;2;R;G;Bm<char> */
            p += snprintf(p, cap - (size_t)(p - out),
                          "\033[38;2;%u;%u;%um\033[48;2;%u;%u;%um",
                          cell->fg_r, cell->fg_g, cell->fg_b,
                          cell->bg_r, cell->bg_g, cell->bg_b);

            if (utf8_len > 0)
            {
                memcpy(p, utf8_buf, utf8_len);
                p += utf8_len;
            }
        }
        if (y + 1 < win->height && y + 1 < rows)
        {
            p += snprintf(p, cap - (size_t)(p - out), "\033[0m\r\n");
        }
    }
    p += snprintf(p, cap - (size_t)(p - out), "\033[0m");

    ssize_t rc = write(STDOUT_FILENO, out, (size_t)(p - out));
    (void)rc;
    fflush(stdout);
    free(out);

    return CROSSOS_OK;
}

/* ─────────────────────────────────────────────────────────────────────────
   Event handling (stub - for now, just basic input)
 ─────────────────────────────────────────────────────────────────────────── */

static void process_input_bytes(void)
{
    char buf[256];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0)
        return;

    for (ssize_t i = 0; i < n; i++)
    {
        if (s_input_len < sizeof(s_input_buf) - 1)
            s_input_buf[s_input_len++] = buf[i];
    }
}

static int term_ui_poll_event(crossos_event_t *ev)
{
    if (!ev)
        return 0;

    process_input_bytes();

    if (s_resize_pending && s_primary_window)
    {
        s_resize_pending = 0;
        memset(ev, 0, sizeof(*ev));
        ev->type = CROSSOS_EVENT_WINDOW_RESIZE;
        ev->window = s_primary_window;
        return 1;
    }

    if (crossos__quit_requested && s_tail == s_head)
    {
        memset(ev, 0, sizeof(*ev));
        ev->type = CROSSOS_EVENT_QUIT;
        ev->window = s_primary_window;
        return 1;
    }

    return pop_event(ev);
}

static int term_ui_wait_event(crossos_event_t *ev)
{
    /* Simple polling wait */
    for (int i = 0; i < 100; i++)
    {
        if (term_ui_poll_event(ev))
            return 1;
        usleep(10000);
    }
    return 0;
}

static void term_ui_run_loop(crossos_window_t *win,
                             crossos_event_cb_t cb,
                             void *user_data)
{
    crossos_event_t ev;
    if (!win)
        return;

    while (!crossos__quit_requested)
    {
        if (term_ui_wait_event(&ev))
        {
            if (cb)
                cb(&ev, user_data);
        }
    }
}

static int term_ui_touch_get_active(const crossos_window_t *win,
                                    crossos_touch_point_t pts[CROSSOS_MAX_TOUCH_POINTS])
{
    (void)win;
    (void)pts;
    return 0;
}

static int term_ui_touch_is_supported(void)
{
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────
   Backend vtable
 ─────────────────────────────────────────────────────────────────────────── */

const crossos_linux_backend_vtable_t crossos__linux_terminal_backend = {
    .platform_init = term_ui_platform_init,
    .platform_shutdown = term_ui_platform_shutdown,
    .window_create = term_ui_window_create,
    .window_destroy = term_ui_window_destroy,
    .window_show = term_ui_window_show,
    .window_hide = term_ui_window_hide,
    .window_set_fullscreen = term_ui_window_set_fullscreen,
    .window_resize = term_ui_window_resize,
    .window_set_title = term_ui_window_set_title,
    .window_get_size = term_ui_window_get_size,
    .window_is_fullscreen = term_ui_window_is_fullscreen,
    .window_get_native_handle = term_ui_window_get_native_handle,
    .display_get_size = term_ui_display_get_size,
    .display_count = term_ui_display_count,
    .surface_get = term_ui_surface_get,
    .surface_lock = term_ui_surface_lock,
    .surface_unlock = term_ui_surface_unlock,
    .surface_present = term_ui_surface_present,
    .poll_event = term_ui_poll_event,
    .wait_event = term_ui_wait_event,
    .run_loop = term_ui_run_loop,
    .touch_get_active = term_ui_touch_get_active,
    .touch_is_supported = term_ui_touch_is_supported,
};

#endif /* __linux__ && !__ANDROID__ */
