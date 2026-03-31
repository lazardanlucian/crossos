/* _GNU_SOURCE is required on Linux to prevent ALSA headers from
 * redefining struct timespec (already declared in <time.h> / <bits/types/>)
 * and to expose usleep() from <unistd.h> under strict C11. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#include <crossos/audio.h>

#include <stdlib.h>

extern void crossos__set_error(const char *fmt, ...);

#if defined(__ANDROID__)

#include <android_native_app_glue.h>
#include <jni.h>

extern struct android_app *s_app;

static jobject s_tone_generator = NULL;

static crossos_result_t crossos__android_get_env(JNIEnv **out_env, int *out_attached)
{
    if (!out_env || !out_attached || !s_app || !s_app->activity || !s_app->activity->vm) {
        crossos__set_error("sound_play_file: Android activity/VM not available");
        return CROSSOS_ERR_AUDIO;
    }

    JavaVM *vm = s_app->activity->vm;
    JNIEnv *env = NULL;
    *out_attached = 0;

    jint get_rc = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (get_rc == JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) {
            crossos__set_error("sound_play_file: failed to attach JNI thread");
            return CROSSOS_ERR_AUDIO;
        }
        *out_attached = 1;
    } else if (get_rc != JNI_OK) {
        crossos__set_error("sound_play_file: failed to obtain JNIEnv");
        return CROSSOS_ERR_AUDIO;
    }

    *out_env = env;
    return CROSSOS_OK;
}

crossos_result_t crossos_sound_play_file(const char *path)
{
    if (!path) {
        crossos__set_error("sound_play_file: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    JNIEnv *env = NULL;
    int attached = 0;
    crossos_result_t env_rc = crossos__android_get_env(&env, &attached);
    if (env_rc != CROSSOS_OK) {
        return env_rc;
    }

    jclass tone_cls = (*env)->FindClass(env, "android/media/ToneGenerator");
    if (!tone_cls) {
        crossos__set_error("sound_play_file: ToneGenerator class not found");
        if (attached) {
            (*s_app->activity->vm)->DetachCurrentThread(s_app->activity->vm);
        }
        return CROSSOS_ERR_AUDIO;
    }

    if (!s_tone_generator) {
        jmethodID ctor = (*env)->GetMethodID(env, tone_cls, "<init>", "(II)V");
        if (!ctor) {
            crossos__set_error("sound_play_file: ToneGenerator ctor not found");
            if (attached) {
                (*s_app->activity->vm)->DetachCurrentThread(s_app->activity->vm);
            }
            return CROSSOS_ERR_AUDIO;
        }

        jobject local_tone = (*env)->NewObject(env, tone_cls, ctor, 3, 100);
        if (!local_tone) {
            crossos__set_error("sound_play_file: failed to create ToneGenerator");
            if (attached) {
                (*s_app->activity->vm)->DetachCurrentThread(s_app->activity->vm);
            }
            return CROSSOS_ERR_AUDIO;
        }

        s_tone_generator = (*env)->NewGlobalRef(env, local_tone);
        (*env)->DeleteLocalRef(env, local_tone);

        if (!s_tone_generator) {
            crossos__set_error("sound_play_file: failed to create global tone ref");
            if (attached) {
                (*s_app->activity->vm)->DetachCurrentThread(s_app->activity->vm);
            }
            return CROSSOS_ERR_AUDIO;
        }
    }

    jmethodID start_tone = (*env)->GetMethodID(env, tone_cls, "startTone", "(II)Z");
    if (!start_tone) {
        crossos__set_error("sound_play_file: startTone method not found");
        if (attached) {
            (*s_app->activity->vm)->DetachCurrentThread(s_app->activity->vm);
        }
        return CROSSOS_ERR_AUDIO;
    }

    /* Use TONE_PROP_BEEP with ~200ms duration as a portable ding. */
    jboolean ok = (*env)->CallBooleanMethod(env, s_tone_generator, start_tone, 24, 200);

    if (attached) {
        (*s_app->activity->vm)->DetachCurrentThread(s_app->activity->vm);
    }

    if (!ok) {
        crossos__set_error("sound_play_file: ToneGenerator startTone failed");
        return CROSSOS_ERR_AUDIO;
    }

    return CROSSOS_OK;
}

