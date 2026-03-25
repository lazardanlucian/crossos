#if defined(__linux__) && !defined(__ANDROID__)

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

struct crossos_window
{
    int width;
    int height;
    int visible;
    int is_fullscreen;
    char title[256];
    unsigned char *fb_pixels;
    int fb_stride;
    struct crossos_surface *surface;
};

struct crossos_surface
{
    crossos_window_t *win;
    int locked;
};

#define QUEUE_CAP 256

static crossos_event_t s_queue[QUEUE_CAP];
static int s_head = 0;
static int s_tail = 0;

static struct termios s_saved_termios;
static int s_termios_saved = 0;
static void (*s_saved_winch_handler)(int) = SIG_DFL;
static int s_saved_winch_valid = 0;
static volatile sig_atomic_t s_resize_pending = 0;
static crossos_window_t *s_primary_window = NULL;
static char s_input_buf[512];
static size_t s_input_len = 0;
static int s_use_smooth_sampling = 1;

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

static void on_sigwinch(int signo)
{
    (void)signo;
    s_resize_pending = 1;
}

static void terminal_write(const char *bytes)
{
    ssize_t rc;
    if (!bytes)
        return;
    rc = write(STDOUT_FILENO, bytes, strlen(bytes));
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
            *cols = (int)ws.ws_col;
        if (rows)
            *rows = (int)ws.ws_row;
        return;
    }
    if (cols)
        *cols = 80;
    if (rows)
        *rows = 24;
}

static int env_is_true(const char *name)
{
    const char *v = getenv(name);
    if (!v || !v[0])
        return 0;
    return strcmp(v, "1") == 0 ||
           strcasecmp(v, "true") == 0 ||
           strcasecmp(v, "yes") == 0 ||
           strcasecmp(v, "on") == 0;
}

static void sample_region_avg(const crossos_window_t *win,
                              int x0,
                              int y0,
                              int x1,
                              int y1,
                              unsigned char out_rgb[3])
{
    uint64_t sum_r = 0;
    uint64_t sum_g = 0;
    uint64_t sum_b = 0;
    uint64_t count = 0;

    if (!win || !win->fb_pixels || win->width <= 0 || win->height <= 0)
    {
        out_rgb[0] = out_rgb[1] = out_rgb[2] = 0;
        return;
    }

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > win->width)
        x1 = win->width;
    if (y1 > win->height)
        y1 = win->height;
    if (x1 <= x0)
        x1 = x0 + 1;
    if (y1 <= y0)
        y1 = y0 + 1;
    if (x1 > win->width)
        x1 = win->width;
    if (y1 > win->height)
        y1 = win->height;

    for (int y = y0; y < y1; y++)
    {
        const unsigned char *row = win->fb_pixels + y * win->fb_stride;
        for (int x = x0; x < x1; x++)
        {
            const unsigned char *px = row + x * 4;
            sum_b += (uint64_t)px[0];
            sum_g += (uint64_t)px[1];
            sum_r += (uint64_t)px[2];
            count++;
        }
    }

    if (count == 0)
    {
        out_rgb[0] = out_rgb[1] = out_rgb[2] = 0;
        return;
    }

    out_rgb[0] = (unsigned char)(sum_r / count);
    out_rgb[1] = (unsigned char)(sum_g / count);
    out_rgb[2] = (unsigned char)(sum_b / count);
}

static void sample_region_nearest(const crossos_window_t *win,
                                  int x0,
                                  int y0,
                                  int x1,
                                  int y1,
                                  unsigned char out_rgb[3])
{
    int x = (x0 + x1) / 2;
    int y = (y0 + y1) / 2;
    const unsigned char *px;

    if (!win || !win->fb_pixels || win->width <= 0 || win->height <= 0)
    {
        out_rgb[0] = out_rgb[1] = out_rgb[2] = 0;
        return;
    }

    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= win->width)
        x = win->width - 1;
    if (y >= win->height)
        y = win->height - 1;

    px = win->fb_pixels + y * win->fb_stride + x * 4;
    out_rgb[0] = px[2];
    out_rgb[1] = px[1];
    out_rgb[2] = px[0];
}

static void query_logical_size(int *width, int *height)
{
    int cols = 80;
    int rows = 24;
    query_terminal_cells(&cols, &rows);
    if (width)
        *width = cols > 0 ? cols * 8 : 640;
    if (height)
        *height = rows > 0 ? rows * 16 : 384;
}

