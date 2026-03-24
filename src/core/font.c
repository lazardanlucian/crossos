/*
 * src/core/font.c  –  OTF/TTF font loading and Unicode text rendering.
 *
 * Backend: stb_truetype v1.26 (public domain, Sean Barrett / RAD Game Tools).
 * Bundled font: Liberation Sans Regular (SIL OFL 1.1).
 *
 * Each crossos_typeface_t holds:
 *   • A copy of the raw font bytes (so the source file/buffer can be freed).
 *   • An initialised stbtt_fontinfo struct pointing into that copy.
 *   • A simple per-size glyph-bitmap cache keyed on (codepoint, pixel_size).
 *
 * The glyph cache uses a fixed-capacity open-addressing hash table.  When it
 * fills up the oldest entry is evicted (FIFO via a generation counter).
 * Cache lookups are O(1) amortised; each miss calls stbtt_GetCodepointBitmap
 * once and stores the result.
 */

/* ── stb_truetype single-file implementation ────────────────────────── */
#define STB_TRUETYPE_IMPLEMENTATION
/* Disable the default stdio FILE* dependency - we load fonts ourselves. */
#include "../thirdparty/stb_truetype.h"

#include <crossos/font.h>

/* Bundled font byte array (Liberation Sans Regular, SIL OFL 1.1). */
#include "../thirdparty/crossos_builtin_font_data.h"

/* Platform system-font finder (declared in each platform backend source). */
crossos_result_t font_platform_find_system(const char               *family,
                                           crossos_typeface_style_t  style,
                                           char                     *out_path,
                                           size_t                    out_size);

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>

/* ── Glyph cache ─────────────────────────────────────────────────────── */

#define GLYPH_CACHE_CAP 512   /* entries; must be a power of two */

typedef struct {
    unsigned int  codepoint;
    float         pixel_size;
    unsigned char *bitmap;    /* malloc'd; NULL = vacant slot    */
    int           bm_w;
    int           bm_h;
    int           xoff;
    int           yoff;
    int           advance;
    int           lsb;
    unsigned int  gen;        /* FIFO generation counter         */
} glyph_entry_t;

/* ── Typeface handle ─────────────────────────────────────────────────── */

struct crossos_typeface {
    unsigned char   *font_data; /* owns the raw font bytes                 */
    size_t           font_size;
    stbtt_fontinfo   info;

    glyph_entry_t    cache[GLYPH_CACHE_CAP];
    unsigned int     gen_counter; /* monotonically increasing               */
};

/* ── UTF-8 decoder ───────────────────────────────────────────────────── */

static unsigned int utf8_next_cp(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    unsigned int c = *s;
    if (c == 0)   { return 0; }
    if (c < 0x80) { *p += 1; return c; }
    if ((c & 0xE0) == 0xC0) {
        if ((s[1] & 0xC0) != 0x80) { *p += 1; return 0xFFFD; }
        *p += 2;
        unsigned int cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        return cp >= 0x80 ? cp : 0xFFFD;
    }
    if ((c & 0xF0) == 0xE0) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) { *p += 1; return 0xFFFD; }
        *p += 3;
        unsigned int cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return cp >= 0x800 ? cp : 0xFFFD;
    }
    if ((c & 0xF8) == 0xF0) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) {
            *p += 1; return 0xFFFD;
        }
        *p += 4;
        unsigned int cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12)
                        | ((s[2] & 0x3F) << 6)  | (s[3] & 0x3F);
        return cp >= 0x10000 ? cp : 0xFFFD;
    }
    *p += 1;
    return 0xFFFD;
}

/* ── Glyph cache helpers ─────────────────────────────────────────────── */

static unsigned int cache_hash(unsigned int cp, float sz)
{
    /* Mix codepoint with a quantised size to form a hash key. */
    unsigned int isz = (unsigned int)(sz * 16.0f + 0.5f);
    unsigned int h = cp * 2654435761u ^ isz * 2246822519u;
    return h & (GLYPH_CACHE_CAP - 1);
}