void crossos_sound_stop(void)
{
    JNIEnv *env = NULL;
    int attached = 0;

    if (!s_tone_generator) {
        return;
    }

    if (crossos__android_get_env(&env, &attached) != CROSSOS_OK) {
        return;
    }

    jclass tone_cls = (*env)->FindClass(env, "android/media/ToneGenerator");
    if (!tone_cls) {
        if (attached) {
            (*s_app->activity->vm)->DetachCurrentThread(s_app->activity->vm);
        }
        return;
    }

    jmethodID stop_tone = (*env)->GetMethodID(env, tone_cls, "stopTone", "()V");
    jmethodID release_tone = (*env)->GetMethodID(env, tone_cls, "release", "()V");

    if (stop_tone) {
        (*env)->CallVoidMethod(env, s_tone_generator, stop_tone);
    }
    if (release_tone) {
        (*env)->CallVoidMethod(env, s_tone_generator, release_tone);
    }

    (*env)->DeleteGlobalRef(env, s_tone_generator);
    s_tone_generator = NULL;

    if (attached) {
        (*s_app->activity->vm)->DetachCurrentThread(s_app->activity->vm);
    }
}

/* ---- PCM streaming via AAudio (API 26+) ---- */
#include <aaudio/AAudio.h>
#include <string.h>

struct crossos_audio_context {
    AAudioStream *stream;
    crossos_audio_callback_t cb;
    void *user_data;
    volatile int paused;
};

static aaudio_data_callback_result_t crossos__aaudio_cb(
    AAudioStream *stream, void *user_data, void *audio_data, int32_t num_frames)
{
    (void)stream;
    struct crossos_audio_context *ctx = user_data;
    float *out = audio_data;
    if (ctx->paused) {
        memset(out, 0, (size_t)num_frames * 2 * sizeof(float));
    } else {
        ctx->cb(out, (int)num_frames, ctx->user_data);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

crossos_result_t crossos_audio_open(int sample_rate, int buffer_frames,
                                    crossos_audio_callback_t callback,
                                    void *user_data,
                                    crossos_audio_context_t **out_ctx)
{
    if (!callback || !out_ctx) {
        crossos__set_error("audio_open: invalid parameters");
        return CROSSOS_ERR_PARAM;
    }

    struct crossos_audio_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { crossos__set_error("audio_open: out of memory"); return CROSSOS_ERR_AUDIO; }

    ctx->cb        = callback;
    ctx->user_data = user_data;

    AAudioStreamBuilder *builder = NULL;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
        free(ctx);
        crossos__set_error("audio_open: AAudio_createStreamBuilder failed");
        return CROSSOS_ERR_AUDIO;
    }

    if (buffer_frames > 0)
        AAudioStreamBuilder_setBufferCapacityInFrames(builder, buffer_frames);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSampleRate(builder, sample_rate);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setDataCallback(builder, crossos__aaudio_cb, ctx);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);

    aaudio_result_t rc = AAudioStreamBuilder_openStream(builder, &ctx->stream);
    AAudioStreamBuilder_delete(builder);

    if (rc != AAUDIO_OK) {
        free(ctx);
        crossos__set_error("audio_open: AAudioStreamBuilder_openStream failed: %d", rc);
        return CROSSOS_ERR_AUDIO;
    }

    if (AAudioStream_requestStart(ctx->stream) != AAUDIO_OK) {
        AAudioStream_close(ctx->stream);
        free(ctx);
        crossos__set_error("audio_open: AAudioStream_requestStart failed");
        return CROSSOS_ERR_AUDIO;
    }

    *out_ctx = (crossos_audio_context_t *)ctx;
    return CROSSOS_OK;
}

void crossos_audio_set_paused(crossos_audio_context_t *ctx, int paused)
{
    if (!ctx) return;
    ((struct crossos_audio_context *)ctx)->paused = paused;
}

void crossos_audio_close(crossos_audio_context_t *ctx)
{
    if (!ctx) return;
    struct crossos_audio_context *c = (struct crossos_audio_context *)ctx;
    AAudioStream_requestStop(c->stream);
    AAudioStream_close(c->stream);
    free(c);
}

