/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_COMPONENT_H
#define BOXRIG_UI_COMPONENT_H

#include "boxrig/ui/view.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Model attachment role selected when a native component is created. */
typedef enum boxrig_ui_component_attachment {
    /** Own the model's exclusive canonical layout authority. */
    BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY = 0,
    /** Present canonical model geometry without changing it. */
    BOXRIG_UI_COMPONENT_PRESENTATION_ONLY
} boxrig_ui_component_attachment;

/**
 * Native presentation ownership selected when a component is created.
 *
 * This choice is independent of boxrig_ui_component_attachment, which only
 * controls authority over canonical terminal geometry.
 */
typedef enum boxrig_ui_component_native_hosting {
    /** The component creates, configures, and destroys its visible child. */
    BOXRIG_UI_COMPONENT_OWNED_CHILD = 0,
    /** The component presents into native targets owned by the caller. */
    BOXRIG_UI_COMPONENT_BORROWED_TARGET
} boxrig_ui_component_native_hosting;

/** Visual scrollbar implementation selected at component creation. */
typedef enum boxrig_ui_component_scrollbar_presentation {
    /** Do not render a component-local scrollbar. */
    BOXRIG_UI_COMPONENT_SCROLLBAR_NONE = 0,
    /** Use a component-owned native child scrollbar in owned-child hosting. */
    BOXRIG_UI_COMPONENT_SCROLLBAR_NATIVE_CHILD,
    /** Rasterize the scrollbar into the component presentation surface. */
    BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED
} boxrig_ui_component_scrollbar_presentation;

/** Runtime policy for a component-owned standard vertical scrollbar. */
typedef enum boxrig_ui_scrollbar_visibility {
    /** Reserve and show the bar even when history navigation is disabled. */
    BOXRIG_UI_SCROLLBAR_ALWAYS = 0,
    /** Reserve and show the bar only while history navigation is enabled. */
    BOXRIG_UI_SCROLLBAR_AUTO,
    /** Hide the bar and return its width to terminal content. */
    BOXRIG_UI_SCROLLBAR_NEVER
} boxrig_ui_scrollbar_visibility;

/** Requested policy and last committed standard-bar geometry. */
typedef struct boxrig_ui_scrollbar_visibility_state {
    /** Last accepted policy; independent of component set_visible(). */
    boxrig_ui_scrollbar_visibility requested;
    /** Whether the committed layout includes a bar, even with a hidden parent. */
    bool visible;
    /** Width reserved in native allocation units; zero when hidden or external. */
    unsigned int reserved_width;
    /** Geometry is waiting for native allocation or the existing grid-resize action. */
    bool pending;
} boxrig_ui_scrollbar_visibility_state;

/** Result of dispatching one native event or message to a component. */
typedef enum boxrig_ui_component_event_result {
    /** The native item belongs to the parent, a sibling, or another component. */
    BOXRIG_UI_COMPONENT_EVENT_UNRELATED = 0,
    /** The component consumed the native item. */
    BOXRIG_UI_COMPONENT_EVENT_HANDLED,
    /** Input backpressure requires retrying this same native item later. */
    BOXRIG_UI_COMPONENT_EVENT_BLOCKED,
    /** The component encountered a fatal dispatch failure. */
    BOXRIG_UI_COMPONENT_EVENT_FAILURE
} boxrig_ui_component_event_result;

/** Host-supplied component viewport and portable layout context. */
typedef struct boxrig_ui_component_allocation {
    /**
     * Parent-relative child origin, or borrowed-target presentation origin.
     */
    int x;
    /**
     * Parent-relative child origin, or borrowed-target presentation origin.
     */
    int y;
    /** Complete component width, including one component-local scrollbar. */
    unsigned int width;
    /** Complete component height. */
    unsigned int height;
    /** Current native toplevel or embedding state. */
    boxrig_ui_window_state window_state;
} boxrig_ui_component_allocation;

/** Bounded outcome of one application-driven component frame. */
typedef struct boxrig_ui_component_frame_result {
    /** Whether presentation work remains, including native or resize waits.
     * This alone does not request immediate dispatch; use additional_dispatch. */
    bool frame_pending;
    /** Whether terminal output capacity currently blocks input. */
    bool input_blocked;
    /** Whether immediate application-loop dispatch can make progress. */
    bool additional_dispatch;
    /** Whether animation_deadline_milliseconds is valid. */
    bool has_animation_deadline;
    /** Absolute monotonic deadline for animation or synchronized output recovery. */
    uint64_t animation_deadline_milliseconds;
} boxrig_ui_component_frame_result;

