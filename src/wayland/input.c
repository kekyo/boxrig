/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "component_private.h"
#include "ibus_private.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#include <ibus.h>
#pragma GCC diagnostic pop
#include "utf8.h"
#include "boxrig/ui/editable.h"

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-names.h>

#include <linux/input-event-codes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define BOXRIG_WAYLAND_SEAT_VERSION 9U

static bool input_enabled(const boxrig_wayland_terminal_component *component)
{
    return component->input_control.state.phase == BOXRIG_UI_INPUT_ENABLED;
}

static bool accepts_item(const boxrig_wayland_terminal_component *component,
    const boxrig_wayland_input_item *item)
{
    return boxrig_ui_input_control_accepts(&component->input_control, item->input_generation);
}

static uint32_t supported_version(uint32_t advertised, uint32_t supported)
{
    return advertised < supported ? advertised : supported;
}

static boxrig_wayland_terminal_component *find_component(
    boxrig_wayland_context *context,
    struct wl_surface *surface)
{
    boxrig_wayland_terminal_component *component;

    for (component = context->components;
         component != NULL;
         component = component->next) {
        if (component->running && !component->reparent_pending && component->surface == surface) {
            return component;
        }
    }
    return NULL;
}

static boxrig_wayland_terminal_component *find_keyboard_component(
    boxrig_wayland_context *context,
    struct wl_surface *surface)
{
    boxrig_wayland_terminal_component *component = find_component(
        context,
        surface);

    if (component != NULL) {
        return component;
    }
    for (component = context->components;
         component != NULL;
         component = component->next) {
        if (component->running && !component->reparent_pending && component->config.keyboard_focus_fallback &&
            component->parent == surface) {
            return component;
        }
    }
    for (component = context->components;
         component != NULL;
         component = component->next) {
        if (component->running && !component->reparent_pending && !component->explicit_parent_input &&
            component->config.keyboard_focus_fallback) {
            /* Decoration libraries may own focusable subsurfaces without
             * exposing their wl_surface objects through a public API. */
            return component;
        }
    }
    return NULL;
}

void boxrig_wayland_input_free_item(boxrig_wayland_input_item *item)
{
    boxrig_wayland_ibus_release_request(item->ime_request);
    free(item->text);
    free(item->segments);
    free(item->commit_text);
    memset(item, 0, sizeof(*item));
}

static bool enqueue_input(
    boxrig_wayland_terminal_component *component,
    boxrig_wayland_input_item *item)
{
    size_t index;

    if (component == NULL || !component->running ||
        component->failure_reason != BOXRIG_WAYLAND_FAILURE_NONE) {
        boxrig_wayland_input_free_item(item);
        return false;
    }
    if (!input_enabled(component) && item->type != BOXRIG_WAYLAND_INPUT_KEY &&
        item->type != BOXRIG_WAYLAND_INPUT_MOUSE && item->type != BOXRIG_WAYLAND_INPUT_FOCUS) {
        boxrig_wayland_input_free_item(item);
        return true;
    }
    if (component->input_queue_depth ==
        component->config.maximum_input_events) {
        component->failure_reason =
            BOXRIG_WAYLAND_FAILURE_INPUT_QUEUE_OVERFLOW;
        boxrig_wayland_input_free_item(item);
        return false;
    }
    if (((item->type == BOXRIG_WAYLAND_INPUT_KEY &&
          item->value.key.action != BOXRIG_KEY_RELEASE) ||
         (item->type == BOXRIG_WAYLAND_INPUT_DOCUMENT_BATCH &&
          (item->commit_set || item->delete_set)) ||
         (item->type == BOXRIG_WAYLAND_INPUT_COMPOSITION &&
          item->value.composition.type == BOXRIG_COMPOSITION_COMMIT)) &&
        boxrig_ui_view_notify_local_input(component->view) != BOXRIG_UI_OK) {
        component->failure_reason = BOXRIG_WAYLAND_FAILURE_INPUT;
        boxrig_wayland_input_free_item(item);
        return false;
    }
    index = (component->input_queue_head + component->input_queue_depth) %
            component->config.maximum_input_events;
    item->input_generation = component->input_control.state.generation;
    component->input_items[index] = *item;
    memset(item, 0, sizeof(*item));
    component->input_queue_depth++;
    if (component->input_queue_depth > component->input_queue_high_water) {
        component->input_queue_high_water = component->input_queue_depth;
    }
    return true;
}

bool boxrig_wayland_input_enqueue(boxrig_wayland_terminal_component *component,
    boxrig_wayland_input_item *item)
{
    if (component == NULL) return enqueue_input(component, item);
    bool processing = component->processing_input;
    component->processing_input = true;
    bool accepted = enqueue_input(component, item);
    component->processing_input = processing;
    return accepted;
}

static bool queue_focus(
    boxrig_wayland_seat *seat,
    boxrig_wayland_terminal_component *component,
    bool focused)
{
    boxrig_wayland_input_item item;

    memset(&item, 0, sizeof(item));
    item.type = BOXRIG_WAYLAND_INPUT_FOCUS;
    item.seat = seat;
    item.value.focus.focused = focused;
    item.ime_focus = boxrig_wayland_ibus_selected(component->context);
    if (!focused && !item.ime_focus)
        (void)boxrig_ui_editable_document_invalidate_input(
            boxrig_ui_view_editable_document(component->view));
    if (!boxrig_wayland_input_enqueue(component, &item)) {
        return false;
    }
    /* Visual focus is independent of backpressure on the queued report. */
    if (boxrig_ui_view_set_focused(component->view, focused) != BOXRIG_UI_OK) {
        component->failure_reason = BOXRIG_WAYLAND_FAILURE_INPUT;
        return false;
    }
    return true;
}

static void set_serial(
    boxrig_wayland_seat *seat,
    boxrig_wayland_serial_kind kind,
    uint32_t value,
    boxrig_wayland_terminal_component *component)
{
    if (component == NULL || !component->running) {
        seat->serial_valid[kind] = false;
        seat->serial_components[kind] = NULL;
        return;
    }
    seat->serials[kind].seat_global_name = seat->global_name;
    seat->serials[kind].kind = kind;
    seat->serials[kind].value = value;
    seat->serials[kind].focus_generation = component->input_generation;
    seat->serial_components[kind] = component;
    seat->serial_valid[kind] = true;
}

static boxrig_key translate_functional_key(xkb_keysym_t symbol)
{
    if (symbol >= XKB_KEY_F1 && symbol <= XKB_KEY_F35) {
        return (boxrig_key)(BOXRIG_KEY_F1 + (symbol - XKB_KEY_F1));
    }
    switch (symbol) {
    case XKB_KEY_Return:
        return BOXRIG_KEY_ENTER;
    case XKB_KEY_BackSpace:
        return BOXRIG_KEY_BACKSPACE;
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab:
        return BOXRIG_KEY_TAB;
    case XKB_KEY_Escape:
        return BOXRIG_KEY_ESCAPE;
    case XKB_KEY_Up:
        return BOXRIG_KEY_UP;
    case XKB_KEY_Down:
        return BOXRIG_KEY_DOWN;
    case XKB_KEY_Right:
        return BOXRIG_KEY_RIGHT;
    case XKB_KEY_Left:
        return BOXRIG_KEY_LEFT;
    case XKB_KEY_Home:
        return BOXRIG_KEY_HOME;
    case XKB_KEY_End:
        return BOXRIG_KEY_END;
    case XKB_KEY_Insert:
        return BOXRIG_KEY_INSERT;
    case XKB_KEY_Delete:
        return BOXRIG_KEY_DELETE;
    case XKB_KEY_Page_Up:
        return BOXRIG_KEY_PAGE_UP;
    case XKB_KEY_Page_Down:
        return BOXRIG_KEY_PAGE_DOWN;
    case XKB_KEY_Caps_Lock:
        return BOXRIG_KEY_CAPS_LOCK;
    case XKB_KEY_Scroll_Lock:
        return BOXRIG_KEY_SCROLL_LOCK;
    case XKB_KEY_Num_Lock:
        return BOXRIG_KEY_NUM_LOCK;
    case XKB_KEY_Print:
        return BOXRIG_KEY_PRINT_SCREEN;
    case XKB_KEY_Pause:
        return BOXRIG_KEY_PAUSE;
    case XKB_KEY_Menu:
        return BOXRIG_KEY_MENU;
    case XKB_KEY_KP_0:
        return BOXRIG_KEY_KP_0;
    case XKB_KEY_KP_1:
        return BOXRIG_KEY_KP_1;
    case XKB_KEY_KP_2:
        return BOXRIG_KEY_KP_2;
    case XKB_KEY_KP_3:
        return BOXRIG_KEY_KP_3;
    case XKB_KEY_KP_4:
        return BOXRIG_KEY_KP_4;
    case XKB_KEY_KP_5:
        return BOXRIG_KEY_KP_5;
    case XKB_KEY_KP_6:
        return BOXRIG_KEY_KP_6;
    case XKB_KEY_KP_7:
        return BOXRIG_KEY_KP_7;
    case XKB_KEY_KP_8:
        return BOXRIG_KEY_KP_8;
    case XKB_KEY_KP_9:
        return BOXRIG_KEY_KP_9;
    case XKB_KEY_KP_Decimal:
        return BOXRIG_KEY_KP_DECIMAL;
    case XKB_KEY_KP_Divide:
        return BOXRIG_KEY_KP_DIVIDE;
    case XKB_KEY_KP_Multiply:
        return BOXRIG_KEY_KP_MULTIPLY;
    case XKB_KEY_KP_Subtract:
        return BOXRIG_KEY_KP_SUBTRACT;
    case XKB_KEY_KP_Add:
        return BOXRIG_KEY_KP_ADD;
    case XKB_KEY_KP_Enter:
        return BOXRIG_KEY_KP_ENTER;
    case XKB_KEY_KP_Equal:
        return BOXRIG_KEY_KP_EQUAL;
    case XKB_KEY_KP_Separator:
        return BOXRIG_KEY_KP_SEPARATOR;
    case XKB_KEY_KP_Left:
        return BOXRIG_KEY_KP_LEFT;
    case XKB_KEY_KP_Right:
        return BOXRIG_KEY_KP_RIGHT;
    case XKB_KEY_KP_Up:
        return BOXRIG_KEY_KP_UP;
    case XKB_KEY_KP_Down:
        return BOXRIG_KEY_KP_DOWN;
    case XKB_KEY_KP_Page_Up:
        return BOXRIG_KEY_KP_PAGE_UP;
    case XKB_KEY_KP_Page_Down:
        return BOXRIG_KEY_KP_PAGE_DOWN;
    case XKB_KEY_KP_Home:
        return BOXRIG_KEY_KP_HOME;
    case XKB_KEY_KP_End:
        return BOXRIG_KEY_KP_END;
    case XKB_KEY_KP_Insert:
        return BOXRIG_KEY_KP_INSERT;
    case XKB_KEY_KP_Delete:
        return BOXRIG_KEY_KP_DELETE;
    case XKB_KEY_KP_Begin:
        return BOXRIG_KEY_KP_BEGIN;
    case XKB_KEY_Shift_L:
        return BOXRIG_KEY_LEFT_SHIFT;
    case XKB_KEY_Control_L:
        return BOXRIG_KEY_LEFT_CONTROL;
    case XKB_KEY_Alt_L:
        return BOXRIG_KEY_LEFT_ALT;
    case XKB_KEY_Super_L:
        return BOXRIG_KEY_LEFT_SUPER;
    case XKB_KEY_Hyper_L:
        return BOXRIG_KEY_LEFT_HYPER;
    case XKB_KEY_Meta_L:
        return BOXRIG_KEY_LEFT_META;
    case XKB_KEY_Shift_R:
        return BOXRIG_KEY_RIGHT_SHIFT;
    case XKB_KEY_Control_R:
        return BOXRIG_KEY_RIGHT_CONTROL;
    case XKB_KEY_Alt_R:
        return BOXRIG_KEY_RIGHT_ALT;
    case XKB_KEY_Super_R:
        return BOXRIG_KEY_RIGHT_SUPER;
    case XKB_KEY_Hyper_R:
        return BOXRIG_KEY_RIGHT_HYPER;
    case XKB_KEY_Meta_R:
        return BOXRIG_KEY_RIGHT_META;
    case XKB_KEY_ISO_Level3_Shift:
        return BOXRIG_KEY_ISO_LEVEL3_SHIFT;
    case XKB_KEY_ISO_Level5_Shift:
        return BOXRIG_KEY_ISO_LEVEL5_SHIFT;
    case XKB_KEY_XF86AudioPlay:
        return BOXRIG_KEY_MEDIA_PLAY;
    case XKB_KEY_XF86AudioPause:
        return BOXRIG_KEY_MEDIA_PAUSE;
    case XKB_KEY_XF86AudioStop:
        return BOXRIG_KEY_MEDIA_STOP;
    case XKB_KEY_XF86AudioForward:
        return BOXRIG_KEY_MEDIA_FAST_FORWARD;
    case XKB_KEY_XF86AudioRewind:
        return BOXRIG_KEY_MEDIA_REWIND;
    case XKB_KEY_XF86AudioNext:
        return BOXRIG_KEY_MEDIA_TRACK_NEXT;
    case XKB_KEY_XF86AudioPrev:
        return BOXRIG_KEY_MEDIA_TRACK_PREVIOUS;
    case XKB_KEY_XF86AudioRecord:
        return BOXRIG_KEY_MEDIA_RECORD;
    case XKB_KEY_XF86AudioLowerVolume:
        return BOXRIG_KEY_LOWER_VOLUME;
    case XKB_KEY_XF86AudioRaiseVolume:
        return BOXRIG_KEY_RAISE_VOLUME;
    case XKB_KEY_XF86AudioMute:
        return BOXRIG_KEY_MUTE_VOLUME;
    default:
        return BOXRIG_KEY_NONE;
    }
}

static uint32_t modifier_for_key(boxrig_key key)
{
    switch (key) {
    case BOXRIG_KEY_LEFT_SHIFT:
    case BOXRIG_KEY_RIGHT_SHIFT:
        return BOXRIG_MODIFIER_SHIFT;
    case BOXRIG_KEY_LEFT_CONTROL:
    case BOXRIG_KEY_RIGHT_CONTROL:
        return BOXRIG_MODIFIER_CONTROL;
    case BOXRIG_KEY_LEFT_ALT:
    case BOXRIG_KEY_RIGHT_ALT:
    case BOXRIG_KEY_ISO_LEVEL3_SHIFT:
    case BOXRIG_KEY_ISO_LEVEL5_SHIFT:
        return BOXRIG_MODIFIER_ALT;
    case BOXRIG_KEY_LEFT_SUPER:
    case BOXRIG_KEY_RIGHT_SUPER:
        return BOXRIG_MODIFIER_SUPER;
    case BOXRIG_KEY_LEFT_HYPER:
    case BOXRIG_KEY_RIGHT_HYPER:
        return BOXRIG_MODIFIER_HYPER;
    case BOXRIG_KEY_LEFT_META:
    case BOXRIG_KEY_RIGHT_META:
        return BOXRIG_MODIFIER_META;
    case BOXRIG_KEY_CAPS_LOCK:
        return BOXRIG_MODIFIER_CAPS_LOCK;
    case BOXRIG_KEY_NUM_LOCK:
        return BOXRIG_MODIFIER_NUM_LOCK;
    default:
        return 0;
    }
}

static bool xkb_modifier_active(
    struct xkb_state *state,
    const char *name)
{
    return xkb_state_mod_name_is_active(
               state,
               name,
               XKB_STATE_MODS_EFFECTIVE) > 0;
}

