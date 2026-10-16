/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_GTK4_INPUT_PRIVATE_H
#define BOXRIG_GTK4_INPUT_PRIVATE_H

#include "boxrig/ui/component.h"
#include "boxrig/ui/composition.h"
#include "boxrig/ui/input.h"
#include "boxrig/gtk/input.h"

#include <gtk/gtk.h>

#include <stdbool.h>
#include <stddef.h>

typedef struct boxrig_gtk4_input boxrig_gtk4_input;

typedef void (*boxrig_gtk4_input_activity_callback)(void *context);

typedef struct boxrig_gtk4_input_config {
    GtkWidget *widget;
    boxrig_terminal *terminal;
    boxrig_ui_view *view;
    boxrig_gtk_ime_policy ime_policy;
    boxrig_ui_candidate_policy candidate_policy;
    size_t paste_capacity;
    size_t paste_chunk_size;
    size_t maximum_events;
    size_t maximum_clipboard_transfers;
    size_t maximum_clipboard_bytes;
    boxrig_gtk4_input_activity_callback activity_callback;
    void *activity_context;
} boxrig_gtk4_input_config;

typedef struct boxrig_gtk4_input_diagnostic {
    boxrig_ui_component_failure_reason failure_reason;
    size_t queue_depth;
    size_t queue_high_water;
    size_t queue_capacity;
    size_t active_clipboard_transfers;
    size_t outgoing_clipboard_bytes;
    bool terminal_blocked;
} boxrig_gtk4_input_diagnostic;

/* Complete normalized snapshot produced from one GTK preedit update. The
 * segments are owned and released by clear_preedit_translation(). */
typedef struct boxrig_gtk4_preedit_translation {
    boxrig_composition_segment *segments;
    size_t segment_count;
    size_t selection_offset;
    size_t selection_length;
    size_t caret_offset;
} boxrig_gtk4_preedit_translation;

bool boxrig_gtk4_input_translate_preedit(
    const char *text,
    PangoAttrList *attributes,
    int cursor,
    boxrig_gtk4_preedit_translation *translation_out);

/* Releases a successful translation and resets every field. */
void boxrig_gtk4_input_clear_preedit_translation(
    boxrig_gtk4_preedit_translation *translation);

boxrig_ui_status boxrig_gtk4_input_create(
    const boxrig_gtk4_input_config *config,
    boxrig_gtk4_input **input_out);

void boxrig_gtk4_input_quiesce(boxrig_gtk4_input *input);

void boxrig_gtk4_input_root(boxrig_gtk4_input *input);

void boxrig_gtk4_input_unroot(boxrig_gtk4_input *input);

void boxrig_gtk4_input_map(boxrig_gtk4_input *input);

void boxrig_gtk4_input_unmap(boxrig_gtk4_input *input);

void boxrig_gtk4_input_destroy(boxrig_gtk4_input *input);

boxrig_ui_status boxrig_gtk4_input_set_enabled(boxrig_gtk4_input *input, bool enabled);
boxrig_ui_status boxrig_gtk4_input_get_state(
    const boxrig_gtk4_input *input, boxrig_ui_input_state *state_out);

boxrig_ui_status boxrig_gtk4_input_set_config(
    boxrig_gtk4_input *input, const boxrig_input_config *config);

boxrig_ui_status boxrig_gtk4_input_set_key_router(
    boxrig_gtk4_input *input,
    boxrig_input_key_router router,
    void *context);

boxrig_ui_status boxrig_gtk4_input_set_mouse_router(
    boxrig_gtk4_input *input,
    boxrig_input_mouse_router router,
    void *context);

boxrig_ui_view_action_capabilities
boxrig_gtk4_input_view_action_capabilities(const boxrig_gtk4_input *input);

boxrig_ui_status boxrig_gtk4_input_apply_view_action(
    boxrig_gtk4_input *input,
    boxrig_ui_view *view,
    const boxrig_ui_view_action *action);

boxrig_ui_status boxrig_gtk4_input_queue_paste(
    boxrig_gtk4_input *input,
    const uint8_t *bytes,
    size_t length);

boxrig_ui_status boxrig_gtk4_input_frame(
    boxrig_gtk4_input *input,
    size_t maximum_items,
    bool *input_blocked_out,
    bool *additional_dispatch_out);

boxrig_composition *boxrig_gtk4_input_composition(boxrig_gtk4_input *input);

void boxrig_gtk4_input_update_candidate(
    boxrig_gtk4_input *input,
    unsigned int output_scale);

void boxrig_gtk4_input_get_diagnostic(
    const boxrig_gtk4_input *input,
    boxrig_gtk4_input_diagnostic *diagnostic_out);

void boxrig_gtk4_input_get_ime_state(
    const boxrig_gtk4_input *input, boxrig_gtk_ime_state *state);

#endif
