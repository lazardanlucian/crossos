#include "iso_image.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#define DISC_BURNER_STAT _stat64
#define DISC_BURNER_UNLINK _unlink
#else
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#define DISC_BURNER_STAT stat
#define DISC_BURNER_UNLINK unlink
extern int mkstemp(char *template_name);
#endif

#define ISO_BLOCK_SIZE 2048U
#define ISO_SYSTEM_AREA_BLOCKS 16U
#define ISO_MAX_NAME_LEN 64U
#define ISO_VOLUME_ID "CROSSOS_DISC"

extern void crossos__set_error(const char *fmt, ...);

typedef struct iso_entry {
    int parent_index;
    int is_dir;
    char source_path[1024];
    char display_name[256];
    char iso_name[ISO_MAX_NAME_LEN];
    char record_name[ISO_MAX_NAME_LEN];
    uint64_t source_size;
    uint32_t extent_block;
    uint32_t data_size;
    uint32_t block_count;
    uint16_t path_table_index;
} iso_entry_t;

typedef struct iso_builder {
    iso_entry_t *entries;
    size_t entry_count;
    size_t entry_capacity;
} iso_builder_t;

typedef struct iso_sort_key {
    int entry_index;
    const char *name;
} iso_sort_key_t;

static int iso_sort_key_compare(const void *lhs, const void *rhs)
{
    const iso_sort_key_t *a = (const iso_sort_key_t *)lhs;
    const iso_sort_key_t *b = (const iso_sort_key_t *)rhs;
    return strcmp(a->name, b->name);
}

static int iso_name_exists(const iso_builder_t *builder,
                           int parent_index,
                           const char *candidate,
                           int is_dir)
{
    for (size_t index = 0; index < builder->entry_count; index++) {
        const iso_entry_t *entry = &builder->entries[index];
        const char *existing = is_dir ? entry->iso_name : entry->record_name;
        if (entry->parent_index == parent_index && strcmp(existing, candidate) == 0) {
            return 1;
        }
    }
    return 0;
}

static void iso_upper_sanitize(const char *src,
                               char *dst,
                               size_t dst_size,
                               size_t max_len)
{
    size_t out = 0;
    if (dst_size == 0) {
        return;
    }

    for (size_t i = 0; src[i] != '\0' && out + 1 < dst_size && out < max_len; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (isalnum(ch)) {
            dst[out++] = (char)toupper(ch);
        } else if (ch == '_' || ch == '-') {
            dst[out++] = '_';
        }
    }

    if (out == 0) {
        const char *fallback = "ITEM";
        while (*fallback != '\0' && out + 1 < dst_size && out < max_len) {
            dst[out++] = *fallback++;
        }
    }

    dst[out] = '\0';
}

static void iso_make_unique_name(const iso_builder_t *builder,
                                 int parent_index,
                                 const char *source_name,
                                 int is_dir,
                                 char *out_iso_name,
                                 size_t out_iso_name_size,
                                 char *out_record_name,
                                 size_t out_record_name_size)
{
    char base[16];
    char ext[8];
    char stem[16];
    const char *dot = strrchr(source_name, '.');

    memset(base, 0, sizeof(base));
    memset(ext, 0, sizeof(ext));
    memset(stem, 0, sizeof(stem));

    if (!is_dir && dot && dot != source_name) {
        char name_part[256];
        size_t name_len = (size_t)(dot - source_name);
        if (name_len >= sizeof(name_part)) {
            name_len = sizeof(name_part) - 1;
        }
        memcpy(name_part, source_name, name_len);
        name_part[name_len] = '\0';
        iso_upper_sanitize(name_part, base, sizeof(base), 8);
        iso_upper_sanitize(dot + 1, ext, sizeof(ext), 3);
    } else {
        iso_upper_sanitize(source_name, base, sizeof(base), is_dir ? 8 : 8);
    }

    snprintf(stem, sizeof(stem), "%s", base);

    for (int attempt = 0; attempt < 1000; attempt++) {
        char candidate_name[ISO_MAX_NAME_LEN];
        char candidate_record[ISO_MAX_NAME_LEN];

        if (attempt == 0) {
            snprintf(base, sizeof(base), "%s", stem);
        } else {
            char suffix[8];
            size_t suffix_len;
            size_t keep_len;

            snprintf(suffix, sizeof(suffix), "%d", attempt);
            suffix_len = strlen(suffix);
            keep_len = strlen(stem);
            if (keep_len + suffix_len > 8) {
                keep_len = 8 - suffix_len;
            }

            memset(base, 0, sizeof(base));
            memcpy(base, stem, keep_len);
            memcpy(base + keep_len, suffix, suffix_len + 1);
        }

        if (is_dir) {
            snprintf(candidate_name, sizeof(candidate_name), "%s", base);
            snprintf(candidate_record, sizeof(candidate_record), "%s", candidate_name);
        } else if (ext[0] != '\0') {
            snprintf(candidate_name, sizeof(candidate_name), "%s.%s", base, ext);
            snprintf(candidate_record, sizeof(candidate_record), "%s", candidate_name);
            strncat(candidate_record, ";1", sizeof(candidate_record) - strlen(candidate_record) - 1U);
        } else {
            snprintf(candidate_name, sizeof(candidate_name), "%s", base);
            snprintf(candidate_record, sizeof(candidate_record), "%s", candidate_name);
            strncat(candidate_record, ";1", sizeof(candidate_record) - strlen(candidate_record) - 1U);
        }

        if (!iso_name_exists(builder, parent_index, is_dir ? candidate_name : candidate_record, is_dir)) {
            snprintf(out_iso_name, out_iso_name_size, "%s", candidate_name);
            snprintf(out_record_name, out_record_name_size, "%s", candidate_record);
            return;
        }
    }

    snprintf(out_iso_name, out_iso_name_size, "%s", is_dir ? "DIR" : "FILE.BIN");
    snprintf(out_record_name, out_record_name_size, "%s", is_dir ? "DIR" : "FILE.BIN;1");
}