static uint32_t translate_modifiers(struct xkb_state *state)
{
    uint32_t modifiers = 0;

    if (xkb_modifier_active(state, XKB_MOD_NAME_SHIFT)) {
        modifiers |= BOXRIG_MODIFIER_SHIFT;
    }
    if (xkb_modifier_active(state, XKB_MOD_NAME_CTRL)) {
        modifiers |= BOXRIG_MODIFIER_CONTROL;
    }
    if (xkb_modifier_active(state, XKB_MOD_NAME_ALT)) {
        modifiers |= BOXRIG_MODIFIER_ALT;
    }
    if (xkb_modifier_active(state, XKB_MOD_NAME_LOGO)) {
        modifiers |= BOXRIG_MODIFIER_SUPER;
    }
    if (xkb_modifier_active(state, XKB_MOD_NAME_CAPS)) {
        modifiers |= BOXRIG_MODIFIER_CAPS_LOCK;
    }
    if (xkb_modifier_active(state, XKB_MOD_NAME_NUM)) {
        modifiers |= BOXRIG_MODIFIER_NUM_LOCK;
    }
    if (xkb_modifier_active(state, "Hyper")) {
        modifiers |= BOXRIG_MODIFIER_HYPER;
    }
    if (xkb_modifier_active(state, "Meta")) {
        modifiers |= BOXRIG_MODIFIER_META;
    }
    return modifiers;
}

static uint32_t scalar_for_level(
    struct xkb_keymap *keymap,
    xkb_keycode_t keycode,
    xkb_layout_index_t layout,
    xkb_level_index_t level)
{
    const xkb_keysym_t *symbols = NULL;

    if (xkb_keymap_key_get_syms_by_level(
            keymap,
            keycode,
            layout,
            level,
            &symbols) != 1) {
        return 0;
    }
    return xkb_keysym_to_utf32(symbols[0]);
}

static uint32_t base_layout_codepoint(
    struct xkb_keymap *keymap,
    xkb_keycode_t keycode)
{
    xkb_layout_index_t layouts = xkb_keymap_num_layouts_for_key(
        keymap,
        keycode);
    uint32_t candidate = 0;
    xkb_layout_index_t layout;

    for (layout = 0; layout < layouts; layout++) {
        uint32_t codepoint = scalar_for_level(keymap, keycode, layout, 0);

        if (codepoint > 0x7f) {
            continue;
        }
        if (candidate != 0 && candidate != codepoint) {
            return 0;
        }
        candidate = codepoint;
    }
    return candidate;
}

static bool copy_xkb_text(
    boxrig_wayland_seat *seat,
    xkb_keycode_t keycode,
    xkb_keysym_t symbol,
    bool compose,
    char **text_out,
    size_t *length_out)
{
    int length;
    char *text;

    *text_out = NULL;
    *length_out = 0;
    if (compose && seat->compose_state != NULL) {
        enum xkb_compose_feed_result feed = xkb_compose_state_feed(
            seat->compose_state,
            symbol);

        if (feed == XKB_COMPOSE_FEED_ACCEPTED) {
            enum xkb_compose_status status = xkb_compose_state_get_status(
                seat->compose_state);

            if (status == XKB_COMPOSE_COMPOSING) {
                return true;
            }
            if (status == XKB_COMPOSE_COMPOSED) {
                length = xkb_compose_state_get_utf8(
                    seat->compose_state,
                    NULL,
                    0);
                if (length < 0) {
                    return false;
                }
                text = malloc((size_t)length + 1);
                if (text == NULL) {
                    return false;
                }
                if (xkb_compose_state_get_utf8(
                        seat->compose_state,
                        text,
                        (size_t)length + 1) != length) {
                    free(text);
                    return false;
                }
                *text_out = text;
                *length_out = (size_t)length;
                xkb_compose_state_reset(seat->compose_state);
                return true;
            }
            if (status == XKB_COMPOSE_CANCELLED) {
                xkb_compose_state_reset(seat->compose_state);
            }
        }
    }
    length = xkb_state_key_get_utf8(seat->xkb_state, keycode, NULL, 0);
    if (length <= 0) {
        return length == 0;
    }
    text = malloc((size_t)length + 1);
    if (text == NULL) {
        return false;
    }
    if (xkb_state_key_get_utf8(
            seat->xkb_state,
            keycode,
            text,
            (size_t)length + 1) != length) {
        free(text);
        return false;
    }
    *text_out = text;
    *length_out = (size_t)length;
    return true;
}

static void clear_key_state(
    boxrig_wayland_seat *seat,
    uint32_t native_code)
{
    boxrig_wayland_pressed_key *key = &seat->keys[native_code];

    if (key->active && key->component != NULL &&
        key->component->pressed_key_count != 0) {
        key->component->pressed_key_count--;
    }
    free(key->text);
    memset(key, 0, sizeof(*key));
    if (seat->repeat_active && seat->repeat_key == native_code) {
        seat->repeat_active = false;
        seat->repeat_component = NULL;
    }
}

static void clear_seat_keys(boxrig_wayland_seat *seat)
{
    uint32_t key;

    for (key = 0; key < 256; key++) {
        clear_key_state(seat, key);
    }
    memset(seat->native_pressed, 0, sizeof(seat->native_pressed));
    if (seat->compose_state != NULL) {
        xkb_compose_state_reset(seat->compose_state);
    }
}

static uint64_t saturating_add(uint64_t left, uint64_t right)
{
    return left > UINT64_MAX - right ? UINT64_MAX : left + right;
}

static uint64_t repeat_interval_offset(
    uint64_t index,
    uint32_t rate)
{
    uint64_t seconds = index / rate;
    uint64_t remainder = index % rate;
    uint64_t milliseconds;

    if (seconds > UINT64_MAX / 1000) {
        return UINT64_MAX;
    }
    milliseconds = seconds * 1000;
    return saturating_add(
        milliseconds,
        (remainder * 1000 + rate - 1) / rate);
}

static void advance_repeat_deadline(boxrig_wayland_seat *seat)
{
    uint64_t first = saturating_add(
        seat->repeat_start,
        (uint64_t)seat->repeat_delay);

    seat->repeat_deadline = saturating_add(
        first,
        repeat_interval_offset(
            seat->repeat_index,
            (uint32_t)seat->repeat_rate));
}

static void start_repeat(
    boxrig_wayland_seat *seat,
    boxrig_wayland_terminal_component *component,
    uint32_t native_code)
{
    if (!seat->keys[native_code].repeatable || seat->repeat_rate <= 0 ||
        seat->repeat_delay < 0 || !seat->context->monotonic_time_set) {
        return;
    }
    seat->repeat_active = true;
    seat->repeat_key = native_code;
    seat->repeat_component = component;
    seat->repeat_start = seat->context->monotonic_milliseconds;
    seat->repeat_index = 0;
    advance_repeat_deadline(seat);
}

static void keyboard_keymap(
    void *data,
    struct wl_keyboard *keyboard,
    uint32_t format,
    int32_t descriptor,
    uint32_t size)
{
    boxrig_wayland_seat *seat = data;
    struct xkb_keymap *keymap = NULL;
    struct xkb_state *state = NULL;
    struct xkb_compose_state *compose_state = NULL;
    void *mapping = MAP_FAILED;

    (void)keyboard;
    if (seat->context->xkb_context != NULL &&
        format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size != 0 &&
        size <= seat->context->config.maximum_keymap_bytes) {
        mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, descriptor, 0);
        if (mapping != MAP_FAILED) {
            keymap = xkb_keymap_new_from_buffer(
                seat->context->xkb_context,
                mapping,
                size,
                XKB_KEYMAP_FORMAT_TEXT_V1,
                XKB_KEYMAP_COMPILE_NO_FLAGS);
            if (keymap != NULL) {
                state = xkb_state_new(keymap);
            }
            if (state != NULL && seat->context->compose_table != NULL) {
                compose_state = xkb_compose_state_new(
                    seat->context->compose_table,
                    XKB_COMPOSE_STATE_NO_FLAGS);
            }
        }
    }
    if (mapping != MAP_FAILED) {
        munmap(mapping, size);
    }
    close(descriptor);
    clear_seat_keys(seat);
    if (seat->compose_state != NULL) {
        xkb_compose_state_unref(seat->compose_state);
    }
    if (seat->xkb_state != NULL) {
        xkb_state_unref(seat->xkb_state);
    }
    if (seat->keymap != NULL) {
        xkb_keymap_unref(seat->keymap);
    }
    seat->keymap = keymap;
    seat->xkb_state = state;
    seat->compose_state = compose_state;
    seat->keymap_available = keymap != NULL && state != NULL;
    if (!seat->keymap_available) {
        seat->context->keymap_failure_count++;
        if (compose_state != NULL) {
            xkb_compose_state_unref(compose_state);
            seat->compose_state = NULL;
        }
        if (state != NULL) {
            xkb_state_unref(state);
            seat->xkb_state = NULL;
        }
        if (keymap != NULL) {
            xkb_keymap_unref(keymap);
            seat->keymap = NULL;
        }
    }
}

static boxrig_wayland_terminal_component *find_reparent_destination(
    boxrig_wayland_context *context, struct wl_surface *surface)
{
    for (boxrig_wayland_terminal_component *component = context->components;
         component != NULL; component = component->next) {
        if (component->running && component->reparent_pending &&
            component->config.keyboard_focus_fallback && component->parent == surface)
            return component;
    }
    return NULL;
}

static void keyboard_enter(
    void *data,
    struct wl_keyboard *keyboard,
    uint32_t serial,
    struct wl_surface *surface,
    struct wl_array *keys)
{
    boxrig_wayland_seat *seat = data;
    boxrig_wayland_terminal_component *component = find_keyboard_component(
        seat->context,
        surface);

    (void)keyboard;
    (void)keys;
    seat->reparent_keyboard_component = find_reparent_destination(seat->context, surface);
    seat->reparent_keyboard_serial = serial;
    set_serial(
        seat,
        BOXRIG_WAYLAND_SERIAL_KEYBOARD_ENTER,
        serial,
        component);
    if (seat->keyboard_component == component) {
        return;
    }
    if (seat->keyboard_component != NULL) {
        boxrig_wayland_terminal_component *previous =
            seat->keyboard_component;

        seat->serial_valid[BOXRIG_WAYLAND_SERIAL_KEYBOARD_KEY] = false;
        seat->serial_components[BOXRIG_WAYLAND_SERIAL_KEYBOARD_KEY] = NULL;
        clear_seat_keys(seat);
        boxrig_wayland_ibus_focus(seat, NULL, true);
        seat->keyboard_component = NULL;
        if (previous->keyboard_focus_count != 0) {
            previous->keyboard_focus_count--;
            if (previous->keyboard_focus_count == 0) {
                (void)queue_focus(seat, previous, false);
            }
        }
    }
    seat->keyboard_component = component;
    boxrig_wayland_ibus_focus(seat, component, true);
    if (component != NULL) {
        if (component->keyboard_focus_count++ == 0) {
            (void)queue_focus(seat, component, true);
        }
    }
}

static void keyboard_leave(
    void *data,
    struct wl_keyboard *keyboard,
    uint32_t serial,
    struct wl_surface *surface)
{
    boxrig_wayland_seat *seat = data;
    boxrig_wayland_terminal_component *component = find_keyboard_component(
        seat->context,
        surface);

    (void)keyboard;
    (void)serial;
    if (seat->reparent_keyboard_component != NULL &&
        seat->reparent_keyboard_component->parent == surface) {
        clear_seat_keys(seat);
        seat->reparent_keyboard_component = NULL;
    }
    if (component == NULL || seat->keyboard_component != component) {
        return;
    }
    seat->serial_valid[BOXRIG_WAYLAND_SERIAL_KEYBOARD_ENTER] = false;
    seat->serial_components[BOXRIG_WAYLAND_SERIAL_KEYBOARD_ENTER] = NULL;
    seat->serial_valid[BOXRIG_WAYLAND_SERIAL_KEYBOARD_KEY] = false;
    seat->serial_components[BOXRIG_WAYLAND_SERIAL_KEYBOARD_KEY] = NULL;
    clear_seat_keys(seat);
    boxrig_wayland_ibus_focus(seat, NULL, true);
    seat->keyboard_component = NULL;
    if (component->keyboard_focus_count != 0) {
        component->keyboard_focus_count--;
        if (component->keyboard_focus_count == 0) {
            (void)queue_focus(seat, component, false);
        }
    }
}

static void keyboard_key(
    void *data,
    struct wl_keyboard *keyboard,
    uint32_t serial,
    uint32_t time,
    uint32_t native_code,
    uint32_t state)
{
    boxrig_wayland_seat *seat = data;
    boxrig_wayland_terminal_component *component =
        seat->keyboard_component;
    boxrig_wayland_input_item item;
    xkb_keycode_t keycode;
    xkb_layout_index_t layout;
    xkb_keysym_t symbol;
    uint32_t changed_modifier;

    (void)keyboard;
    if (component == NULL && seat->reparent_keyboard_component != NULL &&
        native_code < 248 &&
        (state == WL_KEYBOARD_KEY_STATE_PRESSED || state == WL_KEYBOARD_KEY_STATE_RELEASED)) {
        /* Remember a suppressed press until its release, including when the
         * parent acknowledgement arrives between the two native events. */
        bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
        seat->native_pressed[native_code] = pressed;
        seat->inhibited_key[native_code] = pressed;
        return;
    }
    if (component == NULL || !seat->keymap_available || native_code >= 248 ||
        (state != WL_KEYBOARD_KEY_STATE_PRESSED &&
         state != WL_KEYBOARD_KEY_STATE_RELEASED)) {
        if (component != NULL) {
            component->input_error_count++;
        }
        return;
    }
    set_serial(
        seat,
        BOXRIG_WAYLAND_SERIAL_KEYBOARD_KEY,
        serial,
        component);
    keycode = (xkb_keycode_t)(native_code + 8);
    memset(&item, 0, sizeof(item));
    item.type = BOXRIG_WAYLAND_INPUT_KEY;
    item.seat = seat;
    item.native_code = native_code;
    item.ime_time = time;
    item.inhibited_key = !input_enabled(component) || seat->inhibited_key[native_code];
    seat->native_pressed[native_code] = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED && !input_enabled(component))
        seat->inhibited_key[native_code] = true;
    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) seat->inhibited_key[native_code] = false;
    symbol = xkb_state_key_get_one_sym(seat->xkb_state, keycode);
    item.keysym = symbol;
    item.ime_modifiers = boxrig_wayland_ibus_modifiers(seat, symbol, state == WL_KEYBOARD_KEY_STATE_RELEASED);
    item.ime_generation = seat->ime_generation;
    item.compose_pending = !item.inhibited_key && boxrig_wayland_ibus_selected(seat->context) &&
        state == WL_KEYBOARD_KEY_STATE_PRESSED;
    item.value.key.action = state == WL_KEYBOARD_KEY_STATE_PRESSED
                                ? BOXRIG_KEY_PRESS
                                : BOXRIG_KEY_RELEASE;
    if (state == WL_KEYBOARD_KEY_STATE_RELEASED &&
        seat->keys[native_code].active) {
        item.keysym = seat->keys[native_code].keysym;
        item.value.key = seat->keys[native_code].identity;
        item.value.key.action = BOXRIG_KEY_RELEASE;
    } else {
        symbol = xkb_state_key_get_one_sym(seat->xkb_state, keycode);
        item.value.key.key = translate_functional_key(symbol);
        layout = xkb_state_key_get_layout(seat->xkb_state, keycode);
        if (layout == XKB_LAYOUT_INVALID) {
            layout = 0;
        }
        if (item.value.key.key == BOXRIG_KEY_NONE) {
            item.value.key.codepoint = scalar_for_level(
                seat->keymap,
                keycode,
                layout,
                0);
            item.value.key.shifted_codepoint = scalar_for_level(
                seat->keymap,
                keycode,
                layout,
                1);
            if (item.value.key.shifted_codepoint ==
                item.value.key.codepoint) {
                item.value.key.shifted_codepoint = 0;
            }
            item.value.key.base_layout_codepoint = base_layout_codepoint(
                seat->keymap,
                keycode);
        }
        item.value.key.modifiers = translate_modifiers(seat->xkb_state);
        changed_modifier = modifier_for_key(item.value.key.key);
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            item.value.key.modifiers |= changed_modifier;
        } else {
            item.value.key.modifiers &= ~changed_modifier;
        }
        /* Forwarded keyboard events were not consumed by the compositor's
         * input method, even while text-input-v3 is enabled. */
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED &&
            item.value.key.key == BOXRIG_KEY_NONE &&
            !copy_xkb_text(
                seat,
                keycode,
                symbol,
                !item.inhibited_key && !boxrig_wayland_ibus_selected(seat->context),
                &item.text,
                &item.value.key.text_length)) {
            component->input_error_count++;
            return;
        }
        item.value.key.text = item.text;
    }
    (void)boxrig_wayland_input_enqueue(component, &item);
}

