/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WAYLAND_CLIPBOARD_PRIVATE_H
#define BOXRIG_WAYLAND_CLIPBOARD_PRIVATE_H

#include "boxrig/wayland.h"

struct boxrig_wayland_seat;
struct boxrig_wayland_clipboard_source;
typedef struct boxrig_wayland_clipboard_transfer
    boxrig_wayland_clipboard_transfer;

bool boxrig_wayland_clipboard_bind_data_manager(
    boxrig_wayland_context *context,
    struct wl_registry *registry,
    uint32_t name,
    uint32_t version);

bool boxrig_wayland_clipboard_bind_primary_manager(
    boxrig_wayland_context *context,
    struct wl_registry *registry,
    uint32_t name,
    uint32_t version);

bool boxrig_wayland_clipboard_create_seat_devices(
    struct boxrig_wayland_seat *seat);

void boxrig_wayland_clipboard_destroy_seat_devices(
    struct boxrig_wayland_seat *seat);

void boxrig_wayland_clipboard_quiesce_component(
    boxrig_wayland_terminal_component *component);

bool boxrig_wayland_clipboard_input_pending(
    const boxrig_wayland_terminal_component *component);

size_t boxrig_wayland_clipboard_dispatch(
    boxrig_wayland_context *context,
    size_t maximum_items,
    bool *additional_out);

void boxrig_wayland_clipboard_get_context_diagnostic(
    const boxrig_wayland_context *context,
    boxrig_wayland_context_diagnostic *diagnostic);

void boxrig_wayland_clipboard_get_component_diagnostic(
    const boxrig_wayland_terminal_component *component,
    boxrig_wayland_component_diagnostic *diagnostic);

bool boxrig_wayland_clipboard_remove_global(
    boxrig_wayland_context *context,
    uint32_t name);

void boxrig_wayland_clipboard_destroy_context(
    boxrig_wayland_context *context);

#endif
