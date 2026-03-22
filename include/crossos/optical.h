/**
 * crossos/optical.h  -  Optical disc (CD/DVD) helper APIs.
 *
 * This module provides a cross-platform abstraction for detecting optical
 * drives and tracking burn progress. Physical burning is platform/tooling
 * dependent; CrossOS currently exposes a simulated burn pipeline so apps can
 * build and test UX consistently across desktop and Android.
 */

#ifndef CROSSOS_OPTICAL_H
#define CROSSOS_OPTICAL_H

#include "types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CROSSOS_OPTICAL_DEVICE_ID_MAX    64
#define CROSSOS_OPTICAL_DEVICE_LABEL_MAX 128
#define CROSSOS_OPTICAL_MESSAGE_MAX      128

typedef struct crossos_optical_device {
    char id[CROSSOS_OPTICAL_DEVICE_ID_MAX];
    char label[CROSSOS_OPTICAL_DEVICE_LABEL_MAX];
    int is_usb;
    int can_read;
    int can_write;
    int has_media;
    uint64_t media_capacity_bytes;
    uint64_t media_free_bytes;
} crossos_optical_device_t;

typedef enum crossos_optical_burn_state {
    CROSSOS_OPTICAL_BURN_IDLE = 0,
    CROSSOS_OPTICAL_BURN_PREPARING,
    CROSSOS_OPTICAL_BURN_BURNING,
    CROSSOS_OPTICAL_BURN_FINALIZING,
    CROSSOS_OPTICAL_BURN_DONE,
    CROSSOS_OPTICAL_BURN_FAILED,
    CROSSOS_OPTICAL_BURN_CANCELED,
} crossos_optical_burn_state_t;

typedef struct crossos_optical_burn_progress {
    crossos_optical_burn_state_t state;
    float percent;
    float speed_mib_s;
    uint64_t bytes_written;
    uint64_t total_bytes;
    char message[CROSSOS_OPTICAL_MESSAGE_MAX];
} crossos_optical_burn_progress_t;

typedef struct crossos_optical_burn_job crossos_optical_burn_job_t;

typedef struct crossos_optical_backend {
    crossos_result_t (*list_devices)(crossos_optical_device_t *devices,
                                     size_t max_devices,
                                     size_t *out_count,
                                     void *user_data);

    crossos_result_t (*burn_start)(const char *const *paths,
                                   size_t path_count,
                                   const char *target_device_id,
                                   void **out_backend_job,
                                   void *user_data);

    crossos_result_t (*burn_poll)(void *backend_job,
                                  crossos_optical_burn_progress_t *out_progress,
                                  void *user_data);

    crossos_result_t (*burn_cancel)(void *backend_job,
                                    void *user_data);

    void (*burn_free)(void *backend_job,
                      void *user_data);
} crossos_optical_backend_t;

/**
 * Register an optional platform/app-provided optical backend.
 *
 * This is mainly useful on Android where physical optical writing often
 * requires app-specific USB host logic or an external service.
 */
void crossos_optical_set_backend(const crossos_optical_backend_t *backend,
                                 void *user_data);

crossos_result_t crossos_optical_list_devices(crossos_optical_device_t *devices,
                                              size_t max_devices,
                                              size_t *out_count);

crossos_result_t crossos_optical_burn_start_simulated(const char *const *paths,
                                                      size_t path_count,
                                                      const char *target_device_id,
                                                      crossos_optical_burn_job_t **out_job);

/**
 * Unified burn start API:
 *  - uses registered backend when available;
 *  - falls back to simulated burn flow otherwise.
 */
crossos_result_t crossos_optical_burn_start(const char *const *paths,
                                            size_t path_count,
                                            const char *target_device_id,
                                            crossos_optical_burn_job_t **out_job);

crossos_result_t crossos_optical_burn_poll(crossos_optical_burn_job_t *job,
                                           crossos_optical_burn_progress_t *out_progress);

crossos_result_t crossos_optical_burn_cancel(crossos_optical_burn_job_t *job);

void crossos_optical_burn_free(crossos_optical_burn_job_t *job);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_OPTICAL_H */
