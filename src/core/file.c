#include <crossos/file.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void crossos__set_error(const char *fmt, ...);

struct crossos_file {
    FILE *fp;
};

static const char *crossos__mode_to_string(uint32_t mode)
{
    if ((mode & CROSSOS_FILE_APPEND) != 0) {
        return (mode & CROSSOS_FILE_BINARY) ? "ab+" : "a+";
    }

    if ((mode & CROSSOS_FILE_WRITE) != 0 && (mode & CROSSOS_FILE_READ) != 0) {
        if ((mode & CROSSOS_FILE_TRUNC) != 0) {
            return (mode & CROSSOS_FILE_BINARY) ? "wb+" : "w+";
        }
        return (mode & CROSSOS_FILE_BINARY) ? "rb+" : "r+";
    }

    if ((mode & CROSSOS_FILE_WRITE) != 0) {
        return (mode & CROSSOS_FILE_BINARY) ? "wb" : "w";
    }

    if ((mode & CROSSOS_FILE_READ) != 0) {
        return (mode & CROSSOS_FILE_BINARY) ? "rb" : "r";
    }

    return NULL;
}

crossos_result_t crossos_file_open(const char *path,
                                   uint32_t mode,
                                   crossos_file_t **out_file)
{
    if (!path || !out_file) {
        crossos__set_error("file_open: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    const char *mode_str = crossos__mode_to_string(mode);
    if (!mode_str) {
        crossos__set_error("file_open: invalid mode bitmask");
        return CROSSOS_ERR_PARAM;
    }

    FILE *fp = fopen(path, mode_str);
    if (!fp) {
        crossos__set_error("file_open: failed for '%s': %s", path, strerror(errno));
        return CROSSOS_ERR_IO;
    }

    crossos_file_t *file = (crossos_file_t *)calloc(1, sizeof(*file));
    if (!file) {
        fclose(fp);
        crossos__set_error("file_open: out of memory");
        return CROSSOS_ERR_OOM;
    }

    file->fp = fp;
    *out_file = file;
    return CROSSOS_OK;
}

void crossos_file_close(crossos_file_t *file)
{
    if (!file) {
        return;
    }
    if (file->fp) {
        fclose(file->fp);
    }
    free(file);
}

crossos_result_t crossos_file_read(crossos_file_t *file,
                                   void *buffer,
                                   size_t buffer_size,
                                   size_t *out_bytes_read)
{
    if (!file || !file->fp || !buffer || !out_bytes_read) {
        crossos__set_error("file_read: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    size_t n = fread(buffer, 1, buffer_size, file->fp);
    if (n < buffer_size && ferror(file->fp)) {
        crossos__set_error("file_read: read failed: %s", strerror(errno));
        return CROSSOS_ERR_IO;
    }

    *out_bytes_read = n;
    return CROSSOS_OK;
}

crossos_result_t crossos_file_write(crossos_file_t *file,
                                    const void *data,
                                    size_t data_size,
                                    size_t *out_bytes_written)
{
    if (!file || !file->fp || !data || !out_bytes_written) {
        crossos__set_error("file_write: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    size_t n = fwrite(data, 1, data_size, file->fp);
    if (n != data_size) {
        crossos__set_error("file_write: write failed: %s", strerror(errno));
        return CROSSOS_ERR_IO;
    }

    *out_bytes_written = n;
    return CROSSOS_OK;
}

crossos_result_t crossos_file_seek(crossos_file_t *file,
                                   int64_t offset,
                                   crossos_seek_origin_t origin)
{
    if (!file || !file->fp) {
        crossos__set_error("file_seek: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    int whence = SEEK_SET;
    if (origin == CROSSOS_SEEK_CUR) {
        whence = SEEK_CUR;
    } else if (origin == CROSSOS_SEEK_END) {
        whence = SEEK_END;
    }

#if defined(_WIN32)
    if (_fseeki64(file->fp, offset, whence) != 0) {
#else
    if (fseek(file->fp, (long)offset, whence) != 0) {
#endif
        crossos__set_error("file_seek: failed: %s", strerror(errno));
        return CROSSOS_ERR_IO;
    }

    return CROSSOS_OK;
}

crossos_result_t crossos_file_tell(crossos_file_t *file,
                                   int64_t *out_offset)
{
    if (!file || !file->fp || !out_offset) {
        crossos__set_error("file_tell: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

#if defined(_WIN32)
    __int64 pos = _ftelli64(file->fp);
    if (pos < 0) {
#else
    long pos = ftell(file->fp);
    if (pos < 0) {
#endif
        crossos__set_error("file_tell: failed: %s", strerror(errno));
        return CROSSOS_ERR_IO;
    }

    *out_offset = (int64_t)pos;
    return CROSSOS_OK;
}

crossos_result_t crossos_file_size(crossos_file_t *file,
                                   uint64_t *out_size)
{
    if (!file || !file->fp || !out_size) {
        crossos__set_error("file_size: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    int64_t old_pos = 0;
    crossos_result_t rc = crossos_file_tell(file, &old_pos);
    if (rc != CROSSOS_OK) {
        return rc;
    }

    rc = crossos_file_seek(file, 0, CROSSOS_SEEK_END);
    if (rc != CROSSOS_OK) {
        return rc;
    }

    int64_t end_pos = 0;
    rc = crossos_file_tell(file, &end_pos);
    if (rc != CROSSOS_OK) {
        return rc;
    }

    rc = crossos_file_seek(file, old_pos, CROSSOS_SEEK_SET);
    if (rc != CROSSOS_OK) {
        return rc;
    }

    *out_size = (uint64_t)end_pos;
    return CROSSOS_OK;
}

crossos_result_t crossos_file_read_all(const char *path,
                                       void **out_data,
                                       size_t *out_size)
{
    if (!path || !out_data || !out_size) {
        crossos__set_error("file_read_all: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    *out_data = NULL;
    *out_size = 0;

    crossos_file_t *file = NULL;
    crossos_result_t rc = crossos_file_open(path, CROSSOS_FILE_READ | CROSSOS_FILE_BINARY, &file);
    if (rc != CROSSOS_OK) {
        return rc;
    }

    uint64_t size = 0;
    rc = crossos_file_size(file, &size);
    if (rc != CROSSOS_OK) {
        crossos_file_close(file);
        return rc;
    }

    if (size > SIZE_MAX) {
        crossos_file_close(file);
        crossos__set_error("file_read_all: file too large");
        return CROSSOS_ERR_IO;
    }

    void *data = malloc((size_t)size + 1);
    if (!data) {
        crossos_file_close(file);
        crossos__set_error("file_read_all: out of memory");
        return CROSSOS_ERR_OOM;
    }

    size_t bytes_read = 0;
    rc = crossos_file_read(file, data, (size_t)size, &bytes_read);
    crossos_file_close(file);

    if (rc != CROSSOS_OK) {
        free(data);
        return rc;
    }

    ((unsigned char *)data)[bytes_read] = 0;
    *out_data = data;
    *out_size = bytes_read;
    return CROSSOS_OK;
}

void crossos_file_free(void *data)
{
    free(data);
}
