/**
 * core/init.c  –  Platform-agnostic SDK lifecycle and error handling.
 *
 * Each platform backend provides its own crossos__platform_init() and
 * crossos__platform_shutdown() implementations.  This file wires them
 * together and provides the thread-local error string.
 */

#include <crossos/crossos.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ── Internal linkage ─────────────────────────────────────────────────── */

/* Declared in each platform backend. */
extern crossos_result_t crossos__platform_init(void);
extern void             crossos__platform_shutdown(void);

/* ── Thread-local error buffer ────────────────────────────────────────── */

#if defined(_MSC_VER)
#  define CROSSOS_TLS __declspec(thread)
#else
#  define CROSSOS_TLS __thread
#endif

#define CROSSOS_ERR_BUF_SIZE 512

static CROSSOS_TLS char s_error_buf[CROSSOS_ERR_BUF_SIZE];

/* Internal helper – format and store an error message. */
void crossos__set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_error_buf, sizeof(s_error_buf), fmt, ap);
    va_end(ap);
}

const char *crossos_get_error(void)
{
    return s_error_buf;
}

/* ── Quit flag ────────────────────────────────────────────────────────── */

/* Written by crossos_quit(), read by the event-loop helpers in each
 * platform backend.  Declared extern so backends can read it. */
volatile int crossos__quit_requested = 0;

/* ── SDK lifecycle ────────────────────────────────────────────────────── */

crossos_result_t crossos_init(void)
{
    crossos__quit_requested = 0;
    s_error_buf[0] = '\0';
    return crossos__platform_init();
}

void crossos_quit(void)
{
    crossos__quit_requested = 1;
}

void crossos_shutdown(void)
{
    crossos__platform_shutdown();
}

/* ── Version / diagnostics ────────────────────────────────────────────── */

const char *crossos_platform_name(void)
{
#if defined(_WIN32)
    return "windows";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}