static crossos_result_t alloc_window_framebuffer(crossos_window_t *win, int width, int height)
{
    unsigned char *pixels;

    if (!win)
        return CROSSOS_ERR_PARAM;
    if (width <= 0 || height <= 0)
        return CROSSOS_ERR_PARAM;
    if (width == win->width && height == win->height && win->fb_pixels)
        return CROSSOS_OK;

    pixels = (unsigned char *)calloc((size_t)width * (size_t)height, 4);
    if (!pixels)
    {
        crossos__set_error("terminal backend: framebuffer OOM");
        return CROSSOS_ERR_OOM;
    }

    free(win->fb_pixels);
    win->fb_pixels = pixels;
    win->width = width;
    win->height = height;
    win->fb_stride = width * 4;

    return CROSSOS_OK;
}

static void push_resize_event(crossos_window_t *win)
{
    if (win)
    {
        crossos_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = CROSSOS_EVENT_WINDOW_RESIZE;
        ev.window = win;
        ev.resize.width = win->width;
        ev.resize.height = win->height;
        push_event(&ev);
    }
}

static void flush_resize_if_needed(void)
{
    if (s_resize_pending && s_primary_window)
    {
        s_resize_pending = 0;
        push_resize_event(s_primary_window);
    }
}

static void emit_key_event(crossos_window_t *win,
                           int keycode,
                           crossos_key_mod_t mods,
                           int emit_char,
                           unsigned codepoint)
{
    crossos_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CROSSOS_EVENT_KEY_DOWN;
    ev.window = win;
    ev.key.keycode = keycode;
    ev.key.mods = mods;
    ev.key.repeat = 0;
    push_event(&ev);

    if (emit_char)
    {
        memset(&ev, 0, sizeof(ev));
        ev.type = CROSSOS_EVENT_CHAR;
        ev.window = win;
        ev.character.codepoint = codepoint;
        push_event(&ev);
    }
}

static void emit_pointer_event(crossos_window_t *win,
                               crossos_event_type_t type,
                               int button,
                               int term_col,
                               int term_row,
                               float scroll_y)
{
    int cols = 0;
    int rows = 0;
    float x = 0.0f;
    float y = 0.0f;
    crossos_event_t ev;

    query_terminal_cells(&cols, &rows);
    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;

    if (win)
    {
        if (term_col < 0)
            term_col = 0;
        if (term_col >= cols)
            term_col = cols - 1;
        if (term_row < 0)
            term_row = 0;
        if (term_row >= rows)
            term_row = rows - 1;
        x = ((float)term_col + 0.5f) * (float)win->width / (float)cols;
        y = ((float)term_row + 0.5f) * (float)win->height / (float)rows;
    }

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.window = win;
    ev.pointer.button = button;
    ev.pointer.x = x;
    ev.pointer.y = y;
    ev.pointer.scroll_y = scroll_y;
    push_event(&ev);
}

static int parse_mouse_sequence(crossos_window_t *win, const char *buf, size_t len, size_t *consumed)
{
    size_t i = 3;
    int button = 0;
    int col = 0;
    int row = 0;
    int release = 0;

    while (i < len && isdigit((unsigned char)buf[i]))
    {
        button = button * 10 + (buf[i] - '0');
        i++;
    }
    if (i >= len || buf[i] != ';')
        return 0;
    i++;

    while (i < len && isdigit((unsigned char)buf[i]))
    {
        col = col * 10 + (buf[i] - '0');
        i++;
    }
    if (i >= len || buf[i] != ';')
        return 0;
    i++;

    while (i < len && isdigit((unsigned char)buf[i]))
    {
        row = row * 10 + (buf[i] - '0');
        i++;
    }
    if (i >= len)
        return -1;

    release = (buf[i] == 'm');
    if (!release && buf[i] != 'M')
        return 0;

    *consumed = i + 1;

    if (button >= 64 && button <= 65)
    {
        emit_pointer_event(win,
                           CROSSOS_EVENT_POINTER_SCROLL,
                           0,
                           col - 1,
                           row - 1,
                           button == 64 ? 1.0f : -1.0f);
        return 1;
    }

    if (button & 32)
    {
        emit_pointer_event(win,
                           CROSSOS_EVENT_POINTER_MOVE,
                           0,
                           col - 1,
                           row - 1,
                           0.0f);
        return 1;
    }

    emit_pointer_event(win,
                       release ? CROSSOS_EVENT_POINTER_UP : CROSSOS_EVENT_POINTER_DOWN,
                       (button & 3) + 1,
                       col - 1,
                       row - 1,
                       0.0f);
    return 1;
}