/* Look up (cp, pixel_size) in the cache. Returns NULL on miss. */
static glyph_entry_t *cache_lookup(struct crossos_typeface *face,
                                   unsigned int cp, float sz)
{
    unsigned int h = cache_hash(cp, sz);
    for (unsigned int i = 0; i < GLYPH_CACHE_CAP; i++) {
        unsigned int idx = (h + i) & (GLYPH_CACHE_CAP - 1);
        glyph_entry_t *e = &face->cache[idx];
        if (!e->bitmap) return NULL; /* vacant – stop probing */
        if (e->codepoint == cp && e->pixel_size == sz) return e;
    }
    return NULL;
}

/* Insert (or replace oldest) an entry in the cache.
 * The bitmap pointer is transferred to the cache (cache owns it). */
static glyph_entry_t *cache_insert(struct crossos_typeface *face,
                                   unsigned int cp, float sz,
                                   unsigned char *bm, int bw, int bh,
                                   int xoff, int yoff, int advance, int lsb)
{
    unsigned int h = cache_hash(cp, sz);

    /* First pass: find a vacant slot. */
    for (unsigned int i = 0; i < GLYPH_CACHE_CAP; i++) {
        unsigned int idx = (h + i) & (GLYPH_CACHE_CAP - 1);
        glyph_entry_t *e = &face->cache[idx];
        if (!e->bitmap) {
            e->codepoint  = cp;
            e->pixel_size = sz;
            e->bitmap     = bm;
            e->bm_w       = bw;
            e->bm_h       = bh;
            e->xoff       = xoff;
            e->yoff       = yoff;
            e->advance    = advance;
            e->lsb        = lsb;
            e->gen        = face->gen_counter++;
            return e;
        }
    }

    /* Cache is full: evict the entry with the smallest generation value. */
    unsigned int oldest_gen = UINT_MAX;
    glyph_entry_t *victim   = &face->cache[0];
    for (int i = 0; i < GLYPH_CACHE_CAP; i++) {
        if (face->cache[i].gen < oldest_gen) {
            oldest_gen = face->cache[i].gen;
            victim     = &face->cache[i];
        }
    }
    free(victim->bitmap);
    victim->codepoint  = cp;
    victim->pixel_size = sz;
    victim->bitmap     = bm;
    victim->bm_w       = bw;
    victim->bm_h       = bh;
    victim->xoff       = xoff;
    victim->yoff       = yoff;
    victim->advance    = advance;
    victim->lsb        = lsb;
    victim->gen        = face->gen_counter++;
    return victim;
}

/* Get (or rasterise) a glyph for (codepoint, pixel_size). */
static glyph_entry_t *get_glyph(struct crossos_typeface *face,
                                 unsigned int cp, float sz)
{
    glyph_entry_t *e = cache_lookup(face, cp, sz);
    if (e) return e;

    float scale = stbtt_ScaleForPixelHeight(&face->info, sz);

    int advance, lsb;
    stbtt_GetCodepointHMetrics(&face->info, (int)cp, &advance, &lsb);

    int bw = 0, bh = 0, xoff = 0, yoff = 0;
    unsigned char *bm = stbtt_GetCodepointBitmap(&face->info,
                                                  scale, scale,
                                                  (int)cp,
                                                  &bw, &bh, &xoff, &yoff);
    /* bm may be NULL for whitespace/invisible glyphs – that is valid. */

    return cache_insert(face, cp, sz, bm, bw, bh, xoff, yoff,
                        (int)roundf((float)advance * scale),
                        (int)roundf((float)lsb     * scale));
}

/* ── Internal allocation ─────────────────────────────────────────────── */

static crossos_typeface_t *alloc_face(const unsigned char *data, size_t size)
{
    crossos_typeface_t *face = calloc(1, sizeof(*face));
    if (!face) return NULL;

    face->font_data = malloc(size);
    if (!face->font_data) { free(face); return NULL; }
    memcpy(face->font_data, data, size);
    face->font_size = size;

    if (!stbtt_InitFont(&face->info, face->font_data, 0)) {
        free(face->font_data);
        free(face);
        return NULL;
    }
    return face;
}

/* ── Public API ──────────────────────────────────────────────────────── */