void crossos_audio_beep(int freq_hz, int duration_ms)
{
    /* Reuse ToneGenerator for a quick beep on Android. */
    (void)freq_hz; /* ToneGenerator doesn't support arbitrary frequency. */
    JNIEnv *env = NULL;
    int attached = 0;
    if (crossos__android_get_env(&env, &attached) != CROSSOS_OK) return;

    jclass cls = (*env)->FindClass(env, "android/media/ToneGenerator");
    if (!cls) goto detach;

    jobject tg = s_tone_generator;
    int local_tg = 0;
    if (!tg) {
        jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "(II)V");
        if (!ctor) goto detach;
        tg = (*env)->NewObject(env, cls, ctor, 3, 100);
        if (!tg) goto detach;
        local_tg = 1;
    }

    jmethodID start = (*env)->GetMethodID(env, cls, "startTone", "(II)Z");
    if (start) {
        (*env)->CallBooleanMethod(env, tg, start, 25 /* TONE_PROP_BEEP2 */, duration_ms);
    }

    if (local_tg) {
        jmethodID stop = (*env)->GetMethodID(env, cls, "stopTone", "()V");
        jmethodID rel  = (*env)->GetMethodID(env, cls, "release",  "()V");
        if (stop) (*env)->CallVoidMethod(env, tg, stop);
        if (rel)  (*env)->CallVoidMethod(env, tg, rel);
        (*env)->DeleteLocalRef(env, tg);
    }

detach:
    if (attached) (*s_app->activity->vm)->DetachCurrentThread(s_app->activity->vm);
}

#elif defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#include <string.h>
#include <math.h>

/* ---- PCM streaming via WinMM waveOut double-buffering ---- */

struct crossos_audio_context {
    HWAVEOUT hwo;
    WAVEHDR  hdrs[2];
    short   *bufs[2];
    float   *float_buf;
    int      buf_frames;
    crossos_audio_callback_t cb;
    void    *user_data;
    volatile LONG paused;
    volatile LONG closing;
    volatile LONG cur;
};

static void CALLBACK crossos__wave_proc(HWAVEOUT hwo, UINT msg,
                                        DWORD_PTR inst,
                                        DWORD_PTR p1, DWORD_PTR p2)
{
    (void)p1; (void)p2;
    if (msg != WOM_DONE) return;
    struct crossos_audio_context *ctx = (struct crossos_audio_context *)inst;
    if (ctx->closing) return;

    LONG idx = InterlockedIncrement(&ctx->cur) & 1;
    WAVEHDR *hdr = &ctx->hdrs[idx];

    if (!ctx->paused) {
        ctx->cb(ctx->float_buf, ctx->buf_frames, ctx->user_data);
        short *dst = ctx->bufs[idx];
        for (int i = 0; i < ctx->buf_frames * 2; i++) {
            float f = ctx->float_buf[i];
            if (f >  1.f) f =  1.f;
            if (f < -1.f) f = -1.f;
            dst[i] = (short)(f * 32767.f);
        }
    } else {
        memset(ctx->bufs[idx], 0, (size_t)ctx->buf_frames * 2 * sizeof(short));
    }

    waveOutUnprepareHeader(hwo, hdr, sizeof(WAVEHDR));
    waveOutPrepareHeader(hwo, hdr, sizeof(WAVEHDR));
    waveOutWrite(hwo, hdr, sizeof(WAVEHDR));
}