static crossos_result_t iso_builder_add_entry(iso_builder_t *builder,
                                              int parent_index,
                                              const char *source_path,
                                              const char *display_name,
                                              int is_dir,
                                              uint64_t source_size,
                                              int *out_entry_index)
{
    if (builder->entry_count == builder->entry_capacity) {
        size_t next_capacity = builder->entry_capacity == 0 ? 64 : builder->entry_capacity * 2;
        iso_entry_t *next_entries = (iso_entry_t *)realloc(builder->entries, next_capacity * sizeof(*next_entries));
        if (!next_entries) {
            crossos__set_error("iso_builder: out of memory");
            return CROSSOS_ERR_OOM;
        }
        builder->entries = next_entries;
        builder->entry_capacity = next_capacity;
    }

    iso_entry_t *entry = &builder->entries[builder->entry_count];
    memset(entry, 0, sizeof(*entry));
    entry->parent_index = parent_index;
    entry->is_dir = is_dir;
    entry->source_size = source_size;
    snprintf(entry->source_path, sizeof(entry->source_path), "%s", source_path ? source_path : "");
    snprintf(entry->display_name, sizeof(entry->display_name), "%s", display_name ? display_name : "");

    if (parent_index >= 0) {
        iso_make_unique_name(builder,
                             parent_index,
                             entry->display_name,
                             is_dir,
                             entry->iso_name,
                             sizeof(entry->iso_name),
                             entry->record_name,
                             sizeof(entry->record_name));
    }

    *out_entry_index = (int)builder->entry_count;
    builder->entry_count++;
    return CROSSOS_OK;
}

static const char *iso_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
#if defined(_WIN32)
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) {
        slash = backslash;
    }
#endif
    return slash ? slash + 1 : path;
}

static int iso_is_dir_mode(uint64_t mode)
{
#if defined(_WIN32)
    return (mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(mode) ? 1 : 0;
#endif
}

static crossos_result_t iso_collect_path(iso_builder_t *builder,
                                         int parent_index,
                                         const char *path);

static crossos_result_t iso_collect_directory(iso_builder_t *builder,
                                              int parent_index,
                                              const char *path,
                                              int *out_entry_index)
{
    int entry_index = -1;
    crossos_result_t rc = iso_builder_add_entry(builder,
                                                parent_index,
                                                path,
                                                iso_basename(path),
                                                1,
                                                0,
                                                &entry_index);
    if (rc != CROSSOS_OK) {
        return rc;
    }

#if defined(_WIN32)
    WIN32_FIND_DATAA find_data;
    char pattern[1060];
    HANDLE find_handle;

    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    find_handle = FindFirstFileA(pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE) {
        if (out_entry_index) {
            *out_entry_index = entry_index;
        }
        return CROSSOS_OK;
    }

    do {
        char child_path[1060];
        if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0) {
            continue;
        }
        snprintf(child_path, sizeof(child_path), "%s\\%s", path, find_data.cFileName);
        rc = iso_collect_path(builder, entry_index, child_path);
        if (rc != CROSSOS_OK) {
            FindClose(find_handle);
            return rc;
        }
    } while (FindNextFileA(find_handle, &find_data));

    FindClose(find_handle);
#else
    DIR *dir = opendir(path);
    if (!dir) {
        crossos__set_error("iso_builder: cannot open directory '%s': %s", path, strerror(errno));
        return CROSSOS_ERR_IO;
    }

    for (;;) {
        struct dirent *entry = readdir(dir);
        char child_path[1060];
        if (!entry) {
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name);
        rc = iso_collect_path(builder, entry_index, child_path);
        if (rc != CROSSOS_OK) {
            closedir(dir);
            return rc;
        }
    }

    closedir(dir);
#endif

    if (out_entry_index) {
        *out_entry_index = entry_index;
    }
    return CROSSOS_OK;
}

