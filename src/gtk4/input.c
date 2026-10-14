/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "input.h"
#include "../ui/input_control.h"

#include "boxrig/ui/input_text.h"
#include "boxrig/ui/editable.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BOXRIG_GTK4_PHYSICAL_KEY_STATES 512U
#define BOXRIG_GTK4_PREEDIT_SEGMENTS 256U

#include "../ime/client.h"
#include "../gtk/ime_probe.h"
#include "../gtk/ime_cursor.h"
#include "../ime/source.h"
#include <ibus.h>

typedef enum input_item_kind {
    INPUT_ITEM_KEY = 0,
    INPUT_ITEM_FOCUS,
    INPUT_ITEM_MOUSE,
    INPUT_ITEM_COMPOSITION,
    INPUT_ITEM_PASTE,
    INPUT_ITEM_IME_REPLY
} input_item_kind;

typedef enum paste_item_state {
    PASTE_ITEM_BEGIN = 0,
    PASTE_ITEM_CHUNK,
    PASTE_ITEM_END
} paste_item_state;

typedef struct key_item {
    boxrig_key_event event;
    guint physical_keycode;
    guint symbol;
    uint32_t ime_modifiers, ime_time;
    uint64_t ime_generation;
    uint64_t focus_epoch;
    bool deferred;
    GdkEvent *native_event;
    boxrig_ime_request *ime_request;
    bool fallback_processed, fallback_handled;
    char *text;
    bool routed;
    bool send_to_terminal;
    bool inhibited;
} key_item;

typedef struct focus_item {
    boxrig_focus_event event;
    bool composition_updated;
} focus_item;

typedef struct mouse_item {
    boxrig_mouse_event event;
    boxrig_ui_hit_test_result hit;
    uint32_t changed_button;
    bool routed;
    bool send_to_terminal;
} mouse_item;

typedef struct composition_item {
    boxrig_composition_event event;
    uint64_t im_epoch;
    bool editable;
    char *text;
    boxrig_composition_segment *segments;
} composition_item;

typedef struct paste_item {
    char *text;
    size_t length;
    size_t offset;
    paste_item_state state;
} paste_item;

typedef struct input_item {
    input_item_kind kind;
    uint64_t generation;
    union {
        key_item key;
        focus_item focus;
        mouse_item mouse;
        composition_item composition;
        paste_item paste;
        boxrig_ime_request *ime_reply;
    } value;
} input_item;

typedef struct physical_key_state {
    guint keycode;
    bool occupied;
    bool native_pressed;
    bool inhibited;
    bool application_captured;
} physical_key_state;

typedef struct clipboard_offer {
    GdkClipboard *clipboard;
    GdkContentProvider *provider;
    GBytes *bytes;
    gulong changed_handler;
    size_t length;
} clipboard_offer;

typedef struct clipboard_transfer {
    boxrig_gtk4_input *input;
    GdkClipboard *clipboard;
    GCancellable *cancellable;
    GInputStream *stream;
    uint8_t *bytes;
    size_t length;
    size_t capacity;
    bool active;
} clipboard_transfer;

struct boxrig_gtk4_input {
    GtkWidget *widget;
    boxrig_ui_candidate_policy candidate_policy;
    uint32_t candidate_buttons;
    boxrig_terminal *terminal;
    boxrig_ui_view *view;
    GtkEventControllerKey *key_controller;
    GtkIMContext *im_context;
    boxrig_gtk_ime_probe *ime_probe;
    boxrig_gtk_ime_state ime_state;
    uint64_t focus_epoch;
    bool ime_pending;
    boxrig_ime_client *ibus;
    boxrig_ime_session *ibus_session;
    boxrig_ime_target ibus_target;
    GSource *ibus_source;
    uint64_t ibus_generation;
    size_t ibus_errors;
    bool ibus_requested, ibus_selected, ibus_switch_pending, ime_waiting;
    unsigned int candidate_scale;
    boxrig_ui_editable_document *document;
    uint64_t surrounding_generation;
    uint64_t im_epoch;
    uint64_t commit_epoch;
    bool commit_editable;
    bool im_blocked;
    bool im_focused;
    bool processing;
    bool reset_native_input;
    boxrig_ui_input_control control;
    bool filtering_key;
    size_t filter_effects;
    GtkEventControllerFocus *focus_controller;
    GtkGestureClick *click_gesture;
    GtkEventControllerMotion *motion_controller;
    GtkEventControllerScroll *scroll_controller;
    input_item *items;
    clipboard_transfer *clipboard_transfers;
    clipboard_offer clipboard_offers[2];
    physical_key_state physical_keys[BOXRIG_GTK4_PHYSICAL_KEY_STATES];
    size_t queue_head;
    size_t queue_count;
    size_t queue_capacity;
    size_t queue_high_water;
    size_t paste_capacity;
    size_t paste_chunk_size;
    size_t maximum_clipboard_transfers;
    size_t maximum_clipboard_bytes;
    size_t active_clipboard_transfers;
    size_t outgoing_clipboard_bytes;
    boxrig_input_key_router key_router;
    void *key_router_context;
    boxrig_input_mouse_router mouse_router;
    void *mouse_router_context;
    boxrig_composition *composition;
    boxrig_gtk4_input_activity_callback activity_callback;
    void *activity_context;
    boxrig_ui_component_failure_reason failure_reason;
    size_t reference_count;
    uint32_t pressed_buttons;
    uint32_t application_buttons;
    double pointer_x;
    double pointer_y;
    bool pointer_position_valid;
    bool terminal_blocked;
    bool accepting;
    bool rooted;
    bool mapped;
    bool destroyed;
};

static bool refresh_im_context(boxrig_gtk4_input *input);
static bool initialize_ibus(boxrig_gtk4_input *input);
static bool resolve_ime(boxrig_gtk4_input *input);
static void attach_ime(boxrig_gtk4_input *input);
static void focus_ibus(boxrig_gtk4_input *input, bool focused, bool commit);
static void ibus_selection_changed(void *data, bool selected);
static void reject_surrounding(boxrig_gtk4_input *input);
static bool current_im_context(boxrig_gtk4_input *input, GtkIMContext *context);
static boxrig_ui_status process_composition_item(boxrig_gtk4_input *input,
    composition_item *item, bool *finished_out);

static void cancel_clipboard_transfers(
    boxrig_gtk4_input *input,
    GdkClipboard *clipboard);

static bool input_enabled(const boxrig_gtk4_input *input)
{
    return input->control.state.phase == BOXRIG_UI_INPUT_ENABLED;
}

static void notify_activity(boxrig_gtk4_input *input)
{
    if (input->activity_callback != NULL) {
        input->activity_callback(input->activity_context);
    }
}

static void reject_surrounding(boxrig_gtk4_input *input)
{
    input->im_blocked = true;
    notify_activity(input);
}

static bool current_im_context(boxrig_gtk4_input *input, GtkIMContext *context)
{
    if (!input->accepting || !input_enabled(input) || context != input->im_context ||
        input->im_blocked || !input->im_focused) return false;
    if (input->surrounding_generation !=
        boxrig_ui_editable_document_input_generation(input->document)) {
        reject_surrounding(input);
        return false;
    }
    return true;
}

static boxrig_status commit_text(const char *text, size_t length, void *data)
{
    boxrig_gtk4_input *input = data;
    if (!input_enabled(input)) return BOXRIG_STATUS_OK;
    if (!input->commit_editable)
        return boxrig_terminal_send_text(input->terminal, text, length);
    boxrig_ui_editable_document_snapshot snapshot;
    if (input->commit_epoch != input->im_epoch ||
        !current_im_context(input, input->im_context) ||
        !boxrig_ui_editable_document_get_snapshot(input->document, &snapshot))
        return BOXRIG_STATUS_OK;
    if (length > UINT64_MAX - snapshot.selection.offset) {
        reject_surrounding(input);
        return BOXRIG_STATUS_OK;
    }
    uint64_t after = snapshot.selection.offset + length;
    boxrig_ui_editable_edit edit = {snapshot.input_generation, snapshot.selection,
        text, length, after, after};
    boxrig_ui_status status = boxrig_ui_editable_document_apply_edit(input->document, &edit);
    if (status == BOXRIG_UI_BUSY) return BOXRIG_STATUS_WOULD_BLOCK;
    if (status == BOXRIG_UI_OK)
        input->surrounding_generation = boxrig_ui_editable_document_input_generation(input->document);
    else
        reject_surrounding(input);
    /* A refused document replacement must never fall through to the PTY. */
    return BOXRIG_STATUS_OK;
}

static void fail_input(
    boxrig_gtk4_input *input,
    boxrig_ui_component_failure_reason reason)
{
    if (input->failure_reason == BOXRIG_UI_COMPONENT_FAILURE_NONE) {
        input->failure_reason = reason;
        input->accepting = false;
        cancel_clipboard_transfers(input, NULL);
        notify_activity(input);
    }
}

static void retain_input(boxrig_gtk4_input *input)
{
    input->reference_count++;
}

static void release_input(boxrig_gtk4_input *input)
{
    if (input == NULL) {
        return;
    }
    input->reference_count--;
    if (input->reference_count == 0) {
        free(input->clipboard_transfers);
        free(input);
    }
}

static physical_key_state *find_physical_key(
    boxrig_gtk4_input *input,
    guint keycode,
    bool create)
{
    physical_key_state *available = NULL;
    size_t index;

    for (index = 0; index < BOXRIG_GTK4_PHYSICAL_KEY_STATES; index++) {
        physical_key_state *state = &input->physical_keys[index];

        if (state->occupied && state->keycode == keycode) {
            return state;
        }
        if (!state->occupied && available == NULL) {
            available = state;
        }
    }
    if (!create || available == NULL) {
        return NULL;
    }
    memset(available, 0, sizeof(*available));
    available->occupied = true;
    available->keycode = keycode;
    return available;
}

static void release_physical_key_if_idle(physical_key_state *state)
{
    if (state != NULL && !state->native_pressed &&
        !state->application_captured) {
        memset(state, 0, sizeof(*state));
    }
}

static void destroy_input_item(input_item *item)
{
    if (item == NULL) {
        return;
    }
    if (item->kind == INPUT_ITEM_KEY) {
        free(item->value.key.text);
        if (item->value.key.native_event != NULL) gdk_event_unref(item->value.key.native_event);
        boxrig_ime_release_request(item->value.key.ime_request);
    } else if (item->kind == INPUT_ITEM_IME_REPLY) {
        boxrig_ime_release_request(item->value.ime_reply);
    } else if (item->kind == INPUT_ITEM_COMPOSITION) {
        free(item->value.composition.segments);
        free(item->value.composition.text);
    } else if (item->kind == INPUT_ITEM_PASTE) {
        free(item->value.paste.text);
    }
    memset(item, 0, sizeof(*item));
}

static input_item *queue_tail(boxrig_gtk4_input *input)
{
    size_t index;

    if (input->queue_count == input->queue_capacity) {
        fail_input(
            input,
            BOXRIG_UI_COMPONENT_FAILURE_NATIVE_INPUT_OVERFLOW);
        return NULL;
    }
    index = (input->queue_head + input->queue_count) % input->queue_capacity;
    input->queue_count++;
    if (input->queue_count > input->queue_high_water) {
        input->queue_high_water = input->queue_count;
    }
    memset(&input->items[index], 0, sizeof(input->items[index]));
    input->items[index].generation = input->control.state.generation;
    return &input->items[index];
}

static void queue_finished(boxrig_gtk4_input *input)
{
    destroy_input_item(&input->items[input->queue_head]);
    input->queue_head = (input->queue_head + 1) % input->queue_capacity;
    input->queue_count--;
}

static uint32_t translate_modifiers(GdkModifierType state)
{
    uint32_t modifiers = 0;

    if ((state & GDK_SHIFT_MASK) != 0) {
        modifiers |= BOXRIG_MODIFIER_SHIFT;
    }
    if ((state & GDK_ALT_MASK) != 0) {
        modifiers |= BOXRIG_MODIFIER_ALT;
    }
    if ((state & GDK_CONTROL_MASK) != 0) {
        modifiers |= BOXRIG_MODIFIER_CONTROL;
    }
    if ((state & GDK_SUPER_MASK) != 0) {
        modifiers |= BOXRIG_MODIFIER_SUPER;
    }
    if ((state & GDK_HYPER_MASK) != 0) {
        modifiers |= BOXRIG_MODIFIER_HYPER;
    }
    if ((state & GDK_META_MASK) != 0) {
        modifiers |= BOXRIG_MODIFIER_META;
    }
    if ((state & GDK_LOCK_MASK) != 0) {
        modifiers |= BOXRIG_MODIFIER_CAPS_LOCK;
    }
    return modifiers;
}

