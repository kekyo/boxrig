/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WIN32_HOST_H
#define BOXRIG_WIN32_HOST_H

#include "boxrig/ui/notification_dispatch.h"
#include "boxrig/ui/view.h"

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Policy for host actions requested by terminal-controlled content. */
typedef struct boxrig_win32_host_policy {
    /** Apply OSC title changes to the sample window. */
    bool allow_title_change;
    /** Publish OSC 52 writes as CF_UNICODETEXT. */
    bool allow_clipboard_write;
    /** Return CF_UNICODETEXT to OSC 52 queries. */
    bool allow_clipboard_read;
    /** Read external Kitty graphics resources using sample host privileges. */
    bool allow_graphics_external;
    /** Policy for desktop notifications requested by terminal content. */
    boxrig_notification_policy_config notification;
    /** Optional copied provider interface; NULL selects Shell balloon alerts.
     * Its context must outlive the adapter. */
    const boxrig_notification_provider *notification_provider;
} boxrig_win32_host_policy;

/** Explicit native targets used by model-level host actions. */
typedef struct boxrig_win32_host_targets {
    /** Caller-owned toplevel receiving title and notification actions. */
    HWND toplevel;
    /** Caller-owned component-local HWND used as clipboard owner. */
    HWND clipboard_owner;
    /** Caller-owned presented view used for focus and visibility policy. */
    HWND view;
} boxrig_win32_host_targets;

/** Opaque Win32 implementation of boxrig host items. */
typedef struct boxrig_win32_host_adapter boxrig_win32_host_adapter;

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
boxrig_win32_host_policy boxrig_win32_host_policy_default(void);

/**
 * Creates a Win32 host adapter.
 *
 * @param targets Distinct caller-owned toplevel, clipboard, and view targets.
 * @param consumer Exclusive model-level host-item consumer borrowed by the
 * adapter.
 * @param clipboard_capacity Maximum UTF-8 bytes returned from the clipboard.
 * @param policy Host action policy copied by the adapter.
 * @param adapter_out Receives the new adapter.
 * @return Zero on success, or -1 on failure.
 *
 * @remarks The consumer and its model must outlive the adapter.
 */
int boxrig_win32_host_adapter_create(
    const boxrig_win32_host_targets *targets,
    boxrig_ui_host_item_consumer *consumer,
    size_t clipboard_capacity,
    const boxrig_win32_host_policy *policy,
    boxrig_win32_host_adapter **adapter_out);

/**
 * Processes all queued terminal host items.
 *
 * @param adapter Adapter to run.
 * @return Zero on success, or -1 on failure.
 */
int boxrig_win32_host_adapter_process_items(boxrig_win32_host_adapter *adapter);

/**
 * Releases adapter resources.
 *
 * @param adapter Adapter to destroy, or NULL.
 */
void boxrig_win32_host_adapter_destroy(boxrig_win32_host_adapter *adapter);

#ifdef __cplusplus
}
#endif

#endif
