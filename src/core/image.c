/*
 * src/core/image.c  –  Image loading and rendering implementation.
 *
 * Supports:
 *   BMP  – uncompressed DIB (BI_RGB), 24-bit and 32-bit depths.
 *          Bottom-up and top-down row orders are both handled.
 *   PPM  – binary P6, 8-bit per channel (maxval <= 255).
 *
 * All decoded images are stored as RGBA8888 in host memory.
 */

#include <crossos/image.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Internal helpers ────────────────────────────────────────────────── */

static crossos_image_t *alloc_image(int width, int height)
{
    if (width <= 0 || height <= 0) return NULL;
    crossos_image_t *img = malloc(sizeof(*img));
    if (!img) return NULL;
    img->width  = width;
    img->height = height;
    img->stride = width * 4;
    img->pixels = calloc((size_t)(height * img->stride), 1);
    if (!img->pixels) { free(img); return NULL; }
    return img;
}

/* Read little-endian 16-bit value from buf. */
static unsigned int read_u16le(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/* Read little-endian 32-bit value from buf. */
static unsigned int read_u32le(const unsigned char *p)
{
    return (unsigned int)p[0]
         | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16)
         | ((unsigned int)p[3] << 24);
}

/* Read signed 32-bit from buf. */
static int read_i32le(const unsigned char *p)
{
    return (int)read_u32le(p);
}

/* ── BMP loader ──────────────────────────────────────────────────────── */
/*
 * Minimal BMP loader: supports BI_RGB (compression=0), 24-bit and 32-bit.
 * Both bottom-up (positive height) and top-down (negative height) BMPs.
 */

crossos_result_t crossos_image_load_bmp(const char *path, crossos_image_t **out)
{
    if (!path || !out) return CROSSOS_ERR_PARAM;
    *out = NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return CROSSOS_ERR_IO;

    /* --- BITMAPFILEHEADER (14 bytes) ------------------------------------ */
    unsigned char fhdr[14];
    if (fread(fhdr, 1, 14, f) != 14) { fclose(f); return CROSSOS_ERR_IO; }
    if (fhdr[0] != 'B' || fhdr[1] != 'M') { fclose(f); return CROSSOS_ERR_PARAM; }
    unsigned int pixel_offset = read_u32le(fhdr + 10);

    /* --- BITMAPINFOHEADER (40 bytes minimum) ----------------------------- */
    unsigned char ihdr[40];
    if (fread(ihdr, 1, 40, f) != 40) { fclose(f); return CROSSOS_ERR_IO; }

    unsigned int hdr_size   = read_u32le(ihdr);
    int          img_width  = read_i32le(ihdr + 4);
    int          img_height = read_i32le(ihdr + 8);
    unsigned int bit_count  = read_u16le(ihdr + 14);
    unsigned int compression= read_u32le(ihdr + 16);

    if (img_width <= 0) { fclose(f); return CROSSOS_ERR_PARAM; }
    if (compression != 0 /* BI_RGB */) {
        fclose(f);
        return CROSSOS_ERR_PARAM; /* compressed BMPs not supported */
    }
    if (bit_count != 24 && bit_count != 32) {
        fclose(f);
        return CROSSOS_ERR_PARAM; /* only true-colour BMPs */
    }

    /* Rows can be stored bottom-up (positive height) or top-down (negative). */
    int top_down = 0;
    int height   = img_height;
    if (height < 0) { height = -height; top_down = 1; }

    /* Skip any extra header bytes. */
    long extra = (long)hdr_size - 40;
    if (extra > 0) fseek(f, extra, SEEK_CUR);

    /* Seek to pixel data. */
    fseek(f, (long)pixel_offset, SEEK_SET);

    crossos_image_t *img = alloc_image(img_width, height);
    if (!img) { fclose(f); return CROSSOS_ERR_OOM; }

    /* Each BMP row is padded to a multiple of 4 bytes. */
    int bpp      = (int)(bit_count / 8);
    int row_size = (img_width * bpp + 3) & ~3;
    unsigned char *row_buf = malloc((size_t)row_size);
    if (!row_buf) { crossos_image_destroy(img); fclose(f); return CROSSOS_ERR_OOM; }

    for (int y = 0; y < height; y++) {
        if (fread(row_buf, 1, (size_t)row_size, f) != (size_t)row_size) break;

        /* Map BMP y (stored bottom-up by default) to image y. */
        int dst_y = top_down ? y : (height - 1 - y);
        unsigned char *dst = img->pixels + dst_y * img->stride;

        for (int x = 0; x < img_width; x++) {
            const unsigned char *src = row_buf + x * bpp;
            /* BMP stores BGR[A], we want RGBA. */
            dst[x * 4 + 0] = src[2]; /* R */
            dst[x * 4 + 1] = src[1]; /* G */
            dst[x * 4 + 2] = src[0]; /* B */
            dst[x * 4 + 3] = (bpp == 4) ? src[3] : 255;
        }
    }

    free(row_buf);
    fclose(f);
    *out = img;
    return CROSSOS_OK;
}

