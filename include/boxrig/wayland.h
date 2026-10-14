/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WAYLAND_H
#define BOXRIG_WAYLAND_H

#include "boxrig/ui.h"
#include "boxrig/ime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <poll.h>
#include <wayland-client.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Creates a standard monotonic-clock adapter for one owning Wayland loop.
 * @param adapter_out Receives the owned adapter, or NULL on failure.
 * @return A portable creation status.
 * @remarks Share its runtime among components on this loop. Contribute its
 * timeout to the ordinary wait and dispatch it with each event batch. Release
 * components before destroying the adapter; no dedicated worker is created.
 */
boxrig_ui_status boxrig_wayland_animation_poll_create(
    boxrig_ui_animation_poll **adapter_out);

/** Default number of components retained by one Wayland context. */
#define BOXRIG_WAYLAND_COMPONENTS_DEFAULT ((size_t)16)

/** Hard maximum number of components retained by one Wayland context. */
#define BOXRIG_WAYLAND_COMPONENTS_MAXIMUM ((size_t)1024)

/** Default number of callback work items retained by one Wayland context. */
#define BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT ((size_t)256)

/** Hard maximum number of callback work items retained by one Wayland context. */
#define BOXRIG_WAYLAND_WORK_ITEMS_MAXIMUM ((size_t)4096)

/** Default number of seats retained by one Wayland context. */
#define BOXRIG_WAYLAND_SEATS_DEFAULT ((size_t)8)

/** Hard maximum number of seats retained by one Wayland context. */
#define BOXRIG_WAYLAND_SEATS_MAXIMUM ((size_t)256)

/** Default maximum keymap bytes accepted from one seat. */
#define BOXRIG_WAYLAND_KEYMAP_BYTES_DEFAULT ((size_t)(1024u * 1024u))

/** Hard maximum keymap bytes accepted from one seat. */
#define BOXRIG_WAYLAND_KEYMAP_BYTES_MAXIMUM ((size_t)(16u * 1024u * 1024u))

/** Default number of native input events retained by one component. */
#define BOXRIG_WAYLAND_INPUT_EVENTS_DEFAULT ((size_t)256)

/** Hard maximum native input events retained by one component. */
#define BOXRIG_WAYLAND_INPUT_EVENTS_MAXIMUM ((size_t)4096)

/** Default maximum UTF-8 bytes accepted in one input-method event. */
#define BOXRIG_WAYLAND_IME_BYTES_DEFAULT ((size_t)(64u * 1024u))

/** Hard maximum UTF-8 bytes accepted in one input-method event. */
#define BOXRIG_WAYLAND_IME_BYTES_MAXIMUM ((size_t)(1024u * 1024u))

/** Default number of simultaneous native clipboard transfers. */
#define BOXRIG_WAYLAND_CLIPBOARD_TRANSFERS_DEFAULT ((size_t)8)

/** Hard maximum number of simultaneous native clipboard transfers. */
#define BOXRIG_WAYLAND_CLIPBOARD_TRANSFERS_MAXIMUM ((size_t)64)

/** Default aggregate bytes retained by outgoing selection snapshots. */
#define BOXRIG_WAYLAND_CLIPBOARD_BYTES_DEFAULT \
    ((size_t)(64u * 1024u * 1024u))

/** Hard aggregate bytes retained by outgoing selection snapshots. */
#define BOXRIG_WAYLAND_CLIPBOARD_BYTES_MAXIMUM \
    ((size_t)(256u * 1024u * 1024u))

/** Default bytes transferred by one nonblocking clipboard I/O operation. */
#define BOXRIG_WAYLAND_TRANSFER_CHUNK_DEFAULT ((size_t)(64u * 1024u))

/** Hard maximum bytes transferred by one clipboard I/O operation. */
#define BOXRIG_WAYLAND_TRANSFER_CHUNK_MAXIMUM ((size_t)(1024u * 1024u))

/** Default maximum bytes accepted by one native Wayland paste. */
#define BOXRIG_WAYLAND_PASTE_BYTES_DEFAULT ((size_t)(8u * 1024u * 1024u))

/** Hard maximum bytes accepted by one native Wayland paste. */
#define BOXRIG_WAYLAND_PASTE_BYTES_MAXIMUM ((size_t)(256u * 1024u * 1024u))

/** Default bytes submitted by one paste retry operation. */
#define BOXRIG_WAYLAND_PASTE_CHUNK_DEFAULT ((size_t)(4u * 1024u))

/** Hard maximum bytes submitted by one paste retry operation. */
#define BOXRIG_WAYLAND_PASTE_CHUNK_MAXIMUM ((size_t)(64u * 1024u))

/** Opaque display-wide native Wayland presentation context. */
typedef struct boxrig_wayland_context boxrig_wayland_context;

/** Opaque component owning one child surface and retained terminal view. */
typedef struct boxrig_wayland_terminal_component
    boxrig_wayland_terminal_component;

/** Observable lifecycle state of a native Wayland context. */
typedef enum boxrig_wayland_context_state {
    /** Registry and bound-object capability discovery is still in progress. */
    BOXRIG_WAYLAND_CONTEXT_INITIALIZING = 0,
    /** All required globals are available for new components. */
    BOXRIG_WAYLAND_CONTEXT_READY,
    /** Initial registry enumeration lacked a required global. */
    BOXRIG_WAYLAND_CONTEXT_UNAVAILABLE,
    /** A sticky protocol or resource failure prevents further work. */
    BOXRIG_WAYLAND_CONTEXT_FATAL,
    /** The context no longer accepts component work. */
    BOXRIG_WAYLAND_CONTEXT_QUIESCED
} boxrig_wayland_context_state;

/** First sticky failure retained by a Wayland context or component. */
typedef enum boxrig_wayland_failure_reason {
    /** No failure has occurred. */
    BOXRIG_WAYLAND_FAILURE_NONE = 0,
    /** Initial registry enumeration omitted a required singleton. */
    BOXRIG_WAYLAND_FAILURE_REQUIRED_GLOBAL_MISSING,
    /** A required singleton was removed after initialization. */
    BOXRIG_WAYLAND_FAILURE_REQUIRED_GLOBAL_REMOVED,
    /** The borrowed display entered a protocol or transport error state. */
    BOXRIG_WAYLAND_FAILURE_DISPLAY,
    /** The bounded callback work queue overflowed. */
    BOXRIG_WAYLAND_FAILURE_WORK_QUEUE_OVERFLOW,
    /** A native resource transaction could not be completed. */
    BOXRIG_WAYLAND_FAILURE_RESOURCE,
    /** A component input queue reached its configured finite capacity. */
    BOXRIG_WAYLAND_FAILURE_INPUT_QUEUE_OVERFLOW,
    /** A seat supplied an invalid, oversized, or uncompileable keymap. */
    BOXRIG_WAYLAND_FAILURE_KEYMAP,
    /** A native input transition could not be normalized or delivered. */
    BOXRIG_WAYLAND_FAILURE_INPUT,
    /** A managed animation clock update or scheduler reservation failed. */
    BOXRIG_WAYLAND_FAILURE_ANIMATION
} boxrig_wayland_failure_reason;

