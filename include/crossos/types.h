/**
 * crossos/types.h  –  Common types shared across the entire CrossOS SDK.
 *
 * All structs and enumerations used by the public API are declared here so
 * that individual subsystem headers can stay lean and avoid circular
 * inclusion.
 */

#ifndef CROSSOS_TYPES_H
#define CROSSOS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Return codes ──────────────────────────────────────────────────────── */

typedef enum crossos_result {
    CROSSOS_OK            =  0, /**< Success                               */
    CROSSOS_ERR_INIT      = -1, /**< Platform initialisation failed        */
    CROSSOS_ERR_DISPLAY   = -2, /**< Could not open/connect to a display   */
    CROSSOS_ERR_WINDOW    = -3, /**< Window creation / manipulation error  */
    CROSSOS_ERR_OOM       = -4, /**< Out of memory                         */
    CROSSOS_ERR_UNSUPPORT = -5, /**< Feature not supported on this platform*/
    CROSSOS_ERR_PARAM     = -6, /**< Invalid parameter                     */
    CROSSOS_ERR_IO        = -7, /**< File system error                     */
    CROSSOS_ERR_NETWORK   = -8, /**< Network / HTTP request error          */
    CROSSOS_ERR_AUDIO     = -9, /**< Audio subsystem error                 */
} crossos_result_t;

/* ── Opaque handle types ──────────────────────────────────────────────── */

/** Opaque handle representing a top-level window / activity surface. */
typedef struct crossos_window crossos_window_t;

/** Opaque handle representing a pixel surface tied to a window. */
typedef struct crossos_surface crossos_surface_t;

/* ── Pixel format ─────────────────────────────────────────────────────── */

typedef enum crossos_pixel_format {
    CROSSOS_PIXEL_FMT_RGBA8888 = 0, /**< 8-bit per channel, R-G-B-A        */
    CROSSOS_PIXEL_FMT_BGRA8888,     /**< 8-bit per channel, B-G-R-A (Win32)*/
    CROSSOS_PIXEL_FMT_RGB565,       /**< 16-bit packed: 5 R, 6 G, 5 B      */
} crossos_pixel_format_t;

/* ── Geometry ─────────────────────────────────────────────────────────── */

typedef struct crossos_point {
    float x;
    float y;
} crossos_point_t;

typedef struct crossos_size {
    int width;
    int height;
} crossos_size_t;

typedef struct crossos_rect {
    int x;
    int y;
    int width;
    int height;
} crossos_rect_t;

/* ── Input ────────────────────────────────────────────────────────────── */

/** Maximum simultaneous touch points tracked by the SDK. */
#define CROSSOS_MAX_TOUCH_POINTS 10

/** A single touch contact point. */
typedef struct crossos_touch_point {
    int   id;        /**< Unique finger/stylus identifier (platform-assigned) */
    float x;         /**< X position in window-local pixels                   */
    float y;         /**< Y position in window-local pixels                   */
    float pressure;  /**< Normalised pressure [0.0 – 1.0]; 1.0 if unavailable */
} crossos_touch_point_t;

/** Modifier keys bitmask (OR-combination). */
typedef enum crossos_key_mod {
    CROSSOS_MOD_NONE  = 0x00,
    CROSSOS_MOD_SHIFT = 0x01,
    CROSSOS_MOD_CTRL  = 0x02,
    CROSSOS_MOD_ALT   = 0x04,
    CROSSOS_MOD_SUPER = 0x08, /**< Windows key / Command key */
} crossos_key_mod_t;

/* ── Event system ─────────────────────────────────────────────────────── */

