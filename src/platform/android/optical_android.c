#ifdef __ANDROID__

#include <crossos/crossos.h>

#include "android_internal.h"

#include <jni.h>
#include <stdlib.h>
#include <string.h>

extern void crossos__set_error(const char *fmt, ...);

typedef struct android_backend_job {
    jlong job_id;
} android_backend_job_t;

static jclass s_burner_class = NULL;
static jobject s_activity = NULL;

static jmethodID s_mid_init = NULL;
static jmethodID s_mid_list_devices = NULL;
static jmethodID s_mid_start_burn = NULL;
static jmethodID s_mid_poll_burn = NULL;
static jmethodID s_mid_cancel_burn = NULL;
static jmethodID s_mid_free_burn = NULL;
static jmethodID s_mid_get_last_error = NULL;

static JNIEnv *get_env(int *did_attach)
{
    if (did_attach) {
        *did_attach = 0;
    }

    if (!s_app || !s_app->activity || !s_app->activity->vm) {
        crossos__set_error("Android optical backend: app VM unavailable");
        return NULL;
    }

    JNIEnv *env = NULL;
    JavaVM *vm = s_app->activity->vm;
    jint get_env_rc = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (get_env_rc == JNI_OK) {
        return env;
    }

    if (get_env_rc == JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) {
            crossos__set_error("Android optical backend: AttachCurrentThread failed");
            return NULL;
        }
        if (did_attach) {
            *did_attach = 1;
        }
        return env;
    }

    crossos__set_error("Android optical backend: GetEnv failed");
    return NULL;
}

static void detach_env_if_needed(int did_attach)
{
    if (!did_attach || !s_app || !s_app->activity || !s_app->activity->vm) {
        return;
    }
    (*s_app->activity->vm)->DetachCurrentThread(s_app->activity->vm);
}

static void copy_java_error(JNIEnv *env)
{
    if (!env || !s_burner_class || !s_mid_get_last_error) {
        return;
    }

    jstring jerr = (jstring)(*env)->CallStaticObjectMethod(env, s_burner_class, s_mid_get_last_error);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        crossos__set_error("Android optical backend: Java exception");
        return;
    }

    if (!jerr) {
        crossos__set_error("Android optical backend: unknown error");
        return;
    }

    const char *err = (*env)->GetStringUTFChars(env, jerr, NULL);
    if (err && err[0] != '\0') {
        crossos__set_error("%s", err);
    } else {
        crossos__set_error("Android optical backend: operation failed");
    }
    if (err) {
        (*env)->ReleaseStringUTFChars(env, jerr, err);
    }
    (*env)->DeleteLocalRef(env, jerr);
}

static void parse_device_line(const char *line, crossos_optical_device_t *dev)
{
    char buf[512];
    memset(dev, 0, sizeof(*dev));

    snprintf(buf, sizeof(buf), "%s", line ? line : "");
    char *fields[8] = {0};
    int field_count = 0;

    char *save = NULL;
    char *tok = strtok_r(buf, "\t", &save);
    while (tok && field_count < 8) {
        fields[field_count++] = tok;
        tok = strtok_r(NULL, "\t", &save);
    }

    if (field_count < 2) {
        return;
    }

    snprintf(dev->id, sizeof(dev->id), "%s", fields[0]);
    snprintf(dev->label, sizeof(dev->label), "%s", fields[1]);

    if (field_count >= 8) {
        dev->is_usb = atoi(fields[2]);
        dev->can_read = atoi(fields[3]);
        dev->can_write = atoi(fields[4]);
        dev->has_media = atoi(fields[5]);
        dev->media_capacity_bytes = (uint64_t)strtoull(fields[6], NULL, 10);
        dev->media_free_bytes = (uint64_t)strtoull(fields[7], NULL, 10);
    }
}