crossos_result_t crossos_audio_open(int sample_rate, int buffer_frames,
                                    crossos_audio_callback_t callback,
                                    void *user_data,
                                    crossos_audio_context_t **out_ctx)
{
    if (!callback || !out_ctx) {
        crossos__set_error("audio_open: invalid parameters");
        return CROSSOS_ERR_PARAM;
    }
    if (buffer_frames <= 0 || buffer_frames > 65536) buffer_frames = 512;

    struct crossos_audio_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { crossos__set_error("audio_open: out of memory"); return CROSSOS_ERR_AUDIO; }

    ctx->buf_frames = buffer_frames;
    ctx->cb = callback;
    ctx->user_data = user_data;
    ctx->float_buf = calloc((size_t)buffer_frames * 2, sizeof(float));
    if (!ctx->float_buf) { free(ctx); crossos__set_error("audio_open: out of memory"); return CROSSOS_ERR_AUDIO; }

    for (int i = 0; i < 2; i++) {
        ctx->bufs[i] = calloc((size_t)buffer_frames * 2, sizeof(short));
        if (!ctx->bufs[i]) {
            free(ctx->float_buf);
            for (int j = 0; j < i; j++) free(ctx->bufs[j]);
            free(ctx);
            crossos__set_error("audio_open: out of memory");
            return CROSSOS_ERR_AUDIO;
        }
    }

    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = (DWORD)sample_rate;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = (WORD)(wfx.nChannels * wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT rc = waveOutOpen(&ctx->hwo, WAVE_MAPPER, &wfx,
                              (DWORD_PTR)crossos__wave_proc,
                              (DWORD_PTR)ctx, CALLBACK_FUNCTION);
    if (rc != MMSYSERR_NOERROR) {
        free(ctx->float_buf);
        for (int i = 0; i < 2; i++) free(ctx->bufs[i]);
        free(ctx);
        crossos__set_error("audio_open: waveOutOpen failed (%u)", rc);
        return CROSSOS_ERR_AUDIO;
    }

    /* Prime both buffers. */
    for (int i = 0; i < 2; i++) {
        WAVEHDR *hdr = &ctx->hdrs[i];
        hdr->lpData         = (LPSTR)ctx->bufs[i];
        hdr->dwBufferLength = (DWORD)(buffer_frames * 2 * sizeof(short));
        ctx->cb(ctx->float_buf, buffer_frames, ctx->user_data);
        for (int j = 0; j < buffer_frames * 2; j++) {
            float f = ctx->float_buf[j];
            if (f >  1.f) f =  1.f;
            if (f < -1.f) f = -1.f;
            ctx->bufs[i][j] = (short)(f * 32767.f);
        }
        waveOutPrepareHeader(ctx->hwo, hdr, sizeof(WAVEHDR));
        waveOutWrite(ctx->hwo, hdr, sizeof(WAVEHDR));
    }

    *out_ctx = (crossos_audio_context_t *)ctx;
    return CROSSOS_OK;
}

void crossos_audio_set_paused(crossos_audio_context_t *ctx, int paused)
{
    if (!ctx) return;
    struct crossos_audio_context *c = (struct crossos_audio_context *)ctx;
    InterlockedExchange(&c->paused, paused ? 1 : 0);
}

void crossos_audio_close(crossos_audio_context_t *ctx)
{
    if (!ctx) return;
    struct crossos_audio_context *c = (struct crossos_audio_context *)ctx;
    InterlockedExchange(&c->closing, 1);
    waveOutReset(c->hwo);
    for (int i = 0; i < 2; i++)
        waveOutUnprepareHeader(c->hwo, &c->hdrs[i], sizeof(WAVEHDR));
    waveOutClose(c->hwo);
    free(c->float_buf);
    for (int i = 0; i < 2; i++) free(c->bufs[i]);
    free(c);
}

void crossos_audio_beep(int freq_hz, int duration_ms)
{
    Beep((DWORD)freq_hz, (DWORD)duration_ms);
}

crossos_result_t crossos_sound_play_file(const char *path)
{
    if (!path) {
        crossos__set_error("sound_play_file: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    if (!PlaySoundA(path, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
        crossos__set_error("sound_play_file: PlaySound failed for '%s'", path);
        return CROSSOS_ERR_AUDIO;
    }

    return CROSSOS_OK;
}

void crossos_sound_stop(void)
{
    PlaySoundA(NULL, NULL, 0);
}

#elif defined(__linux__) && !defined(__ANDROID__)

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

#ifdef CROSSOS_HAS_ALSA
#include <alsa/asoundlib.h>
#include <pthread.h>

/* ---- PCM streaming via ALSA background thread ---- */

struct crossos_audio_context {
    snd_pcm_t *pcm;
    pthread_t  thread;
    volatile int running;
    volatile int paused;
    crossos_audio_callback_t cb;
    void    *user_data;
    int      buf_frames;
    float   *float_buf;
    short   *pcm_buf;
};

static void *crossos__alsa_thread(void *arg)
{
    struct crossos_audio_context *ctx = arg;
    while (ctx->running) {
        if (ctx->paused) {
            usleep(10000);
            continue;
        }
        ctx->cb(ctx->float_buf, ctx->buf_frames, ctx->user_data);
        for (int i = 0; i < ctx->buf_frames * 2; i++) {
            float f = ctx->float_buf[i];
            if (f >  1.f) f =  1.f;
            if (f < -1.f) f = -1.f;
            ctx->pcm_buf[i] = (short)(f * 32767.f);
        }
        int frames = ctx->buf_frames;
        short *ptr = ctx->pcm_buf;
        while (frames > 0 && ctx->running) {
            int rc = snd_pcm_writei(ctx->pcm, ptr, (snd_pcm_uframes_t)frames);
            if (rc == -EPIPE) {
                snd_pcm_prepare(ctx->pcm);
            } else if (rc < 0) {
                if (snd_pcm_recover(ctx->pcm, rc, 0) < 0) break;
            } else {
                frames -= rc;
                ptr += rc * 2;
            }
        }
    }
    return NULL;
}

crossos_result_t crossos_audio_open(int sample_rate, int buffer_frames,
                                    crossos_audio_callback_t callback,
                                    void *user_data,
                                    crossos_audio_context_t **out_ctx)
{
    if (!callback || !out_ctx) {
        crossos__set_error("audio_open: invalid parameters");
        return CROSSOS_ERR_PARAM;
    }
    if (buffer_frames <= 0 || buffer_frames > 65536) buffer_frames = 512;

    struct crossos_audio_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { crossos__set_error("audio_open: out of memory"); return CROSSOS_ERR_AUDIO; }

    ctx->cb = callback;
    ctx->user_data = user_data;
    ctx->buf_frames = buffer_frames;
    ctx->float_buf = calloc((size_t)buffer_frames * 2, sizeof(float));
    ctx->pcm_buf   = calloc((size_t)buffer_frames * 2, sizeof(short));

    if (!ctx->float_buf || !ctx->pcm_buf) {
        free(ctx->float_buf); free(ctx->pcm_buf); free(ctx);
        crossos__set_error("audio_open: out of memory");
        return CROSSOS_ERR_AUDIO;
    }

    int rc = snd_pcm_open(&ctx->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        free(ctx->float_buf); free(ctx->pcm_buf); free(ctx);
        crossos__set_error("audio_open: snd_pcm_open failed: %s", snd_strerror(rc));
        return CROSSOS_ERR_AUDIO;
    }

    rc = snd_pcm_set_params(ctx->pcm,
                            SND_PCM_FORMAT_S16_LE,
                            SND_PCM_ACCESS_RW_INTERLEAVED,
                            2, (unsigned int)sample_rate,
                            1,    /* allow resampling */
                            40000 /* 40ms latency */);
    if (rc < 0) {
        snd_pcm_close(ctx->pcm);
        free(ctx->float_buf); free(ctx->pcm_buf); free(ctx);
        crossos__set_error("audio_open: snd_pcm_set_params failed: %s", snd_strerror(rc));
        return CROSSOS_ERR_AUDIO;
    }

    ctx->running = 1;
    if (pthread_create(&ctx->thread, NULL, crossos__alsa_thread, ctx) != 0) {
        ctx->running = 0;
        snd_pcm_close(ctx->pcm);
        free(ctx->float_buf); free(ctx->pcm_buf); free(ctx);
        crossos__set_error("audio_open: pthread_create failed");
        return CROSSOS_ERR_AUDIO;
    }

    *out_ctx = (crossos_audio_context_t *)ctx;
    return CROSSOS_OK;
}

void crossos_audio_set_paused(crossos_audio_context_t *ctx, int paused)
{
    if (!ctx) return;
    ((struct crossos_audio_context *)ctx)->paused = paused;
}

void crossos_audio_close(crossos_audio_context_t *ctx)
{
    if (!ctx) return;
    struct crossos_audio_context *c = (struct crossos_audio_context *)ctx;
    c->running = 0;
    pthread_join(c->thread, NULL);
    snd_pcm_close(c->pcm);
    free(c->float_buf);
    free(c->pcm_buf);
    free(c);
}

void crossos_audio_beep(int freq_hz, int duration_ms)
{
    snd_pcm_t *pcm;
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) return;

    int sample_rate = 44100;
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED,
                           2, (unsigned int)sample_rate, 1, 100000) < 0) {
        snd_pcm_close(pcm);
        return;
    }

    int frames = sample_rate * duration_ms / 1000;
    short *buf = malloc((size_t)frames * 2 * sizeof(short));
    if (!buf) { snd_pcm_close(pcm); return; }

    double phase = 0.0;
    double step  = 2.0 * 3.14159265358979 * freq_hz / sample_rate;
    /* Fade in/out 5ms to avoid clicks. */
    int fade = sample_rate * 5 / 1000;
    for (int i = 0; i < frames; i++) {
        double amp = 0.6;
        if (i < fade) amp *= (double)i / fade;
        else if (i >= frames - fade) amp *= (double)(frames - i) / fade;
        short v = (short)(sin(phase) * amp * 32767.0);
        buf[i * 2]     = v;
        buf[i * 2 + 1] = v;
        phase += step;
    }

    short *ptr = buf;
    int remain = frames;
    while (remain > 0) {
        int rc = snd_pcm_writei(pcm, ptr, (snd_pcm_uframes_t)remain);
        if (rc == -EPIPE) { snd_pcm_prepare(pcm); }
        else if (rc < 0) { break; }
        else { remain -= rc; ptr += rc * 2; }
    }
    snd_pcm_drain(pcm);
    free(buf);
    snd_pcm_close(pcm);
}