static crossos_result_t iso_collect_path(iso_builder_t *builder,
                                         int parent_index,
                                         const char *path)
{
    struct DISC_BURNER_STAT st;
    int unused = -1;

    if (DISC_BURNER_STAT(path, &st) != 0) {
        crossos__set_error("iso_builder: cannot stat '%s': %s", path, strerror(errno));
        return CROSSOS_ERR_IO;
    }

    if (iso_is_dir_mode((uint64_t)st.st_mode)) {
        return iso_collect_directory(builder, parent_index, path, &unused);
    }

    return iso_builder_add_entry(builder,
                                 parent_index,
                                 path,
                                 iso_basename(path),
                                 0,
                                 (uint64_t)st.st_size,
                                 &unused);
}

static void iso_builder_free(iso_builder_t *builder)
{
    free(builder->entries);
    builder->entries = NULL;
    builder->entry_count = 0;
    builder->entry_capacity = 0;
}

static uint32_t iso_dir_record_length(size_t name_len)
{
    return (uint32_t)(33U + name_len + ((name_len % 2U) == 0U ? 1U : 0U));
}

static uint32_t iso_path_table_record_length(size_t name_len)
{
    return (uint32_t)(8U + name_len + ((name_len % 2U) == 1U ? 1U : 0U));
}

static void iso_write_u16_both(unsigned char *dst, uint16_t value)
{
    dst[0] = (unsigned char)(value & 0xFF);
    dst[1] = (unsigned char)((value >> 8) & 0xFF);
    dst[2] = dst[1];
    dst[3] = dst[0];
}

static void iso_write_u32_both(unsigned char *dst, uint32_t value)
{
    dst[0] = (unsigned char)(value & 0xFF);
    dst[1] = (unsigned char)((value >> 8) & 0xFF);
    dst[2] = (unsigned char)((value >> 16) & 0xFF);
    dst[3] = (unsigned char)((value >> 24) & 0xFF);
    dst[4] = dst[3];
    dst[5] = dst[2];
    dst[6] = dst[1];
    dst[7] = dst[0];
}

static void iso_write_recording_time(unsigned char *dst, time_t value)
{
    struct tm tm_value;
#if defined(_WIN32)
    gmtime_s(&tm_value, &value);
#else
    {
        struct tm *tmp = gmtime(&value);
        if (tmp) {
            tm_value = *tmp;
        } else {
            memset(&tm_value, 0, sizeof(tm_value));
        }
    }
#endif
    dst[0] = (unsigned char)(tm_value.tm_year);
    dst[1] = (unsigned char)(tm_value.tm_mon + 1);
    dst[2] = (unsigned char)tm_value.tm_mday;
    dst[3] = (unsigned char)tm_value.tm_hour;
    dst[4] = (unsigned char)tm_value.tm_min;
    dst[5] = (unsigned char)tm_value.tm_sec;
    dst[6] = 0;
}

static void iso_write_volume_time(unsigned char *dst, time_t value)
{
    struct tm tm_value;
#if defined(_WIN32)
    gmtime_s(&tm_value, &value);
#else
    {
        struct tm *tmp = gmtime(&value);
        if (tmp) {
            tm_value = *tmp;
        } else {
            memset(&tm_value, 0, sizeof(tm_value));
        }
    }
#endif
    snprintf((char *)dst, 17, "%04u%02u%02u%02u%02u%02u00",
             (unsigned)(tm_value.tm_year + 1900),
             (unsigned)(tm_value.tm_mon + 1),
             (unsigned)tm_value.tm_mday,
             (unsigned)tm_value.tm_hour,
             (unsigned)tm_value.tm_min,
             (unsigned)tm_value.tm_sec);
    dst[16] = 0;
}

static void iso_write_directory_record(unsigned char *dst,
                                       uint32_t extent_block,
                                       uint32_t data_size,
                                       int is_dir,
                                       const unsigned char *identifier,
                                       size_t identifier_len,
                                       time_t timestamp)
{
    uint32_t record_len = iso_dir_record_length(identifier_len);
    memset(dst, 0, record_len);
    dst[0] = (unsigned char)record_len;
    dst[1] = 0;
    iso_write_u32_both(dst + 2, extent_block);
    iso_write_u32_both(dst + 10, data_size);
    iso_write_recording_time(dst + 18, timestamp);
    dst[25] = (unsigned char)(is_dir ? 0x02 : 0x00);
    dst[26] = 0;
    dst[27] = 0;
    iso_write_u16_both(dst + 28, 1);
    dst[32] = (unsigned char)identifier_len;
    memcpy(dst + 33, identifier, identifier_len);
}

