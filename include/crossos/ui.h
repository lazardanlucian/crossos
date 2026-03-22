/**
 * crossos/ui.h  –  Immediate-mode UI toolkit for CrossOS.
 *
 * Design goals:
 *  • Works on software framebuffers (no GPU required).
 *  • Responsive: same code runs on desktop, tablet, and phone.
 *  • Theme-aware: all colors come from a crossos_ui_theme_t.
 *  • Portable: no dynamic allocation inside the library.
 *
 * Usage per frame:
 *   1. Build a crossos_ui_input_t from the current event queue.
 *   2. Call crossos_ui_begin() to initialise a crossos_ui_context_t.
 *   3. Call widget functions; they return 1 when activated / changed.
 *   4. Call crossos_surface_unlock() + crossos_surface_present().
 *   5. Reset pointer_pressed / pointer_released / scroll_dx|dy to 0.
 */

#ifndef CROSSOS_UI_H
#define CROSSOS_UI_H

#include "draw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Theme ──────────────────────────────────────────────────────────── */

typedef struct crossos_ui_theme {
    crossos_color_t bg;             /**< Window / app background            */
    crossos_color_t surface;        /**< Card / panel surface               */
    crossos_color_t surface_alt;    /**< Alternate row background           */
    crossos_color_t surface_hover;  /**< Hovered surface element            */
    crossos_color_t surface_active; /**< Pressed / selected surface         */
    crossos_color_t accent;         /**< Primary action colour              */
    crossos_color_t accent_hover;   /**< Accent on hover                    */
    crossos_color_t accent_dim;     /**< Disabled / dimmed accent           */
    crossos_color_t text;           /**< Primary text                       */
    crossos_color_t text_dim;       /**< Secondary / placeholder text       */
    crossos_color_t text_disabled;  /**< Disabled widget text               */
    crossos_color_t border;         /**< Border / separator colour          */
    crossos_color_t success;        /**< Progress / success indicator       */
    crossos_color_t danger;         /**< Destructive / error colour         */
    crossos_color_t warning;        /**< Warning colour                     */
    int             radius;         /**< Default corner radius (px)         */
    int             spacing;        /**< Base spacing unit (px)             */
} crossos_ui_theme_t;

/** Returns a pointer to the built-in dark theme (GitHub-inspired). */
const crossos_ui_theme_t *crossos_ui_theme_dark(void);

/** Returns a pointer to the built-in light theme. */
const crossos_ui_theme_t *crossos_ui_theme_light(void);

/* ── Input ──────────────────────────────────────────────────────────── */

#define CROSSOS_UI_DROP_MAX 32  /**< Max simultaneous dropped files        */

typedef struct crossos_ui_input {
    /* Pointer / touch */
    int   pointer_x, pointer_y;
    int   pointer_down;         /**< Non-zero while button is held          */
    int   pointer_pressed;      /**< Set to 1 for the frame of press        */
    int   pointer_released;     /**< Set to 1 for the frame of release      */

    /* Scroll wheel / two-finger scroll */
    float scroll_dx;            /**< Horizontal scroll delta (notches)      */
    float scroll_dy;            /**< Vertical scroll delta (notches)        */

    /* Text / keyboard */
    unsigned char_input;        /**< Unicode codepoint typed (0 = none)     */
    int   key_pressed;          /**< Keycode of key pressed this frame (0)  */
    int   key_mods;             /**< Active modifier bitmask                */

    /* OS drag-and-drop */
    int          drop_count;
    const char  *drop_paths[CROSSOS_UI_DROP_MAX]; /**< valid until next poll */
} crossos_ui_input_t;

/* ── Text buffer (caller-owned persistent state) ────────────────────── */

#define CROSSOS_UI_TEXT_MAX 512

typedef struct crossos_ui_text_buf {
    char buf[CROSSOS_UI_TEXT_MAX]; /**< NUL-terminated string               */
    int  len;                      /**< Byte length (excluding NUL)         */
    int  cursor;                   /**< Byte offset of insertion point      */
    int  focused;                  /**< Managed by the UI system            */
} crossos_ui_text_buf_t;

