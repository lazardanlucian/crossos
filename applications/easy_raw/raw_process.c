/**
 * applications/easy_raw/raw_process.c
 *
 * Non-destructive RAW processing pipeline for EasyRaw.
 *
 * Each stage operates on float32 data in linear light except the final
 * gamma-encode step which converts to 8-bit sRGB.
 */

#include "raw_process.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <pthread.h>

/* ── Defaults ────────────────────────────────────────────────────────── */

void er_params_init(er_params_t *p)
{
    memset(p, 0, sizeof(*p));
    p->exposure_ev = 0.0f;
    p->wb_temp = 5500.0f;
    p->wb_tint = 0.0f;
    p->contrast = 0.0f;
    p->vibrance = 0.0f;
    p->saturation = 0.0f;
    p->sharpening = 0.0f;
    p->noise_reduction = 0.0f;
    p->highlight_recovery = 0.0f;
    p->output_gamma = 0.0f; /* 0 = standard sRGB */

    /* Identity tone curves: two end-points only */
    for (int c = 0; c < 4; c++)
    {
        er_curve_t *cv = (c == 0)   ? &p->curve_luma
                         : (c == 1) ? &p->curve_r
                         : (c == 2) ? &p->curve_g
                                    : &p->curve_b;
        cv->count = 2;
        cv->pts[0].in = 0.0f;
        cv->pts[0].out = 0.0f;
        cv->pts[1].in = 1.0f;
        cv->pts[1].out = 1.0f;
    }
}

/* ── Math helpers ────────────────────────────────────────────────────── */

#define CLAMPF(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) \
                                                          : (v))
#define MAXF(a, b) ((a) > (b) ? (a) : (b))
#define MINF(a, b) ((a) < (b) ? (a) : (b))

typedef void (*er_parallel_fn)(int start, int end, void *userdata);

typedef struct
{
    er_parallel_fn fn;
    void *userdata;
    int start;
    int end;
} er_parallel_task_t;

typedef struct
{
    float *plane;
    int w;
    int h;
    int r;
    const float *kernel;
} blur_ctx_t;