static int iso_collect_children(const iso_builder_t *builder,
                                int parent_index,
                                int want_dirs,
                                iso_sort_key_t *out_keys,
                                size_t max_keys)
{
    size_t count = 0;
    for (size_t index = 0; index < builder->entry_count && count < max_keys; index++) {
        const iso_entry_t *entry = &builder->entries[index];
        if (entry->parent_index == parent_index && entry->is_dir == want_dirs) {
            out_keys[count].entry_index = (int)index;
            out_keys[count].name = want_dirs ? entry->iso_name : entry->record_name;
            count++;
        }
    }
    qsort(out_keys, count, sizeof(out_keys[0]), iso_sort_key_compare);
    return (int)count;
}

static crossos_result_t iso_compute_layout(iso_builder_t *builder,
                                           uint32_t *out_volume_blocks,
                                           uint32_t *out_path_table_size,
                                           uint32_t *out_le_path_table_block,
                                           uint32_t *out_be_path_table_block)
{
    int *dir_order = NULL;
    size_t dir_count = 0;
    size_t dir_capacity = builder->entry_count;
    uint32_t path_table_size = 0;
    uint32_t current_block;

    dir_order = (int *)calloc(dir_capacity, sizeof(*dir_order));
    if (!dir_order) {
        crossos__set_error("iso_builder: out of memory");
        return CROSSOS_ERR_OOM;
    }

    dir_order[dir_count++] = 0;
    builder->entries[0].path_table_index = 1;

    for (size_t scan = 0; scan < dir_count; scan++) {
        int parent_index = dir_order[scan];
        iso_sort_key_t child_keys[1024];
        int child_count = iso_collect_children(builder, parent_index, 1, child_keys, 1024);

        for (int child = 0; child < child_count; child++) {
            int entry_index = child_keys[child].entry_index;
            builder->entries[entry_index].path_table_index = (uint16_t)(dir_count + 1);
            dir_order[dir_count++] = entry_index;
        }
    }

    for (size_t dir_index = 0; dir_index < dir_count; dir_index++) {
        const iso_entry_t *entry = &builder->entries[dir_order[dir_index]];
        size_t identifier_len = dir_index == 0 ? 1U : strlen(entry->iso_name);
        path_table_size += iso_path_table_record_length(identifier_len);
    }

    *out_path_table_size = path_table_size;
    *out_le_path_table_block = ISO_SYSTEM_AREA_BLOCKS + 2U;
    *out_be_path_table_block = *out_le_path_table_block + (path_table_size + ISO_BLOCK_SIZE - 1U) / ISO_BLOCK_SIZE;
    current_block = *out_be_path_table_block + (path_table_size + ISO_BLOCK_SIZE - 1U) / ISO_BLOCK_SIZE;

    for (size_t dir_index = 0; dir_index < dir_count; dir_index++) {
        int entry_index = dir_order[dir_index];
        iso_entry_t *entry = &builder->entries[entry_index];
        uint32_t offset = 0;
        iso_sort_key_t dir_keys[1024];
        iso_sort_key_t file_keys[1024];
        int child_dir_count = iso_collect_children(builder, entry_index, 1, dir_keys, 1024);
        int child_file_count = iso_collect_children(builder, entry_index, 0, file_keys, 1024);

        offset += iso_dir_record_length(1U);
        if ((offset % ISO_BLOCK_SIZE) + iso_dir_record_length(1U) > ISO_BLOCK_SIZE) {
            offset += ISO_BLOCK_SIZE - (offset % ISO_BLOCK_SIZE);
        }
        offset += iso_dir_record_length(1U);

        for (int child = 0; child < child_dir_count; child++) {
            size_t name_len = strlen(builder->entries[dir_keys[child].entry_index].iso_name);
            uint32_t record_len = iso_dir_record_length(name_len);
            if ((offset % ISO_BLOCK_SIZE) + record_len > ISO_BLOCK_SIZE) {
                offset += ISO_BLOCK_SIZE - (offset % ISO_BLOCK_SIZE);
            }
            offset += record_len;
        }

        for (int child = 0; child < child_file_count; child++) {
            size_t name_len = strlen(builder->entries[file_keys[child].entry_index].record_name);
            uint32_t record_len = iso_dir_record_length(name_len);
            if ((offset % ISO_BLOCK_SIZE) + record_len > ISO_BLOCK_SIZE) {
                offset += ISO_BLOCK_SIZE - (offset % ISO_BLOCK_SIZE);
            }
            offset += record_len;
        }

        entry->data_size = offset;
        entry->block_count = (offset + ISO_BLOCK_SIZE - 1U) / ISO_BLOCK_SIZE;
        entry->extent_block = current_block;
        current_block += entry->block_count;
    }

    for (size_t index = 1; index < builder->entry_count; index++) {
        iso_entry_t *entry = &builder->entries[index];
        if (entry->is_dir) {
            continue;
        }
        entry->data_size = (uint32_t)entry->source_size;
        entry->block_count = (uint32_t)((entry->source_size + ISO_BLOCK_SIZE - 1U) / ISO_BLOCK_SIZE);
        entry->extent_block = current_block;
        current_block += entry->block_count;
    }

    *out_volume_blocks = current_block;
    free(dir_order);
    return CROSSOS_OK;
}