static crossos_result_t backend_list_devices(crossos_optical_device_t *devices,
                                             size_t max_devices,
                                             size_t *out_count,
                                             void *user_data)
{
    (void)user_data;

    if (!out_count) {
        crossos__set_error("android backend list_devices: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    *out_count = 0;
    if (max_devices > 0 && !devices) {
        crossos__set_error("android backend list_devices: devices buffer is NULL");
        return CROSSOS_ERR_PARAM;
    }

    int did_attach = 0;
    JNIEnv *env = get_env(&did_attach);
    if (!env) {
        return CROSSOS_ERR_INIT;
    }

    jstring jresp = (jstring)(*env)->CallStaticObjectMethod(env,
                                                             s_burner_class,
                                                             s_mid_list_devices,
                                                             s_activity);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        crossos__set_error("android backend list_devices: Java exception");
        detach_env_if_needed(did_attach);
        return CROSSOS_ERR_IO;
    }

    if (!jresp) {
        copy_java_error(env);
        detach_env_if_needed(did_attach);
        return CROSSOS_ERR_IO;
    }

    const char *resp = (*env)->GetStringUTFChars(env, jresp, NULL);
    if (!resp) {
        (*env)->DeleteLocalRef(env, jresp);
        detach_env_if_needed(did_attach);
        crossos__set_error("android backend list_devices: bad response");
        return CROSSOS_ERR_IO;
    }

    char *copy = strdup(resp);
    (*env)->ReleaseStringUTFChars(env, jresp, resp);
    (*env)->DeleteLocalRef(env, jresp);

    if (!copy) {
        detach_env_if_needed(did_attach);
        crossos__set_error("android backend list_devices: out of memory");
        return CROSSOS_ERR_OOM;
    }

    char *save = NULL;
    char *line = strtok_r(copy, "\n", &save);
    while (line && *out_count < max_devices) {
        parse_device_line(line, &devices[*out_count]);
        if (devices[*out_count].id[0] != '\0') {
            (*out_count)++;
        }
        line = strtok_r(NULL, "\n", &save);
    }

    free(copy);
    detach_env_if_needed(did_attach);
    return CROSSOS_OK;
}

static crossos_result_t backend_burn_start(const char *const *paths,
                                           size_t path_count,
                                           const char *target_device_id,
                                           void **out_backend_job,
                                           void *user_data)
{
    (void)user_data;

    if (!paths || path_count == 0 || !out_backend_job) {
        crossos__set_error("android backend burn_start: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    int did_attach = 0;
    JNIEnv *env = get_env(&did_attach);
    if (!env) {
        return CROSSOS_ERR_INIT;
    }

    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    if (!string_class) {
        (*env)->ExceptionClear(env);
        detach_env_if_needed(did_attach);
        crossos__set_error("android backend burn_start: cannot resolve String class");
        return CROSSOS_ERR_INIT;
    }

    jobjectArray jpaths = (*env)->NewObjectArray(env,
                                                  (jsize)path_count,
                                                  string_class,
                                                  NULL);
    (*env)->DeleteLocalRef(env, string_class);
    if (!jpaths) {
        detach_env_if_needed(did_attach);
        crossos__set_error("android backend burn_start: path array allocation failed");
        return CROSSOS_ERR_OOM;
    }

    for (size_t i = 0; i < path_count; i++) {
        jstring jpath = (*env)->NewStringUTF(env, paths[i] ? paths[i] : "");
        if (!jpath) {
            (*env)->DeleteLocalRef(env, jpaths);
            detach_env_if_needed(did_attach);
            crossos__set_error("android backend burn_start: path string allocation failed");
            return CROSSOS_ERR_OOM;
        }
        (*env)->SetObjectArrayElement(env, jpaths, (jsize)i, jpath);
        (*env)->DeleteLocalRef(env, jpath);
    }

    jstring jtarget = (*env)->NewStringUTF(env, target_device_id ? target_device_id : "");
    jlong job_id = (*env)->CallStaticLongMethod(env,
                                                s_burner_class,
                                                s_mid_start_burn,
                                                s_activity,
                                                jtarget,
                                                jpaths);

    if (jtarget) {
        (*env)->DeleteLocalRef(env, jtarget);
    }
    (*env)->DeleteLocalRef(env, jpaths);

    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        detach_env_if_needed(did_attach);
        crossos__set_error("android backend burn_start: Java exception");
        return CROSSOS_ERR_IO;
    }

    if (job_id <= 0) {
        copy_java_error(env);
        detach_env_if_needed(did_attach);
        return CROSSOS_ERR_IO;
    }

    android_backend_job_t *job = (android_backend_job_t *)calloc(1, sizeof(*job));
    if (!job) {
        detach_env_if_needed(did_attach);
        crossos__set_error("android backend burn_start: out of memory");
        return CROSSOS_ERR_OOM;
    }

    job->job_id = job_id;
    *out_backend_job = job;
    detach_env_if_needed(did_attach);
    return CROSSOS_OK;
}

static crossos_result_t backend_burn_poll(void *backend_job,
                                          crossos_optical_burn_progress_t *out_progress,
                                          void *user_data)
{
    (void)user_data;

    if (!backend_job || !out_progress) {
        crossos__set_error("android backend burn_poll: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    android_backend_job_t *job = (android_backend_job_t *)backend_job;

    int did_attach = 0;
    JNIEnv *env = get_env(&did_attach);
    if (!env) {
        return CROSSOS_ERR_INIT;
    }

    jstring jresp = (jstring)(*env)->CallStaticObjectMethod(env,
                                                             s_burner_class,
                                                             s_mid_poll_burn,
                                                             (jlong)job->job_id);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        detach_env_if_needed(did_attach);
        crossos__set_error("android backend burn_poll: Java exception");
        return CROSSOS_ERR_IO;
    }

    if (!jresp) {
        copy_java_error(env);
        detach_env_if_needed(did_attach);
        return CROSSOS_ERR_IO;
    }

    const char *resp = (*env)->GetStringUTFChars(env, jresp, NULL);
    if (!resp) {
        (*env)->DeleteLocalRef(env, jresp);
        detach_env_if_needed(did_attach);
        crossos__set_error("android backend burn_poll: bad response");
        return CROSSOS_ERR_IO;
    }

    char buf[512];
    snprintf(buf, sizeof(buf), "%s", resp);
    (*env)->ReleaseStringUTFChars(env, jresp, resp);
    (*env)->DeleteLocalRef(env, jresp);

    memset(out_progress, 0, sizeof(*out_progress));

    char *fields[6] = {0};
    int field_count = 0;
    char *save = NULL;
    char *tok = strtok_r(buf, "\t", &save);
    while (tok && field_count < 6) {
        fields[field_count++] = tok;
        tok = strtok_r(NULL, "\t", &save);
    }

    if (field_count < 6) {
        detach_env_if_needed(did_attach);
        crossos__set_error("android backend burn_poll: malformed response");
        return CROSSOS_ERR_IO;
    }

    out_progress->state = (crossos_optical_burn_state_t)atoi(fields[0]);
    out_progress->percent = (float)atof(fields[1]);
    out_progress->speed_mib_s = (float)atof(fields[2]);
    out_progress->bytes_written = (uint64_t)strtoull(fields[3], NULL, 10);
    out_progress->total_bytes = (uint64_t)strtoull(fields[4], NULL, 10);
    snprintf(out_progress->message, sizeof(out_progress->message), "%s", fields[5]);

    detach_env_if_needed(did_attach);
    return CROSSOS_OK;
}

static crossos_result_t backend_burn_cancel(void *backend_job,
                                            void *user_data)
{
    (void)user_data;

    if (!backend_job) {
        crossos__set_error("android backend burn_cancel: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    android_backend_job_t *job = (android_backend_job_t *)backend_job;

    int did_attach = 0;
    JNIEnv *env = get_env(&did_attach);
    if (!env) {
        return CROSSOS_ERR_INIT;
    }

    jint rc = (*env)->CallStaticIntMethod(env,
                                          s_burner_class,
                                          s_mid_cancel_burn,
                                          (jlong)job->job_id);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        detach_env_if_needed(did_attach);
        crossos__set_error("android backend burn_cancel: Java exception");
        return CROSSOS_ERR_IO;
    }

    detach_env_if_needed(did_attach);
    return rc ? CROSSOS_OK : CROSSOS_ERR_IO;
}

static void backend_burn_free(void *backend_job,
                              void *user_data)
{
    (void)user_data;

    if (!backend_job) {
        return;
    }

    android_backend_job_t *job = (android_backend_job_t *)backend_job;

    int did_attach = 0;
    JNIEnv *env = get_env(&did_attach);
    if (env) {
        (*env)->CallStaticVoidMethod(env,
                                     s_burner_class,
                                     s_mid_free_burn,
                                     (jlong)job->job_id);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        detach_env_if_needed(did_attach);
    }

    free(job);
}

crossos_result_t crossos_android_optical_backend_init(void)
{
    int did_attach = 0;
    JNIEnv *env = get_env(&did_attach);
    if (!env) {
        return CROSSOS_ERR_INIT;
    }

    jobject activity_local = s_app->activity->clazz;
    if (!activity_local) {
        detach_env_if_needed(did_attach);
        crossos__set_error("Android optical backend: activity unavailable");
        return CROSSOS_ERR_INIT;
    }

    if (!s_activity) {
        s_activity = (*env)->NewGlobalRef(env, activity_local);
    }

    if (!s_burner_class) {
        jclass local_cls = (*env)->FindClass(env, "io/crossos/hello/CrossOSUsbBurner");
        if (!local_cls) {
            (*env)->ExceptionClear(env);
            detach_env_if_needed(did_attach);
            crossos__set_error("Android optical backend: cannot find CrossOSUsbBurner class");
            return CROSSOS_ERR_UNSUPPORT;
        }
        s_burner_class = (*env)->NewGlobalRef(env, local_cls);
        (*env)->DeleteLocalRef(env, local_cls);
    }

    s_mid_init = (*env)->GetStaticMethodID(env,
                                           s_burner_class,
                                           "init",
                                           "(Landroid/app/Activity;)Z");
    s_mid_list_devices = (*env)->GetStaticMethodID(env,
                                                   s_burner_class,
                                                   "listDevices",
                                                   "(Landroid/app/Activity;)Ljava/lang/String;");
    s_mid_start_burn = (*env)->GetStaticMethodID(env,
                                                 s_burner_class,
                                                 "startBurn",
                                                 "(Landroid/app/Activity;Ljava/lang/String;[Ljava/lang/String;)J");
    s_mid_poll_burn = (*env)->GetStaticMethodID(env,
                                                s_burner_class,
                                                "pollBurn",
                                                "(J)Ljava/lang/String;");
    s_mid_cancel_burn = (*env)->GetStaticMethodID(env,
                                                  s_burner_class,
                                                  "cancelBurn",
                                                  "(J)I");
    s_mid_free_burn = (*env)->GetStaticMethodID(env,
                                                s_burner_class,
                                                "freeBurn",
                                                "(J)V");
    s_mid_get_last_error = (*env)->GetStaticMethodID(env,
                                                     s_burner_class,
                                                     "getLastError",
                                                     "()Ljava/lang/String;");

    if (!s_mid_init || !s_mid_list_devices || !s_mid_start_burn ||
        !s_mid_poll_burn || !s_mid_cancel_burn || !s_mid_free_burn ||
        !s_mid_get_last_error) {
        detach_env_if_needed(did_attach);
        crossos__set_error("Android optical backend: method lookup failed");
        return CROSSOS_ERR_UNSUPPORT;
    }

    jboolean init_ok = (*env)->CallStaticBooleanMethod(env,
                                                       s_burner_class,
                                                       s_mid_init,
                                                       s_activity);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        detach_env_if_needed(did_attach);
        crossos__set_error("Android optical backend: init Java exception");
        return CROSSOS_ERR_INIT;
    }

    if (!init_ok) {
        copy_java_error(env);
        detach_env_if_needed(did_attach);
        return CROSSOS_ERR_INIT;
    }

    static const crossos_optical_backend_t backend = {
        backend_list_devices,
        backend_burn_start,
        backend_burn_poll,
        backend_burn_cancel,
        backend_burn_free,
    };

    crossos_optical_set_backend(&backend, NULL);

    detach_env_if_needed(did_attach);
    return CROSSOS_OK;
}

void crossos_android_optical_backend_shutdown(void)
{
    int did_attach = 0;
    JNIEnv *env = get_env(&did_attach);

    crossos_optical_set_backend(NULL, NULL);

    if (env) {
        if (s_burner_class) {
            (*env)->DeleteGlobalRef(env, s_burner_class);
            s_burner_class = NULL;
        }
        if (s_activity) {
            (*env)->DeleteGlobalRef(env, s_activity);
            s_activity = NULL;
        }
    }

    detach_env_if_needed(did_attach);
}

#endif /* __ANDROID__ */