static boxrig_key translate_functional_key(guint keyval)
{
    if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F35) {
        return (boxrig_key)(BOXRIG_KEY_F1 + (keyval - GDK_KEY_F1));
    }
    switch (keyval) {
    case GDK_KEY_Return:
        return BOXRIG_KEY_ENTER;
    case GDK_KEY_BackSpace:
        return BOXRIG_KEY_BACKSPACE;
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab:
    case GDK_KEY_KP_Tab:
        return BOXRIG_KEY_TAB;
    case GDK_KEY_Escape:
        return BOXRIG_KEY_ESCAPE;
    case GDK_KEY_Up:
        return BOXRIG_KEY_UP;
    case GDK_KEY_Down:
        return BOXRIG_KEY_DOWN;
    case GDK_KEY_Right:
        return BOXRIG_KEY_RIGHT;
    case GDK_KEY_Left:
        return BOXRIG_KEY_LEFT;
    case GDK_KEY_Home:
        return BOXRIG_KEY_HOME;
    case GDK_KEY_End:
        return BOXRIG_KEY_END;
    case GDK_KEY_Insert:
        return BOXRIG_KEY_INSERT;
    case GDK_KEY_Delete:
        return BOXRIG_KEY_DELETE;
    case GDK_KEY_Page_Up:
        return BOXRIG_KEY_PAGE_UP;
    case GDK_KEY_Page_Down:
        return BOXRIG_KEY_PAGE_DOWN;
    case GDK_KEY_Caps_Lock:
        return BOXRIG_KEY_CAPS_LOCK;
    case GDK_KEY_Scroll_Lock:
        return BOXRIG_KEY_SCROLL_LOCK;
    case GDK_KEY_Num_Lock:
        return BOXRIG_KEY_NUM_LOCK;
    case GDK_KEY_Print:
        return BOXRIG_KEY_PRINT_SCREEN;
    case GDK_KEY_Pause:
        return BOXRIG_KEY_PAUSE;
    case GDK_KEY_Menu:
        return BOXRIG_KEY_MENU;
    case GDK_KEY_KP_0:
        return BOXRIG_KEY_KP_0;
    case GDK_KEY_KP_1:
        return BOXRIG_KEY_KP_1;
    case GDK_KEY_KP_2:
        return BOXRIG_KEY_KP_2;
    case GDK_KEY_KP_3:
        return BOXRIG_KEY_KP_3;
    case GDK_KEY_KP_4:
        return BOXRIG_KEY_KP_4;
    case GDK_KEY_KP_5:
        return BOXRIG_KEY_KP_5;
    case GDK_KEY_KP_6:
        return BOXRIG_KEY_KP_6;
    case GDK_KEY_KP_7:
        return BOXRIG_KEY_KP_7;
    case GDK_KEY_KP_8:
        return BOXRIG_KEY_KP_8;
    case GDK_KEY_KP_9:
        return BOXRIG_KEY_KP_9;
    case GDK_KEY_KP_Decimal:
        return BOXRIG_KEY_KP_DECIMAL;
    case GDK_KEY_KP_Divide:
        return BOXRIG_KEY_KP_DIVIDE;
    case GDK_KEY_KP_Multiply:
        return BOXRIG_KEY_KP_MULTIPLY;
    case GDK_KEY_KP_Subtract:
        return BOXRIG_KEY_KP_SUBTRACT;
    case GDK_KEY_KP_Add:
        return BOXRIG_KEY_KP_ADD;
    case GDK_KEY_KP_Enter:
        return BOXRIG_KEY_KP_ENTER;
    case GDK_KEY_KP_Equal:
        return BOXRIG_KEY_KP_EQUAL;
    case GDK_KEY_KP_Separator:
        return BOXRIG_KEY_KP_SEPARATOR;
    case GDK_KEY_KP_Left:
        return BOXRIG_KEY_KP_LEFT;
    case GDK_KEY_KP_Right:
        return BOXRIG_KEY_KP_RIGHT;
    case GDK_KEY_KP_Up:
        return BOXRIG_KEY_KP_UP;
    case GDK_KEY_KP_Down:
        return BOXRIG_KEY_KP_DOWN;
    case GDK_KEY_KP_Page_Up:
        return BOXRIG_KEY_KP_PAGE_UP;
    case GDK_KEY_KP_Page_Down:
        return BOXRIG_KEY_KP_PAGE_DOWN;
    case GDK_KEY_KP_Home:
        return BOXRIG_KEY_KP_HOME;
    case GDK_KEY_KP_End:
        return BOXRIG_KEY_KP_END;
    case GDK_KEY_KP_Insert:
        return BOXRIG_KEY_KP_INSERT;
    case GDK_KEY_KP_Delete:
        return BOXRIG_KEY_KP_DELETE;
    case GDK_KEY_KP_Begin:
        return BOXRIG_KEY_KP_BEGIN;
    case GDK_KEY_Shift_L:
        return BOXRIG_KEY_LEFT_SHIFT;
    case GDK_KEY_Control_L:
        return BOXRIG_KEY_LEFT_CONTROL;
    case GDK_KEY_Alt_L:
        return BOXRIG_KEY_LEFT_ALT;
    case GDK_KEY_Super_L:
        return BOXRIG_KEY_LEFT_SUPER;
    case GDK_KEY_Hyper_L:
        return BOXRIG_KEY_LEFT_HYPER;
    case GDK_KEY_Meta_L:
        return BOXRIG_KEY_LEFT_META;
    case GDK_KEY_Shift_R:
        return BOXRIG_KEY_RIGHT_SHIFT;
    case GDK_KEY_Control_R:
        return BOXRIG_KEY_RIGHT_CONTROL;
    case GDK_KEY_Alt_R:
        return BOXRIG_KEY_RIGHT_ALT;
    case GDK_KEY_Super_R:
        return BOXRIG_KEY_RIGHT_SUPER;
    case GDK_KEY_Hyper_R:
        return BOXRIG_KEY_RIGHT_HYPER;
    case GDK_KEY_Meta_R:
        return BOXRIG_KEY_RIGHT_META;
    case GDK_KEY_ISO_Level3_Shift:
        return BOXRIG_KEY_ISO_LEVEL3_SHIFT;
    case GDK_KEY_ISO_Level5_Shift:
        return BOXRIG_KEY_ISO_LEVEL5_SHIFT;
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
    default:
        return 0;
    }
}

static uint32_t keyval_codepoint(guint keyval)
{
    gunichar codepoint = gdk_keyval_to_unicode(keyval);

    if (codepoint == 0 || !g_unichar_validate(codepoint) ||
        g_unichar_iscntrl(codepoint)) {
        return 0;
    }
    return (uint32_t)codepoint;
}

static int current_key_group(GtkEventControllerKey *controller)
{
    GdkEvent *event = gtk_event_controller_get_current_event(
        GTK_EVENT_CONTROLLER(controller));
    guint layout;

    if (event == NULL ||
        (gdk_event_get_event_type(event) != GDK_KEY_PRESS &&
         gdk_event_get_event_type(event) != GDK_KEY_RELEASE)) {
        return 0;
    }
    layout = gdk_key_event_get_layout(event);
    return layout <= INT_MAX ? (int)layout : 0;
}

static uint32_t ascii_base_layout_codepoint(
    GdkDisplay *display,
    guint keycode)
{
    GdkKeymapKey *keys = NULL;
    guint *keyvals = NULL;
    uint32_t candidate = 0;
    bool ambiguous = false;
    int entry_count = 0;
    int index;

    if (!gdk_display_map_keycode(
            display,
            keycode,
            &keys,
            &keyvals,
            &entry_count)) {
        return 0;
    }
    for (index = 0; index < entry_count; index++) {
        uint32_t codepoint;

        if (keys[index].level != 0) {
            continue;
        }
        codepoint = keyval_codepoint(keyvals[index]);
        if (codepoint < 0x20 || codepoint > 0x7e) {
            continue;
        }
        if (candidate != 0 && candidate != codepoint) {
            ambiguous = true;
            break;
        }
        candidate = codepoint;
    }
    g_free(keys);
    g_free(keyvals);
    return ambiguous ? 0 : candidate;
}

static void populate_text_key_identity(
    boxrig_gtk4_input *input,
    GtkEventControllerKey *controller,
    guint keyval,
    guint keycode,
    GdkModifierType state,
    boxrig_key_event *event)
{
    GdkDisplay *display = gtk_widget_get_display(input->widget);
    guint unshifted = GDK_KEY_VoidSymbol;
    guint shifted = GDK_KEY_VoidSymbol;
    int effective_group;
    int level;
    GdkModifierType consumed;
    int group;

    if (display == NULL) {
        uint32_t current = keyval_codepoint(keyval);

        if ((state & GDK_SHIFT_MASK) != 0) {
            event->shifted_codepoint = current;
        } else {
            event->codepoint = current;
        }
        return;
    }
    group = current_key_group(controller);
    if (gdk_display_translate_key(
            display,
            keycode,
            0,
            group,
            &unshifted,
            &effective_group,
            &level,
            &consumed)) {
        event->codepoint = keyval_codepoint(unshifted);
    }
    if (gdk_display_translate_key(
            display,
            keycode,
            GDK_SHIFT_MASK,
            group,
            &shifted,
            &effective_group,
            &level,
            &consumed)) {
        event->shifted_codepoint = keyval_codepoint(shifted);
    }
    if (event->codepoint == 0 && event->shifted_codepoint == 0) {
        uint32_t current = keyval_codepoint(keyval);

        if ((state & GDK_SHIFT_MASK) != 0) {
            event->shifted_codepoint = current;
        } else {
            event->codepoint = current;
        }
    }
    if (event->shifted_codepoint == event->codepoint) {
        event->shifted_codepoint = 0;
    }
    event->base_layout_codepoint = ascii_base_layout_codepoint(
        display,
        keycode);
}

static bool copy_key_text(guint keyval, key_item *item)
{
    gunichar codepoint;
    char encoded[8];
    int length;

    if (item->event.key != BOXRIG_KEY_NONE) {
        return true;
    }
    codepoint = gdk_keyval_to_unicode(keyval);
    if (codepoint == 0 || !g_unichar_validate(codepoint)) {
        return true;
    }
    if (g_unichar_iscntrl(codepoint)) {
        return true;
    }
    length = g_unichar_to_utf8(codepoint, encoded);
    if (length <= 0) {
        return false;
    }
    item->text = malloc((size_t)length);
    if (item->text == NULL) {
        return false;
    }
    memcpy(item->text, encoded, (size_t)length);
    item->event.text = item->text;
    item->event.text_length = (size_t)length;
    return true;
}

/* IBus shares the low modifier bits with GDK; Super is also Mod4. */
static uint32_t ibus_modifiers(guint symbol, GdkModifierType state, bool release)
{
    uint32_t modifiers = (uint32_t)state & (IBUS_SHIFT_MASK | IBUS_LOCK_MASK | IBUS_CONTROL_MASK |
        IBUS_MOD1_MASK | IBUS_MOD2_MASK | IBUS_MOD3_MASK | IBUS_MOD4_MASK | IBUS_MOD5_MASK |
        IBUS_SUPER_MASK | IBUS_HYPER_MASK | IBUS_META_MASK);
    uint32_t changed = 0;
    if (symbol == GDK_KEY_Shift_L || symbol == GDK_KEY_Shift_R) changed = IBUS_SHIFT_MASK;
    if (symbol == GDK_KEY_Control_L || symbol == GDK_KEY_Control_R) changed = IBUS_CONTROL_MASK;
    if (symbol == GDK_KEY_Alt_L || symbol == GDK_KEY_Alt_R) changed = IBUS_MOD1_MASK;
    if (symbol == GDK_KEY_Super_L || symbol == GDK_KEY_Super_R) changed = IBUS_SUPER_MASK | IBUS_MOD4_MASK;
    modifiers = release ? modifiers & ~changed : modifiers | changed;
    if ((modifiers & IBUS_SUPER_MASK) != 0) modifiers |= IBUS_MOD4_MASK;
    return modifiers | (release ? IBUS_RELEASE_MASK : 0);
}

static void retain_ime_key(boxrig_gtk4_input *input, GtkEventControllerKey *controller,
    key_item *item, guint symbol, GdkModifierType state, bool release)
{
    item->symbol = symbol;
    item->ime_modifiers = ibus_modifiers(symbol, state, release);
    item->ime_generation = input->ibus_generation;
    item->focus_epoch = input->focus_epoch;
    item->deferred = input->ime_pending;
    GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
    if (event != NULL) {
        item->native_event = gdk_event_ref(event);
        item->ime_time = gdk_event_get_time(event);
    }
}

static gboolean key_pressed(
    GtkEventControllerKey *controller,
    guint keyval,
    guint keycode,
    GdkModifierType state,
    gpointer user_data)
{
    boxrig_gtk4_input *input = user_data;
    physical_key_state *physical;
    input_item *item;
    uint32_t changed_modifier;

    if (!input->accepting) {
        return TRUE;
    }
    if (boxrig_ui_view_notify_local_input(input->view) != BOXRIG_UI_OK) {
        fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_INPUT);
        return TRUE;
    }
    physical = find_physical_key(input, keycode, true);
    if (physical == NULL) {
        fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_INPUT);
        return TRUE;
    }
    if (!input_enabled(input)) physical->inhibited = true;
    item = queue_tail(input);
    if (item == NULL) {
        return TRUE;
    }
    item->kind = INPUT_ITEM_KEY;
    item->value.key.inhibited = physical->inhibited;
    item->value.key.physical_keycode = keycode;
    retain_ime_key(input, controller, &item->value.key, keyval, state, false);
    item->value.key.event.key = translate_functional_key(keyval);
    item->value.key.event.action = physical->native_pressed
                                       ? BOXRIG_KEY_REPEAT
                                       : BOXRIG_KEY_PRESS;
    item->value.key.event.modifiers = translate_modifiers(state);
    changed_modifier = modifier_for_key(item->value.key.event.key);
    item->value.key.event.modifiers |= changed_modifier;
    if (item->value.key.event.key == BOXRIG_KEY_NONE) {
        populate_text_key_identity(
            input,
            controller,
            keyval,
            keycode,
            state,
            &item->value.key.event);
    }
    physical->native_pressed = true;
    if (!copy_key_text(keyval, &item->value.key)) {
        queue_finished(input);
        fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_INPUT);
        return TRUE;
    }
    notify_activity(input);
    return TRUE;
}

static void key_released(
    GtkEventControllerKey *controller,
    guint keyval,
    guint keycode,
    GdkModifierType state,
    gpointer user_data)
{
    boxrig_gtk4_input *input = user_data;
    physical_key_state *physical;
    input_item *item;
    uint32_t changed_modifier;

    if (!input->accepting) {
        return;
    }
    physical = find_physical_key(input, keycode, true);
    if (physical == NULL) {
        fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_INPUT);
        return;
    }
    item = queue_tail(input);
    if (item == NULL) {
        return;
    }
    item->kind = INPUT_ITEM_KEY;
    item->value.key.inhibited = physical->inhibited;
    item->value.key.physical_keycode = keycode;
    retain_ime_key(input, controller, &item->value.key, keyval, state, true);
    item->value.key.event.key = translate_functional_key(keyval);
    item->value.key.event.action = BOXRIG_KEY_RELEASE;
    item->value.key.event.modifiers = translate_modifiers(state);
    changed_modifier = modifier_for_key(item->value.key.event.key);
    item->value.key.event.modifiers &= ~changed_modifier;
    if (item->value.key.event.key == BOXRIG_KEY_NONE) {
        populate_text_key_identity(
            input,
            controller,
            keyval,
            keycode,
            state,
            &item->value.key.event);
    }
    physical->native_pressed = false;
    physical->inhibited = false;
    notify_activity(input);
}

static bool enqueue_focus(boxrig_gtk4_input *input, bool focused)
{
    input_item *item;

    if (!input->accepting) {
        return false;
    }
    /* Visual focus follows the native event even while terminal output is
     * blocked. The queued report keeps its original input ordering. */
    if (boxrig_ui_view_set_focused(input->view, focused) != BOXRIG_UI_OK) {
        fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_INPUT);
        return false;
    }
    item = queue_tail(input);
    if (item == NULL) {
        return false;
    }
    item->kind = INPUT_ITEM_FOCUS;
    item->value.focus.event.focused = focused;
    notify_activity(input);
    return true;
}

static void focus_entered(
    GtkEventControllerFocus *controller,
    gpointer user_data)
{
    boxrig_gtk4_input *input = user_data;

    (void)controller;
    input->im_focused = true;
    focus_ibus(input, true, false);
    if (enqueue_focus(input, true) && refresh_im_context(input) && !input->ime_pending && input_enabled(input)) {
        gtk_im_context_focus_in(input->im_context);
    }
}

static void focus_left(
    GtkEventControllerFocus *controller,
    gpointer user_data)
{
    boxrig_gtk4_input *input = user_data;
    size_t index;

    (void)controller;
    focus_ibus(input, false, true);
    /* A standard context may synchronously commit its preedit on focus-out.
     * Accept that notification for the old input owner before invalidating
     * its document generation; subsequent late signals remain rejected. */
    if (enqueue_focus(input, false)) {
        gtk_im_context_focus_out(input->im_context);
    }
    input->focus_epoch++;
    input->im_focused = false;
    (void)boxrig_ui_editable_document_invalidate_input(input->document);
    reject_surrounding(input);
    for (index = 0; index < BOXRIG_GTK4_PHYSICAL_KEY_STATES; index++) {
        input->physical_keys[index].native_pressed = false;
    }
    input->pressed_buttons = 0;
}

static uint32_t button_mask(boxrig_mouse_button button)
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

static boxrig_mouse_button translate_button(guint button)
{
    switch (button) {
    case 1:
        return BOXRIG_MOUSE_BUTTON_LEFT;
    case 2:
        return BOXRIG_MOUSE_BUTTON_MIDDLE;
    case 3:
        return BOXRIG_MOUSE_BUTTON_RIGHT;
    case 8:
        return BOXRIG_MOUSE_BUTTON_8;
    case 9:
        return BOXRIG_MOUSE_BUTTON_9;
    case 10:
        return BOXRIG_MOUSE_BUTTON_10;
    case 11:
        return BOXRIG_MOUSE_BUTTON_11;
    default:
        return BOXRIG_MOUSE_BUTTON_NONE;
    }
}

