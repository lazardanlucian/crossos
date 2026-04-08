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

    /* Identity colour matrix */
    out->cam_matrix[0][0] = 1.0;
    out->cam_matrix[1][1] = 1.0;
    out->cam_matrix[2][2] = 1.0;
    out->white_level = 1.0;
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int er_raw_image_load(const char *path, er_raw_image_t *out)
{
    if (!path || !out)
        return -1;
    memset(out, 0, sizeof(*out));

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