static void keyboard_modifiers(
    void *data,
    struct wl_keyboard *keyboard,
    uint32_t serial,
    uint32_t depressed,
    uint32_t latched,
    uint32_t locked,
    uint32_t group)
{
    boxrig_wayland_seat *seat = data;

    (void)keyboard;
    (void)serial;
    if (seat->xkb_state != NULL) {
        (void)xkb_state_update_mask(
            seat->xkb_state,
            depressed,
            latched,
            locked,
            0,
            0,
            group);
    }
}

static void keyboard_repeat_info(
    void *data,
    struct wl_keyboard *keyboard,
    int32_t rate,
    int32_t delay)
{
    boxrig_wayland_seat *seat = data;

    (void)keyboard;
    seat->repeat_rate = rate;
    seat->repeat_delay = delay;
    if (rate <= 0 || delay < 0) {
        seat->repeat_active = false;
        seat->repeat_component = NULL;
    }
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void clear_pointer_axis(boxrig_wayland_seat *seat);

static size_t count_pressed_pointer_buttons(uint32_t buttons)
{
    size_t count = 0;

    while (buttons != 0) {
        count += buttons & 1u;
        buttons >>= 1;
    }
    return count;
}

static boxrig_ui_status synchronize_scrollbar_interaction(boxrig_wayland_terminal_component *component)
{
    bool hovered = false;
    bool dragging = false;
    for (boxrig_wayland_seat *seat = component->context->seats; seat != NULL; seat = seat->next) {
        if (seat->pointer_component == component) {
            hovered |= seat->scrollbar_hovered;
            dragging |= seat->scrollbar_dragging;
        }
    }
    return boxrig_wayland_component_scrollbar_interaction(component, hovered, dragging);
}

void boxrig_wayland_input_cancel_scrollbar(
    boxrig_wayland_terminal_component *component, boxrig_wayland_seat *owner)
{
    for (boxrig_wayland_seat *seat = component->context->seats; seat != NULL; seat = seat->next) {
        if (seat->pointer_component == component && (owner == NULL || owner == seat)) {
            seat->scrollbar_dragging = false;
            seat->scrollbar_hovered = false;
        }
    }
    if (synchronize_scrollbar_interaction(component) != BOXRIG_UI_OK)
        component->failure_reason = BOXRIG_WAYLAND_FAILURE_INPUT;
}

static void clear_pointer_focus(boxrig_wayland_seat *seat)
{
    boxrig_wayland_terminal_component *component = seat->pointer_component;
    size_t pressed = count_pressed_pointer_buttons(seat->pointer_buttons);

    if (component != NULL) {
        boxrig_wayland_input_cancel_scrollbar(component, seat);
        if (component->pointer_focus_count != 0) {
            component->pointer_focus_count--;
        }
        if (pressed > component->pressed_button_count) {
            component->pressed_button_count = 0;
            component->failure_reason = BOXRIG_WAYLAND_FAILURE_INPUT;
        } else {
            component->pressed_button_count -= pressed;
        }
    }
    seat->pointer_component = NULL;
    seat->serial_valid[BOXRIG_WAYLAND_SERIAL_POINTER_ENTER] = false;
    seat->serial_valid[BOXRIG_WAYLAND_SERIAL_POINTER_BUTTON] = false;
    seat->serial_components[BOXRIG_WAYLAND_SERIAL_POINTER_ENTER] = NULL;
    seat->serial_components[BOXRIG_WAYLAND_SERIAL_POINTER_BUTTON] = NULL;
    seat->pointer_buttons = 0;
    seat->pointer_application_buttons = 0;
    seat->pointer_candidate_buttons = 0;
    seat->pointer_scrollbar_buttons = 0;
    seat->pointer_generation++;
    seat->last_click_valid = false;
    clear_pointer_axis(seat);
}

static void pointer_enter(
    void *data,
    struct wl_pointer *pointer,
    uint32_t serial,
    struct wl_surface *surface,
    wl_fixed_t x,
    wl_fixed_t y)
{
    boxrig_wayland_seat *seat = data;
    boxrig_wayland_terminal_component *component = find_component(
        seat->context,
        surface);

    (void)pointer;
    if (seat->pointer_component != component) {
        clear_pointer_focus(seat);
        seat->pointer_component = component;
        if (component != NULL) {
            component->pointer_focus_count++;
        }
    }
    set_serial(
        seat,
        BOXRIG_WAYLAND_SERIAL_POINTER_ENTER,
        serial,
        component);
    seat->pointer_x = x;
    seat->pointer_y = y;
}

static void pointer_leave(
    void *data,
    struct wl_pointer *pointer,
    uint32_t serial,
    struct wl_surface *surface)
{
    boxrig_wayland_seat *seat = data;
    boxrig_wayland_terminal_component *component = find_component(
        seat->context,
        surface);

    (void)pointer;
    (void)serial;
    if (component != NULL && seat->pointer_component == component) {
        clear_pointer_focus(seat);
    }
}

static boxrig_mouse_button translate_pointer_button(uint32_t button)
{
    switch (button) {
    case BTN_LEFT:
        return BOXRIG_MOUSE_BUTTON_LEFT;
    case BTN_MIDDLE:
        return BOXRIG_MOUSE_BUTTON_MIDDLE;
    case BTN_RIGHT:
        return BOXRIG_MOUSE_BUTTON_RIGHT;
    case BTN_SIDE:
        return BOXRIG_MOUSE_BUTTON_8;
    case BTN_EXTRA:
        return BOXRIG_MOUSE_BUTTON_9;
    case BTN_FORWARD:
        return BOXRIG_MOUSE_BUTTON_10;
    case BTN_BACK:
        return BOXRIG_MOUSE_BUTTON_11;
    default:
        return BOXRIG_MOUSE_BUTTON_NONE;
    }
}

static uint32_t pointer_button_mask(boxrig_mouse_button button)
{
    switch (button) {
    case BOXRIG_MOUSE_BUTTON_LEFT:
        return BOXRIG_MOUSE_BUTTON_MASK_LEFT;
    case BOXRIG_MOUSE_BUTTON_MIDDLE:
        return BOXRIG_MOUSE_BUTTON_MASK_MIDDLE;
    case BOXRIG_MOUSE_BUTTON_RIGHT:
        return BOXRIG_MOUSE_BUTTON_MASK_RIGHT;
    case BOXRIG_MOUSE_BUTTON_8:
        return BOXRIG_MOUSE_BUTTON_MASK_8;
    case BOXRIG_MOUSE_BUTTON_9:
        return BOXRIG_MOUSE_BUTTON_MASK_9;
    case BOXRIG_MOUSE_BUTTON_10:
        return BOXRIG_MOUSE_BUTTON_MASK_10;
    case BOXRIG_MOUSE_BUTTON_11:
        return BOXRIG_MOUSE_BUTTON_MASK_11;
    default:
        return 0;
    }
}

/* Floor both conversions so a negative subpixel stays outside the content. */
static int64_t divide_pointer_coordinate(int64_t value, int64_t divisor)
{
    return value / divisor - (value % divisor < 0 ? 1 : 0);
}

static bool make_pointer_event(
    boxrig_wayland_seat *seat,
    boxrig_mouse_action action,
    boxrig_mouse_button button,
    boxrig_mouse_event *event_out,
    boxrig_ui_hit_test_result *hit_out)
{
    boxrig_wayland_terminal_component *component = seat->pointer_component;
    boxrig_ui_hit_test_result hit;
    boxrig_ui_fixed fixed_x;
    boxrig_ui_fixed fixed_y;

    if (component == NULL || hit_out == NULL || !component->allocated ||
        component->scale_numerator == 0) {
        return false;
    }
    fixed_x = divide_pointer_coordinate((int64_t)seat->pointer_x * component->scale_numerator, 480);
    fixed_y = divide_pointer_coordinate((int64_t)seat->pointer_y * component->scale_numerator, 480);
    memset(event_out, 0, sizeof(*event_out));
    event_out->action = action;
    event_out->button = button;
    event_out->pressed_buttons = seat->pointer_buttons;
    if (seat->xkb_state != NULL) {
        event_out->modifiers = translate_modifiers(seat->xkb_state);
    }
    event_out->x = divide_pointer_coordinate(fixed_x, BOXRIG_UI_FIXED_ONE);
    event_out->y = divide_pointer_coordinate(fixed_y, BOXRIG_UI_FIXED_ONE);
    if (boxrig_ui_view_hit_test_with_composition(
            component->view,
            component->composition,
            fixed_x,
            fixed_y,
            &hit) != BOXRIG_UI_OK) {
        return false;
    }
    if (hit.inside_grid) {
        event_out->row = hit.row;
        event_out->column = hit.visual_column;
    }
    *hit_out = hit;
    return true;
}

static bool queue_pointer_event(
    boxrig_wayland_seat *seat,
    boxrig_mouse_action action,
    boxrig_mouse_button button,
    uint8_t click_count)
{
    boxrig_wayland_input_item item;

    memset(&item, 0, sizeof(item));
    item.type = BOXRIG_WAYLAND_INPUT_MOUSE;
    item.seat = seat;
    item.native_code = pointer_button_mask(button);
    if (!make_pointer_event(
            seat,
            action,
            button,
            &item.value.mouse,
            &item.mouse_hit)) {
        if (seat->pointer_component != NULL) {
            seat->pointer_component->input_error_count++;
        }
        return false;
    }
    item.value.mouse.click_count = click_count;
    if (action == BOXRIG_MOUSE_PRESS) seat->pointer_candidate_buttons &= ~item.native_code;
    if (action == BOXRIG_MOUSE_RELEASE &&
        (seat->pointer_candidate_buttons & item.native_code) != 0) {
        seat->pointer_candidate_buttons &= ~item.native_code;
        return true;
    }
    /* The engine owns selection. The overlay consumes pointer interaction,
     * including a captured drag after it leaves the panel or the panel hides.
     * A terminal-owned release remains paired with its earlier press. */
    if ((item.native_code == 0 && seat->pointer_candidate_buttons != 0) ||
        ((item.mouse_hit.overlay_roles & BOXRIG_UI_OVERLAY_CANDIDATES) != 0 &&
            (action != BOXRIG_MOUSE_RELEASE || item.native_code == 0))) {
        if (action == BOXRIG_MOUSE_PRESS) seat->pointer_candidate_buttons |= item.native_code;
        return true;
    }
    boxrig_wayland_terminal_component *component = seat->pointer_component;
    unsigned int width = component->scrollbar_visibility.state.reserved_width;
    int64_t left = (int64_t)component->buffer_width - width;
    if (left < 0) left = 0;
    item.scrollbar_hit = width != 0 && item.value.mouse.x >= left &&
        item.value.mouse.x < component->buffer_width && item.value.mouse.y >= 0 &&
        item.value.mouse.y < component->buffer_height;
    item.pointer_generation = seat->pointer_generation;
    return boxrig_wayland_input_enqueue(seat->pointer_component, &item);
}

static uint8_t pointer_click_count(
    boxrig_wayland_seat *seat,
    boxrig_mouse_button button,
    uint32_t time)
{
    boxrig_wayland_terminal_component *component = seat->pointer_component;
    boxrig_mouse_event event;
    boxrig_ui_hit_test_result hit;
    uint8_t count = 1;
    uint32_t elapsed = time - seat->last_click_time;
    uint64_t distance_x;
    uint64_t distance_y;

    if (!make_pointer_event(
            seat,
            BOXRIG_MOUSE_PRESS,
            button,
            &event,
            &hit)) {
        return 1;
    }
    distance_x = event.x >= seat->last_click_x
                     ? (uint64_t)(event.x - seat->last_click_x)
                     : (uint64_t)(seat->last_click_x - event.x);
    distance_y = event.y >= seat->last_click_y
                     ? (uint64_t)(event.y - seat->last_click_y)
                     : (uint64_t)(seat->last_click_y - event.y);
    if (seat->last_click_valid && seat->last_click_button == button &&
        elapsed <= component->config.multi_click_interval_milliseconds &&
        distance_x <= component->config.multi_click_distance &&
        distance_y <= component->config.multi_click_distance &&
        seat->last_click_count < 3) {
        count = (uint8_t)(seat->last_click_count + 1);
    }
    seat->last_click_time = time;
    seat->last_click_x = event.x;
    seat->last_click_y = event.y;
    seat->last_click_button = button;
    seat->last_click_count = count;
    seat->last_click_valid = true;
    return count;
}

static void pointer_motion(
    void *data,
    struct wl_pointer *pointer,
    uint32_t time,
    wl_fixed_t x,
    wl_fixed_t y)
{
    boxrig_wayland_seat *seat = data;

    (void)pointer;
    (void)time;
    seat->pointer_x = x;
    seat->pointer_y = y;
    (void)queue_pointer_event(
        seat,
        BOXRIG_MOUSE_MOTION,
        BOXRIG_MOUSE_BUTTON_NONE,
        0);
}

static void pointer_button(
    void *data,
    struct wl_pointer *pointer,
    uint32_t serial,
    uint32_t time,
    uint32_t button,
    uint32_t state)
{
    boxrig_wayland_seat *seat = data;
    boxrig_wayland_terminal_component *component = seat->pointer_component;
    boxrig_mouse_button logical = translate_pointer_button(button);
    uint32_t mask = pointer_button_mask(logical);
    uint8_t click_count = 0;

    (void)pointer;
    set_serial(
        seat,
        BOXRIG_WAYLAND_SERIAL_POINTER_BUTTON,
        serial,
        component);
    if (component == NULL || logical == BOXRIG_MOUSE_BUTTON_NONE || mask == 0 ||
        (state != WL_POINTER_BUTTON_STATE_PRESSED &&
         state != WL_POINTER_BUTTON_STATE_RELEASED)) {
        if (component != NULL) {
            component->input_error_count++;
        }
        return;
    }
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if ((seat->pointer_buttons & mask) != 0) {
            component->input_error_count++;
            return;
        }
        seat->pointer_buttons |= mask;
        component->pressed_button_count++;
        click_count = pointer_click_count(seat, logical, time);
        (void)queue_pointer_event(
            seat,
            BOXRIG_MOUSE_PRESS,
            logical,
            click_count);
    } else {
        if ((seat->pointer_buttons & mask) == 0) {
            component->input_error_count++;
            return;
        }
        seat->pointer_buttons &= ~mask;
        if (component->pressed_button_count != 0) {
            component->pressed_button_count--;
        }
        (void)queue_pointer_event(
            seat,
            BOXRIG_MOUSE_RELEASE,
            logical,
            0);
    }
}

static void emit_pointer_axis_frame(boxrig_wayland_seat *seat);

static void pointer_axis(
    void *data,
    struct wl_pointer *pointer,
    uint32_t time,
    uint32_t axis,
    wl_fixed_t value)
{
    boxrig_wayland_seat *seat = data;

    (void)pointer;
    (void)time;
    if (axis > WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        if (seat->pointer_component != NULL) {
            seat->pointer_component->input_error_count++;
        }
        return;
    }
    if ((value > 0 && seat->axis_value[axis] > INT64_MAX - value) ||
        (value < 0 && seat->axis_value[axis] < INT64_MIN - value)) {
        if (seat->pointer_component != NULL) {
            seat->pointer_component->failure_reason =
                BOXRIG_WAYLAND_FAILURE_INPUT;
        }
        return;
    }
    seat->axis_value[axis] += value;
    seat->axis_set[axis] = true;
    if (seat->version < WL_POINTER_FRAME_SINCE_VERSION) {
        emit_pointer_axis_frame(seat);
    }
}