static bool coordinate_to_fixed(
    double coordinate,
    unsigned int scale,
    boxrig_ui_fixed *fixed_out,
    int64_t *pixel_out)
{
    double output = coordinate * (double)scale;

    if (!isfinite(output) || output < (double)INT64_MIN / BOXRIG_UI_FIXED_ONE ||
        output >= (double)INT64_MAX / BOXRIG_UI_FIXED_ONE) {
        return false;
    }
    *fixed_out = (boxrig_ui_fixed)floor(output * BOXRIG_UI_FIXED_ONE);
    *pixel_out = (int64_t)floor(output);
    return true;
}

static bool hits_candidates(boxrig_gtk4_input *input, double x, double y)
{
    boxrig_ui_candidate_view candidates;
    if (boxrig_ui_view_get_candidates(input->view, &candidates) != BOXRIG_UI_OK) return false;
    boxrig_ui_candidate_rectangle panel = candidates.layout.bounds;
    int scale = gtk_widget_get_scale_factor(input->widget);
    x *= scale;
    y *= scale;
    return panel.width > 0 && panel.height > 0 && x >= panel.x && y >= panel.y &&
        x < (double)panel.x + panel.width && y < (double)panel.y + panel.height;
}

static bool enqueue_mouse(
    boxrig_gtk4_input *input,
    boxrig_mouse_action action,
    boxrig_mouse_button button,
    uint8_t click_count,
    double x,
    double y,
    GdkModifierType state)
{
    boxrig_ui_fixed fixed_x;
    boxrig_ui_fixed fixed_y;
    uint32_t changed = button_mask(button);
    unsigned int scale = (unsigned int)gtk_widget_get_scale_factor(input->widget);
    input_item *item;

    if (!input->accepting || scale == 0) {
        return false;
    }
    /* A release may be delivered to a different window after focus changes.
     * A fresh press begins a new sequence for this button. */
    if (action == BOXRIG_MOUSE_PRESS) input->candidate_buttons &= ~changed;
    if (action == BOXRIG_MOUSE_RELEASE && (input->candidate_buttons & changed) != 0) {
        input->candidate_buttons &= ~changed;
        return true;
    }
    if ((action == BOXRIG_MOUSE_MOTION && input->candidate_buttons != 0) ||
        (hits_candidates(input, x, y) &&
         !(action == BOXRIG_MOUSE_RELEASE && (input->pressed_buttons & changed) != 0))) {
        if (action == BOXRIG_MOUSE_PRESS) input->candidate_buttons |= changed;
        return true;
    }
    item = queue_tail(input);
    if (item == NULL) {
        return false;
    }
    item->kind = INPUT_ITEM_MOUSE;
    if (!coordinate_to_fixed(
            x,
            scale,
            &fixed_x,
            &item->value.mouse.event.x) ||
        !coordinate_to_fixed(
            y,
            scale,
            &fixed_y,
            &item->value.mouse.event.y)) {
        queue_finished(input);
        fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_INPUT);
        return false;
    }
    if (boxrig_ui_view_hit_test_with_composition(
            input->view,
            input->composition,
            fixed_x,
            fixed_y,
            &item->value.mouse.hit) != BOXRIG_UI_OK) {
        queue_finished(input);
        fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_INPUT);
        return false;
    }
    if (item->value.mouse.hit.inside_grid) {
        item->value.mouse.event.row = item->value.mouse.hit.row;
        item->value.mouse.event.column =
            item->value.mouse.hit.visual_column;
    }
    if (action == BOXRIG_MOUSE_PRESS) {
        input->pressed_buttons |= changed;
    } else if (action == BOXRIG_MOUSE_RELEASE) {
        input->pressed_buttons &= ~changed;
    }
    item->value.mouse.event.action = action;
    item->value.mouse.event.button = button;
    item->value.mouse.event.pressed_buttons = input->pressed_buttons;
    item->value.mouse.event.modifiers = translate_modifiers(state);
    item->value.mouse.event.click_count = click_count;
    item->value.mouse.changed_button = changed;
    notify_activity(input);
    return true;
}

static GdkModifierType current_event_state(GtkEventController *controller)
{
    return gtk_event_controller_get_current_event_state(controller);
}

static void pointer_pressed(
    GtkGestureClick *gesture,
    int press_count,
    double x,
    double y,
    gpointer user_data)
{
    guint button = gtk_gesture_single_get_current_button(
        GTK_GESTURE_SINGLE(gesture));
    uint8_t count;

    if (button == 0) {
        button = gtk_gesture_single_get_button(GTK_GESTURE_SINGLE(gesture));
    }
    if (button == 0) {
        button = 1;
    }
    count = press_count < 1 ? 1 : press_count > 3 ? 3 : (uint8_t)press_count;
    (void)enqueue_mouse(
        user_data,
        BOXRIG_MOUSE_PRESS,
        translate_button(button),
        count,
        x,
        y,
        current_event_state(GTK_EVENT_CONTROLLER(gesture)));
}

static void pointer_released(
    GtkGestureClick *gesture,
    int press_count,
    double x,
    double y,
    gpointer user_data)
{
    guint button = gtk_gesture_single_get_current_button(
        GTK_GESTURE_SINGLE(gesture));

    (void)press_count;
    if (button == 0) {
        button = gtk_gesture_single_get_button(GTK_GESTURE_SINGLE(gesture));
    }
    if (button == 0) {
        button = 1;
    }
    (void)enqueue_mouse(
        user_data,
        BOXRIG_MOUSE_RELEASE,
        translate_button(button),
        0,
        x,
        y,
        current_event_state(GTK_EVENT_CONTROLLER(gesture)));
}

static void pointer_moved(
    GtkEventControllerMotion *controller,
    double x,
    double y,
    gpointer user_data)
{
    boxrig_gtk4_input *input = user_data;

    input->pointer_x = x;
    input->pointer_y = y;
    input->pointer_position_valid = true;
    (void)enqueue_mouse(
        user_data,
        BOXRIG_MOUSE_MOTION,
        BOXRIG_MOUSE_BUTTON_NONE,
        0,
        x,
        y,
        current_event_state(GTK_EVENT_CONTROLLER(controller)));
}

static void pointer_entered(
    GtkEventControllerMotion *controller,
    double x,
    double y,
    gpointer user_data)
{
    boxrig_gtk4_input *input = user_data;

    (void)controller;
    input->pointer_x = x;
    input->pointer_y = y;
    input->pointer_position_valid = true;
}

static gboolean pointer_scrolled(
    GtkEventControllerScroll *controller,
    double dx,
    double dy,
    gpointer user_data)
{
    boxrig_gtk4_input *input = user_data;

    if (!input->accepting || !input->pointer_position_valid ||
        !isfinite(dx) || !isfinite(dy) || (dx == 0.0 && dy == 0.0)) {
        return FALSE;
    }
    if (input->candidate_buttons != 0 || hits_candidates(input, input->pointer_x, input->pointer_y)) return TRUE;
    /* DISCRETE lets GTK accumulate smooth input before emitting integral
     * deltas. Preserve their magnitude without accumulating a second time. */
    if (trunc(dx) != dx || trunc(dy) != dy) {
        return FALSE;
    }
    if (fabs(dx) + fabs(dy) >
        (double)(input->queue_capacity - input->queue_count)) {
        fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_NATIVE_INPUT_OVERFLOW);
        return FALSE;
    }
    /* GdkScrollEvent has no position. Motion and enter already supply
     * widget-relative coordinates, including the native widget's offset. */
    GdkModifierType state = current_event_state(GTK_EVENT_CONTROLLER(controller));
    const double steps[2] = {dy, dx};
    for (size_t axis = 0; axis < 2; axis++) {
        boxrig_mouse_button button = axis == 0
            ? (steps[axis] < 0 ? BOXRIG_MOUSE_BUTTON_WHEEL_UP : BOXRIG_MOUSE_BUTTON_WHEEL_DOWN)
            : (steps[axis] < 0 ? BOXRIG_MOUSE_BUTTON_WHEEL_LEFT : BOXRIG_MOUSE_BUTTON_WHEEL_RIGHT);
        for (size_t i = 0; i < (size_t)fabs(steps[axis]); i++) {
            if (!enqueue_mouse(input, BOXRIG_MOUSE_PRESS, button, 0,
                    input->pointer_x, input->pointer_y, state)) {
                return FALSE;
            }
        }
    }
    return TRUE;
}

static uint32_t feedback_from_attributes(GSList *attributes)
{
    uint32_t feedback = BOXRIG_COMPOSITION_FEEDBACK_NONE;

    for (GSList *item = attributes; item != NULL; item = item->next) {
        PangoAttribute *attribute = item->data;

        /* GtkIMContext exposes presentation attributes rather than protocol
         * conversion states. Bold emphasis denotes converted text and a
         * background denotes the active conversion target. Ordinary colors
         * and normal weight do not imply a selection. */
        switch (attribute->klass->type) {
        case PANGO_ATTR_UNDERLINE:
            if (((PangoAttrInt *)attribute)->value != PANGO_UNDERLINE_NONE)
                feedback |= BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE;
            break;
        case PANGO_ATTR_BACKGROUND:
            feedback |= BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
                        BOXRIG_COMPOSITION_FEEDBACK_TARGET;
            break;
        case PANGO_ATTR_WEIGHT:
            if (((PangoAttrInt *)attribute)->value > PANGO_WEIGHT_NORMAL)
                feedback |= BOXRIG_COMPOSITION_FEEDBACK_PRIMARY |
                            BOXRIG_COMPOSITION_FEEDBACK_CONVERTED;
            break;
        default:
            break;
        }
    }
    return feedback;
}

typedef struct preedit_attribute_text {
    const char *text;
    size_t length;
} preedit_attribute_text;

static gboolean invalid_preedit_attribute(PangoAttribute *attribute, gpointer data)
{
    const preedit_attribute_text *value = data;
    size_t start = attribute->start_index;
    size_t end = attribute->end_index == G_MAXUINT
        ? value->length : attribute->end_index;

    /* Pango indices are UTF-8 bytes, not grapheme boundaries. Validate before
     * iteration so a reversed or split-scalar range cannot fragment valid
     * decorations. Work on a copy owned by this translation. */
    return start >= end || end > value->length ||
        ((unsigned char)value->text[start] & 0xc0) == 0x80 ||
        ((unsigned char)value->text[end] & 0xc0) == 0x80;
}

void boxrig_gtk4_input_clear_preedit_translation(
    boxrig_gtk4_preedit_translation *translation)
{
    if (translation == NULL) {
        return;
    }
    free(translation->segments);
    memset(translation, 0, sizeof(*translation));
}

bool boxrig_gtk4_input_translate_preedit(
    const char *text,
    PangoAttrList *attributes,
    int cursor,
    boxrig_gtk4_preedit_translation *translation_out)
{
    boxrig_gtk4_preedit_translation candidate = {0};
    boxrig_composition_limits limits = boxrig_composition_limits_default();

    if (translation_out == NULL) return false;
    memset(translation_out, 0, sizeof(*translation_out));
    if (text == NULL || cursor < 0 || !g_utf8_validate(text, -1, NULL)) return false;
    size_t text_length = strlen(text);
    glong character_count = g_utf8_strlen(text, (gssize)text_length);
    if (text_length > limits.maximum_text_bytes || text_length > INT_MAX ||
        character_count < 0 || character_count >= INT_MAX ||
        (glong)cursor > character_count) return false;
    candidate.caret_offset = (size_t)(g_utf8_offset_to_pointer(text, cursor) - text);

    if (attributes != NULL && text_length != 0) {
        candidate.segments = calloc(BOXRIG_GTK4_PREEDIT_SEGMENTS, sizeof(*candidate.segments));
        if (candidate.segments == NULL) return false;
        PangoAttrList *normalized = pango_attr_list_copy(attributes);
        preedit_attribute_text value = {text, text_length};
        PangoAttrList *removed = pango_attr_list_filter(normalized, invalid_preedit_attribute, &value);
        if (removed != NULL) pango_attr_list_unref(removed);
        PangoAttrIterator *iterator = pango_attr_list_get_iterator(normalized);
        bool target_started = false, separate_targets = false;
        size_t target_end = 0;
        do {
            int start, end;
            pango_attr_iterator_range(iterator, &start, &end);
            if ((size_t)start >= text_length) break;
            size_t bounded_end = (size_t)end > text_length ? text_length : (size_t)end;
            GSList *current = pango_attr_iterator_get_attrs(iterator);
            uint32_t feedback = feedback_from_attributes(current);
            g_slist_free_full(current, (GDestroyNotify)pango_attribute_destroy);
            if ((size_t)start >= bounded_end || feedback == BOXRIG_COMPOSITION_FEEDBACK_NONE) continue;

            if (candidate.segment_count == BOXRIG_GTK4_PREEDIT_SEGMENTS) {
                /* Decoration limits must not discard valid text or its caret.
                 * Drop the whole decoration snapshot, never a partial target. */
                candidate.segment_count = 0;
                candidate.selection_offset = candidate.selection_length = 0;
                break;
            }
            boxrig_composition_segment *segment = &candidate.segments[candidate.segment_count++];
            *segment = (boxrig_composition_segment){(size_t)start, bounded_end - (size_t)start, feedback};
            if ((feedback & BOXRIG_COMPOSITION_FEEDBACK_TARGET) != 0) {
                if (!target_started) {
                    candidate.selection_offset = (size_t)start;
                    target_started = true;
                } else if ((size_t)start != target_end) separate_targets = true;
                target_end = bounded_end;
                candidate.selection_length = target_end - candidate.selection_offset;
            }
        } while (pango_attr_iterator_next(iterator));
        pango_attr_iterator_destroy(iterator);
        pango_attr_list_unref(normalized);
        /* The common selection is contiguous. Separate reverse spans remain
         * in segments without selecting the ordinary text between them. */
        if (separate_targets) candidate.selection_offset = candidate.selection_length = 0;
    }
    if (candidate.segment_count == 0) {
        free(candidate.segments);
        candidate.segments = NULL;
    }
    *translation_out = candidate;
    return true;
}

/* Such keys have arrived at the widget but cannot have produced an IM signal
 * yet. A late signal from an earlier filter must be applied before them. */
static bool unfiltered_gtk_key(const input_item *item)
{
    return item->kind == INPUT_ITEM_KEY && item->value.key.native_event != NULL &&
        !item->value.key.fallback_processed;
}

