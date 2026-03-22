/**
 * crossos/file.h  -  File I/O helpers.
 *
 * This module wraps common file operations with CrossOS-style error codes
 * and diagnostics so apps can use one API across platforms.
 */

#ifndef CROSSOS_FILE_H
#define CROSSOS_FILE_H

#include "types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque file handle. */
typedef struct crossos_file crossos_file_t;

/** File open mode bitmask. */
typedef enum crossos_file_mode {
    CROSSOS_FILE_READ   = 0x01,
    CROSSOS_FILE_WRITE  = 0x02,
    CROSSOS_FILE_APPEND = 0x04,
    CROSSOS_FILE_TRUNC  = 0x08,
    CROSSOS_FILE_BINARY = 0x10,
} crossos_file_mode_t;

/** Seek origin for crossos_file_seek. */
typedef enum crossos_seek_origin {
    CROSSOS_SEEK_SET = 0,
    CROSSOS_SEEK_CUR = 1,
    CROSSOS_SEEK_END = 2,
} crossos_seek_origin_t;

crossos_result_t crossos_file_open(const char *path,
                                   uint32_t mode,
                                   crossos_file_t **out_file);

void crossos_file_close(crossos_file_t *file);

crossos_result_t crossos_file_read(crossos_file_t *file,
                                   void *buffer,
                                   size_t buffer_size,
                                   size_t *out_bytes_read);

crossos_result_t crossos_file_write(crossos_file_t *file,
                                    const void *data,
                                    size_t data_size,
                                    size_t *out_bytes_written);

crossos_result_t crossos_file_seek(crossos_file_t *file,
                                   int64_t offset,
                                   crossos_seek_origin_t origin);

crossos_result_t crossos_file_tell(crossos_file_t *file,
                                   int64_t *out_offset);

crossos_result_t crossos_file_size(crossos_file_t *file,
                                   uint64_t *out_size);

/**
 * Convenience helper that loads an entire file into memory.
 * The returned pointer must be released with crossos_file_free().
 */
crossos_result_t crossos_file_read_all(const char *path,
                                       void **out_data,
                                       size_t *out_size);

void crossos_file_free(void *data);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_FILE_H */
