/**
 * applications/easy_raw/raw_decode.c
 *
 * Decoder for PPM P6 and simple 10-bit packed Bayer RAW files.
 *
 * Demosaicing uses a gradient-directed (AHD-lite) algorithm:
 *   1. Green channel interpolated at R/B sites with gradient weighting.
 *   2. R and B filled at G sites via G − (G−color) smoothing.
 *   3. Opposite-corner channel filled via diagonal G−color average.
 *
 * The bilinear implementation is kept for reference/testing.
 */

#include "raw_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#define STB_IMAGE_IMPLEMENTATION
#include "../../src/thirdparty/stb_image.h"

#if defined(EASYRAW_HAS_LIBRAW)
#include <libraw/libraw.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#define ER_POPEN _popen
#define ER_PCLOSE _pclose
#else
extern FILE *popen(const char *command, const char *type);
extern int pclose(FILE *stream);
#define ER_POPEN popen
#define ER_PCLOSE pclose
#endif

static int path_ext_eq(const char *path, const char *ext);

#if defined(EASYRAW_HAS_LIBRAW)
static int decode_with_libraw(const char *path, er_raw_image_t *out);
#endif

/* ── Helpers ─────────────────────────────────────────────────────────── */

static float *alloc_plane(int n)
{
    return (float *)calloc((size_t)n, sizeof(float));
}

static int planes_alloc(er_raw_image_t *img)
{
    int n = img->width * img->height;
    for (int i = 0; i < 3; i++)
    {
        img->planes[i] = alloc_plane(n);
        if (!img->planes[i])
        {
            for (int j = 0; j < i; j++)
            {
                free(img->planes[j]);
                img->planes[j] = NULL;
            }
            return -1;
        }
    }
    return 0;
}

/* ── PPM P6 decoder ──────────────────────────────────────────────────── */

/* Skip whitespace and comments in a PPM header. */
static void ppm_skip(FILE *f)
{
    int c;
    for (;;)
    {
        c = fgetc(f);
        if (c == '#')
        {
            while ((c = fgetc(f)) != '\n' && c != EOF)
            { /* skip comment */
            }
        }
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            /* skip whitespace */
        }
        else
        {
            ungetc(c, f);
            break;
        }
    }
}

static int read_ppm_uint(FILE *f, unsigned int *val)
{
    ppm_skip(f);
    if (fscanf(f, "%u", val) != 1)
        return -1;
    return 0;
}

static int decode_ppm(FILE *f, er_raw_image_t *out)
{
    unsigned int w, h, maxval;
    if (read_ppm_uint(f, &w) != 0)
        return -1;
    if (read_ppm_uint(f, &h) != 0)
        return -1;
    if (read_ppm_uint(f, &maxval) != 0)
        return -1;
    /* skip single whitespace after maxval */
    fgetc(f);

    if (w == 0 || h == 0 || maxval == 0 || maxval > 65535)
        return -1;

    out->width = (int)w;
    out->height = (int)h;
    if (planes_alloc(out) != 0)
        return -1;

    float scale = 1.0f / (float)maxval;
    int n = (int)(w * h);

    if (maxval <= 255)
    {
        /* 8-bit samples */
        uint8_t *row = (uint8_t *)malloc(w * 3);
        if (!row)
            return -1;
        for (unsigned int y = 0; y < h; y++)
        {
            if (fread(row, 1, w * 3, f) != w * 3)
            {
                free(row);
                return -1;
            }
            for (unsigned int x = 0; x < w; x++)
            {
                int idx = (int)(y * w + x);
                out->planes[0][idx] = row[x * 3 + 0] * scale;
                out->planes[1][idx] = row[x * 3 + 1] * scale;
                out->planes[2][idx] = row[x * 3 + 2] * scale;
            }
        }
        free(row);
    }
    else
    {
        /* 16-bit big-endian samples */
        uint8_t *row = (uint8_t *)malloc(w * 6);
        if (!row)
            return -1;
        for (unsigned int y = 0; y < h; y++)
        {
            if (fread(row, 1, w * 6, f) != w * 6)
            {
                free(row);
                return -1;
            }
            for (unsigned int x = 0; x < w; x++)
            {
                int idx = (int)(y * w + x);
                uint16_t r = (uint16_t)((row[x * 6 + 0] << 8) | row[x * 6 + 1]);
                uint16_t g = (uint16_t)((row[x * 6 + 2] << 8) | row[x * 6 + 3]);
                uint16_t b = (uint16_t)((row[x * 6 + 4] << 8) | row[x * 6 + 5]);
                out->planes[0][idx] = r * scale;
                out->planes[1][idx] = g * scale;
                out->planes[2][idx] = b * scale;
            }
        }
        free(row);
    }
    (void)n;
    return 0;
}