static void clear_pointer_axis(boxrig_wayland_seat *seat)
{
    size_t axis;

    for (axis = 0; axis < 2; axis++) {
        seat->axis_value120[axis] = 0;
        seat->axis_discrete[axis] = 0;
        seat->axis_value[axis] = 0;
        seat->axis_value120_set[axis] = false;
        seat->axis_discrete_set[axis] = false;
        seat->axis_set[axis] = false;
    }
}

static uint64_t signed_magnitude(int64_t value)
{
    return value < 0 ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
}

static void emit_pointer_axis_frame(boxrig_wayland_seat *seat)
{
    boxrig_wayland_terminal_component *component = seat->pointer_component;
    int64_t detents[2] = {0, 0};
    uint64_t event_count = 0;
    size_t axis;

    if (component == NULL) {
        clear_pointer_axis(seat);
        return;
    }
    for (axis = 0; axis < 2; axis++) {
        if (seat->axis_value120_set[axis]) {
            int64_t total;

            if ((seat->axis_value120_remainder[axis] > 0 &&
                 seat->axis_value120[axis] >
                     INT64_MAX - seat->axis_value120_remainder[axis]) ||
                (seat->axis_value120_remainder[axis] < 0 &&
                 seat->axis_value120[axis] <
                     INT64_MIN - seat->axis_value120_remainder[axis])) {
                component->failure_reason = BOXRIG_WAYLAND_FAILURE_INPUT;
                clear_pointer_axis(seat);
                return;
            }
            total = seat->axis_value120_remainder[axis] +
                    seat->axis_value120[axis];

            detents[axis] = total / 120;
            seat->axis_value120_remainder[axis] = total % 120;
        } else if (seat->axis_discrete_set[axis]) {
            detents[axis] = seat->axis_discrete[axis];
        } else if (seat->axis_set[axis] && seat->axis_value[axis] != 0) {
            detents[axis] = seat->axis_value[axis] > 0 ? 1 : -1;
        }
        if (signed_magnitude(detents[axis]) > UINT64_MAX / 2 - event_count) {
            component->failure_reason =
                BOXRIG_WAYLAND_FAILURE_INPUT_QUEUE_OVERFLOW;
            clear_pointer_axis(seat);
            return;
        }
        event_count += signed_magnitude(detents[axis]) * 2;
    }
    if (event_count > component->config.maximum_input_events -
                          component->input_queue_depth) {
        component->failure_reason =
            BOXRIG_WAYLAND_FAILURE_INPUT_QUEUE_OVERFLOW;
        clear_pointer_axis(seat);
        return;
    }
    for (axis = 0; axis < 2; axis++) {
        boxrig_mouse_button button;
        uint64_t count = signed_magnitude(detents[axis]);
        uint64_t index;

        if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
            button = detents[axis] < 0 ? BOXRIG_MOUSE_BUTTON_WHEEL_UP
                                      : BOXRIG_MOUSE_BUTTON_WHEEL_DOWN;
        } else {
            button = detents[axis] < 0 ? BOXRIG_MOUSE_BUTTON_WHEEL_LEFT
                                      : BOXRIG_MOUSE_BUTTON_WHEEL_RIGHT;
        }
        for (index = 0; index < count; index++) {
            (void)queue_pointer_event(seat, BOXRIG_MOUSE_PRESS, button, 0);
            (void)queue_pointer_event(seat, BOXRIG_MOUSE_RELEASE, button, 0);
        }
    }
    clear_pointer_axis(seat);
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)pointer;
    emit_pointer_axis_frame(data);
}

static void pointer_axis_source(
    void *data,
    struct wl_pointer *pointer,
    uint32_t source)
{
    boxrig_wayland_seat *seat = data;

    (void)pointer;
    if (source > WL_POINTER_AXIS_SOURCE_WHEEL_TILT &&
        seat->pointer_component != NULL) {
        seat->pointer_component->input_error_count++;
    }
}

static void pointer_axis_stop(
    void *data,
    struct wl_pointer *pointer,
    uint32_t time,
    uint32_t axis)
{
    boxrig_wayland_seat *seat = data;

    (void)pointer;
    (void)time;
    if (axis > WL_POINTER_AXIS_HORIZONTAL_SCROLL &&
        seat->pointer_component != NULL) {
        seat->pointer_component->input_error_count++;
    }
}

static void pointer_axis_discrete(
    void *data,
    struct wl_pointer *pointer,
    uint32_t axis,
    int32_t discrete)
{
    boxrig_wayland_seat *seat = data;

    (void)pointer;
    if (axis > WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        if (seat->pointer_component != NULL) {
            seat->pointer_component->input_error_count++;
        }
        return;
    }
    if ((discrete > 0 &&
         seat->axis_discrete[axis] > INT64_MAX - discrete) ||
        (discrete < 0 &&
         seat->axis_discrete[axis] < INT64_MIN - discrete)) {
        if (seat->pointer_component != NULL) {
            seat->pointer_component->failure_reason =
                BOXRIG_WAYLAND_FAILURE_INPUT;
        }
        return;
    }
    seat->axis_discrete[axis] += discrete;
    seat->axis_discrete_set[axis] = true;
}

static void pointer_axis_value120(
    void *data,
    struct wl_pointer *pointer,
    uint32_t axis,
    int32_t value120)
{
    boxrig_wayland_seat *seat = data;

    (void)pointer;
    if (axis > WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        if (seat->pointer_component != NULL) {
            seat->pointer_component->input_error_count++;
        }
        return;
    }
    if ((value120 > 0 &&
         seat->axis_value120[axis] > INT64_MAX - value120) ||
        (value120 < 0 &&
         seat->axis_value120[axis] < INT64_MIN - value120)) {
        if (seat->pointer_component != NULL) {
            seat->pointer_component->failure_reason =
                BOXRIG_WAYLAND_FAILURE_INPUT;
        }
        return;
    }
    seat->axis_value120[axis] += value120;
    seat->axis_value120_set[axis] = true;
}

static void pointer_axis_relative_direction(
    void *data,
    struct wl_pointer *pointer,
    uint32_t axis,
    uint32_t direction)
{
    (void)data;
    (void)pointer;
    (void)axis;
    (void)direction;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
    .axis_relative_direction = pointer_axis_relative_direction,
};

static void reset_text_batch(boxrig_wayland_seat *seat)
{
    free(seat->pending_preedit);
    seat->pending_preedit = NULL;
    seat->pending_preedit_length = 0;
    free(seat->pending_commit);
    seat->pending_commit = NULL;
    seat->pending_commit_length = 0;
    seat->pending_cursor_begin = 0;
    seat->pending_cursor_end = 0;
    seat->pending_hint_count = 0;
    seat->pending_hint_overflow = false;
    seat->pending_delete_before = 0;
    seat->pending_delete_after = 0;
    seat->pending_preedit_set = false;
    seat->pending_commit_set = false;
    seat->pending_delete_set = false;
    seat->pending_invalid = false;
}

static bool copy_text_event(
    boxrig_wayland_seat *seat,
    const char *source,
    char **destination,
    size_t *length_out)
{
    size_t length = source == NULL ? 0 : strlen(source);
    char *copy = NULL;

    if (length > seat->text_component->config.maximum_ime_bytes) {
        return false;
    }
    if (source != NULL) {
        copy = malloc(length + 1);
        if (copy == NULL) {
            return false;
        }
        memcpy(copy, source, length + 1);
    }
    free(*destination);
    *destination = copy;
    *length_out = length;
    return true;
}

static bool valid_utf8(const char *text, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        uint32_t scalar;
        size_t consumed;

        if (!boxrig_ui_utf8_decode_scalar(
                (const uint8_t *)text + offset,
                length - offset,
                &scalar,
                &consumed)) {
            return false;
        }
        offset += consumed;
    }
    return true;
}

static bool utf8_boundary(
    const char *text,
    size_t length,
    size_t offset)
{
    return offset <= length &&
           (offset == length ||
            (((const unsigned char *)text)[offset] & 0xc0) != 0x80);
}

static bool text_input_has_hints(const boxrig_wayland_seat *seat)
{
    return seat->text_input != NULL &&
        wl_proxy_get_version((struct wl_proxy *)seat->text_input) >= 2;
}

bool boxrig_wayland_input_prepare_hints(
    boxrig_wayland_terminal_component *component,
    const boxrig_wayland_preedit_hint *hints, size_t hint_count, bool overflow,
    boxrig_wayland_input_item *item)
{
    return boxrig_ime_prepare_hints(hints, hint_count, overflow,
        &item->value.composition, &item->segments, &component->input_error_count);
}

static bool prepare_composition_item(
    boxrig_wayland_seat *seat,
    boxrig_composition_event_type type,
    const char *text,
    size_t text_length,
    int32_t cursor_begin,
    int32_t cursor_end,
    boxrig_wayland_input_item *item)
{
    memset(item, 0, sizeof(*item));
    item->type = BOXRIG_WAYLAND_INPUT_COMPOSITION;
    item->seat = seat;
    item->text_epoch = seat->text_epoch;
    item->value.composition.type = type;
    if (text_length != 0) {
        item->text = malloc(text_length);
        if (item->text == NULL) {
            return false;
        }
        memcpy(item->text, text, text_length);
        item->value.composition.text = item->text;
        item->value.composition.text_length = text_length;
    }
    if (type == BOXRIG_COMPOSITION_PREEDIT_UPDATE) {
        if (text_length != 0 && !text_input_has_hints(seat)) {
            item->segments = malloc(sizeof(*item->segments));
            if (item->segments == NULL) {
                boxrig_wayland_input_free_item(item);
                return false;
            }
            item->segments[0].text_offset = 0;
            item->segments[0].text_length = text_length;
            item->segments[0].feedback =
                BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE;
            item->value.composition.segments = item->segments;
            item->value.composition.segment_count = 1;
        }
        if (cursor_begin == -1 && cursor_end == -1) {
            item->value.composition.caret_visible = false;
        } else {
            size_t begin = (size_t)cursor_begin;
            size_t end = (size_t)cursor_end;

            item->value.composition.selection_offset =
                begin < end ? begin : end;
            item->value.composition.selection_length =
                begin < end ? end - begin : begin - end;
            item->value.composition.caret_offset = end;
            item->value.composition.caret_visible = true;
        }
        if (text_length != 0 && text_input_has_hints(seat) &&
            !boxrig_wayland_input_prepare_hints(seat->text_component, seat->pending_hints,
                seat->pending_hint_count, seat->pending_hint_overflow, item)) {
            boxrig_wayland_input_free_item(item);
            return false;
        }
    }
    return true;
}

static void queue_composition_cancel(
    boxrig_wayland_seat *seat,
    boxrig_wayland_terminal_component *component)
{
    boxrig_wayland_input_item item;

    if (component == NULL) {
        return;
    }
    (void)boxrig_ui_editable_document_invalidate_input(
        boxrig_ui_view_editable_document(component->view));
    if (seat != NULL) seat->text_epoch++;
    memset(&item, 0, sizeof(item));
    item.type = BOXRIG_WAYLAND_INPUT_COMPOSITION;
    item.seat = seat;
    item.value.composition.type = BOXRIG_COMPOSITION_CANCEL;
    (void)boxrig_wayland_input_enqueue(component, &item);
}

bool boxrig_wayland_input_cursor_geometry(boxrig_wayland_terminal_component *component,
    bool placed, int32_t rectangle[4])
{
    boxrig_font_effective_metrics metrics;
    boxrig_composition_view composition;
    boxrig_cursor_view cursor;
    uint64_t physical_x;
    uint64_t physical_y;
    uint64_t physical_width;
    uint64_t physical_height;
    uint64_t logical_x;
    uint64_t logical_y;
    uint64_t logical_width;
    uint64_t logical_height;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    /* Applying preedit invalidates its placement until the renderer lays it
     * out. Publishing the terminal cursor in that gap makes compositor
     * feedback alternate between the anchor and the displayed preedit caret.
     * Other text-input state can still be committed while layout is pending. */
    bool composition_available =
        boxrig_composition_get_view(component->composition, &composition) ==
        BOXRIG_COMPOSITION_OK;
    if (component->allocated &&
        (placed || !composition_available ||
         !composition.active) &&
        boxrig_pango_cairo_renderer_get_effective_metrics(
            component->renderer,
            &metrics) == BOXRIG_FONT_CATALOG_OK &&
        metrics.cell_width != 0 && metrics.cell_height != 0) {
        if (composition_available && composition.candidate.visible) {
            physical_x = composition.candidate.exclude_x;
            physical_y = composition.candidate.exclude_y;
            physical_width = composition.candidate.exclude_width;
            physical_height = composition.candidate.exclude_height;
        } else {
            boxrig_terminal_get_cursor(component->terminal, &cursor);
            physical_x = cursor.column * (uint64_t)metrics.cell_width;
            physical_y = cursor.row * (uint64_t)metrics.cell_height;
            physical_width = metrics.cell_width;
            physical_height = metrics.cell_height;
        }
        logical_x = physical_x * 120 / component->scale_numerator;
        logical_y = physical_y * 120 / component->scale_numerator;
        logical_width =
            (physical_width * 120 + component->scale_numerator - 1) /
            component->scale_numerator;
        logical_height =
            (physical_height * 120 + component->scale_numerator - 1) /
            component->scale_numerator;
        if (logical_x <= INT32_MAX && logical_y <= INT32_MAX &&
            logical_width > 0 && logical_width <= INT32_MAX &&
            logical_height > 0 && logical_height <= INT32_MAX) {
            x = (int32_t)logical_x;
            y = (int32_t)logical_y;
            width = (int32_t)logical_width;
            height = (int32_t)logical_height;
            rectangle[0] = x; rectangle[1] = y; rectangle[2] = width; rectangle[3] = height;
            return true;
        }
    }
    return false;
}

