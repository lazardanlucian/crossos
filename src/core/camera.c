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

#define CROSSOS_FALLBACK_CAM_W 640
#define CROSSOS_FALLBACK_CAM_H 480

typedef struct fallback_camera
{
    crossos_camera_info_t info;
    unsigned char *pixels;
    int streaming;
    unsigned long frame_index;
} fallback_camera_t;

typedef struct fallback_node
{
    crossos_camera_t *cam;
    struct fallback_node *next;
} fallback_node_t;

static fallback_node_t *s_fallback_list = NULL;

static fallback_camera_t *fallback_cast(crossos_camera_t *cam)
{
    fallback_node_t *node = s_fallback_list;
    while (node)
    {
        if (node->cam == cam)
            return (fallback_camera_t *)cam;
        node = node->next;
    }
    return NULL;
}

static const fallback_camera_t *fallback_cast_const(const crossos_camera_t *cam)
{
    fallback_node_t *node = s_fallback_list;
    while (node)
    {
        if (node->cam == cam)
            return (const fallback_camera_t *)cam;
        node = node->next;
    }
    return NULL;
}

static int fallback_register(crossos_camera_t *cam)
{
    fallback_node_t *node = (fallback_node_t *)malloc(sizeof(*node));
    if (!node)
        return 0;
    node->cam = cam;
    node->next = s_fallback_list;
    s_fallback_list = node;
    return 1;
}

static void fallback_unregister(crossos_camera_t *cam)
{
    fallback_node_t **pp = &s_fallback_list;
    while (*pp)
    {
        if ((*pp)->cam == cam)
        {
            fallback_node_t *dead = *pp;
            *pp = dead->next;
            free(dead);
            return;
        }
        pp = &((*pp)->next);
    }
}

static void fallback_fill_frame(fallback_camera_t *fc)
{
    int w = CROSSOS_FALLBACK_CAM_W;
    int h = CROSSOS_FALLBACK_CAM_H;
    unsigned long t = fc->frame_index++;

    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            int idx = (y * w + x) * 4;
            fc->pixels[idx + 0] = (unsigned char)((x + t) & 0xFF);       /* R */
            fc->pixels[idx + 1] = (unsigned char)((y + (t * 2)) & 0xFF); /* G */
            fc->pixels[idx + 2] = (unsigned char)(((x ^ y) + t) & 0xFF); /* B */
            fc->pixels[idx + 3] = 255;                                   /* A */
        }
    }
}

static crossos_result_t fallback_open(int device_index, crossos_camera_t **out_cam)
{
    if (device_index != 0)
        return CROSSOS_ERR_PARAM;

    fallback_camera_t *fc = (fallback_camera_t *)calloc(1, sizeof(*fc));
    if (!fc)
        return CROSSOS_ERR_OOM;

    fc->pixels = (unsigned char *)malloc((size_t)CROSSOS_FALLBACK_CAM_W *
                                         (size_t)CROSSOS_FALLBACK_CAM_H * 4u);
    if (!fc->pixels)
    {
        free(fc);
        return CROSSOS_ERR_OOM;
    }

    fc->info.index = 0;
    strncpy(fc->info.name, "CrossOS Virtual Camera", sizeof(fc->info.name) - 1);
    strncpy(fc->info.device_path, "crossos://camera/virtual", sizeof(fc->info.device_path) - 1);
    fc->info.is_default = 1;

    if (!fallback_register((crossos_camera_t *)fc))
    {
        free(fc->pixels);
        free(fc);
        return CROSSOS_ERR_OOM;
    }

    *out_cam = (crossos_camera_t *)fc;
    return CROSSOS_OK;
}

static int fallback_enumerate(crossos_camera_info_t *out_infos, int max_count)
{
    if (!out_infos || max_count <= 0)
        return 0;

    memset(&out_infos[0], 0, sizeof(out_infos[0]));
    out_infos[0].index = 0;
    out_infos[0].is_default = 1;
    strncpy(out_infos[0].name, "CrossOS Virtual Camera", sizeof(out_infos[0].name) - 1);
    strncpy(out_infos[0].device_path, "crossos://camera/virtual", sizeof(out_infos[0].device_path) - 1);
    return 1;
}