/* ── JPEG/PNG decoder via stb_image ─────────────────────────────────── */

static int decode_stb_image_file(const char *path, er_raw_image_t *out)
{
    int w = 0, h = 0, ch = 0;
    unsigned char *pix = stbi_load(path, &w, &h, &ch, 3);
    if (!pix || w <= 0 || h <= 0)
        return -1;

    /* Lightweight EXIF orientation fix for JPEGs. */
    int ori = 1;
    if (path_ext_eq(path, "jpg") || path_ext_eq(path, "jpeg"))
    {
        FILE *jf = fopen(path, "rb");
        if (jf)
        {
            int b0 = fgetc(jf), b1 = fgetc(jf);
            if (b0 == 0xFF && b1 == 0xD8)
            {
                for (;;)
                {
                    int m0 = fgetc(jf);
                    int marker = fgetc(jf);
                    if (m0 != 0xFF || marker == EOF)
                        break;
                    while (marker == 0xFF)
                        marker = fgetc(jf);
                    if (marker == 0xD9 || marker == 0xDA)
                        break;
                    int l0 = fgetc(jf), l1 = fgetc(jf);
                    if (l0 == EOF || l1 == EOF)
                        break;
                    int seg_len = (l0 << 8) | l1;
                    if (seg_len < 2)
                        break;
                    seg_len -= 2;

                    if (marker == 0xE1 && seg_len >= 14)
                    {
                        unsigned char *seg = (unsigned char *)malloc((size_t)seg_len);
                        if (!seg)
                            break;
                        if (fread(seg, 1, (size_t)seg_len, jf) != (size_t)seg_len)
                        {
                            free(seg);
                            break;
                        }

                        if (memcmp(seg, "Exif\0\0", 6) == 0)
                        {
                            const unsigned char *t = seg + 6;
                            int little = (t[0] == 'I' && t[1] == 'I');
                            int big = (t[0] == 'M' && t[1] == 'M');
                            if ((little || big) && seg_len >= 6 + 8)
                            {
#define U16P(p) (little ? (unsigned)((p)[0] | ((p)[1] << 8)) : (unsigned)(((p)[0] << 8) | (p)[1]))
#define U32P(p) (little ? (unsigned)((p)[0] | ((p)[1] << 8) | ((p)[2] << 16) | ((p)[3] << 24)) : (unsigned)(((p)[0] << 24) | ((p)[1] << 16) | ((p)[2] << 8) | (p)[3]))
                                unsigned magic = U16P(t + 2);
                                unsigned ifd0 = U32P(t + 4);
                                if (magic == 42 && ifd0 + 2 <= (unsigned)(seg_len - 6))
                                {
                                    const unsigned char *ifd = t + ifd0;
                                    unsigned nent = U16P(ifd);
                                    for (unsigned i = 0; i < nent; i++)
                                    {
                                        const unsigned char *e = ifd + 2 + i * 12;
                                        if (e + 12 > seg + seg_len)
                                            break;
                                        unsigned tag = U16P(e + 0);
                                        unsigned typ = U16P(e + 2);
                                        unsigned cnt = U32P(e + 4);
                                        if (tag == 0x0112 && typ == 3 && cnt >= 1)
                                        {
                                            unsigned v = U16P(e + 8);
                                            if (v >= 1 && v <= 8)
                                                ori = (int)v;
                                            break;
                                        }
                                    }
                                }
#undef U16P
#undef U32P
                            }
                        }

                        free(seg);
                        if (ori != 1)
                            break;
                    }
                    else
                    {
                        fseek(jf, seg_len, SEEK_CUR);
                    }
                }
            }
            fclose(jf);
        }
    }

    if (ori != 1)
    {
        int nw = (ori >= 5 && ori <= 8) ? h : w;
        int nh = (ori >= 5 && ori <= 8) ? w : h;
        unsigned char *rot = (unsigned char *)malloc((size_t)nw * nh * 3);
        if (!rot)
        {
            stbi_image_free(pix);
            return -1;
        }
        for (int y = 0; y < nh; y++)
        {
            for (int x = 0; x < nw; x++)
            {
                int sx = x, sy = y;
                switch (ori)
                {
                case 2: sx = w - 1 - x; sy = y; break;
                case 3: sx = w - 1 - x; sy = h - 1 - y; break;
                case 4: sx = x; sy = h - 1 - y; break;
                case 5: sx = y; sy = x; break;
                case 6: sx = y; sy = h - 1 - x; break;
                case 7: sx = w - 1 - y; sy = h - 1 - x; break;
                case 8: sx = w - 1 - y; sy = x; break;
                default: break;
                }
                const unsigned char *sp = pix + ((size_t)sy * w + sx) * 3;
                unsigned char *dp = rot + ((size_t)y * nw + x) * 3;
                dp[0] = sp[0];
                dp[1] = sp[1];
                dp[2] = sp[2];
            }
        }
        stbi_image_free(pix);
        pix = rot;
        w = nw;
        h = nh;
    }

    out->width = w;
    out->height = h;
    if (planes_alloc(out) != 0)
    {
        stbi_image_free(pix);
        return -1;
    }

    int n = w * h;
    for (int i = 0; i < n; i++)
    {
        float sr = (float)pix[i * 3 + 0] / 255.0f;
        float sg = (float)pix[i * 3 + 1] / 255.0f;
        float sb = (float)pix[i * 3 + 2] / 255.0f;

        /* Convert display-referred sRGB into linear working space. */
        out->planes[0][i] = (sr <= 0.04045f) ? (sr / 12.92f) : powf((sr + 0.055f) / 1.055f, 2.4f);
        out->planes[1][i] = (sg <= 0.04045f) ? (sg / 12.92f) : powf((sg + 0.055f) / 1.055f, 2.4f);
        out->planes[2][i] = (sb <= 0.04045f) ? (sb / 12.92f) : powf((sb + 0.055f) / 1.055f, 2.4f);
    }

    memset(out->cam_matrix, 0, sizeof(out->cam_matrix));
    out->white_level = 1.0;

    stbi_image_free(pix);
    return 0;
}