static bool enqueue_composition_event(
    boxrig_gtk4_input *input,
    boxrig_composition_event_type type,
    const char *text,
    size_t text_length,
    boxrig_composition_segment *segments,
    size_t segment_count,
    size_t selection_offset,
    size_t selection_length,
    size_t caret_offset,
    bool caret_visible)
{
    input_item *item;
    char *copy = NULL;

    if (!input->accepting || !input_enabled(input)) {
        free(segments);
        return false;
    }
    if (type == BOXRIG_COMPOSITION_COMMIT &&
        boxrig_ui_view_notify_local_input(input->view) != BOXRIG_UI_OK) {
        free(segments);
        fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_INPUT_METHOD);
        return false;
    }
    if (text_length != 0) {
        copy = malloc(text_length);
        if (copy == NULL) {
            free(segments);
            fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_INPUT_METHOD);
            return false;
        }
        memcpy(copy, text, text_length);
    }
    item = queue_tail(input);
    if (item == NULL) {
        free(segments);
        free(copy);
        return false;
    }
    item->kind = INPUT_ITEM_COMPOSITION;
    item->value.composition.im_epoch = input->im_epoch;
    item->value.composition.editable =
        boxrig_ui_editable_document_capabilities(input->document).context_available;
    item->value.composition.text = copy;
    item->value.composition.segments = segments;
    item->value.composition.event.type = type;
    item->value.composition.event.text = copy;
    item->value.composition.event.text_length = text_length;
    item->value.composition.event.segments = segments;
    item->value.composition.event.segment_count = segment_count;
    item->value.composition.event.selection_offset = selection_offset;
    item->value.composition.event.selection_length = selection_length;
    item->value.composition.event.caret_offset = caret_offset;
    item->value.composition.event.caret_visible = caret_visible;
    size_t destination = input->queue_count - 1;
    if (input->filtering_key) {
        /* Only composition effects belong to the key currently being filtered.
         * IBus hybrid mode can dispatch later native keys in its nested loop;
         * leave those keys behind this transaction instead of promoting them. */
        destination = 1 + input->filter_effects++;
    } else {
        /* Context creation and asynchronous modules can emit after the filter
         * returned. Keep their received effects ahead of unsent native keys,
         * preserving the order of effects already queued. Never move a frame's
         * current item while its callback still holds a pointer to it. */
        for (size_t index = input->processing ? 1 : 0; index < destination; index++) {
            if (unfiltered_gtk_key(&input->items[(input->queue_head + index) % input->queue_capacity])) {
                destination = index;
                break;
            }
        }
    }
    input_item effect = *item;
    for (size_t position = input->queue_count - 1; position > destination; position--)
        input->items[(input->queue_head + position) % input->queue_capacity] =
            input->items[(input->queue_head + position - 1) % input->queue_capacity];
    input->items[(input->queue_head + destination) % input->queue_capacity] = effect;
    notify_activity(input);
    return true;
}

static void composition_committed(
    GtkIMContext *context,
    const char *text,
    gpointer user_data)
{
    boxrig_gtk4_input *input = user_data;
    size_t length;

    if (!current_im_context(input, context)) return;
    if (text == NULL || !g_utf8_validate(text, -1, NULL)) {
        fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_INPUT_METHOD);
        return;
    }
    length = strlen(text);
    (void)enqueue_composition_event(
        input,
        BOXRIG_COMPOSITION_COMMIT,
        text,
        length,
        NULL,
        0,
        0,
        0,
        0,
        false);
}

static void composition_preedit_changed(
    GtkIMContext *context,
    gpointer user_data)
{
    boxrig_gtk4_input *input = user_data;
    boxrig_gtk4_preedit_translation translation;
    PangoAttrList *attributes = NULL;
    char *text = NULL;
    int cursor = 0;
    size_t length;

    if (!current_im_context(input, context)) return;
    memset(&translation, 0, sizeof(translation));

    gtk_im_context_get_preedit_string(
        context,
        &text,
        &attributes,
        &cursor);
    if (!boxrig_gtk4_input_translate_preedit(
            text,
            attributes,
            cursor,
            &translation)) {
        fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_INPUT_METHOD);
        goto cleanup;
    }
    length = strlen(text);
    (void)enqueue_composition_event(
        input,
        BOXRIG_COMPOSITION_PREEDIT_UPDATE,
        text,
        length,
        translation.segments,
        translation.segment_count,
        translation.selection_offset,
        translation.selection_length,
        translation.caret_offset,
        true);
    translation.segments = NULL;

cleanup:
    boxrig_gtk4_input_clear_preedit_translation(&translation);
    if (attributes != NULL) {
        pango_attr_list_unref(attributes);
    }
    g_free(text);
}

static void composition_preedit_ended(
    GtkIMContext *context,
    gpointer user_data)
{
    boxrig_gtk4_input *input = user_data;
    if (!current_im_context(input, context)) return;
    (void)enqueue_composition_event(
        input,
        BOXRIG_COMPOSITION_PREEDIT_COMPLETE,
        NULL,
        0,
        NULL,
        0,
        0,
        0,
        0,
        false);
}

/* Surrounding requests are synchronous. Drain earlier input first; a blocked
 * FIFO cannot acknowledge a deletion against a state it has not applied yet. */
static bool flush_surrounding(boxrig_gtk4_input *input, bool deletion)
{
    if (input->filtering_key) {
        /* Earlier keys have already completed. Apply this key's preceding
         * synchronous signals, never later native keys from a nested loop. */
        while (input->filter_effects != 0 && !input->terminal_blocked) {
            input_item *effect = &input->items[(input->queue_head + 1) % input->queue_capacity];
            bool finished;
            boxrig_ui_status status = process_composition_item(input, &effect->value.composition, &finished);
            if (status != BOXRIG_UI_OK || !finished) return false;
            destroy_input_item(effect);
            for (size_t index = 1; index + 1 < input->queue_count; index++)
                input->items[(input->queue_head + index) % input->queue_capacity] =
                    input->items[(input->queue_head + index + 1) % input->queue_capacity];
            input->queue_count--;
            input->filter_effects--;
            memset(&input->items[(input->queue_head + input->queue_count) % input->queue_capacity],
                0, sizeof(input_item));
        }
        boxrig_composition_view current;
        return !input->terminal_blocked && input->filter_effects == 0 &&
            boxrig_composition_get_view(input->composition, &current) == BOXRIG_COMPOSITION_OK &&
            !current.commit_pending;
    }
    bool needs_flush = deletion && input->queue_count != 0;
    boxrig_composition_view view;
    if (boxrig_composition_get_view(input->composition, &view) != BOXRIG_COMPOSITION_OK)
        return false;
    needs_flush |= view.commit_pending && input->commit_editable;
    for (size_t index = 0; index < input->queue_count && !needs_flush; index++) {
        const input_item *item = &input->items[(input->queue_head + index) % input->queue_capacity];
        needs_flush = item->kind == INPUT_ITEM_COMPOSITION &&
            item->value.composition.im_epoch == input->im_epoch &&
            item->value.composition.editable &&
            item->value.composition.event.type == BOXRIG_COMPOSITION_COMMIT;
    }
    if (!needs_flush) return true;
    if (input->processing) return false;
    /* A synchronous surrounding reply belongs to the signals already received.
     * Filtering a later native key here can recursively emit its own deletion
     * before this one, changing the document underneath the first transaction. */
    size_t count = 0;
    while (count < input->queue_count &&
        !unfiltered_gtk_key(&input->items[(input->queue_head + count) % input->queue_capacity])) count++;
    bool blocked = input->terminal_blocked, pending;
    if (count != 0 &&
        boxrig_gtk4_input_frame(input, count, &blocked, &pending) != BOXRIG_UI_OK) return false;
    return !blocked &&
        boxrig_composition_get_view(input->composition, &view) == BOXRIG_COMPOSITION_OK &&
        !view.commit_pending;
}

static gboolean composition_retrieve_surrounding(GtkIMContext *context, gpointer user_data)
{
    boxrig_gtk4_input *input = user_data;
    boxrig_ui_editable_document_snapshot snapshot;
    if (!current_im_context(input, context) || !flush_surrounding(input, false) ||
        !current_im_context(input, context) ||
        !boxrig_ui_editable_document_get_snapshot(input->document, &snapshot) ||
        snapshot.context_length > INT_MAX ||
        memchr(snapshot.context, '\0', snapshot.context_length) != NULL) return FALSE;
    gtk_im_context_set_surrounding_with_selection(context, snapshot.context, (int)snapshot.context_length,
        (int)(snapshot.cursor - snapshot.context_offset), (int)(snapshot.anchor - snapshot.context_offset));
    return TRUE;
}

/* The cache has validated UTF-8, so each bounded step skips continuation bytes. */
static bool move_characters(const char *text, size_t length, size_t *position, int64_t count)
{
    while (count < 0) {
        if (*position == 0) return false;
        do { --*position; } while (*position != 0 && ((unsigned char)text[*position] & 0xc0) == 0x80);
        count++;
    }
    while (count > 0) {
        if (*position == length) return false;
        do { ++*position; } while (*position < length && ((unsigned char)text[*position] & 0xc0) == 0x80);
        count--;
    }
    return true;
}

static uint64_t position_after_delete(uint64_t position, uint64_t first, uint64_t last)
{
    return position < first ? position : position < last ? first : position - (last - first);
}

static gboolean composition_delete_surrounding(GtkIMContext *context,
    int offset, int character_count, gpointer user_data)
{
    boxrig_gtk4_input *input = user_data;
    boxrig_ui_editable_document_snapshot snapshot;
    if (!current_im_context(input, context)) return FALSE;
    if (character_count < 0 || !flush_surrounding(input, true) ||
        !current_im_context(input, context) ||
        !boxrig_ui_editable_document_get_snapshot(input->document, &snapshot) ||
        memchr(snapshot.context, '\0', snapshot.context_length) != NULL) goto rejected;
    size_t first = (size_t)(snapshot.cursor - snapshot.context_offset);
    if (!move_characters(snapshot.context, snapshot.context_length, &first, offset)) goto rejected;
    size_t last = first;
    if (!move_characters(snapshot.context, snapshot.context_length, &last, character_count)) goto rejected;
    uint64_t start = snapshot.context_offset + first, end = snapshot.context_offset + last;
    boxrig_ui_editable_edit edit = {snapshot.input_generation, {start, last - first}, NULL, 0,
        position_after_delete(snapshot.cursor, start, end), position_after_delete(snapshot.anchor, start, end)};
    if (boxrig_ui_editable_document_apply_edit(input->document, &edit) != BOXRIG_UI_OK) goto rejected;
    input->surrounding_generation = boxrig_ui_editable_document_input_generation(input->document);
    notify_activity(input);
    return TRUE;
rejected:
    reject_surrounding(input);
    return FALSE;
}

static void connect_im_context(boxrig_gtk4_input *input)
{
    gtk_im_context_set_use_preedit(input->im_context, TRUE);
    g_signal_connect(input->im_context, "commit", G_CALLBACK(composition_committed), input);
    g_signal_connect(input->im_context, "preedit-changed", G_CALLBACK(composition_preedit_changed), input);
    g_signal_connect(input->im_context, "preedit-end", G_CALLBACK(composition_preedit_ended), input);
    g_signal_connect(input->im_context, "retrieve-surrounding", G_CALLBACK(composition_retrieve_surrounding), input);
    g_signal_connect(input->im_context, "delete-surrounding", G_CALLBACK(composition_delete_surrounding), input);
}

static bool refresh_im_context(boxrig_gtk4_input *input)
{
    if (!resolve_ime(input)) return false;
    if (input->ime_pending) return true;
    if (input->ibus_switch_pending && boxrig_ime_selected(input->ibus))
        ibus_selection_changed(input, true);
    uint64_t generation = boxrig_ui_editable_document_input_generation(input->document);
    if (!input->accepting || (!input->reset_native_input && !input->im_blocked &&
        generation == input->surrounding_generation))
        return input->accepting;
    if (input->ibus_selected && !input->reset_native_input) {
        input->surrounding_generation = generation;
        input->im_blocked = false;
        boxrig_ime_refresh(input->ibus);
        return input->accepting;
    }
    /* Direct IBus owns native focus. Preserve its local fallback on reset;
     * a multicontext would activate a second IBus client after resumption. */
    GtkIMContext *next = input->ibus_selected ? gtk_im_context_simple_new() : gtk_im_multicontext_new();
    if (GTK_IS_IM_MULTICONTEXT(next) && input->ime_state.reason == BOXRIG_GTK_IME_REASON_WAYLAND_CLAUSES)
        gtk_im_multicontext_set_context_id(GTK_IM_MULTICONTEXT(next), "wayland");
    else if (GTK_IS_IM_MULTICONTEXT(next) && input->ime_state.reason == BOXRIG_GTK_IME_REASON_NON_WAYLAND &&
            (strcmp(input->ime_state.gtk_module, "ibus") == 0 || strcmp(input->ime_state.gtk_module, "fcitx") == 0))
        gtk_im_multicontext_set_context_id(GTK_IM_MULTICONTEXT(next), input->ime_state.gtk_module);
    if (next == NULL) {
        fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_INPUT_METHOD);
        return false;
    }
    /* Disconnect before reset: some input methods commit during reset/focus-out.
     * A fresh public context also restores capabilities disabled by IBus. */
    g_signal_handlers_disconnect_by_data(input->im_context, input);
    gtk_im_context_focus_out(input->im_context);
    gtk_im_context_reset(input->im_context);
    gtk_im_context_set_client_widget(input->im_context, NULL);
    g_object_unref(input->im_context);
    input->im_context = next;
    input->reset_native_input = false;
    input->im_epoch++;
    input->surrounding_generation = generation;
    input->im_blocked = false;
    if (input->commit_editable)
        (void)boxrig_composition_cancel_pending_commit(input->composition);
    boxrig_composition_event cancel = {.type = BOXRIG_COMPOSITION_CANCEL};
    (void)boxrig_composition_apply_event(input->composition, &cancel);
    connect_im_context(input);
    if (input->rooted) gtk_im_context_set_client_widget(next, input->widget);
    if (input->im_focused && input_enabled(input)) gtk_im_context_focus_in(next);
    return true;
}

static void ibus_wake(void *data, bool waiting)
{
    boxrig_gtk4_input *input = data;
    input->ime_waiting = waiting;
    if (!waiting && input->accepting) notify_activity(input);
}

static void ibus_error(void *data, bool resource)
{
    boxrig_gtk4_input *input = data;
    input->ibus_errors++;
    if (resource) fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_INPUT_METHOD);
}

static void ibus_source_failed(void *data)
{
    ibus_error(data, true);
}

static void enqueue_ibus_reply(void *data, void *seat, boxrig_ime_request *request)
{
    (void)seat;
    boxrig_gtk4_input *input = data;
    input_item *item = input->accepting ? queue_tail(input) : NULL;
    if (item == NULL) { boxrig_ime_release_request(request); return; }
    item->kind = INPUT_ITEM_IME_REPLY;
    item->value.ime_reply = request;
    notify_activity(input);
}

static bool ibus_forward_key(void *data, void *seat, uint32_t symbol,
    uint32_t code, uint32_t modifiers, boxrig_ime_effect *effect)
{
    (void)seat; (void)code;
    boxrig_gtk4_input *input = data;
    if (!input_enabled(input)) return false;
    key_item key = {0};
    key.event.key = translate_functional_key(symbol);
    key.event.action = (modifiers & IBUS_RELEASE_MASK) != 0 ? BOXRIG_KEY_RELEASE : BOXRIG_KEY_PRESS;
    key.event.modifiers = translate_modifiers((GdkModifierType)modifiers);
    key.event.codepoint = keyval_codepoint(symbol);
    if (key.event.action == BOXRIG_KEY_PRESS && !copy_key_text(symbol, &key)) return false;
    *effect = (boxrig_ime_effect){.type = BOXRIG_IME_KEY, .value.key = key.event, .text = key.text};
    return true;
}