/** Optional input facilities currently available to a context or component. */
typedef enum boxrig_wayland_input_capability {
    /** At least one seat currently owns a usable keyboard keymap. */
    BOXRIG_WAYLAND_INPUT_CAPABILITY_KEYBOARD = 1u << 0,
    /** At least one seat currently owns a wl_pointer. */
    BOXRIG_WAYLAND_INPUT_CAPABILITY_POINTER = 1u << 1,
    /** The display currently advertises text-input-v3. */
    BOXRIG_WAYLAND_INPUT_CAPABILITY_TEXT_INPUT_V3 = 1u << 2,
    /** xkb compose fallback was created for the process locale. */
    BOXRIG_WAYLAND_INPUT_CAPABILITY_XKB_COMPOSE = 1u << 3,
    /** The selected direct IBus connection is usable. */
    BOXRIG_WAYLAND_INPUT_CAPABILITY_IBUS = 1u << 4,
    /** Direct asynchronous fcitx 5 is the current input owner. */
    BOXRIG_WAYLAND_INPUT_CAPABILITY_FCITX5 = 1u << 5
} boxrig_wayland_input_capability;

/** Optional native selection protocols currently available to a context. */
typedef enum boxrig_wayland_clipboard_capability {
    /** Core wl_data_device_manager provides the CLIPBOARD selection. */
    BOXRIG_WAYLAND_CLIPBOARD_CAPABILITY_CLIPBOARD = 1u << 0,
    /** primary-selection-v1 provides the PRIMARY selection. */
    BOXRIG_WAYLAND_CLIPBOARD_CAPABILITY_PRIMARY = 1u << 1
} boxrig_wayland_clipboard_capability;

/** Text representation selected for the most recent clipboard operation. */
typedef enum boxrig_wayland_clipboard_mime {
    /** No supported text representation was selected. */
    BOXRIG_WAYLAND_CLIPBOARD_MIME_NONE = 0,
    /** text/plain;charset=utf-8. */
    BOXRIG_WAYLAND_CLIPBOARD_MIME_TEXT_UTF8,
    /** UTF8_STRING. */
    BOXRIG_WAYLAND_CLIPBOARD_MIME_UTF8_STRING,
    /** text/plain. */
    BOXRIG_WAYLAND_CLIPBOARD_MIME_TEXT
} boxrig_wayland_clipboard_mime;

/** Recoverable result retained for the most recent clipboard failure. */
typedef enum boxrig_wayland_clipboard_failure {
    /** No clipboard failure has been observed. */
    BOXRIG_WAYLAND_CLIPBOARD_FAILURE_NONE = 0,
    /** The requested optional selection protocol is unavailable. */
    BOXRIG_WAYLAND_CLIPBOARD_FAILURE_PROTOCOL_UNAVAILABLE,
    /** Copy was not associated with a current focused input serial. */
    BOXRIG_WAYLAND_CLIPBOARD_FAILURE_SERIAL_UNAVAILABLE,
    /** The requested seat has no current selection offer. */
    BOXRIG_WAYLAND_CLIPBOARD_FAILURE_OFFER_UNAVAILABLE,
    /** The current offer has no accepted plain-text MIME. */
    BOXRIG_WAYLAND_CLIPBOARD_FAILURE_MIME_UNSUPPORTED,
    /** A conflicting or aggregate-limited transfer was rejected. */
    BOXRIG_WAYLAND_CLIPBOARD_FAILURE_BUSY,
    /** Incoming or outgoing clipboard bytes exceeded a configured limit. */
    BOXRIG_WAYLAND_CLIPBOARD_FAILURE_LIMIT_EXCEEDED,
    /** A transfer descriptor failed. */
    BOXRIG_WAYLAND_CLIPBOARD_FAILURE_IO,
    /** Incoming bytes were not valid normalized UTF-8 text. */
    BOXRIG_WAYLAND_CLIPBOARD_FAILURE_INVALID_TEXT
} boxrig_wayland_clipboard_failure;

/** Current reason that a clipboard transaction cannot make progress. */
typedef enum boxrig_wayland_clipboard_blocked_reason {
    /** Clipboard work is not blocked. */
    BOXRIG_WAYLAND_CLIPBOARD_NOT_BLOCKED = 0,
    /** An incoming pipe is waiting for readable bytes or EOF. */
    BOXRIG_WAYLAND_CLIPBOARD_BLOCKED_READ,
    /** An outgoing pipe is waiting for writable capacity. */
    BOXRIG_WAYLAND_CLIPBOARD_BLOCKED_WRITE,
    /** Terminal output capacity is blocking a validated paste. */
    BOXRIG_WAYLAND_CLIPBOARD_BLOCKED_TERMINAL
} boxrig_wayland_clipboard_blocked_reason;

/** Protocol event class whose serial retains its originating seat. */
typedef enum boxrig_wayland_serial_kind {
    /** Serial received with wl_keyboard.enter. */
    BOXRIG_WAYLAND_SERIAL_KEYBOARD_ENTER = 0,
    /** Serial received with wl_pointer.enter. */
    BOXRIG_WAYLAND_SERIAL_POINTER_ENTER,
    /** Serial received with wl_pointer.button. */
    BOXRIG_WAYLAND_SERIAL_POINTER_BUTTON,
    /** Serial received with wl_keyboard.key. */
    BOXRIG_WAYLAND_SERIAL_KEYBOARD_KEY
} boxrig_wayland_serial_kind;

/** Typed Wayland serial that cannot be confused across seats or event kinds. */
typedef struct boxrig_wayland_serial {
    /** Registry global name of the originating wl_seat. */
    uint32_t seat_global_name;
    /** Protocol event class that supplied value. */
    boxrig_wayland_serial_kind kind;
    /** Opaque compositor serial. */
    uint32_t value;
    /** Component input generation focused when the serial was observed. */
    uint64_t focus_generation;
} boxrig_wayland_serial;

/** Native Wayland input-method selection policy. */
typedef enum boxrig_wayland_input_method {
    /** Use the compositor text-input protocol, with xkb compose fallback. */
    BOXRIG_WAYLAND_IME_WAYLAND = 0,
    /** Prefer version-2 clause hints, otherwise discover a running direct IME. */
    BOXRIG_WAYLAND_IME_AUTO,
    /** Connect directly to IBus, falling back when it is unavailable. */
    BOXRIG_WAYLAND_IME_IBUS,
    /** Connect directly to fcitx 5, falling back when it is unavailable. */
    BOXRIG_WAYLAND_IME_FCITX5
} boxrig_wayland_input_method;

/** Bounded display-wide context configuration. */
typedef struct boxrig_wayland_context_config {
    /** Input policy; defaults to AUTO. */
    boxrig_wayland_input_method input_method;
    /** Candidate display policy; AUTO keeps a capable standard Wayland route. */
    boxrig_ui_candidate_policy candidate_policy;
    /** Maximum simultaneously attached terminal components. */
    size_t maximum_components;
    /** Maximum callback work items retained between caller dispatches. */
    size_t maximum_work_items;
    /** Maximum simultaneously bound wl_seat globals. */
    size_t maximum_seats;
    /** Maximum keymap bytes accepted from each seat. */
    size_t maximum_keymap_bytes;
    /** Maximum simultaneous incoming and outgoing clipboard transfers. */
    size_t maximum_clipboard_transfers;
    /** Maximum aggregate bytes retained by outgoing selection snapshots. */
    size_t maximum_clipboard_bytes;
    /** Maximum bytes processed by one transfer dispatch operation. */
    size_t transfer_chunk_size;
} boxrig_wayland_context_config;