/* ── 10-bit packed Bayer decoder ─────────────────────────────────────── */
/*
 * Simple file format:
 *   Bytes 0-3:  uint32LE  width
 *   Bytes 4-7:  uint32LE  height
 *   Bytes 8+:   10-bit little-endian packed Bayer RGGB samples
 *               (4 samples packed into 5 bytes, LSB-first)
 */

static void bilinear_demosaic_rggb(const uint16_t *bayer,
                                   int w, int h,
                                   float *R, float *G, float *B,
                                   float scale)
{
    /* Step 1: place known values */
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            int idx = y * w + x;
            float v = bayer[idx] * scale;
            int even_row = (y % 2 == 0);
            int even_col = (x % 2 == 0);
            if (even_row && even_col)
                R[idx] = v;
            else if (even_row && !even_col)
                G[idx] = v;
            else if (!even_row && even_col)
                G[idx] = v;
            else
                B[idx] = v;
        }
    }

    /* Step 2: bilinear interpolation – simplified inner loop */
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) \
                                                         : (v))
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            int idx = y * w + x;
            int even_row = (y % 2 == 0);
            int even_col = (x % 2 == 0);

            int x0 = CLAMP(x - 1, 0, w - 1);
            int x1 = CLAMP(x + 1, 0, w - 1);
            int y0 = CLAMP(y - 1, 0, h - 1);
            int y1 = CLAMP(y + 1, 0, h - 1);

            if (even_row && even_col)
            {
                /* R pixel: interpolate G and B */
                G[idx] = (G[y * w + x0] + G[y * w + x1] +
                          G[y0 * w + x] + G[y1 * w + x]) *
                         0.25f;
                B[idx] = (B[y0 * w + x0] + B[y0 * w + x1] +
                          B[y1 * w + x0] + B[y1 * w + x1]) *
                         0.25f;
            }
            else if (!even_row && !even_col)
            {
                /* B pixel: interpolate G and R */
                G[idx] = (G[y * w + x0] + G[y * w + x1] +
                          G[y0 * w + x] + G[y1 * w + x]) *
                         0.25f;
                R[idx] = (R[y0 * w + x0] + R[y0 * w + x1] +
                          R[y1 * w + x0] + R[y1 * w + x1]) *
                         0.25f;
            }
            else
            {
                /* G pixel on red row: interpolate R left/right, B up/down */
                /* G pixel on blue row: interpolate B left/right, R up/down */
                if (even_row)
                {
                    R[idx] = (R[y * w + x0] + R[y * w + x1]) * 0.5f;
                    B[idx] = (B[y0 * w + x] + B[y1 * w + x]) * 0.5f;
                }
                else
                {
                    B[idx] = (B[y * w + x0] + B[y * w + x1]) * 0.5f;
                    R[idx] = (R[y0 * w + x] + R[y1 * w + x]) * 0.5f;
                }
            }
        }
    }