static boxrig_ui_status apply_ibus_effect(void *data, boxrig_ime_effect *effect,
    bool *blocked, bool *accepted)
{
    boxrig_gtk4_input *input = data;
    *blocked = false;
    *accepted = true;
    if (!input_enabled(input)) return BOXRIG_UI_OK;
    if (effect->type == BOXRIG_IME_CANDIDATES) {
        const boxrig_ime_candidates *source = &effect->value.candidates;
        boxrig_ui_candidate_snapshot snapshot = {
            .count = source->count, .page_size = source->page_size, .cursor = source->cursor,
            .orientation = source->orientation,
            .visible = source->visible, .cursor_visible = source->cursor_visible,
            .auxiliary_visible = source->auxiliary_visible,
            .items = (const char *const *)source->items, .labels = (const char *const *)source->labels,
            .auxiliary = source->auxiliary};
        return boxrig_ui_view_set_candidates(input->view, &snapshot);
    }
    if (effect->type == BOXRIG_IME_KEY) {
        boxrig_status status = boxrig_terminal_send_key(input->terminal, &effect->value.key);
        *blocked = status == BOXRIG_STATUS_WOULD_BLOCK;
        *accepted = !*blocked;
        return status == BOXRIG_STATUS_OK || *blocked ? BOXRIG_UI_OK : BOXRIG_UI_UNAVAILABLE;
    }
    /* The common client has already applied any editable transaction; only
     * a PTY commit reaches the composition sink on this path. */
    input->commit_editable = false;
    boxrig_composition_status status = boxrig_composition_apply_event(input->composition,
        &effect->value.composition);
    *blocked = status == BOXRIG_COMPOSITION_WOULD_BLOCK;
    return status == BOXRIG_COMPOSITION_OK || *blocked ? BOXRIG_UI_OK : BOXRIG_UI_UNAVAILABLE;
}

static void ibus_cursor(void *data)
{
    boxrig_gtk4_input *input = data;
    boxrig_gtk4_input_update_candidate(input, input->candidate_scale);
}

static void focus_ibus(boxrig_gtk4_input *input, bool focused, bool commit)
{
    if (input->ibus_session == NULL) return;
    boxrig_ime_target *target = focused && input->ibus_selected && input_enabled(input)
        ? &input->ibus_target : NULL;
    if (!boxrig_ime_is_focused(input->ibus_session, target)) {
        input->ime_state.candidate_valid = false;
        boxrig_ime_focus(input->ibus_session, target, commit && input_enabled(input), ++input->ibus_generation);
    }
}

static void ibus_selection_changed(void *data, bool selected)
{
    boxrig_gtk4_input *input = data;
    if (selected && !input->ibus_requested) return;
    if (!selected) input->ibus_switch_pending = false;
    if (!input->accepting || selected == input->ibus_selected) return;
    boxrig_composition_view view;
    (void)boxrig_composition_get_view(input->composition, &view);
    if (selected && !input->ime_pending && (view.active || view.commit_pending || input->queue_count != 0)) {
        if (!input->ibus_switch_pending) notify_activity(input);
        input->ibus_switch_pending = true;
        return;
    }
    input->ibus_switch_pending = false;
    focus_ibus(input, false, false);
    g_signal_handlers_disconnect_by_data(input->im_context, input);
    gtk_im_context_focus_out(input->im_context);
    gtk_im_context_reset(input->im_context);
    gtk_im_context_set_client_widget(input->im_context, NULL);
    g_object_unref(input->im_context);
    input->im_context = selected ? gtk_im_context_simple_new() : gtk_im_multicontext_new();
    input->ibus_selected = selected;
    input->ime_state.candidate_valid = false;
    input->im_epoch++;
    input->im_blocked = false;
    input->surrounding_generation = boxrig_ui_editable_document_input_generation(input->document);
    boxrig_composition_event cancel = {.type = BOXRIG_COMPOSITION_CANCEL};
    (void)boxrig_composition_apply_event(input->composition, &cancel);
    connect_im_context(input);
    if (input->rooted) gtk_im_context_set_client_widget(input->im_context, input->widget);
    if (input->im_focused) {
        gtk_im_context_focus_in(input->im_context);
        focus_ibus(input, true, false);
    }
    input->ime_waiting = false;
    notify_activity(input);
}

static void clear_ibus_candidates(void *data)
{
    boxrig_gtk4_input *input = data;
    boxrig_ui_view_clear_candidates(input->view);
}

static bool initialize_ibus(boxrig_gtk4_input *input)
{
    input->ibus_target = (boxrig_ime_target){.view = input->view, .data = input,
        .maximum_events = input->queue_capacity,
        .maximum_bytes = boxrig_composition_limits_default().maximum_text_bytes,
        .wake = ibus_wake, .error = ibus_error, .enqueue = enqueue_ibus_reply,
        .forward = ibus_forward_key, .apply = apply_ibus_effect, .cursor = ibus_cursor,
        .clear_candidates = input->ime_state.requested_policy != BOXRIG_GTK_IME_FCITX5 &&
            input->candidate_policy != BOXRIG_UI_CANDIDATE_SYSTEM ? clear_ibus_candidates : NULL};
    input->ibus = boxrig_ime_create(input->ime_state.requested_policy == BOXRIG_GTK_IME_FCITX5 ?
        BOXRIG_IME_BACKEND_FCITX5 : BOXRIG_IME_BACKEND_IBUS, true, 1, "boxrig-gtk4-wayland", ibus_selection_changed, input);
    if (input->ibus == NULL) return false;
    input->ibus_session = boxrig_ime_add_session(input->ibus, input);
    if (input->ibus_session == NULL) return false;
    input->ibus_source = boxrig_ime_source_new(input->ibus, ibus_source_failed, input);
    if (input->ibus_source == NULL) return false;
    g_source_attach(input->ibus_source, g_main_context_get_thread_default());
    /* create() can select synchronously before the new session exists. */
    focus_ibus(input, input->im_focused, false);
    return true;
}

static void ime_probe_changed(void *data)
{
    boxrig_gtk4_input *input = data;
    if (input->accepting) notify_activity(input);
}

static bool resolve_ime(boxrig_gtk4_input *input)
{
    if (!input->ime_pending || input->ime_probe == NULL) return true;
    bool direct;
    if (!boxrig_gtk_ime_probe_get(input->ime_probe, &input->ime_state, &direct)) return true;
    input->ibus_requested = direct;
    if (direct && input->ibus == NULL) {
        if (!initialize_ibus(input)) {
            fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_INPUT_METHOD);
            return false;
        }
    } else ibus_selection_changed(input, direct && boxrig_ime_selected(input->ibus));
    input->ime_pending = false;
    if (input->im_focused) {
        gtk_im_context_focus_in(input->im_context);
        focus_ibus(input, true, false);
    }
    notify_activity(input);
    return true;
}

static void attach_ime(boxrig_gtk4_input *input)
{
    if (input->ime_probe != NULL) return;
    GdkDisplay *display = gtk_widget_get_display(input->widget);
    if (g_object_get_data(G_OBJECT(display), "boxrig-ime-preedit-style") == NULL) {
        /* GTK's Wayland context converts preedit hint classes to Pango
         * presentation attributes. Older desktop themes lack these rules.
         * Keep the selection marker within our CSS node; the terminal's
         * renderer still chooses its actual reverse-video colors. */
        GtkCssProvider *style = gtk_css_provider_new();
        gtk_css_provider_load_from_string(style,
            "boxrig-terminal preedit { background-color: transparent; }"
            "boxrig-terminal preedit.whole { text-decoration: underline; }"
            "boxrig-terminal preedit.selection { background-color: white; }");
        gtk_style_context_add_provider_for_display(display,
            GTK_STYLE_PROVIDER(style), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_set_data_full(G_OBJECT(display), "boxrig-ime-preedit-style", style, g_object_unref);
    }
    input->ime_pending = true;
    /* A new root can use another display; do not reuse its former delegate. */
    if (input->ibus_selected) ibus_selection_changed(input, false);
    else if ((input->ime_state.reason == BOXRIG_GTK_IME_REASON_WAYLAND_CLAUSES ||
            (input->ime_state.reason == BOXRIG_GTK_IME_REASON_NON_WAYLAND &&
             strcmp(input->ime_state.gtk_module, "ibus") == 0)) &&
            GTK_IS_IM_MULTICONTEXT(input->im_context))
        gtk_im_multicontext_set_context_id(GTK_IM_MULTICONTEXT(input->im_context), NULL);
    gtk_im_context_set_client_widget(input->im_context, input->widget);
    input->ime_probe = boxrig_gtk_ime_probe_new(gtk_widget_get_display(input->widget),
        input->im_context, input->ime_state.requested_policy, input->candidate_policy, ime_probe_changed, input);
    if (input->ime_probe == NULL) fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_INPUT_METHOD);
    else (void)resolve_ime(input);
}

/* The standard or simple context emits synchronously while filtering a key.
 * Keep the original key at the FIFO head until its filter returns; its
 * composition signals are inserted immediately after it by the enqueue path. */
static bool filter_gtk_key(boxrig_gtk4_input *input, key_item *item)
{
    if (!item->fallback_processed) {
        item->fallback_processed = true;
        if (item->native_event != NULL) {
            GtkIMContext *context = g_object_ref(input->im_context);
            uint64_t epoch = input->im_epoch;
            input->filtering_key = true;
            input->filter_effects = 0;
            item->fallback_handled = gtk_im_context_filter_keypress(context, item->native_event);
            input->filtering_key = false;
            input->filter_effects = 0;
            if (epoch != input->im_epoch) item->fallback_handled = true;
            g_object_unref(context);
        }
    }
    return item->fallback_handled;
}

