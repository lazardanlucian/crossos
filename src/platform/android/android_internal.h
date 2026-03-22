/**
 * platform/android/android_internal.h
 *
 * Internal header shared by window_android.c and input_android.c.
 * NOT part of the public API; do not include from application code.
 */

#ifndef CROSSOS_ANDROID_INTERNAL_H
#define CROSSOS_ANDROID_INTERNAL_H

#ifdef __ANDROID__

#include <android_native_app_glue.h>

/* Module-level Android app state – defined in window_android.c */
extern struct android_app *s_app;

#endif /* __ANDROID__ */
#endif /* CROSSOS_ANDROID_INTERNAL_H */
