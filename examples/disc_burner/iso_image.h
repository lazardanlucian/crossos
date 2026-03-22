#ifndef DISC_BURNER_ISO_IMAGE_H
#define DISC_BURNER_ISO_IMAGE_H

#include <crossos/crossos.h>

#include <stddef.h>
#include <stdint.h>

crossos_result_t disc_burner_create_iso_image(const char *const *paths,
                                              size_t path_count,
                                              char *out_iso_path,
                                              size_t out_iso_path_size,
                                              uint64_t *out_iso_size);

void disc_burner_delete_temp_image(const char *path);

#endif