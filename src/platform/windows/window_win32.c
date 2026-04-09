/**
 * platform/windows/window_win32.c  –  Win32 window and surface implementation.
 *
 * Uses:
 *   • CreateWindowExW / RegisterClassExW for window creation
 *   • WM_TOUCH / WM_POINTER for multi-touch input (Windows 8+)
 *   • GDI (GetDC / SetDIBitsToDevice) for software framebuffer presentation
 */

#ifdef _WIN32

#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>  /* GET_X_LPARAM / GET_Y_LPARAM */
#include <shellapi.h>  /* DragAcceptFiles, DragQueryFileW, DragFinish */

#include <crossos/crossos.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ── Internal declarations from core ─────────────────────────────────── */
extern void             crossos__set_error(const char *fmt, ...);
extern volatile int     crossos__quit_requested;
extern void             crossos__push_event(const crossos_event_t *ev);

/* ── Window class name ────────────────────────────────────────────────── */
static const wchar_t *WCLASS_NAME = L"CrossOSWindow";
static HINSTANCE       s_hinstance = NULL;
static int             s_class_registered = 0;

/* Static drop buffer for CROSSOS_EVENT_DROP_FILES payloads. */
#define DROP_MAX_FILES 32
#define DROP_PATH_BUF  512
static char       s_drop_bufs[DROP_MAX_FILES][DROP_PATH_BUF];
static const char *s_drop_ptrs[DROP_MAX_FILES];
static int         s_drop_count = 0;

/* ── Internal window structure ────────────────────────────────────────── */
struct crossos_window {
    HWND              hwnd;
    int               width;
    int               height;
    int               is_fullscreen;

    /* Software framebuffer */
    void             *fb_pixels;
    int               fb_stride;   /* bytes per row */
    BITMAPINFO        bmi;

    /* Attached surface (owned by the window) */
    struct crossos_surface *surface;
};

struct crossos_surface {
    crossos_window_t *win;
    int               locked;
};

/* ── Helpers ──────────────────────────────────────────────────────────── */

/** Bytes-per-row for a 32 bpp (4 byte/pixel) image, 4-byte aligned. */
static int fb_stride(int width)
{
    return width * 4; /* 4 bytes/pixel is already 4-byte aligned for any width */
}

static void utf8_to_wchar(const char *src, wchar_t *dst, int dst_len)
{
    MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dst_len);
}

static void push_touch_from_pointer(UINT msg, WPARAM wParam, LPARAM lParam,
                                    crossos_window_t *win, int button)
{
    (void)wParam;
    if (!win) {
        return;
    }

    crossos_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.window = win;

    POINT pt;
    if (msg == WM_POINTERDOWN || msg == WM_POINTERUP || msg == WM_POINTERUPDATE) {
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        ScreenToClient(win->hwnd, &pt);
    } else {
        /* Mouse messages already deliver client coordinates in lParam. */
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
    }

    if (msg == WM_POINTERDOWN || msg == WM_LBUTTONDOWN ||
        msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN) {
        ev.type = CROSSOS_EVENT_POINTER_DOWN;
    } else if (msg == WM_POINTERUP || msg == WM_LBUTTONUP ||
               msg == WM_RBUTTONUP || msg == WM_MBUTTONUP) {
        ev.type = CROSSOS_EVENT_POINTER_UP;
    } else {
        ev.type = CROSSOS_EVENT_POINTER_MOVE;
    }
    ev.pointer.x = (float)pt.x;
    ev.pointer.y = (float)pt.y;
    ev.pointer.button = button;
    crossos__push_event(&ev);
}

static crossos_key_mod_t win_mods(void)
{
    crossos_key_mod_t m = CROSSOS_MOD_NONE;
    if (GetKeyState(VK_SHIFT) & 0x8000)   m |= CROSSOS_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= CROSSOS_MOD_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000)    m |= CROSSOS_MOD_ALT;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) {
        m |= CROSSOS_MOD_SUPER;
    }
    return m;
}