static int er_cpu_count(void)
{
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors > 0 ? (int)info.dwNumberOfProcessors : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

static void *er_parallel_thread(void *arg)
{
    er_parallel_task_t *task = (er_parallel_task_t *)arg;
    task->fn(task->start, task->end, task->userdata);
    return NULL;
}

static void er_parallel_for(int total, int min_chunk,
                            er_parallel_fn fn, void *userdata)
{
    if (total <= 0)
        return;

    int workers = er_cpu_count();
    if (workers > 8)
        workers = 8;
    if (workers < 1)
        workers = 1;
    if (min_chunk < 1)
        min_chunk = 1;
    if (total / workers < min_chunk)
        workers = MAXF(1, total / min_chunk);
    if (workers < 2)
    {
        fn(0, total, userdata);
        return;
    }

    pthread_t tids[8];
    er_parallel_task_t tasks[8];
    int base = total / workers;
    int rem = total % workers;
    int start = 0;

    for (int i = 0; i < workers; i++)
    {
        int count = base + (i < rem ? 1 : 0);
        tasks[i].fn = fn;
        tasks[i].userdata = userdata;
        tasks[i].start = start;
        tasks[i].end = start + count;
        start += count;
    }

    for (int i = 1; i < workers; i++)
        pthread_create(&tids[i], NULL, er_parallel_thread, &tasks[i]);

    tasks[0].fn(tasks[0].start, tasks[0].end, tasks[0].userdata);

    for (int i = 1; i < workers; i++)
        pthread_join(tids[i], NULL);
}

static void blur_horizontal_pass(int start_y, int end_y, void *userdata)
{
    blur_ctx_t *job = (blur_ctx_t *)userdata;
    float *tmp = (float *)malloc((size_t)job->w * sizeof(float));
    if (!tmp)
        return;
    for (int y = start_y; y < end_y; y++)
    {
        float *row = job->plane + y * job->w;
        memcpy(tmp, row, (size_t)job->w * sizeof(float));
        for (int x = 0; x < job->w; x++)
        {
            float acc = 0.0f;
            for (int k = -job->r; k <= job->r; k++)
            {
                int sx = x + k;
                if (sx < 0)
                    sx = 0;
                if (sx >= job->w)
                    sx = job->w - 1;
                acc += tmp[sx] * job->kernel[k + job->r];
            }
            row[x] = acc;
        }
    }
    free(tmp);
}

static void blur_vertical_pass(int start_x, int end_x, void *userdata)
{
    blur_ctx_t *job = (blur_ctx_t *)userdata;
    float *col = (float *)malloc((size_t)job->h * sizeof(float));
    if (!col)
        return;
    for (int x = start_x; x < end_x; x++)
    {
        for (int y = 0; y < job->h; y++)
            col[y] = job->plane[y * job->w + x];
        for (int y = 0; y < job->h; y++)
        {
            float acc = 0.0f;
            for (int k = -job->r; k <= job->r; k++)
            {
                int sy = y + k;
                if (sy < 0)
                    sy = 0;
                if (sy >= job->h)
                    sy = job->h - 1;
                acc += col[sy] * job->kernel[k + job->r];
            }
            job->plane[y * job->w + x] = acc;
        }
    }
    free(col);
}

/** sRGB linear → gamma-encoded byte, standard IEC 61966-2-1 curve. */
static uint8_t linear_to_srgb8(float x)
{
    x = CLAMPF(x, 0.0f, 1.0f);
    float enc = (x <= 0.0031308f) ? (12.92f * x)
                                  : (1.055f * powf(x, 1.0f / 2.4f) - 0.055f);
    return (uint8_t)(enc * 255.0f + 0.5f);
}

/** Evaluate a piecewise-linear tone curve at value x ∈ [0,1]. */
static float eval_curve(const er_curve_t *c, float x)
{
    if (c->count < 2)
        return x;
    /* Find segment */
    for (int i = 0; i < c->count - 1; i++)
    {
        float x0 = c->pts[i].in, y0 = c->pts[i].out;
        float x1 = c->pts[i + 1].in, y1 = c->pts[i + 1].out;
        if (x <= x1 || i == c->count - 2)
        {
            if (x1 == x0)
                return y0;
            float t = (x - x0) / (x1 - x0);
            t = CLAMPF(t, 0.0f, 1.0f);
            return y0 + t * (y1 - y0);
        }
    }
    return x;
}

/* ── White Balance multipliers from temperature ─────────────────────── */
/*
 * Approximate daylight-locus multipliers using a polynomial fit to
 * the CIE daylight series (sufficient for display purposes).
 */
static void wb_multipliers(float temp_k, float tint,
                           float *mr, float *mg, float *mb)
{
    /* Normalise temperature to roughly 1.0 at 5500 K (daylight) */
    float t = temp_k / 5500.0f;
    float tr = 1.0f / t;             /* cooler temp → more red */
    float tb = t;                    /* warmer temp → more blue*/
    float tg = 1.0f + tint / 200.0f; /* tint: positive = more green */

    /* Normalise so G = 1.0 */
    *mg = 1.0f / tg;
    *mr = tr / tg;
    *mb = tb / tg;
}

/* ── Shadows/Highlights ─────────────────────────────────────────────── */
/*
 * Lifted from the RawTherapee parametric tone curve approach:
 * apply a smooth S-curve per channel driven by shadows/highlights sliders.
 */
static float tone_adjustment(float v, float shadows, float highlights,
                             float blacks, float whites, float midtones)
{
    /* Convert sliders (-100..+100) to scale factors */
    float sh = shadows / 200.0f; /* -0.5 .. +0.5 */
    float hl = highlights / 200.0f;
    float bl = blacks / 200.0f;
    float wh = whites / 200.0f;
    float mid = midtones / 200.0f;

    /* Apply black/white point adjust */
    float lo = 0.0f + bl * 0.5f;
    float hi = 1.0f + wh * 0.5f;
    if (hi != lo)
        v = (v - lo) / (hi - lo);
    v = CLAMPF(v, 0.0f, 1.0f);

    /* Shadow lift (logistic blend in dark region) */
    float mask_shadow = 1.0f - v;
    mask_shadow = mask_shadow * mask_shadow;
    float mask_highlight = v * v;
    float mask_mid = 4.0f * v * (1.0f - v); /* peaks at 0.5 */

    v += sh * mask_shadow + hl * mask_highlight + mid * mask_mid;
    return CLAMPF(v, 0.0f, 1.0f);
}

/* ── RGB → HSL → RGB (for vibrance / saturation / HSL sliders) ───────── */

static void rgb_to_hsl(float r, float g, float b,
                       float *h, float *s, float *l)
{
    float mx = MAXF(r, MAXF(g, b));
    float mn = MINF(r, MINF(g, b));
    *l = (mx + mn) * 0.5f;
    float d = mx - mn;
    if (d < 1e-6f)
    {
        *h = 0.0f;
        *s = 0.0f;
        return;
    }
    *s = d / (1.0f - fabsf(2.0f * (*l) - 1.0f));
    if (mx == r)
        *h = fmodf((g - b) / d + 6.0f, 6.0f) / 6.0f;
    else if (mx == g)
        *h = ((b - r) / d + 2.0f) / 6.0f;
    else
        *h = ((r - g) / d + 4.0f) / 6.0f;
}

static float hue_to_rgb(float p, float q, float t)
{
    if (t < 0.0f)
        t += 1.0f;
    if (t > 1.0f)
        t -= 1.0f;
    if (t < 1.0f / 6.0f)
        return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f)
        return q;
    if (t < 2.0f / 3.0f)
        return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

static void hsl_to_rgb(float h, float s, float l,
                       float *r, float *g, float *b)
{
    if (s < 1e-6f)
    {
        *r = *g = *b = l;
        return;
    }
    float q = (l < 0.5f) ? l * (1.0f + s) : l + s - l * s;
    float p = 2.0f * l - q;
    *r = hue_to_rgb(p, q, h + 1.0f / 3.0f);
    *g = hue_to_rgb(p, q, h);
    *b = hue_to_rgb(p, q, h - 1.0f / 3.0f);
}

/* Returns the HSL slice index 0-5 for a given hue (R,O,Y,G,Cy,B). */
static int hue_slice(float h)
{
    /* 6 slices, each 1/6 of the hue wheel */
    int s = (int)(h * 6.0f);
    if (s < 0)
        s = 0;
    if (s > 5)
        s = 5;
    return s;
}

/* ── Simple Gaussian blur (noise reduction) ─────────────────────────── */
/*
 * Separable 1-D Gaussian with radius r = ceil(2.5 * sigma).
 * Operates in-place; uses a temporary row buffer.
 */
static void gaussian_blur_plane(float *plane, int w, int h, float sigma)
{
    if (sigma < 0.01f)
        return;
    int r = (int)ceilf(2.5f * sigma);
    if (r < 1)
        r = 1;

    int klen = 2 * r + 1;
    float *kernel = (float *)malloc((size_t)klen * sizeof(float));
    if (!kernel)
    {
        free(kernel);
        return;
    }

    /* Build 1-D Gaussian kernel */
    float sum = 0.0f;
    for (int i = 0; i < klen; i++)
    {
        float d = (float)(i - r);
        kernel[i] = expf(-0.5f * d * d / (sigma * sigma));
        sum += kernel[i];
    }
    for (int i = 0; i < klen; i++)
        kernel[i] /= sum;

    blur_ctx_t ctx = {plane, w, h, r, kernel};
    er_parallel_for(h, 32, blur_horizontal_pass, &ctx);
    er_parallel_for(w, 32, blur_vertical_pass, &ctx);
    free(kernel);
}

/*
 * Faster sharpening: one luma blur drives edge enhancement for all channels.
 * This is noticeably cheaper than blurring/sharpening each RGB plane.
 */
static void unsharp_mask_luma(float *R, float *G, float *B, int w, int h, float amount)
{
    if (amount < 0.01f)
        return;

    int n = w * h;
    float *luma = (float *)malloc((size_t)n * sizeof(float));
    if (!luma)
        return;

    for (int i = 0; i < n; i++)
        luma[i] = 0.2126f * R[i] + 0.7152f * G[i] + 0.0722f * B[i];

    float *blurred = (float *)malloc((size_t)n * sizeof(float));
    if (!blurred)
    {
        free(luma);
        return;
    }

    memcpy(blurred, luma, (size_t)n * sizeof(float));
    gaussian_blur_plane(blurred, w, h, 1.2f);

    float gain = amount * 2.0f;
    for (int i = 0; i < n; i++)
    {
        float edge = (luma[i] - blurred[i]) * gain;
        R[i] = CLAMPF(R[i] + edge, 0.0f, 1.0f);
        G[i] = CLAMPF(G[i] + edge, 0.0f, 1.0f);
        B[i] = CLAMPF(B[i] + edge, 0.0f, 1.0f);
    }

    free(blurred);
    free(luma);
}

/* ── Camera colour matrix application ─────────────────────────────────────── */
/*
 * The cam_matrix field stores a 3×3 camera-to-XYZ (D65) transform.
 * Compose it with the standard D65 XYZ→sRGB matrix to get a single
 * cam→sRGB matrix that we can apply per-pixel.
 *
 * XYZ D65 → sRGB (IEC 61966-2-1):
 *   [[ 3.2406, -1.5372, -0.4986],
 *    [-0.9689,  1.8758,  0.0415],
 *    [ 0.0557, -0.2040,  1.0570]]
 */
static int is_cam_matrix_valid(const double m[3][3])
{
    double sum = 0.0;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            sum += m[r][c] * m[r][c];
    return sum > 1e-6;
}

static void apply_cam_matrix(float *R, float *G, float *B, int n,
                             const double cam[3][3])
{
    /* D65 XYZ→sRGB (row-major; multiply column vector) */
    static const double xyz2srgb[3][3] = {
        {3.2406, -1.5372, -0.4986},
        {-0.9689, 1.8758, 0.0415},
        {0.0557, -0.2040, 1.0570}};
    /* Compose: M = xyz2srgb * cam */
    double M[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
        {
            M[r][c] = 0.0;
            for (int k = 0; k < 3; k++)
                M[r][c] += xyz2srgb[r][k] * cam[k][c];
        }
    for (int i = 0; i < n; i++)
    {
        float ri = R[i], gi = G[i], bi = B[i];
        float ro = (float)(M[0][0] * ri + M[0][1] * gi + M[0][2] * bi);
        float go = (float)(M[1][0] * ri + M[1][1] * gi + M[1][2] * bi);
        float bo = (float)(M[2][0] * ri + M[2][1] * gi + M[2][2] * bi);
        R[i] = CLAMPF(ro, 0.0f, 1.0f);
        G[i] = CLAMPF(go, 0.0f, 1.0f);
        B[i] = CLAMPF(bo, 0.0f, 1.0f);
    }
}

/* ── Highlight recovery ────────────────────────────────────────────────── */
/*
 * For pixels where one or two channels have clipped (value ≥ clip_thresh):
 *   • If only one channel clips: reconstruct it from the G−R / G−B color
 *     ratio locked in the local average of non-clipped neighbours.
 *   • If two or three channels clip: blend towards neutral luminance.
 * The `amount` parameter (0–1) controls the blend strength.
 */
static void highlight_recovery(float *R, float *G, float *B, int w, int h,
                               float amount)
{
    if (amount < 0.001f)
        return;
    const float clip = 0.98f;

    for (int i = 0; i < w * h; i++)
    {
        float r = R[i], g = G[i], b = B[i];
        int cr = (r >= clip), cg = (g >= clip), cb = (b >= clip);
        int nc = cr + cg + cb;
        if (nc == 0)
            continue;

        if (nc == 1)
        {
            /* Single-channel clip: reconstruct from the two good channels */
            float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            if (cr)
            {
                /* Predict R from G and B */
                float pred = (g + b) > 1e-4f ? luma * 2.0f - g - b + (r - (g + b) * 0.5f) : r;
                pred = CLAMPF(pred, clip, 1.5f);
                R[i] = r + amount * (pred - r);
            }
            else if (cg)
            {
                float pred = (r + b) > 1e-4f ? luma * 2.0f - r - b + (g - (r + b) * 0.5f) : g;
                pred = CLAMPF(pred, clip, 1.5f);
                G[i] = g + amount * (pred - g);
            }
            else
            {
                float pred = (r + g) > 1e-4f ? luma * 2.0f - r - g + (b - (r + g) * 0.5f) : b;
                pred = CLAMPF(pred, clip, 1.5f);
                B[i] = b + amount * (pred - b);
            }
        }
        else
        {
            /* Multiple channels clipped: blend to neutral luma */
            float luma = MAXF(r, MAXF(g, b));
            R[i] = r + amount * (luma - r);
            G[i] = g + amount * (luma - g);
            B[i] = b + amount * (luma - b);
        }
    }
}

/* ── Main pipeline ────────────────────────────────────────────────────── */

int er_process_ex(const er_raw_image_t *src,
                  const er_params_t *params,
                  er_output_t *out,
                  int fast_preview)
{
    if (!src || !params || !out)
        return -1;
    int w = src->width, h = src->height;

    int scale = 1;
    if (fast_preview)
    {
        if (w * h > 3840 * 2160)
            scale = 4;
        else if (w * h > 1280 * 720)
            scale = 2;
    }
    int pw = (w + scale - 1) / scale;
    int ph = (h + scale - 1) / scale;
    int n = pw * ph;

    /* Allocate or reuse output buffer */
    size_t needed = (size_t)(w * h * 4);
    if (!out->pixels || out->width != w || out->height != h)
    {
        free(out->pixels);
        out->pixels = (uint8_t *)malloc(needed);
        if (!out->pixels)
            return -1;
        out->width = w;
        out->height = h;
        out->stride = w * 4;
    }

    /* Working copy of float planes */
    float *R = (float *)malloc((size_t)n * sizeof(float));
    float *G = (float *)malloc((size_t)n * sizeof(float));
    float *B = (float *)malloc((size_t)n * sizeof(float));
    if (!R || !G || !B)
    {
        free(R);
        free(G);
        free(B);
        return -1;
    }

    if (scale == 1)
    {
        memcpy(R, src->planes[0], (size_t)n * sizeof(float));
        memcpy(G, src->planes[1], (size_t)n * sizeof(float));
        memcpy(B, src->planes[2], (size_t)n * sizeof(float));
    }
    else
    {
        for (int y = 0; y < ph; y++)
        {
            int sy = y * scale;
            if (sy >= h)
                sy = h - 1;
            for (int x = 0; x < pw; x++)
            {
                int sx = x * scale;
                if (sx >= w)
                    sx = w - 1;
                int si = sy * w + sx;
                int di = y * pw + x;
                R[di] = src->planes[0][si];
                G[di] = src->planes[1][si];
                B[di] = src->planes[2][si];
            }
        }
    }

    /* ── 1. Noise reduction (before sharpening) ── */
    if (!fast_preview)
    {
        float nr_sigma = params->noise_reduction * 4.0f; /* 0..4 pixel sigma */
        gaussian_blur_plane(R, pw, ph, nr_sigma);
        gaussian_blur_plane(G, pw, ph, nr_sigma);
        gaussian_blur_plane(B, pw, ph, nr_sigma);
    }

    /* ── 2. Exposure ── */
    float exp_scale = powf(2.0f, params->exposure_ev);
    for (int i = 0; i < n; i++)
    {
        R[i] = CLAMPF(R[i] * exp_scale, 0.0f, 1.0f);
        G[i] = CLAMPF(G[i] * exp_scale, 0.0f, 1.0f);
        B[i] = CLAMPF(B[i] * exp_scale, 0.0f, 1.0f);
    }

    /* ── 3. White Balance ── */
    float mr, mg_wb, mb;
    wb_multipliers(params->wb_temp, params->wb_tint, &mr, &mg_wb, &mb);
    for (int i = 0; i < n; i++)
    {
        R[i] = CLAMPF(R[i] * mr, 0.0f, 2.0f); /* allow > 1 for highlight recovery */
        G[i] = CLAMPF(G[i] * mg_wb, 0.0f, 2.0f);
        B[i] = CLAMPF(B[i] * mb, 0.0f, 2.0f);
    }

    /* ── 3b. Highlight recovery (before hard clip) ── */
    if (!fast_preview)
        highlight_recovery(R, G, B, pw, ph, params->highlight_recovery);

    /* Hard clamp after recovery */
    for (int i = 0; i < n; i++)
    {
        R[i] = CLAMPF(R[i], 0.0f, 1.0f);
        G[i] = CLAMPF(G[i], 0.0f, 1.0f);
        B[i] = CLAMPF(B[i], 0.0f, 1.0f);
    }

    /* ── 3c. Camera colour matrix (cam→sRGB) ── */
    if (is_cam_matrix_valid(src->cam_matrix))
        apply_cam_matrix(R, G, B, n, src->cam_matrix);

    /* ── 4. Tone adjustments (shadows, highlights, etc.) ── */
    for (int i = 0; i < n; i++)
    {
        R[i] = tone_adjustment(R[i], params->shadows, params->highlights,
                               params->blacks, params->whites, params->midtones);
        G[i] = tone_adjustment(G[i], params->shadows, params->highlights,
                               params->blacks, params->whites, params->midtones);
        B[i] = tone_adjustment(B[i], params->shadows, params->highlights,
                               params->blacks, params->whites, params->midtones);
    }

    /* ── 5. Contrast (S-curve around 0.5) ── */
    if (fabsf(params->contrast) > 0.01f)
    {
        float c = params->contrast / 100.0f; /* -1..+1 */
        for (int i = 0; i < n; i++)
        {
            /* Simple contrast: pivot at 0.5, scale deviation */
            float rv = (R[i] - 0.5f) * (1.0f + c) + 0.5f;
            float gv = (G[i] - 0.5f) * (1.0f + c) + 0.5f;
            float bv = (B[i] - 0.5f) * (1.0f + c) + 0.5f;
            R[i] = CLAMPF(rv, 0.0f, 1.0f);
            G[i] = CLAMPF(gv, 0.0f, 1.0f);
            B[i] = CLAMPF(bv, 0.0f, 1.0f);
        }
    }

    /* ── 6. Per-channel tone curves ── */
    for (int i = 0; i < n; i++)
    {
        R[i] = eval_curve(&params->curve_r, R[i]);
        G[i] = eval_curve(&params->curve_g, G[i]);
        B[i] = eval_curve(&params->curve_b, B[i]);
    }

    /* ── 7. Luma curve (applied to luminance, preserving hue) ── */
    for (int i = 0; i < n; i++)
    {
        float luma = 0.2126f * R[i] + 0.7152f * G[i] + 0.0722f * B[i];
        float new_luma = eval_curve(&params->curve_luma, luma);
        float scale_l = (luma > 1e-6f) ? new_luma / luma : 1.0f;
        R[i] = CLAMPF(R[i] * scale_l, 0.0f, 1.0f);
        G[i] = CLAMPF(G[i] * scale_l, 0.0f, 1.0f);
        B[i] = CLAMPF(B[i] * scale_l, 0.0f, 1.0f);
    }

    /* ── 8. Saturation & Vibrance ── */
    if (fabsf(params->saturation) > 0.01f || fabsf(params->vibrance) > 0.01f)
    {
        float sat_scale = 1.0f + params->saturation / 100.0f;
        float vib = params->vibrance / 100.0f;
        for (int i = 0; i < n; i++)
        {
            float h, s, l;
            rgb_to_hsl(R[i], G[i], B[i], &h, &s, &l);
            /* Vibrance protects already-saturated colours */
            float vib_boost = vib * (1.0f - s);
            s = CLAMPF(s * sat_scale + vib_boost, 0.0f, 1.0f);
            hsl_to_rgb(h, s, l, &R[i], &G[i], &B[i]);
        }
    }

    /* ── 9. HSL per-colour sliders ── */
    int has_hsl = 0;
    for (int c = 0; c < 6; c++)
    {
        if (fabsf(params->hsl_hue_shift[c]) > 0.01f ||
            fabsf(params->hsl_sat_shift[c]) > 0.01f ||
            fabsf(params->hsl_lum_shift[c]) > 0.01f)
        {
            has_hsl = 1;
            break;
        }
    }
    if (has_hsl)
    {
        for (int i = 0; i < n; i++)
        {
            float h, s, l;
            rgb_to_hsl(R[i], G[i], B[i], &h, &s, &l);
            int sl = hue_slice(h);
            h += params->hsl_hue_shift[sl] / 360.0f;
            if (h < 0.0f)
                h += 1.0f;
            if (h > 1.0f)
                h -= 1.0f;
            s = CLAMPF(s + params->hsl_sat_shift[sl] / 100.0f, 0.0f, 1.0f);
            l = CLAMPF(l + params->hsl_lum_shift[sl] / 100.0f, 0.0f, 1.0f);
            hsl_to_rgb(h, s, l, &R[i], &G[i], &B[i]);
        }
    }

    /* ── 10. Sharpening ── */
    if (!fast_preview)
    {
        unsharp_mask_luma(R, G, B, pw, ph, params->sharpening);
    }

    /* ── 11. Gamma encode & pack RGBA8888 ── */
    float gamma = params->output_gamma;
    for (int y = 0; y < h; y++)
    {
        int py = y / scale;
        if (py >= ph)
            py = ph - 1;
        for (int x = 0; x < w; x++)
        {
            int pxs = x / scale;
            if (pxs >= pw)
                pxs = pw - 1;
            int si = py * pw + pxs;
            uint8_t r8, g8, b8;
            if (gamma > 0.01f)
            {
                r8 = (uint8_t)(powf(CLAMPF(R[si], 0.0f, 1.0f), 1.0f / gamma) * 255.0f + 0.5f);
                g8 = (uint8_t)(powf(CLAMPF(G[si], 0.0f, 1.0f), 1.0f / gamma) * 255.0f + 0.5f);
                b8 = (uint8_t)(powf(CLAMPF(B[si], 0.0f, 1.0f), 1.0f / gamma) * 255.0f + 0.5f);
            }
            else
            {
                r8 = linear_to_srgb8(R[si]);
                g8 = linear_to_srgb8(G[si]);
                b8 = linear_to_srgb8(B[si]);
            }
            uint8_t *px = out->pixels + ((size_t)y * w + x) * 4;
            px[0] = r8;
            px[1] = g8;
            px[2] = b8;
            px[3] = 0xFF;
        }
    }

    free(R);
    free(G);
    free(B);
    return 0;
}

int er_process(const er_raw_image_t *src,
               const er_params_t *params,
               er_output_t *out)
{
    return er_process_ex(src, params, out, 0);
}

void er_output_free(er_output_t *out)
{
    if (!out)
        return;
    free(out->pixels);
    out->pixels = NULL;
    out->width = out->height = out->stride = 0;
}