/* ── Platform backend declarations ─────────────────────────────────── */

#if defined(_WIN32)

crossos_result_t camera_platform_init(void);
void camera_platform_shutdown(void);
int camera_platform_enumerate(crossos_camera_info_t *out, int max);
crossos_result_t camera_platform_open(int idx, crossos_camera_t **out);
void camera_platform_close(crossos_camera_t *cam);
crossos_result_t camera_platform_start(crossos_camera_t *cam);
void camera_platform_stop(crossos_camera_t *cam);
crossos_result_t camera_platform_capture_frame(crossos_camera_t *cam,
                                               crossos_camera_frame_t *out);
void camera_platform_release_frame(crossos_camera_t *cam,
                                   crossos_camera_frame_t *frame);
crossos_result_t camera_platform_get_info(const crossos_camera_t *cam,
                                          crossos_camera_info_t *out);

#elif defined(__ANDROID__)

crossos_result_t camera_platform_init(void);
void camera_platform_shutdown(void);
int camera_platform_enumerate(crossos_camera_info_t *out, int max);
crossos_result_t camera_platform_open(int idx, crossos_camera_t **out);
void camera_platform_close(crossos_camera_t *cam);
crossos_result_t camera_platform_start(crossos_camera_t *cam);
void camera_platform_stop(crossos_camera_t *cam);
crossos_result_t camera_platform_capture_frame(crossos_camera_t *cam,
                                               crossos_camera_frame_t *out);
void camera_platform_release_frame(crossos_camera_t *cam,
                                   crossos_camera_frame_t *frame);
crossos_result_t camera_platform_get_info(const crossos_camera_t *cam,
                                          crossos_camera_info_t *out);

#elif defined(__linux__)

crossos_result_t camera_platform_init(void);
void camera_platform_shutdown(void);
int camera_platform_enumerate(crossos_camera_info_t *out, int max);
crossos_result_t camera_platform_open(int idx, crossos_camera_t **out);
void camera_platform_close(crossos_camera_t *cam);
crossos_result_t camera_platform_start(crossos_camera_t *cam);
void camera_platform_stop(crossos_camera_t *cam);
crossos_result_t camera_platform_capture_frame(crossos_camera_t *cam,
                                               crossos_camera_frame_t *out);
void camera_platform_release_frame(crossos_camera_t *cam,
                                   crossos_camera_frame_t *frame);
crossos_result_t camera_platform_get_info(const crossos_camera_t *cam,
                                          crossos_camera_info_t *out);

#else /* Unsupported platform – stub everything out */

static crossos_result_t camera_platform_init(void)
{
    return CROSSOS_ERR_UNSUPPORT;
}
static void camera_platform_shutdown(void) {}
static int camera_platform_enumerate(crossos_camera_info_t *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}
static crossos_result_t camera_platform_open(int idx, crossos_camera_t **out)
{
    (void)idx;
    (void)out;
    return CROSSOS_ERR_UNSUPPORT;
}
static void camera_platform_close(crossos_camera_t *cam) { (void)cam; }
static crossos_result_t camera_platform_start(crossos_camera_t *cam)
{
    (void)cam;
    return CROSSOS_ERR_UNSUPPORT;
}
static void camera_platform_stop(crossos_camera_t *cam) { (void)cam; }
static crossos_result_t camera_platform_capture_frame(crossos_camera_t *cam,
                                                      crossos_camera_frame_t *out)
{
    (void)cam;
    (void)out;
    return CROSSOS_ERR_UNSUPPORT;
}
static void camera_platform_release_frame(crossos_camera_t *cam,
                                          crossos_camera_frame_t *frame)
{
    (void)cam;
    (void)frame;
}
static crossos_result_t camera_platform_get_info(const crossos_camera_t *cam,
                                                 crossos_camera_info_t *out)
{
    (void)cam;
    (void)out;
    return CROSSOS_ERR_UNSUPPORT;
}