#undef CLAMP
}

/* ── AHD-lite demosaic (gradient-directed green + G-guided R/B) ───────── */
/*
 * Significantly better than bilinear at edges and fine detail:
 *   Step 1 – place known sensor samples.
 *   Step 2 – interpolate G at R/B sites using horizontal vs vertical
 *             gradient to choose the smoothest direction; Malvar-style
 *             second-order correction removes colour halo.
 *   Step 3 – fill R and B at G sites using the G−R / G−B difference
 *             maps, which are locally smooth even across edges.
 *   Step 4 – fill opposite-corner channel (B at R, R at B) by averaging
 *             the four diagonal G−colour differences.
 */
static void ahd_demosaic_rggb(const uint16_t *bayer, int w, int h,
                              float *R, float *G, float *B, float scale)
{
    int n = w * h;

/* Safe clamped-edge accessors */
#define CX(x) ((x) < 0 ? 0 : (x) >= w ? w - 1 \
                                      : (x))
#define CY(y) ((y) < 0 ? 0 : (y) >= h ? h - 1 \
                                      : (y))
#define BAY(y, x) (bayer[CY(y) * w + CX(x)] * scale)
#define GV(y, x) G[CY(y) * w + CX(x)]
#define RV(y, x) R[CY(y) * w + CX(x)]
#define BV(y, x) B[CY(y) * w + CX(x)]
#define CF(v) ((v) < 0.0f ? 0.0f : (v) > 1.0f ? 1.0f \
                                              : (v))

    memset(R, 0, (size_t)n * sizeof(float));
    memset(G, 0, (size_t)n * sizeof(float));
    memset(B, 0, (size_t)n * sizeof(float));

    /* Step 1: place known sensor samples */
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            float v = bayer[y * w + x] * scale;
            if (y % 2 == 0 && x % 2 == 0)
                R[y * w + x] = v;
            else if (y % 2 == 1 && x % 2 == 1)
                B[y * w + x] = v;
            else
                G[y * w + x] = v;
        }
    }

    /* Step 2: gradient-directed G at R and B sites */
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            if (!((y % 2 == 0 && x % 2 == 0) || (y % 2 == 1 && x % 2 == 1)))
                continue;

            float s = bayer[y * w + x] * scale;
            float gL = GV(y, x - 1), gR = GV(y, x + 1);
            float gU = GV(y - 1, x), gD = GV(y + 1, x);

            float h_grad = fabsf(gL - gR) + fabsf(s - 0.5f * (BAY(y, x - 2) + BAY(y, x + 2)));
            float v_grad = fabsf(gU - gD) + fabsf(s - 0.5f * (BAY(y - 2, x) + BAY(y + 2, x)));

            /* Malvar-He-Cutler direction-weighted correction */
            float gH = 0.5f * (gL + gR) + 0.25f * (2.0f * s - BAY(y, x - 2) - BAY(y, x + 2));
            float gV = 0.5f * (gU + gD) + 0.25f * (2.0f * s - BAY(y - 2, x) - BAY(y + 2, x));

            float g = (h_grad < v_grad) ? gH : (v_grad < h_grad) ? gV
                                                                 : 0.5f * (gH + gV);
            G[y * w + x] = CF(g);
        }
    }

    /* Step 3: R and B at G sites via G−colour difference */
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            float g = G[y * w + x];
            if (y % 2 == 0 && x % 2 == 1)
            {
                /* Gr site (green on red row): R left/right, B up/down */
                float d_r = 0.5f * ((GV(y, x - 1) - RV(y, x - 1)) + (GV(y, x + 1) - RV(y, x + 1)));
                float d_b = 0.5f * ((GV(y - 1, x) - BV(y - 1, x)) + (GV(y + 1, x) - BV(y + 1, x)));
                R[y * w + x] = CF(g - d_r);
                B[y * w + x] = CF(g - d_b);
            }
            else if (y % 2 == 1 && x % 2 == 0)
            {
                /* Gb site (green on blue row): B left/right, R up/down */
                float d_b = 0.5f * ((GV(y, x - 1) - BV(y, x - 1)) + (GV(y, x + 1) - BV(y, x + 1)));
                float d_r = 0.5f * ((GV(y - 1, x) - RV(y - 1, x)) + (GV(y + 1, x) - RV(y + 1, x)));
                B[y * w + x] = CF(g - d_b);
                R[y * w + x] = CF(g - d_r);
            }
        }
    }

    /* Step 4: opposite-corner channel via diagonal G−colour average */
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            float g = G[y * w + x];
            if (y % 2 == 0 && x % 2 == 0)
            {
                /* R site → B from four diagonal B sites */
                float d = 0.25f * ((GV(y - 1, x - 1) - BV(y - 1, x - 1)) + (GV(y - 1, x + 1) - BV(y - 1, x + 1)) +
                                   (GV(y + 1, x - 1) - BV(y + 1, x - 1)) + (GV(y + 1, x + 1) - BV(y + 1, x + 1)));
                B[y * w + x] = CF(g - d);
            }
            else if (y % 2 == 1 && x % 2 == 1)
            {
                /* B site → R from four diagonal R sites */
                float d = 0.25f * ((GV(y - 1, x - 1) - RV(y - 1, x - 1)) + (GV(y - 1, x + 1) - RV(y - 1, x + 1)) +
                                   (GV(y + 1, x - 1) - RV(y + 1, x - 1)) + (GV(y + 1, x + 1) - RV(y + 1, x + 1)));
                R[y * w + x] = CF(g - d);
            }
        }
    }

