/**
 * crossos/telemetry.h  –  Debug telemetry and analytics for CrossOS.
 *
 * When CROSSOS_TELEMETRY is defined (via the CMake option CROSSOS_TELEMETRY=ON),
 * log messages and analytics events are sent as HTTP POST requests to a
 * configured analytics server.  When the option is off every call and macro
 * compiles away to nothing.
 *
 * Typical usage in a debug build:
 *
 *   crossos_telemetry_init("https://mysite.com/crossos", "device-abc", "1.0");
 *   CROSSOS_LOG_INFO("app", "Application started");
 *   crossos_telemetry_event("render_frame", "{\"fps\":60}");
 *   crossos_telemetry_shutdown();
 *
 * The device_id should be stable across runs (e.g. a UUID persisted in a
 * local config file).  It is used server-side to group log entries by device.
 *
 * Auth is handled automatically: telemetry.c derives a time-windowed token
 * from the compile-time CROSSOS_TELEMETRY_SECRET and sends it with each
 * request so the server can reject unauthenticated payloads.
 */

#ifndef CROSSOS_TELEMETRY_H
#define CROSSOS_TELEMETRY_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Compile-time defaults (override with -DCROSSOS_TELEMETRY_URL_DEFAULT=…) */

#ifndef CROSSOS_TELEMETRY_URL_DEFAULT
#  define CROSSOS_TELEMETRY_URL_DEFAULT ""
#endif

/** Shared secret used to generate per-request auth tokens.
 *  Override at CMake configure time with -DCROSSOS_TELEMETRY_SECRET=<value>. */
#ifndef CROSSOS_TELEMETRY_SECRET_DEFAULT
#  define CROSSOS_TELEMETRY_SECRET_DEFAULT "Xk9m2pQr7vY3E4wA"
#endif

/* ══════════════════════════════════════════════════════════════════════════
 *  ENABLED path – real functions are compiled in (src/core/telemetry.c).
 * ══════════════════════════════════════════════════════════════════════════ */

#if defined(CROSSOS_TELEMETRY) && CROSSOS_TELEMETRY

/**
 * Initialise the telemetry subsystem.
 *
 * Must be called once, after crossos_init(), before any log/event call.
 *
 * @param server_url  Base URL of the analytics endpoint, e.g.
 *                    "https://mysite.com/crossos".  Pass NULL to use the
 *                    compile-time default CROSSOS_TELEMETRY_URL_DEFAULT.
 * @param device_id   Opaque string identifying this device/install.
 *                    Must not be NULL.
 * @param app_version Application version string (e.g. "1.2.0").  May be NULL.
 * @return CROSSOS_OK on success, CROSSOS_ERR_PARAM if device_id is NULL.
 */
crossos_result_t crossos_telemetry_init(const char *server_url,
                                        const char *device_id,
                                        const char *app_version);

/**
 * Flush pending entries and tear down the telemetry subsystem.
 *
 * Call before crossos_shutdown().
 */
void crossos_telemetry_shutdown(void);

/**
 * Send a log entry.
 *
 * @param level   Severity: "debug", "info", "warn", or "error".
 * @param tag     Short category tag, e.g. "renderer" or "network".
 * @param message Human-readable message.
 */
void crossos_telemetry_log(const char *level,
                           const char *tag,
                           const char *message);

/**
 * printf-style variant of crossos_telemetry_log.
 */
void crossos_telemetry_logf(const char *level,
                            const char *tag,
                            const char *fmt, ...);

/**
 * Send an analytics event.
 *
 * @param name       Event name, e.g. "app_start" or "render_frame".
 * @param props_json Optional JSON object string with extra properties, or NULL.
 *                   Example: "{\"fps\":60,\"api\":\"opengl\"}".
 */
void crossos_telemetry_event(const char *name, const char *props_json);

/* ── Convenience macros (log + optional format string) ─────────────────── */

#define CROSSOS_LOG_DEBUG(tag, msg)    crossos_telemetry_log("debug", (tag), (msg))
#define CROSSOS_LOG_INFO(tag, msg)     crossos_telemetry_log("info",  (tag), (msg))
#define CROSSOS_LOG_WARN(tag, msg)     crossos_telemetry_log("warn",  (tag), (msg))
#define CROSSOS_LOG_ERROR(tag, msg)    crossos_telemetry_log("error", (tag), (msg))

#define CROSSOS_LOG_DEBUGF(tag, ...)   crossos_telemetry_logf("debug", (tag), __VA_ARGS__)
#define CROSSOS_LOG_INFOF(tag, ...)    crossos_telemetry_logf("info",  (tag), __VA_ARGS__)
#define CROSSOS_LOG_WARNF(tag, ...)    crossos_telemetry_logf("warn",  (tag), __VA_ARGS__)
#define CROSSOS_LOG_ERRORF(tag, ...)   crossos_telemetry_logf("error", (tag), __VA_ARGS__)

/* ══════════════════════════════════════════════════════════════════════════
 *  DISABLED path – everything compiles away to nothing.
 * ══════════════════════════════════════════════════════════════════════════ */

#else /* CROSSOS_TELEMETRY not enabled */

#define crossos_telemetry_init(url, dev, ver)  (CROSSOS_OK)
#define crossos_telemetry_shutdown()           ((void)0)
#define crossos_telemetry_log(lvl, tag, msg)   ((void)0)
#define crossos_telemetry_event(name, props)   ((void)0)

/* Variadic macro: consumes fmt + all extra args without a call. */
#define crossos_telemetry_logf(lvl, tag, ...)  ((void)0)

#define CROSSOS_LOG_DEBUG(tag, msg)    ((void)0)
#define CROSSOS_LOG_INFO(tag, msg)     ((void)0)
#define CROSSOS_LOG_WARN(tag, msg)     ((void)0)
#define CROSSOS_LOG_ERROR(tag, msg)    ((void)0)

#define CROSSOS_LOG_DEBUGF(tag, ...)   ((void)0)
#define CROSSOS_LOG_INFOF(tag, ...)    ((void)0)
#define CROSSOS_LOG_WARNF(tag, ...)    ((void)0)
#define CROSSOS_LOG_ERRORF(tag, ...)   ((void)0)

#endif /* CROSSOS_TELEMETRY */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_TELEMETRY_H */