static void commit_text_input_state(
    boxrig_wayland_seat *seat,
    bool force,
    bool composition_position_ready)
{
    boxrig_wayland_terminal_component *component = seat->text_component;
    int32_t x, y, width, height;
    bool rectangle_changed = false;

    if (!seat->text_enabled || seat->text_input == NULL || component == NULL || !input_enabled(component)) {
        return;
    }
    boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(component->view);
    uint64_t generation = boxrig_ui_editable_document_input_generation(document);
    bool external_change = generation != seat->text_document_generation;
    bool reset = force || external_change || seat->text_reset_required;
    if (reset) {
        /* Commit a disabled state before replacing an active field. wlroots
         * 0.17 latches optional capabilities only on disabled-to-enabled
         * transitions; this also gives the IME an explicit deactivation. */
        if (!force) {
            zwp_text_input_v3_disable(seat->text_input);
            zwp_text_input_v3_commit(seat->text_input);
            seat->text_commit_serial++;
        }
        zwp_text_input_v3_enable(seat->text_input);
        zwp_text_input_v3_set_content_type(seat->text_input,
            ZWP_TEXT_INPUT_V3_CONTENT_HINT_MULTILINE |
                (text_input_has_hints(seat) ? ZWP_TEXT_INPUT_V3_CONTENT_HINT_PREEDIT_SHOWN : 0),
            ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL);
        seat->text_epoch++;
        seat->text_document_generation = generation;
        seat->text_sent_generation = 0;
        seat->text_state_deferred = false;
        seat->text_reset_required = false;
        seat->text_cursor_valid = false;
        reset_text_batch(seat);
        boxrig_composition_event cancel = {.type = BOXRIG_COMPOSITION_CANCEL};
        (void)boxrig_composition_apply_event(component->composition, &cancel);
    }
    if (seat->text_state_deferred) return;
    bool context_changed = generation != seat->text_sent_generation;
    if (context_changed) {
        boxrig_ui_editable_document_snapshot snapshot;
        seat->text_document_attached = boxrig_ui_editable_document_capabilities(document).context_available;
        seat->text_context_available =
            boxrig_ui_editable_document_get_context(document, 4000, &snapshot) == BOXRIG_UI_OK;
        /* Wayland strings are NUL-terminated; a length-delimited document
         * containing U+0000 cannot be advertised without losing context. */
        if (seat->text_context_available && memchr(snapshot.context, '\0', snapshot.context_length) != NULL)
            seat->text_context_available = false;
        if (seat->text_context_available) {
            char surrounding[4001];
            memcpy(surrounding, snapshot.context, snapshot.context_length);
            surrounding[snapshot.context_length] = '\0';
            zwp_text_input_v3_set_surrounding_text(seat->text_input, surrounding,
                (int32_t)(snapshot.cursor - snapshot.context_offset),
                (int32_t)(snapshot.anchor - snapshot.context_offset));
            seat->text_context_offset = snapshot.context_offset;
            seat->text_context_length = snapshot.context_length;
        }
        if (external_change)
            zwp_text_input_v3_set_text_change_cause(seat->text_input, ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_OTHER);
        seat->text_sent_generation = generation;
    }
    int32_t rectangle[4];
    if (boxrig_wayland_input_cursor_geometry(component, composition_position_ready, rectangle)) {
        x = rectangle[0]; y = rectangle[1]; width = rectangle[2]; height = rectangle[3];
        if (text_input_has_hints(seat) && seat->text_focus_surface == component->parent) {
            int64_t parent_x = (int64_t)x + component->candidate_x;
            int64_t parent_y = (int64_t)y + component->candidate_y;
            if (parent_x < INT32_MIN || parent_x > INT32_MAX ||
                parent_y < INT32_MIN || parent_y > INT32_MAX) goto commit_state;
            x = (int32_t)parent_x;
            y = (int32_t)parent_y;
        }
        rectangle_changed = !seat->text_cursor_valid ||
                            seat->text_cursor_x != x ||
                            seat->text_cursor_y != y ||
                            seat->text_cursor_width != width ||
                            seat->text_cursor_height != height;
        if (force || rectangle_changed) {
            zwp_text_input_v3_set_cursor_rectangle(
                seat->text_input,
                x,
                y,
                width,
                height);
            seat->text_cursor_valid = true;
            seat->text_cursor_x = x;
            seat->text_cursor_y = y;
            seat->text_cursor_width = width;
            seat->text_cursor_height = height;
        }
    }
commit_state:
    if (reset || context_changed || rectangle_changed) {
        zwp_text_input_v3_commit(seat->text_input);
        seat->text_commit_serial++;
        if (reset) seat->text_enable_serial = seat->text_commit_serial;
        if (rectangle_changed && text_input_has_hints(seat)) {
            /* Version 2 applies the committed rectangle on the focused
             * surface's next commit; a borrowed parent stays host-owned. */
            boxrig_wayland_component_schedule_input_commit(component,
                seat->text_focus_surface != component->surface);
        }
        seat->context->flush_required = true;
    }
}

static void text_input_enter(
    void *data,
    struct zwp_text_input_v3 *text_input,
    struct wl_surface *surface)
{
    boxrig_wayland_seat *seat = data;

    boxrig_wayland_terminal_component *component = find_keyboard_component(
        seat->context,
        surface);

    (void)text_input;
    seat->reparent_text_component = find_reparent_destination(seat->context, surface);
    if (component == NULL) {
        return;
    }
    if (seat->text_enabled && seat->text_component != NULL && seat->text_component != component) {
        queue_composition_cancel(seat, seat->text_component);
    }
    reset_text_batch(seat);
    seat->text_component = component;
    seat->text_focus_surface = surface;
    seat->text_component_generation = component->input_generation;
    seat->text_enabled = input_enabled(component) && !boxrig_wayland_ibus_selected(seat->context);
    seat->text_cursor_valid = false;
    commit_text_input_state(seat, true, false);
}

static void text_input_leave(
    void *data,
    struct zwp_text_input_v3 *text_input,
    struct wl_surface *surface)
{
    boxrig_wayland_seat *seat = data;

    boxrig_wayland_terminal_component *component = find_keyboard_component(
        seat->context,
        surface);

    (void)text_input;
    if (seat->reparent_text_component != NULL &&
        seat->reparent_text_component->parent == surface)
        seat->reparent_text_component = NULL;
    if (component != NULL && seat->text_component == component) {
        if (seat->text_enabled) queue_composition_cancel(seat, component);
        zwp_text_input_v3_disable(seat->text_input);
        zwp_text_input_v3_commit(seat->text_input);
        seat->text_commit_serial++;
        seat->context->flush_required = true;
        reset_text_batch(seat);
        seat->text_component = NULL;
        seat->text_focus_surface = NULL;
        seat->text_enabled = false;
        seat->text_cursor_valid = false;
    }
}

static void text_input_preedit(
    void *data,
    struct zwp_text_input_v3 *text_input,
    const char *text,
    int32_t cursor_begin,
    int32_t cursor_end)
{
    boxrig_wayland_seat *seat = data;
    if (!seat->text_enabled) return;

    (void)text_input;
    if (seat->text_component == NULL || seat->pending_invalid ||
        !copy_text_event(
            seat,
            text,
            &seat->pending_preedit,
            &seat->pending_preedit_length)) {
        if (seat->text_component != NULL) {
            seat->text_component->input_error_count++;
            seat->pending_invalid = true;
        }
        return;
    }
    seat->pending_cursor_begin = cursor_begin;
    seat->pending_cursor_end = cursor_end;
    seat->pending_preedit_set = true;
}

static void text_input_commit(
    void *data,
    struct zwp_text_input_v3 *text_input,
    const char *text)
{
    boxrig_wayland_seat *seat = data;
    if (!seat->text_enabled) return;

    (void)text_input;
    if (seat->text_component == NULL || seat->pending_invalid ||
        !copy_text_event(
            seat,
            text,
            &seat->pending_commit,
            &seat->pending_commit_length)) {
        if (seat->text_component != NULL) {
            seat->text_component->input_error_count++;
            seat->pending_invalid = true;
        }
        return;
    }
    seat->pending_commit_set = true;
}

static void text_input_preedit_hint(
    void *data,
    struct zwp_text_input_v3 *text_input,
    uint32_t start,
    uint32_t end,
    uint32_t hint)
{
    boxrig_wayland_seat *seat = data;
    if (!seat->text_enabled) return;
    uint32_t feedback;
    (void)text_input;
    if (seat->text_component == NULL || seat->pending_hint_overflow) return;
    switch (hint) {
    case ZWP_TEXT_INPUT_V3_PREEDIT_HINT_WHOLE:
        feedback = BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE;
        break;
    case ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SELECTION:
        feedback = BOXRIG_COMPOSITION_FEEDBACK_REVERSE | BOXRIG_COMPOSITION_FEEDBACK_TARGET;
        break;
    case ZWP_TEXT_INPUT_V3_PREEDIT_HINT_PREDICTION:
        feedback = BOXRIG_COMPOSITION_FEEDBACK_SECONDARY;
        break;
    case ZWP_TEXT_INPUT_V3_PREEDIT_HINT_PREFIX:
    case ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SUFFIX:
        feedback = BOXRIG_COMPOSITION_FEEDBACK_CONVERTED;
        break;
    case ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SPELLING_ERROR:
    case ZWP_TEXT_INPUT_V3_PREEDIT_HINT_COMPOSE_ERROR:
        feedback = BOXRIG_COMPOSITION_FEEDBACK_ERROR;
        break;
    default:
        return;
    }
    if (start >= end || end > seat->text_component->config.maximum_ime_bytes) {
        seat->text_component->input_error_count++;
        return;
    }
    for (size_t index = 0; index < seat->pending_hint_count; index++) {
        boxrig_wayland_preedit_hint *previous = &seat->pending_hints[index];
        if (previous->start == start && previous->end == end) {
            previous->feedback |= feedback;
            return;
        }
    }
    if (seat->pending_hint_count == BOXRIG_WAYLAND_PREEDIT_HINTS_MAXIMUM) {
        seat->pending_hint_overflow = true;
        seat->text_component->input_error_count++;
        return;
    }
    seat->pending_hints[seat->pending_hint_count++] =
        (boxrig_wayland_preedit_hint){start, end, feedback};
}

static void text_input_action(
    void *data, struct zwp_text_input_v3 *text_input, uint32_t action, uint32_t serial)
{
    /* No submit actions are advertised for a terminal. NONE changes no state. */
    (void)data; (void)text_input; (void)action; (void)serial;
}

static void text_input_language(
    void *data, struct zwp_text_input_v3 *text_input, const char *language)
{
    /* The engine's language does not change terminal encoding or input text. */
    (void)data; (void)text_input; (void)language;
}

static void text_input_delete(
    void *data,
    struct zwp_text_input_v3 *text_input,
    uint32_t before_length,
    uint32_t after_length)
{
    boxrig_wayland_seat *seat = data;
    if (!seat->text_enabled) return;

    (void)text_input;
    if (seat->text_component == NULL || seat->pending_invalid) {
        return;
    }
    seat->pending_delete_before = before_length;
    seat->pending_delete_after = after_length;
    seat->pending_delete_set = true;
}

static void text_input_done(
    void *data,
    struct zwp_text_input_v3 *text_input,
    uint32_t serial)
{
    boxrig_wayland_seat *seat = data;
    if (!seat->text_enabled) return;
    boxrig_wayland_terminal_component *component = seat->text_component;
    boxrig_wayland_input_item items[3];
    size_t item_count = 0;
    size_t index;
    bool replace_preedit;
    bool preedit_valid = true;
    bool commit_valid = true;

    (void)text_input;
    memset(items, 0, sizeof(items));
    if (component == NULL || !seat->text_enabled || seat->text_reset_required ||
        seat->text_component_generation != component->input_generation ||
        (uint32_t)(serial - seat->text_enable_serial) >
            (uint32_t)(seat->text_commit_serial - seat->text_enable_serial) ||
        seat->text_document_generation != boxrig_ui_editable_document_input_generation(
            boxrig_ui_view_editable_document(component->view)) || seat->pending_invalid) {
        if (component != NULL) {
            component->input_error_count++;
            if (seat->text_document_attached) {
                seat->text_reset_required = true;
                seat->text_epoch++;
            }
        }
        reset_text_batch(seat);
        return;
    }
    /* A cursor-rectangle commit may overtake an otherwise current edit.
     * Apply it, but defer protocol state until a matching done arrives. */
    seat->text_state_deferred = serial != seat->text_commit_serial;
    /* Empty preedit ends the old composition. Stale cursor positions have
     * no meaning when there is no remaining text. */
    if (seat->pending_preedit_set && seat->pending_preedit_length != 0) {
        const bool hidden = seat->pending_cursor_begin == -1 &&
                            seat->pending_cursor_end == -1;

        preedit_valid = valid_utf8(
                            seat->pending_preedit,
                            seat->pending_preedit_length) &&
                        (hidden ||
                         (seat->pending_cursor_begin >= 0 &&
                          utf8_boundary(
                              seat->pending_preedit,
                              seat->pending_preedit_length,
                              (size_t)seat->pending_cursor_begin)));
        /* An unusable selection endpoint must not discard valid text or
         * a commit in the same batch. Keep the valid cursor without a selection. */
        if (preedit_valid && !hidden &&
            (seat->pending_cursor_end < 0 ||
             !utf8_boundary(
                 seat->pending_preedit,
                 seat->pending_preedit_length,
                 (size_t)seat->pending_cursor_end))) {
            seat->pending_cursor_end = seat->pending_cursor_begin;
        }
    }
    if (seat->pending_commit_set) {
        commit_valid = valid_utf8(
            seat->pending_commit,
            seat->pending_commit_length);
    }
    if (!preedit_valid || !commit_valid) {
        component->input_error_count++;
        if (seat->text_document_attached) {
            seat->text_reset_required = true;
            seat->text_epoch++;
        }
        reset_text_batch(seat);
        return;
    }
    if (seat->text_document_attached) {
        if (!prepare_composition_item(seat,
                seat->pending_preedit_length != 0 ? BOXRIG_COMPOSITION_PREEDIT_UPDATE : BOXRIG_COMPOSITION_PREEDIT_COMPLETE,
                seat->pending_preedit, seat->pending_preedit_length,
                seat->pending_cursor_begin, seat->pending_cursor_end, &items[0])) goto resource_failure;
        item_count = 1;
        items[0].type = BOXRIG_WAYLAND_INPUT_DOCUMENT_BATCH;
        items[0].text_epoch = seat->text_epoch;
        items[0].commit_text = seat->pending_commit;
        items[0].commit_length = seat->pending_commit_length;
        seat->pending_commit = NULL;
        items[0].commit_set = seat->pending_commit_set;
        items[0].delete_set = seat->pending_delete_set;
        items[0].delete_before = seat->pending_delete_before;
        items[0].delete_after = seat->pending_delete_after;
        goto queue_items;
    }
    replace_preedit = seat->pending_commit_set || seat->pending_delete_set;
    if (replace_preedit) {
        if (!prepare_composition_item(
                seat,
                BOXRIG_COMPOSITION_PREEDIT_COMPLETE,
                NULL,
                0,
                0,
                0,
                &items[item_count++])) {
            goto resource_failure;
        }
    }
    if (seat->pending_commit_set && seat->pending_commit_length != 0) {
        if (!prepare_composition_item(
                seat,
                BOXRIG_COMPOSITION_COMMIT,
                seat->pending_commit,
                seat->pending_commit_length,
                0,
                0,
                &items[item_count++])) {
            goto resource_failure;
        }
    }
    if (seat->pending_preedit_set) {
        if (seat->pending_preedit_length != 0) {
            if (!prepare_composition_item(
                    seat,
                    BOXRIG_COMPOSITION_PREEDIT_UPDATE,
                    seat->pending_preedit,
                    seat->pending_preedit_length,
                    seat->pending_cursor_begin,
                    seat->pending_cursor_end,
                    &items[item_count++])) {
                goto resource_failure;
            }
        } else if (!replace_preedit) {
            if (!prepare_composition_item(
                    seat,
                    BOXRIG_COMPOSITION_PREEDIT_COMPLETE,
                    NULL,
                    0,
                    0,
                    0,
                    &items[item_count++])) {
                goto resource_failure;
            }
        }
    }
queue_items:
    if (item_count > component->config.maximum_input_events -
                         component->input_queue_depth) {
        component->failure_reason =
            BOXRIG_WAYLAND_FAILURE_INPUT_QUEUE_OVERFLOW;
        goto discard_items;
    }
    for (index = 0; index < item_count; index++) {
        (void)boxrig_wayland_input_enqueue(component, &items[index]);
    }
    reset_text_batch(seat);
    return;

resource_failure:
    component->failure_reason = BOXRIG_WAYLAND_FAILURE_RESOURCE;
discard_items:
    for (index = 0; index < item_count; index++) {
        boxrig_wayland_input_free_item(&items[index]);
    }
    reset_text_batch(seat);
}

static const struct zwp_text_input_v3_listener text_input_listener = {
    .enter = text_input_enter,
    .leave = text_input_leave,
    .preedit_string = text_input_preedit,
    .commit_string = text_input_commit,
    .delete_surrounding_text = text_input_delete,
    .done = text_input_done,
    .action = text_input_action,
    .language = text_input_language,
    .preedit_hint = text_input_preedit_hint,
};

static bool create_text_input(boxrig_wayland_seat *seat)
{
    if (seat->text_input != NULL ||
        seat->context->text_input_manager == NULL) {
        return true;
    }
    seat->text_input = zwp_text_input_manager_v3_get_text_input(
        seat->context->text_input_manager,
        seat->proxy);
    if (seat->text_input == NULL) {
        return false;
    }
    wl_proxy_set_queue(
        (struct wl_proxy *)seat->text_input,
        seat->context->queue);
    if (zwp_text_input_v3_add_listener(
            seat->text_input,
            &text_input_listener,
            seat) != 0) {
        zwp_text_input_v3_destroy(seat->text_input);
        seat->text_input = NULL;
        return false;
    }
    seat->context->flush_required = true;
    return true;
}

