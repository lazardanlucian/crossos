/**
 * crossos/dialog.h  -  Native dialog helpers (file picker, etc.).
 */

#ifndef CROSSOS_DIALOG_H
#define CROSSOS_DIALOG_H

#include "types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct crossos_dialog_file_list {
    char   **items;
    size_t   count;
} crossos_dialog_file_list_t;

crossos_result_t crossos_dialog_pick_files(const char *title,
                                           int allow_multiple,
                                           crossos_dialog_file_list_t *out_files);

void crossos_dialog_file_list_free(crossos_dialog_file_list_t *files);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_DIALOG_H */
