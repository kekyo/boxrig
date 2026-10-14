/* boxrig - Under MIT. */
#include "component_private.h"
#include "ibus_private.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#include <ibus.h>
#pragma GCC diagnostic pop
#include <xkbcommon/xkbcommon-keysyms.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

uint32_t boxrig_wayland_ibus_modifiers(boxrig_wayland_seat *seat, xkb_keysym_t symbol, bool release)
{
    const char *names[] = {"Shift", "Lock", "Control", "Mod1", "Mod2", "Mod3", "Mod4", "Mod5", "Hyper", "Meta"};
    const guint masks[] = {IBUS_SHIFT_MASK, IBUS_LOCK_MASK, IBUS_CONTROL_MASK, IBUS_MOD1_MASK,
        IBUS_MOD2_MASK, IBUS_MOD3_MASK, IBUS_MOD4_MASK | IBUS_SUPER_MASK, IBUS_MOD5_MASK,
        IBUS_HYPER_MASK, IBUS_META_MASK};
    guint modifiers = release ? IBUS_RELEASE_MASK : 0;
    for (size_t index = 0; index < G_N_ELEMENTS(names); index++)
        if (xkb_state_mod_name_is_active(seat->xkb_state, names[index], XKB_STATE_MODS_EFFECTIVE) > 0)
            modifiers |= masks[index];
    guint changed = 0;
    if (symbol == XKB_KEY_Shift_L || symbol == XKB_KEY_Shift_R) changed = IBUS_SHIFT_MASK;
    if (symbol == XKB_KEY_Control_L || symbol == XKB_KEY_Control_R) changed = IBUS_CONTROL_MASK;
    if (symbol == XKB_KEY_Alt_L || symbol == XKB_KEY_Alt_R) changed = IBUS_MOD1_MASK;
    if (symbol == XKB_KEY_Super_L || symbol == XKB_KEY_Super_R) changed = IBUS_MOD4_MASK | IBUS_SUPER_MASK;
    return release ? modifiers & ~changed : modifiers | changed;
}


static void wake_target(void *data, bool waiting)
{
    boxrig_wayland_terminal_component *component = data;
    component->ime_waiting = waiting;
}

static void target_error(void *data, bool resource)
{
    boxrig_wayland_terminal_component *component = data;
    if (resource) component->failure_reason = BOXRIG_WAYLAND_FAILURE_RESOURCE;
    else component->input_error_count++;
}

static void enqueue_reply(void *data, void *seat, boxrig_ime_request *request)
{
    boxrig_wayland_input_item item = {.type = BOXRIG_WAYLAND_INPUT_IME_REPLY,
        .seat = seat, .ime_request = request};
    (void)boxrig_wayland_input_enqueue(data, &item);
}

static bool forward_key(void *data, void *seat, uint32_t symbol, uint32_t code,
    uint32_t modifiers, boxrig_ime_effect *effect)
{
    (void)data; (void)code;
    boxrig_wayland_input_item item;
    if (!boxrig_wayland_input_forward_key(seat, symbol, modifiers, &item)) return false;
    *effect = (boxrig_ime_effect){.type = BOXRIG_IME_KEY,
        .value.key = item.value.key, .text = item.text};
    return true;
}

static boxrig_ui_status apply_effect(void *data, boxrig_ime_effect *effect,
    bool *blocked, bool *accepted)
{
    boxrig_wayland_terminal_component *component = data;
    if (component->input_control.state.phase != BOXRIG_UI_INPUT_ENABLED) {
        *blocked = false;
        *accepted = true;
        return BOXRIG_UI_OK;
    }
    if (effect->type == BOXRIG_IME_CANDIDATES) {
        const boxrig_ime_candidates *source = &effect->value.candidates;
        boxrig_ui_candidate_snapshot snapshot = {
            .format = source->format,
            .count = source->count, .page_size = source->page_size, .cursor = source->cursor,
            .orientation = source->orientation,
            .visible = source->visible, .cursor_visible = source->cursor_visible,
            .auxiliary_visible = source->auxiliary_visible,
            .auxiliary_above_visible = source->auxiliary_above_visible,
            .has_previous_page = source->has_previous_page, .has_next_page = source->has_next_page,
            .items = (const char *const *)source->items, .labels = (const char *const *)source->labels,
            .auxiliary = source->auxiliary, .auxiliary_above = source->auxiliary_above};
        *blocked = false;
        *accepted = true;
        return boxrig_ui_view_set_candidates(component->view, &snapshot);
    }
    boxrig_wayland_input_item item = {.text = effect->text, .segments = effect->segments,
        .input_generation = component->input_control.state.generation};
    if (effect->type == BOXRIG_IME_KEY) {
        item.type = BOXRIG_WAYLAND_INPUT_KEY;
        item.ime_forwarded = true;
        item.value.key = effect->value.key;
    } else {
        item.type = BOXRIG_WAYLAND_INPUT_COMPOSITION;
        item.value.composition = effect->value.composition;
    }
    return boxrig_wayland_input_apply_item(data, &item, blocked, accepted);
}