static void destroy_keyboard(boxrig_wayland_seat *seat)
{
    boxrig_wayland_ibus_focus(seat, NULL, true);
    clear_seat_keys(seat);
    if (seat->keyboard != NULL) {
        if (wl_proxy_get_version((struct wl_proxy *)seat->keyboard) >=
            WL_KEYBOARD_RELEASE_SINCE_VERSION) {
            wl_keyboard_release(seat->keyboard);
        } else {
            wl_keyboard_destroy(seat->keyboard);
        }
        seat->keyboard = NULL;
        seat->context->flush_required = true;
    }
    if (seat->keyboard_component != NULL) {
        boxrig_wayland_terminal_component *component =
            seat->keyboard_component;

        seat->keyboard_component = NULL;
        if (component->keyboard_focus_count != 0) {
            component->keyboard_focus_count--;
            if (component->keyboard_focus_count == 0) {
                (void)queue_focus(seat, component, false);
            }
        }
    }
    seat->serial_valid[BOXRIG_WAYLAND_SERIAL_KEYBOARD_ENTER] = false;
    seat->serial_components[BOXRIG_WAYLAND_SERIAL_KEYBOARD_ENTER] = NULL;
    seat->serial_valid[BOXRIG_WAYLAND_SERIAL_KEYBOARD_KEY] = false;
    seat->serial_components[BOXRIG_WAYLAND_SERIAL_KEYBOARD_KEY] = NULL;
}

static void destroy_pointer(boxrig_wayland_seat *seat)
{
    if (seat->pointer != NULL) {
        if (wl_proxy_get_version((struct wl_proxy *)seat->pointer) >=
            WL_POINTER_RELEASE_SINCE_VERSION) {
            wl_pointer_release(seat->pointer);
        } else {
            wl_pointer_destroy(seat->pointer);
        }
        seat->pointer = NULL;
        seat->context->flush_required = true;
    }
    clear_pointer_focus(seat);
}

static void seat_capabilities(
    void *data,
    struct wl_seat *proxy,
    uint32_t capabilities)
{
    boxrig_wayland_seat *seat = data;

    (void)proxy;
    seat->capabilities = capabilities;
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 &&
        seat->keyboard == NULL) {
        seat->keyboard = wl_seat_get_keyboard(seat->proxy);
        if (seat->keyboard != NULL) {
            wl_proxy_set_queue(
                (struct wl_proxy *)seat->keyboard,
                seat->context->queue);
            if (wl_keyboard_add_listener(
                    seat->keyboard,
                    &keyboard_listener,
                    seat) != 0) {
                destroy_keyboard(seat);
            }
        }
    } else if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) == 0) {
        destroy_keyboard(seat);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 &&
        seat->pointer == NULL) {
        seat->pointer = wl_seat_get_pointer(seat->proxy);
        if (seat->pointer != NULL) {
            wl_proxy_set_queue(
                (struct wl_proxy *)seat->pointer,
                seat->context->queue);
            if (wl_pointer_add_listener(
                    seat->pointer,
                    &pointer_listener,
                    seat) != 0) {
                destroy_pointer(seat);
            }
        }
    } else if ((capabilities & WL_SEAT_CAPABILITY_POINTER) == 0) {
        destroy_pointer(seat);
    }
}

