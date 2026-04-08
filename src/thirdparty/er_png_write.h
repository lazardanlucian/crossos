/**
 * er_png_write.h  —  Minimal single-header PNG writer (no zlib dependency).
 *
 * Uses PNG uncompressed DEFLATE (BTYPE=00) blocks, which is valid PNG and
 * universally readable.  Files are larger than fully-compressed PNG but
 * correctness is guaranteed without a zlib dependency.
 *
 * Usage:
 *   #define ER_PNG_WRITE_IMPLEMENTATION
 *   #include "er_png_write.h"
 *
 * API:
 *   int er_png_write_rgba(const char *path, const uint8_t *pixels,
 *                         int width, int height, int stride_bytes);
 *   Returns 0 on success, -1 on error.
 *
 * License: public domain / CC0
 */

#ifndef ER_PNG_WRITE_H
#define ER_PNG_WRITE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    int er_png_write_rgba(const char *path,
                          const uint8_t *pixels,
                          int width, int height, int stride_bytes);

#ifdef __cplusplus
}
#endif

/* ── Implementation ─────────────────────────────────────────────────── */
#ifdef ER_PNG_WRITE_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── CRC-32 table ─────────────────────────────────────────────────── */
static uint32_t er__crc_table[256];
static int er__crc_init = 0;

static void er__build_crc(void)
{
    for (uint32_t n = 0; n < 256; n++)
    {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        er__crc_table[n] = c;
    }
    er__crc_init = 1;
}

static uint32_t er__crc32(uint32_t crc, const uint8_t *buf, size_t len)
{
    if (!er__crc_init)
        er__build_crc();
    crc ^= 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++)
        crc = er__crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFU;
}

/* ── Adler-32 ─────────────────────────────────────────────────────── */
static uint32_t er__adler32(const uint8_t *buf, size_t len)
{
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < len; i++)
    {
        s1 = (s1 + buf[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    return (s2 << 16) | s1;
}

/* ── Write helpers ────────────────────────────────────────────────── */
static void er__w32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16),
                    (uint8_t)(v >> 8), (uint8_t)(v)};
    fwrite(b, 1, 4, f);
}

static void er__chunk(FILE *f, const char type[4],
                      const uint8_t *data, size_t len)
{
    er__w32(f, (uint32_t)len);
    fwrite(type, 1, 4, f);
    if (data && len)
        fwrite(data, 1, len, f);
    /* CRC over type + data */
    uint32_t crc = er__crc32(0, (const uint8_t *)type, 4);
    if (data && len)
        crc = er__crc32(crc, data, len);
    er__w32(f, crc);
}

/* ── IDAT payload: zlib with uncompressed (stored) DEFLATE blocks ─── */
/*
 * zlib header + one or more non-compressed DEFLATE blocks + adler32.
 * Each DEFLATE block holds at most 65535 bytes of data.
 */
static uint8_t *er__make_idat(const uint8_t *raw, size_t raw_len,
                              size_t *out_len)
{
    /* Each stored block: 1 (BFINAL/BTYPE) + 2 (LEN) + 2 (NLEN) + data */
    const size_t BLOCK_MAX = 65535;
    size_t num_blocks = (raw_len + BLOCK_MAX - 1) / BLOCK_MAX;
    if (num_blocks == 0)
        num_blocks = 1;

    size_t idat_len = 2 /* zlib hdr */ + num_blocks * 5 + raw_len + 4 /* adler */;
    uint8_t *buf = (uint8_t *)malloc(idat_len);
    if (!buf)
        return NULL;

    size_t pos = 0;
    /* zlib header: CMF=0x78, FLG=0x01 (no dict, level 0, checksum) */
    buf[pos++] = 0x78;
    buf[pos++] = 0x01;

    size_t src_pos = 0;
    for (size_t b = 0; b < num_blocks; b++)
    {
        size_t chunk = raw_len - src_pos;
        if (chunk > BLOCK_MAX)
            chunk = BLOCK_MAX;
        int last = (b == num_blocks - 1) ? 1 : 0;
        uint16_t len = (uint16_t)chunk;
        uint16_t nlen = (uint16_t)(~len);
        buf[pos++] = (uint8_t)last; /* BFINAL | (BTYPE=00 << 1) */
        buf[pos++] = (uint8_t)(len & 0xFF);
        buf[pos++] = (uint8_t)(len >> 8);
        buf[pos++] = (uint8_t)(nlen & 0xFF);
        buf[pos++] = (uint8_t)(nlen >> 8);
        memcpy(buf + pos, raw + src_pos, chunk);
        pos += chunk;
        src_pos += chunk;
    }
    /* Adler-32 of the uncompressed data (big-endian) */
    uint32_t adler = er__adler32(raw, raw_len);
    buf[pos++] = (uint8_t)(adler >> 24);
    buf[pos++] = (uint8_t)(adler >> 16);
    buf[pos++] = (uint8_t)(adler >> 8);
    buf[pos++] = (uint8_t)(adler);

    *out_len = pos;
    return buf;
}

/* ── Public API ───────────────────────────────────────────────────── */
int er_png_write_rgba(const char *path,
                      const uint8_t *pixels,
                      int width, int height, int stride_bytes)
{
    if (!path || !pixels || width <= 0 || height <= 0)
        return -1;

    /* Build filtered data: each row prefixed with filter byte 0 (None) */
    size_t row_bytes = (size_t)width * 4;
    size_t raw_len = (size_t)height * (1 + row_bytes);
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw)
        return -1;

    for (int y = 0; y < height; y++)
    {
        raw[y * (1 + row_bytes)] = 0; /* filter byte = None */
        memcpy(raw + y * (1 + row_bytes) + 1,
               pixels + y * stride_bytes,
               row_bytes);
    }

    /* Build IDAT compressed payload */
    size_t idat_len;
    uint8_t *idat = er__make_idat(raw, raw_len, &idat_len);
    free(raw);
    if (!idat)
        return -1;

    FILE *f = fopen(path, "wb");
    if (!f)
    {
        free(idat);
        return -1;
    }

    /* PNG signature */
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);

    /* IHDR */
    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(width >> 24);
    ihdr[1] = (uint8_t)(width >> 16);
    ihdr[2] = (uint8_t)(width >> 8);
    ihdr[3] = (uint8_t)(width);
    ihdr[4] = (uint8_t)(height >> 24);
    ihdr[5] = (uint8_t)(height >> 16);
    ihdr[6] = (uint8_t)(height >> 8);
    ihdr[7] = (uint8_t)(height);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* colour type: RGBA */
    ihdr[10] = 0; /* compression: deflate */
    ihdr[11] = 0; /* filter: adaptive */
    ihdr[12] = 0; /* interlace: none */
    er__chunk(f, "IHDR", ihdr, 13);

    /* IDAT */
    er__chunk(f, "IDAT", idat, idat_len);
    free(idat);

    /* IEND */
    er__chunk(f, "IEND", NULL, 0);

    fclose(f);
    return 0;
}

#endif /* ER_PNG_WRITE_IMPLEMENTATION */
#endif /* ER_PNG_WRITE_H */
