#ifndef PAGE_MANAGER_H
#define PAGE_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Platform abstraction ---- */
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#define PM_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define PM_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define PM_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define PM_LOGI(tag, fmt, ...) printf("[I] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define PM_LOGE(tag, fmt, ...) printf("[E] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define PM_LOGW(tag, fmt, ...) printf("[W] %s: " fmt "\n", tag, ##__VA_ARGS__)
#endif

/* Stack configuration */
#ifndef PM_MAX_STACK_DEPTH
#define PM_MAX_STACK_DEPTH 16
#endif

/* Overlay configuration */
#ifndef PM_MAX_OVERLAY_DEPTH
#define PM_MAX_OVERLAY_DEPTH 4
#endif

/* Animation configuration */
#ifndef PM_ANIM_DURATION_MS
#define PM_ANIM_DURATION_MS 180
#endif

#ifndef PM_ANIM_PATH_CB
#define PM_ANIM_PATH_CB lv_anim_path_ease_out
#endif

/* Event polling interval (ms). Lower = faster UI response, higher = less CPU */
#ifndef PM_EVENT_POLL_MS
#define PM_EVENT_POLL_MS 10
#endif

/* Hard cap for queued events processed in one LVGL tick. 0 = unlimited. */
#ifndef PM_EVENT_DISPATCH_LIMIT
#define PM_EVENT_DISPATCH_LIMIT 0
#endif

/* Opaque types */
typedef struct pm_t pm_t;
typedef struct pm_page_t pm_page_t;

typedef enum {
    PM_NAV_PUSH = 0,
    PM_NAV_POP,
    PM_NAV_REPLACE,
    PM_NAV_SWITCH,
    PM_NAV_OVERLAY_SHOW,
    PM_NAV_OVERLAY_CLOSE,
} pm_nav_type_t;

typedef enum {
    PM_TRANSITION_STYLE_NONE = 0,
    PM_TRANSITION_STYLE_SLIDE,
    PM_TRANSITION_STYLE_FADE,
    PM_TRANSITION_STYLE_CUSTOM,
} pm_transition_style_t;

typedef void (*pm_transition_done_cb_t)(void *user_data);

typedef struct {
    pm_nav_type_t nav_type;
    pm_page_t *old_page;
    pm_page_t *new_page;
    lv_obj_t *old_root;
    lv_obj_t *new_root;
    int32_t width;
    int32_t height;
    uint32_t duration_ms;
    pm_transition_done_cb_t done_cb;
    void *done_user_data;
    void *user_data;
} pm_transition_ctx_t;

/**
 * Custom transition starter.
 *
 * The ctx pointer is valid only during this call. Copy any values you need for
 * asynchronous LVGL animations. A custom transition must call
 * ctx->done_cb(ctx->done_user_data) exactly once when the transition is over.
 */
typedef void (*pm_transition_start_cb_t)(const pm_transition_ctx_t *ctx);

/**
 * Optional custom transition cancel callback.
 *
 * Called when a transition is interrupted by another navigation/config change.
 * Delete animations you started here. Do not call ctx->done_cb() from cancel.
 */
typedef void (*pm_transition_cancel_cb_t)(const pm_transition_ctx_t *ctx);

/**
 * Page lifecycle callbacks.
 * - root:      the page's full-screen lv_obj_t container (pre-created by manager)
 * - user_data: user-provided pointer from pm_register_page()
 */
typedef struct {
    void (*on_create)(lv_obj_t *root, void *user_data);
    void (*on_show)(lv_obj_t *root, void *user_data);
    void (*on_hide)(lv_obj_t *root, void *user_data);
    void (*on_destroy)(lv_obj_t *root, void *user_data);
    void (*on_event)(lv_obj_t *root, int event_id, const void *data,
                     size_t data_len, void *user_data);
} pm_page_cbs_t;

/**
 * Create a page manager bound to the given display.
 * Creates a full-screen host container on the display's active screen.
 *
 * NOTE: Caller must hold LVGL lock before calling this and all other pm_*
 * functions, except pm_send_event() and pm_broadcast_event(), which are
 * thread-safe event posting APIs.
 *
 * @param disp  LVGL display to bind to (NULL for default display)
 * @return Page manager handle, or NULL on error
 */
pm_t *pm_create(lv_display_t *disp);

/**
 * Destroy page manager. Pops all pages, frees all resources.
 * After this call, the handle is invalid.
 */
void pm_destroy(pm_t *pm);

/**
 * Register a page type (does NOT push it onto the stack).
 * @param name       Debug name (logged on transitions). Copied internally.
 * @param callbacks  Lifecycle callbacks (copied internally)
 * @param user_data  Passed to all callbacks
 * @return Page handle, or NULL on error
 */
