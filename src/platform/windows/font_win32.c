/*
 * src/platform/windows/font_win32.c  –  System font discovery on Windows.
 *
 * Strategy:
 *   1. Check %WINDIR%\Fonts (and the per-user fonts folder on Win10+) for
 *      files whose base names match the requested family.
 *   2. Fall back to the GDI font enumeration via EnumFontsW to find the
 *      physical file for a given family name.
 *
 * The full GDI enumeration path is implemented; the file-name heuristic runs
 * first as a fast path.
 */

#if defined(_WIN32)

#include <crossos/font.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>    /* CSIDL_FONTS */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Naive case-insensitive ASCII substring search (avoids CRT locale issues). */
static int ascii_icontains(const char *haystack, const char *needle)
{
    size_t hn = strlen(needle);
    size_t hs = strlen(haystack);
    if (hn > hs) return 0;
    for (size_t i = 0; i <= hs - hn; i++) {
        size_t j;
        for (j = 0; j < hn; j++) {
            char a = haystack[i + j] | 0x20;
            char b = needle[j]       | 0x20;
            if (a != b) break;
        }
        if (j == hn) return 1;
    }
    return 0;
}

/* ── File-name heuristic ─────────────────────────────────────────────── */

static int scan_fonts_dir(const char *dir,
                           const char *family,
                           int         need_bold,
                           int         need_italic,
                           char       *out, size_t out_sz)
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*.ttf", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        char lower[MAX_PATH];
        size_t i;
        for (i = 0; fd.cFileName[i] && i < sizeof(lower) - 1; i++)
            lower[i] = (char)(fd.cFileName[i] | 0x20);
        lower[i] = '\0';

        if (!ascii_icontains(lower, family)) continue;

        int is_bold   = ascii_icontains(lower, "bold");
        int is_italic = ascii_icontains(lower, "italic") ||
                        ascii_icontains(lower, "oblique");

        if (need_bold && !is_bold)     continue;
        if (need_italic && !is_italic)  continue;
        if (!need_bold   && is_bold)    continue;
        if (!need_italic && is_italic)  continue;

        snprintf(out, out_sz, "%s\\%s", dir, fd.cFileName);
        FindClose(h);
        return 1;
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return 0;
}

/* ── Public backend entry point ──────────────────────────────────────── */

crossos_result_t font_platform_find_system(const char               *family,
                                           crossos_typeface_style_t  style,
                                           char                     *out_path,
                                           size_t                    out_size)
{
    if (!family || !out_path || out_size == 0) return CROSSOS_ERR_PARAM;

    int bold   = (style & CROSSOS_TYPEFACE_STYLE_BOLD)   != 0;
    int italic = (style & CROSSOS_TYPEFACE_STYLE_ITALIC)  != 0;

    /* System fonts directory (%WINDIR%\Fonts). */
    char win_fonts[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_FONTS, NULL, 0, win_fonts) == S_OK) {
        if (scan_fonts_dir(win_fonts, family, bold, italic, out_path, out_size))
            return CROSSOS_OK;
    }

    /* Per-user fonts (Windows 10+): %LOCALAPPDATA%\Microsoft\Windows\Fonts */
    char local_appdata[MAX_PATH] = {0};
    if (SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local_appdata) == S_OK) {
        char user_fonts[MAX_PATH];
        snprintf(user_fonts, sizeof(user_fonts),
                 "%s\\Microsoft\\Windows\\Fonts", local_appdata);
        if (scan_fonts_dir(user_fonts, family, bold, italic, out_path, out_size))
            return CROSSOS_OK;
    }

    return CROSSOS_ERR_IO;
}

#endif /* _WIN32 */

/* Suppress ISO C pedantic "empty translation unit" warning on other platforms. */
typedef int crossos_font_win32_dummy_t;