static crossos_result_t iso_seek_block(FILE *fp, uint32_t block)
{
    if (fseek(fp, (long)(block * ISO_BLOCK_SIZE), SEEK_SET) != 0) {
        crossos__set_error("iso_builder: seek failed: %s", strerror(errno));
        return CROSSOS_ERR_IO;
    }
    return CROSSOS_OK;
}

static crossos_result_t iso_write_path_tables(FILE *fp,
                                              const iso_builder_t *builder,
                                              uint32_t path_table_size,
                                              uint32_t le_block,
                                              uint32_t be_block)
{
    int *dir_order = (int *)calloc(builder->entry_count, sizeof(int));
    size_t dir_count = 0;
    unsigned char *le_buf = NULL;
    unsigned char *be_buf = NULL;
    uint32_t offset = 0;

    if (!dir_order) {
        crossos__set_error("iso_builder: out of memory");
        return CROSSOS_ERR_OOM;
    }

    dir_order[dir_count++] = 0;
    for (size_t scan = 0; scan < dir_count; scan++) {
        iso_sort_key_t child_keys[1024];
        int child_count = iso_collect_children(builder, dir_order[scan], 1, child_keys, 1024);
        for (int child = 0; child < child_count; child++) {
            dir_order[dir_count++] = child_keys[child].entry_index;
        }
    }

    le_buf = (unsigned char *)calloc((path_table_size + ISO_BLOCK_SIZE - 1U) / ISO_BLOCK_SIZE, ISO_BLOCK_SIZE);
    be_buf = (unsigned char *)calloc((path_table_size + ISO_BLOCK_SIZE - 1U) / ISO_BLOCK_SIZE, ISO_BLOCK_SIZE);
    if (!le_buf || !be_buf) {
        free(dir_order);
        free(le_buf);
        free(be_buf);
        crossos__set_error("iso_builder: out of memory");
        return CROSSOS_ERR_OOM;
    }

    for (size_t dir_index = 0; dir_index < dir_count; dir_index++) {
        const iso_entry_t *entry = &builder->entries[dir_order[dir_index]];
        const unsigned char *identifier = dir_index == 0
            ? (const unsigned char *)"\0"
            : (const unsigned char *)entry->iso_name;
        size_t identifier_len = dir_index == 0 ? 1U : strlen(entry->iso_name);
        uint32_t record_len = iso_path_table_record_length(identifier_len);
        uint16_t parent_index = dir_index == 0
            ? 1U
            : builder->entries[entry->parent_index].path_table_index;

        le_buf[offset + 0] = (unsigned char)identifier_len;
        le_buf[offset + 1] = 0;
        le_buf[offset + 2] = (unsigned char)(entry->extent_block & 0xFF);
        le_buf[offset + 3] = (unsigned char)((entry->extent_block >> 8) & 0xFF);
        le_buf[offset + 4] = (unsigned char)((entry->extent_block >> 16) & 0xFF);
        le_buf[offset + 5] = (unsigned char)((entry->extent_block >> 24) & 0xFF);
        le_buf[offset + 6] = (unsigned char)(parent_index & 0xFF);
        le_buf[offset + 7] = (unsigned char)((parent_index >> 8) & 0xFF);
        memcpy(le_buf + offset + 8, identifier, identifier_len);

        be_buf[offset + 0] = (unsigned char)identifier_len;
        be_buf[offset + 1] = 0;
        be_buf[offset + 2] = (unsigned char)((entry->extent_block >> 24) & 0xFF);
        be_buf[offset + 3] = (unsigned char)((entry->extent_block >> 16) & 0xFF);
        be_buf[offset + 4] = (unsigned char)((entry->extent_block >> 8) & 0xFF);
        be_buf[offset + 5] = (unsigned char)(entry->extent_block & 0xFF);
        be_buf[offset + 6] = (unsigned char)((parent_index >> 8) & 0xFF);
        be_buf[offset + 7] = (unsigned char)(parent_index & 0xFF);
        memcpy(be_buf + offset + 8, identifier, identifier_len);

        offset += record_len;
    }

    if (iso_seek_block(fp, le_block) != CROSSOS_OK) {
        free(dir_order);
        free(le_buf);
        free(be_buf);
        return CROSSOS_ERR_IO;
    }
    if (fwrite(le_buf, 1, ((path_table_size + ISO_BLOCK_SIZE - 1U) / ISO_BLOCK_SIZE) * ISO_BLOCK_SIZE, fp) == 0U) {
        free(dir_order);
        free(le_buf);
        free(be_buf);
        crossos__set_error("iso_builder: failed to write path table: %s", strerror(errno));
        return CROSSOS_ERR_IO;
    }

    if (iso_seek_block(fp, be_block) != CROSSOS_OK) {
        free(dir_order);
        free(le_buf);
        free(be_buf);
        return CROSSOS_ERR_IO;
    }
    if (fwrite(be_buf, 1, ((path_table_size + ISO_BLOCK_SIZE - 1U) / ISO_BLOCK_SIZE) * ISO_BLOCK_SIZE, fp) == 0U) {
        free(dir_order);
        free(le_buf);
        free(be_buf);
        crossos__set_error("iso_builder: failed to write path table: %s", strerror(errno));
        return CROSSOS_ERR_IO;
    }

    free(dir_order);
    free(le_buf);
    free(be_buf);
    return CROSSOS_OK;
}