static int parse_escape_sequence(crossos_window_t *win,
                                 const char *buf,
                                 size_t len,
                                 size_t *consumed)
{
    if (len < 2)
        return -1;
    if (buf[1] == '[')
    {
        if (len >= 3 && buf[2] == '<')
        {
            return parse_mouse_sequence(win, buf, len, consumed);
        }
        if (len < 3)
            return -1;
        if (buf[2] == 'A')
        {
            emit_key_event(win, CROSSOS_KEY_UP, CROSSOS_MOD_NONE, 0, 0);
            *consumed = 3;
            return 1;
        }
        if (buf[2] == 'B')
        {
            emit_key_event(win, CROSSOS_KEY_DOWN, CROSSOS_MOD_NONE, 0, 0);
            *consumed = 3;
            return 1;
        }
        if (buf[2] == 'C')
        {
            emit_key_event(win, CROSSOS_KEY_RIGHT, CROSSOS_MOD_NONE, 0, 0);
            *consumed = 3;
            return 1;
        }
        if (buf[2] == 'D')
        {
            emit_key_event(win, CROSSOS_KEY_LEFT, CROSSOS_MOD_NONE, 0, 0);
            *consumed = 3;
            return 1;
        }
        if (buf[2] == 'H')
        {
            emit_key_event(win, CROSSOS_KEY_HOME, CROSSOS_MOD_NONE, 0, 0);
            *consumed = 3;
            return 1;
        }
        if (buf[2] == 'F')
        {
            emit_key_event(win, CROSSOS_KEY_END, CROSSOS_MOD_NONE, 0, 0);
            *consumed = 3;
            return 1;
        }
        if (isdigit((unsigned char)buf[2]))
        {
            size_t i = 2;
            int value = 0;
            while (i < len && isdigit((unsigned char)buf[i]))
            {
                value = value * 10 + (buf[i] - '0');
                i++;
            }
            if (i >= len)
                return -1;
            if (buf[i] != '~')
                return 0;
            switch (value)
            {
            case 2:
                emit_key_event(win, CROSSOS_KEY_INSERT, CROSSOS_MOD_NONE, 0, 0);
                break;
            case 3:
                emit_key_event(win, CROSSOS_KEY_DELETE, CROSSOS_MOD_NONE, 0, 0);
                break;
            case 5:
                emit_key_event(win, CROSSOS_KEY_PAGE_UP, CROSSOS_MOD_NONE, 0, 0);
                break;
            case 6:
                emit_key_event(win, CROSSOS_KEY_PAGE_DOWN, CROSSOS_MOD_NONE, 0, 0);
                break;
            default:
                break;
            }
            *consumed = i + 1;
            return 1;
        }
        return 0;
    }
    if (buf[1] == 'O' && len >= 3)
    {
        switch (buf[2])
        {
        case 'P':
            emit_key_event(win, CROSSOS_KEY_F1, CROSSOS_MOD_NONE, 0, 0);
            break;
        case 'Q':
            emit_key_event(win, CROSSOS_KEY_F2, CROSSOS_MOD_NONE, 0, 0);
            break;
        case 'R':
            emit_key_event(win, CROSSOS_KEY_F3, CROSSOS_MOD_NONE, 0, 0);
            break;
        case 'S':
            emit_key_event(win, CROSSOS_KEY_F4, CROSSOS_MOD_NONE, 0, 0);
            break;
        default:
            return 0;
        }
        *consumed = 3;
        return 1;
    }
    emit_key_event(win, CROSSOS_KEY_ESCAPE, CROSSOS_MOD_NONE, 0, 0);
    *consumed = 1;
    return 1;
}