static int vk_to_crossos(WPARAM vk)
{
    if (vk >= '0' && vk <= '9') return (int)vk;
    if (vk >= 'A' && vk <= 'Z') return (int)vk;

    switch (vk) {
    case VK_SPACE:    return CROSSOS_KEY_SPACE;
    case VK_OEM_7:    return CROSSOS_KEY_APOSTROPHE;
    case VK_OEM_COMMA:return CROSSOS_KEY_COMMA;
    case VK_OEM_MINUS:return CROSSOS_KEY_MINUS;
    case VK_OEM_PERIOD:return CROSSOS_KEY_PERIOD;
    case VK_OEM_2:    return CROSSOS_KEY_SLASH;
    case VK_OEM_1:    return CROSSOS_KEY_SEMICOLON;
    case VK_OEM_PLUS: return CROSSOS_KEY_EQUAL;

    case VK_ESCAPE:   return CROSSOS_KEY_ESCAPE;
    case VK_RETURN:   return CROSSOS_KEY_ENTER;
    case VK_TAB:      return CROSSOS_KEY_TAB;
    case VK_BACK:     return CROSSOS_KEY_BACKSPACE;
    case VK_INSERT:   return CROSSOS_KEY_INSERT;
    case VK_DELETE:   return CROSSOS_KEY_DELETE;
    case VK_RIGHT:    return CROSSOS_KEY_RIGHT;
    case VK_LEFT:     return CROSSOS_KEY_LEFT;
    case VK_DOWN:     return CROSSOS_KEY_DOWN;
    case VK_UP:       return CROSSOS_KEY_UP;
    case VK_PRIOR:    return CROSSOS_KEY_PAGE_UP;
    case VK_NEXT:     return CROSSOS_KEY_PAGE_DOWN;
    case VK_HOME:     return CROSSOS_KEY_HOME;
    case VK_END:      return CROSSOS_KEY_END;

    case VK_F1:  return CROSSOS_KEY_F1;
    case VK_F2:  return CROSSOS_KEY_F2;
    case VK_F3:  return CROSSOS_KEY_F3;
    case VK_F4:  return CROSSOS_KEY_F4;
    case VK_F5:  return CROSSOS_KEY_F5;
    case VK_F6:  return CROSSOS_KEY_F6;
    case VK_F7:  return CROSSOS_KEY_F7;
    case VK_F8:  return CROSSOS_KEY_F8;
    case VK_F9:  return CROSSOS_KEY_F9;
    case VK_F10: return CROSSOS_KEY_F10;
    case VK_F11: return CROSSOS_KEY_F11;
    case VK_F12: return CROSSOS_KEY_F12;

    case VK_LSHIFT:   return CROSSOS_KEY_LEFT_SHIFT;
    case VK_RSHIFT:   return CROSSOS_KEY_RIGHT_SHIFT;
    case VK_LCONTROL: return CROSSOS_KEY_LEFT_CTRL;
    case VK_RCONTROL: return CROSSOS_KEY_RIGHT_CTRL;
    case VK_LMENU:    return CROSSOS_KEY_LEFT_ALT;
    case VK_RMENU:    return CROSSOS_KEY_RIGHT_ALT;
    case VK_LWIN:     return CROSSOS_KEY_LEFT_SUPER;
    case VK_RWIN:     return CROSSOS_KEY_RIGHT_SUPER;
    default:          return CROSSOS_KEY_UNKNOWN;
    }
}