/** Queryable display-wide Wayland state. */
typedef struct boxrig_wayland_context_diagnostic {
    /** Selected input route; AUTO while direct service discovery is pending. */
    boxrig_wayland_input_method input_method;
    /** Common direct IME selection and transport diagnostics. */
    boxrig_ime_diagnostic direct;
    /** Current asynchronous registry lifecycle. */
    boxrig_wayland_context_state state;
    /** First sticky failure, or BOXRIG_WAYLAND_FAILURE_NONE. */
    boxrig_wayland_failure_reason failure_reason;
    /** Bound wl_compositor version, or zero. */
    uint32_t compositor_version;
    /** Bound wl_shm version, or zero. */
    uint32_t shm_version;
    /** Bound wl_subcompositor version, or zero. */
    uint32_t subcompositor_version;
    /** Bound wp_viewporter version, or zero. */
    uint32_t viewporter_version;
    /** Bound wp_fractional_scale_manager_v1 version, or zero. */
    uint32_t fractional_scale_version;
    /** Bound zwp_text_input_manager_v3 version, or zero. */
    uint32_t text_input_version;
    /** Bound wl_data_device_manager version, or zero. */
    uint32_t data_device_manager_version;
    /** Bound zwp_primary_selection_device_manager_v1 version, or zero. */
    uint32_t primary_selection_manager_version;
    /** Bitwise boxrig_wayland_input_capability snapshot. */
    uint32_t input_capabilities;
    /** Bitwise boxrig_wayland_clipboard_capability snapshot. */
    uint32_t clipboard_capabilities;
    /** Whether the display-wide xkb context was created. */
    bool xkb_available;
    /** Whether wl_shm advertised premultiplied ARGB8888. */
    bool argb8888;
    /** Whether wl_shm advertised opaque XRGB8888. */
    bool xrgb8888;
    /** Monotonic registry and output change generation. */
    uint64_t global_generation;
    /** Number of currently attached components. */
    size_t component_count;
    /** Number of currently bound seats. */
    size_t seat_count;
    /** Greatest simultaneously bound seat count observed. */
    size_t seat_high_water;
    /** Seat globals retained as unavailable after reaching the seat limit. */
    size_t unavailable_seat_count;
    /** Number of invalid, oversized, or uncompileable keymap events. */
    size_t keymap_failure_count;
    /** Number of seats currently owning an active repeat deadline. */
    size_t active_repeat_count;
    /** Number of enabled seat text-input objects with a focused component. */
    size_t active_text_input_count;
    /** Number of callbacks waiting behind the initial sync. */
    size_t pending_callbacks;
    /** Current bounded callback work depth. */
    size_t work_queue_depth;
    /** Greatest callback work depth observed. */
    size_t work_queue_high_water;
    /** Number of active incoming and outgoing clipboard transfers. */
    size_t active_clipboard_transfers;
    /** Greatest simultaneous clipboard transfer count observed. */
    size_t clipboard_transfer_high_water;
    /** Aggregate bytes retained by outgoing selection snapshots. */
    size_t outgoing_clipboard_bytes;
    /** Greatest aggregate outgoing selection bytes observed. */
    size_t outgoing_clipboard_bytes_high_water;
    /** Aggregate bytes retained by incoming paste transfers. */
    size_t incoming_clipboard_bytes;
    /** Greatest aggregate incoming paste bytes observed. */
    size_t incoming_clipboard_bytes_high_water;
    /** Most recent recoverable clipboard failure. */
    boxrig_wayland_clipboard_failure clipboard_failure;
    /** Current aggregate clipboard wait reason. */
    boxrig_wayland_clipboard_blocked_reason clipboard_blocked_reason;
    /** Seat global name associated with the most recent operation. */
    uint32_t clipboard_seat_global_name;
    /** Clipboard or primary-selection target of the most recent operation. */
    uint32_t clipboard_target;
    /** MIME selected for the most recent operation. */
    boxrig_wayland_clipboard_mime clipboard_mime;
} boxrig_wayland_context_diagnostic;

/** Result of bounded context work performed after caller queue dispatch. */
typedef struct boxrig_wayland_dispatch_result {
    /** Whether another immediate bounded dispatch can make progress. */
    bool additional_dispatch;
    /** Whether requests produced by the library still need caller flushing. */
    bool flush_required;
    /** Number of component work items processed by this call. */
    size_t processed_items;
    /** Whether an active repeat has a future or immediately due deadline. */
    bool has_repeat_deadline;
    /** Earliest application-monotonic repeat deadline in milliseconds. */
    uint64_t repeat_deadline_milliseconds;
} boxrig_wayland_dispatch_result;

/** Readiness requested or reported for one native transfer descriptor. */
typedef enum boxrig_wayland_wait_interest {
    /** The descriptor can receive clipboard bytes without blocking. */
    BOXRIG_WAYLAND_WAIT_READABLE = 1u << 0,
    /** The descriptor can send clipboard bytes without blocking. */
    BOXRIG_WAYLAND_WAIT_WRITABLE = 1u << 1,
    /** The descriptor reported an error or peer hangup. */
    BOXRIG_WAYLAND_WAIT_ERROR = 1u << 2
} boxrig_wayland_wait_interest;

/** Stable snapshot of one clipboard descriptor exposed to caller poll. */
typedef struct boxrig_wayland_wait_source {
    /** Context-unique identity used to reject a stale descriptor snapshot. */
    uint64_t identifier;
    /** Borrowed descriptor valid until the next library dispatch. */
    int descriptor;
    /** Bitwise interests requested by the library. */
    uint32_t interests;
    /** Bitwise readiness supplied by the caller before dispatch. */
    uint32_t readiness;
} boxrig_wayland_wait_source;

/** Result of bounded nonblocking clipboard descriptor dispatch. */
typedef struct boxrig_wayland_transfer_dispatch_result {
    /** Number of ready transfer descriptors examined by this call. */
    size_t processed_transfers;
    /** Number of payload bytes successfully read or written. */
    size_t processed_bytes;
    /** Whether bounded work remains immediately ready. */
    bool additional_dispatch;
} boxrig_wayland_transfer_dispatch_result;

/** Default number of wl_shm buffers retained by one component. */
#define BOXRIG_WAYLAND_BUFFERS_DEFAULT ((size_t)3)

/** Minimum number of wl_shm buffers retained by one component. */
#define BOXRIG_WAYLAND_BUFFERS_MINIMUM ((size_t)2)

/** Maximum number of wl_shm buffers retained by one component. */
#define BOXRIG_WAYLAND_BUFFERS_MAXIMUM ((size_t)4)

/** Default aggregate canonical-image and wl_shm allocation limit. */
#define BOXRIG_WAYLAND_BYTES_DEFAULT ((size_t)(256u * 1024u * 1024u))

/** Hard aggregate canonical-image and wl_shm allocation limit. */
#define BOXRIG_WAYLAND_BYTES_MAXIMUM ((size_t)(1024u * 1024u * 1024u))

/** Default stale dirty rectangle count retained for each buffer. */
#define BOXRIG_WAYLAND_DIRTY_RECTS_DEFAULT ((size_t)64)

/** Hard stale dirty rectangle count retained for each buffer. */
#define BOXRIG_WAYLAND_DIRTY_RECTS_MAXIMUM ((size_t)1024)

