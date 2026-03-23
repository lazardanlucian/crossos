/**
 * crossos/camera.h  –  Camera capture helpers.
 *
 * Provides camera device enumeration, streaming capture, and per-frame
 * access across Windows (Media Foundation), Linux (V4L2), and Android
 * (Camera2 JNI).
 *
 * Each captured frame is delivered as a RGBA8888 pixel buffer that can be
 * directly blitted into a CrossOS framebuffer with crossos_image_blit_scaled().
 *
 * Quick-start:
 *
 *   crossos_camera_init();
 *
 *   crossos_camera_info_t infos[8];
 *   int count = crossos_camera_enumerate(infos, 8);
 *   if (count == 0) { ... }
 *
 *   crossos_camera_t *cam = NULL;
 *   crossos_camera_open(0, &cam);
 *   crossos_camera_start(cam);
 *
 *   crossos_camera_frame_t frame = {0};
 *   if (crossos_camera_capture_frame(cam, &frame) == CROSSOS_OK) {
 *       // frame.pixels is RGBA8888, frame.width x frame.height
 *       crossos_camera_release_frame(cam, &frame);
 *   }
 *
 *   crossos_camera_stop(cam);
 *   crossos_camera_close(cam);
 *   crossos_camera_shutdown();
 */

#ifndef CROSSOS_CAMERA_H
#define CROSSOS_CAMERA_H

#include "types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Camera device info ──────────────────────────────────────────────── */

/** Maximum length of a camera device name string (including NUL). */
#define CROSSOS_CAMERA_NAME_MAX 128

/** Maximum length of a camera device path/identifier (including NUL). */
#define CROSSOS_CAMERA_PATH_MAX 256

/** Maximum number of cameras that can be enumerated in one call. */
#define CROSSOS_CAMERA_MAX_DEVICES 16

/**
 * Describes a discovered camera device.
 *
 * Fields are filled in by crossos_camera_enumerate().  Both @p name and
 * @p device_path may be empty strings if the platform does not expose them.
 */
typedef struct crossos_camera_info {
    int  index;                         /**< Ordinal (0-based) device index  */
    char name[CROSSOS_CAMERA_NAME_MAX]; /**< Human-readable device name      */
    char device_path[CROSSOS_CAMERA_PATH_MAX]; /**< Platform device path     */
    int  is_default;                    /**< Non-zero for the default camera  */
} crossos_camera_info_t;

/* ── Captured frame ──────────────────────────────────────────────────── */

/**
 * A single captured video frame decoded to RGBA8888.
 *
 * pixel[y * stride + x * 4 + 0] = R
 * pixel[y * stride + x * 4 + 1] = G
 * pixel[y * stride + x * 4 + 2] = B
 * pixel[y * stride + x * 4 + 3] = A (always 255)
 *
 * The pixel buffer is owned by the camera subsystem until
 * crossos_camera_release_frame() is called.  Do not free or store the
 * pointer beyond that point.
 */
typedef struct crossos_camera_frame {
    unsigned char *pixels;      /**< RGBA8888 pixel data                     */
    int            width;       /**< Frame width in pixels                   */
    int            height;      /**< Frame height in pixels                  */
    int            stride;      /**< Row stride in bytes (>= width * 4)      */
    unsigned long  timestamp_ms;/**< Capture timestamp in milliseconds       */
    void          *platform_data;/**< Reserved for internal platform use     */
} crossos_camera_frame_t;

/* ── Opaque camera handle ────────────────────────────────────────────── */

/** Opaque handle representing an open camera device. */
typedef struct crossos_camera crossos_camera_t;

/* ── Subsystem lifecycle ─────────────────────────────────────────────── */

/**
 * Initialise the camera subsystem.
 *
 * Must be called once before any other crossos_camera_* function.
 * Safe to call multiple times; subsequent calls are no-ops.
 *
 * @return CROSSOS_OK on success, CROSSOS_ERR_CAMERA if the platform
 *         camera layer cannot be initialised, or CROSSOS_ERR_UNSUPPORT
 *         on unsupported platforms.
 */
crossos_result_t crossos_camera_init(void);