static void process_input_bytes(void)
{
    ssize_t got = 0;
    crossos_window_t *win = s_primary_window;

    if (!win)
        return;

    flush_resize_if_needed();

    while ((got = read(STDIN_FILENO,
                       s_input_buf + s_input_len,
                       sizeof(s_input_buf) - s_input_len)) > 0)
    {
        s_input_len += (size_t)got;
        if (s_input_len == sizeof(s_input_buf))
            break;
    }

    if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    {
        crossos__set_error("terminal backend: stdin read failed");
    }

    {
        size_t offset = 0;
        while (offset < s_input_len)
        {
            unsigned char ch = (unsigned char)s_input_buf[offset];
            if (ch == 0x1b)
            {
                size_t consumed = 0;
                int rc = parse_escape_sequence(win,
                                               s_input_buf + offset,
                                               s_input_len - offset,
                                               &consumed);
                if (rc < 0)
                    break;
                if (rc == 0)
                {
                    emit_key_event(win, CROSSOS_KEY_ESCAPE, CROSSOS_MOD_NONE, 0, 0);
                    offset += 1;
                }
                else
                {
                    offset += consumed;
                }
                continue;
            }

            if (ch == '\r' || ch == '\n')
            {
                emit_key_event(win, CROSSOS_KEY_ENTER, CROSSOS_MOD_NONE, 0, 0);
            }
            else if (ch == '\t')
            {
                emit_key_event(win, CROSSOS_KEY_TAB, CROSSOS_MOD_NONE, 0, 0);
            }
            else if (ch == 127 || ch == 8)
            {
                emit_key_event(win, CROSSOS_KEY_BACKSPACE, CROSSOS_MOD_NONE, 0, 0);
            }
            else if (ch >= 32 && ch < 127)
            {
                emit_key_event(win, (int)ch, CROSSOS_MOD_NONE, 1, (unsigned)ch);
            }
            else if (ch >= 1 && ch <= 26)
            {
                emit_key_event(win,
                               CROSSOS_KEY_A + (int)ch - 1,
                               CROSSOS_MOD_CTRL,
                               0,
                               0);
            }
            offset += 1;
        }

        if (offset > 0)
        {
            memmove(s_input_buf, s_input_buf + offset, s_input_len - offset);
            s_input_len -= offset;
        }
    }
}

static crossos_result_t term_platform_init(void)
{
    struct termios raw;

    s_use_smooth_sampling = !env_is_true("CROSSOS_TERM_NEAREST");

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
    {
        crossos__set_error("terminal backend requires stdin/stdout to be TTYs");
        return CROSSOS_ERR_DISPLAY;
    }

    if (tcgetattr(STDIN_FILENO, &s_saved_termios) != 0)
    {
        crossos__set_error("terminal backend: tcgetattr failed");
        return CROSSOS_ERR_INIT;
    }
    s_termios_saved = 1;

    raw = s_saved_termios;
    raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL);
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
    {
        crossos__set_error("terminal backend: tcsetattr failed");
        return CROSSOS_ERR_INIT;
    }

    s_saved_winch_handler = signal(SIGWINCH, on_sigwinch);
    if (s_saved_winch_handler != SIG_ERR)
    {
        s_saved_winch_valid = 1;
    }

    terminal_write("\033[?1049h\033[?25l\033[?7l\033[?1000h\033[?1002h\033[?1003h\033[?1006h\033[2J\033[H");
    fflush(stdout);
    return CROSSOS_OK;
}

static void term_platform_shutdown(void)
{
    if (s_saved_winch_valid)
    {
        (void)signal(SIGWINCH, s_saved_winch_handler);
        s_saved_winch_valid = 0;
    }

    terminal_write("\033[0m\033[?1006l\033[?1003l\033[?1002l\033[?1000l\033[?7h\033[?25h\033[?1049l");
    fflush(stdout);

    if (s_termios_saved)
    {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &s_saved_termios);
        s_termios_saved = 0;
    }

    s_primary_window = NULL;
    s_input_len = 0;
    s_head = 0;
    s_tail = 0;
}