static void seat_name(
    void *data,
    struct wl_seat *seat,
    const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

bool boxrig_wayland_input_add_seat(
    boxrig_wayland_context *context,
    uint32_t name,
    uint32_t version)
{
    boxrig_wayland_seat *seat;

    if (context == NULL || context->registry == NULL ||
        context->seat_count >= context->config.maximum_seats) {
        return false;
    }
    seat = calloc(1, sizeof(*seat));
    if (seat == NULL) {
        return false;
    }
    seat->context = context;
    seat->global_name = name;
    seat->version = supported_version(version, BOXRIG_WAYLAND_SEAT_VERSION);
    seat->proxy = wl_registry_bind(
        context->registry,
        name,
        &wl_seat_interface,
        seat->version);
    if (seat->proxy == NULL) {
        free(seat);
        return false;
    }
    wl_proxy_set_queue((struct wl_proxy *)seat->proxy, context->queue);
    if (wl_seat_add_listener(seat->proxy, &seat_listener, seat) != 0) {
        wl_seat_destroy(seat->proxy);
        free(seat);
        return false;
    }
    seat->next = context->seats;
    context->seats = seat;
    context->seat_count++;
    if (context->seat_count > context->seat_high_water) {
        context->seat_high_water = context->seat_count;
    }
    (void)create_text_input(seat);
    (void)boxrig_wayland_clipboard_create_seat_devices(seat);
    context->flush_required = true;
    return true;
}

static void remove_queued_seat_items(
    boxrig_wayland_terminal_component *component,
    boxrig_wayland_seat *seat)
{
    size_t count = component->input_queue_depth;
    size_t index;

    for (index = 0; index < count; index++) {
        boxrig_wayland_input_item item =
            component->input_items[component->input_queue_head];

        component->input_queue_head =
            (component->input_queue_head + 1) %
            component->config.maximum_input_events;
        component->input_queue_depth--;
        if (item.seat == seat) {
            boxrig_wayland_input_free_item(&item);
        } else {
            size_t tail =
                (component->input_queue_head + component->input_queue_depth) %
                component->config.maximum_input_events;

            component->input_items[tail] = item;
            component->input_queue_depth++;
        }
    }
}

static void destroy_seat(boxrig_wayland_seat *seat)
{
    boxrig_wayland_ibus_remove_seat(seat);
    boxrig_wayland_terminal_component *component;

    boxrig_wayland_clipboard_destroy_seat_devices(seat);
    for (component = seat->context->components;
         component != NULL;
         component = component->next) {
        remove_queued_seat_items(component, seat);
    }
    if (seat->keyboard_component != NULL) {
        boxrig_wayland_terminal_component *focused =
            seat->keyboard_component;

        seat->keyboard_component = NULL;
        if (focused->keyboard_focus_count != 0) {
            focused->keyboard_focus_count--;
            if (focused->keyboard_focus_count == 0) {
                (void)queue_focus(NULL, focused, false);
            }
        }
    }
    destroy_keyboard(seat);
    destroy_pointer(seat);
    if (seat->text_input != NULL) {
        zwp_text_input_v3_destroy(seat->text_input);
    }
    if (seat->compose_state != NULL) {
        xkb_compose_state_unref(seat->compose_state);
    }
    if (seat->xkb_state != NULL) {
        xkb_state_unref(seat->xkb_state);
    }
    if (seat->keymap != NULL) {
        xkb_keymap_unref(seat->keymap);
    }
    if (seat->version >= WL_SEAT_RELEASE_SINCE_VERSION) {
        wl_seat_release(seat->proxy);
    } else {
        wl_seat_destroy(seat->proxy);
    }
    free(seat->pending_preedit);
    free(seat->pending_commit);
    free(seat);
}

bool boxrig_wayland_input_remove_global(
    boxrig_wayland_context *context,
    uint32_t name)
{
    boxrig_wayland_seat **link;

    if (context == NULL) {
        return false;
    }
    if (name == context->text_input_name) {
        boxrig_wayland_seat *seat;

        for (seat = context->seats; seat != NULL; seat = seat->next) {
            if (seat->text_input != NULL) {
                zwp_text_input_v3_destroy(seat->text_input);
                seat->text_input = NULL;
            }
            if (seat->text_enabled) queue_composition_cancel(seat, seat->text_component);
            reset_text_batch(seat);
            seat->text_component = NULL;
            seat->text_focus_surface = NULL;
            seat->text_enabled = false;
            seat->text_cursor_valid = false;
        }
        zwp_text_input_manager_v3_destroy(context->text_input_manager);
        context->text_input_manager = NULL;
        context->text_input_name = 0;
        return true;
    }
    link = &context->seats;
    while (*link != NULL) {
        if ((*link)->global_name == name) {
            boxrig_wayland_seat *seat = *link;

            *link = seat->next;
            context->seat_count--;
            destroy_seat(seat);
            return true;
        }
        link = &(*link)->next;
    }
    return false;
}

bool boxrig_wayland_input_bind_text_manager(
    boxrig_wayland_context *context,
    struct wl_registry *registry,
    uint32_t name,
    uint32_t version)
{
    boxrig_wayland_seat *seat;

    if (context == NULL || registry == NULL || version == 0) {
        return false;
    }
    context->text_input_manager = wl_registry_bind(
        registry,
        name,
        &zwp_text_input_manager_v3_interface,
        version);
    if (context->text_input_manager == NULL) {
        return false;
    }
    context->text_input_name = name;
    wl_proxy_set_queue(
        (struct wl_proxy *)context->text_input_manager,
        context->queue);
    for (seat = context->seats; seat != NULL; seat = seat->next) {
        if (!create_text_input(seat)) {
            return false;
        }
    }
    context->flush_required = true;
    return true;
}

void boxrig_wayland_input_destroy_context(boxrig_wayland_context *context)
{
    while (context != NULL && context->seats != NULL) {
        boxrig_wayland_seat *seat = context->seats;

        context->seats = seat->next;
        context->seat_count--;
        destroy_seat(seat);
    }
    if (context != NULL && context->text_input_manager != NULL) {
        zwp_text_input_manager_v3_destroy(context->text_input_manager);
        context->text_input_manager = NULL;
    }
}

boxrig_ui_status boxrig_wayland_input_initialize_component(
    boxrig_wayland_terminal_component *component)
{
    boxrig_composition_limits limits = boxrig_composition_limits_default();

    boxrig_ui_input_control_initialize(&component->input_control);
    component->input_items = calloc(
        component->config.maximum_input_events,
        sizeof(*component->input_items));
    if (component->input_items == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    limits.maximum_text_bytes = component->config.maximum_ime_bytes;
    if (boxrig_composition_create(
            component->terminal,
            limits,
            &component->composition) != BOXRIG_COMPOSITION_OK) {
        free(component->input_items);
        component->input_items = NULL;
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_wayland_terminal_component_set_input_enabled(
    boxrig_wayland_terminal_component *component, bool enabled)
{
    if (component == NULL || !component->running) return BOXRIG_UI_INVALID_ARGUMENT;
    if (component->processing_input) return BOXRIG_UI_BUSY;
    if (enabled && component->input_control.state.phase == BOXRIG_UI_INPUT_STOPPING) return BOXRIG_UI_BUSY;
    bool was_enabled = input_enabled(component);
    bool started = boxrig_wayland_clipboard_paste_started(component);
    boxrig_ui_status status = boxrig_ui_input_control_request(&component->input_control, enabled, started);
    if (status != BOXRIG_UI_OK && status != BOXRIG_UI_BUSY) return status;
    if (was_enabled != input_enabled(component)) {
        component->processing_input = true;
        if (!enabled) {
            boxrig_wayland_ibus_forget_component(component);
            component->ime_waiting = false;
            boxrig_wayland_clipboard_cancel_input(component);
            size_t retained = 0;
            for (size_t index = 0; index < component->input_queue_depth; index++) {
                size_t source = (component->input_queue_head + index) % component->config.maximum_input_events;
                boxrig_wayland_input_item *item = &component->input_items[source];
                if (item->type == BOXRIG_WAYLAND_INPUT_MOUSE || item->type == BOXRIG_WAYLAND_INPUT_FOCUS) {
                    size_t destination = (component->input_queue_head + retained++) % component->config.maximum_input_events;
                    if (source != destination) {
                        component->input_items[destination] = *item;
                        memset(item, 0, sizeof(*item));
                    }
                } else boxrig_wayland_input_free_item(item);
            }
            component->input_queue_depth = retained;
            (void)boxrig_composition_cancel_pending_commit(component->composition);
            boxrig_composition_event cancel = {.type = BOXRIG_COMPOSITION_CANCEL};
            (void)boxrig_composition_apply_event(component->composition, &cancel);
            boxrig_ui_view_clear_candidates(component->view);
        }
        boxrig_ui_status document = boxrig_ui_editable_document_invalidate_input(
            boxrig_ui_view_editable_document(component->view));
        if (document != BOXRIG_UI_OK) status = document;
        for (boxrig_wayland_seat *seat = component->context->seats; seat != NULL; seat = seat->next) {
            if (seat->pointer_component == component) {
                /* A frame or fractional detent may span the host's request.
                 * Do not restamp that older motion with the next generation. */
                clear_pointer_axis(seat);
                memset(seat->axis_value120_remainder, 0, sizeof(seat->axis_value120_remainder));
            }
            if (seat->keyboard_component == component) {
                if (!enabled) {
                    for (size_t key = 0; key < 256; key++) {
                        if (seat->native_pressed[key]) seat->inhibited_key[key] = true;
                        else if (seat->keys[key].component == component) clear_key_state(seat, (uint32_t)key);
                    }
                    if (seat->compose_state != NULL) xkb_compose_state_reset(seat->compose_state);
                } else boxrig_wayland_ibus_focus(seat, component, false);
            }
            if (seat->text_component != component || seat->text_input == NULL) continue;
            if (seat->text_enabled) {
                zwp_text_input_v3_disable(seat->text_input);
                zwp_text_input_v3_commit(seat->text_input);
                seat->text_commit_serial++;
                component->context->flush_required = true;
            }
            seat->text_enabled = enabled && !boxrig_wayland_ibus_selected(component->context);
            seat->text_cursor_valid = false;
            seat->text_epoch++;
            reset_text_batch(seat);
            commit_text_input_state(seat, true, false);
        }
        size_t rows, columns;
        boxrig_terminal_size(component->terminal, &rows, &columns);
        boxrig_ui_dirty dirty = {0, rows, 0, columns};
        if (boxrig_ui_view_set_composition_active(component->view, false) != BOXRIG_UI_OK ||
            boxrig_ui_view_invalidate(component->view, BOXRIG_UI_DIRTY_OVERLAY, dirty) != BOXRIG_UI_OK)
            status = BOXRIG_UI_UNAVAILABLE;
        component->processing_input = false;
    }
    if (status != BOXRIG_UI_OK && status != BOXRIG_UI_BUSY) return status;
    status = boxrig_ui_input_control_service(&component->input_control, component->terminal);
    component->input_blocked = component->input_control.state.phase == BOXRIG_UI_INPUT_STOPPING;
    return status;
}

boxrig_ui_status boxrig_wayland_terminal_component_get_input_state(
    const boxrig_wayland_terminal_component *component, boxrig_ui_input_state *state_out)
{
    if (component == NULL || !component->running || state_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *state_out = component->input_control.state;
    return BOXRIG_UI_OK;
}

static void detach_component_input(
    boxrig_wayland_terminal_component *component, bool terminating)
{
    boxrig_wayland_seat *seat;

    if (component == NULL || component->context == NULL) {
        return;
    }
    boxrig_wayland_ibus_forget_component(component);
    (void)boxrig_ui_editable_document_invalidate_input(
        boxrig_ui_view_editable_document(component->view));
    for (seat = component->context->seats;
         seat != NULL;
         seat = seat->next) {
        if (seat->reparent_keyboard_component == component)
            seat->reparent_keyboard_component = NULL;
        if (seat->reparent_text_component == component)
            seat->reparent_text_component = NULL;
        if (seat->keyboard_component == component) {
            clear_seat_keys(seat);
            seat->keyboard_component = NULL;
        }
        if (seat->pointer_component == component) {
            clear_pointer_focus(seat);
        }
        if (seat->text_component == component) {
            if (seat->text_input != NULL && seat->text_enabled) {
                zwp_text_input_v3_disable(seat->text_input);
                zwp_text_input_v3_commit(seat->text_input);
                seat->text_commit_serial++;
                seat->context->flush_required = true;
            }
            reset_text_batch(seat);
            seat->text_component = NULL;
            seat->text_focus_surface = NULL;
            seat->text_enabled = false;
            seat->text_cursor_valid = false;
        }
    }
    component->keyboard_focus_count = 0;
    component->pointer_focus_count = 0;
    component->pressed_key_count = 0;
    component->pressed_button_count = 0;
    if (component->composition != NULL && terminating) {
        (void)boxrig_composition_terminate(component->composition);
    }
}

void boxrig_wayland_input_quiesce_component(
    boxrig_wayland_terminal_component *component)
{
    detach_component_input(component, true);
}

void boxrig_wayland_input_reparent_component(
    boxrig_wayland_terminal_component *component)
{
    bool was_focused = component->keyboard_focus_count != 0;
    component->input_generation++;
    component->input_control.state.generation++;
    detach_component_input(component, false);
    (void)boxrig_composition_focus_changed(component->composition, false);
    (void)boxrig_ui_view_set_focused(component->view, false);
    if (was_focused) {
        /* The document and IME were invalidated by detach_component_input.
         * Keep the terminal focus report ordered in the retained input queue. */
        boxrig_wayland_input_item item = {0};
        item.type = BOXRIG_WAYLAND_INPUT_FOCUS;
        item.value.focus.focused = false;
        (void)boxrig_wayland_input_enqueue(component, &item);
    }
    for (boxrig_wayland_seat *seat = component->context->seats; seat != NULL; seat = seat->next) {
        for (size_t index = 0; index < 4; index++) {
            if (seat->serial_components[index] == component) {
                seat->serial_valid[index] = false;
                seat->serial_components[index] = NULL;
            }
        }
    }
}

void boxrig_wayland_input_complete_reparent(
    boxrig_wayland_terminal_component *component)
{
    for (boxrig_wayland_seat *seat = component->context->seats; seat != NULL; seat = seat->next) {
        if (seat->reparent_keyboard_component == component) {
            struct wl_array keys = {0};
            uint32_t serial = seat->reparent_keyboard_serial;
            seat->reparent_keyboard_component = NULL;
            keyboard_enter(seat, seat->keyboard, serial, component->parent, &keys);
        }
        if (seat->reparent_text_component == component) {
            seat->reparent_text_component = NULL;
            text_input_enter(seat, seat->text_input, component->parent);
        }
    }
}

void boxrig_wayland_input_destroy_component(
    boxrig_wayland_terminal_component *component)
{
    if (component == NULL) {
        return;
    }
    while (component->input_queue_depth != 0) {
        boxrig_wayland_input_item *item =
            &component->input_items[component->input_queue_head];

        boxrig_wayland_input_free_item(item);
        component->input_queue_head =
            (component->input_queue_head + 1) %
            component->config.maximum_input_events;
        component->input_queue_depth--;
    }
    boxrig_composition_destroy(component->composition);
    component->composition = NULL;
    free(component->input_items);
    component->input_items = NULL;
}

static bool copy_pressed_key(
    boxrig_wayland_pressed_key *destination,
    boxrig_wayland_terminal_component *component,
    const boxrig_wayland_input_item *item,
    bool application_route)
{
    char *text = NULL;

    if (item->value.key.text_length != 0) {
        text = malloc(item->value.key.text_length);
        if (text == NULL) {
            return false;
        }
        memcpy(text, item->value.key.text, item->value.key.text_length);
    }
    destination->keysym = item->keysym;
    destination->ime_modifiers = item->ime_modifiers;
    destination->ime_time = item->ime_time;
    destination->component = component;
    destination->identity = item->value.key;
    destination->identity.text = text;
    destination->text = text;
    destination->active = true;
    destination->application_route = application_route;
    destination->route_resolved = true;
    destination->repeatable = xkb_keymap_key_repeats(
        item->seat->keymap,
        (xkb_keycode_t)(item->native_code + 8)) != 0;
    component->pressed_key_count++;
    return true;
}

static bool apply_deferred_compose(boxrig_wayland_input_item *item)
{
    item->compose_pending = false;
    struct xkb_compose_state *compose = item->seat->compose_state;
    if (compose == NULL || xkb_compose_state_feed(compose, item->keysym) != XKB_COMPOSE_FEED_ACCEPTED)
        return true;
    enum xkb_compose_status status = xkb_compose_state_get_status(compose);
    if (status == XKB_COMPOSE_COMPOSING) {
        free(item->text);
        item->text = NULL;
        item->value.key.text = NULL;
        item->value.key.text_length = 0;
        item->value.key.codepoint = 0;
    } else if (status == XKB_COMPOSE_COMPOSED) {
        int length = xkb_compose_state_get_utf8(compose, NULL, 0);
        if (length < 0) return false;
        char *text = malloc((size_t)length + 1);
        if (text == NULL) return false;
        if (xkb_compose_state_get_utf8(compose, text, (size_t)length + 1) != length) {
            free(text);
            return false;
        }
        free(item->text);
        item->text = text;
        item->value.key.text = text;
        item->value.key.text_length = (size_t)length;
        xkb_compose_state_reset(compose);
    } else if (status == XKB_COMPOSE_CANCELLED) xkb_compose_state_reset(compose);
    return true;
}

bool boxrig_wayland_input_forward_key(boxrig_wayland_seat *seat, uint32_t symbol,
    uint32_t modifiers, boxrig_wayland_input_item *item)
{
    memset(item, 0, sizeof(*item));
    item->type = BOXRIG_WAYLAND_INPUT_KEY;
    item->seat = seat;
    item->ime_forwarded = true;
    item->value.key.action = (modifiers & IBUS_RELEASE_MASK) != 0 ? BOXRIG_KEY_RELEASE : BOXRIG_KEY_PRESS;
    item->value.key.key = translate_functional_key(symbol);
    item->value.key.codepoint = xkb_keysym_to_utf32(symbol);
    const uint32_t native[] = {IBUS_SHIFT_MASK, IBUS_CONTROL_MASK, IBUS_MOD1_MASK, IBUS_SUPER_MASK,
        IBUS_HYPER_MASK, IBUS_META_MASK, IBUS_LOCK_MASK, IBUS_MOD2_MASK};
    const uint32_t portable[] = {BOXRIG_MODIFIER_SHIFT, BOXRIG_MODIFIER_CONTROL, BOXRIG_MODIFIER_ALT,
        BOXRIG_MODIFIER_SUPER, BOXRIG_MODIFIER_HYPER, BOXRIG_MODIFIER_META, BOXRIG_MODIFIER_CAPS_LOCK,
        BOXRIG_MODIFIER_NUM_LOCK};
    for (size_t index = 0; index < sizeof(native) / sizeof(*native); index++)
        if ((modifiers & native[index]) != 0) item->value.key.modifiers |= portable[index];
    if (item->value.key.action == BOXRIG_KEY_PRESS && item->value.key.key == BOXRIG_KEY_NONE) {
        char text[8];
        int length = xkb_keysym_to_utf8(symbol, text, sizeof(text));
        if (length > 1) {
            item->text = malloc((size_t)length);
            if (item->text == NULL) return false;
            memcpy(item->text, text, (size_t)length);
            item->value.key.text = item->text;
            item->value.key.text_length = (size_t)length - 1;
        }
    }
    return true;
}

static boxrig_ui_status process_key_item(
    boxrig_wayland_terminal_component *component,
    boxrig_wayland_input_item *item,
    bool *blocked_out)
{
    if (item->ime_forwarded) {
        if (!accepts_item(component, item)) return BOXRIG_UI_OK;
        boxrig_status forwarded = boxrig_terminal_send_key(component->terminal, &item->value.key);
        *blocked_out = forwarded == BOXRIG_STATUS_WOULD_BLOCK;
        return forwarded == BOXRIG_STATUS_OK || *blocked_out ? BOXRIG_UI_OK : BOXRIG_UI_UNAVAILABLE;
    }
    boxrig_wayland_pressed_key *pressed =
        &item->seat->keys[item->native_code];
    boxrig_input_key_route route = BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    boxrig_status status;

    if (!item->route_resolved) {
        if ((item->value.key.action == BOXRIG_KEY_RELEASE ||
             item->value.key.action == BOXRIG_KEY_REPEAT) &&
            pressed->active) {
            route = pressed->application_route
                        ? BOXRIG_INPUT_KEY_ROUTE_APPLICATION
                        : BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
            if (component->key_router != NULL) {
                boxrig_input_key_route observed = component->key_router(
                    &item->value.key,
                    component->key_router_context);

                if (observed == BOXRIG_INPUT_KEY_ROUTE_ERROR ||
                    (observed != BOXRIG_INPUT_KEY_ROUTE_TERMINAL &&
                     observed != BOXRIG_INPUT_KEY_ROUTE_APPLICATION)) {
                    return BOXRIG_UI_UNAVAILABLE;
                }
            }
        } else if (component->key_router != NULL) {
            route = component->key_router(
                &item->value.key,
                component->key_router_context);
            if (route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL &&
                route != BOXRIG_INPUT_KEY_ROUTE_APPLICATION) {
                return BOXRIG_UI_UNAVAILABLE;
            }
        }
        item->application_route =
            route == BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
        item->route_resolved = true;
        if (item->value.key.action == BOXRIG_KEY_PRESS && !pressed->active &&
            !copy_pressed_key(
                pressed,
                component,
                item,
                item->application_route)) {
            return BOXRIG_UI_OUT_OF_MEMORY;
        }
    }
    bool handled = false;
    bool inhibited = item->inhibited_key || !accepts_item(component, item);
    if (!item->application_route && !inhibited) {
        boxrig_ui_status filtered = boxrig_wayland_ibus_filter_key(component, item, blocked_out, &handled);
        if (filtered != BOXRIG_UI_OK || *blocked_out) return filtered;
        if (item->value.key.action == BOXRIG_KEY_PRESS) pressed->ime_consumed = handled;
        if (item->value.key.action == BOXRIG_KEY_RELEASE && pressed->ime_consumed) handled = true;
        if (!handled && item->compose_pending && !apply_deferred_compose(item)) return BOXRIG_UI_OUT_OF_MEMORY;
    }
    if (!item->application_route && !handled && !inhibited) {
        status = boxrig_terminal_send_key(component->terminal, &item->value.key);
        if (status == BOXRIG_STATUS_WOULD_BLOCK) {
            *blocked_out = true;
            return BOXRIG_UI_OK;
        }
        if (status != BOXRIG_STATUS_OK) {
            return BOXRIG_UI_UNAVAILABLE;
        }
    }
    if (item->value.key.action == BOXRIG_KEY_RELEASE) {
        clear_key_state(item->seat, item->native_code);
    } else if (item->value.key.action == BOXRIG_KEY_PRESS &&
               pressed->active) {
        start_repeat(item->seat, component, item->native_code);
    }
    return BOXRIG_UI_OK;
}

static bool queue_due_repeat(boxrig_wayland_context *context)
{
    boxrig_wayland_seat *seat;

    for (seat = context->seats; seat != NULL; seat = seat->next) {
        boxrig_wayland_pressed_key *pressed;
        boxrig_wayland_input_item item;

        if (!seat->repeat_active ||
            seat->repeat_deadline > context->monotonic_milliseconds ||
            seat->repeat_component == NULL ||
            (input_enabled(seat->repeat_component) && seat->repeat_component->input_blocked) ||
            seat->repeat_component->ime_waiting ||
            seat->repeat_component->failure_reason !=
                BOXRIG_WAYLAND_FAILURE_NONE) {
            continue;
        }
        pressed = &seat->keys[seat->repeat_key];
        if (!pressed->active || pressed->component != seat->repeat_component) {
            seat->repeat_active = false;
            seat->repeat_component = NULL;
            continue;
        }
        memset(&item, 0, sizeof(item));
        item.type = BOXRIG_WAYLAND_INPUT_KEY;
        item.seat = seat;
        item.native_code = seat->repeat_key;
        item.inhibited_key = seat->inhibited_key[seat->repeat_key];
        item.keysym = pressed->keysym;
        item.ime_modifiers = pressed->ime_modifiers;
        item.ime_time = pressed->ime_time;
        item.ime_generation = seat->ime_generation;
        item.compose_pending = boxrig_wayland_ibus_selected(context);
        item.value.key = pressed->identity;
        item.value.key.action = BOXRIG_KEY_REPEAT;
        if (pressed->identity.text_length != 0) {
            item.text = malloc(pressed->identity.text_length);
            if (item.text == NULL) {
                seat->repeat_component->failure_reason =
                    BOXRIG_WAYLAND_FAILURE_RESOURCE;
                seat->repeat_active = false;
                seat->repeat_component = NULL;
                return false;
            }
            memcpy(
                item.text,
                pressed->identity.text,
                pressed->identity.text_length);
            item.value.key.text = item.text;
        }
        if (!boxrig_wayland_input_enqueue(seat->repeat_component, &item)) {
            seat->repeat_active = false;
            seat->repeat_component = NULL;
            return false;
        }
        seat->repeat_index++;
        advance_repeat_deadline(seat);
        return true;
    }
    return false;
}

static boxrig_ui_status process_focus_item(
    boxrig_wayland_terminal_component *component,
    boxrig_wayland_input_item *item,
    bool *blocked_out)
{
    boxrig_status status = !accepts_item(component, item) ? BOXRIG_STATUS_OK : boxrig_terminal_send_focus(
        component->terminal,
        &item->value.focus);

    if (status == BOXRIG_STATUS_WOULD_BLOCK) {
        *blocked_out = true;
        return BOXRIG_UI_OK;
    }
    if (status != BOXRIG_STATUS_OK ||
        boxrig_composition_focus_changed(
            component->composition,
            item->value.focus.focused) != BOXRIG_COMPOSITION_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (item->ime_focus && !item->value.focus.focused)
        (void)boxrig_ui_editable_document_invalidate_input(boxrig_ui_view_editable_document(component->view));
    return BOXRIG_UI_OK;
}

static boxrig_ui_status process_scrollbar_item(
    boxrig_wayland_terminal_component *component, boxrig_wayland_input_item *item, bool *handled_out)
{
    boxrig_wayland_seat *seat = item->seat;
    const boxrig_mouse_event *event = &item->value.mouse;
    uint32_t mask = item->native_code;
    bool captured = mask != 0 ? (seat->pointer_scrollbar_buttons & mask) != 0
        : seat->pointer_scrollbar_buttons != 0;
    bool enabled = component->scrollbar_visibility.state.visible &&
        item->pointer_generation == seat->pointer_generation && seat->pointer_component == component;
    /* An application or terminal press keeps its route when dragged over a bar. */
    bool other_capture = event->action != BOXRIG_MOUSE_PRESS &&
        (event->pressed_buttons & ~seat->pointer_scrollbar_buttons) != 0;
    *handled_out = captured || (item->scrollbar_hit && !other_capture);
    if (!enabled) {
        if (*handled_out && event->action == BOXRIG_MOUSE_PRESS &&
            item->pointer_generation == seat->pointer_generation)
            seat->pointer_scrollbar_buttons |= mask;
        if (event->action == BOXRIG_MOUSE_RELEASE) seat->pointer_scrollbar_buttons &= ~mask;
        return BOXRIG_UI_OK;
    }
    seat->scrollbar_hovered = item->scrollbar_hit && !other_capture;
    if (!*handled_out) return synchronize_scrollbar_interaction(component);
    if (event->action == BOXRIG_MOUSE_RELEASE) {
        seat->pointer_scrollbar_buttons &= ~mask;
        if (event->button == BOXRIG_MOUSE_BUTTON_LEFT) seat->scrollbar_dragging = false;
        return synchronize_scrollbar_interaction(component);
    }
    boxrig_scrollbar_model model;
    if (boxrig_scrollbar_model_read(component->terminal, &model) == -1) return BOXRIG_UI_UNAVAILABLE;
    boxrig_scrollbar_action action = BOXRIG_SCROLLBAR_POSITION;
    size_t position = model.position;
    unsigned int width = component->scrollbar_visibility.state.reserved_width;
    if (event->action == BOXRIG_MOUSE_MOTION && seat->scrollbar_dragging) {
        int64_t requested = event->y - seat->scrollbar_drag_anchor;
        position = boxrig_scrollbar_position_from_thumb(&model, component->buffer_height,
            width, requested < 0 ? 0 : (size_t)requested);
    } else if (event->action == BOXRIG_MOUSE_PRESS) {
        seat->pointer_scrollbar_buttons |= mask;
        if (event->button == BOXRIG_MOUSE_BUTTON_LEFT && model.enabled) {
            boxrig_scrollbar_thumb thumb;
            boxrig_scrollbar_calculate_thumb(&model, component->buffer_height, width, &thumb);
            if ((size_t)event->y >= thumb.start && (size_t)event->y < thumb.start + thumb.length) {
                seat->scrollbar_dragging = true;
                seat->scrollbar_drag_anchor = event->y - (int64_t)thumb.start;
            } else {
                action = (size_t)event->y < thumb.start ? BOXRIG_SCROLLBAR_PAGE_UP : BOXRIG_SCROLLBAR_PAGE_DOWN;
            }
        } else if (event->button == BOXRIG_MOUSE_BUTTON_WHEEL_UP) action = BOXRIG_SCROLLBAR_LINE_UP;
        else if (event->button == BOXRIG_MOUSE_BUTTON_WHEEL_DOWN) action = BOXRIG_SCROLLBAR_LINE_DOWN;
    }
    bool changed;
    boxrig_ui_status status = boxrig_ui_view_apply_scrollbar_action(component->view, action, position, &changed);
    return status != BOXRIG_UI_OK ? status : synchronize_scrollbar_interaction(component);
}

static boxrig_ui_status process_mouse_item(
    boxrig_wayland_terminal_component *component,
    boxrig_wayland_input_item *item,
    bool *blocked_out)
{
    if (component->scrollbar_visibility.state.pending) {
        *blocked_out = true;
        return BOXRIG_UI_OK;
    }
    bool scrollbar_handled;
    boxrig_ui_status scrollbar_status = process_scrollbar_item(component, item, &scrollbar_handled);
    if (scrollbar_status != BOXRIG_UI_OK || scrollbar_handled) return scrollbar_status;
    boxrig_wayland_seat *seat = item->seat;
    uint32_t mask = item->native_code;
    bool captured = mask != 0
                        ? (seat->pointer_application_buttons & mask) != 0
                        : seat->pointer_application_buttons != 0;
    boxrig_input_mouse_route route = BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    boxrig_status status;

    if (!item->route_resolved) {
        if (component->mouse_router != NULL) {
            boxrig_input_mouse_route observed = component->mouse_router(
                &item->value.mouse,
                &item->mouse_hit,
                component->mouse_router_context);

            if (observed != BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL &&
                observed != BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION) {
                return BOXRIG_UI_UNAVAILABLE;
            }
            if (!captured || item->value.mouse.action == BOXRIG_MOUSE_PRESS) {
                route = observed;
            }
        }
        if (captured) {
            route = BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
        }
        item->application_route =
            route == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
        item->route_resolved = true;
        if (item->value.mouse.action == BOXRIG_MOUSE_PRESS && mask != 0 &&
            item->application_route) {
            seat->pointer_application_buttons |= mask;
        }
    }
    if (!item->application_route && accepts_item(component, item)) {
        status = boxrig_terminal_send_mouse(
            component->terminal,
            &item->value.mouse);
        if (status == BOXRIG_STATUS_WOULD_BLOCK) {
            *blocked_out = true;
            return BOXRIG_UI_OK;
        }
        if (status != BOXRIG_STATUS_OK) {
            return BOXRIG_UI_UNAVAILABLE;
        }
    }
    if (item->value.mouse.action == BOXRIG_MOUSE_RELEASE && mask != 0) {
        seat->pointer_application_buttons &= ~mask;
    }
    return BOXRIG_UI_OK;
}

static void invalidate_composition(
    boxrig_wayland_terminal_component *component)
{
    boxrig_ui_dirty dirty;

    if (!component->allocated || component->layout.rows == 0 ||
        component->layout.columns == 0) {
        return;
    }
    dirty.row = 0;
    dirty.row_count = component->layout.rows;
    dirty.column = 0;
    dirty.column_count = component->layout.columns;
    (void)boxrig_ui_view_invalidate(
        component->view,
        BOXRIG_UI_DIRTY_OVERLAY,
        dirty);
}

static boxrig_ui_status process_document_batch(
    boxrig_wayland_terminal_component *component,
    boxrig_wayland_input_item *item,
    bool *blocked_out,
    bool *consumed_out)
{
    boxrig_wayland_seat *seat = item->seat;
    boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(component->view);
    boxrig_ui_editable_document_snapshot snapshot;
    if (seat == NULL || !seat->text_enabled || seat->text_component != component ||
        item->text_epoch != seat->text_epoch ||
        seat->text_document_generation != boxrig_ui_editable_document_input_generation(document) ||
        !boxrig_ui_editable_document_get_snapshot(document, &snapshot)) return BOXRIG_UI_OK;

    uint64_t first = snapshot.selection.offset;
    uint64_t last = first + snapshot.selection.length;
    uint64_t before = item->delete_before, after = item->delete_after;
    if (before != 0 || after != 0) {
        if (!seat->text_context_available || first < seat->text_context_offset ||
            before > first - seat->text_context_offset ||
            last > seat->text_context_offset + seat->text_context_length ||
            after > seat->text_context_offset + seat->text_context_length - last) goto rejected;
    }
    bool replace_selection = item->commit_set || item->value.composition.text_length != 0;
    boxrig_ui_editable_edit edit = {snapshot.input_generation,
        {first - before, last - first + before + after},
        item->commit_text, item->commit_length, 0, 0};
    if (replace_selection) {
        if (edit.replacement_length > UINT64_MAX - edit.range.offset) goto rejected;
        edit.cursor_after = edit.anchor_after = edit.range.offset + edit.replacement_length;
    } else {
        /* Deletion lengths exclude selected text. Preserve that selection
         * when neither a commit nor a new preedit replaces it. */
        edit.replacement = snapshot.context + (size_t)(first - snapshot.context_offset);
        edit.replacement_length = (size_t)snapshot.selection.length;
        edit.cursor_after = snapshot.cursor - before;
        edit.anchor_after = snapshot.anchor - before;
    }
    /* Prepare the next visual state before accepting an owner edit. No paint
     * can interleave this dispatch; a refused edit cancels this preedit. */
    if (!item->preedit_prepared) {
        if (boxrig_composition_apply_event(component->composition, &item->value.composition) !=
            BOXRIG_COMPOSITION_OK) goto rejected;
        item->preedit_prepared = true;
    }
    if (before != 0 || after != 0 || (replace_selection &&
            (snapshot.selection.length != 0 || item->commit_length != 0))) {
        boxrig_ui_status status = boxrig_ui_editable_document_apply_edit(document, &edit);
        if (status == BOXRIG_UI_BUSY) {
            *blocked_out = true;
            *consumed_out = false;
            invalidate_composition(component);
            return BOXRIG_UI_OK;
        }
        if (status != BOXRIG_UI_OK) goto rejected;
        seat->text_document_generation = boxrig_ui_editable_document_input_generation(document);
        if (seat->text_context_available)
            seat->text_context_length = seat->text_context_length - (size_t)edit.range.length + edit.replacement_length;
    }
    invalidate_composition(component);
    return BOXRIG_UI_OK;
rejected:
    component->input_error_count++;
    seat->text_reset_required = true;
    seat->text_epoch++;
    boxrig_composition_event cancel = {.type = BOXRIG_COMPOSITION_CANCEL};
    (void)boxrig_composition_apply_event(component->composition, &cancel);
    invalidate_composition(component);
    return BOXRIG_UI_OK;
}

static boxrig_ui_status process_composition_item(
    boxrig_wayland_terminal_component *component,
    boxrig_wayland_input_item *item,
    bool *blocked_out,
    bool *consumed_out)
{
    /* Ordinary terminal commits already captured in the FIFO retain their
     * destination. A previous native preedit cannot enter a new document. */
    if (item->text_epoch != 0 && item->seat != NULL &&
        item->value.composition.type != BOXRIG_COMPOSITION_COMMIT &&
        item->text_epoch != item->seat->text_epoch) return BOXRIG_UI_OK;
    boxrig_composition_status status = boxrig_composition_apply_event(
        component->composition,
        &item->value.composition);

    if (status == BOXRIG_COMPOSITION_WOULD_BLOCK) {
        *blocked_out = true;
        *consumed_out = true;
        invalidate_composition(component);
        return BOXRIG_UI_OK;
    }
    if (status == BOXRIG_COMPOSITION_OUT_OF_MEMORY) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    if (status != BOXRIG_COMPOSITION_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    invalidate_composition(component);
    return BOXRIG_UI_OK;
}

static boxrig_ui_status apply_input_item(
    boxrig_wayland_terminal_component *component,
    boxrig_wayland_input_item *item,
    bool *blocked_out,
    bool *consumed_out)
{
    *blocked_out = false;
    *consumed_out = true;
    if (item->type != BOXRIG_WAYLAND_INPUT_KEY && item->type != BOXRIG_WAYLAND_INPUT_FOCUS &&
        item->type != BOXRIG_WAYLAND_INPUT_MOUSE && !accepts_item(component, item)) return BOXRIG_UI_OK;
    if (item->type == BOXRIG_WAYLAND_INPUT_IME_REPLY)
        return boxrig_wayland_ibus_apply_reply(component, item, blocked_out, consumed_out);
    if (item->type == BOXRIG_WAYLAND_INPUT_DOCUMENT_BATCH)
        return process_document_batch(component, item, blocked_out, consumed_out);
    if (item->type == BOXRIG_WAYLAND_INPUT_KEY) {
        boxrig_ui_status status = process_key_item(
            component,
            item,
            blocked_out);

        *consumed_out = !*blocked_out;
        return status;
    }
    if (item->type == BOXRIG_WAYLAND_INPUT_FOCUS) {
        boxrig_ui_status status = process_focus_item(
            component,
            item,
            blocked_out);

        *consumed_out = !*blocked_out;
        return status;
    }
    if (item->type == BOXRIG_WAYLAND_INPUT_MOUSE) {
        boxrig_ui_status status = process_mouse_item(
            component,
            item,
            blocked_out);

        *consumed_out = !*blocked_out;
        return status;
    }
    if (item->type == BOXRIG_WAYLAND_INPUT_COMPOSITION) {
        return process_composition_item(
            component,
            item,
            blocked_out,
            consumed_out);
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_wayland_input_apply_item(boxrig_wayland_terminal_component *component,
    boxrig_wayland_input_item *item, bool *blocked_out, bool *consumed_out)
{
    bool processing = component->processing_input;
    component->processing_input = true;
    boxrig_ui_status result = apply_input_item(component, item, blocked_out, consumed_out);
    component->processing_input = processing;
    return result;
}

size_t boxrig_wayland_input_dispatch(
    boxrig_wayland_context *context,
    size_t maximum_items,
    bool *additional_dispatch_out)
{
    boxrig_wayland_terminal_component *component;
    size_t processed = 0;
    bool ready = false;

    *additional_dispatch_out = false;
    boxrig_wayland_ibus_update(context);
    for (boxrig_wayland_seat *seat = context->seats; seat != NULL; seat = seat->next)
        commit_text_input_state(seat, false, false);
    do {
        ready = false;
        for (component = context->components;
             component != NULL;
             component = component->next) {
            boxrig_ui_status stop = boxrig_ui_input_control_service(&component->input_control, component->terminal);
            if (stop != BOXRIG_UI_OK && stop != BOXRIG_UI_BUSY) {
                component->failure_reason = BOXRIG_WAYLAND_FAILURE_INPUT;
                continue;
            }
            bool processing = component->processing_input;
            component->processing_input = true;
            boxrig_composition_status retry_status =
                boxrig_composition_retry_commit(component->composition);
            component->processing_input = processing;

            if (retry_status == BOXRIG_COMPOSITION_WOULD_BLOCK) {
                component->input_blocked = true;
                continue;
            }
            if (retry_status != BOXRIG_COMPOSITION_OK) {
                component->failure_reason =
                    retry_status == BOXRIG_COMPOSITION_OUT_OF_MEMORY
                        ? BOXRIG_WAYLAND_FAILURE_RESOURCE
                        : BOXRIG_WAYLAND_FAILURE_INPUT;
                continue;
            }
            component->input_blocked = component->input_control.state.phase == BOXRIG_UI_INPUT_STOPPING;
            while (processed < maximum_items &&
                   component->input_queue_depth != 0 &&
                   component->failure_reason ==
                       BOXRIG_WAYLAND_FAILURE_NONE) {
                boxrig_wayland_input_item *item =
                    &component->input_items[component->input_queue_head];
                bool blocked;
                bool consumed;
                boxrig_ui_status status = boxrig_wayland_input_apply_item(
                    component,
                    item,
                    &blocked,
                    &consumed);

                if (status != BOXRIG_UI_OK) {
                    component->failure_reason =
                        status == BOXRIG_UI_OUT_OF_MEMORY
                            ? BOXRIG_WAYLAND_FAILURE_RESOURCE
                            : BOXRIG_WAYLAND_FAILURE_INPUT;
                    break;
                }
                component->input_blocked = component->input_control.state.phase == BOXRIG_UI_INPUT_STOPPING ||
                    (blocked && !component->ime_waiting);
                if (consumed) {
                    boxrig_wayland_input_free_item(item);
                    component->input_queue_head =
                        (component->input_queue_head + 1) %
                        component->config.maximum_input_events;
                    component->input_queue_depth--;
                    processed++;
                }
                if (blocked) {
                    break;
                }
            }
            if (component->input_queue_depth != 0 &&
                (!input_enabled(component) || !component->input_blocked) && !component->ime_waiting &&
                component->failure_reason == BOXRIG_WAYLAND_FAILURE_NONE) {
                ready = true;
            }
        }
        if (processed < maximum_items && queue_due_repeat(context)) {
            ready = true;
        }
    } while (processed < maximum_items && ready);
    if (processed == maximum_items) {
        boxrig_wayland_seat *seat;

        for (seat = context->seats; seat != NULL; seat = seat->next) {
            if (seat->repeat_active && seat->repeat_component != NULL &&
                (!input_enabled(seat->repeat_component) || !seat->repeat_component->input_blocked) &&
                !seat->repeat_component->ime_waiting &&
                seat->repeat_deadline <= context->monotonic_milliseconds) {
                ready = true;
                break;
            }
        }
    }
    *additional_dispatch_out = ready;
    for (boxrig_wayland_seat *seat = context->seats; seat != NULL; seat = seat->next)
        commit_text_input_state(seat, false, false);
    return processed;
}

bool boxrig_wayland_input_repeat_deadline(
    const boxrig_wayland_context *context,
    uint64_t *deadline_out)
{
    const boxrig_wayland_seat *seat;
    bool found = false;
    uint64_t deadline = UINT64_MAX;

    for (seat = context->seats; seat != NULL; seat = seat->next) {
        if (seat->repeat_active && seat->repeat_component != NULL &&
            (!input_enabled(seat->repeat_component) || !seat->repeat_component->input_blocked) &&
            !seat->repeat_component->ime_waiting &&
            seat->repeat_deadline < deadline) {
            deadline = seat->repeat_deadline;
            found = true;
        }
    }
    if (found && deadline_out != NULL) {
        *deadline_out = deadline;
    }
    return found;
}

void boxrig_wayland_input_update_cursor_rectangle(
    boxrig_wayland_terminal_component *component)
{
    if (component != NULL) boxrig_wayland_ibus_cursor(component, true);
    boxrig_wayland_seat *seat;

    if (component == NULL || component->context == NULL) {
        return;
    }
    for (seat = component->context->seats;
         seat != NULL;
         seat = seat->next) {
        if (seat->text_component == component && seat->text_enabled) {
            commit_text_input_state(seat, false, true);
        }
    }
}

void boxrig_wayland_input_use_ibus(boxrig_wayland_context *context, bool enabled)
{
    for (boxrig_wayland_seat *seat = context->seats; seat != NULL; seat = seat->next) {
        if (seat->text_input == NULL || seat->text_component == NULL) continue;
        if (enabled && seat->text_enabled) {
            zwp_text_input_v3_disable(seat->text_input);
            zwp_text_input_v3_commit(seat->text_input);
            seat->text_commit_serial++;
            seat->text_enabled = false;
            seat->text_epoch++;
            reset_text_batch(seat);
            queue_composition_cancel(seat, seat->text_component);
            context->flush_required = true;
        } else if (!enabled && !seat->text_enabled && input_enabled(seat->text_component)) {
            seat->text_enabled = true;
            commit_text_input_state(seat, true, false);
        }
    }
}