#endif /* platform selection */

/* ── Public API ──────────────────────────────────────────────────────── */

crossos_result_t crossos_camera_init(void)
{
    crossos_result_t rc = camera_platform_init();
    if (rc == CROSSOS_ERR_UNSUPPORT)
        return CROSSOS_OK;
    return rc;
}

void crossos_camera_shutdown(void)
{
    camera_platform_shutdown();
}

int crossos_camera_enumerate(crossos_camera_info_t *out_infos, int max_count)
{
    if (!out_infos || max_count <= 0)
        return 0;
    if (max_count > CROSSOS_CAMERA_MAX_DEVICES)
        max_count = CROSSOS_CAMERA_MAX_DEVICES;

    int count = camera_platform_enumerate(out_infos, max_count);
    if (count > 0)
        return count;

    return fallback_enumerate(out_infos, max_count);
}

crossos_result_t crossos_camera_open(int device_index, crossos_camera_t **out_cam)
{
    if (!out_cam)
        return CROSSOS_ERR_PARAM;
    if (device_index < 0)
        return CROSSOS_ERR_PARAM;

    crossos_result_t rc = camera_platform_open(device_index, out_cam);
    if (rc == CROSSOS_ERR_UNSUPPORT)
    {
        return fallback_open(device_index, out_cam);
    }
    return rc;
}

void crossos_camera_close(crossos_camera_t *cam)
{
    if (!cam)
        return;

    fallback_camera_t *fc = fallback_cast(cam);
    if (fc)
    {
        fallback_unregister(cam);
        free(fc->pixels);
        free(fc);
        return;
    }

    camera_platform_close(cam);
}

crossos_result_t crossos_camera_start(crossos_camera_t *cam)
{
    if (!cam)
        return CROSSOS_ERR_PARAM;

    fallback_camera_t *fc = fallback_cast(cam);
    if (fc)
    {
        fc->streaming = 1;
        return CROSSOS_OK;
    }

    return camera_platform_start(cam);
}

void crossos_camera_stop(crossos_camera_t *cam)
{
    if (!cam)
        return;

    fallback_camera_t *fc = fallback_cast(cam);
    if (fc)
    {
        fc->streaming = 0;
        return;
    }

    camera_platform_stop(cam);
}

crossos_result_t crossos_camera_capture_frame(crossos_camera_t *cam,
                                              crossos_camera_frame_t *out_frame)
{
    if (!cam || !out_frame)
        return CROSSOS_ERR_PARAM;

    fallback_camera_t *fc = fallback_cast(cam);
    if (fc)
    {
        if (!fc->streaming)
            return CROSSOS_ERR_CAMERA;
        fallback_fill_frame(fc);
        out_frame->pixels = fc->pixels;
        out_frame->width = CROSSOS_FALLBACK_CAM_W;
        out_frame->height = CROSSOS_FALLBACK_CAM_H;
        out_frame->stride = CROSSOS_FALLBACK_CAM_W * 4;
        out_frame->timestamp_ms = fc->frame_index * 16ul;
        out_frame->platform_data = NULL;
        return CROSSOS_OK;
    }

    return camera_platform_capture_frame(cam, out_frame);
}

void crossos_camera_release_frame(crossos_camera_t *cam,
                                  crossos_camera_frame_t *frame)
{
    if (!cam || !frame)
        return;

    fallback_camera_t *fc = fallback_cast(cam);
    if (fc)
    {
        (void)fc;
        frame->pixels = NULL;
        frame->platform_data = NULL;
        return;
    }

    camera_platform_release_frame(cam, frame);
}

crossos_result_t crossos_camera_get_info(const crossos_camera_t *cam,
                                         crossos_camera_info_t *out_info)
{
    if (!cam || !out_info)
        return CROSSOS_ERR_PARAM;

    const fallback_camera_t *fc = fallback_cast_const(cam);
    if (fc)
    {
        *out_info = fc->info;
        return CROSSOS_OK;
    }

    return camera_platform_get_info(cam, out_info);
}