static crossos_result_t iso_write_descriptors(FILE *fp,
                                              const iso_builder_t *builder,
                                              uint32_t volume_blocks,
                                              uint32_t path_table_size,
                                              uint32_t le_block,
                                              uint32_t be_block)
{
    unsigned char pvd[ISO_BLOCK_SIZE];
    unsigned char term[ISO_BLOCK_SIZE];
    time_t now = time(NULL);

    memset(pvd, 0, sizeof(pvd));
    memset(term, 0, sizeof(term));

    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    memcpy(pvd + 8, ISO_VOLUME_ID, sizeof(ISO_VOLUME_ID) - 1);
    iso_write_u32_both(pvd + 80, volume_blocks);
    iso_write_u16_both(pvd + 120, 1);
    iso_write_u16_both(pvd + 124, 1);
    iso_write_u16_both(pvd + 128, ISO_BLOCK_SIZE);
    iso_write_u32_both(pvd + 132, path_table_size);
    pvd[140] = (unsigned char)(le_block & 0xFF);
    pvd[141] = (unsigned char)((le_block >> 8) & 0xFF);
    pvd[142] = (unsigned char)((le_block >> 16) & 0xFF);
    pvd[143] = (unsigned char)((le_block >> 24) & 0xFF);
    pvd[148] = (unsigned char)((be_block >> 24) & 0xFF);
    pvd[149] = (unsigned char)((be_block >> 16) & 0xFF);
    pvd[150] = (unsigned char)((be_block >> 8) & 0xFF);
    pvd[151] = (unsigned char)(be_block & 0xFF);
    iso_write_directory_record(pvd + 156,
                               builder->entries[0].extent_block,
                               builder->entries[0].data_size,
                               1,
                               (const unsigned char *)"\0",
                               1,
                               now);
    iso_write_volume_time(pvd + 813, now);
    iso_write_volume_time(pvd + 830, now);
    iso_write_volume_time(pvd + 847, now);
    iso_write_volume_time(pvd + 864, 0);
    pvd[881] = 1;

    term[0] = 255;
    memcpy(term + 1, "CD001", 5);
    term[6] = 1;

    if (iso_seek_block(fp, ISO_SYSTEM_AREA_BLOCKS) != CROSSOS_OK) {
        return CROSSOS_ERR_IO;
    }
    if (fwrite(pvd, 1, sizeof(pvd), fp) != sizeof(pvd) ||
        fwrite(term, 1, sizeof(term), fp) != sizeof(term)) {
        crossos__set_error("iso_builder: failed to write volume descriptors: %s", strerror(errno));
        return CROSSOS_ERR_IO;
    }

    return CROSSOS_OK;
}