static boxrig_ui_status process_key_item(
    boxrig_gtk4_input *input,
    key_item *item,
    bool allow_terminal,
    bool *finished_out)
{
    physical_key_state *physical = find_physical_key(
        input,
        item->physical_keycode,
        true);
    boxrig_input_key_route route = BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    boxrig_status status;

    *finished_out = false;
    if (physical == NULL) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (item->deferred) {
        item->deferred = false;
        if (item->focus_epoch != input->focus_epoch) {
            release_physical_key_if_idle(physical);
            *finished_out = true;
            return BOXRIG_UI_OK;
        }
        item->ime_generation = input->ibus_generation;
    }
    /* PTY output can move the cursor after this key was queued. Publish its
     * current position before a synchronous GTK filter starts the preedit. */
    if (!item->fallback_processed && item->ime_request == NULL)
        boxrig_gtk4_input_update_candidate(input, input->candidate_scale);
    /* Preserve standard GTK's IME-before-shortcut precedence, but perform its
     * potentially reentrant filter inside our ordered input transaction. */
    if (!input->ibus_selected && item->ime_request == NULL && filter_gtk_key(input, item)) {
        release_physical_key_if_idle(physical);
        *finished_out = true;
        return BOXRIG_UI_OK;
    }
    if (!item->routed) {
        item->send_to_terminal = true;
        if (input->key_router != NULL) {
            bool captured = physical->application_captured;

            if (item->event.action == BOXRIG_KEY_PRESS || captured) {
                route = input->key_router(
                    &item->event,
                    input->key_router_context);
                if (route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL &&
                    route != BOXRIG_INPUT_KEY_ROUTE_APPLICATION) {
                    return BOXRIG_UI_UNAVAILABLE;
                }
            }
            if (item->event.action == BOXRIG_KEY_PRESS &&
                route == BOXRIG_INPUT_KEY_ROUTE_APPLICATION) {
                physical->application_captured = true;
                item->send_to_terminal = false;
            } else if (captured) {
                item->send_to_terminal = false;
                if (item->event.action == BOXRIG_KEY_RELEASE) {
                    physical->application_captured = false;
                }
            }
        }
        item->routed = true;
    }
    if (!item->send_to_terminal || !allow_terminal || item->inhibited) {
        release_physical_key_if_idle(physical);
        *finished_out = true;
        return BOXRIG_UI_OK;
    }
    if (input->ibus_selected || item->ime_request != NULL || item->native_event != NULL) {
        bool blocked = false, handled = false;
        boxrig_ui_status filtered = BOXRIG_UI_OK;
        if (input->ibus_selected || item->ime_request != NULL) filtered = boxrig_ime_filter_key(input->ibus_session, &input->ibus_target,
            item->ime_generation, &(boxrig_ime_key){.symbol = item->symbol,
                .code = item->physical_keycode >= 8 ? item->physical_keycode - 8 : 0,
                .modifiers = item->ime_modifiers & ~IBUS_RELEASE_MASK, .time = item->ime_time,
                .release = (item->ime_modifiers & IBUS_RELEASE_MASK) != 0,
                .repeat = item->event.action == BOXRIG_KEY_REPEAT}, &item->ime_request, &blocked, &handled);
        if (blocked) {
            if (!input->ime_waiting) input->terminal_blocked = true;
            return filtered;
        }
        if (filtered != BOXRIG_UI_OK) return filtered;
        if (handled) {
            release_physical_key_if_idle(physical);
            *finished_out = true;
            return BOXRIG_UI_OK;
        }
        if (filter_gtk_key(input, item)) {
            release_physical_key_if_idle(physical);
            *finished_out = true;
            return BOXRIG_UI_OK;
        }
    }
    status = boxrig_terminal_send_key(input->terminal, &item->event);
    if (status == BOXRIG_STATUS_WOULD_BLOCK) {
        input->terminal_blocked = true;
        return BOXRIG_UI_OK;
    }
    if (status != BOXRIG_STATUS_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    release_physical_key_if_idle(physical);
    *finished_out = true;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status process_focus_item(
    boxrig_gtk4_input *input,
    focus_item *item,
    bool allow_terminal,
    bool *finished_out)
{
    boxrig_status status;

    *finished_out = false;
    if (!item->composition_updated) {
        if (boxrig_composition_focus_changed(
                input->composition,
                item->event.focused) != BOXRIG_COMPOSITION_OK) {
            return BOXRIG_UI_UNAVAILABLE;
        }
        if (!item->event.focused) {
            input->application_buttons = 0;
        }
        item->composition_updated = true;
    }
    if (!allow_terminal) {
        *finished_out = true;
        return BOXRIG_UI_OK;
    }
    status = boxrig_terminal_send_focus(input->terminal, &item->event);
    if (status == BOXRIG_STATUS_WOULD_BLOCK) {
        input->terminal_blocked = true;
        return BOXRIG_UI_OK;
    }
    if (status != BOXRIG_STATUS_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    *finished_out = true;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status process_mouse_item(
    boxrig_gtk4_input *input,
    mouse_item *item,
    bool allow_terminal,
    bool *finished_out)
{
    boxrig_input_mouse_route route = BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    bool captured = item->changed_button != 0
                        ? (input->application_buttons &
                           item->changed_button) != 0
                        : input->application_buttons != 0;
    boxrig_status status;

    *finished_out = false;
    if (!item->routed) {
        item->send_to_terminal = true;
        if (input->mouse_router != NULL) {
            if (item->event.action != BOXRIG_MOUSE_RELEASE || captured) {
                route = input->mouse_router(
                    &item->event,
                    &item->hit,
                    input->mouse_router_context);
                if (route != BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL &&
                    route != BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION) {
                    return BOXRIG_UI_UNAVAILABLE;
                }
            }
            if (item->event.action == BOXRIG_MOUSE_RELEASE && captured) {
                input->application_buttons &= ~item->changed_button;
                item->send_to_terminal = false;
            } else if (captured) {
                item->send_to_terminal = false;
            } else if (route == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION) {
                if (item->event.action == BOXRIG_MOUSE_PRESS) {
                    input->application_buttons |= item->changed_button;
                }
                item->send_to_terminal = false;
            }
        }
        item->routed = true;
    }
    if (!item->send_to_terminal || !allow_terminal) {
        *finished_out = true;
        return BOXRIG_UI_OK;
    }
    status = boxrig_terminal_send_mouse(input->terminal, &item->event);
    if (status == BOXRIG_STATUS_WOULD_BLOCK) {
        input->terminal_blocked = true;
        return BOXRIG_UI_OK;
    }
    if (status != BOXRIG_STATUS_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    *finished_out = true;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status process_composition_item(
    boxrig_gtk4_input *input,
    composition_item *item,
    bool *finished_out)
{
    *finished_out = true;
    if ((item->editable || item->event.type != BOXRIG_COMPOSITION_COMMIT) &&
        (item->im_epoch != input->im_epoch || input->im_blocked)) return BOXRIG_UI_OK;
    input->commit_editable = item->editable;
    input->commit_epoch = item->im_epoch;
    boxrig_composition_status status = boxrig_composition_apply_event(
        input->composition,
        &item->event);

    *finished_out = true;
    if (status == BOXRIG_COMPOSITION_WOULD_BLOCK) {
        input->terminal_blocked = true;
        return BOXRIG_UI_OK;
    }
    return status == BOXRIG_COMPOSITION_OK ? BOXRIG_UI_OK
                                            : BOXRIG_UI_UNAVAILABLE;
}

static boxrig_ui_status process_paste_item(
    boxrig_gtk4_input *input,
    paste_item *item,
    bool *finished_out)
{
    boxrig_paste_event event;
    boxrig_status status;

    *finished_out = false;
    memset(&event, 0, sizeof(event));
    if (item->state == PASTE_ITEM_BEGIN) {
        event.phase = BOXRIG_PASTE_BEGIN;
    } else if (item->state == PASTE_ITEM_CHUNK) {
        size_t remaining = item->length - item->offset;

        if (remaining == 0) {
            item->state = PASTE_ITEM_END;
            return BOXRIG_UI_OK;
        }
        event.phase = BOXRIG_PASTE_CHUNK;
        event.text = item->text + item->offset;
        event.text_length = boxrig_input_text_chunk_length(
                event.text, remaining, input->paste_chunk_size);
    } else {
        event.phase = BOXRIG_PASTE_END;
    }
    status = boxrig_terminal_send_paste(input->terminal, &event);
    if (status == BOXRIG_STATUS_WOULD_BLOCK) {
        input->terminal_blocked = true;
        return BOXRIG_UI_OK;
    }
    if (status != BOXRIG_STATUS_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (item->state == PASTE_ITEM_BEGIN) {
        item->state = PASTE_ITEM_CHUNK;
    } else if (item->state == PASTE_ITEM_CHUNK) {
        item->offset += event.text_length;
    } else {
        *finished_out = true;
    }
    return BOXRIG_UI_OK;
}

static bool composition_commit_pending(boxrig_gtk4_input *input)
{
    boxrig_composition_view view;

    return boxrig_composition_get_view(input->composition, &view) ==
               BOXRIG_COMPOSITION_OK &&
           view.commit_pending;
}

static int clipboard_offer_index(uint32_t target)
{
    if (target == BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION) {
        return 0;
    }
    if (target == BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD) {
        return 1;
    }
    return -1;
}

static void clear_clipboard_offer(
    boxrig_gtk4_input *input,
    clipboard_offer *offer)
{
    if (offer->length <= input->outgoing_clipboard_bytes) {
        input->outgoing_clipboard_bytes -= offer->length;
    } else {
        input->outgoing_clipboard_bytes = 0;
    }
    offer->length = 0;
    g_clear_object(&offer->provider);
    g_clear_pointer(&offer->bytes, g_bytes_unref);
}

static void cancel_clipboard_transfers(
    boxrig_gtk4_input *input,
    GdkClipboard *clipboard)
{
    size_t index;

    if (input == NULL || input->clipboard_transfers == NULL) {
        return;
    }
    for (index = 0; index < input->maximum_clipboard_transfers; index++) {
        clipboard_transfer *transfer = &input->clipboard_transfers[index];

        if (transfer->active &&
            (clipboard == NULL || transfer->clipboard == clipboard)) {
            g_cancellable_cancel(transfer->cancellable);
        }
    }
}

static void clipboard_changed(
    GdkClipboard *clipboard,
    gpointer user_data)
{
    boxrig_gtk4_input *input = user_data;
    size_t index;
    bool offer_changed = false;

    if (input->destroyed) {
        return;
    }
    for (index = 0; index < 2; index++) {
        clipboard_offer *offer = &input->clipboard_offers[index];

        if (offer->clipboard == clipboard && offer->provider != NULL &&
            gdk_clipboard_get_content(clipboard) != offer->provider) {
            clear_clipboard_offer(input, offer);
            offer_changed = true;
        }
    }
    cancel_clipboard_transfers(input, clipboard);
    if (offer_changed) {
        notify_activity(input);
    }
}

static void attach_clipboards(boxrig_gtk4_input *input)
{
    GdkDisplay *display;
    GdkClipboard *clipboards[2];
    size_t index;

    if (input == NULL || input->destroyed || input->rooted ||
        input->widget == NULL) {
        return;
    }
    display = gtk_widget_get_display(input->widget);
    if (display == NULL) {
        return;
    }
    clipboards[0] = gdk_display_get_primary_clipboard(display);
    clipboards[1] = gdk_display_get_clipboard(display);
    for (index = 0; index < 2; index++) {
        clipboard_offer *offer = &input->clipboard_offers[index];

        if (clipboards[index] == NULL) {
            continue;
        }
        offer->clipboard = g_object_ref(clipboards[index]);
        offer->changed_handler = g_signal_connect(
            offer->clipboard,
            "changed",
            G_CALLBACK(clipboard_changed),
            input);
    }
    input->rooted = input->clipboard_offers[0].clipboard != NULL ||
                    input->clipboard_offers[1].clipboard != NULL;
}

static void detach_clipboards(boxrig_gtk4_input *input)
{
    size_t index;

    if (input == NULL) {
        return;
    }
    input->rooted = false;
    cancel_clipboard_transfers(input, NULL);
    for (index = 0; index < 2; index++) {
        clipboard_offer *offer = &input->clipboard_offers[index];

        if (offer->clipboard != NULL && offer->changed_handler != 0) {
            g_signal_handler_disconnect(
                offer->clipboard,
                offer->changed_handler);
            offer->changed_handler = 0;
        }
        if (offer->clipboard != NULL && offer->provider != NULL &&
            gdk_clipboard_get_content(offer->clipboard) == offer->provider) {
            (void)gdk_clipboard_set_content(offer->clipboard, NULL);
        }
        clear_clipboard_offer(input, offer);
        g_clear_object(&offer->clipboard);
    }
}

static void destroy_clipboard_snapshot(gpointer data)
{
    boxrig_text_snapshot_destroy(data);
}

static boxrig_ui_status copy_selection_to_clipboard(
    boxrig_gtk4_input *input,
    boxrig_ui_view *view,
    uint32_t target)
{
    boxrig_text_snapshot *snapshot = NULL;
    clipboard_offer *offer;
    GdkContentProvider *provider;
    GBytes *bytes;
    const uint8_t *data;
    size_t length;
    size_t retained;
    int index = clipboard_offer_index(target);
    boxrig_ui_status status;
    gboolean set;

    if (view == NULL || index < 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    offer = &input->clipboard_offers[index];
    if (offer->clipboard == NULL) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    status = boxrig_ui_view_create_selection_snapshot(view, &snapshot);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    data = boxrig_text_snapshot_data(snapshot, &length);
    retained = input->outgoing_clipboard_bytes - offer->length;
    if (length > input->maximum_clipboard_bytes - retained) {
        boxrig_text_snapshot_destroy(snapshot);
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    bytes = g_bytes_new_with_free_func(
        data,
        length,
        destroy_clipboard_snapshot,
        snapshot);
    if (bytes == NULL) {
        boxrig_text_snapshot_destroy(snapshot);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    provider = gdk_content_provider_new_for_bytes(
        "text/plain;charset=utf-8",
        bytes);
    if (provider == NULL) {
        g_bytes_unref(bytes);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    if (offer->changed_handler != 0) {
        g_signal_handler_block(offer->clipboard, offer->changed_handler);
    }
    set = gdk_clipboard_set_content(offer->clipboard, provider);
    if (offer->changed_handler != 0) {
        g_signal_handler_unblock(offer->clipboard, offer->changed_handler);
    }
    if (!set) {
        g_object_unref(provider);
        g_bytes_unref(bytes);
        return BOXRIG_UI_UNAVAILABLE;
    }
    clear_clipboard_offer(input, offer);
    offer->provider = provider;
    offer->bytes = bytes;
    offer->length = length;
    input->outgoing_clipboard_bytes += length;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status enqueue_owned_paste(
    boxrig_gtk4_input *input,
    char *text,
    size_t length)
{
    if (!input_enabled(input)) return BOXRIG_UI_UNAVAILABLE;
    boxrig_ui_status status = boxrig_ui_view_notify_local_input(input->view);
    if (status != BOXRIG_UI_OK) return status;
    input_item *item = queue_tail(input);

    if (item == NULL) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    item->kind = INPUT_ITEM_PASTE;
    item->value.paste.text = text;
    item->value.paste.length = length;
    item->value.paste.state = PASTE_ITEM_BEGIN;
    notify_activity(input);
    return BOXRIG_UI_OK;
}

static bool normalize_clipboard_text(
    uint8_t *bytes,
    size_t *length_in_out)
{
    size_t source = 0;
    size_t destination = 0;
    size_t length = *length_in_out;

    if (length == 0) {
        return true;
    }
    if (bytes == NULL || length > G_MAXSSIZE ||
        !g_utf8_validate((const char *)bytes, (gssize)length, NULL)) {
        return false;
    }
    while (source < length) {
        const char *current = (const char *)bytes + source;
        const char *next = g_utf8_next_char(current);
        size_t encoded_length = (size_t)(next - current);
        gunichar codepoint = g_utf8_get_char(current);

        if (codepoint == '\r') {
            bytes[destination++] = '\n';
            source += encoded_length;
            if (source < length && bytes[source] == '\n') {
                source++;
            }
            continue;
        }
        if (codepoint == '\n' || codepoint == '\t' ||
            (codepoint >= 0x20 && codepoint != 0x7f &&
             !(codepoint >= 0x80 && codepoint <= 0x9f))) {
            memmove(bytes + destination, bytes + source, encoded_length);
            destination += encoded_length;
        }
        source += encoded_length;
    }
    *length_in_out = destination;
    return true;
}

static void finish_clipboard_transfer(clipboard_transfer *transfer)
{
    boxrig_gtk4_input *input = transfer->input;

    if (!transfer->active) {
        return;
    }
    transfer->active = false;
    if (input->active_clipboard_transfers != 0) {
        input->active_clipboard_transfers--;
    }
    g_clear_object(&transfer->stream);
    g_clear_object(&transfer->cancellable);
    g_clear_object(&transfer->clipboard);
    free(transfer->bytes);
    transfer->bytes = NULL;
    transfer->length = 0;
    transfer->capacity = 0;
    transfer->input = NULL;
    release_input(input);
}

static void fail_clipboard_transfer(clipboard_transfer *transfer)
{
    boxrig_gtk4_input *input = transfer->input;

    if (input->accepting) {
        fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_CLIPBOARD);
    }
    finish_clipboard_transfer(transfer);
}

static void read_clipboard_chunk(
    GObject *source,
    GAsyncResult *result,
    gpointer user_data);

static void begin_clipboard_chunk(clipboard_transfer *transfer)
{
    boxrig_gtk4_input *input = transfer->input;
    size_t limit = input->paste_capacity + 1;

    if (!transfer->active || !input->accepting || !input->mapped ||
        !input->rooted ||
        g_cancellable_is_cancelled(transfer->cancellable)) {
        finish_clipboard_transfer(transfer);
        return;
    }
    if (transfer->capacity == transfer->length) {
        size_t remaining = limit - transfer->capacity;
        size_t growth = remaining < input->paste_chunk_size
                            ? remaining
                            : input->paste_chunk_size;
        size_t capacity = transfer->capacity + growth;
        uint8_t *replacement;

        if (growth == 0) {
            fail_clipboard_transfer(transfer);
            return;
        }
        replacement = realloc(transfer->bytes, capacity);
        if (replacement == NULL) {
            fail_clipboard_transfer(transfer);
            return;
        }
        transfer->bytes = replacement;
        transfer->capacity = capacity;
    }
    g_input_stream_read_async(
        transfer->stream,
        transfer->bytes + transfer->length,
        transfer->capacity - transfer->length,
        G_PRIORITY_DEFAULT,
        transfer->cancellable,
        read_clipboard_chunk,
        transfer);
}

static void read_clipboard_chunk(
    GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
    clipboard_transfer *transfer = user_data;
    boxrig_gtk4_input *input = transfer->input;
    GError *error = NULL;
    gssize length;

    length = g_input_stream_read_finish(
        G_INPUT_STREAM(source),
        result,
        &error);
    if (length < 0) {
        bool cancelled = g_error_matches(
            error,
            G_IO_ERROR,
            G_IO_ERROR_CANCELLED);

        g_clear_error(&error);
        if (cancelled || !input->accepting || !input->mapped ||
            !input->rooted) {
            finish_clipboard_transfer(transfer);
        } else {
            fail_clipboard_transfer(transfer);
        }
        return;
    }
    if (length == 0) {
        char *text;
        boxrig_ui_status status;

        if (!input->accepting || !input->mapped || !input->rooted ||
            g_cancellable_is_cancelled(transfer->cancellable)) {
            finish_clipboard_transfer(transfer);
            return;
        }
        if (!normalize_clipboard_text(
                transfer->bytes,
                &transfer->length)) {
            fail_clipboard_transfer(transfer);
            return;
        }
        text = (char *)transfer->bytes;
        transfer->bytes = NULL;
        status = enqueue_owned_paste(input, text, transfer->length);
        if (status != BOXRIG_UI_OK) {
            free(text);
            if (input->failure_reason == BOXRIG_UI_COMPONENT_FAILURE_NONE) {
                fail_clipboard_transfer(transfer);
                return;
            }
        }
        finish_clipboard_transfer(transfer);
        return;
    }
    transfer->length += (size_t)length;
    if (transfer->length > input->paste_capacity) {
        fail_clipboard_transfer(transfer);
        return;
    }
    begin_clipboard_chunk(transfer);
}

static bool supported_clipboard_mime(const char *mime_type)
{
    return g_strcmp0(mime_type, "text/plain;charset=utf-8") == 0 ||
           g_strcmp0(mime_type, "text/plain") == 0;
}

static void clipboard_stream_ready(
    GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
    clipboard_transfer *transfer = user_data;
    boxrig_gtk4_input *input = transfer->input;
    const char *mime_type = NULL;
    GError *error = NULL;
    GInputStream *stream = gdk_clipboard_read_finish(
        GDK_CLIPBOARD(source),
        result,
        &mime_type,
        &error);

    if (stream == NULL) {
        bool cancelled = g_error_matches(
            error,
            G_IO_ERROR,
            G_IO_ERROR_CANCELLED);

        g_clear_error(&error);
        if (cancelled || !input->accepting || !input->mapped ||
            !input->rooted) {
            finish_clipboard_transfer(transfer);
        } else {
            fail_clipboard_transfer(transfer);
        }
        return;
    }
    if (!supported_clipboard_mime(mime_type) || !input->accepting ||
        !input->mapped || !input->rooted ||
        g_cancellable_is_cancelled(transfer->cancellable)) {
        g_object_unref(stream);
        if (!input->accepting || !input->mapped || !input->rooted ||
            g_cancellable_is_cancelled(transfer->cancellable)) {
            finish_clipboard_transfer(transfer);
        } else {
            fail_clipboard_transfer(transfer);
        }
        return;
    }
    transfer->stream = stream;
    begin_clipboard_chunk(transfer);
}

static boxrig_ui_status begin_clipboard_paste(
    boxrig_gtk4_input *input,
    uint32_t target)
{
    static const char *mime_types[] = {
        "text/plain;charset=utf-8",
        "text/plain",
        NULL};
    clipboard_offer *offer;
    clipboard_transfer *transfer = NULL;
    size_t index;
    int offer_index = clipboard_offer_index(target);

    if (offer_index < 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    offer = &input->clipboard_offers[offer_index];
    if (offer->clipboard == NULL) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (input->active_clipboard_transfers >=
        input->maximum_clipboard_transfers) {
        return BOXRIG_UI_BUSY;
    }
    for (index = 0; index < input->maximum_clipboard_transfers; index++) {
        if (!input->clipboard_transfers[index].active) {
            transfer = &input->clipboard_transfers[index];
            break;
        }
    }
    if (transfer == NULL) {
        return BOXRIG_UI_BUSY;
    }
    memset(transfer, 0, sizeof(*transfer));
    transfer->cancellable = g_cancellable_new();
    if (transfer->cancellable == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    transfer->input = input;
    transfer->clipboard = g_object_ref(offer->clipboard);
    transfer->active = true;
    retain_input(input);
    input->active_clipboard_transfers++;
    gdk_clipboard_read_async(
        transfer->clipboard,
        mime_types,
        G_PRIORITY_DEFAULT,
        transfer->cancellable,
        clipboard_stream_ready,
        transfer);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk4_input_create(
    const boxrig_gtk4_input_config *config,
    boxrig_gtk4_input **input_out)
{
    boxrig_gtk4_input *input;

    if (config == NULL || config->widget == NULL ||
        config->terminal == NULL || config->view == NULL ||
        config->paste_capacity == 0 || config->paste_chunk_size == 0 ||
        config->maximum_events == 0 ||
        config->maximum_clipboard_transfers == 0 ||
        config->maximum_clipboard_bytes == 0 || input_out == NULL ||
        config->ime_policy < BOXRIG_GTK_IME_AUTO || config->ime_policy > BOXRIG_GTK_IME_FCITX5 ||
        config->candidate_policy < BOXRIG_UI_CANDIDATE_AUTO || config->candidate_policy > BOXRIG_UI_CANDIDATE_CLIENT ||
        ((config->activity_callback == NULL) !=
         (config->activity_context == NULL))) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *input_out = NULL;
    input = calloc(1, sizeof(*input));
    if (input == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    input->reference_count = 1;
    input->items = calloc(config->maximum_events, sizeof(*input->items));
    input->clipboard_transfers = calloc(
        config->maximum_clipboard_transfers,
        sizeof(*input->clipboard_transfers));
    if (input->items == NULL || input->clipboard_transfers == NULL ||
        boxrig_composition_create(
            config->terminal,
            boxrig_composition_limits_default(),
            &input->composition) != BOXRIG_COMPOSITION_OK) {
        free(input->items);
        free(input->clipboard_transfers);
        free(input);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    input->widget = config->widget;
    input->ime_state.requested_policy = config->ime_policy;
    input->candidate_policy = config->candidate_policy;
    /* Detached widgets still accept portable paste and normalized input.
     * Discovery starts at root/realize, before native keys can arrive. */
    input->candidate_scale = 1;
    input->terminal = config->terminal;
    input->view = config->view;
    input->queue_capacity = config->maximum_events;
    input->paste_capacity = config->paste_capacity;
    input->paste_chunk_size = config->paste_chunk_size;
    input->maximum_clipboard_transfers =
        config->maximum_clipboard_transfers;
    input->maximum_clipboard_bytes = config->maximum_clipboard_bytes;
    input->activity_callback = config->activity_callback;
    input->activity_context = config->activity_context;
    input->accepting = true;
    boxrig_ui_input_control_initialize(&input->control);
    /* Realizing a background widget must not acquire the active IME context.
     * Native focus events establish input ownership after creation. */
    input->im_focused = false;
    input->document = boxrig_ui_view_editable_document(input->view);
    input->surrounding_generation = boxrig_ui_editable_document_input_generation(input->document);
    input->im_epoch = 1;
    (void)boxrig_composition_set_commit_sink(input->composition, commit_text, input);

    input->im_context = gtk_im_multicontext_new();
    input->key_controller = GTK_EVENT_CONTROLLER_KEY(
        gtk_event_controller_key_new());
    input->focus_controller = GTK_EVENT_CONTROLLER_FOCUS(
        gtk_event_controller_focus_new());
    input->click_gesture = GTK_GESTURE_CLICK(gtk_gesture_click_new());
    input->motion_controller = GTK_EVENT_CONTROLLER_MOTION(
        gtk_event_controller_motion_new());
    input->scroll_controller = GTK_EVENT_CONTROLLER_SCROLL(
        gtk_event_controller_scroll_new(
            GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES |
            GTK_EVENT_CONTROLLER_SCROLL_DISCRETE));
    if (input->im_context == NULL || input->key_controller == NULL ||
        input->focus_controller == NULL || input->click_gesture == NULL ||
        input->motion_controller == NULL || input->scroll_controller == NULL) {
        boxrig_gtk4_input_destroy(input);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    /* Until the actual root display is known, neither IME owns a key. */
    /* Filtering belongs to the FIFO, not the controller's pre-signal path:
     * an IBus hybrid wait may dispatch the next native key reentrantly. */
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(input->click_gesture), 0);
    g_signal_connect(
        input->key_controller,
        "key-pressed",
        G_CALLBACK(key_pressed),
        input);
    g_signal_connect(
        input->key_controller,
        "key-released",
        G_CALLBACK(key_released),
        input);
    g_signal_connect(
        input->focus_controller,
        "enter",
        G_CALLBACK(focus_entered),
        input);
    g_signal_connect(
        input->focus_controller,
        "leave",
        G_CALLBACK(focus_left),
        input);
    g_signal_connect(
        input->click_gesture,
        "pressed",
        G_CALLBACK(pointer_pressed),
        input);
    g_signal_connect(
        input->click_gesture,
        "released",
        G_CALLBACK(pointer_released),
        input);
    g_signal_connect(
        input->motion_controller,
        "enter",
        G_CALLBACK(pointer_entered),
        input);
    g_signal_connect(
        input->motion_controller,
        "motion",
        G_CALLBACK(pointer_moved),
        input);
    g_signal_connect(
        input->scroll_controller,
        "scroll",
        G_CALLBACK(pointer_scrolled),
        input);
    connect_im_context(input);
    gtk_widget_add_controller(
        config->widget,
        GTK_EVENT_CONTROLLER(input->key_controller));
    gtk_widget_add_controller(
        config->widget,
        GTK_EVENT_CONTROLLER(input->focus_controller));
    gtk_widget_add_controller(
        config->widget,
        GTK_EVENT_CONTROLLER(input->click_gesture));
    gtk_widget_add_controller(
        config->widget,
        GTK_EVENT_CONTROLLER(input->motion_controller));
    gtk_widget_add_controller(
        config->widget,
        GTK_EVENT_CONTROLLER(input->scroll_controller));
    *input_out = input;
    return BOXRIG_UI_OK;
}

void boxrig_gtk4_input_quiesce(boxrig_gtk4_input *input)
{
    if (input == NULL || input->destroyed) {
        return;
    }
    focus_ibus(input, false, false);
    (void)boxrig_ui_view_set_focused(input->view, false);
    if (input->accepting) {
        input->accepting = false;
        input->focus_epoch++;
        input->im_focused = false;
        (void)boxrig_ui_editable_document_invalidate_input(input->document);
        if (input->im_context != NULL) {
            gtk_im_context_focus_out(input->im_context);
            gtk_im_context_reset(input->im_context);
        }
        if (input->composition != NULL) {
            (void)boxrig_composition_terminate(input->composition);
        }
    }
    cancel_clipboard_transfers(input, NULL);
}

void boxrig_gtk4_input_root(boxrig_gtk4_input *input)
{
    if (input != NULL && input->accepting) {
        attach_clipboards(input);
        attach_ime(input);
    }
}

void boxrig_gtk4_input_unroot(boxrig_gtk4_input *input)
{
    if (input != NULL && input->im_context != NULL) {
        boxrig_gtk_ime_probe_destroy(input->ime_probe);
        input->ime_probe = NULL;
        input->ime_pending = true;
        input->ibus_requested = false;
        input->ibus_switch_pending = false;
        focus_ibus(input, false, false);
        input->focus_epoch++;
        input->im_focused = false;
        input->im_blocked = true;
        (void)boxrig_ui_editable_document_invalidate_input(input->document);
        input->mapped = false;
        (void)boxrig_ui_view_set_focused(input->view, false);
        gtk_im_context_focus_out(input->im_context);
        gtk_im_context_reset(input->im_context);
        gtk_im_context_set_client_widget(input->im_context, NULL);
        detach_clipboards(input);
    }
}

void boxrig_gtk4_input_map(boxrig_gtk4_input *input)
{
    if (input != NULL && input->accepting && input->rooted) {
        input->mapped = true;
        /* A reparented focused widget can keep the controller's focus state
         * without a second enter signal. Resume the new input destination. */
        if (!input->im_focused && gtk_event_controller_focus_contains_focus(input->focus_controller))
            focus_entered(input->focus_controller, input);
    }
}

void boxrig_gtk4_input_unmap(boxrig_gtk4_input *input)
{
    if (input != NULL) {
        focus_ibus(input, false, false);
        input->focus_epoch++;
        input->im_focused = false;
        input->im_blocked = true;
        (void)boxrig_ui_editable_document_invalidate_input(input->document);
        input->mapped = false;
        cancel_clipboard_transfers(input, NULL);
    }
}

void boxrig_gtk4_input_destroy(boxrig_gtk4_input *input)
{
    size_t index;

    if (input == NULL || input->destroyed) {
        return;
    }
    boxrig_gtk4_input_quiesce(input);
    boxrig_gtk4_input_unroot(input);
    if (input->im_context != NULL) {
        g_signal_handlers_disconnect_by_data(input->im_context, input);
    }
    if (input->widget != NULL) {
        if (input->key_controller != NULL) {
            gtk_widget_remove_controller(
                input->widget,
                GTK_EVENT_CONTROLLER(input->key_controller));
        }
        if (input->focus_controller != NULL) {
            gtk_widget_remove_controller(
                input->widget,
                GTK_EVENT_CONTROLLER(input->focus_controller));
        }
        if (input->click_gesture != NULL) {
            gtk_widget_remove_controller(
                input->widget,
                GTK_EVENT_CONTROLLER(input->click_gesture));
        }
        if (input->motion_controller != NULL) {
            gtk_widget_remove_controller(
                input->widget,
                GTK_EVENT_CONTROLLER(input->motion_controller));
        }
        if (input->scroll_controller != NULL) {
            gtk_widget_remove_controller(
                input->widget,
                GTK_EVENT_CONTROLLER(input->scroll_controller));
        }
    }
    for (index = 0; index < input->queue_count; index++) {
        size_t item_index =
            (input->queue_head + index) % input->queue_capacity;

        destroy_input_item(&input->items[item_index]);
    }
    if (input->ibus_source != NULL) {
        g_source_destroy(input->ibus_source);
        g_source_unref(input->ibus_source);
    }
    boxrig_ime_remove_session(input->ibus_session);
    boxrig_ime_destroy(input->ibus);
    input->ibus = NULL;
    if (input->composition != NULL) {
        (void)boxrig_composition_terminate(input->composition);
    }
    boxrig_composition_destroy(input->composition);
    input->composition = NULL;
    g_clear_object(&input->im_context);
    free(input->items);
    input->items = NULL;
    input->widget = NULL;
    input->terminal = NULL;
    input->view = NULL;
    input->activity_callback = NULL;
    input->activity_context = NULL;
    input->destroyed = true;
    release_input(input);
}

boxrig_ui_status boxrig_gtk4_input_set_enabled(boxrig_gtk4_input *input, bool enabled)
{
    if (input == NULL || !input->accepting) return BOXRIG_UI_INVALID_ARGUMENT;
    if (input->processing || input->filtering_key) return BOXRIG_UI_BUSY;
    if (enabled && input->control.state.phase == BOXRIG_UI_INPUT_STOPPING) return BOXRIG_UI_BUSY;
    bool was_enabled = input_enabled(input);
    bool started = false;
    for (size_t i = 0; i < input->queue_count; i++) {
        input_item *item = &input->items[(input->queue_head + i) % input->queue_capacity];
        if (item->kind == INPUT_ITEM_PASTE && item->value.paste.state != PASTE_ITEM_BEGIN)
            started = true;
    }
    boxrig_ui_status status = boxrig_ui_input_control_request(&input->control, enabled, started);
    if (status != BOXRIG_UI_OK && status != BOXRIG_UI_BUSY) return status;
    if (was_enabled != input_enabled(input)) {
        input->focus_epoch++;
        input->im_epoch++;
        input->im_blocked = true;
        input->reset_native_input = true;
        (void)boxrig_ui_editable_document_invalidate_input(input->document);
        if (!enabled) {
            focus_ibus(input, false, false);
            input->ime_waiting = false;
            cancel_clipboard_transfers(input, NULL);
            (void)boxrig_composition_cancel_pending_commit(input->composition);
            boxrig_composition_event cancel = {.type = BOXRIG_COMPOSITION_CANCEL};
            (void)boxrig_composition_apply_event(input->composition, &cancel);
            (void)boxrig_ui_view_set_composition_active(input->view, false);
            for (size_t i = 0; i < BOXRIG_GTK4_PHYSICAL_KEY_STATES; i++)
                if (input->physical_keys[i].native_pressed) input->physical_keys[i].inhibited = true;
            /* Keep local pointer/focus routing usable, including a selection drag.
             * Their old generation suppresses any terminal-directed report. */
            size_t count = input->queue_count;
            for (size_t i = 0; i < count; i++) {
                input_item item = input->items[input->queue_head];
                bool keep = item.kind == INPUT_ITEM_MOUSE || item.kind == INPUT_ITEM_FOCUS;
                queue_finished(input);
                if (keep) *queue_tail(input) = item;
            }
        }
        if (!refresh_im_context(input)) return BOXRIG_UI_UNAVAILABLE;
        if (enabled && input->im_focused) {
            focus_ibus(input, true, false);
            if (!input->ime_pending) gtk_im_context_focus_in(input->im_context);
        }
    }
    status = boxrig_ui_input_control_service(&input->control, input->terminal);
    notify_activity(input);
    return status;
}

boxrig_ui_status boxrig_gtk4_input_get_state(
    const boxrig_gtk4_input *input, boxrig_ui_input_state *state_out)
{
    if (input == NULL || state_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *state_out = input->control.state;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk4_input_set_config(
    boxrig_gtk4_input *input, const boxrig_input_config *config)
{
    if (input == NULL || !input->accepting || config == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    boxrig_composition_view composition;
    if (boxrig_composition_get_view(input->composition, &composition) != BOXRIG_COMPOSITION_OK) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (input->control.state.phase == BOXRIG_UI_INPUT_STOPPING || input->processing || input->queue_count != 0u || input->ime_pending ||
        input->ime_waiting || input->ibus_switch_pending || input->filtering_key ||
        input->active_clipboard_transfers != 0u || composition.active || composition.commit_pending) {
        return BOXRIG_UI_BUSY;
    }
    for (size_t i = 0u; i < BOXRIG_GTK4_PHYSICAL_KEY_STATES; i++) {
        if (input->physical_keys[i].native_pressed || input->physical_keys[i].application_captured) {
            return BOXRIG_UI_BUSY;
        }
    }
    boxrig_status status = boxrig_terminal_set_input_config(input->terminal, config);
    if (status == BOXRIG_STATUS_OK) return BOXRIG_UI_OK;
    if (status == BOXRIG_STATUS_OUT_OF_MEMORY) return BOXRIG_UI_OUT_OF_MEMORY;
    if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) return BOXRIG_UI_LIMIT_EXCEEDED;
    return BOXRIG_UI_INVALID_ARGUMENT;
}

boxrig_ui_status boxrig_gtk4_input_set_key_router(
    boxrig_gtk4_input *input,
    boxrig_input_key_router router,
    void *context)
{
    size_t index;

    if (input == NULL || !input->accepting ||
        (router == NULL && context != NULL)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    for (index = 0; index < BOXRIG_GTK4_PHYSICAL_KEY_STATES; index++) {
        if (input->physical_keys[index].native_pressed ||
            input->physical_keys[index].application_captured) {
            return BOXRIG_UI_BUSY;
        }
    }
    input->key_router = router;
    input->key_router_context = context;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk4_input_set_mouse_router(
    boxrig_gtk4_input *input,
    boxrig_input_mouse_router router,
    void *context)
{
    size_t index;

    if (input == NULL || !input->accepting ||
        (router == NULL && context != NULL)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (input->pressed_buttons != 0 || input->application_buttons != 0) {
        return BOXRIG_UI_BUSY;
    }
    for (index = 0; index < input->queue_count; index++) {
        size_t item_index =
            (input->queue_head + index) % input->queue_capacity;
        input_item *item = &input->items[item_index];

        if (item->kind == INPUT_ITEM_MOUSE &&
            item->value.mouse.changed_button != 0) {
            return BOXRIG_UI_BUSY;
        }
    }
    input->mouse_router = router;
    input->mouse_router_context = context;
    return BOXRIG_UI_OK;
}

boxrig_ui_view_action_capabilities
boxrig_gtk4_input_view_action_capabilities(const boxrig_gtk4_input *input)
{
    boxrig_ui_view_action_capabilities capabilities;

    memset(&capabilities, 0, sizeof(capabilities));
    if (input != NULL && input->accepting && input->rooted) {
        if (input->clipboard_offers[0].clipboard != NULL) {
            capabilities.copy_targets |=
                BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
            capabilities.paste_targets |=
                BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
        }
        if (input->clipboard_offers[1].clipboard != NULL) {
            capabilities.copy_targets |=
                BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
            capabilities.paste_targets |=
                BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
        }
    }
    if (input != NULL && !input_enabled(input)) {
        capabilities.paste_targets = 0;
        capabilities.reconversion = false;
    }
    return capabilities;
}

boxrig_ui_status boxrig_gtk4_input_apply_view_action(
    boxrig_gtk4_input *input,
    boxrig_ui_view *view,
    const boxrig_ui_view_action *action)
{
    boxrig_ui_view_action_capabilities capabilities;

    if (input == NULL || action == NULL ||
        (action->kind != BOXRIG_UI_VIEW_ACTION_COPY &&
         action->kind != BOXRIG_UI_VIEW_ACTION_PASTE &&
         action->kind != BOXRIG_UI_VIEW_ACTION_RECONVERT) ||
        (action->kind == BOXRIG_UI_VIEW_ACTION_RECONVERT
             ? action->clipboard_target != 0
             : clipboard_offer_index(action->clipboard_target) < 0)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    capabilities = boxrig_gtk4_input_view_action_capabilities(input);
    if (!boxrig_ui_view_action_supported(&capabilities, action) ||
        !input->mapped) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (action->kind == BOXRIG_UI_VIEW_ACTION_COPY) {
        return copy_selection_to_clipboard(
            input,
            view,
            action->clipboard_target);
    }
    return begin_clipboard_paste(input, action->clipboard_target);
}

boxrig_ui_status boxrig_gtk4_input_queue_paste(
    boxrig_gtk4_input *input,
    const uint8_t *bytes,
    size_t length)
{
    if (input != NULL && !input_enabled(input)) return BOXRIG_UI_UNAVAILABLE;
    char *normalized = NULL;
    size_t normalized_length = 0;
    boxrig_ui_status status;

    if (input == NULL || !input->accepting ||
        boxrig_input_text_normalize_utf8(
            bytes,
            length,
            input->paste_capacity,
            &normalized,
            &normalized_length) == -1) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = enqueue_owned_paste(input, normalized, normalized_length);
    if (status != BOXRIG_UI_OK) {
        free(normalized);
    }
    return status;
}

static boxrig_ui_status process_input_frame(
    boxrig_gtk4_input *input,
    size_t maximum_items,
    bool *input_blocked_out,
    bool *additional_dispatch_out)
{
    boxrig_composition_status composition_status;
    size_t work = 0;
    bool discovery_blocked = false;

    if (input == NULL || maximum_items == 0 || input_blocked_out == NULL ||
        additional_dispatch_out == NULL ||
        input->failure_reason != BOXRIG_UI_COMPONENT_FAILURE_NONE) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    boxrig_ui_status stop_status = boxrig_ui_input_control_service(&input->control, input->terminal);
    if (stop_status != BOXRIG_UI_OK && stop_status != BOXRIG_UI_BUSY) return stop_status;
    input->terminal_blocked = false;
    composition_status = boxrig_composition_retry_commit(input->composition);
    if (composition_status == BOXRIG_COMPOSITION_WOULD_BLOCK) {
        input->terminal_blocked = true;
    } else if (composition_status != BOXRIG_COMPOSITION_OK) {
        fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_INPUT_METHOD);
        return BOXRIG_UI_UNAVAILABLE;
    }
    while (!input->terminal_blocked && !input->ime_waiting && input->queue_count != 0 &&
           work < maximum_items) {
        input_item *item = &input->items[input->queue_head];
        boxrig_ui_status status;
        bool finished = false;
        bool allow_terminal = boxrig_ui_input_control_accepts(&input->control, item->generation);
        if (!allow_terminal && item->kind != INPUT_ITEM_KEY &&
            item->kind != INPUT_ITEM_MOUSE && item->kind != INPUT_ITEM_FOCUS) {
            queue_finished(input);
            work++;
            continue;
        }

        if (allow_terminal && input->ime_pending && item->kind == INPUT_ITEM_KEY) {
            discovery_blocked = true;
            break;
        }

        if (item->kind == INPUT_ITEM_KEY) {
            status = process_key_item(input, &item->value.key, allow_terminal, &finished);
        } else if (item->kind == INPUT_ITEM_IME_REPLY) {
            bool blocked;
            status = boxrig_ime_apply_reply(&input->ibus_target, item->value.ime_reply, &blocked, &finished);
            if (blocked && !input->ime_waiting) input->terminal_blocked = true;
        } else if (item->kind == INPUT_ITEM_FOCUS) {
            status = process_focus_item(input, &item->value.focus, allow_terminal, &finished);
        } else if (item->kind == INPUT_ITEM_MOUSE) {
            status = process_mouse_item(input, &item->value.mouse, allow_terminal, &finished);
        } else if (item->kind == INPUT_ITEM_COMPOSITION) {
            status = process_composition_item(
                input,
                &item->value.composition,
                &finished);
        } else {
            status = process_paste_item(
                input,
                &item->value.paste,
                &finished);
        }
        work++;
        if (status != BOXRIG_UI_OK) {
            fail_input(
                input,
                item->kind == INPUT_ITEM_COMPOSITION
                    ? BOXRIG_UI_COMPONENT_FAILURE_INPUT_METHOD
                    : BOXRIG_UI_COMPONENT_FAILURE_INPUT);
            return status;
        }
        if (finished) {
            queue_finished(input);
        }
    }
    boxrig_composition_view composition;
    if (boxrig_composition_get_view(input->composition, &composition) !=
            BOXRIG_COMPOSITION_OK ||
        boxrig_ui_view_set_composition_active(input->view,
            composition.active && composition.focused) !=
            BOXRIG_UI_OK) {
        fail_input(input, BOXRIG_UI_COMPONENT_FAILURE_INPUT_METHOD);
        return BOXRIG_UI_UNAVAILABLE;
    }
    *input_blocked_out = input->terminal_blocked || stop_status == BOXRIG_UI_BUSY;
    *additional_dispatch_out =
        (!discovery_blocked && !input->ime_waiting && input->queue_count != 0) || composition_commit_pending(input);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk4_input_frame(
    boxrig_gtk4_input *input, size_t maximum_items,
    bool *input_blocked_out, bool *additional_dispatch_out)
{
    if (input == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (input->processing) return BOXRIG_UI_BUSY;
    input->processing = true;
    boxrig_ime_refresh(input->ibus);
    if (!refresh_im_context(input)) {
        input->processing = false;
        return BOXRIG_UI_UNAVAILABLE;
    }
    boxrig_ui_status status = process_input_frame(input, maximum_items,
        input_blocked_out, additional_dispatch_out);
    if (input->ibus_switch_pending && boxrig_ime_selected(input->ibus))
        ibus_selection_changed(input, true);
    input->processing = false;
    return status;
}

boxrig_composition *boxrig_gtk4_input_composition(boxrig_gtk4_input *input)
{
    return input == NULL ? NULL : input->composition;
}

void boxrig_gtk4_input_update_candidate(
    boxrig_gtk4_input *input,
    unsigned int output_scale)
{
    GdkRectangle rectangle;

    if (input == NULL || input->im_context == NULL ||
        !boxrig_gtk_ime_cursor_rectangle(input->view, input->terminal,
            input->composition, output_scale, input->im_focused, &rectangle)) {
        return;
    }
    input->candidate_scale = output_scale;
    if (input->ibus_selected) {
        GtkNative *native = gtk_widget_get_native(input->widget);
        graphene_point_t point;
        if (native == NULL || !gtk_widget_compute_point(input->widget, GTK_WIDGET(native),
                &GRAPHENE_POINT_INIT(rectangle.x, rectangle.y), &point)) return;
        double tx, ty;
        gtk_native_get_surface_transform(native, &tx, &ty);
        /* GtkNative's content origin is added for the surface-local IM
         * rectangle, as in GTK's standard Wayland input context. */
        double x = point.x + tx, y = point.y + ty;
        if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX) return;
        int32_t relative[4] = {(int32_t)x, (int32_t)y, rectangle.width, rectangle.height};
        boxrig_ime_cursor(input->ibus, &input->ibus_target, relative, true);
        boxrig_ime_diagnostic diagnostic;
        boxrig_ime_get_diagnostic(input->ibus, &diagnostic);
        if (diagnostic.contexts != 0 && boxrig_ime_is_focused(input->ibus_session, &input->ibus_target)) {
            input->ime_state.candidate_valid = true;
            input->ime_state.candidate_surface_relative = true;
            memcpy(input->ime_state.candidate, relative, sizeof(relative));
        }
    } else gtk_im_context_set_cursor_location(input->im_context, &rectangle);
}

void boxrig_gtk4_input_get_diagnostic(
    const boxrig_gtk4_input *input,
    boxrig_gtk4_input_diagnostic *diagnostic_out)
{
    if (diagnostic_out == NULL) {
        return;
    }
    memset(diagnostic_out, 0, sizeof(*diagnostic_out));
    if (input == NULL) {
        return;
    }
    diagnostic_out->failure_reason = input->failure_reason;
    diagnostic_out->queue_depth = input->queue_count;
    diagnostic_out->queue_high_water = input->queue_high_water;
    diagnostic_out->queue_capacity = input->queue_capacity;
    diagnostic_out->active_clipboard_transfers =
        input->active_clipboard_transfers;
    diagnostic_out->outgoing_clipboard_bytes = input->outgoing_clipboard_bytes;
    diagnostic_out->terminal_blocked = input->terminal_blocked;
}

void boxrig_gtk4_input_get_ime_state(const boxrig_gtk4_input *input, boxrig_gtk_ime_state *state)
{
    *state = input->ime_state;
    boxrig_ime_diagnostic diagnostic;
    boxrig_ime_get_diagnostic(input->ibus, &diagnostic);
    state->gtk_major = gtk_get_major_version();
    state->gtk_minor = gtk_get_minor_version();
    state->gtk_micro = gtk_get_micro_version();
    state->active_route = input->ime_probe == NULL || input->ime_pending ? BOXRIG_GTK_IME_ROUTE_PENDING :
        input->ibus_selected ? (diagnostic.backend == BOXRIG_IME_BACKEND_FCITX5 ?
            BOXRIG_GTK_IME_ROUTE_FCITX5 : BOXRIG_GTK_IME_ROUTE_IBUS) : BOXRIG_GTK_IME_ROUTE_GTK;
    state->ibus_state = (boxrig_gtk_ibus_state)diagnostic.state;
    state->ibus_pending = diagnostic.pending;
    state->ibus_contexts = diagnostic.contexts;
    state->ibus_errors = diagnostic.errors + input->ibus_errors;
    state->candidate_policy = input->candidate_policy;
    state->candidate_owner = state->active_route == BOXRIG_GTK_IME_ROUTE_PENDING
        ? BOXRIG_UI_CANDIDATE_OWNER_NONE : input->ibus_selected &&
          input->ibus_target.clear_candidates != NULL ? BOXRIG_UI_CANDIDATE_OWNER_CLIENT :
          BOXRIG_UI_CANDIDATE_OWNER_SYSTEM;
    state->candidate_reason = state->active_route == BOXRIG_GTK_IME_ROUTE_PENDING ? BOXRIG_UI_CANDIDATE_PENDING :
        input->candidate_policy == BOXRIG_UI_CANDIDATE_SYSTEM ? BOXRIG_UI_CANDIDATE_SYSTEM_REQUEST :
        input->ibus_requested && diagnostic.state == BOXRIG_IME_UNAVAILABLE ? BOXRIG_UI_CANDIDATE_IBUS_UNAVAILABLE :
        !input->ibus_selected ? BOXRIG_UI_CANDIDATE_STANDARD_INPUT :
        BOXRIG_UI_CANDIDATE_READY;
    boxrig_ui_candidate_view candidates;
    (void)boxrig_ui_view_get_candidates(input->view, &candidates);
    state->candidate_revision = candidates.revision;
    state->candidate_count = candidates.snapshot.count;
    state->candidate_cursor = candidates.snapshot.cursor;
    state->candidate_visible = candidates.snapshot.visible;
    state->auxiliary_visible = candidates.snapshot.auxiliary_visible;

    if (input->ibus_requested && diagnostic.state == BOXRIG_IME_UNAVAILABLE)
        state->reason = diagnostic.backend == BOXRIG_IME_BACKEND_FCITX5 ?
            BOXRIG_GTK_IME_REASON_FCITX5_UNAVAILABLE : BOXRIG_GTK_IME_REASON_IBUS_UNAVAILABLE;
    if (input->ime_probe != NULL && !input->ime_pending && !input->ibus_selected &&
        GTK_IS_IM_MULTICONTEXT(input->im_context)) {
        const char *module = gtk_im_multicontext_get_context_id(GTK_IM_MULTICONTEXT(input->im_context));
        g_strlcpy(state->gtk_module, module == NULL ? "" : module, sizeof(state->gtk_module));
    }
}