#else /* no ALSA — stubbed PCM streaming */

crossos_result_t crossos_audio_open(int sample_rate, int buffer_frames,
                                    crossos_audio_callback_t callback,
                                    void *user_data,
                                    crossos_audio_context_t **out_ctx)
{
    (void)sample_rate; (void)buffer_frames; (void)callback;
    (void)user_data;   (void)out_ctx;
    crossos__set_error("audio_open: ALSA not available");
    return CROSSOS_ERR_UNSUPPORT;
}

void crossos_audio_set_paused(crossos_audio_context_t *ctx, int paused)
{ (void)ctx; (void)paused; }

void crossos_audio_close(crossos_audio_context_t *ctx)
{ (void)ctx; }

void crossos_audio_beep(int freq_hz, int duration_ms)
{
    (void)freq_hz; (void)duration_ms;
    write(STDOUT_FILENO, "\a", 1);
}

#endif /* CROSSOS_HAS_ALSA */

int kill(pid_t pid, int sig);

static pid_t s_audio_pid = -1;

static void crossos__reap_audio_child(void)
{
    if (s_audio_pid <= 0) {
        return;
    }

    int status = 0;
    pid_t rc = waitpid(s_audio_pid, &status, WNOHANG);
    if (rc == s_audio_pid) {
        s_audio_pid = -1;
    }
}