/** Bounded configuration copied by one terminal component. */
typedef struct boxrig_wayland_terminal_component_config {
    /** Portable retained-view limits. */
    boxrig_ui_view_config view;
    /** Canonical layout authority or presentation-only attachment. */
    boxrig_ui_component_attachment attachment;
    /** Must be BOXRIG_UI_COMPONENT_OWNED_CHILD in this API version. */
    boxrig_ui_component_native_hosting hosting;
    /** COMPOSITED (default) or NONE for an application-owned scrollbar. */
    boxrig_ui_component_scrollbar_presentation scrollbar_presentation;
    /** Number of reusable wl_shm buffers, from two through four. */
    size_t buffer_count;
    /** Maximum canonical-image and wl_shm aggregate bytes. */
    size_t maximum_bytes;
    /** Maximum stale rectangles retained per buffer before full collapse. */
    size_t maximum_dirty_rectangles;
    /** Initial component-view transparency request. */
    boxrig_ui_transparency_config transparency;
    /**
     * Route otherwise-unmatched host keyboard and text-input focus here.
     * At most one running component per context may enable this fallback.
     */
    bool keyboard_focus_fallback;
    /** Optional application-loop frame request notification. */
    boxrig_ui_frame_request_callback frame_request_callback;
    /** Context passed to frame_request_callback. */
    void *frame_request_context;
    /** Maximum native input events retained in arrival order. */
    size_t maximum_input_events;
    /** Maximum UTF-8 bytes accepted in one preedit or commit event. */
    size_t maximum_ime_bytes;
    /** Maximum source and normalized bytes accepted by one paste. */
    size_t paste_capacity;
    /** Preferred normalized paste bytes; a UTF-8 scalar is never split. */
    size_t paste_chunk_size;
    /** Maximum unsigned native-time interval joining consecutive clicks. */
    uint32_t multi_click_interval_milliseconds;
    /** Maximum surface-logical distance joining consecutive clicks. */
    uint32_t multi_click_distance;
    /**
     * Shared runtime for this owning Wayland loop. Required while any blink
     * channel is enabled. The runtime outlives the component attachment.
     */
    boxrig_ui_animation_runtime *animation_runtime;
} boxrig_wayland_terminal_component_config;

/** Native child state whose application requires a caller-owned parent commit. */
typedef struct boxrig_wayland_host_action {
    /** Nonzero generation echoed after the parent is committed. */
    uint64_t revision;
    /** Whether applying this action requires wl_surface.commit(parent). */
    bool parent_commit_required;
    /** Candidate parent-relative child x coordinate. */
    int32_t x;
    /** Candidate parent-relative child y coordinate. */
    int32_t y;
} boxrig_wayland_host_action;

/** Queryable component-owned Wayland presentation state. */
typedef struct boxrig_wayland_component_diagnostic {
    /** Whether new allocations and frames are accepted. */
    bool running;
    /** Whether a host parent commit is waiting for completion. */
    bool parent_commit_pending;
    /** Latest candidate parent-state revision. */
    uint64_t parent_state_revision;
    /** Last parent-state revision completed by the host. */
    uint64_t effective_parent_state_revision;
    /** Effective child x coordinate; unchanged by stale completions. */
    int32_t effective_x;
    /** Effective child y coordinate; unchanged by stale completions. */
    int32_t effective_y;
    /** Number of configured wl_shm buffers. */
    size_t buffer_count;
    /** Current surface-logical allocation width. */
    unsigned int logical_width;
    /** Current surface-logical allocation height. */
    unsigned int logical_height;
    /** Current physical buffer width. */
    unsigned int buffer_width;
    /** Current physical buffer height. */
    unsigned int buffer_height;
    /** Aggregate canonical image and mapped wl_shm bytes. */
    size_t memory_bytes;
    /** Last complete canonical software-frame generation. */
    uint64_t presentation_generation;
    /** Number of buffers not yet released by the compositor. */
    size_t busy_buffer_count;
    /** Whether one wl_surface.frame callback is outstanding. */
    bool frame_callback_pending;
    /** Whether canonical dirty still needs a buffer commit. */
    bool presentation_pending;
    /** Greatest stale rectangle count retained for any current buffer. */
    size_t dirty_rectangle_high_water;
    /** Number of per-buffer rectangle overflows collapsed to full dirty. */
    size_t dirty_collapse_count;
    /** Committed physical-output scale numerator over denominator 120. */
    uint32_t scale_numerator;
    /** Committed wl_surface buffer scale. */
    int32_t buffer_scale;
    /** Whether fractional scale and viewport objects define the transform. */
    bool fractional_scale_active;
    /** Number of entered outputs contributing to integer scale selection. */
    size_t entered_output_count;
    /** Monotonic committed allocation, font, and input-transform revision. */
    uint64_t scale_revision;
    /** Fractional viewport destination width, or zero in integer mode. */
    unsigned int viewport_width;
    /** Fractional viewport destination height, or zero in integer mode. */
    unsigned int viewport_height;
    /** Current wl_shm format code, or zero before allocation. */
    uint32_t shm_format;
    /** First sticky component or context failure. */
    boxrig_wayland_failure_reason failure_reason;
    /** Bitwise input facilities usable by this component. */
    uint32_t input_capabilities;
    /** Number of seats whose keyboard focus is on this child surface. */
    size_t keyboard_focus_count;
    /** Number of seats whose pointer focus is on this child surface. */
    size_t pointer_focus_count;
    /** Number of enabled seat text-input objects on this child surface. */
    size_t text_input_focus_count;
    /** Current retained input queue depth. */
    size_t input_queue_depth;
    /** Greatest retained input queue depth observed. */
    size_t input_queue_high_water;
    /** Whether terminal output capacity prevents queue-head progress. */
    bool input_blocked;
    /** Whether a validated native paste transaction remains active. */
    bool paste_active;
    /** Whether terminal capacity blocks the active paste phase. */
    bool paste_blocked;
    /** Validated paste bytes retained by the active transaction. */
    size_t paste_bytes;
    /** Number of currently retained physical key press routes. */
    size_t pressed_key_count;
    /** Number of currently retained physical pointer button routes. */
    size_t pressed_button_count;
    /** Number of rejected malformed native input transitions. */
    size_t input_error_count;
    /** Whether the component currently retains visible or hidden preedit. */
    bool ime_active;
    /** Whether the queue head awaits an asynchronous IME reply or setup. */
    bool ime_waiting;
    /** Whether one accepted IME commit is waiting for terminal capacity. */
    bool ime_commit_pending;
    /** Requested candidate display policy from the owning context. */
    boxrig_ui_candidate_policy candidate_policy;
    /** Current candidate display owner, independent of keyboard focus. */
    boxrig_ui_candidate_owner candidate_owner;
    /** Reason for the current candidate display owner. */
    boxrig_ui_candidate_reason candidate_reason;
    /** Revision of the component's owned candidate snapshot. */
    uint64_t candidate_revision;
    /** Number of retained candidate strings, including nonvisible pages. */
    size_t candidate_count;
    /** Engine-selected candidate index in the retained snapshot. */
    size_t candidate_cursor;
    /** Whether the engine requests the candidate list to be visible. */
    bool candidate_visible;
    /** Whether the engine requests auxiliary text to be visible. */
    bool auxiliary_visible;
} boxrig_wayland_component_diagnostic;

/**
 * Returns conservative display-wide context defaults.
 *
 * @return A configuration with finite component and callback-work limits.
 */
boxrig_wayland_context_config boxrig_wayland_context_config_default(void);

/**
 * Starts asynchronous registry discovery on a borrowed display and queue.
 *
 * @param display Caller-owned display that outlives the context.
 * @param queue Caller-owned dedicated event queue that outlives the context.
 * @param config Optional bounded configuration, or NULL for defaults.
 * @param context_out Receives an INITIALIZING context on success.
 * @return A portable UI status.
 *
 * @remarks Every proxy created by the context is assigned to queue. This call
 * never dispatches, performs a roundtrip, reads the display FD, or disconnects
 * the display. The caller must perform race-free queue reads and dispatches.
 */
