/*
 * src/platform/linux/camera_linux.c  –  V4L2 camera backend.
 *
 * Implements camera capture via the Video4Linux 2 (V4L2) kernel API.
 * No external libraries are required; all interaction is through ioctl()
 * on /dev/videoN device nodes.
 *
 * Frame pipeline:
 *   1. Enumerate /dev/video0 … /dev/video9
 *   2. Open the device, query capabilities (VIDIOC_QUERYCAP)
 *   3. Negotiate format: prefer YUYV 640×480 (VIDIOC_S_FMT)
 *   4. Request and mmap() 4 streaming buffers (VIDIOC_REQBUFS)
 *   5. Queue all buffers, start streaming (VIDIOC_STREAMON)
 *   6. Per capture_frame: VIDIOC_DQBUF → YUYV→RGBA conversion →
 *      VIDIOC_QBUF (re-queue)
 *   7. VIDIOC_STREAMOFF + munmap on close
 *
 * YUYV → RGBA conversion uses the standard BT.601 integer math.
 */

#if defined(__linux__) && !defined(__ANDROID__)

#include <crossos/camera.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>                /* must precede linux/videodev2.h for timespec */
#include <linux/videodev2.h>

/* ── Build-time guard ──────────────────────────────────────────────── */
/*
 * V4L2 is always available on Linux; no optional compile flag needed.
 * The videodev2.h header is part of the linux-libc-dev (Debian/Ubuntu)
 * or kernel-headers package.
 */

/* ── Constants ─────────────────────────────────────────────────────── */

#define CAM_NUM_BUFS   4        /* number of mmap buffers per stream */
#define CAM_MAX_DEVS   10       /* /dev/video0 … /dev/video9         */
#define CAM_DEF_WIDTH  640
#define CAM_DEF_HEIGHT 480

/* ── Buffer descriptor ──────────────────────────────────────────────── */

typedef struct {
    void  *start;
    size_t length;
} cam_buf_t;

/* ── Camera handle ──────────────────────────────────────────────────── */

struct crossos_camera {
    int                 fd;
    crossos_camera_info_t info;
    int                 streaming;

    /* Negotiated capture format */
    int                 cap_width;
    int                 cap_height;
    __u32               cap_pixfmt;  /* V4L2_PIX_FMT_* */

    /* mmap'd streaming buffers */
    cam_buf_t           bufs[CAM_NUM_BUFS];
    int                 num_bufs;

    /* RGBA conversion buffer (reused across frames) */
    unsigned char      *rgba_buf;
    int                 rgba_size;
};

/* ── ioctl wrapper ─────────────────────────────────────────────────── */

static int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

/* ── YUYV → RGBA conversion ────────────────────────────────────────── */
/*
 * Converts a YUYV (YUV 4:2:2 packed) frame to RGBA8888.
 * Uses BT.601 coefficients with integer arithmetic.
 *
 * Layout: Y0 U0 Y1 V0 Y2 U1 Y3 V1 ...
 * Two adjacent pixels share the same U/V pair.
 */

static inline int clamp_u8(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static void yuyv_to_rgba(const unsigned char *src, unsigned char *dst,
                          int width, int height)
{
    int npairs = (width * height) / 2;
    for (int i = 0; i < npairs; i++) {
        int y0 = src[0];
        int u  = src[1];
        int y1 = src[2];
        int v  = src[3];
        src += 4;

        int c0 = y0 - 16;
        int c1 = y1 - 16;
        int d  = u  - 128;
        int e  = v  - 128;

        /* Pixel 0 */
        dst[0] = (unsigned char)clamp_u8((298 * c0           + 409 * e + 128) >> 8);
        dst[1] = (unsigned char)clamp_u8((298 * c0 - 100 * d - 208 * e + 128) >> 8);
        dst[2] = (unsigned char)clamp_u8((298 * c0 + 516 * d           + 128) >> 8);
        dst[3] = 255;

        /* Pixel 1 */
        dst[4] = (unsigned char)clamp_u8((298 * c1           + 409 * e + 128) >> 8);
        dst[5] = (unsigned char)clamp_u8((298 * c1 - 100 * d - 208 * e + 128) >> 8);
        dst[6] = (unsigned char)clamp_u8((298 * c1 + 516 * d           + 128) >> 8);
        dst[7] = 255;

        dst += 8;
    }
}

/* ── Enumerate helpers ──────────────────────────────────────────────── */

static int probe_device(const char *path, crossos_camera_info_t *out, int idx)
{
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) return 0;

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        close(fd);
        return 0;
    }

    /* Must support video capture and streaming. */
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
        close(fd);
        return 0;
    }

    out->index      = idx;
    out->is_default = 0;  /* set by the caller (camera_platform_enumerate) */
    snprintf(out->name, sizeof(out->name), "%s", (char *)cap.card);
    snprintf(out->device_path, sizeof(out->device_path), "%s", path);

    close(fd);
    return 1;
}