#undef CF
#undef CX
#undef CY
#undef BAY
#undef GV
#undef RV
#undef BV
}

static int decode_raw10(FILE *f, er_raw_image_t *out)
{
    uint8_t hdr[8];
    if (fread(hdr, 1, 8, f) != 8)
        return -1;
    uint32_t w = (uint32_t)(hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (hdr[3] << 24));
    uint32_t h = (uint32_t)(hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (hdr[7] << 24));
    if (w == 0 || h == 0 || w > 16384 || h > 16384)
        return -1;

    size_t n = (size_t)w * h;
    /* 4 samples per 5 bytes → packed size */
    size_t packed = (n * 5 + 3) / 4;

    uint8_t *raw_bytes = (uint8_t *)malloc(packed);
    uint16_t *bayer = (uint16_t *)malloc(n * sizeof(uint16_t));
    if (!raw_bytes || !bayer)
    {
        free(raw_bytes);
        free(bayer);
        return -1;
    }

    if (fread(raw_bytes, 1, packed, f) < packed)
    {
        free(raw_bytes);
        free(bayer);
        return -1;
    }

    /* Unpack 10-bit little-endian */
    for (size_t i = 0; i < n; i += 4)
    {
        size_t bi = i / 4 * 5;
        bayer[i + 0] = (uint16_t)(raw_bytes[bi + 0] | ((raw_bytes[bi + 4] & 0x03) << 8));
        if (i + 1 < n)
            bayer[i + 1] = (uint16_t)(raw_bytes[bi + 1] | (((raw_bytes[bi + 4] >> 2) & 0x03) << 8));
        if (i + 2 < n)
            bayer[i + 2] = (uint16_t)(raw_bytes[bi + 2] | (((raw_bytes[bi + 4] >> 4) & 0x03) << 8));
        if (i + 3 < n)
            bayer[i + 3] = (uint16_t)(raw_bytes[bi + 3] | (((raw_bytes[bi + 4] >> 6) & 0x03) << 8));
    }
    free(raw_bytes);

    out->width = (int)w;
    out->height = (int)h;
    if (planes_alloc(out) != 0)
    {
        free(bayer);
        return -1;
    }

    float scale = 1.0f / 1023.0f; /* 10-bit max */
    ahd_demosaic_rggb(bayer, (int)w, (int)h,
                      out->planes[0], out->planes[1], out->planes[2],
                      scale);
    free(bayer);

    /* No camera matrix available for this simple raw10 container. */
    memset(out->cam_matrix, 0, sizeof(out->cam_matrix));
    out->white_level = 1.0;
    return 0;
}

