/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WAYLAND_CONTEXT_PRIVATE_H
#define BOXRIG_WAYLAND_CONTEXT_PRIVATE_H

#include "boxrig/wayland.h"
#include "clipboard_private.h"
#include "fractional-scale-v1-client-protocol.h"
#include "input_private.h"
#include "primary-selection-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

typedef struct boxrig_wayland_output {
    boxrig_wayland_context *context;
    uint32_t global_name;
    uint32_t version;
    int32_t scale;
    struct wl_output *proxy;
} boxrig_wayland_output;

typedef enum boxrig_wayland_work_type {
    BOXRIG_WAYLAND_WORK_COMPONENT_SCALE = 0,
    BOXRIG_WAYLAND_WORK_OUTPUT_CHANGE
} boxrig_wayland_work_type;

typedef struct boxrig_wayland_work_item {
    boxrig_wayland_work_type type;
    boxrig_wayland_terminal_component *component;
} boxrig_wayland_work_item;

struct boxrig_wayland_context {
    struct wl_display *display;
    struct wl_event_queue *queue;
    struct wl_registry *registry;
    struct wl_callback *initial_sync;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_subcompositor *subcompositor;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_scale_manager;
    struct zwp_text_input_manager_v3 *text_input_manager;
    struct wl_data_device_manager *data_device_manager;
    struct zwp_primary_selection_device_manager_v1 *primary_selection_manager;
    struct boxrig_wayland_clipboard_source *clipboard_sources;
    struct boxrig_wayland_clipboard_transfer *clipboard_transfers;
    struct xkb_context *xkb_context;
    struct xkb_compose_table *compose_table;
    struct boxrig_ime_client *ibus;
    boxrig_wayland_terminal_component *components;
    boxrig_wayland_seat *seats;
    boxrig_wayland_output **outputs;
    boxrig_wayland_work_item *work_items;
    size_t output_count;
    size_t output_capacity;
    boxrig_wayland_context_config config;
    boxrig_wayland_context_state state;
    boxrig_wayland_failure_reason failure_reason;
    uint32_t compositor_name;
    uint32_t shm_name;
    uint32_t subcompositor_name;
    uint32_t viewporter_name;
    uint32_t fractional_scale_name;
    uint32_t text_input_name;
    uint32_t data_device_manager_name;
    uint32_t primary_selection_manager_name;
    uint64_t global_generation;
    size_t component_count;
    size_t seat_count;
    size_t seat_high_water;
    size_t unavailable_seat_count;
    size_t keymap_failure_count;
    size_t work_queue_depth;
    size_t work_queue_high_water;
    size_t work_queue_head;
    size_t outgoing_clipboard_bytes;
    size_t outgoing_clipboard_bytes_high_water;
    size_t incoming_clipboard_bytes;
    size_t incoming_clipboard_bytes_high_water;
    size_t active_clipboard_transfers;
    size_t clipboard_transfer_high_water;
    uint64_t next_clipboard_transfer_identifier;
    boxrig_wayland_clipboard_failure clipboard_failure;
    uint32_t clipboard_seat_global_name;
    uint32_t clipboard_target;
    boxrig_wayland_clipboard_mime clipboard_mime;
    bool argb8888;
    bool xrgb8888;
    bool registry_globals_synchronized;
    bool flush_required;
    bool output_work_queued;
    bool dispatch_clipboard_first;
    bool monotonic_time_set;
    uint64_t monotonic_milliseconds;
};

bool boxrig_wayland_context_queue_component_scale(
    boxrig_wayland_context *context,
    boxrig_wayland_terminal_component *component);

void boxrig_wayland_context_queue_output_change(
    boxrig_wayland_context *context);

void boxrig_wayland_context_remove_component_work(
    boxrig_wayland_context *context,
    boxrig_wayland_terminal_component *component);

void boxrig_wayland_terminal_components_note_output(
    boxrig_wayland_context *context,
    uint32_t global_name);

void boxrig_wayland_terminal_components_process_output_changes(
    boxrig_wayland_context *context);

void boxrig_wayland_terminal_component_process_scale(
    boxrig_wayland_terminal_component *component);

#endif