/* ── Scroll state (caller-owned) ────────────────────────────────────── */

typedef struct crossos_ui_scroll {
    float offset;     /**< Current scroll offset in pixels (top of view)   */
    float content_h;  /**< Total scrollable content height in pixels        */
    float view_h;     /**< Visible area height – set by scroll_begin()      */
} crossos_ui_scroll_t;

/* ── Context ────────────────────────────────────────────────────────── */

typedef struct crossos_ui_context {
    const crossos_framebuffer_t *fb;
    crossos_ui_input_t           input;
    crossos_ui_theme_t           theme;
    int                          scale;
    unsigned                     id_counter; /**< Auto-incrementing widget id */
    unsigned                     active_id;  /**< Widget being dragged/held   */
    unsigned                     focus_id;   /**< Keyboard-focused widget     */
    int                          frame;      /**< Frame counter (cursor blink)*/
    /* Internal: scroll state pushed by scroll_begin */
    int                          _clip_active;
    int                          _scroll_ox, _scroll_oy;
} crossos_ui_context_t;

/* ── Layout ─────────────────────────────────────────────────────────── */

typedef struct crossos_ui_layout {
    int x, y, w, h;
    int cursor;
    int gap;
    int horizontal; /**< 1 = row (items placed left→right), 0 = column     */
} crossos_ui_layout_t;

/* ── API ────────────────────────────────────────────────────────────── */

/* ----- Context -------------------------------------------------------- */

/** Initialise (or re-initialise) the context for one frame. */
void crossos_ui_begin(crossos_ui_context_t *ui,
                      const crossos_framebuffer_t *fb,
                      const crossos_ui_input_t *input);

/** Override the theme; must be called after crossos_ui_begin(). */
void crossos_ui_set_theme(crossos_ui_context_t *ui,
                          const crossos_ui_theme_t *theme);

/** Returns 1 (normal DPI) or 2 (hi-DPI: min surface dimension ≥ 1000). */
int crossos_ui_scale_for_surface(const crossos_framebuffer_t *fb);

/* ----- Static elements ------------------------------------------------ */

/** Filled background panel. */
void crossos_ui_panel(crossos_ui_context_t *ui,
                      int x, int y, int w, int h,
                      crossos_color_t color);

/** Text label at (x, y) in the given colour. */
void crossos_ui_label(crossos_ui_context_t *ui,
                      int x, int y,
                      const char *text,
                      crossos_color_t color);

/** Label centred horizontally within [x, x+w] and vertically in [y, y+h]. */
void crossos_ui_label_centered(crossos_ui_context_t *ui,
                                int x, int y, int w, int h,
                                const char *text,
                                crossos_color_t color);

/** Horizontal divider line. */
void crossos_ui_separator(crossos_ui_context_t *ui,
                          int x, int y, int w);

/** Small circular badge drawn at (x, y) showing `count`. */
void crossos_ui_badge(crossos_ui_context_t *ui,
                      int x, int y, int count,
                      crossos_color_t color);

/** Animated spinner (use ui->frame for animation). */
void crossos_ui_spinner(crossos_ui_context_t *ui,
                        int cx, int cy, int size);

/* ----- Interactive widgets -------------------------------------------- */

/** Rounded button; returns 1 on click. */
int crossos_ui_button(crossos_ui_context_t *ui,
                      int x, int y, int w, int h,
                      const char *text,
                      int enabled);

/** Danger-coloured button (red accent). */
int crossos_ui_button_danger(crossos_ui_context_t *ui,
                              int x, int y, int w, int h,
                              const char *text,
                              int enabled);

/** Ghost / outline button (transparent fill). */
int crossos_ui_button_ghost(crossos_ui_context_t *ui,
                             int x, int y, int w, int h,
                             const char *text,
                             int enabled);

/** Selectable list row; returns 1 on click. */
int crossos_ui_selectable(crossos_ui_context_t *ui,
                          int x, int y, int w, int h,
                          const char *text,
                          int selected);

/** Toggleable checkbox; updates *checked; returns 1 when value changes. */
int crossos_ui_checkbox(crossos_ui_context_t *ui,
                        int x, int y,
                        const char *label,
                        int *checked);

