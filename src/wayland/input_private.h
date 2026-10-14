/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WAYLAND_INPUT_PRIVATE_H
#define BOXRIG_WAYLAND_INPUT_PRIVATE_H

#include "boxrig/wayland.h"
#include "../ime/preedit.h"
#include "../ime/client.h"
#include "text-input-v3-client-protocol.h"

#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

typedef struct boxrig_wayland_seat boxrig_wayland_seat;
typedef struct boxrig_wayland_clipboard_offer boxrig_wayland_clipboard_offer;
typedef struct boxrig_wayland_clipboard_source boxrig_wayland_clipboard_source;
struct boxrig_ime_session;
struct boxrig_ime_request;

/* At most 256 boundaries normalize into at most 255 composition segments. */
#define BOXRIG_WAYLAND_PREEDIT_HINTS_MAXIMUM BOXRIG_IME_HINTS_MAXIMUM
typedef boxrig_ime_hint boxrig_wayland_preedit_hint;

typedef enum boxrig_wayland_input_item_type {
    BOXRIG_WAYLAND_INPUT_FOCUS = 0,
    BOXRIG_WAYLAND_INPUT_KEY,
    BOXRIG_WAYLAND_INPUT_MOUSE,
    BOXRIG_WAYLAND_INPUT_COMPOSITION,
    BOXRIG_WAYLAND_INPUT_DOCUMENT_BATCH,
    BOXRIG_WAYLAND_INPUT_IME_REPLY
} boxrig_wayland_input_item_type;

typedef struct boxrig_wayland_input_item {
    boxrig_wayland_input_item_type type;
    uint64_t input_generation;
    bool inhibited_key;
    boxrig_wayland_seat *seat;
    uint32_t native_code;
    bool route_resolved;
    bool application_route;
    bool scrollbar_hit;
    uint64_t pointer_generation;
    boxrig_ui_hit_test_result mouse_hit;
    union {
        boxrig_focus_event focus;
        boxrig_key_event key;
        boxrig_mouse_event mouse;
        boxrig_composition_event composition;
    } value;
    char *text;
    boxrig_composition_segment *segments;
    /* A native done batch owns both the replacement and next preedit. */
    char *commit_text;
    size_t commit_length;
    uint64_t text_epoch;
    uint32_t delete_before;
    uint32_t delete_after;
    bool commit_set;
    bool delete_set;
    bool preedit_prepared;
    /* Keep original key identity while an asynchronous IME reply is pending. */
    xkb_keysym_t keysym;
    uint32_t ime_modifiers;
    uint64_t ime_generation;
    bool compose_pending;
    bool ime_forwarded;
    bool ime_focus;
    struct boxrig_ime_request *ime_request;
} boxrig_wayland_input_item;

typedef struct boxrig_wayland_pressed_key {
    boxrig_wayland_terminal_component *component;
    boxrig_key_event identity;
    char *text;
    bool active;
    bool application_route;
    bool route_resolved;
    bool repeatable;
    bool ime_consumed;
    xkb_keysym_t keysym;
    uint32_t ime_modifiers;
} boxrig_wayland_pressed_key;