static crossos_window_t *term_window_create(const char *title,
                                            int width,
                                            int height,
                                            uint32_t flags)
{
    crossos_window_t *win;
    int default_w = 0;
    int default_h = 0;

    win = (crossos_window_t *)calloc(1, sizeof(*win));
    if (!win)
    {
        crossos__set_error("terminal backend: window OOM");
        return NULL;
    }

    win->surface = (crossos_surface_t *)calloc(1, sizeof(*win->surface));
    if (!win->surface)
    {
        free(win);
        crossos__set_error("terminal backend: surface OOM");
        return NULL;
    }

    snprintf(win->title, sizeof(win->title), "%s", title ? title : "CrossOS");
    win->visible = (flags & CROSSOS_WINDOW_HIDDEN) == 0;
    win->is_fullscreen = (flags & CROSSOS_WINDOW_FULLSCREEN) != 0;
    win->surface->win = win;

    query_logical_size(&default_w, &default_h);
    if (width <= 0)
        width = default_w;
    if (height <= 0)
        height = default_h;

    if (alloc_window_framebuffer(win, width, height) != CROSSOS_OK)
    {
        free(win->surface);
        free(win);
        return NULL;
    }

    s_primary_window = win;
    set_terminal_title(win->title);
    return win;
}

static void term_window_destroy(crossos_window_t *win)
{
    if (!win)
        return;
    if (s_primary_window == win)
        s_primary_window = NULL;
    free(win->surface);
    free(win->fb_pixels);
    free(win);
}

static void term_window_show(crossos_window_t *win)
{
    if (!win)
        return;
    win->visible = 1;
    set_terminal_title(win->title);
}

static void term_window_hide(crossos_window_t *win)
{
    if (!win)
        return;
    win->visible = 0;
    terminal_write("\033[2J\033[H");
    fflush(stdout);
}

static crossos_result_t term_window_set_fullscreen(crossos_window_t *win, int fullscreen)
{
    if (!win)
        return CROSSOS_ERR_PARAM;
    win->is_fullscreen = fullscreen != 0;
    return CROSSOS_OK;
}

static crossos_result_t term_window_resize(crossos_window_t *win, int width, int height)
{
    if (!win)
        return CROSSOS_ERR_PARAM;
    if (width <= 0 || height <= 0)
        return CROSSOS_ERR_PARAM;
    if (alloc_window_framebuffer(win, width, height) != CROSSOS_OK)
    {
        return CROSSOS_ERR_OOM;
    }
    push_resize_event(win);
    return CROSSOS_OK;
}

static crossos_result_t term_window_set_title(crossos_window_t *win, const char *title)
{
    if (!win)
        return CROSSOS_ERR_PARAM;
    snprintf(win->title, sizeof(win->title), "%s", title ? title : "CrossOS");
    set_terminal_title(win->title);
    return CROSSOS_OK;
}

static void term_window_get_size(const crossos_window_t *win, int *width, int *height)
{
    if (!win)
    {
        if (width)
            *width = 0;
        if (height)
            *height = 0;
        return;
    }
    if (width)
        *width = win->width;
    if (height)
        *height = win->height;
}

static int term_window_is_fullscreen(const crossos_window_t *win)
{
    return win ? win->is_fullscreen : 0;
}

static void *term_window_get_native_handle(const crossos_window_t *win)
{
    (void)win;
    return NULL;
}

static crossos_result_t term_display_get_size(int display_index, int *width, int *height)
{
    if (display_index != 0)
        return CROSSOS_ERR_PARAM;
    query_logical_size(width, height);
    return CROSSOS_OK;
}

static int term_display_count(void)
{
    return 1;
}

static crossos_surface_t *term_surface_get(crossos_window_t *win)
{
    return win ? win->surface : NULL;
}

static crossos_result_t term_surface_lock(crossos_surface_t *surf,
                                          crossos_framebuffer_t *fb)
{
    crossos_window_t *win;

    if (!surf || !fb)
        return CROSSOS_ERR_PARAM;
    if (surf->locked)
        return CROSSOS_ERR_DISPLAY;
    win = surf->win;
    if (!win || !win->fb_pixels)
        return CROSSOS_ERR_DISPLAY;

    surf->locked = 1;
    fb->pixels = win->fb_pixels;
    fb->width = win->width;
    fb->height = win->height;
    fb->stride = win->fb_stride;
    fb->format = CROSSOS_PIXEL_FMT_BGRA8888;
    return CROSSOS_OK;
}

static void term_surface_unlock(crossos_surface_t *surf)
{
    if (surf)
        surf->locked = 0;
}