boxrig_ui_status boxrig_wayland_context_create(
    struct wl_display *display,
    struct wl_event_queue *queue,
    const boxrig_wayland_context_config *config,
    boxrig_wayland_context **context_out);

/**
 * Releases library-owned proxies without destroying borrowed objects.
 *
 * @param context Context to release, or NULL.
 *
 * @remarks All components must be destroyed first. The borrowed display and
 * event queue are never dispatched, disconnected, or destroyed.
 */
void boxrig_wayland_context_destroy(boxrig_wayland_context *context);

/**
 * Performs a bounded amount of callback-triggered component work.
 *
 * @param context Context whose dedicated queue was dispatched by the caller.
 * @param maximum_items Nonzero per-call bound, at most the configured limit.
 * @param result_out Receives progress and caller-flush state.
 * @return BOXRIG_UI_UNAVAILABLE after a sticky fatal failure, otherwise a
 * portable UI status.
 */
boxrig_ui_status boxrig_wayland_context_dispatch(
    boxrig_wayland_context *context,
    size_t maximum_items,
    boxrig_wayland_dispatch_result *result_out);

/**
 * Copies the current clipboard descriptor interests for caller-owned polling.
 *
 * @param context Context containing native clipboard transfers.
 * @param sources Destination array, or NULL only when capacity is zero.
 * @param capacity Number of entries available in sources.
 * @param count_out Receives the number of active descriptor interests.
 * @return BOXRIG_UI_LIMIT_EXCEEDED when capacity is smaller than the current
 * transfer count, otherwise a portable UI status.
 *
 * @remarks Each descriptor is borrowed and must not be closed by the caller.
 * Rebuild the poll set after every Wayland or transfer dispatch.
 */
boxrig_ui_status boxrig_wayland_context_prepare_wait_sources(
    const boxrig_wayland_context *context,
    boxrig_wayland_wait_source *sources,
    size_t capacity,
    size_t *count_out);

/**
 * Performs bounded nonblocking I/O for caller-reported ready descriptors.
 *
 * @param context Context that produced sources.
 * @param sources Wait-source snapshot with readiness populated by the caller.
 * @param count Number of entries in sources.
 * @param maximum_transfers Nonzero number of descriptors examined at most.
 * @param result_out Receives transfer and byte progress.
 * @return A portable UI status. Stale source identities are ignored.
 */
boxrig_ui_status boxrig_wayland_context_dispatch_wait_sources(
    boxrig_wayland_context *context,
    const boxrig_wayland_wait_source *sources,
    size_t count,
    size_t maximum_transfers,
    boxrig_wayland_transfer_dispatch_result *result_out);

/**
 * Prepares the optional IME descriptors and contributes its next timeout.
 * @param context Context owned by the calling thread.
 * @param descriptors_out Receives a borrowed pollfd array, copied before poll.
 * @param count_out Receives the number of descriptors, possibly zero.
 * @param timeout_inout Caller timeout in milliseconds; -1 means no deadline.
 * @return A portable UI status.
 * @remarks Keep descriptor identities and interests unchanged until dispatch.
 * All context APIs run on the owner thread. Preparing again cancels the prior
 * snapshot. This never dispatches another application's GLib default context.
 */
boxrig_ui_status boxrig_wayland_context_prepare_ime_poll(
    boxrig_wayland_context *context, const struct pollfd **descriptors_out,
    size_t *count_out, int *timeout_inout);

/**
 * Dispatches one prepared IME poll snapshot without waiting.
 * @param context Context that produced the snapshot.
 * @param descriptors Copied pollfd array with caller-populated revents.
 * @param count Number of entries in descriptors.
 * @return INVALID_ARGUMENT for a mismatching snapshot, otherwise UI status.
 * @remarks Call after a timeout as well as descriptor readiness. Then perform
 * ordinary context dispatch to apply completed input in its original order.
 */
boxrig_ui_status boxrig_wayland_context_dispatch_ime_poll(
    boxrig_wayland_context *context, const struct pollfd *descriptors, size_t count);

/**
 * Supplies the parent surface origin relative to the toplevel client surface.
 * @param component Component whose host supplies a nested parent's position.
 * @param x Parent origin in client-surface logical pixels, excluding child position.
 * @param y Parent origin in client-surface logical pixels, excluding child position.
 * @return A portable UI status.
 * @remarks Defaults to (0,0) when the parent is the toplevel client surface.
 * Do not add decorations outside that surface, such as libdecor's title bar.
 * Update after moving a nested parent. The component adds its allocation and
 * converts its physical caret by the scale.
 * This affects direct IBus only; standard Wayland uses its focused surface.
 */
boxrig_ui_status boxrig_wayland_terminal_component_set_ime_window_offset(
    boxrig_wayland_terminal_component *component, int32_t x, int32_t y);


/**
 * Acknowledges that the caller successfully flushed the borrowed display.
 *
 * @param context Context whose pending requests were flushed.
 */
void boxrig_wayland_context_note_flushed(boxrig_wayland_context *context);

/**
 * Supplies application-monotonic time used only for key-repeat deadlines.
 *
 * @param context Context to update.
 * @param milliseconds Nondecreasing monotonic milliseconds.
 * @return A portable UI status. Backward time is rejected.
 */
boxrig_ui_status boxrig_wayland_context_update_monotonic_time(
    boxrig_wayland_context *context,
    uint64_t milliseconds);

/**
 * Reads one serial without discarding its seat and event provenance.
 *
 * @param context Context containing the seat.
 * @param seat_global_name Registry global name of the seat.
 * @param kind Required serial event class.
 * @param serial_out Receives the typed serial.
 * @return BOXRIG_UI_UNAVAILABLE before that serial is observed or after seat
 * removal, otherwise a portable UI status.
 */
boxrig_ui_status boxrig_wayland_context_get_serial(
    const boxrig_wayland_context *context,
    uint32_t seat_global_name,
    boxrig_wayland_serial_kind kind,
    boxrig_wayland_serial *serial_out);

/**
 * Reads asynchronous registry, capability, and resource state.
 *
 * @param context Context to inspect.
 * @param diagnostic_out Receives a complete snapshot.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_wayland_context_get_diagnostic(
    const boxrig_wayland_context *context,
    boxrig_wayland_context_diagnostic *diagnostic_out);

/**
 * Returns conservative owned-child component defaults.
 *
 * @return Layout-authority configuration with three buffers and finite memory
 * and dirty limits.
 */
boxrig_wayland_terminal_component_config
boxrig_wayland_terminal_component_config_default(void);

/**
 * Creates one child surface and desynchronized subsurface transactionally.
 *
 * @param context READY context that outlives the component.
 * @param parent Caller-owned parent surface that outlives the component.
 * @param model Caller-owned model that outlives the component.
 * @param config Optional bounded configuration, or NULL for defaults.
 * @param component_out Receives the component on success.
 * @return BOXRIG_UI_BUSY while context initializes, BOXRIG_UI_UNAVAILABLE for
 * missing capabilities or authority conflicts, or BOXRIG_UI_LIMIT_EXCEEDED at
 * the context component limit.
 *
 * @remarks The component owns only its child surface, subsurface role, private
 * renderer, view, and later presentation resources. It never commits or
 * destroys parent.
 */