typedef enum crossos_event_type {
    CROSSOS_EVENT_NONE = 0,

    /* Application lifecycle */
    CROSSOS_EVENT_QUIT,           /**< User / OS requested application quit */

    /* Window events */
    CROSSOS_EVENT_WINDOW_CLOSE,   /**< Window close button pressed          */
    CROSSOS_EVENT_WINDOW_RESIZE,  /**< Window was resized                   */
    CROSSOS_EVENT_WINDOW_FOCUS,   /**< Window gained input focus            */
    CROSSOS_EVENT_WINDOW_BLUR,    /**< Window lost input focus              */

    /* Keyboard events */
    CROSSOS_EVENT_KEY_DOWN,       /**< Physical key pressed                 */
    CROSSOS_EVENT_KEY_UP,         /**< Physical key released                */

    /* Pointer (mouse / trackpad / stylus) events */
    CROSSOS_EVENT_POINTER_DOWN,   /**< Pointer button pressed               */
    CROSSOS_EVENT_POINTER_UP,     /**< Pointer button released              */
    CROSSOS_EVENT_POINTER_MOVE,   /**< Pointer moved (no button required)   */
    CROSSOS_EVENT_POINTER_SCROLL, /**< Scroll wheel / two-finger scroll     */

    /* Touch events (multi-touch surface) */
    CROSSOS_EVENT_TOUCH_BEGIN,    /**< New finger(s) placed on surface      */
    CROSSOS_EVENT_TOUCH_UPDATE,   /**< Existing finger(s) moved             */
    CROSSOS_EVENT_TOUCH_END,      /**< Finger(s) lifted from surface        */
    CROSSOS_EVENT_TOUCH_CANCEL,   /**< Touch sequence cancelled by OS       */

    /* Text input */
    CROSSOS_EVENT_CHAR,           /**< Unicode codepoint typed by user      */

    /* File drop */
    CROSSOS_EVENT_DROP_FILES,     /**< User dragged files onto the window   */
} crossos_event_type_t;

/** Payload for CROSSOS_EVENT_WINDOW_RESIZE. */
typedef struct crossos_event_resize {
    int width;
    int height;
} crossos_event_resize_t;

/** Payload for CROSSOS_EVENT_KEY_DOWN / CROSSOS_EVENT_KEY_UP. */
typedef struct crossos_event_key {
    int               keycode;  /**< Platform-independent virtual keycode  */
    int               scancode; /**< Hardware scancode                     */
    crossos_key_mod_t mods;     /**< Active modifier keys                  */
    int               repeat;   /**< Non-zero if this is a key-repeat event*/
} crossos_event_key_t;

/** Payload for pointer events. */
typedef struct crossos_event_pointer {
    float x;      /**< Cursor X in window-local pixels  */
    float y;      /**< Cursor Y in window-local pixels  */
    int   button; /**< Button index (1=left,2=mid,3=right); 0 for MOVE */
    float scroll_x; /**< Horizontal scroll delta (POINTER_SCROLL only)  */
    float scroll_y; /**< Vertical scroll delta   (POINTER_SCROLL only)  */
} crossos_event_pointer_t;

/** Payload for touch events. */
typedef struct crossos_event_touch {
    int                   count;
    crossos_touch_point_t points[CROSSOS_MAX_TOUCH_POINTS];
} crossos_event_touch_t;

/** Payload for CROSSOS_EVENT_CHAR. */
typedef struct crossos_event_char {
    unsigned codepoint; /**< Unicode codepoint (Basic Multilingual Plane) */
} crossos_event_char_t;

/**
 * Payload for CROSSOS_EVENT_DROP_FILES.
 * `paths` points into a platform-managed static buffer and is only valid
 * until the next call to crossos_poll_event().  Copy any paths you need.
 */
#define CROSSOS_DROP_FILES_MAX 32
typedef struct crossos_event_drop {
    int           count;
    const char  **paths; /**< Array of `count` NUL-terminated UTF-8 paths */
} crossos_event_drop_t;

/** Tagged-union event structure delivered to the application. */
typedef struct crossos_event {
    crossos_event_type_t   type;
    crossos_window_t      *window; /**< Window that received the event; may be NULL */
    union {
        crossos_event_resize_t  resize;
        crossos_event_key_t     key;
        crossos_event_pointer_t pointer;
        crossos_event_touch_t   touch;
        crossos_event_char_t    character;
        crossos_event_drop_t    drop;
    };
} crossos_event_t;

/** Application-supplied event callback signature. */
typedef void (*crossos_event_cb_t)(const crossos_event_t *event, void *user_data);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_TYPES_H */