static crossos_result_t iso_write_directories(FILE *fp,
                                              const iso_builder_t *builder)
{
    time_t now = time(NULL);

    for (size_t index = 0; index < builder->entry_count; index++) {
        const iso_entry_t *entry = &builder->entries[index];
        unsigned char *buffer;
        uint32_t offset = 0;
        uint32_t alloc_size;
        iso_sort_key_t dir_keys[1024];
        iso_sort_key_t file_keys[1024];
        int child_dir_count;
        int child_file_count;

        if (!entry->is_dir) {
            continue;
        }

        alloc_size = entry->block_count * ISO_BLOCK_SIZE;
        buffer = (unsigned char *)calloc(1, alloc_size);
        if (!buffer) {
            crossos__set_error("iso_builder: out of memory");
            return CROSSOS_ERR_OOM;
        }

        iso_write_directory_record(buffer + offset,
                                   entry->extent_block,
                                   entry->data_size,
                                   1,
                                   (const unsigned char *)"\0",
                                   1,
                                   now);
        offset += iso_dir_record_length(1U);

        if ((offset % ISO_BLOCK_SIZE) + iso_dir_record_length(1U) > ISO_BLOCK_SIZE) {
            offset += ISO_BLOCK_SIZE - (offset % ISO_BLOCK_SIZE);
        }
        {
            const iso_entry_t *parent = index == 0 ? entry : &builder->entries[entry->parent_index];
            iso_write_directory_record(buffer + offset,
                                       parent->extent_block,
                                       parent->data_size,
                                       1,
                                       (const unsigned char *)"\1",
                                       1,
                                       now);
        }
        offset += iso_dir_record_length(1U);

        child_dir_count = iso_collect_children(builder, (int)index, 1, dir_keys, 1024);
        child_file_count = iso_collect_children(builder, (int)index, 0, file_keys, 1024);

        for (int child = 0; child < child_dir_count; child++) {
            const iso_entry_t *child_entry = &builder->entries[dir_keys[child].entry_index];
            uint32_t record_len = iso_dir_record_length(strlen(child_entry->iso_name));
            if ((offset % ISO_BLOCK_SIZE) + record_len > ISO_BLOCK_SIZE) {
                offset += ISO_BLOCK_SIZE - (offset % ISO_BLOCK_SIZE);
            }
            iso_write_directory_record(buffer + offset,
                                       child_entry->extent_block,
                                       child_entry->data_size,
                                       1,
                                       (const unsigned char *)child_entry->iso_name,
                                       strlen(child_entry->iso_name),
                                       now);
            offset += record_len;
        }

        for (int child = 0; child < child_file_count; child++) {
            const iso_entry_t *child_entry = &builder->entries[file_keys[child].entry_index];
            uint32_t record_len = iso_dir_record_length(strlen(child_entry->record_name));
            if ((offset % ISO_BLOCK_SIZE) + record_len > ISO_BLOCK_SIZE) {
                offset += ISO_BLOCK_SIZE - (offset % ISO_BLOCK_SIZE);
            }
            iso_write_directory_record(buffer + offset,
                                       child_entry->extent_block,
                                       child_entry->data_size,
                                       0,
                                       (const unsigned char *)child_entry->record_name,
                                       strlen(child_entry->record_name),
                                       now);
            offset += record_len;
        }

        if (iso_seek_block(fp, entry->extent_block) != CROSSOS_OK) {
            free(buffer);
            return CROSSOS_ERR_IO;
        }
        if (fwrite(buffer, 1, alloc_size, fp) != alloc_size) {
            free(buffer);
            crossos__set_error("iso_builder: failed to write directory records: %s", strerror(errno));
            return CROSSOS_ERR_IO;
        }
        free(buffer);
    }

    return CROSSOS_OK;
}

static crossos_result_t iso_write_files(FILE *fp,
                                        const iso_builder_t *builder)
{
    unsigned char zero_pad[ISO_BLOCK_SIZE];
    memset(zero_pad, 0, sizeof(zero_pad));

    for (size_t index = 0; index < builder->entry_count; index++) {
        const iso_entry_t *entry = &builder->entries[index];
        FILE *src;
        unsigned char buffer[8192];

        if (entry->is_dir) {
            continue;
        }

        src = fopen(entry->source_path, "rb");
        if (!src) {
            crossos__set_error("iso_builder: failed to open '%s': %s", entry->source_path, strerror(errno));
            return CROSSOS_ERR_IO;
        }

        if (iso_seek_block(fp, entry->extent_block) != CROSSOS_OK) {
            fclose(src);
            return CROSSOS_ERR_IO;
        }

        for (;;) {
            size_t bytes_read = fread(buffer, 1, sizeof(buffer), src);
            if (bytes_read > 0) {
                if (fwrite(buffer, 1, bytes_read, fp) != bytes_read) {
                    fclose(src);
                    crossos__set_error("iso_builder: failed to write file payload: %s", strerror(errno));
                    return CROSSOS_ERR_IO;
                }
            }
            if (bytes_read < sizeof(buffer)) {
                if (ferror(src)) {
                    fclose(src);
                    crossos__set_error("iso_builder: failed to read '%s': %s", entry->source_path, strerror(errno));
                    return CROSSOS_ERR_IO;
                }
                break;
            }
        }

        fclose(src);

        if ((entry->source_size % ISO_BLOCK_SIZE) != 0U) {
            size_t pad = (size_t)(ISO_BLOCK_SIZE - (entry->source_size % ISO_BLOCK_SIZE));
            if (fwrite(zero_pad, 1, pad, fp) != pad) {
                crossos__set_error("iso_builder: failed to pad file payload: %s", strerror(errno));
                return CROSSOS_ERR_IO;
            }
        }
    }

    return CROSSOS_OK;
}