boxrig_ui_status boxrig_wayland_terminal_component_create(
    boxrig_wayland_context *context,
    struct wl_surface *parent,
    boxrig_ui_model *model,
    const boxrig_wayland_terminal_component_config *config,
    boxrig_wayland_terminal_component **component_out);

/**
 * Stops new component work and unmaps its owned child without blocking.
 *
 * @param component Component to quiesce.
 * @return A portable UI status. Repeated calls are harmless.
 */
boxrig_ui_status boxrig_wayland_terminal_component_quiesce(
    boxrig_wayland_terminal_component *component);

/**
 * Releases one component after nonblocking quiescence.
 *
 * @param component Component to release, or NULL.
 */
void boxrig_wayland_terminal_component_destroy(
    boxrig_wayland_terminal_component *component);

/**
 * Returns the component-owned child surface.
 *
 * @param component Component to inspect.
 * @return Borrowed child surface, or NULL after quiescence.
 */
struct wl_surface *boxrig_wayland_terminal_component_surface(
    boxrig_wayland_terminal_component *component);

/**
 * Returns the component-owned portable view.
 *
 * @param component Component to inspect.
 * @return Borrowed view, or NULL.
 */
boxrig_ui_view *boxrig_wayland_terminal_component_view(
    boxrig_wayland_terminal_component *component);

/**
 * Reads synchronized shared values for an application-owned scrollbar.
 * @param component Component to inspect.
 * @param state_out Receives values and the shared UI model revision.
 * @return A portable UI status.
 * @remarks Synchronize the model after direct terminal mutations. Read this
 * state during the scheduled frame; this getter does not emit callbacks.
 */
boxrig_ui_status boxrig_wayland_terminal_component_get_scrollbar_state(
    boxrig_wayland_terminal_component *component, boxrig_scrollbar_state *state_out);

/**
 * Changes the component's standard scrollbar visibility policy.
 * @param component Running component to update.
 * @param policy ALWAYS, AUTO (scrollable history), or NEVER.
 * @return OK, BUSY during another layout transaction, or an argument/error status.
 * @remarks The default is ALWAYS. NONE reserves no space under any policy.
 * A width change uses the existing grid-resize action; the old presentation
 * remains effective until complete_grid_resize succeeds. Failure quiesces the
 * component and cancels the uncommitted policy change. No history is discarded.
 */
boxrig_ui_status boxrig_wayland_terminal_component_set_scrollbar_visibility(
    boxrig_wayland_terminal_component *component, boxrig_ui_scrollbar_visibility policy);

/**
 * Reads the requested policy and committed standard scrollbar allocation.
 * @param component Component to query, including after quiescence.
 * @param state_out Receives the state; reserved_width uses surface-logical pixels.
 * @return OK, or INVALID_ARGUMENT for a null argument.
 * @remarks A hidden parent does not change the scrollbar's committed allocation.
 */
boxrig_ui_status boxrig_wayland_terminal_component_get_scrollbar_visibility(
    const boxrig_wayland_terminal_component *component,
    boxrig_ui_scrollbar_visibility_state *state_out);

/**
 * Reads the terminal's current scrollbar range and position.
 * @param component Component to inspect.
 * @param model_out Receives the current physical-row scrollbar values.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_wayland_terminal_component_get_scrollbar_model(
    boxrig_wayland_terminal_component *component, boxrig_scrollbar_model *model_out);

/**
 * Applies an external scrollbar operation to the shared viewport.
 * @param component Component whose shared viewport is controlled.
 * @param action Portable scrollbar operation.
 * @param position Oldest-to-live position used only for POSITION.
 * @param changed_out Receives whether the terminal viewport changed.
 * @return A portable UI status, including any subsequent synchronization error.
 * @remarks Changed positions synchronize the UI model and request frames.
 * Repeated positions, absent history and alternate screens are no-ops.
 */
boxrig_ui_status boxrig_wayland_terminal_component_apply_scrollbar_action(
    boxrig_wayland_terminal_component *component, boxrig_scrollbar_action action,
    size_t position, bool *changed_out);


/**
 * Returns the component-owned transient input-method composition.
 *
 * @param component Component to inspect.
 * @return Borrowed composition, or NULL for invalid input.
 */
boxrig_composition *boxrig_wayland_terminal_component_composition(
    boxrig_wayland_terminal_component *component);

/**
 * Requests input inhibition or resumption on the owning event thread.
 * @param component Component whose terminal input is controlled.
 * @param enabled True to resume, false to cancel unsubmitted input.
 * @return OK when complete, BUSY while a paste terminator awaits capacity or
 * during input dispatch, INVALID_ARGUMENT, LIMIT_EXCEEDED, or UNAVAILABLE.
 * @remarks An accepted disable request advances the generation immediately.
 * Continue context dispatch to enqueue any required paste terminator, and use
 * get_input_state() to distinguish STOPPING from DISABLED before resuming.
 * Reentrant BUSY preserves the old state. DISABLED denotes core submission,
 * not transport drain. Selection, copy, local routes, rendering, and terminal
 * protocol replies remain available. Held keys stay inhibited until release.
 */
boxrig_ui_status boxrig_wayland_terminal_component_set_input_enabled(
    boxrig_wayland_terminal_component *component, bool enabled);

/**
 * Reads the input phase and generation without advancing pending work.
 * @param component Component on its owning event thread.
 * @param state_out Required state output.
 * @return OK or INVALID_ARGUMENT.
 */
boxrig_ui_status boxrig_wayland_terminal_component_get_input_state(
    const boxrig_wayland_terminal_component *component, boxrig_ui_input_state *state_out);

/**
 * Exchanges input settings when no native key or IME transaction is pending.
 * @param component Frontend to configure on its owning event thread.
 * @param config Complete replacement, copied using the core validation rules.
 * @return OK, BUSY while a key is held or input/IME/clipboard/paste is pending,
 *         INVALID_ARGUMENT, LIMIT_EXCEEDED, or OUT_OF_MEMORY.
 * @remarks BUSY and other failures preserve all old settings and input state.
 */
boxrig_ui_status boxrig_wayland_terminal_component_set_input_config(
    boxrig_wayland_terminal_component *component, const boxrig_input_config *config);

/**
 * Installs an application key router before terminal encoding.
 *
 * @param component Component to configure.
 * @param router Router callback, or NULL to disable application routing.
 * @param context Opaque callback context, NULL when router is NULL.
 * @return BOXRIG_UI_BUSY while a physical key series is active, otherwise a
 * portable UI status.
 */
boxrig_ui_status boxrig_wayland_terminal_component_set_key_router(
    boxrig_wayland_terminal_component *component,
    boxrig_input_key_router router,
    void *context);

/**
 * Installs an application pointer router before terminal mouse encoding.
 *
 * @param component Component to configure.
 * @param router Router callback, or NULL to disable application routing.
 * @param context Opaque callback context, NULL when router is NULL.
 * @return BOXRIG_UI_BUSY while a physical button series is active, otherwise
 * a portable UI status.
 */
boxrig_ui_status boxrig_wayland_terminal_component_set_mouse_router(
    boxrig_wayland_terminal_component *component,
    boxrig_input_mouse_router router,
    void *context);

/**
 * Reports portable view actions available through one Wayland seat.
 *
 * @param component Component whose context and lifecycle are inspected.
 * @param seat_global_name Registry global name of the originating seat.
 * @return A complete capability snapshot, or no capabilities when the seat or
 * component is unavailable.
 */
