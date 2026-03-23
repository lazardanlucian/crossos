/*
 * src/core/camera.c  –  Camera subsystem core.
 *
 * This file provides the platform-independent glue and dispatches to the
 * correct backend selected at compile time:
 *
 *   Windows  → camera_win32.c   (Media Foundation)
 *   Linux    → camera_linux.c   (V4L2)
 *   Android  → camera_android.c (Camera2 JNI)
 *   Other    → stub (CROSSOS_ERR_UNSUPPORT)
 */

#include <crossos/camera.h>

#include <string.h>
#include <stdlib.h>

/* ── Platform backend declarations ─────────────────────────────────── */

#if defined(_WIN32)

crossos_result_t camera_platform_init(void);
void             camera_platform_shutdown(void);
int              camera_platform_enumerate(crossos_camera_info_t *out, int max);
crossos_result_t camera_platform_open(int idx, crossos_camera_t **out);
void             camera_platform_close(crossos_camera_t *cam);
crossos_result_t camera_platform_start(crossos_camera_t *cam);
void             camera_platform_stop(crossos_camera_t *cam);
crossos_result_t camera_platform_capture_frame(crossos_camera_t *cam,
                                               crossos_camera_frame_t *out);
void             camera_platform_release_frame(crossos_camera_t *cam,
                                               crossos_camera_frame_t *frame);
crossos_result_t camera_platform_get_info(const crossos_camera_t *cam,
                                          crossos_camera_info_t *out);

#elif defined(__ANDROID__)

crossos_result_t camera_platform_init(void);
void             camera_platform_shutdown(void);
int              camera_platform_enumerate(crossos_camera_info_t *out, int max);
crossos_result_t camera_platform_open(int idx, crossos_camera_t **out);
void             camera_platform_close(crossos_camera_t *cam);
crossos_result_t camera_platform_start(crossos_camera_t *cam);
void             camera_platform_stop(crossos_camera_t *cam);
crossos_result_t camera_platform_capture_frame(crossos_camera_t *cam,
                                               crossos_camera_frame_t *out);
void             camera_platform_release_frame(crossos_camera_t *cam,
                                               crossos_camera_frame_t *frame);
crossos_result_t camera_platform_get_info(const crossos_camera_t *cam,
                                          crossos_camera_info_t *out);

#elif defined(__linux__)

crossos_result_t camera_platform_init(void);
void             camera_platform_shutdown(void);
int              camera_platform_enumerate(crossos_camera_info_t *out, int max);
crossos_result_t camera_platform_open(int idx, crossos_camera_t **out);
void             camera_platform_close(crossos_camera_t *cam);
crossos_result_t camera_platform_start(crossos_camera_t *cam);
void             camera_platform_stop(crossos_camera_t *cam);
crossos_result_t camera_platform_capture_frame(crossos_camera_t *cam,
                                               crossos_camera_frame_t *out);
void             camera_platform_release_frame(crossos_camera_t *cam,
                                               crossos_camera_frame_t *frame);
crossos_result_t camera_platform_get_info(const crossos_camera_t *cam,
                                          crossos_camera_info_t *out);

#else /* Unsupported platform – stub everything out */

static crossos_result_t camera_platform_init(void) {
    return CROSSOS_ERR_UNSUPPORT;
}
static void camera_platform_shutdown(void) {}
static int camera_platform_enumerate(crossos_camera_info_t *out, int max) {
    (void)out; (void)max; return 0;
}
static crossos_result_t camera_platform_open(int idx, crossos_camera_t **out) {
    (void)idx; (void)out; return CROSSOS_ERR_UNSUPPORT;
}
static void camera_platform_close(crossos_camera_t *cam) { (void)cam; }
static crossos_result_t camera_platform_start(crossos_camera_t *cam) {
    (void)cam; return CROSSOS_ERR_UNSUPPORT;
}
static void camera_platform_stop(crossos_camera_t *cam) { (void)cam; }
static crossos_result_t camera_platform_capture_frame(crossos_camera_t *cam,
                                                       crossos_camera_frame_t *out) {
    (void)cam; (void)out; return CROSSOS_ERR_UNSUPPORT;
}
static void camera_platform_release_frame(crossos_camera_t *cam,
                                           crossos_camera_frame_t *frame) {
    (void)cam; (void)frame;
}
static crossos_result_t camera_platform_get_info(const crossos_camera_t *cam,
                                                  crossos_camera_info_t *out) {
    (void)cam; (void)out; return CROSSOS_ERR_UNSUPPORT;
}

#endif /* platform selection */

/* ── Public API ──────────────────────────────────────────────────────── */

crossos_result_t crossos_camera_init(void)
{
    return camera_platform_init();
}

void crossos_camera_shutdown(void)
{
    camera_platform_shutdown();
}

int crossos_camera_enumerate(crossos_camera_info_t *out_infos, int max_count)
{
    if (!out_infos || max_count <= 0) return 0;
    if (max_count > CROSSOS_CAMERA_MAX_DEVICES)
        max_count = CROSSOS_CAMERA_MAX_DEVICES;
    return camera_platform_enumerate(out_infos, max_count);
}

crossos_result_t crossos_camera_open(int device_index, crossos_camera_t **out_cam)
{
    if (!out_cam) return CROSSOS_ERR_PARAM;
    if (device_index < 0) return CROSSOS_ERR_PARAM;
    return camera_platform_open(device_index, out_cam);
}

void crossos_camera_close(crossos_camera_t *cam)
{
    if (!cam) return;
    camera_platform_close(cam);
}

crossos_result_t crossos_camera_start(crossos_camera_t *cam)
{
    if (!cam) return CROSSOS_ERR_PARAM;
    return camera_platform_start(cam);
}

void crossos_camera_stop(crossos_camera_t *cam)
{
    if (!cam) return;
    camera_platform_stop(cam);
}

crossos_result_t crossos_camera_capture_frame(crossos_camera_t       *cam,
                                              crossos_camera_frame_t *out_frame)
{
    if (!cam || !out_frame) return CROSSOS_ERR_PARAM;
    return camera_platform_capture_frame(cam, out_frame);
}

void crossos_camera_release_frame(crossos_camera_t       *cam,
                                  crossos_camera_frame_t *frame)
{
    if (!cam || !frame) return;
    camera_platform_release_frame(cam, frame);
}

crossos_result_t crossos_camera_get_info(const crossos_camera_t *cam,
                                         crossos_camera_info_t  *out_info)
{
    if (!cam || !out_info) return CROSSOS_ERR_PARAM;
    return camera_platform_get_info(cam, out_info);
}
