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

#elif defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>

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

#endif