boxrig_ui_view_action_capabilities
boxrig_wayland_terminal_component_view_action_capabilities(
    const boxrig_wayland_terminal_component *component,
    uint32_t seat_global_name);

/**
 * Starts one portable view action for an explicit Wayland seat.
 *
 * @param component Component owning the view and receiving pasted input.
 * @param seat_global_name Registry global name of the triggering seat.
 * @param serial Exact typed serial returned for the triggering input event;
 * required for copy and ignored for paste.
 * @param action Complete portable action.
 * @return BOXRIG_UI_UNAVAILABLE for a missing protocol, offer, focus, or valid
 * copy serial; BOXRIG_UI_BUSY for a conflicting bounded transaction; otherwise
 * a portable UI status.
 *
 * @remarks Copy validates the serial's seat, event kind, focused component,
 * and component generation before changing selection ownership. Paste uses
 * the named seat's current offer and does not require an input serial.
 */
boxrig_ui_status boxrig_wayland_terminal_component_apply_view_action(
    boxrig_wayland_terminal_component *component,
    uint32_t seat_global_name,
    const boxrig_wayland_serial *serial,
    const boxrig_ui_view_action *action);

/**
 * Transactionally changes the raw portable font specification.
 *
 * @param component Component whose private renderer is reconfigured.
 * @param specification Candidate specification copied on success.
 * @return A portable UI status.
 *
 * @remarks Font resolution and canonical metric validation finish before the
 * stored specification is replaced.
 */
boxrig_ui_status boxrig_wayland_terminal_component_set_font_specification(
    boxrig_wayland_terminal_component *component,
    const boxrig_font_specification *specification);

/**
 * Reads the current raw portable font specification.
 *
 * @param component Component to inspect.
 * @param specification_out Receives a descriptor borrowing component storage.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_wayland_terminal_component_get_font_specification(
    const boxrig_wayland_terminal_component *component,
    boxrig_font_specification *specification_out);

/**
 * Transactionally changes output DPI and rasterization policy.
 *
 * @param component Component whose private renderer is reconfigured.
 * @param policy Candidate policy whose output scale matches the surface scale.
 * @return A portable UI status.
 */
boxrig_ui_status
boxrig_wayland_terminal_component_set_font_rendering_policy(
    boxrig_wayland_terminal_component *component,
    const boxrig_font_rendering_policy *policy);

/**
 * Reads the committed output font policy.
 *
 * @param component Component to inspect.
 * @param policy_out Receives the current rendering policy.
 * @return A portable UI status.
 */
boxrig_ui_status
boxrig_wayland_terminal_component_get_font_rendering_policy(
    const boxrig_wayland_terminal_component *component,
    boxrig_font_rendering_policy *policy_out);

/**
 * Reads the private renderer's committed cell metrics.
 *
 * @param component Component to inspect.
 * @param metrics_out Receives physical-output-pixel metrics.
 * @return A portable font-catalog status.
 */
boxrig_font_catalog_status
boxrig_wayland_terminal_component_get_effective_metrics(
    const boxrig_wayland_terminal_component *component,
    boxrig_font_effective_metrics *metrics_out);

/**
 * Registers one validated application font file in the private renderer.
 *
 * @param component Component owning the private font catalog.
 * @param path Length-delimited UTF-8 file path.
 * @param path_length Number of bytes in path.
 * @param face_index Zero-based font face index.
 * @param resource_id_out Receives a nonzero exact resource identifier.
 * @return A portable font-catalog status.
 */
boxrig_font_catalog_status
boxrig_wayland_terminal_component_register_font_file(
    boxrig_wayland_terminal_component *component,
    const char *path,
    size_t path_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out);

/**
 * Registers copied immutable font bytes in the private renderer.
 *
 * @param component Component owning the private font catalog.
 * @param data Font bytes copied before return.
 * @param data_length Number of bytes in data.
 * @param face_index Zero-based font face index.
 * @param resource_id_out Receives a nonzero exact resource identifier.
 * @return A portable font-catalog status.
 */
boxrig_font_catalog_status
boxrig_wayland_terminal_component_register_font_memory(
    boxrig_wayland_terminal_component *component,
    const uint8_t *data,
    size_t data_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out);

/**
 * Returns the active exact application font resource.
 *
 * @param component Component to inspect.
 * @return Resource identifier, or BOXRIG_FONT_RESOURCE_NONE.
 */
boxrig_font_resource_id
boxrig_wayland_terminal_component_primary_font_resource(
    const boxrig_wayland_terminal_component *component);

/**
 * Returns the private font catalog revision.
 *
 * @param component Component to inspect.
 * @return Nonzero revision, or zero for NULL.
 */
uint64_t boxrig_wayland_terminal_component_font_catalog_revision(
    const boxrig_wayland_terminal_component *component);

/**
 * Changes explicit extra line height transactionally.
 *
 * @param component Component to configure.
 * @param adjustment_dip Nonnegative logical-pixel addition.
 * @return A portable UI status.
 */
boxrig_ui_status
boxrig_wayland_terminal_component_set_line_height_adjustment(
    boxrig_wayland_terminal_component *component,
    float adjustment_dip);

/**
 * Reads the explicit raw line-height addition.
 *
 * @param component Component to inspect.
 * @param adjustment_dip_out Receives the logical-pixel addition.
 * @return A portable UI status.
 */
boxrig_ui_status
boxrig_wayland_terminal_component_get_line_height_adjustment(
    const boxrig_wayland_terminal_component *component,
    float *adjustment_dip_out);

/**
 * Changes the component font by one visually distinct size step.
 *
 * @param component Component to reconfigure.
 * @param direction Whether to decrease or increase the visual size.
 * @param policy Bounded visual-step search policy.
 * @param changed_out Receives whether a different effective step was applied.
 * @return BOXRIG_UI_BUSY while a dispatched size command is unresolved;
 * otherwise a portable UI status.
 *
 * @remarks A layout-authority component issues a take-once application size
 * command when the effective cell metrics change in normal window state.
 */
boxrig_ui_status boxrig_wayland_terminal_component_visual_zoom(
    boxrig_wayland_terminal_component *component,
    boxrig_font_zoom_direction direction,
    boxrig_font_visual_step_policy policy,
    bool *changed_out);

/**
 * Restores the component font size at the start of the visual zoom sequence.
 *
 * @param component Component to reconfigure.
 * @param changed_out Receives whether the effective size changed.
 * @return BOXRIG_UI_BUSY while a dispatched size command is unresolved;
 * otherwise a portable UI status.
 */
boxrig_ui_status boxrig_wayland_terminal_component_visual_zoom_reset(
    boxrig_wayland_terminal_component *component,
    bool *changed_out);

/**
 * Selects automatic, forced LTR, or forced RTL row layout.
 *
 * @param component Component to configure.
 * @param policy New row direction policy.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_wayland_terminal_component_set_text_direction(
    boxrig_wayland_terminal_component *component,
    boxrig_text_direction_policy policy);

/**
 * Returns the current row direction policy.
 *
 * @param component Component to inspect.
 * @return Current policy, or BOXRIG_TEXT_DIRECTION_AUTO for NULL.
 */
boxrig_text_direction_policy
boxrig_wayland_terminal_component_text_direction(
    const boxrig_wayland_terminal_component *component);