crossos_result_t crossos_typeface_load_file(const char *path,
                                            crossos_typeface_t **out_face)
{
    if (!path || !out_face) return CROSSOS_ERR_PARAM;
    *out_face = NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return CROSSOS_ERR_IO;

    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsz <= 0) { fclose(f); return CROSSOS_ERR_IO; }

    unsigned char *buf = malloc((size_t)fsz);
    if (!buf) { fclose(f); return CROSSOS_ERR_OOM; }

    if (fread(buf, 1, (size_t)fsz, f) != (size_t)fsz) {
        free(buf); fclose(f); return CROSSOS_ERR_IO;
    }
    fclose(f);

    crossos_typeface_t *face = alloc_face(buf, (size_t)fsz);
    free(buf);
    if (!face) return CROSSOS_ERR_PARAM; /* stbtt_InitFont failed */

    *out_face = face;
    return CROSSOS_OK;
}

crossos_result_t crossos_typeface_load_memory(const void *data, size_t size,
                                              crossos_typeface_t **out_face)
{
    if (!data || size == 0 || !out_face) return CROSSOS_ERR_PARAM;
    *out_face = NULL;

    crossos_typeface_t *face = alloc_face((const unsigned char *)data, size);
    if (!face) return CROSSOS_ERR_PARAM;

    *out_face = face;
    return CROSSOS_OK;
}

crossos_result_t crossos_typeface_load_builtin(crossos_typeface_t **out_face)
{
    if (!out_face) return CROSSOS_ERR_PARAM;
    *out_face = NULL;

    crossos_typeface_t *face = alloc_face(crossos__builtin_font_data,
                                          (size_t)crossos__builtin_font_size);
    if (!face) return CROSSOS_ERR_OOM;

    *out_face = face;
    return CROSSOS_OK;
}

crossos_result_t crossos_typeface_load_system(const char               *family_name,
                                              crossos_typeface_style_t  style,
                                              crossos_typeface_t       **out_face)
{
    if (!family_name || !out_face) return CROSSOS_ERR_PARAM;
    *out_face = NULL;

    char path[1024];
    crossos_result_t rc = font_platform_find_system(family_name, style,
                                                    path, sizeof(path));
    if (rc != CROSSOS_OK) return rc;

    return crossos_typeface_load_file(path, out_face);
}

void crossos_typeface_destroy(crossos_typeface_t *face)
{
    if (!face) return;
    for (int i = 0; i < GLYPH_CACHE_CAP; i++) {
        if (face->cache[i].bitmap)
            free(face->cache[i].bitmap);
    }
    free(face->font_data);
    free(face);
}

/* ── Metrics ─────────────────────────────────────────────────────────── */

crossos_result_t crossos_typeface_measure(const crossos_typeface_t *face,
                                          const char               *text,
                                          float                     pixel_size,
                                          crossos_text_metrics_t   *out)
{
    if (!face || !text || pixel_size <= 0.0f || !out) return CROSSOS_ERR_PARAM;

    float scale = stbtt_ScaleForPixelHeight(
                      (stbtt_fontinfo *)&face->info, pixel_size);

    int asc, desc, lg;
    stbtt_GetFontVMetrics((stbtt_fontinfo *)&face->info, &asc, &desc, &lg);

    out->ascent  = (int)ceilf( (float)asc  * scale);
    out->descent = (int)ceilf(-(float)desc * scale); /* positive down */
    out->height  = out->ascent + out->descent;

    /* Accumulate advance widths across codepoints. */
    int total = 0;
    const char *p = text;
    unsigned int cp;
    while ((cp = utf8_next_cp(&p)) != 0) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics((stbtt_fontinfo *)&face->info,
                                   (int)cp, &adv, &lsb);
        total += (int)roundf((float)adv * scale);

        /* Apply kern between this and the next codepoint. */
        const char *peek = p;
        unsigned int next = utf8_next_cp(&peek);
        if (next) {
            int kern = stbtt_GetCodepointKernAdvance(
                           (stbtt_fontinfo *)&face->info, (int)cp, (int)next);
            total += (int)roundf((float)kern * scale);
        }
    }

    out->width = total;
    return CROSSOS_OK;
}

/* ── Rendering ───────────────────────────────────────────────────────── */