crossos_result_t crossos_sound_play_file(const char *path)
{
    if (!path) {
        crossos__set_error("sound_play_file: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    crossos__reap_audio_child();

    if (s_audio_pid > 0) {
        kill(s_audio_pid, SIGTERM);
        waitpid(s_audio_pid, NULL, 0);
        s_audio_pid = -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        crossos__set_error("sound_play_file: fork failed");
        return CROSSOS_ERR_AUDIO;
    }

    if (pid == 0) {
        execlp("aplay", "aplay", "-q", path, (char *)NULL);
        execlp("paplay", "paplay", path, (char *)NULL);
        _exit(127);
    }

    s_audio_pid = pid;
    return CROSSOS_OK;
}

void crossos_sound_stop(void)
{
    crossos__reap_audio_child();

    if (s_audio_pid > 0) {
        kill(s_audio_pid, SIGTERM);
        waitpid(s_audio_pid, NULL, 0);
        s_audio_pid = -1;
    }
}

#else

crossos_result_t crossos_sound_play_file(const char *path)
{
    (void)path;
    crossos__set_error("sound_play_file: not supported on this platform");
    return CROSSOS_ERR_UNSUPPORT;
}

void crossos_sound_stop(void)
{
}

crossos_result_t crossos_audio_open(int sample_rate, int buffer_frames,
                                    crossos_audio_callback_t callback,
                                    void *user_data,
                                    crossos_audio_context_t **out_ctx)
{
    (void)sample_rate; (void)buffer_frames; (void)callback;
    (void)user_data;   (void)out_ctx;
    crossos__set_error("audio_open: not supported on this platform");
    return CROSSOS_ERR_UNSUPPORT;
}

void crossos_audio_set_paused(crossos_audio_context_t *ctx, int paused)
{ (void)ctx; (void)paused; }

void crossos_audio_close(crossos_audio_context_t *ctx)
{ (void)ctx; }

void crossos_audio_beep(int freq_hz, int duration_ms)
{ (void)freq_hz; (void)duration_ms; }

#endif