/* ── Platform API ───────────────────────────────────────────────────── */

crossos_result_t camera_platform_init(void)
{
    return CROSSOS_OK; /* V4L2 needs no global init */
}

void camera_platform_shutdown(void)
{
    /* Nothing to do at the subsystem level. */
}

int camera_platform_enumerate(crossos_camera_info_t *out, int max)
{
    int found = 0;
    char path[32];
    for (int i = 0; i < CAM_MAX_DEVS && found < max; i++) {
        snprintf(path, sizeof(path), "/dev/video%d", i);
        /* Pass the enumeration position (found) as the logical index so
         * that crossos_camera_open(n) reliably opens the n-th found camera.
         * The device_path field carries the actual /dev/videoN path.     */
        if (probe_device(path, &out[found], found)) {
            /* The first successfully probed camera is the system default. */
            out[found].is_default = (found == 0);
            found++;
        }
    }
    return found;
}

crossos_result_t camera_platform_open(int idx, crossos_camera_t **out)
{
    /* Map the logical index to a /dev/videoN that actually exists. */
    crossos_camera_info_t infos[CROSSOS_CAMERA_MAX_DEVICES];
    int count = camera_platform_enumerate(infos,
                    CROSSOS_CAMERA_MAX_DEVICES);
    if (idx >= count) return CROSSOS_ERR_PARAM;

    int fd = open(infos[idx].device_path, O_RDWR);
    if (fd < 0) return CROSSOS_ERR_CAMERA;

    /* Query capabilities. */
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        close(fd);
        return CROSSOS_ERR_CAMERA;
    }

    struct crossos_camera *cam = calloc(1, sizeof(*cam));
    if (!cam) { close(fd); return CROSSOS_ERR_OOM; }

    cam->fd   = fd;
    cam->info = infos[idx];

    *out = cam;
    return CROSSOS_OK;
}

/* Forward declaration: camera_platform_close calls camera_platform_stop. */
static void camera_platform_stop_impl(crossos_camera_t *cam);

void camera_platform_close(crossos_camera_t *cam)
{
    if (!cam) return;
    camera_platform_stop_impl(cam);

    /* Unmap and free buffers. */
    for (int i = 0; i < cam->num_bufs; i++) {
        if (cam->bufs[i].start && cam->bufs[i].start != MAP_FAILED)
            munmap(cam->bufs[i].start, cam->bufs[i].length);
    }

    free(cam->rgba_buf);
    close(cam->fd);
    free(cam);
}

crossos_result_t camera_platform_start(crossos_camera_t *cam)
{
    if (!cam || cam->streaming) return CROSSOS_ERR_PARAM;

    /* ── Negotiate format ──────────────────────────────────────────── */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = CAM_DEF_WIDTH;
    fmt.fmt.pix.height      = CAM_DEF_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;

    if (xioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
        /* Try MJPEG as a fallback. */
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        if (xioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0)
            return CROSSOS_ERR_CAMERA;
    }

    cam->cap_width  = (int)fmt.fmt.pix.width;
    cam->cap_height = (int)fmt.fmt.pix.height;
    cam->cap_pixfmt = fmt.fmt.pix.pixelformat;

    /* ── Allocate mmap buffers ─────────────────────────────────────── */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = CAM_NUM_BUFS;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1)
        return CROSSOS_ERR_CAMERA;

    int n = (int)req.count;
    if (n > CAM_NUM_BUFS) n = CAM_NUM_BUFS;

    for (int i = 0; i < n; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = (unsigned)i;

        if (xioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0)
            return CROSSOS_ERR_CAMERA;

        cam->bufs[i].length = buf.length;
        cam->bufs[i].start  = mmap(NULL, buf.length,
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED,
                                   cam->fd, buf.m.offset);

        if (cam->bufs[i].start == MAP_FAILED)
            return CROSSOS_ERR_CAMERA;
    }
    cam->num_bufs = n;

    /* ── Queue all buffers ─────────────────────────────────────────── */
    for (int i = 0; i < n; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = (unsigned)i;
        if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0)
            return CROSSOS_ERR_CAMERA;
    }

    /* ── Allocate RGBA conversion buffer ───────────────────────────── */
    cam->rgba_size = cam->cap_width * cam->cap_height * 4;
    cam->rgba_buf  = malloc((size_t)cam->rgba_size);
    if (!cam->rgba_buf) return CROSSOS_ERR_OOM;

    /* ── Start streaming ───────────────────────────────────────────── */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
        free(cam->rgba_buf);
        cam->rgba_buf = NULL;
        return CROSSOS_ERR_CAMERA;
    }

    cam->streaming = 1;
    return CROSSOS_OK;
}