/* ── PPM loader ──────────────────────────────────────────────────────── */
/*
 * Supports binary PPM (P6) with maxval <= 255.
 */

/* Skip whitespace and '#' comment lines in a PPM header. */
static void ppm_skip(FILE *f)
{
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '#') {
            while ((c = fgetc(f)) != EOF && c != '\n') {}
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            ungetc(c, f);
            return;
        }
    }
}

/* Read a decimal integer from a PPM header. */
static int ppm_read_int(FILE *f)
{
    ppm_skip(f);
    int val = 0, got = 0;
    int c;
    while ((c = fgetc(f)) != EOF && c >= '0' && c <= '9') {
        val = val * 10 + (c - '0');
        got = 1;
    }
    return got ? val : -1;
}

crossos_result_t crossos_image_load_ppm(const char *path, crossos_image_t **out)
{
    if (!path || !out) return CROSSOS_ERR_PARAM;
    *out = NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return CROSSOS_ERR_IO;

    /* Magic */
    char magic[3] = {0};
    if (fread(magic, 1, 2, f) != 2 || magic[0] != 'P' || magic[1] != '6') {
        fclose(f);
        return CROSSOS_ERR_PARAM;
    }

    int width  = ppm_read_int(f);
    int height = ppm_read_int(f);
    int maxval = ppm_read_int(f);

    /* One mandatory whitespace byte follows maxval. */
    fgetc(f);

    if (width <= 0 || height <= 0 || maxval <= 0 || maxval > 255) {
        fclose(f);
        return CROSSOS_ERR_PARAM;
    }

    crossos_image_t *img = alloc_image(width, height);
    if (!img) { fclose(f); return CROSSOS_ERR_OOM; }

    for (int y = 0; y < height; y++) {
        unsigned char *row = img->pixels + y * img->stride;
        for (int x = 0; x < width; x++) {
            unsigned char rgb[3];
            if (fread(rgb, 1, 3, f) != 3) goto done;
            row[x * 4 + 0] = rgb[0];
            row[x * 4 + 1] = rgb[1];
            row[x * 4 + 2] = rgb[2];
            row[x * 4 + 3] = 255;
        }
    }
done:
    fclose(f);
    *out = img;
    return CROSSOS_OK;
}

/* ── Creation / destruction ──────────────────────────────────────────── */

crossos_result_t crossos_image_create(int width, int height, crossos_image_t **out)
{
    if (!out) return CROSSOS_ERR_PARAM;
    *out = NULL;
    crossos_image_t *img = alloc_image(width, height);
    if (!img) return (width <= 0 || height <= 0) ? CROSSOS_ERR_PARAM
                                                  : CROSSOS_ERR_OOM;
    *out = img;
    return CROSSOS_OK;
}

void crossos_image_destroy(crossos_image_t *img)
{
    if (!img) return;
    free(img->pixels);
    free(img);
}

/* ── Rendering helpers ───────────────────────────────────────────────── */