pm_page_t *pm_register_page(pm_t *pm, const char *name,
                             const pm_page_cbs_t *callbacks,
                             void *user_data);

/** Push a page onto the stack. Slide-in animation from right. */
void pm_push(pm_t *pm, pm_page_t *page);

/**
 * Push a page with navigation data.
 * Data is dynamically allocated and copied. The page can retrieve it via
 * pm_get_nav_data() in on_create/on_show.
 */
void pm_push_with_data(pm_t *pm, pm_page_t *page,
                       const void *data, size_t data_len);

/**
 * Replace the top page with a new one, carrying navigation data.
 * Equivalent to pm_replace_top() but sets nav_data before creation.
 */
void pm_replace_top_with_data(pm_t *pm, pm_page_t *page,
                              const void *data, size_t data_len);

/**
 * Retrieve the navigation data stored on this page.
 * @param page    Page handle
 * @param out_len If non-NULL, receives the data length
 * @return Pointer to internal buffer (valid while page is alive), or NULL
 */
const void *pm_get_nav_data(pm_page_t *page, size_t *out_len);

/**
 * Pop the top page off the stack. Slide-out animation to right.
 * Returns false if only one page left (can't pop last page).
 */
bool pm_pop(pm_t *pm);

/** Pop all pages above the specified page, making it the top. */
bool pm_pop_to(pm_t *pm, pm_page_t *page);

/** Get current top page handle, or NULL if stack is empty. */
pm_page_t *pm_get_top_page(pm_t *pm);

/** Get the page's root lv_obj_t. */
lv_obj_t *pm_get_page_root(pm_page_t *page);

/** Get current stack depth. */
int pm_get_stack_depth(pm_t *pm);

/** Get page name. */
const char *pm_get_page_name(pm_page_t *page);

/** Check if a page is currently in the stack. */
bool pm_has_page(pm_t *pm, pm_page_t *page);

/** Check if a page is the current top of the stack. */
bool pm_is_top(pm_t *pm, pm_page_t *page);

/** Get page at given stack index (0 = bottom). Returns NULL if out of range. */
pm_page_t *pm_get_page_at(pm_t *pm, int index);

/** Pop the top page and immediately push a new one (atomic replace). */
void pm_replace_top(pm_t *pm, pm_page_t *page);

/**
 * Switch to a main page without keeping push/pop history.
 *
 * This is intended for tabs/main sections. The previous visible page is hidden
 * but not destroyed; pages above it in the history stack are destroyed.
 */
bool pm_switch_to(pm_t *pm, pm_page_t *page);

/** Switch to a main page and update its navigation data first. */
bool pm_switch_to_with_data(pm_t *pm, pm_page_t *page,
                            const void *data, size_t data_len);

/**
 * Create an independent page instance from a registered page template.
 *
 * Use this when multiple copies of the same page type must exist at the same
 * time with different navigation data. Instances are automatically freed when
 * removed from the stack/overlay.
 */
pm_page_t *pm_create_page_instance(pm_t *pm, pm_page_t *template_page,
                                   const void *data, size_t data_len);

/** Destroy an instance created by pm_create_page_instance() if it is not needed. */
void pm_destroy_page_instance(pm_t *pm, pm_page_t *page);

/** Create and push a new page instance from a registered page template. */
pm_page_t *pm_push_instance(pm_t *pm, pm_page_t *template_page,
                            const void *data, size_t data_len);

/**
 * Show a page above the current page stack.
 *
 * The underlying page remains visible and does not receive on_hide().
 */
bool pm_show_overlay(pm_t *pm, pm_page_t *page);

/** Show an overlay and update its navigation data first. */
bool pm_show_overlay_with_data(pm_t *pm, pm_page_t *page,
                               const void *data, size_t data_len);

/** Close the top-most overlay. */
bool pm_close_overlay(pm_t *pm);

/** Close all overlays. */
void pm_close_all_overlays(pm_t *pm);

/** Get the top-most overlay page, or NULL if none is open. */
pm_page_t *pm_get_top_overlay(pm_t *pm);

/** Get current overlay depth. */
int pm_get_overlay_depth(pm_t *pm);

/**
 * Post an event to ALL pages in the stack (bottom to top), not just the top.
 *
 * Safe to call from any thread. No LVGL lock needed.
 */
void pm_broadcast_event(pm_t *pm, int event_id, const void *data, size_t data_len);

/**
 * Post an event to the page manager (thread-safe).
 * Data is dynamically allocated and queued. The event will be dispatched
 * asynchronously to the current top page's on_event callback on the LVGL thread.
 *
 * Safe to call from any thread (esp_timer, FreeRTOS task, pthread, etc.)
 * No LVGL lock needed.
 *
 * @param pm        Page manager handle
 * @param event_id  User-defined event identifier
 * @param data      Event data to copy (NULL if no data)
 * @param data_len  Size of data
 */
