/*
 * src/platform/android/font_android.c  –  System font discovery on Android.
 *
 * Android ships fonts in /system/fonts (all versions) and may also have
 * OEM fonts in /vendor/fonts.  Since API 29, per-app fonts can appear in
 * /data/fonts.  We scan all three locations.
 *
 * The standard system sans-serif on most Android devices is Roboto (bundled
 * since 4.0) at /system/fonts/Roboto-Regular.ttf.  NotoSans covers the
 * extended Unicode range.
 *
 * Note: on Android 12+ the font system is managed by ASystemFontIterator
 * (API 29+).  For maximum compatibility this file uses a plain directory
 * scan that works on all API levels.
 */

#if defined(__ANDROID__)

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

/* ── Directory scanner ───────────────────────────────────────────────── */

static int scan_dir(const char *dir,
                    const char *family,
                    int         need_bold,
                    int         need_italic,
                    char       *out, size_t out_sz)
{
    DIR *d = opendir(dir);
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        /* Extension check */
        size_t len = strlen(ent->d_name);
        if (len < 4) continue;
        const char *ext = ent->d_name + len - 4;
        int is_font = (strcasecmp(ext, ".ttf") == 0 ||
                       strcasecmp(ext, ".otf") == 0);
        if (!is_font) continue;

        /* Case-insensitive family name check */
        char lower[256];
        size_t i;
        for (i = 0; i < sizeof(lower) - 1 && ent->d_name[i]; i++)
            lower[i] = (char)(ent->d_name[i] | 0x20);
        lower[i] = '\0';

        char lower_fam[128];
        for (i = 0; i < sizeof(lower_fam) - 1 && family[i]; i++)
            lower_fam[i] = (char)(family[i] | 0x20);
        lower_fam[i] = '\0';

        if (!strstr(lower, lower_fam)) continue;

        int is_bold   = (strstr(lower, "bold")    != NULL);
        int is_italic = (strstr(lower, "italic")   != NULL ||
                         strstr(lower, "oblique")  != NULL);

        if (need_bold && !is_bold)     continue;
        if (need_italic && !is_italic)  continue;
        if (!need_bold   && is_bold)    continue;
        if (!need_italic && is_italic)  continue;

        snprintf(out, out_sz, "%s/%s", dir, ent->d_name);
        closedir(d);
        return 1;
    }
    closedir(d);
    return 0;
}

/* Resolve a generic alias to a known Android font family name. */
static const char *resolve_alias(const char *family)
{
    /* Normalize generic CSS-style aliases to their Android file-name roots. */
    if (strcasecmp(family, "sans-serif") == 0 ||
        strcasecmp(family, "sans")       == 0) return "Roboto";
    if (strcasecmp(family, "serif")       == 0) return "NotoSerif";
    if (strcasecmp(family, "monospace")   == 0 ||
        strcasecmp(family, "mono")        == 0) return "DroidSansMono";
    return family;
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

    const char *resolved = resolve_alias(family);

    static const char * const dirs[] = {
        "/system/fonts",
        "/vendor/fonts",
        "/data/fonts",
        NULL
    };

    for (int i = 0; dirs[i]; i++) {
        if (scan_dir(dirs[i], resolved, bold, italic, out_path, out_size))
            return CROSSOS_OK;
    }

    /* Second pass: relax style constraints (bold/italic) if no exact match. */
    for (int i = 0; dirs[i]; i++) {
        if (scan_dir(dirs[i], resolved, 0, 0, out_path, out_size))
            return CROSSOS_OK;
    }

    return CROSSOS_ERR_IO;
}

#endif /* __ANDROID__ */

/* Suppress ISO C pedantic "empty translation unit" warning on other platforms. */
typedef int crossos_font_android_dummy_t;