static crossos_result_t iso_make_temp_path(char *out_path, size_t out_path_size)
{
#if defined(_WIN32)
    char temp_dir[MAX_PATH];
    char temp_name[MAX_PATH];
    char *dot;

    if (GetTempPathA((DWORD)sizeof(temp_dir), temp_dir) == 0) {
        crossos__set_error("iso_builder: GetTempPath failed");
        return CROSSOS_ERR_IO;
    }
    if (GetTempFileNameA(temp_dir, "cdb", 0, temp_name) == 0) {
        crossos__set_error("iso_builder: GetTempFileName failed");
        return CROSSOS_ERR_IO;
    }
    dot = strrchr(temp_name, '.');
    if (dot) {
        *dot = '\0';
    }
    snprintf(out_path, out_path_size, "%s.iso", temp_name);
    DISC_BURNER_UNLINK(temp_name);
#else
    char template_path[] = "/tmp/crossos-burn-XXXXXX";
    char final_path[sizeof(template_path) + 4];
    int fd;

    fd = mkstemp(template_path);
    if (fd < 0) {
        crossos__set_error("iso_builder: mkstemp failed: %s", strerror(errno));
        return CROSSOS_ERR_IO;
    }
    close(fd);
    snprintf(final_path, sizeof(final_path), "%s.iso", template_path);
    if (rename(template_path, final_path) != 0) {
        DISC_BURNER_UNLINK(template_path);
        crossos__set_error("iso_builder: rename failed: %s", strerror(errno));
        return CROSSOS_ERR_IO;
    }
    snprintf(out_path, out_path_size, "%s", final_path);
#endif
    return CROSSOS_OK;
}

crossos_result_t disc_burner_create_iso_image(const char *const *paths,
                                              size_t path_count,
                                              char *out_iso_path,
                                              size_t out_iso_path_size,
                                              uint64_t *out_iso_size)
{
    iso_builder_t builder;
    FILE *iso_file = NULL;
    uint32_t volume_blocks = 0;
    uint32_t path_table_size = 0;
    uint32_t le_block = 0;
    uint32_t be_block = 0;
    crossos_result_t rc;

    memset(&builder, 0, sizeof(builder));

    if (!paths || path_count == 0 || !out_iso_path || out_iso_path_size == 0 || !out_iso_size) {
        crossos__set_error("iso_builder: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    rc = iso_builder_add_entry(&builder, -1, "", "", 1, 0, &(int){0});
    if (rc != CROSSOS_OK) {
        iso_builder_free(&builder);
        return rc;
    }

    for (size_t index = 0; index < path_count; index++) {
        rc = iso_collect_path(&builder, 0, paths[index]);
        if (rc != CROSSOS_OK) {
            iso_builder_free(&builder);
            return rc;
        }
    }

    rc = iso_compute_layout(&builder, &volume_blocks, &path_table_size, &le_block, &be_block);
    if (rc != CROSSOS_OK) {
        iso_builder_free(&builder);
        return rc;
    }

    rc = iso_make_temp_path(out_iso_path, out_iso_path_size);
    if (rc != CROSSOS_OK) {
        iso_builder_free(&builder);
        return rc;
    }

    iso_file = fopen(out_iso_path, "wb+");
    if (!iso_file) {
        iso_builder_free(&builder);
        crossos__set_error("iso_builder: failed to create '%s': %s", out_iso_path, strerror(errno));
        return CROSSOS_ERR_IO;
    }

    rc = iso_write_descriptors(iso_file, &builder, volume_blocks, path_table_size, le_block, be_block);
    if (rc == CROSSOS_OK) {
        rc = iso_write_path_tables(iso_file, &builder, path_table_size, le_block, be_block);
    }
    if (rc == CROSSOS_OK) {
        rc = iso_write_directories(iso_file, &builder);
    }
    if (rc == CROSSOS_OK) {
        rc = iso_write_files(iso_file, &builder);
    }

    if (fflush(iso_file) != 0) {
        rc = CROSSOS_ERR_IO;
        crossos__set_error("iso_builder: flush failed: %s", strerror(errno));
    }

    fclose(iso_file);
    iso_builder_free(&builder);

    if (rc != CROSSOS_OK) {
        disc_burner_delete_temp_image(out_iso_path);
        out_iso_path[0] = '\0';
        return rc;
    }

    *out_iso_size = (uint64_t)volume_blocks * ISO_BLOCK_SIZE;
    return CROSSOS_OK;
}

void disc_burner_delete_temp_image(const char *path)
{
    if (!path || path[0] == '\0') {
        return;
    }
    DISC_BURNER_UNLINK(path);
}