struct boxrig_wayland_seat {
    boxrig_wayland_context *context;
    boxrig_wayland_seat *next;
    uint32_t global_name;
    uint32_t version;
    struct wl_seat *proxy;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct zwp_text_input_v3 *text_input;
    struct boxrig_ime_session *ibus;
    uint64_t ime_generation;
    /* Borrowed focus identity; only the host may commit a parent surface. */
    struct wl_surface *text_focus_surface;
    struct wl_data_device *data_device;
    struct zwp_primary_selection_device_v1 *primary_selection_device;
    boxrig_wayland_clipboard_offer *clipboard_offers;
    boxrig_wayland_clipboard_offer *clipboard_offer;
    boxrig_wayland_clipboard_offer *primary_selection_offer;
    boxrig_wayland_clipboard_source *clipboard_source;
    boxrig_wayland_clipboard_source *primary_selection_source;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;
    struct xkb_compose_state *compose_state;
    boxrig_wayland_terminal_component *keyboard_component;
    boxrig_wayland_terminal_component *pointer_component;
    boxrig_wayland_terminal_component *text_component;
    boxrig_wayland_terminal_component *repeat_component;
    boxrig_wayland_pressed_key keys[256];
    bool native_pressed[256];
    bool inhibited_key[256];
    boxrig_wayland_serial serials[4];
    boxrig_wayland_terminal_component *serial_components[4];
    bool serial_valid[4];
    uint32_t capabilities;
    uint32_t pointer_buttons;
    uint32_t pointer_application_buttons;
    /* Candidate presses retain their releases after hide or pointer motion. */
    uint32_t pointer_candidate_buttons;
    /* A scrollbar press owns its release, even after its bar is hidden. */
    uint32_t pointer_scrollbar_buttons;
    uint64_t pointer_generation;
    int64_t scrollbar_drag_anchor;
    bool scrollbar_dragging;
    bool scrollbar_hovered;
    int32_t pointer_x;
    int32_t pointer_y;
    uint32_t text_commit_serial;
    uint32_t text_enable_serial;
    uint64_t text_component_generation;
    uint64_t text_epoch;
    uint64_t text_document_generation;
    uint64_t text_sent_generation;
    uint64_t text_context_offset;
    size_t text_context_length;
    bool text_document_attached;
    bool text_context_available;
    bool text_state_deferred;
    bool text_reset_required;
    char *pending_preedit;
    size_t pending_preedit_length;
    int32_t pending_cursor_begin;
    int32_t pending_cursor_end;
    boxrig_wayland_preedit_hint pending_hints[BOXRIG_WAYLAND_PREEDIT_HINTS_MAXIMUM];
    size_t pending_hint_count;
    bool pending_hint_overflow;
    char *pending_commit;
    size_t pending_commit_length;
    uint32_t pending_delete_before;
    uint32_t pending_delete_after;
    bool pending_preedit_set;
    bool pending_commit_set;
    bool pending_delete_set;
    bool pending_invalid;
    bool keymap_available;
    bool text_enabled;
    bool text_cursor_valid;
    int32_t text_cursor_x;
    int32_t text_cursor_y;
    int32_t text_cursor_width;
    int32_t text_cursor_height;
    bool repeat_active;
    uint32_t repeat_key;
    int32_t repeat_rate;
    int32_t repeat_delay;
    uint64_t repeat_start;
    uint64_t repeat_index;
    uint64_t repeat_deadline;
    uint32_t last_click_time;
    int64_t last_click_x;
    int64_t last_click_y;
    boxrig_mouse_button last_click_button;
    uint8_t last_click_count;
    bool last_click_valid;
    int64_t axis_value120[2];
    int64_t axis_value120_remainder[2];
    int64_t axis_discrete[2];
    int64_t axis_value[2];
    bool axis_value120_set[2];
    bool axis_discrete_set[2];
    bool axis_set[2];
};

bool boxrig_wayland_input_add_seat(
    boxrig_wayland_context *context,
    uint32_t name,
    uint32_t version);

bool boxrig_wayland_input_remove_global(
    boxrig_wayland_context *context,
    uint32_t name);

bool boxrig_wayland_input_bind_text_manager(
    boxrig_wayland_context *context,
    struct wl_registry *registry,
    uint32_t name,
    uint32_t version);

void boxrig_wayland_input_destroy_context(boxrig_wayland_context *context);

boxrig_ui_status boxrig_wayland_input_initialize_component(
    boxrig_wayland_terminal_component *component);

void boxrig_wayland_input_quiesce_component(
    boxrig_wayland_terminal_component *component);

void boxrig_wayland_input_destroy_component(
    boxrig_wayland_terminal_component *component);

size_t boxrig_wayland_input_dispatch(
    boxrig_wayland_context *context,
    size_t maximum_items,
    bool *additional_dispatch_out);

bool boxrig_wayland_input_repeat_deadline(
    const boxrig_wayland_context *context,
    uint64_t *deadline_out);

void boxrig_wayland_input_update_cursor_rectangle(
    boxrig_wayland_terminal_component *component);

/* Internal ownership boundary shared by compositor and direct IME adapters. */
void boxrig_wayland_input_free_item(boxrig_wayland_input_item *item);
bool boxrig_wayland_input_enqueue(boxrig_wayland_terminal_component *component,
    boxrig_wayland_input_item *item);
boxrig_ui_status boxrig_wayland_input_apply_item(boxrig_wayland_terminal_component *component,
    boxrig_wayland_input_item *item, bool *blocked, bool *consumed);
void boxrig_wayland_input_use_ibus(boxrig_wayland_context *context, bool enabled);
bool boxrig_wayland_input_cursor_geometry(boxrig_wayland_terminal_component *component,
    bool placed, int32_t rectangle[4]);
bool boxrig_wayland_input_prepare_hints(boxrig_wayland_terminal_component *component,
    const boxrig_wayland_preedit_hint *hints, size_t count, bool overflow,
    boxrig_wayland_input_item *item);
bool boxrig_wayland_input_forward_key(boxrig_wayland_seat *seat, uint32_t symbol,
    uint32_t modifiers, boxrig_wayland_input_item *item);

#endif