static void update_cursor(void *data)
{
    boxrig_wayland_ibus_cursor(data, false);
}

static void clear_candidates(void *data)
{
    boxrig_wayland_terminal_component *component = data;
    boxrig_ui_view_clear_candidates(component->view);
}

static void initialize_target(boxrig_wayland_terminal_component *component)
{
    if (component == NULL || component->ibus_target.data != NULL) return;
    component->ibus_target = (boxrig_ime_target){.view = component->view,
        .maximum_events = component->config.maximum_input_events,
        .maximum_bytes = component->config.maximum_ime_bytes, .data = component,
        .wake = wake_target, .error = target_error, .enqueue = enqueue_reply,
        .forward = forward_key, .apply = apply_effect, .cursor = update_cursor,
        .clear_candidates = component->context->config.candidate_policy != BOXRIG_UI_CANDIDATE_SYSTEM
            ? clear_candidates : NULL};
}

static void selection_changed(void *data, bool selected)
{
    boxrig_wayland_input_use_ibus(data, selected);
}

bool boxrig_wayland_ibus_initialize(boxrig_wayland_context *context)
{
    if (context->ibus != NULL || context->config.input_method == BOXRIG_WAYLAND_IME_WAYLAND) return true;
    if (context->config.input_method == BOXRIG_WAYLAND_IME_AUTO) {
        if (context->config.candidate_policy != BOXRIG_UI_CANDIDATE_CLIENT &&
            context->text_input_manager != NULL &&
            wl_proxy_get_version((struct wl_proxy *)context->text_input_manager) >= 2) return true;
    }
    context->ibus = context->config.input_method == BOXRIG_WAYLAND_IME_AUTO ?
        boxrig_ime_create_auto(true, context->config.maximum_seats, "boxrig-wayland", NULL, selection_changed, context) :
        boxrig_ime_create(context->config.input_method == BOXRIG_WAYLAND_IME_FCITX5 ?
            BOXRIG_IME_BACKEND_FCITX5 : context->config.input_method == BOXRIG_WAYLAND_IME_FCITX4 ?
            BOXRIG_IME_BACKEND_FCITX4 : BOXRIG_IME_BACKEND_IBUS, true, context->config.maximum_seats,
            "boxrig-wayland", selection_changed, context);
    return context->ibus != NULL;
}

bool boxrig_wayland_ibus_selected(const boxrig_wayland_context *context)
{
    return boxrig_ime_selected(context->ibus);
}

void boxrig_wayland_ibus_focus(boxrig_wayland_seat *seat,
    boxrig_wayland_terminal_component *component, bool commit)
{
    if (seat->context->ibus == NULL) return;
    if (component != NULL && component->input_control.state.phase != BOXRIG_UI_INPUT_ENABLED)
        component = NULL;
    initialize_target(component);
    if (seat->ibus == NULL) {
        if (component == NULL) return;
        seat->ibus = boxrig_ime_add_session(seat->context->ibus, seat);
        if (seat->ibus == NULL) { target_error(component, true); return; }
    }
    boxrig_ime_target *target = component == NULL ? NULL : &component->ibus_target;
    if (!boxrig_ime_is_focused(seat->ibus, target))
        boxrig_ime_focus(seat->ibus, target, commit, ++seat->ime_generation);
}

void boxrig_wayland_ibus_remove_seat(boxrig_wayland_seat *seat)
{
    boxrig_wayland_ibus_focus(seat, NULL, false);
    boxrig_ime_remove_session(seat->ibus);
    seat->ibus = NULL;
}

void boxrig_wayland_ibus_forget_component(boxrig_wayland_terminal_component *component)
{
    for (boxrig_wayland_seat *seat = component->context->seats; seat != NULL; seat = seat->next)
        if (boxrig_ime_is_focused(seat->ibus, &component->ibus_target))
            boxrig_wayland_ibus_focus(seat, NULL, false);
}

