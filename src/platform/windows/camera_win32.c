/*
 * src/platform/windows/camera_win32.c  –  Media Foundation camera backend.
 *
 * This file provides the Windows camera backend using the Media Foundation
 * Source Reader API (IMFSourceReader / IMFMediaSource).
 *
 * Full Media Foundation enumeration and capture require linking against
 * mfplat.dll, mf.dll, mfreadwrite.dll, and mfuuid.lib.  To keep the SDK
 * dependency-free by default, this backend currently provides functional
 * stubs that return CROSSOS_ERR_UNSUPPORT.
 *
 * A full implementation would follow this pipeline:
 *   1. CoInitializeEx / MFStartup
 *   2. MFEnumDeviceSources(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)
 *   3. IMFActivate::ActivateObject → IMFMediaSource
 *   4. MFCreateSourceReaderFromMediaSource → IMFSourceReader
 *   5. IMFSourceReader::SetCurrentMediaType (MFVideoFormat_YUY2 or RGB32)
 *   6. IMFSourceReader::ReadSample in a loop
 *   7. Lock IMFMediaBuffer, convert to RGBA8888
 */

#if defined(_WIN32)

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
     * Full implementation: CoInitializeEx(NULL, COINIT_MULTITHREADED)
     *                      + MFStartup(MF_VERSION, MFSTARTUP_FULL)
     */
    return CROSSOS_ERR_UNSUPPORT;
}

void camera_platform_shutdown(void)
{
    /* Full implementation: MFShutdown() + CoUninitialize() */
}

int camera_platform_enumerate(crossos_camera_info_t *out, int max)
{
    (void)out;
    (void)max;
    /*
     * Full implementation:
     *   IMFAttributes *attr;
     *   MFCreateAttributes(&attr, 1);
     *   attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
     *                 MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
     *   MFEnumDeviceSources(attr, &ppDevices, &count);
     *   for each ppDevices[i]: GetAllocatedString(
     *       MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, ...)
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

#endif /* _WIN32 */

/* Suppress ISO C pedantic "empty translation unit" warning on other platforms. */
typedef int crossos_camera_win32_dummy_t;