/* ── Window procedure ─────────────────────────────────────────────────── */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg,
                                  WPARAM wParam, LPARAM lParam)
{
    crossos_window_t *win =
        (crossos_window_t *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    crossos_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.window = win;

    switch (msg) {
    case WM_CLOSE:
        ev.type = CROSSOS_EVENT_WINDOW_CLOSE;
        crossos__push_event(&ev);
        return 0;

    case WM_SIZE:
        if (win) {
            win->width  = LOWORD(lParam);
            win->height = HIWORD(lParam);

            /* Re-allocate framebuffer */
            int stride = fb_stride(win->width);
            free(win->fb_pixels);
            win->fb_pixels = calloc((size_t)stride * (size_t)win->height, 1);
            win->fb_stride = stride;

            win->bmi.bmiHeader.biWidth    = win->width;
            win->bmi.bmiHeader.biHeight   = -win->height; /* top-down */

            ev.type         = CROSSOS_EVENT_WINDOW_RESIZE;
            ev.resize.width = win->width;
            ev.resize.height= win->height;
            crossos__push_event(&ev);
        }
        return 0;

    case WM_SETFOCUS:
        ev.type = CROSSOS_EVENT_WINDOW_FOCUS;
        crossos__push_event(&ev);
        return 0;

    case WM_KILLFOCUS:
        ev.type = CROSSOS_EVENT_WINDOW_BLUR;
        crossos__push_event(&ev);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        ev.type         = CROSSOS_EVENT_KEY_DOWN;
        ev.key.scancode = (int)((lParam >> 16) & 0xFF);
        ev.key.keycode  = vk_to_crossos(wParam);
        ev.key.mods     = win_mods();
        ev.key.repeat   = (lParam & (1 << 30)) ? 1 : 0;
        crossos__push_event(&ev);
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        ev.type         = CROSSOS_EVENT_KEY_UP;
        ev.key.scancode = (int)((lParam >> 16) & 0xFF);
        ev.key.keycode  = vk_to_crossos(wParam);
        ev.key.mods     = win_mods();
        crossos__push_event(&ev);
        return 0;

    case WM_LBUTTONDOWN:
        push_touch_from_pointer(msg, wParam, lParam, win, 1);
        return 0;
    case WM_LBUTTONUP:
        push_touch_from_pointer(msg, wParam, lParam, win, 1);
        return 0;
    case WM_RBUTTONDOWN:
        push_touch_from_pointer(msg, wParam, lParam, win, 3);
        return 0;
    case WM_RBUTTONUP:
        push_touch_from_pointer(msg, wParam, lParam, win, 3);
        return 0;
    case WM_MBUTTONDOWN:
        push_touch_from_pointer(msg, wParam, lParam, win, 2);
        return 0;
    case WM_MBUTTONUP:
        push_touch_from_pointer(msg, wParam, lParam, win, 2);
        return 0;
    case WM_MOUSEMOVE:
        push_touch_from_pointer(msg, wParam, lParam, win, 0);
        return 0;

    case WM_CHAR: {
        /* UTF-16 codepoint from TranslateMessage; skip control chars */
        unsigned int cp = (unsigned int)wParam;
        if (cp >= 32 && cp != 127) {
            crossos_event_t cev;
            memset(&cev, 0, sizeof(cev));
            cev.type           = CROSSOS_EVENT_CHAR;
            cev.window         = win;
            cev.character.codepoint = cp;
            crossos__push_event(&cev);
        }
        return 0;
    }

    case WM_DROPFILES: {
        HDROP hdrop = (HDROP)wParam;
        UINT n = DragQueryFileW(hdrop, 0xFFFFFFFF, NULL, 0);
        if (n > DROP_MAX_FILES) n = DROP_MAX_FILES;
        s_drop_count = 0;
        for (UINT i = 0; i < n; i++) {
            wchar_t wpath[DROP_PATH_BUF];
            if (DragQueryFileW(hdrop, i, wpath, DROP_PATH_BUF) > 0) {
                WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                    s_drop_bufs[s_drop_count], DROP_PATH_BUF, NULL, NULL);
                s_drop_ptrs[s_drop_count] = s_drop_bufs[s_drop_count];
                s_drop_count++;
            }
        }
        DragFinish(hdrop);
        if (s_drop_count > 0) {
            crossos_event_t dev;
            memset(&dev, 0, sizeof(dev));
            dev.type        = CROSSOS_EVENT_DROP_FILES;
            dev.window      = win;
            dev.drop.count  = s_drop_count;
            dev.drop.paths  = s_drop_ptrs;
            crossos__push_event(&dev);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT pt;
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        ScreenToClient(hwnd, &pt);
        ev.type = CROSSOS_EVENT_POINTER_SCROLL;
        ev.pointer.x        = (float)pt.x;
        ev.pointer.y        = (float)pt.y;
        ev.pointer.scroll_y = (float)GET_WHEEL_DELTA_WPARAM(wParam) / 120.0f;
        crossos__push_event(&ev);
        return 0;
    }

    case WM_TOUCH: {
        UINT num = LOWORD(wParam);
        if (num > CROSSOS_MAX_TOUCH_POINTS) num = CROSSOS_MAX_TOUCH_POINTS;
        TOUCHINPUT ti[CROSSOS_MAX_TOUCH_POINTS];
        if (GetTouchInputInfo((HTOUCHINPUT)lParam, num, ti, sizeof(TOUCHINPUT))) {
            crossos_event_t tev;
            memset(&tev, 0, sizeof(tev));
            tev.window       = win;
            tev.touch.count  = (int)num;

            /* Determine event type from first point */
            if (ti[0].dwFlags & TOUCHEVENTF_DOWN)
                tev.type = CROSSOS_EVENT_TOUCH_BEGIN;
            else if (ti[0].dwFlags & TOUCHEVENTF_UP)
                tev.type = CROSSOS_EVENT_TOUCH_END;
            else
                tev.type = CROSSOS_EVENT_TOUCH_UPDATE;

            for (UINT i = 0; i < num; i++) {
                POINT pt;
                pt.x = ti[i].x / 100;
                pt.y = ti[i].y / 100;
                ScreenToClient(hwnd, &pt);
                tev.touch.points[i].id       = (int)ti[i].dwID;
                tev.touch.points[i].x        = (float)pt.x;
                tev.touch.points[i].y        = (float)pt.y;
                tev.touch.points[i].pressure = 1.0f;
            }
            crossos__push_event(&tev);
            CloseTouchInputHandle((HTOUCHINPUT)lParam);
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ── Platform init / shutdown ─────────────────────────────────────────── */

crossos_result_t crossos__platform_init(void)
{
    s_hinstance = GetModuleHandleW(NULL);
    if (!s_hinstance) {
        crossos__set_error("Win32: GetModuleHandle failed");
        return CROSSOS_ERR_INIT;
    }

    if (!s_class_registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc   = wnd_proc;
        wc.hInstance     = s_hinstance;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = WCLASS_NAME;
        wc.hIcon         = LoadIconW(NULL, IDI_APPLICATION);
        wc.hIconSm       = wc.hIcon;
        if (!RegisterClassExW(&wc)) {
            crossos__set_error("Win32: RegisterClassEx failed (%lu)",
                               GetLastError());
            return CROSSOS_ERR_INIT;
        }
        s_class_registered = 1;
    }
    return CROSSOS_OK;
}

void crossos__platform_shutdown(void)
{
    if (s_class_registered && s_hinstance) {
        UnregisterClassW(WCLASS_NAME, s_hinstance);
        s_class_registered = 0;
    }
}

/* ── Window lifecycle ─────────────────────────────────────────────────── */

crossos_window_t *crossos_window_create(const char *title,
                                        int width, int height,
                                        uint32_t flags)
{
    crossos_window_t *win = calloc(1, sizeof(*win));
    if (!win) {
        crossos__set_error("Win32: out of memory");
        return NULL;
    }

    win->width  = width;
    win->height = height;

    int stride = fb_stride(width);
    win->fb_pixels = calloc((size_t)stride * (size_t)height, 1);
    win->fb_stride = stride;
    if (!win->fb_pixels) {
        free(win);
        crossos__set_error("Win32: framebuffer OOM");
        return NULL;
    }

    win->bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    win->bmi.bmiHeader.biWidth       = width;
    win->bmi.bmiHeader.biHeight      = -height;
    win->bmi.bmiHeader.biPlanes      = 1;
    win->bmi.bmiHeader.biBitCount    = 32;
    win->bmi.bmiHeader.biCompression = BI_RGB;

    DWORD ws = WS_OVERLAPPEDWINDOW;
    if (flags & CROSSOS_WINDOW_RESIZABLE)  ws |= WS_THICKFRAME;
    if (flags & CROSSOS_WINDOW_BORDERLESS) ws = WS_POPUP;

    RECT r = { 0, 0, width, height };
    AdjustWindowRect(&r, ws, FALSE);

    wchar_t wtitle[256];
    utf8_to_wchar(title ? title : "", wtitle, 256);

    DWORD ex_style = (flags & CROSSOS_WINDOW_FULLSCREEN) ? WS_EX_TOPMOST : 0;

    win->hwnd = CreateWindowExW(
        ex_style,
        WCLASS_NAME, wtitle, ws,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        NULL, NULL, s_hinstance, NULL);

    if (!win->hwnd) {
        free(win->fb_pixels);
        free(win);
        crossos__set_error("Win32: CreateWindowEx failed (%lu)", GetLastError());
        return NULL;
    }

    SetWindowLongPtrW(win->hwnd, GWLP_USERDATA, (LONG_PTR)win);
    RegisterTouchWindow(win->hwnd, 0);
    DragAcceptFiles(win->hwnd, TRUE);   /* enable WM_DROPFILES */

    /* Create attached surface */
    win->surface = calloc(1, sizeof(*win->surface));
    if (win->surface) win->surface->win = win;

    if (!(flags & CROSSOS_WINDOW_HIDDEN))
        ShowWindow(win->hwnd, SW_SHOWNORMAL);

    return win;
}

void crossos_window_destroy(crossos_window_t *win)
{
    if (!win) return;
    free(win->surface);
    if (win->hwnd) DestroyWindow(win->hwnd);
    free(win->fb_pixels);
    free(win);
}

void crossos_window_show(crossos_window_t *win)
{
    if (win && win->hwnd) ShowWindow(win->hwnd, SW_SHOWNORMAL);
}

void crossos_window_hide(crossos_window_t *win)
{
    if (win && win->hwnd) ShowWindow(win->hwnd, SW_HIDE);
}

crossos_result_t crossos_window_set_fullscreen(crossos_window_t *win,
                                               int fullscreen)
{
    if (!win) return CROSSOS_ERR_PARAM;
    win->is_fullscreen = fullscreen;
    /* Full-screen toggle via SetWindowLong / SetWindowPos */
    if (fullscreen) {
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(win->hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongW(win->hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(win->hwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right  - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(win->hwnd, GWL_STYLE,
                       WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(win->hwnd, HWND_NOTOPMOST,
                     CW_USEDEFAULT, CW_USEDEFAULT,
                     win->width, win->height,
                     SWP_FRAMECHANGED);
    }
    return CROSSOS_OK;
}

crossos_result_t crossos_window_resize(crossos_window_t *win,
                                       int width, int height)
{
    if (!win) return CROSSOS_ERR_PARAM;
    RECT r = { 0, 0, width, height };
    DWORD ws = (DWORD)GetWindowLongW(win->hwnd, GWL_STYLE);
    AdjustWindowRect(&r, ws, FALSE);
    SetWindowPos(win->hwnd, NULL, 0, 0,
                 r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER);
    return CROSSOS_OK;
}

crossos_result_t crossos_window_set_title(crossos_window_t *win,
                                          const char *title)
{
    if (!win || !win->hwnd) return CROSSOS_ERR_PARAM;
    wchar_t wtitle[256];
    utf8_to_wchar(title ? title : "", wtitle, 256);
    SetWindowTextW(win->hwnd, wtitle);
    return CROSSOS_OK;
}

void crossos_window_get_size(const crossos_window_t *win,
                             int *width, int *height)
{
    if (!win) {
        if (width)  *width  = 0;
        if (height) *height = 0;
        return;
    }
    if (width)  *width  = win->width;
    if (height) *height = win->height;
}

int crossos_window_is_fullscreen(const crossos_window_t *win)
{
    return win ? win->is_fullscreen : 0;
}

void *crossos_window_get_native_handle(const crossos_window_t *win)
{
    return win ? (void *)win->hwnd : NULL;
}

/* ── Display (surface / framebuffer) ──────────────────────────────────── */

crossos_surface_t *crossos_surface_get(crossos_window_t *win)
{
    return win ? win->surface : NULL;
}

crossos_result_t crossos_surface_lock(crossos_surface_t   *surf,
                                      crossos_framebuffer_t *fb)
{
    if (!surf || !fb) return CROSSOS_ERR_PARAM;
    if (surf->locked)  return CROSSOS_ERR_DISPLAY;
    surf->locked = 1;

    crossos_window_t *win = surf->win;
    fb->pixels = win->fb_pixels;
    fb->width  = win->width;
    fb->height = win->height;
    fb->stride = win->fb_stride;
    fb->format = CROSSOS_PIXEL_FMT_BGRA8888; /* Win32 DIBs are BGRA */
    return CROSSOS_OK;
}

void crossos_surface_unlock(crossos_surface_t *surf)
{
    if (surf) surf->locked = 0;
}

crossos_result_t crossos_surface_present(crossos_surface_t *surf)
{
    if (!surf || !surf->win) return CROSSOS_ERR_PARAM;
    crossos_window_t *win = surf->win;
    HDC hdc = GetDC(win->hwnd);
    if (!hdc) return CROSSOS_ERR_DISPLAY;
    SetDIBitsToDevice(hdc,
                      0, 0, (DWORD)win->width, (DWORD)win->height,
                      0, 0, 0, (UINT)win->height,
                      win->fb_pixels, &win->bmi, DIB_RGB_COLORS);
    ReleaseDC(win->hwnd, hdc);
    return CROSSOS_OK;
}

crossos_result_t crossos_display_get_size(int idx, int *w, int *h)
{
    if (idx != 0) return CROSSOS_ERR_PARAM;
    if (w) *w = GetSystemMetrics(SM_CXSCREEN);
    if (h) *h = GetSystemMetrics(SM_CYSCREEN);
    return CROSSOS_OK;
}

int crossos_display_count(void)
{
    return GetSystemMetrics(SM_CMONITORS);
}

#endif /* _WIN32 */