boxrig_ui_status boxrig_wayland_ibus_filter_key(boxrig_wayland_terminal_component *component,
    boxrig_wayland_input_item *item, bool *blocked, bool *handled)
{
    if (boxrig_wayland_ibus_selected(component->context) && item->seat->ibus == NULL) {
        *handled = true;
        return BOXRIG_UI_OK;
    }
    return boxrig_ime_filter_key(item->seat->ibus, &component->ibus_target,
        item->ime_generation, &(boxrig_ime_key){.symbol = item->keysym, .code = item->native_code,
            .modifiers = item->ime_modifiers & ~IBUS_RELEASE_MASK, .time = item->ime_time,
            .release = (item->ime_modifiers & IBUS_RELEASE_MASK) != 0,
            .repeat = item->value.key.action == BOXRIG_KEY_REPEAT}, &item->ime_request, blocked, handled);
}

boxrig_ui_status boxrig_wayland_ibus_apply_reply(boxrig_wayland_terminal_component *component,
    boxrig_wayland_input_item *item, bool *blocked, bool *consumed)
{
    return boxrig_ime_apply_reply(&component->ibus_target, item->ime_request, blocked, consumed);
}

void boxrig_wayland_ibus_release_request(boxrig_ime_request *request)
{
    boxrig_ime_release_request(request);
}

void boxrig_wayland_ibus_cursor(boxrig_wayland_terminal_component *component, bool placed)
{
    int32_t rectangle[4];
    if (!boxrig_wayland_ibus_selected(component->context) ||
        !boxrig_wayland_input_cursor_geometry(component, placed, rectangle)) return;
    int64_t x = (int64_t)rectangle[0] + component->candidate_x + component->ime_window_x;
    int64_t y = (int64_t)rectangle[1] + component->candidate_y + component->ime_window_y;
    if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) return;
    rectangle[0] = (int32_t)x; rectangle[1] = (int32_t)y;
    boxrig_ime_cursor(component->context->ibus, &component->ibus_target, rectangle, true,
        component->scale_numerator / 120.0);
}

boxrig_ui_status boxrig_wayland_terminal_component_set_ime_window_offset(
    boxrig_wayland_terminal_component *component, int32_t x, int32_t y)
{
    if (component == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    component->ime_window_x = x;
    component->ime_window_y = y;
    boxrig_wayland_ibus_cursor(component, false);
    return BOXRIG_UI_OK;
}


void boxrig_wayland_ibus_update(boxrig_wayland_context *context)
{
    boxrig_ime_update(context->ibus);
}

void boxrig_wayland_ibus_diagnostic(const boxrig_wayland_context *context,
    boxrig_wayland_context_diagnostic *diagnostic)
{
    boxrig_ime_diagnostic state;
    boxrig_ime_get_diagnostic(context->ibus, &state);
    diagnostic->input_method = boxrig_wayland_ibus_selected(context) ?
        (state.backend == BOXRIG_IME_BACKEND_NONE ? BOXRIG_WAYLAND_IME_AUTO :
         state.backend == BOXRIG_IME_BACKEND_FCITX5 ? BOXRIG_WAYLAND_IME_FCITX5 :
         state.backend == BOXRIG_IME_BACKEND_FCITX4 ? BOXRIG_WAYLAND_IME_FCITX4 : BOXRIG_WAYLAND_IME_IBUS) : BOXRIG_WAYLAND_IME_WAYLAND;
    diagnostic->direct = state;
    if (boxrig_wayland_ibus_selected(context) && state.backend != BOXRIG_IME_BACKEND_NONE) diagnostic->input_capabilities |=
        state.backend == BOXRIG_IME_BACKEND_FCITX5 ? BOXRIG_WAYLAND_INPUT_CAPABILITY_FCITX5 :
            state.backend == BOXRIG_IME_BACKEND_FCITX4 ? BOXRIG_WAYLAND_INPUT_CAPABILITY_FCITX4 :
            BOXRIG_WAYLAND_INPUT_CAPABILITY_IBUS;
}

boxrig_ui_status boxrig_wayland_context_prepare_ime_poll(boxrig_wayland_context *context,
    const struct pollfd **descriptors_out, size_t *count_out, int *timeout_inout)
{
    if (context == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    return boxrig_ime_prepare_poll(context->ibus, descriptors_out, count_out, timeout_inout);
}

boxrig_ui_status boxrig_wayland_context_dispatch_ime_poll(boxrig_wayland_context *context,
    const struct pollfd *descriptors, size_t count)
{
    if (context == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    return boxrig_ime_dispatch_poll(context->ibus, descriptors, count);
}

void boxrig_wayland_ibus_destroy(boxrig_wayland_context *context)
{
    boxrig_ime_destroy(context->ibus);
    context->ibus = NULL;
}