static void camera_platform_stop_impl(crossos_camera_t *cam)
{
    if (!cam || !cam->streaming) return;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(cam->fd, VIDIOC_STREAMOFF, &type);
    cam->streaming = 0;

    free(cam->rgba_buf);
    cam->rgba_buf = NULL;

    for (int i = 0; i < cam->num_bufs; i++) {
        if (cam->bufs[i].start && cam->bufs[i].start != MAP_FAILED) {
            munmap(cam->bufs[i].start, cam->bufs[i].length);
            cam->bufs[i].start  = NULL;
            cam->bufs[i].length = 0;
        }
    }
    cam->num_bufs = 0;
}

void camera_platform_stop(crossos_camera_t *cam)
{
    camera_platform_stop_impl(cam);
}

crossos_result_t camera_platform_capture_frame(crossos_camera_t       *cam,
                                               crossos_camera_frame_t *out)
{
    if (!cam || !cam->streaming || !out) return CROSSOS_ERR_PARAM;

    /* Wait for a buffer with select(). */
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(cam->fd, &fds);
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };

    int r = select(cam->fd + 1, &fds, NULL, NULL, &tv);
    if (r <= 0) return CROSSOS_ERR_CAMERA; /* timeout or error */

    /* Dequeue a buffer. */
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (xioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0)
        return CROSSOS_ERR_CAMERA;

    /* Get monotonic timestamp in ms. */
    struct timeval now;
    gettimeofday(&now, NULL);
    unsigned long ts_ms = (unsigned long)(now.tv_sec * 1000UL +
                                          now.tv_usec / 1000UL);

    /* Convert to RGBA. */
    const unsigned char *raw = (const unsigned char *)cam->bufs[buf.index].start;

    if (cam->cap_pixfmt == V4L2_PIX_FMT_YUYV) {
        yuyv_to_rgba(raw, cam->rgba_buf, cam->cap_width, cam->cap_height);
    } else {
        /* For unsupported pixel formats, fill with a grey checkerboard. */
        memset(cam->rgba_buf, 0x80, (size_t)cam->rgba_size);
    }

    /* Re-queue the buffer immediately so the driver can refill it. */
    xioctl(cam->fd, VIDIOC_QBUF, &buf);

    /* Fill the output frame descriptor. */
    out->pixels       = cam->rgba_buf;
    out->width        = cam->cap_width;
    out->height       = cam->cap_height;
    out->stride       = cam->cap_width * 4;
    out->timestamp_ms = ts_ms;
    out->platform_data = NULL;

    return CROSSOS_OK;
}

void camera_platform_release_frame(crossos_camera_t       *cam,
                                   crossos_camera_frame_t *frame)
{
    /* The RGBA buffer is owned by the camera handle and re-used across
     * frames; nothing to free here.  Simply clear the caller's pointer. */
    (void)cam;
    if (frame) {
        frame->pixels       = NULL;
        frame->platform_data = NULL;
    }
}

crossos_result_t camera_platform_get_info(const crossos_camera_t *cam,
                                           crossos_camera_info_t  *out)
{
    if (!cam || !out) return CROSSOS_ERR_PARAM;
    *out = cam->info;
    return CROSSOS_OK;
}

#endif /* __linux__ && !__ANDROID__ */