/* ── RAF fallback via dcraw (if installed) ──────────────────────────── */

static int path_ext_eq(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    if (!dot || !dot[1])
        return 0;
    dot++;

    while (*dot && *ext)
    {
        if (tolower((unsigned char)*dot) != tolower((unsigned char)*ext))
            return 0;
        dot++;
        ext++;
    }
    return (*dot == '\0' && *ext == '\0');
}

#if defined(EASYRAW_HAS_LIBRAW)
static int decode_with_libraw(const char *path, er_raw_image_t *out)
{
    libraw_data_t *lr = libraw_init(0);
    if (!lr)
        return -1;

    lr->params.use_camera_wb = 1;
    lr->params.no_auto_bright = 1;
    lr->params.output_bps = 16;

    int rc = libraw_open_file(lr, path);
    if (rc != LIBRAW_SUCCESS)
    {
        libraw_close(lr);
        return -1;
    }

    rc = libraw_unpack(lr);
    if (rc != LIBRAW_SUCCESS)
    {
        libraw_close(lr);
        return -1;
    }

    rc = libraw_dcraw_process(lr);
    if (rc != LIBRAW_SUCCESS)
    {
        libraw_close(lr);
        return -1;
    }

    int err = LIBRAW_SUCCESS;
    libraw_processed_image_t *img = libraw_dcraw_make_mem_image(lr, &err);
    if (!img || err != LIBRAW_SUCCESS)
    {
        if (img)
            libraw_dcraw_clear_mem(img);
        libraw_close(lr);
        return -1;
    }

    if (img->type != LIBRAW_IMAGE_BITMAP || img->colors < 3 || img->width <= 0 || img->height <= 0)
    {
        libraw_dcraw_clear_mem(img);
        libraw_close(lr);
        return -1;
    }

    out->width = (int)img->width;
    out->height = (int)img->height;
    if (planes_alloc(out) != 0)
    {
        libraw_dcraw_clear_mem(img);
        libraw_close(lr);
        return -1;
    }

    int n = out->width * out->height;
    int ch = img->colors;
    int bits = img->bits;
    const float scale = (bits > 8) ? (1.0f / 65535.0f) : (1.0f / 255.0f);

    if (bits > 8)
    {
        const uint16_t *p = (const uint16_t *)img->data;
        for (int i = 0; i < n; i++)
        {
            int b = i * ch;
            out->planes[0][i] = (float)p[b + 0] * scale;
            out->planes[1][i] = (float)p[b + 1] * scale;
            out->planes[2][i] = (float)p[b + 2] * scale;
        }
    }
    else
    {
        const uint8_t *p = (const uint8_t *)img->data;
        for (int i = 0; i < n; i++)
        {
            int b = i * ch;
            out->planes[0][i] = (float)p[b + 0] * scale;
            out->planes[1][i] = (float)p[b + 1] * scale;
            out->planes[2][i] = (float)p[b + 2] * scale;
        }
    }

    memset(out->cam_matrix, 0, sizeof(out->cam_matrix));
    out->white_level = 1.0;

    libraw_dcraw_clear_mem(img);
    libraw_close(lr);
    return 0;
}
#endif

