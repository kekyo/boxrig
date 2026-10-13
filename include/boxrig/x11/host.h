/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_X11_HOST_H
#define BOXRIG_X11_HOST_H

#include "boxrig/ui/notification.h"
#include "boxrig/ui/view.h"

#include <X11/Xlib.h>

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>

/** Policy for host actions requested by terminal-controlled content. */
typedef struct boxrig_x11_host_policy {
    /** Apply OSC title changes to the sample window. */
    bool allow_title_change;
    /** Publish OSC 52 writes to X11 selections and cut buffers. */
    bool allow_clipboard_write;
    /** Return X11 selection contents to OSC 52 queries. */
    bool allow_clipboard_read;
    /** Read external Kitty graphics resources using sample host privileges. */
    bool allow_graphics_external;
    /** Policy for desktop notifications requested by terminal content. */
    boxrig_notification_policy_config notification;
} boxrig_x11_host_policy;

/** Explicit native targets used by model-level host actions. */
typedef struct boxrig_x11_host_targets {
    /** Caller-owned toplevel receiving title and notification actions. */
    Window toplevel;
    /** Caller-owned component-local window used for X11 selections. */
    Window clipboard_owner;
    /** Caller-owned presented view used for focus and visibility policy. */
    Window view;
} boxrig_x11_host_targets;

/** Opaque X11 implementation of boxrig host items. */
typedef struct boxrig_x11_host_adapter boxrig_x11_host_adapter;

/**
 * Returns a deterministic security-conscious host policy.
 *
 * @return A policy that permits titles and clipboard writes, denies clipboard
 * reads, desktop notifications, and external graphics.
 *
 * @remarks Working-directory URIs are always treated as metadata, and OSC 8
 * hyperlinks are never activated by this adapter. The library never reads
 * process environment variables; applications explicitly copy any opt-in into
 * the returned policy.
 */
boxrig_x11_host_policy boxrig_x11_host_policy_default(void);

/**
 * Creates an X11 host adapter.
 *
 * @param display X11 connection used by window.
 * @param targets Distinct caller-owned toplevel, clipboard, and view targets.
 * @param consumer Exclusive model-level host-item consumer borrowed by the
 * adapter.
 * @param clipboard_capacity Maximum bytes fetched from an X11 selection.
 * @param policy Host action policy copied by the adapter.
 * @param adapter_out Receives the new adapter.
 * @return Zero on success, or -1 on failure.
 *
 * @remarks The consumer and its model must outlive the adapter.
 */
int boxrig_x11_host_adapter_create(
    Display *display,
    const boxrig_x11_host_targets *targets,
    boxrig_ui_host_item_consumer *consumer,
    size_t clipboard_capacity,
    const boxrig_x11_host_policy *policy,
    boxrig_x11_host_adapter **adapter_out);

/**
 * Processes queued terminal host items until the queue or adapter blocks.
 *
 * @param adapter Adapter to run.
 * @return Zero on success, or -1 on failure.
 */
int boxrig_x11_host_adapter_process_items(boxrig_x11_host_adapter *adapter);

/**
 * Handles an X11 selection event when it belongs to the adapter.
 *
 * @param adapter Adapter to run.
 * @param event Event obtained from XNextEvent().
 * @return One when handled, zero when unrelated, or -1 on failure.
 */
int boxrig_x11_host_adapter_process_event(
    boxrig_x11_host_adapter *adapter,
    const XEvent *event);

/**
 * Returns the current number of D-Bus descriptors needed by poll().
 *
 * @param adapter Adapter to inspect.
 * @return Number of enabled descriptors, or zero for NULL or an idle provider.
 */
size_t boxrig_x11_host_adapter_poll_descriptor_count(
    const boxrig_x11_host_adapter *adapter);

/**
 * Adds the adapter's current D-Bus watches and timeout to a poll cycle.
 *
 * @param adapter Adapter to prepare.
 * @param descriptors Storage for up to descriptor_capacity poll entries.
 * @param descriptor_capacity Number of entries available in descriptors.
 * @param descriptor_count_out Receives the number of entries written.
 * @param timeout_milliseconds_in_out Caller timeout on entry, and the shorter
 * D-Bus timeout on return. A value of -1 means no caller timeout.
 * @return Zero on success, or -1 for invalid arguments or allocation failure.
 *
 * @remarks The returned descriptors must be passed unchanged to
 * boxrig_x11_host_adapter_process_poll() after poll().
 */
int boxrig_x11_host_adapter_prepare_poll(
    boxrig_x11_host_adapter *adapter,
    struct pollfd *descriptors,
    size_t descriptor_capacity,
    size_t *descriptor_count_out,
    int *timeout_milliseconds_in_out);

/**
 * Handles one prepared D-Bus poll cycle and dispatches pending replies.
 *
 * @param adapter Adapter to run.
 * @param descriptors Entries prepared by the preceding prepare call.
 * @param descriptor_count Number of prepared entries.
 * @return Zero on success, or -1 on adapter or terminal failure.
 */
int boxrig_x11_host_adapter_process_poll(
    boxrig_x11_host_adapter *adapter,
    const struct pollfd *descriptors,
    size_t descriptor_count);

/**
 * Releases selection ownership and adapter resources.
 *
 * @param adapter Adapter to destroy, or NULL.
 */
void boxrig_x11_host_adapter_destroy(boxrig_x11_host_adapter *adapter);

#endif