static crossos_result_t term_surface_present(crossos_surface_t *surf)
{
    crossos_window_t *win;
    int cols;
    int rows;
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

    cap = 32u + (size_t)rows * ((size_t)cols * 48u + 8u);
    out = (char *)malloc(cap);
    if (!out)
        return CROSSOS_ERR_OOM;

    p = out;
    p += snprintf(p, cap, "\033[H");

    for (int row = 0; row < rows; row++)
    {
        int top_y0 = ((2 * row) * win->height) / (2 * rows);
        int top_y1 = ((2 * row + 1) * win->height) / (2 * rows);
        int bot_y0 = top_y1;
        int bot_y1 = ((2 * row + 2) * win->height) / (2 * rows);
        for (int col = 0; col < cols; col++)
        {
            int x0 = (col * win->width) / cols;
            int x1 = ((col + 1) * win->width) / cols;
            unsigned char top_rgb[3];
            unsigned char bot_rgb[3];

            if (s_use_smooth_sampling)
            {
                sample_region_avg(win, x0, top_y0, x1, top_y1, top_rgb);
                sample_region_avg(win, x0, bot_y0, x1, bot_y1, bot_rgb);
            }
            else
            {
                sample_region_nearest(win, x0, top_y0, x1, top_y1, top_rgb);
                sample_region_nearest(win, x0, bot_y0, x1, bot_y1, bot_rgb);
            }

            p += snprintf(p,
                          cap - (size_t)(p - out),
                          "\033[38;2;%u;%u;%um\033[48;2;%u;%u;%um▀",
                          (unsigned)top_rgb[0], (unsigned)top_rgb[1], (unsigned)top_rgb[2],
                          (unsigned)bot_rgb[0], (unsigned)bot_rgb[1], (unsigned)bot_rgb[2]);
        }
        if (row + 1 < rows)
        {
            p += snprintf(p, cap - (size_t)(p - out), "\033[0m\r\n");
        }
    }
    p += snprintf(p, cap - (size_t)(p - out), "\033[0m");

    {
        ssize_t rc = write(STDOUT_FILENO, out, (size_t)(p - out));
        (void)rc;
    }
    fflush(stdout);
    free(out);
    return CROSSOS_OK;
}

static int term_poll_event(crossos_event_t *ev)
{
    if (!ev)
        return 0;
    process_input_bytes();
    if (crossos__quit_requested && s_tail == s_head)
    {
        memset(ev, 0, sizeof(*ev));
        ev->type = CROSSOS_EVENT_QUIT;
        ev->window = s_primary_window;
        return 1;
    }
    return pop_event(ev);
}

static int term_wait_event(crossos_event_t *ev)
{
    if (!ev)
        return 0;
    for (;;)
    {
        fd_set rfds;
        int ready;

        if (term_poll_event(ev))
            return ev->type != CROSSOS_EVENT_QUIT;

        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        ready = select(STDIN_FILENO + 1, &rfds, NULL, NULL, NULL);
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                flush_resize_if_needed();
                continue;
            }
            crossos__set_error("terminal backend: select failed");
            return 0;
        }
    }
}

static void term_run_loop(crossos_window_t *win,
                          crossos_event_cb_t cb,
                          void *user_data)
{
    crossos_event_t ev;
    (void)win;
    while (!crossos__quit_requested)
    {
        if (!term_wait_event(&ev))
            break;
        if (ev.type == CROSSOS_EVENT_QUIT || ev.type == CROSSOS_EVENT_WINDOW_CLOSE)
        {
            crossos__quit_requested = 1;
        }
        if (cb)
            cb(&ev, user_data);
    }
}

static int term_touch_get_active(const crossos_window_t *win,
                                 crossos_touch_point_t pts[CROSSOS_MAX_TOUCH_POINTS])
{
    (void)win;
    (void)pts;
    return 0;
}

static int term_touch_is_supported(void)
{
    return 0;
}

const crossos_linux_backend_vtable_t crossos__linux_terminal_backend = {
    term_platform_init,
    term_platform_shutdown,
    term_window_create,
    term_window_destroy,
    term_window_show,
    term_window_hide,
    term_window_set_fullscreen,
    term_window_resize,
    term_window_set_title,
    term_window_get_size,
    term_window_is_fullscreen,
    term_window_get_native_handle,
    term_display_get_size,
    term_display_count,
    term_surface_get,
    term_surface_lock,
    term_surface_unlock,
    term_surface_present,
    term_poll_event,
    term_wait_event,
    term_run_loop,
    term_touch_get_active,
    term_touch_is_supported,
};

#endif /* __linux__ && !__ANDROID__ */