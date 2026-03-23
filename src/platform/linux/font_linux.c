/*
 * src/platform/linux/font_linux.c  –  System font discovery on Linux.
 *
 * Strategy (in order):
 *   1. fontconfig (fc-match) if available at runtime – the most reliable
 *      approach; handles aliases like "sans-serif" correctly.
 *   2. Fallback: walk /usr/share/fonts and /usr/local/share/fonts looking
 *      for a file whose name contains the requested family string.
 *   3. Final fallback: return CROSSOS_ERR_IO so the caller can use the
 *      bundled font.
 */

#if defined(__linux__) && !defined(__ANDROID__)

/* Need POSIX for popen/pclose, strcasecmp, and struct dirent. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <crossos/font.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

/* ── fontconfig via fc-match ─────────────────────────────────────────── */

static int fc_match_find(const char *family,
                         crossos_typeface_style_t style,
                         char *out, size_t out_sz)
{
    /* Build a fontconfig pattern like "sans-serif:bold:slant=italic". */
    char pattern[256];
    int n = snprintf(pattern, sizeof(pattern), "%s", family);
    if (style & CROSSOS_TYPEFACE_STYLE_BOLD)
        n += snprintf(pattern + n, sizeof(pattern) - (size_t)n, ":bold");
    if (style & CROSSOS_TYPEFACE_STYLE_ITALIC)
        n += snprintf(pattern + n, sizeof(pattern) - (size_t)n, ":slant=italic");

    /* Run fc-match and read the resolved file path. */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "fc-match --format='%%{file}' '%s' 2>/dev/null", pattern);

    FILE *pipe = popen(cmd, "r");
    if (!pipe) return 0;

    char buf[1024] = {0};
    size_t nread = fread(buf, 1, sizeof(buf) - 1, pipe);
    pclose(pipe);

    if (nread == 0) return 0;

    /* Strip trailing whitespace / newline. */
    while (nread > 0 &&
           (buf[nread - 1] == '\n' || buf[nread - 1] == '\r' ||
            buf[nread - 1] == ' ')) {
        buf[--nread] = '\0';
    }
    if (nread == 0) return 0;

    /* Verify the file actually exists. */
    struct stat st;
    if (stat(buf, &st) != 0) return 0;

    snprintf(out, out_sz, "%s", buf);
    return 1;
}

/* ── Recursive directory walk fallback ──────────────────────────────── */

/*
 * Walk a font directory tree; return 1 and fill out[] with the first .ttf or
 * .otf file whose base name (case-insensitive) contains needle[].
 */
static int walk_dir(const char *dir,
                    const char *needle,
                    int need_bold,
                    int need_italic,
                    char *out, size_t out_sz,
                    int depth)
{
    if (depth > 4) return 0; /* safety limit */

    DIR *d = opendir(dir);
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (walk_dir(path, needle, need_bold, need_italic,
                         out, out_sz, depth + 1)) {
                closedir(d);
                return 1;
            }
            continue;
        }

        /* Check extension. */
        size_t len = strlen(ent->d_name);
        if (len < 4) continue;
        const char *ext = ent->d_name + len - 4;
        int is_font = (strcasecmp(ext, ".ttf") == 0 ||
                       strcasecmp(ext, ".otf") == 0);
        if (!is_font) continue;

        /* Case-insensitive substring match for the family name. */
        char lower_name[256];
        size_t i;
        for (i = 0; i < sizeof(lower_name) - 1 && ent->d_name[i]; i++)
            lower_name[i] = (char)(ent->d_name[i] | 0x20); /* tolower */
        lower_name[i] = '\0';

        char lower_needle[128];
        for (i = 0; i < sizeof(lower_needle) - 1 && needle[i]; i++)
            lower_needle[i] = (char)(needle[i] | 0x20);
        lower_needle[i] = '\0';

        if (!strstr(lower_name, lower_needle)) continue;

        /* Check bold/italic requirements roughly by file name. */
        int is_bold   = (strstr(lower_name, "bold")   != NULL);
        int is_italic = (strstr(lower_name, "italic")  != NULL ||
                         strstr(lower_name, "oblique") != NULL);

        if (need_bold && !is_bold)   continue;
        if (need_italic && !is_italic) continue;
        if (!need_bold   && is_bold)   continue;
        if (!need_italic && is_italic) continue;

        snprintf(out, out_sz, "%s", path);
        closedir(d);
        return 1;
    }
    closedir(d);
    return 0;
}

/* ── Public backend entry point ──────────────────────────────────────── */

crossos_result_t font_platform_find_system(const char               *family,
                                           crossos_typeface_style_t  style,
                                           char                     *out_path,
                                           size_t                    out_size)
{
    if (!family || !out_path || out_size == 0) return CROSSOS_ERR_PARAM;

    /* 1. Try fontconfig. */
    if (fc_match_find(family, style, out_path, out_size))
        return CROSSOS_OK;

    /* 2. Walk common font directories. */
    int bold   = (style & CROSSOS_TYPEFACE_STYLE_BOLD)   != 0;
    int italic = (style & CROSSOS_TYPEFACE_STYLE_ITALIC)  != 0;

    static const char * const dirs[] = {
        "/usr/share/fonts",
        "/usr/local/share/fonts",
        "/usr/share/fonts/truetype",
        "/usr/share/fonts/opentype",
        NULL
    };

    for (int i = 0; dirs[i]; i++) {
        if (walk_dir(dirs[i], family, bold, italic, out_path, out_size, 0))
            return CROSSOS_OK;
    }

    return CROSSOS_ERR_IO;
}

#endif /* __linux__ && !__ANDROID__ */

/* Suppress ISO C pedantic "empty translation unit" warning on other platforms. */
typedef int crossos_font_linux_dummy_t;