crossos_result_t crossos_typeface_draw_text(const crossos_framebuffer_t *fb,
                                            const crossos_typeface_t    *face,
                                            int                          x,
                                            int                          y,
                                            const char                  *text,
                                            float                        pixel_size,
                                            crossos_color_t              color)
{
    if (!fb || !face || !text || pixel_size <= 0.0f) return CROSSOS_ERR_PARAM;
    if (!fb->pixels) return CROSSOS_ERR_PARAM;

    /* Compute ascent so y is the top of the line (not the baseline). */
    float scale = stbtt_ScaleForPixelHeight(
                      (stbtt_fontinfo *)&face->info, pixel_size);
    int asc, desc, lg;
    stbtt_GetFontVMetrics((stbtt_fontinfo *)&face->info, &asc, &desc, &lg);
    int baseline = y + (int)ceilf((float)asc * scale);

    int pen_x = x;
    const char *p = text;
    unsigned int cp;

    while ((cp = utf8_next_cp(&p)) != 0) {
        /* Skip non-printable control characters. */
        if (cp < 0x20 && cp != '\t') continue;

        glyph_entry_t *g = get_glyph((crossos_typeface_t *)face, cp, pixel_size);
        if (!g) return CROSSOS_ERR_OOM;

        /* Render the glyph bitmap (may be NULL for spaces/invisible glyphs). */
        if (g->bitmap && g->bm_w > 0 && g->bm_h > 0) {
            int gx = pen_x + g->xoff;
            int gy = baseline + g->yoff;

            for (int gy2 = 0; gy2 < g->bm_h; gy2++) {
                int fbY = gy + gy2;
                if (fbY < 0 || fbY >= fb->height) continue;
                for (int gx2 = 0; gx2 < g->bm_w; gx2++) {
                    int fbX = gx + gx2;
                    if (fbX < 0 || fbX >= fb->width) continue;

                    unsigned char alpha = g->bitmap[gy2 * g->bm_w + gx2];
                    if (alpha == 0) continue;

                    /* Combine glyph alpha with color.a, then blend. */
                    unsigned int combined = ((unsigned int)alpha *
                                            (unsigned int)color.a + 127) >> 8;
                    if (combined == 0) continue;

                    unsigned char *dst =
                        (unsigned char *)fb->pixels
                        + fbY * fb->stride + fbX * 4;

                    if (combined == 255) {
                        dst[0] = color.b;
                        dst[1] = color.g;
                        dst[2] = color.r;
                        dst[3] = 255;
                    } else {
                        unsigned int ia = 255 - combined;
                        dst[0] = (unsigned char)(
                            (color.b * combined + dst[0] * ia + 127) >> 8);
                        dst[1] = (unsigned char)(
                            (color.g * combined + dst[1] * ia + 127) >> 8);
                        dst[2] = (unsigned char)(
                            (color.r * combined + dst[2] * ia + 127) >> 8);
                        dst[3] = 255;
                    }
                }
            }
        }

        pen_x += g->advance;

        /* Kern between current and next codepoint. */
        const char *peek = p;
        unsigned int next = utf8_next_cp(&peek);
        if (next) {
            int kern = stbtt_GetCodepointKernAdvance(
                           (stbtt_fontinfo *)&face->info, (int)cp, (int)next);
            pen_x += (int)roundf((float)kern * scale);
        }
    }

    return CROSSOS_OK;
}

crossos_result_t crossos_typeface_draw_text_aligned(
                                    const crossos_framebuffer_t *fb,
                                    const crossos_typeface_t    *face,
                                    int                          x,
                                    int                          y,
                                    const char                  *text,
                                    float                        pixel_size,
                                    crossos_color_t              color,
                                    crossos_text_align_t         align)
{
    if (align == CROSSOS_TEXT_ALIGN_LEFT)
        return crossos_typeface_draw_text(fb, face, x, y, text, pixel_size, color);

    crossos_text_metrics_t m;
    crossos_result_t rc = crossos_typeface_measure(face, text, pixel_size, &m);
    if (rc != CROSSOS_OK) return rc;

    int ox = x;
    if (align == CROSSOS_TEXT_ALIGN_CENTER) ox = x - m.width / 2;
    else if (align == CROSSOS_TEXT_ALIGN_RIGHT) ox = x - m.width;

    return crossos_typeface_draw_text(fb, face, ox, y, text, pixel_size, color);
}