/** Radio button option; returns 1 if this option was clicked. */
int crossos_ui_radio(crossos_ui_context_t *ui,
                     int x, int y,
                     const char *label,
                     int selected);

/**
 * Horizontal slider.
 * *value is clamped to [min_v, max_v] and updates while dragging.
 * Returns 1 whenever the value changes.
 */
int crossos_ui_slider(crossos_ui_context_t *ui,
                      int x, int y, int w, int h,
                      float min_v, float max_v,
                      float *value);

/**
 * Single-line text input field.
 * `buf` holds persistent editing state; `placeholder` shown when empty.
 * Returns 1 while the field has focus.
 * Feed char_input / key_pressed from crossos_ui_input_t each frame.
 */
int crossos_ui_text_input(crossos_ui_context_t *ui,
                          int x, int y, int w, int h,
                          crossos_ui_text_buf_t *buf,
                          const char *placeholder);

/** Like text_input but shows bullet characters instead of typed text. */
int crossos_ui_password_input(crossos_ui_context_t *ui,
                               int x, int y, int w, int h,
                               crossos_ui_text_buf_t *buf,
                               const char *placeholder);

/* ----- Progress & status ---------------------------------------------- */

void crossos_ui_progress_bar(crossos_ui_context_t *ui,
                             int x, int y, int w, int h,
                             float percent,
                             const char *label);

/* ----- Containers ------------------------------------------------------ */

/** Collapsible section header; returns 1 when header is clicked. */
int crossos_ui_dropdown_header(crossos_ui_context_t *ui,
                               int x, int y, int w, int h,
                               const char *label,
                               int opened);

/** Tree node row; returns 1 when clicked. */
int crossos_ui_tree_header(crossos_ui_context_t *ui,
                           int x, int y, int w, int h,
                           const char *label,
                           int expanded,
                           int depth);

/** Single tab; returns 1 when clicked. */
int crossos_ui_tab(crossos_ui_context_t *ui,
                   int x, int y, int w, int h,
                   const char *label,
                   int active);

/* ----- Drag-and-drop zone --------------------------------------------- */

/**
 * Visual drop-target area.
 * Set `dragging_over` = 1 when an OS drag is hovering over the window.
 * Returns 1 when tapped/clicked (so user can open a file picker).
 */
int crossos_ui_drop_zone(crossos_ui_context_t *ui,
                         int x, int y, int w, int h,
                         const char *label,
                         int dragging_over);

/* ----- Scroll area ----------------------------------------------------- */

/**
 * Begin a scrollable region.  All widget positions passed after this call
 * should be offset by -(int)scroll->offset in the scroll direction.
 * The function draws the scrollbar track/thumb and handles scroll events.
 */
void crossos_ui_scroll_begin(crossos_ui_context_t *ui,
                             int x, int y, int w, int h,
                             crossos_ui_scroll_t *scroll,
                             int content_h);

/** End the scrollable region (pops clip). */
void crossos_ui_scroll_end(crossos_ui_context_t *ui,
                           int x, int y, int w, int h,
                           const crossos_ui_scroll_t *scroll);

/* ----- Layout helpers -------------------------------------------------- */

/** Begin a column layout (items stacked top→bottom). */
void crossos_ui_layout_begin_column(crossos_ui_layout_t *layout,
                                    int x, int y, int w, int h,
                                    int gap);

/**
 * Advance the column cursor.
 * Fills *out_rect with the next slot; returns 0 when exhausted.
 */
int crossos_ui_layout_next(crossos_ui_layout_t *layout,
                           int item_h,
                           crossos_rect_t *out_rect);

/** Begin a row layout (items placed left→right). */
void crossos_ui_layout_begin_row(crossos_ui_layout_t *layout,
                                 int x, int y, int w, int h,
                                 int gap);

/**
 * Advance the row cursor.
 * Fills *out_rect with the next slot; returns 0 when exhausted.
 */
int crossos_ui_layout_row_next(crossos_ui_layout_t *layout,
                               int item_w,
                               crossos_rect_t *out_rect);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_UI_H */