/** Portable terminal-component feature flags. */
typedef enum boxrig_ui_component_capability {
    /** The component owns a visible native child surface. */
    BOXRIG_UI_COMPONENT_CAPABILITY_OWNED_VISIBLE_CHILD = 1u << 0,
    /** Rendering consumes independently retained portable view dirty. */
    BOXRIG_UI_COMPONENT_CAPABILITY_VIEW_DIRTY = 1u << 1,
    /** Application font files and immutable memory resources are accepted. */
    BOXRIG_UI_COMPONENT_CAPABILITY_FONT_RESOURCES = 1u << 2,
    /** Native input-method composition is connected to the child surface. */
    BOXRIG_UI_COMPONENT_CAPABILITY_INPUT_METHOD = 1u << 3,
    /** A component-owned native child scrollbar is active. */
    BOXRIG_UI_COMPONENT_CAPABILITY_NATIVE_CHILD_SCROLLBAR = 1u << 4,
    /** User-originated bounded paste can be queued. */
    BOXRIG_UI_COMPONENT_CAPABILITY_CLIPBOARD_PASTE = 1u << 5,
    /** Revisioned take-once application size commands are supported. */
    BOXRIG_UI_COMPONENT_CAPABILITY_SIZE_COMMAND = 1u << 6,
    /** The active target can retain premultiplied alpha. */
    BOXRIG_UI_COMPONENT_CAPABILITY_TARGET_ALPHA = 1u << 7,
    /** Presentation uses a caller-owned native target. */
    BOXRIG_UI_COMPONENT_CAPABILITY_BORROWED_TARGET = 1u << 8,
    /** A scrollbar is composited into the component frame. */
    BOXRIG_UI_COMPONENT_CAPABILITY_COMPOSITED_SCROLLBAR = 1u << 9,
    /** Completed frames target a caller-owned presentation endpoint. */
    BOXRIG_UI_COMPONENT_CAPABILITY_EXTERNAL_PRESENTATION = 1u << 10
} boxrig_ui_component_capability;

/** Sticky failure that stopped one native component lifecycle. */
typedef enum boxrig_ui_component_failure_reason {
    /** No sticky failure has occurred. */
    BOXRIG_UI_COMPONENT_FAILURE_NONE = 0,
    /** The bounded normalized native-input FIFO reached its hard capacity. */
    BOXRIG_UI_COMPONENT_FAILURE_NATIVE_INPUT_OVERFLOW,
    /** A normalized input transition or application router failed. */
    BOXRIG_UI_COMPONENT_FAILURE_INPUT,
    /** Native input-method state could not be converted atomically. */
    BOXRIG_UI_COMPONENT_FAILURE_INPUT_METHOD,
    /** An asynchronous native clipboard operation failed. */
    BOXRIG_UI_COMPONENT_FAILURE_CLIPBOARD,
    /** A retained renderer candidate could not be produced. */
    BOXRIG_UI_COMPONENT_FAILURE_RENDERER,
    /** A managed animation clock update or scheduler reservation failed. */
    BOXRIG_UI_COMPONENT_FAILURE_ANIMATION
} boxrig_ui_component_failure_reason;

/** Queryable component state for application-owned logging and recovery. */
typedef struct boxrig_ui_component_diagnostic {
    /** Whether native events and bounded frames are currently accepted. */
    bool running;
    /** Whether the application has made the component visible. */
    bool visible;
    /** Whether the component last received native focus. */
    bool focused;
    /** Whether retryable input is waiting for terminal output capacity. */
    bool input_blocked;
    /** Whether this view owns the model's canonical layout authority. */
    bool layout_authority;
    /** Whether at least one actual allocation has been configured. */
    bool layout_configured;
    /** Current committed renderer and layout metric revision. */
    uint64_t metrics_revision;
    /** Current resolved transparency revision. */
    uint64_t transparency_revision;
    /** Whether a nonrecoverable component-local failure is sticky. */
    bool failed;
    /** First sticky failure, or BOXRIG_UI_COMPONENT_FAILURE_NONE. */
    boxrig_ui_component_failure_reason failure_reason;
    /** Current number of retained normalized native-input items. */
    size_t native_input_queue_depth;
    /** Greatest native-input queue depth observed in this lifecycle. */
    size_t native_input_queue_high_water;
    /** Configured hard capacity of the native-input queue. */
    size_t native_input_queue_capacity;
    /** Current number of asynchronous native clipboard transfers. */
    size_t active_clipboard_transfers;
    /** Aggregate bytes retained by active outgoing clipboard offers. */
    size_t outgoing_clipboard_bytes;
} boxrig_ui_component_diagnostic;

#endif