/**
 * Shut down the camera subsystem and release all resources.
 *
 * Stops any active capture and closes all open camera handles.
 * After this call crossos_camera_init() must be invoked again before
 * using any other camera function.
 */
void crossos_camera_shutdown(void);

/* ── Device enumeration ──────────────────────────────────────────────── */

/**
 * Discover available camera devices.
 *
 * Fills @p out_infos with up to @p max_count entries.
 *
 * @param out_infos  Array with room for at least @p max_count entries.
 * @param max_count  Capacity of @p out_infos (capped at
 *                   CROSSOS_CAMERA_MAX_DEVICES).
 * @return           Number of cameras found (may be 0).
 */
int crossos_camera_enumerate(crossos_camera_info_t *out_infos, int max_count);

/* ── Camera lifecycle ────────────────────────────────────────────────── */

/**
 * Open a camera device by its enumeration index.
 *
 * The camera is opened in an idle state; call crossos_camera_start() to
 * begin frame capture.
 *
 * @param device_index  0-based index as reported by crossos_camera_enumerate().
 *                      Pass 0 to open the default camera.
 * @param out_cam       Receives a new camera handle on success.  The caller
 *                      must call crossos_camera_close() when done.
 * @return              CROSSOS_OK on success; CROSSOS_ERR_CAMERA if the
 *                      device cannot be opened; CROSSOS_ERR_PARAM for an
 *                      invalid index.
 */
crossos_result_t crossos_camera_open(int device_index, crossos_camera_t **out_cam);

/**
 * Close an open camera handle and release its resources.
 *
 * Automatically stops capture if it is still running.
 * Safe to call with NULL.
 */
void crossos_camera_close(crossos_camera_t *cam);

/* ── Capture control ─────────────────────────────────────────────────── */

/**
 * Start frame capture on an open camera.
 *
 * After a successful call, crossos_camera_capture_frame() may be used to
 * retrieve frames.  The preferred resolution and format are selected
 * automatically (typically 640×480 YUYV or equivalent, converted to
 * RGBA8888 by the platform backend).
 *
 * @param cam  Camera handle returned by crossos_camera_open().
 * @return     CROSSOS_OK on success; CROSSOS_ERR_CAMERA on failure.
 */
crossos_result_t crossos_camera_start(crossos_camera_t *cam);

/**
 * Stop frame capture.
 *
 * Safe to call even if capture was never started or has already been stopped.
 *
 * @param cam  Camera handle returned by crossos_camera_open().
 */
void crossos_camera_stop(crossos_camera_t *cam);

/* ── Frame capture ───────────────────────────────────────────────────── */

/**
 * Capture the next available frame (blocking).
 *
 * Blocks until a new frame arrives from the device.  The frame's pixel
 * buffer is valid until crossos_camera_release_frame() is called.
 *
 * crossos_camera_start() must have been called successfully beforehand.
 *
 * @param cam        Open camera handle.
 * @param out_frame  Receives the captured frame on success.  Fields are
 *                   only valid until crossos_camera_release_frame() is
 *                   called.
 * @return           CROSSOS_OK on success; CROSSOS_ERR_CAMERA on capture
 *                   error or if the stream is not running.
 */
crossos_result_t crossos_camera_capture_frame(crossos_camera_t    *cam,
                                              crossos_camera_frame_t *out_frame);

/**
 * Release resources held by a captured frame.
 *
 * Must be called for every frame returned by crossos_camera_capture_frame()
 * before capturing the next one.  After this call @p frame->pixels is
 * no longer valid.
 *
 * @param cam    Camera handle that produced the frame.
 * @param frame  Frame to release.
 */
void crossos_camera_release_frame(crossos_camera_t       *cam,
                                  crossos_camera_frame_t *frame);

/* ── Metadata ────────────────────────────────────────────────────────── */

/**
 * Retrieve device information for an open camera.
 *
 * @param cam       Open camera handle.
 * @param out_info  Receives a copy of the device info on success.
 * @return          CROSSOS_OK on success; CROSSOS_ERR_PARAM if either
 *                  pointer is NULL.
 */
crossos_result_t crossos_camera_get_info(const crossos_camera_t *cam,
                                         crossos_camera_info_t  *out_info);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_CAMERA_H */