static int decode_raf_via_dcraw(const char *path, er_raw_image_t *out)
{
    int has_dcraw = 0;
#if defined(_WIN32)
    has_dcraw = (system("where dcraw >NUL 2>NUL") == 0);
#else
    has_dcraw = (system("command -v dcraw >/dev/null 2>&1") == 0);
#endif
    if (!has_dcraw)
        return -1;

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "dcraw -c -w \"%s\" 2>/dev/null", path);

    FILE *pipe = ER_POPEN(cmd, "rb");
    if (!pipe)
        return -1;

    uint8_t magic[2];
    size_t nr = fread(magic, 1, 2, pipe);
    int rc = -1;
    if (nr == 2 && magic[0] == 'P' && (magic[1] == '6' || magic[1] == '5'))
    {
        rc = decode_ppm(pipe, out);
    }

    ER_PCLOSE(pipe);
    if (rc != 0)
        er_raw_image_free(out);
    return rc;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int er_raw_image_load(const char *path, er_raw_image_t *out)
{
    if (!path || !out)
        return -1;
    memset(out, 0, sizeof(*out));

    if (path_ext_eq(path, "jpg") || path_ext_eq(path, "jpeg") ||
        path_ext_eq(path, "png"))
    {
        return decode_stb_image_file(path, out);
    }

    if (path_ext_eq(path, "raf"))
    {
#if defined(EASYRAW_HAS_LIBRAW)
        if (decode_with_libraw(path, out) == 0)
            return 0;
#endif
        return decode_raf_via_dcraw(path, out);
    }

    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    /* Sniff the format */
    uint8_t magic[2];
    size_t nr = fread(magic, 1, 2, f);
    rewind(f);

    int rc = -1;
    if (nr == 2 && magic[0] == 'P' && (magic[1] == '6' || magic[1] == '5'))
    {
        /* PPM / PGM */
        fgetc(f);
        fgetc(f); /* consume "P6" or "P5" */
        rc = decode_ppm(f, out);
    }
    else
    {
        /* Assume 10-bit packed Bayer RAW */
        rc = decode_raw10(f, out);
    }

    fclose(f);
    if (rc != 0)
        er_raw_image_free(out);
    return rc;
}

void er_raw_image_free(er_raw_image_t *img)
{
    if (!img)
        return;
    for (int i = 0; i < 3; i++)
    {
        free(img->planes[i]);
        img->planes[i] = NULL;
    }
}