void pm_send_event(pm_t *pm, int event_id, const void *data, size_t data_len);

/**
 * Optional callback for pm_send_event_fast().
 * Called on the LVGL thread after the event has been dispatched or dropped.
 */
typedef void (*pm_event_data_release_cb_t)(void *data, void *user_data);

/**
 * Post an event without copying its data payload.
 *
 * This is intended for large transient payloads such as camera frames. The data
 * pointer is passed directly to page callbacks, so the caller must keep it valid
 * until release_cb is called. If release_cb is non-NULL, ownership is normally
 * transferred to the page manager.
 *
 * Safe to call from any thread. No LVGL lock needed.
 *
 * @return true if the event was queued, false on allocation/argument failure.
 */
bool pm_send_event_fast(pm_t *pm, int event_id, void *data, size_t data_len,
                        pm_event_data_release_cb_t release_cb,
                        void *release_user_data);

/**
 * Dispatch queued events immediately on the current thread.
 *
 * Must be called on the LVGL thread or while holding the LVGL lock. This is
 * useful in simulators/tests where you want deterministic event processing
 * without waiting for the manager's poll timer.
 *
 * @return Number of events dispatched or consumed.
 */
int pm_dispatch_events(pm_t *pm);

/** Drop all queued but not-yet-dispatched events. */
void pm_clear_events(pm_t *pm);

/* ---- Page transition animation ---- */

/** Set the active transition style. Default: PM_TRANSITION_STYLE_NONE. */
void pm_set_transition_style(pm_t *pm, pm_transition_style_t style);

/** Get the active transition style. */
pm_transition_style_t pm_get_transition_style(pm_t *pm);

/** Set transition duration in milliseconds. 0 disables transition animation. */
void pm_set_transition_duration(pm_t *pm, uint32_t duration_ms);

/** Get transition duration in milliseconds. */
uint32_t pm_get_transition_duration(pm_t *pm);

/**
 * Register a custom transition callback and user data.
 * Select it with pm_set_transition_style(pm, PM_TRANSITION_STYLE_CUSTOM).
 */
void pm_set_custom_transition(pm_t *pm, pm_transition_start_cb_t start_cb,
                              pm_transition_cancel_cb_t cancel_cb,
                              void *user_data);

/* ---- Page action (outward data flow) ---- */

/**
 * Action callback for pages to notify external code of user actions.
 * @param page       The page that sent the action (current top page)
 * @param action_id  User-defined action identifier
 * @param data       Action data (NULL if no data)
 * @param data_len   Size of data
 * @param user_data  User-provided pointer from pm_on_action()
 */
typedef void (*pm_action_cb_t)(pm_page_t *page, int action_id,
                                     const void *data, size_t data_len,
                                     void *user_data);

/**
 * Register a global action callback. Called when a page invokes pm_send_action().
 * @param pm         Page manager handle
 * @param handler    Callback function (NULL to unregister)
 * @param user_data  Passed to handler on each call
 */
void pm_on_action(pm_t *pm, pm_action_cb_t handler,
                           void *user_data);

/**
 * Event preprocessor callback. Called for each event before dispatching to pages.
 * Return true to consume the event (prevent further dispatch), false to pass through.
 * Called on the LVGL thread.
 */
typedef bool (*pm_event_preprocessor_t)(int event_id, const void *data,
                                         size_t data_len, void *user_data);

void pm_set_event_preprocessor(pm_t *pm, pm_event_preprocessor_t handler,
                                void *user_data);

/**
 * Send an action from the current top page to the registered action callback.
 * Must be called on the LVGL thread (e.g. from within a page callback or
 * an LVGL event handler). The caller must hold the LVGL lock.
 *
 * @param pm         Page manager handle
 * @param action_id  User-defined action identifier
 * @param data       Action data to pass (NULL if no data)
 * @param data_len   Size of data
 */
void pm_send_action(pm_t *pm, int action_id, const void *data,
                    size_t data_len);

/** Get page's user_data pointer (set during pm_register_page). */
void *pm_get_page_user_data(pm_page_t *page);

/**
 * Set whether a page should stay alive in the background when another page
 * is pushed on top. Default: false (page is destroyed when leaving, recreated when returning).
 * Set true for pages that must preserve state across navigation.
 */
void pm_page_set_keep_alive(pm_page_t *page, bool enable);

/** Query whether a page has keep_alive enabled. */
bool pm_page_get_keep_alive(pm_page_t *page);

#ifdef __cplusplus
}
#endif

#endif /* PAGE_MANAGER_H */