/* Blend a single RGBA pixel onto the framebuffer at (fx, fy). */
static void fb_put_rgba(const crossos_framebuffer_t *fb,
                        int fx, int fy,
                        unsigned char r, unsigned char g,
                        unsigned char b, unsigned char a)
{
    if (!fb || !fb->pixels) return;
    if (fx < 0 || fy < 0 || fx >= fb->width || fy >= fb->height) return;

    unsigned char *p = (unsigned char *)fb->pixels
                     + fy * fb->stride + fx * 4;

    if (a == 255) {
        /* Framebuffer is BGRA on most backends; draw.c uses B,G,R,A order. */
        p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
    } else if (a > 0) {
        int ia = 255 - (int)a;
        p[0] = (unsigned char)(((int)b * a + p[0] * ia + 127) >> 8);
        p[1] = (unsigned char)(((int)g * a + p[1] * ia + 127) >> 8);
        p[2] = (unsigned char)(((int)r * a + p[2] * ia + 127) >> 8);
        p[3] = 255;
    }
}

/* ── Public rendering API ────────────────────────────────────────────── */

void crossos_image_blit(const crossos_framebuffer_t *fb,
                        const crossos_image_t       *img,
                        int                          dst_x,
                        int                          dst_y)
{
    if (!fb || !img || !img->pixels) return;

    for (int y = 0; y < img->height; y++) {
        const unsigned char *row = img->pixels + y * img->stride;
        for (int x = 0; x < img->width; x++) {
            fb_put_rgba(fb,
                        dst_x + x, dst_y + y,
                        row[x * 4 + 0],
                        row[x * 4 + 1],
                        row[x * 4 + 2],
                        row[x * 4 + 3]);
        }
    }
}

void crossos_image_blit_scaled(const crossos_framebuffer_t *fb,
                               const crossos_image_t       *img,
                               int                          dst_x,
                               int                          dst_y,
                               int                          dst_w,
                               int                          dst_h)
{
    if (!fb || !img || !img->pixels) return;
    if (dst_w <= 0 || dst_h <= 0) return;

    for (int dy = 0; dy < dst_h; dy++) {
        int sy = (dy * img->height) / dst_h;
        const unsigned char *src_row = img->pixels + sy * img->stride;
        for (int dx = 0; dx < dst_w; dx++) {
            int sx = (dx * img->width) / dst_w;
            const unsigned char *p = src_row + sx * 4;
            fb_put_rgba(fb,
                        dst_x + dx, dst_y + dy,
                        p[0], p[1], p[2], p[3]);
        }
    }
}

void crossos_image_blit_region(const crossos_framebuffer_t *fb,
                               const crossos_image_t       *img,
                               int                          src_x,
                               int                          src_y,
                               int                          src_w,
                               int                          src_h,
                               int                          dst_x,
                               int                          dst_y)
{
    if (!fb || !img || !img->pixels) return;

    /* Clamp source region to image bounds. */
    if (src_x < 0) { dst_x -= src_x; src_w += src_x; src_x = 0; }
    if (src_y < 0) { dst_y -= src_y; src_h += src_y; src_y = 0; }
    if (src_x + src_w > img->width)  src_w = img->width  - src_x;
    if (src_y + src_h > img->height) src_h = img->height - src_y;
    if (src_w <= 0 || src_h <= 0) return;

    for (int y = 0; y < src_h; y++) {
        const unsigned char *row = img->pixels + (src_y + y) * img->stride;
        for (int x = 0; x < src_w; x++) {
            const unsigned char *p = row + (src_x + x) * 4;
            fb_put_rgba(fb,
                        dst_x + x, dst_y + y,
                        p[0], p[1], p[2], p[3]);
        }
    }
}

unsigned char *crossos_image_pixel_at(const crossos_image_t *img, int x, int y)
{
    if (!img || !img->pixels) return NULL;
    if (x < 0 || y < 0 || x >= img->width || y >= img->height) return NULL;
    return img->pixels + y * img->stride + x * 4;
}
