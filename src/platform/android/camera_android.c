/*
 * src/platform/android/camera_android.c  –  Camera2 JNI camera backend.
 *
 * This file provides the Android camera backend using the NDK Camera2 API
 * (ACameraManager, ACameraDevice, ACaptureSession, AImageReader).
 *
 * The NDK Camera2 API requires API level 24+ and linking against
 * libcamera2ndk.so and libmediandk.so.  To keep the build system
 * straightforward this backend currently provides documented stubs that
 * return CROSSOS_ERR_UNSUPPORT.
 *
 * A full implementation would follow this pipeline:
 *   1. ACameraManager_create()
 *   2. ACameraManager_getCameraIdList() → enumerate cameras
 *   3. ACameraManager_openCamera() → ACameraDevice
 *   4. AImageReader_new(width, height, AIMAGE_FORMAT_YUV_420_888, 4, &reader)
 *   5. AImageReader_getWindow() → ANativeWindow
 *   6. ACaptureSessionOutput_create(window, &output)
 *   7. ACaptureRequest_create(dev, TEMPLATE_PREVIEW, &request)
 *   8. ACameraDevice_createCaptureSession(...)
 *   9. ACameraCaptureSession_setRepeatingRequest(...)
 *  10. AImageReader_acquireLatestImage() per frame
 *  11. AImage_getPlaneData() → YUV→RGBA conversion → CROSSOS_OK
 *
 * AndroidManifest.xml must declare:
 *   <uses-permission android:name="android.permission.CAMERA" />
 *   <uses-feature android:name="android.hardware.camera" />
 */

#if defined(__ANDROID__)

#include <crossos/camera.h>

#include <stdlib.h>
#include <string.h>

/* ── Camera handle (opaque on this platform) ────────────────────────── */

struct crossos_camera
{
    crossos_camera_info_t info;
    int streaming;
};

static void camera_platform_stop(crossos_camera_t *cam);

/* ── Platform API ───────────────────────────────────────────────────── */

crossos_result_t camera_platform_init(void)
{
    /*
     * Full implementation: ACameraManager_create()
     * Requires android.permission.CAMERA at runtime on API 23+.
     */
    return CROSSOS_ERR_UNSUPPORT;
}

void camera_platform_shutdown(void)
{
    /* Full implementation: ACameraManager_delete() */
}

int camera_platform_enumerate(crossos_camera_info_t *out, int max)
{
    (void)out;
    (void)max;
    /*
     * Full implementation:
     *   ACameraIdList *list;
     *   ACameraManager_getCameraIdList(mgr, &list);
     *   for i in list->numCameras:
     *     ACameraManager_getCameraCharacteristics(mgr, list->cameraIds[i], &chars)
     *     ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_FACING, &entry)
     */
    return 0;
}

crossos_result_t camera_platform_open(int idx, crossos_camera_t **out)
{
    (void)idx;
    (void)out;
    return CROSSOS_ERR_UNSUPPORT;
}

void camera_platform_close(crossos_camera_t *cam)
{
    if (!cam)
        return;
    camera_platform_stop(cam);
    free(cam);
}

crossos_result_t camera_platform_start(crossos_camera_t *cam)
{
    (void)cam;
    return CROSSOS_ERR_UNSUPPORT;
}

void camera_platform_stop(crossos_camera_t *cam)
{
    if (!cam)
        return;
    cam->streaming = 0;
}

crossos_result_t camera_platform_capture_frame(crossos_camera_t *cam,
                                               crossos_camera_frame_t *out)
{
    (void)cam;
    (void)out;
    return CROSSOS_ERR_UNSUPPORT;
}

void camera_platform_release_frame(crossos_camera_t *cam,
                                   crossos_camera_frame_t *frame)
{
    (void)cam;
    if (frame)
    {
        frame->pixels = NULL;
        frame->platform_data = NULL;
    }
}

crossos_result_t camera_platform_get_info(const crossos_camera_t *cam,
                                          crossos_camera_info_t *out)
{
    if (!cam || !out)
        return CROSSOS_ERR_PARAM;
    *out = cam->info;
    return CROSSOS_OK;
}

#endif /* __ANDROID__ */

/* Suppress ISO C pedantic "empty translation unit" warning on other platforms. */
typedef int crossos_camera_android_dummy_t;