/**
 * Accepts an actual surface-logical allocation and prepares a complete frame.
 *
 * @param component Component to resize and reposition.
 * @param allocation Actual parent-relative allocation accepted by the host.
 * @param layout_out Receives the resulting physical-output-pixel grid.
 * @return A portable UI status.
 *
 * @remarks Buffer dimensions, renderer image, and layout are replaced only
 * after bounded size validation and complete wl_shm allocation. A visible
 * allocation caches its complete child state until the host performs the
 * returned parent-commit action. This acceptance never creates an application
 * size command for the parent.
 */
boxrig_ui_status boxrig_wayland_terminal_component_allocate(
    boxrig_wayland_terminal_component *component,
    const boxrig_ui_component_allocation *allocation,
    boxrig_ui_layout_result *layout_out);

/**
 * Reads the last accepted allocation and effective grid.
 *
 * @param component Component to inspect.
 * @param allocation_out Receives parent-relative logical geometry.
 * @param layout_out Receives the portable physical-pixel grid.
 * @return BOXRIG_UI_INVALID_ARGUMENT before initial allocation, otherwise a
 * portable UI status.
 */
boxrig_ui_status boxrig_wayland_terminal_component_get_layout(
    const boxrig_wayland_terminal_component *component,
    boxrig_ui_component_allocation *allocation_out,
    boxrig_ui_layout_result *layout_out);

/**
 * Changes whether the component may request and present frames.
 *
 * @param component Component to configure.
 * @param visible Whether presentation is enabled.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_wayland_terminal_component_set_visible(
    boxrig_wayland_terminal_component *component,
    bool visible);

/**
 * Advances retained rendering and commits at most one released shm buffer.
 *
 * @param component Component to advance.
 * @param result_out Receives bounded progress, pending dirty, and animation
 * deadline state.
 * @return A portable UI status.
 *
 * @remarks This function never dispatches or flushes the display. When dirty
 * is waiting only for wl_buffer.release or wl_surface.frame, frame_pending is
 * true and additional_dispatch is false so the caller can wait for an event.
 */
boxrig_ui_status boxrig_wayland_terminal_component_frame(
    boxrig_wayland_terminal_component *component,
    boxrig_ui_component_frame_result *result_out);

/**
 * Maps a surface-local point through the committed output-scale transform.
 *
 * @param component Component to inspect.
 * @param x Horizontal 26.6 surface-local coordinate.
 * @param y Vertical 26.6 surface-local coordinate.
 * @param result_out Receives retained grid and logical-cell coordinates.
 * @return A portable UI status.
 *
 * @remarks A pending or failed scale transaction does not affect this mapping;
 * the transform always matches the currently committed frame and layout.
 */
boxrig_ui_status boxrig_wayland_terminal_component_hit_test(
    boxrig_wayland_terminal_component *component,
    boxrig_ui_fixed x,
    boxrig_ui_fixed y,
    boxrig_ui_hit_test_result *result_out);

/**
 * Applies a component-view transparency request transactionally.
 *
 * @param component Component to reconfigure.
 * @param config Opaque, preferred, or required transparency request.
 * @return BOXRIG_UI_UNAVAILABLE when a required target or scope is missing,
 * otherwise a portable UI status.
 *
 * @remarks An unavailable request or allocation failure retains the previous
 * view state, shm format, geometry, transform, and complete frame.
 */
boxrig_ui_status boxrig_wayland_terminal_component_set_transparency(
    boxrig_wayland_terminal_component *component,
    const boxrig_ui_transparency_config *config);

/**
 * Reads requested, capable, and effective component-view transparency.
 *
 * @param component Component to inspect.
 * @param state_out Receives resolved transparency state.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_wayland_terminal_component_get_transparency(
    const boxrig_wayland_terminal_component *component,
    boxrig_ui_transparency_state *state_out);

/**
 * Takes the one currently executable component layout action.
 *
 * @param component Component to inspect.
 * @param action_out Receives a tagged grid-resize or content-size action.
 * @return True only when one READY action advances to DISPATCHED.
 *
 * @remarks Content-size has priority over grid resize. The caller must
 * complete the returned action through the matching typed completion before
 * taking another. Standard component applications must not drain layout
 * actions from the borrowed view directly.
 */
bool boxrig_wayland_terminal_component_take_layout_action(
    boxrig_wayland_terminal_component *component,
    boxrig_ui_layout_action *action_out);

/**
 * Completes one application-driven content-size transaction.
 *
 * @param component Component that returned the action.
 * @param revision Revision returned by take_layout_action.
 * @param outcome Native acceptance result.
 * @param allocation_width Actual content width in 26.6 output pixels.
 * @param allocation_height Actual content height in 26.6 output pixels.
 * @param applied_out Receives false for an obsolete revision.
 * @param layout_out Receives the resulting effective layout.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_wayland_terminal_component_complete_size_command(
    boxrig_wayland_terminal_component *component,
    uint64_t revision,
    boxrig_ui_size_outcome outcome,
    boxrig_ui_fixed allocation_width,
    boxrig_ui_fixed allocation_height,
    bool *applied_out,
    boxrig_ui_layout_result *layout_out);

/**
 * Records the application-owned result of one dispatched grid resize.
 *
 * @param component Component that returned the action.
 * @param revision Revision returned by take_layout_action.
 * @param succeeded Whether transport and terminal resize both succeeded.
 * @return BOXRIG_UI_OK on completion, BOXRIG_UI_STALE for an unknown or
 * completed revision, or BOXRIG_UI_INVALID_ARGUMENT when the terminal grid
 * does not match a successful action.
 *
 * @remarks Failure quiesces the component because an external transport
 * resize cannot be rolled back safely.
 */
boxrig_ui_status boxrig_wayland_terminal_component_complete_grid_resize(
    boxrig_wayland_terminal_component *component,
    uint64_t revision,
    bool succeeded);

/**
 * Takes the newest parent-commit request exactly once.
 *
 * @param component Component to inspect.
 * @param action_out Receives the candidate position and revision. An action
 * can be returned for a visible allocation whose position did not change.
 * @return True only when an untaken action was returned.
 *
 * @remarks The host must commit the borrowed parent surface before completing
 * the returned revision so synchronized child allocation state is published
 * with the matching parent state.
 */
bool boxrig_wayland_terminal_component_take_host_action(
    boxrig_wayland_terminal_component *component,
    boxrig_wayland_host_action *action_out);

/**
 * Completes a parent commit without committing the borrowed parent itself.
 *
 * @param component Component containing the candidate action.
 * @param revision Revision previously returned to the host.
 * @param applied_out Receives false for an incomplete or stale revision.
 * @return A portable UI status.
 *
 * @remarks A matching completion restores desynchronized child presentation
 * after any synchronized visible-allocation transaction.
 */
boxrig_ui_status boxrig_wayland_terminal_component_complete_parent_commit(
    boxrig_wayland_terminal_component *component,
    uint64_t revision,
    bool *applied_out);

/**
 * Returns the component feature set without exposing private renderer types.
 *
 * @param component Component to inspect.
 * @return Bitwise boxrig_ui_component_capability values, or zero.
 */
uint32_t boxrig_wayland_terminal_component_capabilities(
    const boxrig_wayland_terminal_component *component);

/**
 * Reads component ownership and parent-handshake state.
 *
 * @param component Component to inspect.
 * @param diagnostic_out Receives a complete snapshot.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_wayland_terminal_component_get_diagnostic(
    const boxrig_wayland_terminal_component *component,
    boxrig_wayland_component_diagnostic *diagnostic_out);

#ifdef __cplusplus
}
#endif

#endif
