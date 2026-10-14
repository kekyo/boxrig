/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/win32/input.h"

#include "boxrig/ui/input_text.h"
#include "input_private.h"
#include "../ui/editable_private.h"

#include <imm.h>
#include <windowsx.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    KEY_SLOT_COUNT = 512,
    TO_UNICODE_DO_NOT_CHANGE_STATE = 1u << 2
};

typedef enum pending_input_kind {
    PENDING_INPUT_NONE = 0,
    PENDING_INPUT_KEY,
    PENDING_INPUT_FOCUS,
    PENDING_INPUT_MOUSE
} pending_input_kind;

typedef struct key_identity {
    boxrig_key key;
    uint32_t codepoint;
    uint32_t shifted_codepoint;
    uint32_t base_layout_codepoint;
} key_identity;

typedef struct pending_input {
    pending_input_kind kind;
    boxrig_key_event key;
    boxrig_focus_event focus;
    boxrig_mouse_event mouse;
    char *key_text;
} pending_input;

typedef struct pending_character {
    bool active;
    bool application;
    size_t slot;
    key_identity identity;
    boxrig_key_action action;
    uint32_t modifiers;
} pending_character;

typedef enum paste_state {
    PASTE_STATE_BEGIN = 0,
    PASTE_STATE_CHUNK,
    PASTE_STATE_END
} paste_state;

typedef struct decoded_utf16_text {
    char *text;
    size_t text_length;
    size_t *unit_offsets;
    size_t unit_count;
} decoded_utf16_text;

typedef struct deferred_imm_preedit {
    WCHAR *text;
    size_t text_length;
    BYTE *attributes;
    size_t attribute_count;
    DWORD *clauses;
    size_t clause_count;
    DWORD caret;
    bool active;
} deferred_imm_preedit;

struct boxrig_win32_input_adapter {
    HWND window;
    boxrig_terminal *terminal;
    bool dispatching;
    boxrig_ui_view *animation_view;
    boxrig_ui_status animation_failure;
    unsigned int cell_width;
    unsigned int cell_height;
    int viewport_x;
    int viewport_y;
    size_t paste_capacity;
    size_t paste_chunk_size;
    uint32_t modifiers;
    bool pressed[KEY_SLOT_COUNT];
    bool application_key[KEY_SLOT_COUNT];
    key_identity identities[KEY_SLOT_COUNT];
    boxrig_input_key_router key_router;
    void *key_router_context;
    boxrig_ui_view *mouse_view;
    boxrig_input_mouse_router mouse_router;
    void *mouse_router_context;
    uint32_t application_buttons;
    DWORD last_click_time;
    boxrig_mouse_button last_click_button;
    int last_click_x;
    int last_click_y;
    uint8_t last_click_count;
    bool last_click_valid;
    /* TranslateMessage separates physical identity from UTF-16 text. */
    pending_character character;
    wchar_t high_surrogate;
    pending_input pending;
    bool paste_active;
    paste_state paste_state;
    char *paste_text;
    size_t paste_length;
    size_t paste_offset;
    boxrig_composition *composition;
    boxrig_ui_editable_document *editable_document;
    WCHAR *reconversion_characters;
    size_t reconversion_character_count;
    size_t reconversion_character_capacity;
    bool reconversion_sink_active;
    WCHAR ime_high_surrogate;
    deferred_imm_preedit deferred_preedit;
    bool native_composing;
    bool composition_updated_since_key_release;
    HIMC placement_input_context;
    COMPOSITIONFORM synchronized_composition_form;
    CANDIDATEFORM synchronized_candidate_form;
    bool synchronized_composition_form_valid;
    bool synchronized_candidate_form_valid;
    bool discard_focus_loss_ime_result;
    int vertical_wheel_delta;
    int horizontal_wheel_delta;
    boxrig_mouse_event wheel_event;
};

static void invalidate_ime_placement(
    boxrig_win32_input_adapter *adapter)
{
    adapter->placement_input_context = NULL;
    adapter->synchronized_composition_form_valid = false;
    adapter->synchronized_candidate_form_valid = false;
}

static void discard_native_composition_on_focus_loss(
    boxrig_win32_input_adapter *adapter)
{
    if (!adapter->native_composing) {
        return;
    }
    /* Some IMEs complete the native string when their window loses focus.
     * Consume that stale result instead of directing the IME to cancel,
     * because CPS_CANCEL can also alter a third-party IME's input mode. */
    adapter->discard_focus_loss_ime_result = true;
    adapter->native_composing = false;
    adapter->ime_high_surrogate = 0;
    invalidate_ime_placement(adapter);
}

static bool composition_forms_equal(
    const COMPOSITIONFORM *left,
    const COMPOSITIONFORM *right)
{
    return left->dwStyle == right->dwStyle &&
           left->ptCurrentPos.x == right->ptCurrentPos.x &&
           left->ptCurrentPos.y == right->ptCurrentPos.y &&
           left->rcArea.left == right->rcArea.left &&
           left->rcArea.top == right->rcArea.top &&
           left->rcArea.right == right->rcArea.right &&
           left->rcArea.bottom == right->rcArea.bottom;
}

static bool candidate_forms_equal(
    const CANDIDATEFORM *left,
    const CANDIDATEFORM *right)
{
    return left->dwIndex == right->dwIndex &&
           left->dwStyle == right->dwStyle &&
           left->ptCurrentPos.x == right->ptCurrentPos.x &&
           left->ptCurrentPos.y == right->ptCurrentPos.y &&
           left->rcArea.left == right->rcArea.left &&
           left->rcArea.top == right->rcArea.top &&
           left->rcArea.right == right->rcArea.right &&
           left->rcArea.bottom == right->rcArea.bottom;
}

static boxrig_status route_ime_commit(
    const char *text,
    size_t text_length,
    void *context)
{
    boxrig_win32_input_adapter *adapter = context;

    return boxrig_terminal_send_text(adapter->terminal, text, text_length);
}

static bool is_deferred_ime_edit_request(WPARAM request)
{
    return request == IMR_RECONVERTSTRING ||
           request == IMR_CONFIRMRECONVERTSTRING ||
           request == IMR_DOCUMENTFEED;
}

static bool dword_add(DWORD left, DWORD right, DWORD *result_out)
{
    if (result_out == NULL || right > UINT32_MAX - left) {
        return false;
    }
    *result_out = left + right;
    return true;
}

static bool dword_multiply(DWORD left, DWORD right, DWORD *result_out)
{
    if (result_out == NULL || (right != 0 && left > UINT32_MAX / right)) {
        return false;
    }
    *result_out = left * right;
    return true;
}

static bool reconversion_range_to_utf16(
    const boxrig_ui_editable_document_snapshot *snapshot,
    const boxrig_ui_editable_document *document,
    boxrig_ui_editable_range range,
    DWORD *offset_out,
    DWORD *length_out)
{
    uint64_t range_end;
    uint64_t context_end;
    size_t relative_start;
    size_t relative_end;
    size_t utf16_start;
    size_t utf16_end;

    if (snapshot == NULL || document == NULL || offset_out == NULL ||
        length_out == NULL ||
        (uint64_t)snapshot->context_length >
            UINT64_MAX - snapshot->context_offset ||
        (uint64_t)range.length > UINT64_MAX - range.offset) {
        return false;
    }
    context_end =
        snapshot->context_offset + (uint64_t)snapshot->context_length;
    range_end = range.offset + (uint64_t)range.length;
    if (range.offset < snapshot->context_offset || range_end > context_end) {
        return false;
    }
    relative_start = (size_t)(range.offset - snapshot->context_offset);
    relative_end = (size_t)(range_end - snapshot->context_offset);
    if (!boxrig_ui_editable_document_utf8_to_utf16(
            document,
            relative_start,
            &utf16_start) ||
        !boxrig_ui_editable_document_utf8_to_utf16(
            document,
            relative_end,
            &utf16_end) ||
        utf16_start > UINT32_MAX || utf16_end > UINT32_MAX ||
        utf16_end < utf16_start) {
        return false;
    }
    *offset_out = (DWORD)utf16_start;
    *length_out = (DWORD)(utf16_end - utf16_start);
    return true;
}

static bool reconversion_description(
    boxrig_win32_input_adapter *adapter,
    bool fixed_range,
    RECONVERTSTRING *description_out,
    const uint16_t **context_out,
    DWORD *required_out)
{
    boxrig_ui_editable_document_snapshot snapshot;
    boxrig_ui_editable_range range;
    const uint16_t *context;
    size_t context_length;
    DWORD string_bytes;
    DWORD range_offset;
    DWORD range_length;
    DWORD range_offset_bytes;
    DWORD required;

    if (adapter == NULL || adapter->editable_document == NULL ||
        description_out == NULL || context_out == NULL ||
        required_out == NULL || sizeof(WCHAR) != sizeof(uint16_t) ||
        sizeof(RECONVERTSTRING) > UINT32_MAX ||
        !boxrig_ui_editable_document_get_snapshot(
            adapter->editable_document,
            &snapshot) ||
        !boxrig_ui_editable_document_get_utf16_context(
            adapter->editable_document,
            &context,
            &context_length) ||
        context_length > UINT32_MAX) {
        return false;
    }
    if (fixed_range) {
        if (!boxrig_ui_editable_document_get_reconversion_range(
                adapter->editable_document,
                &range)) {
            return false;
        }
    } else {
        range = snapshot.selection;
    }
    if (!reconversion_range_to_utf16(
            &snapshot,
            adapter->editable_document,
            range,
            &range_offset,
            &range_length) ||
        !dword_multiply(
            (DWORD)context_length,
            (DWORD)sizeof(WCHAR),
            &string_bytes) ||
        !dword_multiply(
            range_offset,
            (DWORD)sizeof(WCHAR),
            &range_offset_bytes) ||
        !dword_add(
            (DWORD)sizeof(RECONVERTSTRING),
            string_bytes,
            &required)) {
        return false;
    }
    memset(description_out, 0, sizeof(*description_out));
    description_out->dwSize = required;
    description_out->dwStrLen = (DWORD)context_length;
    description_out->dwStrOffset = sizeof(RECONVERTSTRING);
    description_out->dwCompStrLen = range_length;
    description_out->dwCompStrOffset = range_offset_bytes;
    description_out->dwTargetStrLen = range_length;
    description_out->dwTargetStrOffset = range_offset_bytes;
    *context_out = context;
    *required_out = required;
    return true;
}

static LRESULT fill_reconversion_request(
    boxrig_win32_input_adapter *adapter,
    LPARAM long_parameter,
    bool fixed_range)
{
    RECONVERTSTRING description;
    const uint16_t *context;
    DWORD required;
    DWORD capacity;

    if (!reconversion_description(
            adapter,
            fixed_range,
            &description,
            &context,
            &required)) {
        return 0;
    }
    if (long_parameter == 0) {
        return (LRESULT)required;
    }
    memcpy(&capacity, (const void *)long_parameter, sizeof(capacity));
    if (capacity < required) {
        return 0;
    }
    memcpy((void *)long_parameter, &description, sizeof(description));
    memcpy(
        (BYTE *)long_parameter + description.dwStrOffset,
        context,
        (size_t)description.dwStrLen * sizeof(WCHAR));
    return (LRESULT)required;
}

static bool reconversion_subrange(
    boxrig_win32_input_adapter *adapter,
    const boxrig_ui_editable_document_snapshot *snapshot,
    DWORD offset_bytes,
    DWORD length_units,
    boxrig_ui_editable_range *range_out)
{
    DWORD length_bytes;
    DWORD end_bytes;
    size_t utf8_start;
    size_t utf8_end;

    if (adapter == NULL || snapshot == NULL || range_out == NULL ||
        offset_bytes % sizeof(WCHAR) != 0 ||
        !dword_multiply(
            length_units,
            (DWORD)sizeof(WCHAR),
            &length_bytes) ||
        !dword_add(offset_bytes, length_bytes, &end_bytes) ||
        !boxrig_ui_editable_document_utf16_to_utf8(
            adapter->editable_document,
            offset_bytes / sizeof(WCHAR),
            &utf8_start) ||
        !boxrig_ui_editable_document_utf16_to_utf8(
            adapter->editable_document,
            end_bytes / sizeof(WCHAR),
            &utf8_end) ||
        utf8_end < utf8_start ||
        (uint64_t)utf8_start > UINT64_MAX - snapshot->context_offset) {
        return false;
    }
    range_out->offset = snapshot->context_offset + (uint64_t)utf8_start;
    range_out->length = utf8_end - utf8_start;
    return true;
}

static LRESULT confirm_reconversion_request(
    boxrig_win32_input_adapter *adapter,
    LPARAM long_parameter)
{
    RECONVERTSTRING request;
    RECONVERTSTRING expected;
    boxrig_ui_editable_document_snapshot snapshot;
    boxrig_ui_editable_range composition_range;
    boxrig_ui_editable_range target_range;
    const uint16_t *context;
    DWORD required;
    DWORD string_bytes;
    DWORD string_end;
    DWORD composition_bytes;
    DWORD composition_end;
    DWORD target_bytes;
    DWORD target_end;

    if (adapter == NULL || adapter->editable_document == NULL ||
        long_parameter == 0 ||
        boxrig_ui_editable_document_reconversion_state(
            adapter->editable_document) !=
            BOXRIG_UI_EDITABLE_RECONVERSION_ACTIVE) {
        return FALSE;
    }
    memcpy(&request, (const void *)long_parameter, sizeof(request));
    if (!reconversion_description(
            adapter,
            true,
            &expected,
            &context,
            &required) ||
        !boxrig_ui_editable_document_get_snapshot(
            adapter->editable_document,
            &snapshot) ||
        request.dwVersion != 0 || request.dwSize != required ||
        request.dwStrLen != expected.dwStrLen ||
        request.dwStrOffset != expected.dwStrOffset ||
        request.dwStrOffset % sizeof(WCHAR) != 0 ||
        request.dwCompStrOffset % sizeof(WCHAR) != 0 ||
        request.dwTargetStrOffset % sizeof(WCHAR) != 0 ||
        !dword_multiply(
            request.dwStrLen,
            (DWORD)sizeof(WCHAR),
            &string_bytes) ||
        !dword_add(request.dwStrOffset, string_bytes, &string_end) ||
        string_end > request.dwSize ||
        !dword_multiply(
            request.dwCompStrLen,
            (DWORD)sizeof(WCHAR),
            &composition_bytes) ||
        !dword_add(
            request.dwCompStrOffset,
            composition_bytes,
            &composition_end) ||
        composition_end > string_bytes ||
        !dword_multiply(
            request.dwTargetStrLen,
            (DWORD)sizeof(WCHAR),
            &target_bytes) ||
        !dword_add(
            request.dwTargetStrOffset,
            target_bytes,
            &target_end) ||
        target_end > string_bytes ||
        request.dwTargetStrOffset < request.dwCompStrOffset ||
        target_end > composition_end ||
        memcmp(
            (const BYTE *)long_parameter + request.dwStrOffset,
            context,
            string_bytes) != 0 ||
        !reconversion_subrange(
            adapter,
            &snapshot,
            request.dwCompStrOffset,
            request.dwCompStrLen,
            &composition_range) ||
        !reconversion_subrange(
            adapter,
            &snapshot,
            request.dwTargetStrOffset,
            request.dwTargetStrLen,
            &target_range) ||
        boxrig_ui_editable_document_validate_reconversion_range(
            adapter->editable_document,
            composition_range) != BOXRIG_UI_OK ||
        boxrig_ui_editable_document_validate_reconversion_range(
            adapter->editable_document,
            target_range) != BOXRIG_UI_OK) {
        return FALSE;
    }
    return boxrig_ui_editable_document_confirm_reconversion_range(
               adapter->editable_document,
               composition_range) == BOXRIG_UI_OK
               ? TRUE
               : FALSE;
}

static void clear_deferred_imm_preedit(deferred_imm_preedit *preedit);

static void reset_reconversion_characters(
    boxrig_win32_input_adapter *adapter)
{
    adapter->reconversion_character_count = 0;
}

static void notify_native_reconversion_cancel(
    boxrig_win32_input_adapter *adapter)
{
    HIMC input_context = ImmGetContext(adapter->window);

    if (input_context == NULL) {
        return;
    }
    (void)ImmNotifyIME(
        input_context,
        NI_COMPOSITIONSTR,
        CPS_CANCEL,
        0);
    (void)ImmReleaseContext(adapter->window, input_context);
}

static int restore_terminal_commit_sink(
    boxrig_win32_input_adapter *adapter)
{
    boxrig_composition_view view;

    if (!adapter->reconversion_sink_active) {
        return 0;
    }
    if (boxrig_composition_get_view(adapter->composition, &view) !=
            BOXRIG_COMPOSITION_OK ||
        view.active || view.commit_pending ||
        boxrig_composition_set_commit_sink(
            adapter->composition,
            route_ime_commit,
            adapter) != BOXRIG_COMPOSITION_OK) {
        return -1;
    }
    adapter->reconversion_sink_active = false;
    reset_reconversion_characters(adapter);
    return 0;
}

static int cancel_reconversion_session(
    boxrig_win32_input_adapter *adapter,
    bool cancel_native,
    bool discard_late_result)
{
    boxrig_ui_editable_reconversion_state state;
    boxrig_composition_view view;
    boxrig_composition_event event;

    if (adapter == NULL || adapter->editable_document == NULL) {
        return -1;
    }
    state = boxrig_ui_editable_document_reconversion_state(
        adapter->editable_document);
    if (cancel_native) {
        notify_native_reconversion_cancel(adapter);
    }
    if (boxrig_composition_get_view(adapter->composition, &view) !=
        BOXRIG_COMPOSITION_OK) {
        return -1;
    }
    if (view.commit_pending) {
        if (boxrig_composition_cancel_pending_commit(adapter->composition) !=
            BOXRIG_COMPOSITION_OK) {
            return -1;
        }
    } else if (view.active) {
        memset(&event, 0, sizeof(event));
        event.type = BOXRIG_COMPOSITION_CANCEL;
        if (boxrig_composition_apply_event(adapter->composition, &event) !=
            BOXRIG_COMPOSITION_OK) {
            return -1;
        }
    }
    clear_deferred_imm_preedit(&adapter->deferred_preedit);
    reset_reconversion_characters(adapter);
    adapter->native_composing = false;
    adapter->ime_high_surrogate = 0;
    adapter->composition_updated_since_key_release = false;
    invalidate_ime_placement(adapter);
    if (discard_late_result) {
        adapter->discard_focus_loss_ime_result = true;
    }
    if (state == BOXRIG_UI_EDITABLE_RECONVERSION_AWAITING_COMPLETION) {
        return 0;
    }
    if (state != BOXRIG_UI_EDITABLE_RECONVERSION_NONE &&
        boxrig_ui_editable_document_cancel_reconversion(
            adapter->editable_document) != BOXRIG_UI_OK) {
        return -1;
    }
    return restore_terminal_commit_sink(adapter);
}

static int synchronize_reconversion_session(
    boxrig_win32_input_adapter *adapter)
{
    boxrig_ui_editable_reconversion_state state;

    if (adapter == NULL || !adapter->reconversion_sink_active) {
        return 0;
    }
    if (adapter->editable_document == NULL) {
        return -1;
    }
    state = boxrig_ui_editable_document_reconversion_state(
        adapter->editable_document);
    if (state == BOXRIG_UI_EDITABLE_RECONVERSION_STALE) {
        return cancel_reconversion_session(adapter, true, true);
    }
    if (state == BOXRIG_UI_EDITABLE_RECONVERSION_NONE) {
        clear_deferred_imm_preedit(&adapter->deferred_preedit);
        return restore_terminal_commit_sink(adapter);
    }
    return 0;
}

static int begin_reconversion_session(
    boxrig_win32_input_adapter *adapter)
{
    boxrig_ui_editable_reconversion_state state;
    boxrig_composition_view view;

    if (adapter == NULL || adapter->editable_document == NULL ||
        synchronize_reconversion_session(adapter) == -1) {
        return -1;
    }
    state = boxrig_ui_editable_document_reconversion_state(
        adapter->editable_document);
    if (adapter->reconversion_sink_active) {
        return state == BOXRIG_UI_EDITABLE_RECONVERSION_ACTIVE ? 0 : -1;
    }
    if (state != BOXRIG_UI_EDITABLE_RECONVERSION_NONE ||
        boxrig_composition_get_view(adapter->composition, &view) !=
            BOXRIG_COMPOSITION_OK ||
        view.active || view.commit_pending ||
        boxrig_ui_editable_document_begin_reconversion(
            adapter->editable_document) != BOXRIG_UI_OK) {
        return -1;
    }
    if (boxrig_composition_set_commit_sink(
            adapter->composition,
            boxrig_ui_editable_document_commit_reconversion,
            adapter->editable_document) != BOXRIG_COMPOSITION_OK) {
        (void)boxrig_ui_editable_document_cancel_reconversion(
            adapter->editable_document);
        return -1;
    }
    adapter->reconversion_sink_active = true;
    adapter->discard_focus_loss_ime_result = false;
    reset_reconversion_characters(adapter);
    return 0;
}

static int append_reconversion_character(
    boxrig_win32_input_adapter *adapter,
    WCHAR character)
{
    boxrig_composition_limits limits = boxrig_composition_limits_default();
    WCHAR *characters;
    size_t capacity;

    if (adapter->reconversion_character_count >=
        limits.maximum_text_bytes) {
        return -1;
    }
    if (adapter->reconversion_character_count ==
        adapter->reconversion_character_capacity) {
        capacity = adapter->reconversion_character_capacity == 0
                       ? 16
                       : adapter->reconversion_character_capacity * 2;
        if (capacity < adapter->reconversion_character_capacity ||
            capacity > limits.maximum_text_bytes) {
            capacity = limits.maximum_text_bytes;
        }
        characters = realloc(
            adapter->reconversion_characters,
            capacity * sizeof(*characters));
        if (characters == NULL) {
            return -1;
        }
        adapter->reconversion_characters = characters;
        adapter->reconversion_character_capacity = capacity;
    }
    adapter->reconversion_characters[
        adapter->reconversion_character_count++] = character;
    return 0;
}

static LRESULT process_reconversion_request(
    boxrig_win32_input_adapter *adapter,
    WPARAM request,
    LPARAM long_parameter)
{
    boxrig_ui_editable_reconversion_state state;

    if (adapter->editable_document == NULL) {
        return 0;
    }
    state = boxrig_ui_editable_document_reconversion_state(
        adapter->editable_document);
    if (request == IMR_RECONVERTSTRING) {
        if (state == BOXRIG_UI_EDITABLE_RECONVERSION_NONE) {
            if (begin_reconversion_session(adapter) == -1) {
                return 0;
            }
        } else if (state != BOXRIG_UI_EDITABLE_RECONVERSION_ACTIVE ||
                   !adapter->reconversion_sink_active) {
            return 0;
        }
        return fill_reconversion_request(adapter, long_parameter, true);
    }
    if (request == IMR_DOCUMENTFEED) {
        if (state == BOXRIG_UI_EDITABLE_RECONVERSION_STALE ||
            state ==
                BOXRIG_UI_EDITABLE_RECONVERSION_AWAITING_COMPLETION) {
            return 0;
        }
        return fill_reconversion_request(
            adapter,
            long_parameter,
            state == BOXRIG_UI_EDITABLE_RECONVERSION_ACTIVE);
    }
    if (request == IMR_CONFIRMRECONVERTSTRING) {
        if (!adapter->reconversion_sink_active) {
            return 0;
        }
        return confirm_reconversion_request(adapter, long_parameter);
    }
    return 0;
}

static void clear_deferred_imm_preedit(deferred_imm_preedit *preedit)
{
    free(preedit->text);
    free(preedit->attributes);
    free(preedit->clauses);
    memset(preedit, 0, sizeof(*preedit));
}

static void defer_imm_preedit(
    boxrig_win32_input_adapter *adapter,
    WCHAR *text,
    size_t text_length,
    BYTE *attributes,
    size_t attribute_count,
    DWORD *clauses,
    size_t clause_count,
    DWORD caret)
{
    clear_deferred_imm_preedit(&adapter->deferred_preedit);
    adapter->deferred_preedit.text = text;
    adapter->deferred_preedit.text_length = text_length;
    adapter->deferred_preedit.attributes = attributes;
    adapter->deferred_preedit.attribute_count = attribute_count;
    adapter->deferred_preedit.clauses = clauses;
    adapter->deferred_preedit.clause_count = clause_count;
    adapter->deferred_preedit.caret = caret;
    adapter->deferred_preedit.active = true;
}

static int apply_deferred_imm_preedit(
    boxrig_win32_input_adapter *adapter)
{
    deferred_imm_preedit preedit;
    int result;

    if (!adapter->deferred_preedit.active) {
        return 0;
    }
    preedit = adapter->deferred_preedit;
    memset(&adapter->deferred_preedit, 0, sizeof(adapter->deferred_preedit));
    result = boxrig_win32_input_adapter_composition_update_utf16(
        adapter,
        preedit.text,
        preedit.text_length,
        preedit.attributes,
        preedit.attribute_count,
        preedit.clauses,
        preedit.clause_count,
        preedit.caret);
    clear_deferred_imm_preedit(&preedit);
    return result;
}

static unsigned int message_scan_code(LPARAM parameter)
{
    return (unsigned int)(((ULONG_PTR)parameter >> 16) & 0xffu);
}

static bool message_extended(LPARAM parameter)
{
    return (((ULONG_PTR)parameter >> 24) & 1u) != 0;
}

static size_t key_slot(UINT virtual_key, LPARAM parameter)
{
    unsigned int scan_code = message_scan_code(parameter);

    if (scan_code == 0) {
        scan_code = virtual_key & 0xffu;
    }
    return scan_code + (message_extended(parameter) ? 256u : 0u);
}

static UINT resolved_virtual_key(UINT virtual_key, LPARAM parameter)
{
    UINT scan_code;
    UINT resolved;

    if (virtual_key != VK_SHIFT && virtual_key != VK_CONTROL &&
        virtual_key != VK_MENU) {
        return virtual_key;
    }
    scan_code = message_scan_code(parameter);
    if (message_extended(parameter)) {
        scan_code |= 0xe000u;
    }
    resolved = MapVirtualKeyExW(
        scan_code,
        MAPVK_VSC_TO_VK_EX,
        GetKeyboardLayout(0));
    if (resolved != 0) {
        return resolved;
    }
    if (virtual_key == VK_SHIFT) {
        return VK_LSHIFT;
    }
    if (virtual_key == VK_CONTROL) {
        return message_extended(parameter) ? VK_RCONTROL : VK_LCONTROL;
    }
    return message_extended(parameter) ? VK_RMENU : VK_LMENU;
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
    default:
        return 0;
    }
}

static bool modifier_still_pressed(
    const boxrig_win32_input_adapter *adapter,
    uint32_t modifier)
{
    size_t slot;

    for (slot = 0; slot < KEY_SLOT_COUNT; slot++) {
        if (adapter->pressed[slot] &&
            modifier_for_key(adapter->identities[slot].key) == modifier) {
            return true;
        }
    }
    return false;
}

static uint32_t keyboard_modifier_state(void)
{
    uint32_t modifiers = 0;

    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        modifiers |= BOXRIG_MODIFIER_SHIFT;
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        modifiers |= BOXRIG_MODIFIER_CONTROL;
    }
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
        modifiers |= BOXRIG_MODIFIER_ALT;
    }
    if ((GetKeyState(VK_LWIN) & 0x8000) != 0 ||
        (GetKeyState(VK_RWIN) & 0x8000) != 0) {
        modifiers |= BOXRIG_MODIFIER_SUPER;
    }
    if ((GetKeyState(VK_CAPITAL) & 1) != 0) {
        modifiers |= BOXRIG_MODIFIER_CAPS_LOCK;
    }
    if ((GetKeyState(VK_NUMLOCK) & 1) != 0) {
        modifiers |= BOXRIG_MODIFIER_NUM_LOCK;
    }
    return modifiers;
}

static boxrig_key translate_functional_key(
    UINT virtual_key,
    bool extended)
{
    if (virtual_key >= VK_F1 && virtual_key <= VK_F24) {
        return (boxrig_key)(BOXRIG_KEY_F1 + (virtual_key - VK_F1));
    }
    switch (virtual_key) {
    case VK_RETURN:
        return extended ? BOXRIG_KEY_KP_ENTER : BOXRIG_KEY_ENTER;
    case VK_BACK:
        return BOXRIG_KEY_BACKSPACE;
    case VK_TAB:
        return BOXRIG_KEY_TAB;
    case VK_ESCAPE:
        return BOXRIG_KEY_ESCAPE;
    case VK_UP:
        return extended ? BOXRIG_KEY_UP : BOXRIG_KEY_KP_UP;
    case VK_DOWN:
        return extended ? BOXRIG_KEY_DOWN : BOXRIG_KEY_KP_DOWN;
    case VK_RIGHT:
        return extended ? BOXRIG_KEY_RIGHT : BOXRIG_KEY_KP_RIGHT;
    case VK_LEFT:
        return extended ? BOXRIG_KEY_LEFT : BOXRIG_KEY_KP_LEFT;
    case VK_HOME:
        return extended ? BOXRIG_KEY_HOME : BOXRIG_KEY_KP_HOME;
    case VK_END:
        return extended ? BOXRIG_KEY_END : BOXRIG_KEY_KP_END;
    case VK_INSERT:
        return extended ? BOXRIG_KEY_INSERT : BOXRIG_KEY_KP_INSERT;
    case VK_DELETE:
        return extended ? BOXRIG_KEY_DELETE : BOXRIG_KEY_KP_DELETE;
    case VK_PRIOR:
        return extended ? BOXRIG_KEY_PAGE_UP : BOXRIG_KEY_KP_PAGE_UP;
    case VK_NEXT:
        return extended ? BOXRIG_KEY_PAGE_DOWN : BOXRIG_KEY_KP_PAGE_DOWN;
    case VK_CLEAR:
        return BOXRIG_KEY_KP_BEGIN;
    case VK_NUMPAD0:
        return BOXRIG_KEY_KP_0;
    case VK_NUMPAD1:
        return BOXRIG_KEY_KP_1;
    case VK_NUMPAD2:
        return BOXRIG_KEY_KP_2;
    case VK_NUMPAD3:
        return BOXRIG_KEY_KP_3;
    case VK_NUMPAD4:
        return BOXRIG_KEY_KP_4;
    case VK_NUMPAD5:
        return BOXRIG_KEY_KP_5;
    case VK_NUMPAD6:
        return BOXRIG_KEY_KP_6;
    case VK_NUMPAD7:
        return BOXRIG_KEY_KP_7;
    case VK_NUMPAD8:
        return BOXRIG_KEY_KP_8;
    case VK_NUMPAD9:
        return BOXRIG_KEY_KP_9;
    case VK_DECIMAL:
        return BOXRIG_KEY_KP_DECIMAL;
    case VK_DIVIDE:
        return BOXRIG_KEY_KP_DIVIDE;
    case VK_MULTIPLY:
        return BOXRIG_KEY_KP_MULTIPLY;
    case VK_SUBTRACT:
        return BOXRIG_KEY_KP_SUBTRACT;
    case VK_ADD:
        return BOXRIG_KEY_KP_ADD;
    case VK_SEPARATOR:
        return BOXRIG_KEY_KP_SEPARATOR;
    case VK_CAPITAL:
        return BOXRIG_KEY_CAPS_LOCK;
    case VK_SCROLL:
        return BOXRIG_KEY_SCROLL_LOCK;
    case VK_NUMLOCK:
        return BOXRIG_KEY_NUM_LOCK;
    case VK_SNAPSHOT:
        return BOXRIG_KEY_PRINT_SCREEN;
    case VK_PAUSE:
        return BOXRIG_KEY_PAUSE;
    case VK_APPS:
        return BOXRIG_KEY_MENU;
    case VK_LSHIFT:
        return BOXRIG_KEY_LEFT_SHIFT;
    case VK_RSHIFT:
        return BOXRIG_KEY_RIGHT_SHIFT;
    case VK_LCONTROL:
        return BOXRIG_KEY_LEFT_CONTROL;
    case VK_RCONTROL:
        return BOXRIG_KEY_RIGHT_CONTROL;
    case VK_LMENU:
        return BOXRIG_KEY_LEFT_ALT;
    case VK_RMENU:
        return BOXRIG_KEY_RIGHT_ALT;
    case VK_LWIN:
        return BOXRIG_KEY_LEFT_SUPER;
    case VK_RWIN:
        return BOXRIG_KEY_RIGHT_SUPER;
    case VK_MEDIA_PLAY_PAUSE:
        return BOXRIG_KEY_MEDIA_PLAY_PAUSE;
    case VK_MEDIA_STOP:
        return BOXRIG_KEY_MEDIA_STOP;
    case VK_MEDIA_NEXT_TRACK:
        return BOXRIG_KEY_MEDIA_TRACK_NEXT;
    case VK_MEDIA_PREV_TRACK:
        return BOXRIG_KEY_MEDIA_TRACK_PREVIOUS;
    case VK_VOLUME_DOWN:
        return BOXRIG_KEY_LOWER_VOLUME;
    case VK_VOLUME_UP:
        return BOXRIG_KEY_RAISE_VOLUME;
    case VK_VOLUME_MUTE:
        return BOXRIG_KEY_MUTE_VOLUME;
    default:
        return BOXRIG_KEY_NONE;
    }
}

static uint32_t utf16_codepoint(const wchar_t *text, int length)
{
    uint32_t high;
    uint32_t low;

    if (length == 1 && (text[0] < 0xd800 || text[0] > 0xdfff)) {
        return (uint32_t)text[0];
    }
    if (length != 2 || text[0] < 0xd800 || text[0] > 0xdbff ||
        text[1] < 0xdc00 || text[1] > 0xdfff) {
        return 0;
    }
    high = (uint32_t)text[0] - 0xd800u;
    low = (uint32_t)text[1] - 0xdc00u;
    return 0x10000u + (high << 10) + low;
}

static uint32_t translated_codepoint(
    UINT virtual_key,
    UINT scan_code,
    bool shifted)
{
    BYTE keyboard_state[256];
    wchar_t text[4];
    int length;
    uint32_t codepoint;

    memset(keyboard_state, 0, sizeof(keyboard_state));
    if (shifted) {
        keyboard_state[VK_SHIFT] = 0x80;
    }
    length = ToUnicodeEx(
        virtual_key,
        scan_code,
        keyboard_state,
        text,
        (int)(sizeof(text) / sizeof(text[0])),
        TO_UNICODE_DO_NOT_CHANGE_STATE,
        GetKeyboardLayout(0));
    codepoint = length > 0 ? utf16_codepoint(text, length) : 0;
    if (codepoint < 0x20 || (codepoint >= 0x7f && codepoint <= 0x9f)) {
        return 0;
    }
    return codepoint;
}

static uint32_t pc101_base_layout_codepoint(UINT virtual_key)
{
    if (virtual_key >= 'A' && virtual_key <= 'Z') {
        return (uint32_t)('a' + (virtual_key - 'A'));
    }
    if (virtual_key >= '0' && virtual_key <= '9') {
        return (uint32_t)virtual_key;
    }
    switch (virtual_key) {
    case VK_SPACE:
        return ' ';
    case VK_OEM_1:
        return ';';
    case VK_OEM_PLUS:
        return '=';
    case VK_OEM_COMMA:
        return ',';
    case VK_OEM_MINUS:
        return '-';
    case VK_OEM_PERIOD:
        return '.';
    case VK_OEM_2:
        return '/';
    case VK_OEM_3:
        return '`';
    case VK_OEM_4:
        return '[';
    case VK_OEM_5:
        return '\\';
    case VK_OEM_6:
        return ']';
    case VK_OEM_7:
        return '\'';
    default:
        return 0;
    }
}

static key_identity identify_key(
    UINT virtual_key,
    LPARAM parameter)
{
    key_identity identity;
    UINT resolved = resolved_virtual_key(virtual_key, parameter);
    UINT scan_code = message_scan_code(parameter);

    memset(&identity, 0, sizeof(identity));
    identity.key = translate_functional_key(
        resolved,
        message_extended(parameter));
    if (identity.key != BOXRIG_KEY_NONE) {
        return identity;
    }
    identity.base_layout_codepoint = pc101_base_layout_codepoint(resolved);
    identity.codepoint = translated_codepoint(
        resolved,
        scan_code,
        false);
    identity.shifted_codepoint = translated_codepoint(
        resolved,
        scan_code,
        true);
    if (identity.codepoint == 0 && resolved >= 'A' && resolved <= 'Z') {
        identity.codepoint = (uint32_t)('a' + (resolved - 'A'));
        identity.shifted_codepoint = (uint32_t)resolved;
    }
    if (identity.codepoint == 0 && resolved >= '0' && resolved <= '9') {
        identity.codepoint = (uint32_t)resolved;
    }
    if (identity.codepoint == 0 && resolved == VK_SPACE) {
        identity.codepoint = 0x20;
    }
    if (identity.shifted_codepoint == identity.codepoint) {
        identity.shifted_codepoint = 0;
    }
    return identity;
}

static boxrig_input_key_route route_key_identity(
    boxrig_win32_input_adapter *adapter,
    key_identity identity,
    boxrig_key_action action)
{
    boxrig_key_event event;
    boxrig_input_key_route route;

    if (adapter->key_router == NULL) {
        return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    }
    memset(&event, 0, sizeof(event));
    event.key = identity.key;
    event.codepoint = identity.codepoint;
    event.shifted_codepoint = identity.shifted_codepoint;
    event.base_layout_codepoint = identity.base_layout_codepoint;
    event.action = action;
    event.modifiers = adapter->modifiers;
    route = adapter->key_router(&event, adapter->key_router_context);
    if (route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL &&
        route != BOXRIG_INPUT_KEY_ROUTE_APPLICATION &&
        route != BOXRIG_INPUT_KEY_ROUTE_ERROR) {
        return BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    return route;
}

static void prepare_routed_character(
    boxrig_win32_input_adapter *adapter,
    key_identity identity,
    boxrig_key_action action,
    size_t slot,
    bool application)
{
    adapter->character.active = true;
    adapter->character.application = application;
    adapter->character.slot = slot;
    adapter->character.identity = identity;
    adapter->character.action = action;
    adapter->character.modifiers = adapter->modifiers;
}

static int queue_key(
    boxrig_win32_input_adapter *adapter,
    const boxrig_key_event *event)
{
    boxrig_status status = boxrig_terminal_send_key(adapter->terminal, event);

    if (status == BOXRIG_STATUS_OK) {
        return 0;
    }
    if (status != BOXRIG_STATUS_WOULD_BLOCK ||
        adapter->pending.kind != PENDING_INPUT_NONE) {
        return -1;
    }
    adapter->pending.kind = PENDING_INPUT_KEY;
    adapter->pending.key = *event;
    adapter->pending.key.text = NULL;
    if (event->text_length != 0) {
        adapter->pending.key_text = malloc(event->text_length);
        if (adapter->pending.key_text == NULL) {
            adapter->pending.kind = PENDING_INPUT_NONE;
            return -1;
        }
        memcpy(
            adapter->pending.key_text,
            event->text,
            event->text_length);
        adapter->pending.key.text = adapter->pending.key_text;
    }
    return 0;
}

static int queue_focus(
    boxrig_win32_input_adapter *adapter,
    const boxrig_focus_event *event)
{
    boxrig_status status = boxrig_terminal_send_focus(
        adapter->terminal,
        event);

    if (status == BOXRIG_STATUS_OK) {
        return 0;
    }
    if (status != BOXRIG_STATUS_WOULD_BLOCK ||
        adapter->pending.kind != PENDING_INPUT_NONE) {
        return -1;
    }
    adapter->pending.kind = PENDING_INPUT_FOCUS;
    adapter->pending.focus = *event;
    return 0;
}

static int queue_mouse(
    boxrig_win32_input_adapter *adapter,
    const boxrig_mouse_event *event)
{
    boxrig_status status = boxrig_terminal_send_mouse(
        adapter->terminal,
        event);

    if (status == BOXRIG_STATUS_OK) {
        return 0;
    }
    if (status != BOXRIG_STATUS_WOULD_BLOCK ||
        adapter->pending.kind != PENDING_INPUT_NONE) {
        return -1;
    }
    adapter->pending.kind = PENDING_INPUT_MOUSE;
    adapter->pending.mouse = *event;
    return 0;
}

static int retry_pending_input(boxrig_win32_input_adapter *adapter)
{
    boxrig_status status;

    switch (adapter->pending.kind) {
    case PENDING_INPUT_NONE:
        return 0;
    case PENDING_INPUT_KEY:
        status = boxrig_terminal_send_key(
            adapter->terminal,
            &adapter->pending.key);
        break;
    case PENDING_INPUT_FOCUS:
        status = boxrig_terminal_send_focus(
            adapter->terminal,
            &adapter->pending.focus);
        break;
    case PENDING_INPUT_MOUSE:
        status = boxrig_terminal_send_mouse(
            adapter->terminal,
            &adapter->pending.mouse);
        break;
    default:
        return -1;
    }
    if (status == BOXRIG_STATUS_WOULD_BLOCK) {
        return 0;
    }
    if (status != BOXRIG_STATUS_OK) {
        return -1;
    }
    free(adapter->pending.key_text);
    memset(&adapter->pending, 0, sizeof(adapter->pending));
    return 0;
}

static int retry_paste(boxrig_win32_input_adapter *adapter)
{
    while (adapter->paste_active) {
        boxrig_paste_event event;
        boxrig_status status;

        memset(&event, 0, sizeof(event));
        if (adapter->paste_state == PASTE_STATE_BEGIN) {
            event.phase = BOXRIG_PASTE_BEGIN;
        } else if (adapter->paste_state == PASTE_STATE_CHUNK) {
            size_t remaining = adapter->paste_length - adapter->paste_offset;

            if (remaining == 0) {
                adapter->paste_state = PASTE_STATE_END;
                continue;
            }
            event.phase = BOXRIG_PASTE_CHUNK;
            event.text = adapter->paste_text + adapter->paste_offset;
            event.text_length = boxrig_input_text_chunk_length(
                event.text, remaining, adapter->paste_chunk_size);
        } else {
            event.phase = BOXRIG_PASTE_END;
        }
        status = boxrig_terminal_send_paste(adapter->terminal, &event);
        if (status == BOXRIG_STATUS_WOULD_BLOCK) {
            return 0;
        }
        if (status != BOXRIG_STATUS_OK) {
            return -1;
        }
        if (adapter->paste_state == PASTE_STATE_BEGIN) {
            adapter->paste_state = PASTE_STATE_CHUNK;
        } else if (adapter->paste_state == PASTE_STATE_CHUNK) {
            adapter->paste_offset += event.text_length;
        } else {
            free(adapter->paste_text);
            adapter->paste_text = NULL;
            adapter->paste_length = 0;
            adapter->paste_offset = 0;
            adapter->paste_active = false;
        }
    }
    return 0;
}

static int encode_codepoint(
    uint32_t codepoint,
    char *utf8,
    size_t *length_out)
{
    wchar_t utf16[2];
    int utf16_length;
    int utf8_length;

    if (codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
        return -1;
    }
    if (codepoint <= 0xffff) {
        utf16[0] = (wchar_t)codepoint;
        utf16_length = 1;
    } else {
        uint32_t value = codepoint - 0x10000u;

        utf16[0] = (wchar_t)(0xd800u + (value >> 10));
        utf16[1] = (wchar_t)(0xdc00u + (value & 0x3ffu));
        utf16_length = 2;
    }
    utf8_length = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        utf16,
        utf16_length,
        utf8,
        4,
        NULL,
        NULL);
    if (utf8_length <= 0) {
        return -1;
    }
    *length_out = (size_t)utf8_length;
    return 0;
}

static bool checked_array_size(size_t count, size_t element_size)
{
    return element_size == 0 || count <= SIZE_MAX / element_size;
}

static void decoded_utf16_text_destroy(decoded_utf16_text *decoded)
{
    free(decoded->text);
    free(decoded->unit_offsets);
    memset(decoded, 0, sizeof(*decoded));
}

static int decode_utf16_text(
    const WCHAR *source,
    size_t source_length,
    decoded_utf16_text *decoded_out)
{
    decoded_utf16_text decoded;
    size_t unit = 0;

    memset(&decoded, 0, sizeof(decoded));
    if ((source == NULL && source_length != 0) ||
        source_length > SIZE_MAX / 4 ||
        !checked_array_size(
            source_length + 1,
            sizeof(*decoded.unit_offsets))) {
        return -1;
    }
    decoded.text = malloc(source_length * 4 + 1);
    decoded.unit_offsets = malloc(
        (source_length + 1) * sizeof(*decoded.unit_offsets));
    if (decoded.text == NULL || decoded.unit_offsets == NULL) {
        decoded_utf16_text_destroy(&decoded);
        return -1;
    }
    decoded.unit_count = source_length;
    while (unit < source_length) {
        uint32_t codepoint;
        size_t unit_length = 1;
        size_t encoded_length;

        decoded.unit_offsets[unit] = decoded.text_length;
        if (source[unit] >= 0xd800 && source[unit] <= 0xdbff) {
            if (unit + 1 >= source_length || source[unit + 1] < 0xdc00 ||
                source[unit + 1] > 0xdfff) {
                decoded_utf16_text_destroy(&decoded);
                return -1;
            }
            codepoint = 0x10000u +
                        (((uint32_t)source[unit] - 0xd800u) << 10) +
                        ((uint32_t)source[unit + 1] - 0xdc00u);
            decoded.unit_offsets[unit + 1] = SIZE_MAX;
            unit_length = 2;
        } else {
            if (source[unit] >= 0xdc00 && source[unit] <= 0xdfff) {
                decoded_utf16_text_destroy(&decoded);
                return -1;
            }
            codepoint = source[unit];
        }
        if (encode_codepoint(
                codepoint,
                decoded.text + decoded.text_length,
                &encoded_length) == -1) {
            decoded_utf16_text_destroy(&decoded);
            return -1;
        }
        decoded.text_length += encoded_length;
        unit += unit_length;
        decoded.unit_offsets[unit] = decoded.text_length;
    }
    decoded.unit_offsets[source_length] = decoded.text_length;
    decoded.text[decoded.text_length] = '\0';
    *decoded_out = decoded;
    return 0;
}

static uint32_t translate_imm_attribute(BYTE attribute)
{
    switch (attribute) {
    case ATTR_INPUT:
        return BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE;
    case ATTR_TARGET_CONVERTED:
        return BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
               BOXRIG_COMPOSITION_FEEDBACK_TARGET;
    case ATTR_CONVERTED:
        return BOXRIG_COMPOSITION_FEEDBACK_PRIMARY |
               BOXRIG_COMPOSITION_FEEDBACK_CONVERTED;
    case ATTR_TARGET_NOTCONVERTED:
        return BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
               BOXRIG_COMPOSITION_FEEDBACK_TARGET;
    case ATTR_INPUT_ERROR:
        return BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE |
               BOXRIG_COMPOSITION_FEEDBACK_ERROR;
    case ATTR_FIXEDCONVERTED:
        return BOXRIG_COMPOSITION_FEEDBACK_TERTIARY |
               BOXRIG_COMPOSITION_FEEDBACK_CONVERTED;
    default:
        return BOXRIG_COMPOSITION_FEEDBACK_NONE;
    }
}

static bool is_clause_boundary(
    const DWORD *clauses,
    size_t clause_count,
    size_t unit)
{
    size_t index;

    for (index = 1; index + 1 < clause_count; index++) {
        if ((size_t)clauses[index] == unit) {
            return true;
        }
    }
    return false;
}

static bool clauses_are_valid(
    const decoded_utf16_text *decoded,
    const DWORD *clauses,
    size_t clause_count)
{
    size_t index;

    if (clause_count == 0) {
        return clauses == NULL;
    }
    if (clauses == NULL ||
        (clause_count == 1 && decoded->unit_count != 0) ||
        (clause_count > 1 && clauses[0] != 0) ||
        clauses[clause_count - 1] != decoded->unit_count) {
        return false;
    }
    /* IMM may expose both endpoints of an empty clause as offset zero. */
    if (decoded->unit_count == 0 && clause_count == 2) {
        return true;
    }
    for (index = 1; index < clause_count; index++) {
        if (clauses[index] <= clauses[index - 1] ||
            clauses[index] > decoded->unit_count ||
            decoded->unit_offsets[clauses[index]] == SIZE_MAX) {
            return false;
        }
    }
    return true;
}

static int apply_empty_composition_event(
    boxrig_win32_input_adapter *adapter,
    boxrig_composition_event_type type)
{
    boxrig_composition_event event;

    memset(&event, 0, sizeof(event));
    event.type = type;
    return boxrig_composition_apply_event(adapter->composition, &event) ==
                   BOXRIG_COMPOSITION_OK
               ? 0
               : -1;
}

static int queue_character(
    boxrig_win32_input_adapter *adapter,
    uint32_t codepoint)
{
    boxrig_key_event event;
    char utf8[4];
    size_t utf8_length;

    if (encode_codepoint(codepoint, utf8, &utf8_length) == -1) {
        return -1;
    }
    memset(&event, 0, sizeof(event));
    if (adapter->character.active) {
        event.key = adapter->character.identity.key;
        event.codepoint = adapter->character.identity.codepoint;
        event.shifted_codepoint =
            adapter->character.identity.shifted_codepoint;
        event.base_layout_codepoint =
            adapter->character.identity.base_layout_codepoint;
        event.action = adapter->character.action;
        event.modifiers = adapter->character.modifiers;
    } else {
        event.codepoint = codepoint;
        event.action = BOXRIG_KEY_PRESS;
        event.modifiers = adapter->modifiers;
    }
    if (event.key == BOXRIG_KEY_NONE && event.codepoint == 0) {
        event.codepoint = codepoint;
    }
    event.text = utf8;
    event.text_length = utf8_length;
    adapter->character.active = false;
    return queue_key(adapter, &event);
}

static bool suppress_application_character(
    boxrig_win32_input_adapter *adapter,
    wchar_t character)
{
    if (!adapter->character.active || !adapter->character.application) {
        return false;
    }
    if (character >= 0xd800 && character <= 0xdbff) {
        adapter->high_surrogate = character;
        return true;
    }
    adapter->character.active = false;
    adapter->character.application = false;
    adapter->high_surrogate = 0;
    return true;
}

static int process_utf16_character(
    boxrig_win32_input_adapter *adapter,
    wchar_t character)
{
    uint32_t codepoint;

    if (character >= 0xd800 && character <= 0xdbff) {
        adapter->high_surrogate = character;
        return 0;
    }
    if (character >= 0xdc00 && character <= 0xdfff) {
        wchar_t pair[2];

        if (adapter->high_surrogate == 0) {
            return 0;
        }
        pair[0] = adapter->high_surrogate;
        pair[1] = character;
        adapter->high_surrogate = 0;
        codepoint = utf16_codepoint(pair, 2);
    } else {
        adapter->high_surrogate = 0;
        codepoint = (uint32_t)character;
    }
    return queue_character(adapter, codepoint);
}

static int process_ime_utf16_character(
    boxrig_win32_input_adapter *adapter,
    WCHAR character)
{
    WCHAR text[2];
    size_t length = 1;

    if (character >= 0xd800 && character <= 0xdbff) {
        adapter->ime_high_surrogate = character;
        return 0;
    }
    if (character >= 0xdc00 && character <= 0xdfff) {
        if (adapter->ime_high_surrogate == 0) {
            return 0;
        }
        text[0] = adapter->ime_high_surrogate;
        text[1] = character;
        length = 2;
    } else {
        text[0] = character;
    }
    adapter->ime_high_surrogate = 0;
    return boxrig_win32_input_adapter_composition_commit_utf16(
        adapter,
        text,
        length);
}

static int paste_from_clipboard(boxrig_win32_input_adapter *adapter)
{
    HGLOBAL handle;
    const wchar_t *text;
    SIZE_T allocation_size;
    size_t maximum_characters;
    size_t scan_limit;
    size_t character_count = 0;
    int utf8_length;
    uint8_t *utf8 = NULL;
    int result = -1;

    if (!OpenClipboard(adapter->window)) {
        return -1;
    }
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        goto cleanup;
    }
    handle = (HGLOBAL)GetClipboardData(CF_UNICODETEXT);
    if (handle == NULL) {
        goto cleanup;
    }
    allocation_size = GlobalSize(handle);
    if (allocation_size < sizeof(*text) ||
        allocation_size % sizeof(*text) != 0) {
        goto cleanup;
    }
    text = GlobalLock(handle);
    if (text == NULL) {
        goto cleanup;
    }
    maximum_characters = (size_t)allocation_size / sizeof(*text);
    scan_limit = maximum_characters;
    if (adapter->paste_capacity < scan_limit - 1) {
        scan_limit = adapter->paste_capacity + 1;
    }
    if ((size_t)INT_MAX < scan_limit - 1) {
        scan_limit = (size_t)INT_MAX + 1;
    }
    while (character_count < scan_limit &&
           text[character_count] != L'\0') {
        character_count++;
    }
    if (character_count == scan_limit) {
        (void)GlobalUnlock(handle);
        goto cleanup;
    }
    if (character_count == 0) {
        (void)GlobalUnlock(handle);
        if (!CloseClipboard()) {
            return -1;
        }
        return boxrig_win32_input_adapter_queue_paste(adapter, NULL, 0);
    }
    utf8_length = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text,
        (int)character_count,
        NULL,
        0,
        NULL,
        NULL);
    if (utf8_length <= 0 ||
        (size_t)utf8_length > adapter->paste_capacity) {
        (void)GlobalUnlock(handle);
        goto cleanup;
    }
    utf8 = malloc((size_t)utf8_length);
    if (utf8 == NULL) {
        (void)GlobalUnlock(handle);
        result = -1;
        goto cleanup;
    }
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text,
            (int)character_count,
            (char *)utf8,
            utf8_length,
            NULL,
            NULL) != utf8_length) {
        (void)GlobalUnlock(handle);
        result = -1;
        goto cleanup;
    }
    (void)GlobalUnlock(handle);
    if (!CloseClipboard()) {
        free(utf8);
        return -1;
    }
    result = boxrig_win32_input_adapter_queue_paste(
        adapter,
        utf8,
        (size_t)utf8_length);
    free(utf8);
    return result;

cleanup:
    free(utf8);
    (void)CloseClipboard();
    return result;
}

static boxrig_ui_status copy_to_clipboard(
    boxrig_win32_input_adapter *adapter,
    boxrig_ui_view *view)
{
    boxrig_text_snapshot *snapshot = NULL;
    const uint8_t *utf8;
    size_t utf8_length;
    size_t line_feed_count = 0;
    size_t wide_character_count;
    size_t allocation_character_count;
    size_t allocation_size;
    size_t source;
    size_t destination;
    int converted_count;
    HGLOBAL allocation = NULL;
    WCHAR *text;
    boxrig_ui_status status;

    if (view == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = boxrig_ui_view_create_selection_snapshot(view, &snapshot);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    utf8 = boxrig_text_snapshot_data(snapshot, &utf8_length);
    if (utf8_length > INT_MAX) {
        boxrig_text_snapshot_destroy(snapshot);
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    for (source = 0; source < utf8_length; source++) {
        if (utf8[source] == '\n') {
            line_feed_count++;
        }
    }
    if (utf8_length == 0) {
        converted_count = 0;
    } else {
        converted_count = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            (const char *)utf8,
            (int)utf8_length,
            NULL,
            0);
        if (converted_count <= 0) {
            boxrig_text_snapshot_destroy(snapshot);
            return BOXRIG_UI_INVALID_ARGUMENT;
        }
    }
    wide_character_count = (size_t)converted_count;
    if (wide_character_count > SIZE_MAX - line_feed_count - 1) {
        boxrig_text_snapshot_destroy(snapshot);
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    allocation_character_count =
        wide_character_count + line_feed_count + 1;
    if (!checked_array_size(allocation_character_count, sizeof(*text))) {
        boxrig_text_snapshot_destroy(snapshot);
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    allocation_size = allocation_character_count * sizeof(*text);
    allocation = GlobalAlloc(GMEM_MOVEABLE, allocation_size);
    if (allocation == NULL) {
        boxrig_text_snapshot_destroy(snapshot);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    text = GlobalLock(allocation);
    if (text == NULL) {
        boxrig_text_snapshot_destroy(snapshot);
        (void)GlobalFree(allocation);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    if (utf8_length != 0 &&
        MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            (const char *)utf8,
            (int)utf8_length,
            text,
            converted_count) != converted_count) {
        boxrig_text_snapshot_destroy(snapshot);
        (void)GlobalUnlock(allocation);
        (void)GlobalFree(allocation);
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    boxrig_text_snapshot_destroy(snapshot);
    source = wide_character_count;
    destination = wide_character_count + line_feed_count;
    text[destination] = L'\0';
    while (source != 0) {
        WCHAR character = text[--source];

        text[--destination] = character;
        if (character == L'\n') {
            text[--destination] = L'\r';
        }
    }
    /* GlobalUnlock returns zero for both the final release and a failure. */
    SetLastError(NO_ERROR);
    if (!GlobalUnlock(allocation) && GetLastError() != NO_ERROR) {
        (void)GlobalFree(allocation);
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (!OpenClipboard(adapter->window)) {
        (void)GlobalFree(allocation);
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (!EmptyClipboard() ||
        SetClipboardData(CF_UNICODETEXT, allocation) == NULL) {
        (void)CloseClipboard();
        (void)GlobalFree(allocation);
        return BOXRIG_UI_UNAVAILABLE;
    }
    allocation = NULL;
    return CloseClipboard() ? BOXRIG_UI_OK : BOXRIG_UI_UNAVAILABLE;
}

static int process_key_down(
    boxrig_win32_input_adapter *adapter,
    UINT virtual_key,
    LPARAM parameter)
{
    size_t slot = key_slot(virtual_key, parameter);
    key_identity identity = identify_key(virtual_key, parameter);
    uint32_t changed_modifier = modifier_for_key(identity.key);
    bool repeated = (((ULONG_PTR)parameter >> 30) & 1u) != 0 ||
                    adapter->pressed[slot];
    boxrig_key_action action = repeated ? BOXRIG_KEY_REPEAT
                                        : BOXRIG_KEY_PRESS;
    boxrig_input_key_route route;

    if (!repeated) {
        adapter->application_key[slot] = false;
    }
    adapter->pressed[slot] = true;
    adapter->identities[slot] = identity;
    if (identity.key == BOXRIG_KEY_CAPS_LOCK && !repeated) {
        adapter->modifiers ^= BOXRIG_MODIFIER_CAPS_LOCK;
    }
    if (identity.key == BOXRIG_KEY_NUM_LOCK && !repeated) {
        adapter->modifiers ^= BOXRIG_MODIFIER_NUM_LOCK;
    }
    adapter->modifiers |= changed_modifier;
    if (repeated && adapter->application_key[slot]) {
        route = route_key_identity(adapter, identity, action);
        if (route == BOXRIG_INPUT_KEY_ROUTE_ERROR) {
            return -1;
        }
        prepare_routed_character(
            adapter,
            identity,
            action,
            slot,
            true);
        return 0;
    }
    if (!repeated) {
        route = route_key_identity(adapter, identity, action);
        if (route == BOXRIG_INPUT_KEY_ROUTE_ERROR) {
            return -1;
        }
        if (route == BOXRIG_INPUT_KEY_ROUTE_APPLICATION) {
            adapter->application_key[slot] = true;
            prepare_routed_character(
                adapter,
                identity,
                action,
                slot,
                true);
            return 0;
        }
    }
    if (identity.key != BOXRIG_KEY_NONE) {
        boxrig_key_event event;

        adapter->character.active = false;
        memset(&event, 0, sizeof(event));
        event.key = identity.key;
        event.action = action;
        event.modifiers = adapter->modifiers;
        return queue_key(adapter, &event);
    }
    prepare_routed_character(adapter, identity, action, slot, false);
    return 0;
}

static int process_key_up(
    boxrig_win32_input_adapter *adapter,
    UINT virtual_key,
    LPARAM parameter)
{
    size_t slot = key_slot(virtual_key, parameter);
    key_identity identity = adapter->pressed[slot]
                                ? adapter->identities[slot]
                                : identify_key(virtual_key, parameter);
    uint32_t changed_modifier = modifier_for_key(identity.key);
    boxrig_key_event event;
    boxrig_input_key_route route;
    bool application = adapter->application_key[slot];

    adapter->pressed[slot] = false;
    if (changed_modifier != 0 &&
        !modifier_still_pressed(adapter, changed_modifier)) {
        adapter->modifiers &= ~changed_modifier;
    }
    if (application) {
        if (adapter->character.active && adapter->character.application &&
            adapter->character.slot == slot) {
            adapter->character.active = false;
            adapter->character.application = false;
            adapter->high_surrogate = 0;
        }
        route = route_key_identity(adapter, identity, BOXRIG_KEY_RELEASE);
        adapter->application_key[slot] = false;
        return route == BOXRIG_INPUT_KEY_ROUTE_ERROR ? -1 : 0;
    }
    memset(&event, 0, sizeof(event));
    event.key = identity.key;
    event.codepoint = identity.codepoint;
    event.shifted_codepoint = identity.shifted_codepoint;
    event.base_layout_codepoint = identity.base_layout_codepoint;
    event.action = BOXRIG_KEY_RELEASE;
    event.modifiers = adapter->modifiers;
    return queue_key(adapter, &event);
}

static uint32_t mouse_buttons(WPARAM parameter)
{
    uint32_t buttons = 0;
    WORD state = LOWORD(parameter);

    if ((state & MK_LBUTTON) != 0) {
        buttons |= BOXRIG_MOUSE_BUTTON_MASK_LEFT;
    }
    if ((state & MK_MBUTTON) != 0) {
        buttons |= BOXRIG_MOUSE_BUTTON_MASK_MIDDLE;
    }
    if ((state & MK_RBUTTON) != 0) {
        buttons |= BOXRIG_MOUSE_BUTTON_MASK_RIGHT;
    }
    if ((state & MK_XBUTTON1) != 0) {
        buttons |= BOXRIG_MOUSE_BUTTON_MASK_8;
    }
    if ((state & MK_XBUTTON2) != 0) {
        buttons |= BOXRIG_MOUSE_BUTTON_MASK_9;
    }
    return buttons;
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
    default:
        return 0;
    }
}

static int route_mouse_transition(
    boxrig_win32_input_adapter *adapter,
    const boxrig_mouse_event *event,
    uint32_t button)
{
    boxrig_ui_hit_test_result hit;
    boxrig_input_mouse_route route;
    bool captured = button != 0
                        ? (adapter->application_buttons & button) != 0
                        : adapter->application_buttons != 0;

    if (adapter->mouse_router == NULL) {
        return 0;
    }
    if (event->action == BOXRIG_MOUSE_RELEASE && !captured) {
        return 0;
    }
    if (adapter->mouse_view == NULL ||
        event->x > INT64_MAX / BOXRIG_UI_FIXED_ONE ||
        event->x < INT64_MIN / BOXRIG_UI_FIXED_ONE ||
        event->y > INT64_MAX / BOXRIG_UI_FIXED_ONE ||
        event->y < INT64_MIN / BOXRIG_UI_FIXED_ONE ||
        boxrig_ui_view_hit_test_with_composition(
            adapter->mouse_view,
            adapter->composition,
            event->x * BOXRIG_UI_FIXED_ONE,
            event->y * BOXRIG_UI_FIXED_ONE,
            &hit) != BOXRIG_UI_OK) {
        return -1;
    }
    route = adapter->mouse_router(
        event,
        &hit,
        adapter->mouse_router_context);
    if (route != BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL &&
        route != BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION &&
        route != BOXRIG_INPUT_MOUSE_ROUTE_ERROR) {
        return -1;
    }
    if (route == BOXRIG_INPUT_MOUSE_ROUTE_ERROR) {
        return -1;
    }
    if (event->action == BOXRIG_MOUSE_RELEASE && captured) {
        adapter->application_buttons &= ~button;
        return 1;
    }
    if (captured) {
        return 1;
    }
    if (route == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION) {
        if (event->action == BOXRIG_MOUSE_PRESS) {
            adapter->application_buttons |= button;
        }
        return 1;
    }
    return 0;
}

static uint32_t mouse_modifiers(
    const boxrig_win32_input_adapter *adapter,
    WPARAM parameter)
{
    uint32_t modifiers = adapter->modifiers &
                         ~(BOXRIG_MODIFIER_SHIFT |
                           BOXRIG_MODIFIER_CONTROL);
    WORD state = LOWORD(parameter);

    if ((state & MK_SHIFT) != 0) {
        modifiers |= BOXRIG_MODIFIER_SHIFT;
    }
    if ((state & MK_CONTROL) != 0) {
        modifiers |= BOXRIG_MODIFIER_CONTROL;
    }
    return modifiers;
}

static void translate_coordinates(
    boxrig_win32_input_adapter *adapter,
    int x,
    int y,
    int64_t *local_x_out,
    int64_t *local_y_out,
    size_t *row_out,
    size_t *column_out)
{
    size_t rows;
    size_t columns;
    int64_t local_x = (int64_t)x - adapter->viewport_x;
    int64_t local_y = (int64_t)y - adapter->viewport_y;
    size_t row =
        local_y < 0 ? 0 : (size_t)local_y / adapter->cell_height;
    size_t column =
        local_x < 0 ? 0 : (size_t)local_x / adapter->cell_width;

    boxrig_terminal_size(adapter->terminal, &rows, &columns);
    if (row >= rows) {
        row = rows - 1;
    }
    if (column >= columns) {
        column = columns - 1;
    }
    *local_x_out = local_x;
    *local_y_out = local_y;
    *row_out = row;
    *column_out = column;
}

static boxrig_mouse_button message_button(UINT message, WPARAM parameter)
{
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        return BOXRIG_MOUSE_BUTTON_LEFT;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
        return BOXRIG_MOUSE_BUTTON_MIDDLE;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
        return BOXRIG_MOUSE_BUTTON_RIGHT;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        return GET_XBUTTON_WPARAM(parameter) == XBUTTON1
                   ? BOXRIG_MOUSE_BUTTON_8
                   : BOXRIG_MOUSE_BUTTON_9;
    default:
        return BOXRIG_MOUSE_BUTTON_NONE;
    }
}

static bool release_message(UINT message)
{
    return message == WM_LBUTTONUP || message == WM_MBUTTONUP ||
           message == WM_RBUTTONUP || message == WM_XBUTTONUP;
}

static bool double_click_message(UINT message)
{
    return message == WM_LBUTTONDBLCLK ||
           message == WM_MBUTTONDBLCLK ||
           message == WM_RBUTTONDBLCLK ||
           message == WM_XBUTTONDBLCLK;
}

static bool inside_double_click_rectangle(
    const boxrig_win32_input_adapter *adapter,
    int x,
    int y)
{
    int width = GetSystemMetrics(SM_CXDOUBLECLK);
    int height = GetSystemMetrics(SM_CYDOUBLECLK);
    int64_t x_distance = (int64_t)x - adapter->last_click_x;
    int64_t y_distance = (int64_t)y - adapter->last_click_y;

    if (x_distance < 0) {
        x_distance = -x_distance;
    }
    if (y_distance < 0) {
        y_distance = -y_distance;
    }
    return width > 0 && height > 0 && x_distance * 2 < width &&
           y_distance * 2 < height;
}

static uint8_t press_click_count(
    boxrig_win32_input_adapter *adapter,
    UINT message,
    boxrig_mouse_button button,
    int x,
    int y)
{
    DWORD time = (DWORD)GetMessageTime();
    DWORD elapsed = time - adapter->last_click_time;
    uint8_t count = 1;

    if (double_click_message(message)) {
        count = 2;
    } else if (adapter->last_click_valid &&
               adapter->last_click_count == 2 &&
               adapter->last_click_button == button &&
               elapsed <= GetDoubleClickTime() &&
               inside_double_click_rectangle(adapter, x, y)) {
        count = 3;
    }
    adapter->last_click_time = time;
    adapter->last_click_button = button;
    adapter->last_click_x = x;
    adapter->last_click_y = y;
    adapter->last_click_count = count;
    adapter->last_click_valid = true;
    return count;
}

static int process_mouse_button(
    boxrig_win32_input_adapter *adapter,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter)
{
    boxrig_mouse_event event;
    int x = GET_X_LPARAM(long_parameter);
    int y = GET_Y_LPARAM(long_parameter);
    int route;

    memset(&event, 0, sizeof(event));
    event.action = release_message(message) ? BOXRIG_MOUSE_RELEASE
                                            : BOXRIG_MOUSE_PRESS;
    event.button = message_button(message, word_parameter);
    event.pressed_buttons = mouse_buttons(word_parameter);
    event.modifiers = mouse_modifiers(adapter, word_parameter);
    translate_coordinates(
        adapter,
        x,
        y,
        &event.x,
        &event.y,
        &event.row,
        &event.column);
    if (event.action == BOXRIG_MOUSE_PRESS) {
        event.click_count = press_click_count(
            adapter,
            message,
            event.button,
            x,
            y);
    }
    route = route_mouse_transition(
        adapter,
        &event,
        button_mask(event.button));
    if (route != 0) {
        return route < 0 ? -1 : 0;
    }
    return queue_mouse(adapter, &event);
}

static int process_mouse_motion(
    boxrig_win32_input_adapter *adapter,
    WPARAM word_parameter,
    LPARAM long_parameter)
{
    boxrig_mouse_event event;
    int route;

    memset(&event, 0, sizeof(event));
    event.action = BOXRIG_MOUSE_MOTION;
    event.pressed_buttons = mouse_buttons(word_parameter);
    event.modifiers = mouse_modifiers(adapter, word_parameter);
    translate_coordinates(
        adapter,
        GET_X_LPARAM(long_parameter),
        GET_Y_LPARAM(long_parameter),
        &event.x,
        &event.y,
        &event.row,
        &event.column);
    route = route_mouse_transition(adapter, &event, 0);
    if (route != 0) {
        return route < 0 ? -1 : 0;
    }
    return queue_mouse(adapter, &event);
}

static int retry_wheel(boxrig_win32_input_adapter *adapter)
{
    while (adapter->vertical_wheel_delta >= WHEEL_DELTA ||
           adapter->vertical_wheel_delta <= -WHEEL_DELTA ||
           adapter->horizontal_wheel_delta >= WHEEL_DELTA ||
           adapter->horizontal_wheel_delta <= -WHEEL_DELTA) {
        boxrig_mouse_event event = adapter->wheel_event;
        int *delta;

        if (adapter->vertical_wheel_delta >= WHEEL_DELTA ||
            adapter->vertical_wheel_delta <= -WHEEL_DELTA) {
            delta = &adapter->vertical_wheel_delta;
            event.button = *delta > 0 ? BOXRIG_MOUSE_BUTTON_WHEEL_UP
                                      : BOXRIG_MOUSE_BUTTON_WHEEL_DOWN;
        } else {
            delta = &adapter->horizontal_wheel_delta;
            event.button = *delta > 0 ? BOXRIG_MOUSE_BUTTON_WHEEL_RIGHT
                                      : BOXRIG_MOUSE_BUTTON_WHEEL_LEFT;
        }
        {
            int route = route_mouse_transition(adapter, &event, 0);

            if (route < 0 || (route == 0 && queue_mouse(adapter, &event) == -1)) {
                return -1;
            }
        }
        *delta += *delta > 0 ? -WHEEL_DELTA : WHEEL_DELTA;
        if (adapter->pending.kind != PENDING_INPUT_NONE) {
            return 0;
        }
    }
    return 0;
}

static int process_mouse_wheel(
    boxrig_win32_input_adapter *adapter,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter)
{
    POINT point;
    int delta = GET_WHEEL_DELTA_WPARAM(word_parameter);

    point.x = GET_X_LPARAM(long_parameter);
    point.y = GET_Y_LPARAM(long_parameter);
    if (!ScreenToClient(adapter->window, &point)) {
        return -1;
    }
    memset(&adapter->wheel_event, 0, sizeof(adapter->wheel_event));
    adapter->wheel_event.action = BOXRIG_MOUSE_PRESS;
    adapter->wheel_event.pressed_buttons = mouse_buttons(word_parameter);
    adapter->wheel_event.modifiers = mouse_modifiers(
        adapter,
        word_parameter);
    translate_coordinates(
        adapter,
        point.x,
        point.y,
        &adapter->wheel_event.x,
        &adapter->wheel_event.y,
        &adapter->wheel_event.row,
        &adapter->wheel_event.column);
    if (message == WM_MOUSEWHEEL) {
        adapter->vertical_wheel_delta += delta;
    } else {
        adapter->horizontal_wheel_delta += delta;
    }
    return retry_wheel(adapter);
}

boxrig_composition *boxrig_win32_input_adapter_composition(
    boxrig_win32_input_adapter *adapter)
{
    return adapter == NULL ? NULL : adapter->composition;
}

int boxrig_win32_input_adapter_composition_start(
    boxrig_win32_input_adapter *adapter)
{
    boxrig_composition_event event;
    boxrig_ui_editable_reconversion_state state =
        BOXRIG_UI_EDITABLE_RECONVERSION_NONE;

    if (adapter == NULL || adapter->composition == NULL ||
        synchronize_reconversion_session(adapter) == -1) {
        return -1;
    }
    if (adapter->editable_document != NULL) {
        state = boxrig_ui_editable_document_reconversion_state(
            adapter->editable_document);
    }
    if (state == BOXRIG_UI_EDITABLE_RECONVERSION_AWAITING_COMPLETION) {
        return -1;
    }
    adapter->discard_focus_loss_ime_result = false;
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.caret_visible = true;
    if (boxrig_composition_apply_event(adapter->composition, &event) !=
        BOXRIG_COMPOSITION_OK) {
        return -1;
    }
    clear_deferred_imm_preedit(&adapter->deferred_preedit);
    reset_reconversion_characters(adapter);
    adapter->ime_high_surrogate = 0;
    invalidate_ime_placement(adapter);
    adapter->native_composing = true;
    adapter->composition_updated_since_key_release = true;
    return 0;
}

int boxrig_win32_input_adapter_composition_update_utf16(
    boxrig_win32_input_adapter *adapter,
    const WCHAR *text,
    size_t text_length,
    const BYTE *attributes,
    size_t attribute_count,
    const DWORD *clauses,
    size_t clause_count,
    DWORD caret)
{
    decoded_utf16_text decoded;
    boxrig_composition_segment *segments = NULL;
    boxrig_composition_event event;
    size_t segment_count = 0;
    size_t selection_offset = 0;
    size_t selection_end = 0;
    size_t unit = 0;
    bool selection_found = false;
    bool caret_visible = caret != UINT32_MAX;
    int result = -1;

    memset(&decoded, 0, sizeof(decoded));
    if (adapter == NULL || adapter->composition == NULL ||
        (attributes == NULL && attribute_count != 0) ||
        (attributes != NULL && attribute_count != text_length) ||
        decode_utf16_text(text, text_length, &decoded) == -1 ||
        !clauses_are_valid(&decoded, clauses, clause_count) ||
        (caret_visible &&
         (caret > text_length || decoded.unit_offsets[caret] == SIZE_MAX))) {
        goto cleanup;
    }
    if (text_length != 0) {
        segments = calloc(text_length, sizeof(*segments));
        if (segments == NULL) {
            goto cleanup;
        }
    }
    while (unit < text_length) {
        size_t next_unit = unit + 1;
        uint32_t feedback;

        if (text[unit] >= 0xd800 && text[unit] <= 0xdbff) {
            next_unit++;
            if (attributes != NULL &&
                attributes[unit] != attributes[unit + 1]) {
                goto cleanup;
            }
        }
        feedback = translate_imm_attribute(
            attributes == NULL ? ATTR_INPUT : attributes[unit]);
        if (segment_count == 0 ||
            feedback != segments[segment_count - 1].feedback ||
            is_clause_boundary(clauses, clause_count, unit)) {
            segments[segment_count].text_offset =
                decoded.unit_offsets[unit];
            segments[segment_count].text_length =
                decoded.unit_offsets[next_unit] -
                decoded.unit_offsets[unit];
            segments[segment_count].feedback = feedback;
            segment_count++;
        } else {
            segments[segment_count - 1].text_length =
                decoded.unit_offsets[next_unit] -
                segments[segment_count - 1].text_offset;
        }
        if ((feedback & BOXRIG_COMPOSITION_FEEDBACK_TARGET) != 0) {
            if (!selection_found) {
                selection_offset = decoded.unit_offsets[unit];
                selection_found = true;
            }
            selection_end = decoded.unit_offsets[next_unit];
        }
        unit = next_unit;
    }
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = decoded.text;
    event.text_length = decoded.text_length;
    event.segments = segments;
    event.segment_count = segment_count;
    event.selection_offset = selection_offset;
    event.selection_length = selection_found
                                 ? selection_end - selection_offset
                                 : 0;
    event.caret_offset = caret_visible ? decoded.unit_offsets[caret] : 0;
    event.caret_visible = caret_visible;
    if (boxrig_composition_apply_event(adapter->composition, &event) !=
        BOXRIG_COMPOSITION_OK) {
        goto cleanup;
    }
    if (!adapter->native_composing) {
        invalidate_ime_placement(adapter);
    }
    adapter->native_composing = true;
    adapter->composition_updated_since_key_release = true;
    result = 0;

cleanup:
    free(segments);
    decoded_utf16_text_destroy(&decoded);
    return result;
}

int boxrig_win32_input_adapter_composition_complete(
    boxrig_win32_input_adapter *adapter)
{
    boxrig_ui_editable_reconversion_state state;

    if (adapter == NULL || adapter->composition == NULL ||
        synchronize_reconversion_session(adapter) == -1 ||
        apply_empty_composition_event(
            adapter,
            BOXRIG_COMPOSITION_PREEDIT_COMPLETE) == -1) {
        return -1;
    }
    clear_deferred_imm_preedit(&adapter->deferred_preedit);
    invalidate_ime_placement(adapter);
    adapter->native_composing = false;
    adapter->ime_high_surrogate = 0;
    reset_reconversion_characters(adapter);
    if (!adapter->reconversion_sink_active) {
        return 0;
    }
    state = boxrig_ui_editable_document_reconversion_state(
        adapter->editable_document);
    if (state != BOXRIG_UI_EDITABLE_RECONVERSION_AWAITING_COMPLETION &&
        cancel_reconversion_session(adapter, false, false) == -1) {
        return -1;
    }
    return 0;
}

int boxrig_win32_input_adapter_composition_cancel(
    boxrig_win32_input_adapter *adapter)
{
    boxrig_ui_editable_reconversion_state state;

    if (adapter == NULL || adapter->composition == NULL ||
        synchronize_reconversion_session(adapter) == -1 ||
        apply_empty_composition_event(
            adapter,
            BOXRIG_COMPOSITION_CANCEL) == -1) {
        return -1;
    }
    clear_deferred_imm_preedit(&adapter->deferred_preedit);
    invalidate_ime_placement(adapter);
    adapter->native_composing = false;
    adapter->ime_high_surrogate = 0;
    reset_reconversion_characters(adapter);
    if (!adapter->reconversion_sink_active) {
        return 0;
    }
    state = boxrig_ui_editable_document_reconversion_state(
        adapter->editable_document);
    if (state != BOXRIG_UI_EDITABLE_RECONVERSION_AWAITING_COMPLETION &&
        cancel_reconversion_session(adapter, false, false) == -1) {
        return -1;
    }
    return 0;
}

static boxrig_ui_status notify_animation_input(boxrig_win32_input_adapter *adapter)
{
    if (adapter->animation_failure != BOXRIG_UI_OK) return adapter->animation_failure;
    boxrig_ui_status status = adapter->animation_view == NULL ? BOXRIG_UI_OK :
        boxrig_ui_view_notify_local_input(adapter->animation_view);
    boxrig_win32_input_adapter_report_animation_failure(adapter, status);
    return status;
}

int boxrig_win32_input_adapter_composition_commit_utf16(
    boxrig_win32_input_adapter *adapter,
    const WCHAR *text,
    size_t text_length)
{
    decoded_utf16_text decoded;
    boxrig_composition_event event;
    boxrig_composition_status status;

    memset(&decoded, 0, sizeof(decoded));
    if (adapter == NULL || adapter->composition == NULL ||
        synchronize_reconversion_session(adapter) == -1) {
        return -1;
    }
    adapter->ime_high_surrogate = 0;
    if (decode_utf16_text(text, text_length, &decoded) == -1) {
        return -1;
    }
    if (notify_animation_input(adapter) != BOXRIG_UI_OK) {
        decoded_utf16_text_destroy(&decoded);
        return -1;
    }
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_COMMIT;
    event.text = decoded.text;
    event.text_length = decoded.text_length;
    status = boxrig_composition_apply_event(adapter->composition, &event);
    decoded_utf16_text_destroy(&decoded);
    if (status != BOXRIG_COMPOSITION_OK &&
        status != BOXRIG_COMPOSITION_WOULD_BLOCK) {
        return -1;
    }
    invalidate_ime_placement(adapter);
    adapter->native_composing = false;
    return 0;
}

static boxrig_win32_ime_placement_status synchronize_candidate(
    boxrig_win32_input_adapter *adapter,
    bool force)
{
    boxrig_composition_view view;
    HIMC input_context;
    CANDIDATEFORM candidate;
    COMPOSITIONFORM composition;
    int64_t spot_x;
    int64_t spot_y;
    int64_t exclude_left;
    int64_t exclude_top;
    int64_t exclude_right;
    int64_t exclude_bottom;
    bool composition_rejected = false;
    bool candidate_rejected = false;

    if (adapter == NULL || adapter->composition == NULL ||
        boxrig_composition_get_view(adapter->composition, &view) !=
            BOXRIG_COMPOSITION_OK) {
        return BOXRIG_WIN32_IME_PLACEMENT_ERROR;
    }
    input_context = ImmGetContext(adapter->window);
    if (input_context == NULL) {
        invalidate_ime_placement(adapter);
        return BOXRIG_WIN32_IME_PLACEMENT_OK;
    }
    if (adapter->placement_input_context != input_context) {
        adapter->synchronized_composition_form_valid = false;
        adapter->synchronized_candidate_form_valid = false;
        adapter->placement_input_context = input_context;
    }
    if (adapter->native_composing && !view.active) {
        (void)ImmNotifyIME(
            input_context,
            NI_COMPOSITIONSTR,
            CPS_CANCEL,
            0);
        adapter->native_composing = false;
        invalidate_ime_placement(adapter);
    }
    if (!view.candidate.visible) {
        invalidate_ime_placement(adapter);
        (void)ImmReleaseContext(adapter->window, input_context);
        return BOXRIG_WIN32_IME_PLACEMENT_OK;
    }
    spot_x = (int64_t)view.candidate.spot_x + adapter->viewport_x;
    spot_y = (int64_t)view.candidate.spot_y + adapter->viewport_y;
    exclude_left =
        (int64_t)view.candidate.exclude_x + adapter->viewport_x;
    exclude_top =
        (int64_t)view.candidate.exclude_y + adapter->viewport_y;
    exclude_right = exclude_left + view.candidate.exclude_width;
    exclude_bottom = exclude_top + view.candidate.exclude_height;
    if (spot_x < LONG_MIN || spot_x > LONG_MAX || spot_y < LONG_MIN ||
        spot_y > LONG_MAX || exclude_left < LONG_MIN ||
        exclude_left > LONG_MAX || exclude_top < LONG_MIN ||
        exclude_top > LONG_MAX || exclude_right < LONG_MIN ||
        exclude_right > LONG_MAX || exclude_bottom < LONG_MIN ||
        exclude_bottom > LONG_MAX) {
        invalidate_ime_placement(adapter);
        (void)ImmReleaseContext(adapter->window, input_context);
        return BOXRIG_WIN32_IME_PLACEMENT_ERROR;
    }
    memset(&composition, 0, sizeof(composition));
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos.x = (LONG)spot_x;
    composition.ptCurrentPos.y = (LONG)spot_y;
    memset(&candidate, 0, sizeof(candidate));
    candidate.dwStyle = CFS_EXCLUDE;
    candidate.ptCurrentPos = composition.ptCurrentPos;
    candidate.rcArea.left = (LONG)exclude_left;
    candidate.rcArea.top = (LONG)exclude_top;
    candidate.rcArea.right = (LONG)exclude_right;
    candidate.rcArea.bottom = (LONG)exclude_bottom;
    if (force || !adapter->synchronized_composition_form_valid ||
        !composition_forms_equal(
            &adapter->synchronized_composition_form,
            &composition)) {
        if (ImmSetCompositionWindow(input_context, &composition)) {
            adapter->synchronized_composition_form = composition;
            adapter->synchronized_composition_form_valid = true;
        } else {
            adapter->synchronized_composition_form_valid = false;
            composition_rejected = true;
        }
    }
    if (force || !adapter->synchronized_candidate_form_valid ||
        !candidate_forms_equal(
            &adapter->synchronized_candidate_form,
            &candidate)) {
        if (ImmSetCandidateWindow(input_context, &candidate)) {
            adapter->synchronized_candidate_form = candidate;
            adapter->synchronized_candidate_form_valid = true;
        } else {
            adapter->synchronized_candidate_form_valid = false;
            candidate_rejected = true;
        }
    }
    (void)ImmReleaseContext(adapter->window, input_context);
    if (composition_rejected && candidate_rejected) {
        return BOXRIG_WIN32_IME_PLACEMENT_BOTH_REJECTED;
    }
    if (composition_rejected) {
        return BOXRIG_WIN32_IME_PLACEMENT_COMPOSITION_REJECTED;
    }
    if (candidate_rejected) {
        return BOXRIG_WIN32_IME_PLACEMENT_CANDIDATE_REJECTED;
    }
    return BOXRIG_WIN32_IME_PLACEMENT_OK;
}

boxrig_win32_ime_placement_status
boxrig_win32_input_adapter_synchronize_candidate(
    boxrig_win32_input_adapter *adapter)
{
    return synchronize_candidate(adapter, false);
}

boxrig_win32_ime_placement_status
boxrig_win32_input_adapter_resynchronize_candidate(
    boxrig_win32_input_adapter *adapter)
{
    return synchronize_candidate(adapter, true);
}

static int read_imm_data(
    HIMC input_context,
    DWORD index,
    size_t maximum_bytes,
    void **data_out,
    size_t *length_out)
{
    LONG required;
    void *data;
    LONG received;

    *data_out = NULL;
    *length_out = 0;
    required = ImmGetCompositionStringW(input_context, index, NULL, 0);
    if (required == IMM_ERROR_NODATA) {
        return 0;
    }
    if (required < 0 || (size_t)required > maximum_bytes) {
        return -1;
    }
    if (required == 0) {
        return 0;
    }
    data = malloc((size_t)required);
    if (data == NULL) {
        return -1;
    }
    received = ImmGetCompositionStringW(
        input_context,
        index,
        data,
        (DWORD)required);
    if (received != required) {
        free(data);
        return -1;
    }
    *data_out = data;
    *length_out = (size_t)received;
    return 0;
}

static int process_ime_composition(
    boxrig_win32_input_adapter *adapter,
    LPARAM flags)
{
    boxrig_composition_limits limits = boxrig_composition_limits_default();
    HIMC input_context;
    void *result_text = NULL;
    size_t result_bytes = 0;
    void *preedit_text = NULL;
    size_t preedit_bytes = 0;
    void *attributes = NULL;
    size_t attribute_bytes = 0;
    void *clauses = NULL;
    size_t clause_bytes = 0;
    LONG caret;
    const LPARAM preedit_flags =
        GCS_COMPSTR | GCS_COMPATTR | GCS_COMPCLAUSE | GCS_CURSORPOS;
    int result = -1;

    if (adapter->discard_focus_loss_ime_result) {
        return boxrig_win32_input_adapter_composition_cancel(adapter);
    }
    if (flags == 0) {
        return boxrig_win32_input_adapter_composition_cancel(adapter);
    }
    input_context = ImmGetContext(adapter->window);
    if (input_context == NULL) {
        return 0;
    }
    if ((flags & GCS_RESULTSTR) != 0) {
        reset_reconversion_characters(adapter);
        if (read_imm_data(
                input_context,
                GCS_RESULTSTR,
                limits.maximum_text_bytes * sizeof(WCHAR),
                &result_text,
                &result_bytes) == -1 ||
            result_bytes % sizeof(WCHAR) != 0 ||
            boxrig_win32_input_adapter_composition_commit_utf16(
                adapter,
                (const WCHAR *)result_text,
                result_bytes / sizeof(WCHAR)) == -1) {
            goto cleanup;
        }
        if (adapter->reconversion_sink_active &&
            boxrig_ui_editable_document_reconversion_state(
                adapter->editable_document) ==
                BOXRIG_UI_EDITABLE_RECONVERSION_AWAITING_COMPLETION) {
            result = 0;
            goto cleanup;
        }
    }
    if ((flags & preedit_flags) != 0) {
        boxrig_composition_view view;

        if (read_imm_data(
                input_context,
                GCS_COMPSTR,
                limits.maximum_text_bytes * sizeof(WCHAR),
                &preedit_text,
                &preedit_bytes) == -1 ||
            preedit_bytes % sizeof(WCHAR) != 0 ||
            read_imm_data(
                input_context,
                GCS_COMPATTR,
                limits.maximum_text_bytes,
                &attributes,
                &attribute_bytes) == -1 ||
            read_imm_data(
                input_context,
                GCS_COMPCLAUSE,
                (limits.maximum_segments + 1) * sizeof(DWORD),
                &clauses,
                &clause_bytes) == -1 ||
            clause_bytes % sizeof(DWORD) != 0) {
            goto cleanup;
        }
        caret = ImmGetCompositionStringW(
            input_context,
            GCS_CURSORPOS,
            NULL,
            0);
        if (boxrig_composition_get_view(adapter->composition, &view) !=
            BOXRIG_COMPOSITION_OK) {
            goto cleanup;
        }
        if (view.commit_pending) {
            /* Keep the remainder of this native snapshot behind the commit. */
            defer_imm_preedit(
                adapter,
                preedit_text,
                preedit_bytes / sizeof(WCHAR),
                attributes,
                attribute_bytes,
                clauses,
                clause_bytes / sizeof(DWORD),
                caret < 0 ? UINT32_MAX : (DWORD)caret);
            preedit_text = NULL;
            attributes = NULL;
            clauses = NULL;
        } else if (boxrig_win32_input_adapter_composition_update_utf16(
                       adapter,
                       (const WCHAR *)preedit_text,
                       preedit_bytes / sizeof(WCHAR),
                       (const BYTE *)attributes,
                       attribute_bytes,
                       (const DWORD *)clauses,
                       clause_bytes / sizeof(DWORD),
                       caret < 0 ? UINT32_MAX : (DWORD)caret) == -1) {
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    free(result_text);
    free(preedit_text);
    free(attributes);
    free(clauses);
    (void)ImmReleaseContext(adapter->window, input_context);
    if (result == -1 && adapter->reconversion_sink_active) {
        (void)cancel_reconversion_session(adapter, true, true);
    }
    return result;
}

static int recover_missing_empty_composition(
    boxrig_win32_input_adapter *adapter,
    UINT virtual_key,
    bool composition_updated)
{
    boxrig_composition_view view;
    HIMC input_context;
    size_t segment_index;
    bool recover = false;

    if (!adapter->native_composing || composition_updated ||
        (virtual_key != VK_BACK && virtual_key != VK_ESCAPE)) {
        return 0;
    }
    if (boxrig_composition_get_view(adapter->composition, &view) !=
        BOXRIG_COMPOSITION_OK) {
        return -1;
    }
    if (!view.active || view.text_length == 0 || view.segment_count == 0) {
        return 0;
    }
    if (virtual_key == VK_BACK) {
        /* Wine's IBus bridge can consume the final Backspace without emitting
         * the empty WM_IME_COMPOSITION snapshot. Limit this recovery to one
         * unchanged cluster so ordinary IMM deletion remains snapshot-driven.
         */
        recover = view.cluster_count == 1 && view.caret_visible &&
                  view.caret_offset == view.text_length &&
                  view.segment_count == 1;
    } else {
        /* Escape cancellation may cover multiple clauses. A converted or
         * selected segment still belongs to the native revert stage, so only
         * recover after every segment has returned to plain input feedback.
         */
        recover = true;
    }
    for (segment_index = 0;
         recover && segment_index < view.segment_count;
         segment_index++) {
        uint32_t feedback = view.segments[segment_index].feedback;

        recover =
            (feedback & BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE) != 0 &&
            (feedback & (BOXRIG_COMPOSITION_FEEDBACK_TARGET |
                         BOXRIG_COMPOSITION_FEEDBACK_CONVERTED)) == 0;
    }
    if (!recover) {
        return 0;
    }
    if (boxrig_win32_input_adapter_composition_cancel(adapter) == -1) {
        return -1;
    }
    input_context = ImmGetContext(adapter->window);
    if (input_context != NULL) {
        (void)ImmNotifyIME(
            input_context,
            NI_COMPOSITIONSTR,
            CPS_CANCEL,
            0);
        (void)ImmReleaseContext(adapter->window, input_context);
    }
    return 0;
}

int boxrig_win32_input_adapter_create(
    HWND window,
    boxrig_terminal *terminal,
    unsigned int cell_width,
    unsigned int cell_height,
    size_t paste_capacity,
    size_t paste_chunk_size,
    boxrig_win32_input_adapter **adapter_out)
{
    boxrig_win32_input_adapter *adapter;

    if (window == NULL || terminal == NULL || cell_width == 0 ||
        cell_height == 0 || paste_chunk_size == 0 || adapter_out == NULL) {
        return -1;
    }
    *adapter_out = NULL;
    adapter = calloc(1, sizeof(*adapter));
    if (adapter == NULL) {
        return -1;
    }
    adapter->window = window;
    adapter->terminal = terminal;
    adapter->cell_width = cell_width;
    adapter->cell_height = cell_height;
    adapter->paste_capacity = paste_capacity;
    adapter->paste_chunk_size = paste_chunk_size;
    adapter->modifiers = keyboard_modifier_state();
    if (boxrig_composition_create(
            terminal,
            boxrig_composition_limits_default(),
            &adapter->composition) != BOXRIG_COMPOSITION_OK ||
        boxrig_composition_set_commit_sink(
            adapter->composition,
            route_ime_commit,
            adapter) != BOXRIG_COMPOSITION_OK) {
        boxrig_composition_destroy(adapter->composition);
        free(adapter);
        return -1;
    }
    *adapter_out = adapter;
    return 0;
}

void boxrig_win32_input_adapter_set_animation_view(
    boxrig_win32_input_adapter *adapter, boxrig_ui_view *view)
{
    if (adapter != NULL) adapter->animation_view = view;
}

void boxrig_win32_input_adapter_report_animation_failure(
    boxrig_win32_input_adapter *adapter, boxrig_ui_status status)
{
    if (adapter == NULL || status == BOXRIG_UI_OK) return;
    if (adapter->animation_failure == BOXRIG_UI_OK) adapter->animation_failure = status;
    if (adapter->animation_view != NULL) {
        /* Wake the existing frame path even when the failed transition rolled back. */
        boxrig_ui_dirty dirty = {.row = 0, .row_count = 1, .column = 0, .column_count = 1};
        (void)boxrig_ui_view_invalidate(adapter->animation_view, BOXRIG_UI_DIRTY_PRESENT, dirty);
    }
}

boxrig_ui_status boxrig_win32_input_adapter_animation_status(
    const boxrig_win32_input_adapter *adapter)
{
    return adapter == NULL ? BOXRIG_UI_OK : adapter->animation_failure;
}

int boxrig_win32_input_adapter_set_editable_document(
    boxrig_win32_input_adapter *adapter,
    boxrig_ui_editable_document *document)
{
    if (adapter == NULL || adapter->reconversion_sink_active ||
        (adapter->editable_document != NULL &&
         boxrig_ui_editable_document_reconversion_state(
             adapter->editable_document) !=
             BOXRIG_UI_EDITABLE_RECONVERSION_NONE) ||
        (document != NULL &&
         boxrig_ui_editable_document_reconversion_state(document) !=
             BOXRIG_UI_EDITABLE_RECONVERSION_NONE)) {
        return -1;
    }
    adapter->editable_document = document;
    return 0;
}

boxrig_ui_status boxrig_win32_input_adapter_set_input_config(
    boxrig_win32_input_adapter *adapter, const boxrig_input_config *config)
{
    if (adapter == NULL || config == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    boxrig_composition_view composition;
    if (boxrig_composition_get_view(adapter->composition, &composition) != BOXRIG_COMPOSITION_OK) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (adapter->dispatching || adapter->pending.kind != PENDING_INPUT_NONE ||
        adapter->paste_active || composition.active || composition.commit_pending ||
        adapter->character.active || adapter->high_surrogate != 0 ||
        adapter->ime_high_surrogate != 0 || adapter->native_composing ||
        adapter->deferred_preedit.active || adapter->reconversion_sink_active) {
        return BOXRIG_UI_BUSY;
    }
    for (size_t i = 0u; i < KEY_SLOT_COUNT; i++) {
        if (adapter->pressed[i]) return BOXRIG_UI_BUSY;
    }
    boxrig_status status = boxrig_terminal_set_input_config(adapter->terminal, config);
    if (status == BOXRIG_STATUS_OK) return BOXRIG_UI_OK;
    if (status == BOXRIG_STATUS_OUT_OF_MEMORY) return BOXRIG_UI_OUT_OF_MEMORY;
    if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) return BOXRIG_UI_LIMIT_EXCEEDED;
    return BOXRIG_UI_INVALID_ARGUMENT;
}

int boxrig_win32_input_adapter_set_key_router(
    boxrig_win32_input_adapter *adapter,
    boxrig_input_key_router router,
    void *context)
{
    size_t slot;

    if (adapter == NULL || (router == NULL && context != NULL)) {
        return -1;
    }
    for (slot = 0; slot < KEY_SLOT_COUNT; slot++) {
        if (adapter->pressed[slot]) {
            return -1;
        }
    }
    adapter->key_router = router;
    adapter->key_router_context = context;
    memset(adapter->application_key, 0, sizeof(adapter->application_key));
    return 0;
}

int boxrig_win32_input_adapter_set_mouse_router(
    boxrig_win32_input_adapter *adapter,
    boxrig_ui_view *view,
    boxrig_input_mouse_router router,
    void *context)
{
    if (adapter == NULL ||
        (router == NULL && (view != NULL || context != NULL)) ||
        (router != NULL &&
         (view == NULL ||
          boxrig_ui_model_terminal(boxrig_ui_view_model(view)) !=
              adapter->terminal)) ||
        adapter->application_buttons != 0) {
        return -1;
    }
    adapter->mouse_view = view;
    adapter->mouse_router = router;
    adapter->mouse_router_context = context;
    return 0;
}

static bool ime_reconversion_available(
    const boxrig_win32_input_adapter *adapter)
{
    return adapter != NULL && adapter->editable_document != NULL &&
           boxrig_ui_editable_document_capabilities(
               adapter->editable_document).reconversion_available &&
           (ImmGetProperty(GetKeyboardLayout(0), IGP_SETCOMPSTR) &
            SCS_CAP_SETRECONVERTSTRING) != 0;
}

static boxrig_ui_status apply_reconversion_action(
    boxrig_win32_input_adapter *adapter)
{
    RECONVERTSTRING description;
    const uint16_t *context;
    DWORD required;
    BYTE *buffer = NULL;
    HIMC input_context = NULL;
    bool success = false;

    if (!ime_reconversion_available(adapter) ||
        begin_reconversion_session(adapter) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (!reconversion_description(
            adapter,
            true,
            &description,
            &context,
            &required)) {
        (void)cancel_reconversion_session(adapter, false, false);
        return BOXRIG_UI_UNAVAILABLE;
    }
    buffer = malloc(required);
    if (buffer == NULL) {
        (void)cancel_reconversion_session(adapter, false, false);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    memcpy(buffer, &description, sizeof(description));
    memcpy(
        buffer + description.dwStrOffset,
        context,
        (size_t)description.dwStrLen * sizeof(WCHAR));
    input_context = ImmGetContext(adapter->window);
    if (input_context != NULL &&
        ImmSetCompositionStringW(
            input_context,
            SCS_QUERYRECONVERTSTRING,
            buffer,
            required,
            NULL,
            0) &&
        confirm_reconversion_request(adapter, (LPARAM)buffer) != FALSE &&
        ImmSetCompositionStringW(
            input_context,
            SCS_SETRECONVERTSTRING,
            buffer,
            required,
            NULL,
            0)) {
        success = true;
    }
    if (input_context != NULL) {
        (void)ImmReleaseContext(adapter->window, input_context);
    }
    free(buffer);
    if (!success) {
        (void)cancel_reconversion_session(adapter, false, true);
        return BOXRIG_UI_UNAVAILABLE;
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_view_action_capabilities
boxrig_win32_input_adapter_view_action_capabilities(
    const boxrig_win32_input_adapter *adapter)
{
    boxrig_ui_view_action_capabilities capabilities;

    memset(&capabilities, 0, sizeof(capabilities));
    if (adapter != NULL) {
        capabilities.copy_targets =
            BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
        capabilities.paste_targets =
            BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
        capabilities.reconversion =
            ime_reconversion_available(adapter);
    }
    return capabilities;
}

boxrig_ui_status boxrig_win32_input_adapter_apply_view_action(
    boxrig_win32_input_adapter *adapter,
    boxrig_ui_view *view,
    const boxrig_ui_view_action *action)
{
    boxrig_ui_view_action_capabilities capabilities;

    if (adapter == NULL || action == NULL ||
        (action->kind != BOXRIG_UI_VIEW_ACTION_COPY &&
         action->kind != BOXRIG_UI_VIEW_ACTION_PASTE &&
         action->kind != BOXRIG_UI_VIEW_ACTION_RECONVERT) ||
        (action->kind == BOXRIG_UI_VIEW_ACTION_RECONVERT
             ? action->clipboard_target != 0
             : (action->clipboard_target !=
                    BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION &&
                action->clipboard_target !=
                    BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD))) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (synchronize_reconversion_session(adapter) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    capabilities =
        boxrig_win32_input_adapter_view_action_capabilities(adapter);
    if (!boxrig_ui_view_action_supported(&capabilities, action)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (action->kind == BOXRIG_UI_VIEW_ACTION_RECONVERT) {
        return apply_reconversion_action(adapter);
    }
    if (action->kind == BOXRIG_UI_VIEW_ACTION_COPY) {
        return copy_to_clipboard(adapter, view);
    }
    if (adapter->paste_active) {
        return BOXRIG_UI_BUSY;
    }
    return paste_from_clipboard(adapter) == 0 ? BOXRIG_UI_OK
                                               : BOXRIG_UI_UNAVAILABLE;
}

int boxrig_win32_input_adapter_set_cell_size(
    boxrig_win32_input_adapter *adapter,
    unsigned int cell_width,
    unsigned int cell_height)
{
    if (adapter == NULL || cell_width == 0 || cell_height == 0) {
        return -1;
    }
    adapter->cell_width = cell_width;
    adapter->cell_height = cell_height;
    return 0;
}

int boxrig_win32_input_adapter_set_viewport_origin(
    boxrig_win32_input_adapter *adapter,
    int x,
    int y)
{
    if (adapter == NULL) {
        return -1;
    }
    if (adapter->viewport_x != x || adapter->viewport_y != y) {
        adapter->viewport_x = x;
        adapter->viewport_y = y;
        invalidate_ime_placement(adapter);
    }
    return 0;
}

int boxrig_win32_input_adapter_pretranslate_message(
    boxrig_win32_input_adapter *adapter,
    const MSG *message)
{
    boxrig_ui_editable_reconversion_state state;

    if (adapter == NULL || message == NULL) {
        return -1;
    }
    if (message->hwnd != adapter->window ||
        (message->message != WM_KEYDOWN &&
         message->message != WM_SYSKEYDOWN) ||
        message->wParam != VK_PROCESSKEY) {
        return 0;
    }
    if (synchronize_reconversion_session(adapter) == -1) {
        return -1;
    }
    if (!adapter->reconversion_sink_active ||
        adapter->editable_document == NULL) {
        return 0;
    }
    state = boxrig_ui_editable_document_reconversion_state(
        adapter->editable_document);
    if (state != BOXRIG_UI_EDITABLE_RECONVERSION_ACTIVE ||
        ImmGetVirtualKey(adapter->window) != VK_ESCAPE) {
        return 0;
    }
    return cancel_reconversion_session(adapter, true, true) == 0 ? 1 : -1;
}

static int process_message(boxrig_win32_input_adapter *adapter, UINT message, WPARAM word_parameter,
    LPARAM long_parameter, LRESULT *result_out);

int boxrig_win32_input_adapter_process_message(boxrig_win32_input_adapter *adapter, UINT message, WPARAM word_parameter,
    LPARAM long_parameter, LRESULT *result_out)
{
    if (adapter == NULL) return -1;
    bool was_dispatching = adapter->dispatching;
    adapter->dispatching = true;
    int result = process_message(adapter, message, word_parameter, long_parameter, result_out);
    adapter->dispatching = was_dispatching;
    return result;
}

static int process_message(
    boxrig_win32_input_adapter *adapter,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter,
    LRESULT *result_out)
{
    boxrig_ui_editable_reconversion_state reconversion_state =
        BOXRIG_UI_EDITABLE_RECONVERSION_NONE;
    int status;

    if (adapter == NULL || result_out == NULL) {
        return -1;
    }
    *result_out = 0;
    if (adapter->animation_view != NULL &&
        (message == WM_KEYDOWN || message == WM_SYSKEYDOWN ||
         message == WM_IME_KEYDOWN || message == WM_CHAR || message == WM_SYSCHAR) &&
        notify_animation_input(adapter) != BOXRIG_UI_OK) {
        return -1;
    }
    if (synchronize_reconversion_session(adapter) == -1) {
        return -1;
    }
    if (adapter->editable_document != NULL) {
        reconversion_state =
            boxrig_ui_editable_document_reconversion_state(
                adapter->editable_document);
    }
    if (boxrig_win32_input_adapter_blocked(adapter)) {
        switch (message) {
        case WM_IME_ENDCOMPOSITION:
        case WM_IME_SETCONTEXT:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            break;
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_COMPOSITION:
        case WM_IME_REQUEST:
        case WM_IME_CHAR:
            return adapter->reconversion_sink_active ? 1 : 0;
        default:
            return 0;
        }
    }
    switch (message) {
    case WM_IME_STARTCOMPOSITION:
        adapter->discard_focus_loss_ime_result = false;
        return boxrig_win32_input_adapter_composition_start(adapter) == 0
                   ? 1
                   : -1;
    case WM_IME_COMPOSITION:
        return process_ime_composition(adapter, long_parameter) == 0
                   ? 1
                   : -1;
    case WM_IME_ENDCOMPOSITION: {
        boxrig_composition_view view;

        if (adapter->discard_focus_loss_ime_result) {
            invalidate_ime_placement(adapter);
            adapter->native_composing = false;
            adapter->ime_high_surrogate = 0;
            adapter->discard_focus_loss_ime_result = false;
            reset_reconversion_characters(adapter);
            return 1;
        }
        if (adapter->reconversion_sink_active &&
            reconversion_state == BOXRIG_UI_EDITABLE_RECONVERSION_ACTIVE &&
            adapter->reconversion_character_count != 0) {
            status = boxrig_win32_input_adapter_composition_commit_utf16(
                adapter,
                adapter->reconversion_characters,
                adapter->reconversion_character_count);
            reset_reconversion_characters(adapter);
            if (status == -1) {
                (void)cancel_reconversion_session(adapter, true, true);
                return -1;
            }
        }
        if (boxrig_composition_get_view(adapter->composition, &view) !=
            BOXRIG_COMPOSITION_OK) {
            return -1;
        }
        if (!view.active) {
            invalidate_ime_placement(adapter);
            adapter->native_composing = false;
            adapter->ime_high_surrogate = 0;
            return 1;
        }
        return boxrig_win32_input_adapter_composition_complete(adapter) == 0
                   ? 1
                   : -1;
    }
    case WM_IME_SETCONTEXT:
        if (word_parameter == FALSE) {
            if (adapter->reconversion_sink_active) {
                if (cancel_reconversion_session(
                        adapter,
                        true,
                        true) == -1) {
                    return -1;
                }
            } else {
                discard_native_composition_on_focus_loss(adapter);
            }
        }
        *result_out = DefWindowProcW(
            adapter->window,
            message,
            word_parameter,
            long_parameter & ~ISC_SHOWUICOMPOSITIONWINDOW);
        return 1;
    case WM_IME_REQUEST:
        if (!is_deferred_ime_edit_request(word_parameter)) {
            return 0;
        }
        if (adapter->discard_focus_loss_ime_result) {
            return 1;
        }
        *result_out = process_reconversion_request(
            adapter,
            word_parameter,
            long_parameter);
        return 1;
    case WM_IME_CHAR: {
        WCHAR character;

        if (adapter->discard_focus_loss_ime_result) {
            return 1;
        }
        if (word_parameter > 0xffffu) {
            return -1;
        }
        character = (WCHAR)word_parameter;
        if (adapter->reconversion_sink_active &&
            reconversion_state == BOXRIG_UI_EDITABLE_RECONVERSION_ACTIVE) {
            return append_reconversion_character(adapter, character) == 0
                       ? 1
                       : -1;
        }
        return process_ime_utf16_character(adapter, character) == 0
                   ? 1
                   : -1;
    }
    case WM_IME_KEYDOWN:
        /* An IME can preserve key ordering with WM_IME_KEYDOWN instead of
         * exposing the original WM_KEYDOWN. Escape cancels the application
         * transaction and must never fall through to the terminal sink. */
        if (word_parameter == VK_ESCAPE &&
            adapter->reconversion_sink_active &&
            reconversion_state == BOXRIG_UI_EDITABLE_RECONVERSION_ACTIVE) {
            return cancel_reconversion_session(adapter, true, true) == 0
                       ? 1
                       : -1;
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        status = process_key_down(
            adapter,
            (UINT)word_parameter,
            long_parameter);
        return status == 0 ? 1 : -1;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        {
            bool composition_updated =
                adapter->composition_updated_since_key_release;

            adapter->composition_updated_since_key_release = false;
            status = process_key_up(
                adapter,
                (UINT)word_parameter,
                long_parameter);
            if (status == -1 ||
                recover_missing_empty_composition(
                    adapter,
                    (UINT)word_parameter,
                    composition_updated) == -1) {
                return -1;
            }
        }
        return 1;
    case WM_CHAR:
    case WM_SYSCHAR:
        if (suppress_application_character(
                adapter,
                (wchar_t)word_parameter)) {
            return 1;
        }
        /* Functional keys were already queued by WM_KEYDOWN. The same code
         * units produced for a pending text key are Control input instead. */
        if ((word_parameter == L'\r' || word_parameter == L'\b' ||
             word_parameter == L'\t' || word_parameter == 0x1b) &&
            !adapter->character.active) {
            return 1;
        }
        if (word_parameter > 0xffff) {
            return -1;
        }
        status = process_utf16_character(
            adapter,
            (wchar_t)word_parameter);
        return status == 0 ? 1 : -1;
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
        if (adapter->character.active && adapter->character.application) {
            adapter->character.active = false;
            adapter->character.application = false;
            adapter->high_surrogate = 0;
        }
        return 1;
    case WM_UNICHAR:
        if (word_parameter == UNICODE_NOCHAR) {
            *result_out = TRUE;
            return 1;
        }
        if (word_parameter > 0x10ffffu) {
            return -1;
        }
        if (adapter->character.active && adapter->character.application) {
            adapter->character.active = false;
            adapter->character.application = false;
            adapter->high_surrogate = 0;
            return 1;
        }
        status = queue_character(adapter, (uint32_t)word_parameter);
        return status == 0 ? 1 : -1;
    case WM_SETFOCUS: {
        boxrig_focus_event event = {true};

        adapter->modifiers = keyboard_modifier_state();
        adapter->discard_focus_loss_ime_result = false;
        invalidate_ime_placement(adapter);
        if (boxrig_composition_focus_changed(
                adapter->composition,
                true) != BOXRIG_COMPOSITION_OK) {
            return -1;
        }
        return queue_focus(adapter, &event) == 0 ? 1 : -1;
    }
    case WM_KILLFOCUS: {
        boxrig_focus_event event = {false};

        if (adapter->reconversion_sink_active) {
            if (cancel_reconversion_session(
                    adapter,
                    true,
                    true) == -1) {
                return -1;
            }
        } else {
            discard_native_composition_on_focus_loss(adapter);
        }
        if (boxrig_composition_focus_changed(
                adapter->composition,
                false) != BOXRIG_COMPOSITION_OK) {
            return -1;
        }
        invalidate_ime_placement(adapter);
        adapter->native_composing = false;
        adapter->ime_high_surrogate = 0;
        status = queue_focus(adapter, &event);
        memset(adapter->pressed, 0, sizeof(adapter->pressed));
        adapter->modifiers &= BOXRIG_MODIFIER_CAPS_LOCK |
                              BOXRIG_MODIFIER_NUM_LOCK;
        adapter->application_buttons = 0;
        adapter->last_click_valid = false;
        adapter->character.active = false;
        adapter->high_surrogate = 0;
        return status == 0 ? 1 : -1;
    }
    case WM_PASTE: {
        boxrig_ui_view_action action;
        boxrig_ui_status action_status;

        action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
        action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
        action_status =
            boxrig_win32_input_adapter_apply_view_action(
                adapter,
                NULL,
                &action);
        return action_status == BOXRIG_UI_OK ? 1 : -1;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
        return process_mouse_button(
                   adapter,
                   message,
                   word_parameter,
                   long_parameter) == 0
                   ? 1
                   : -1;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        *result_out = TRUE;
        return process_mouse_button(
                   adapter,
                   message,
                   word_parameter,
                   long_parameter) == 0
                   ? 1
                   : -1;
    case WM_MOUSEMOVE:
        return process_mouse_motion(
                   adapter,
                   word_parameter,
                   long_parameter) == 0
                   ? 1
                   : -1;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        return process_mouse_wheel(
                   adapter,
                   message,
                   word_parameter,
                   long_parameter) == 0
                   ? 1
                   : -1;
    default:
        return 0;
    }
}

int boxrig_win32_input_adapter_queue_paste(
    boxrig_win32_input_adapter *adapter,
    const uint8_t *bytes,
    size_t length)
{
    char *normalized;
    size_t normalized_length;

    if (adapter == NULL || adapter->paste_active ||
        boxrig_input_text_normalize_utf8(
            bytes,
            length,
            adapter->paste_capacity,
            &normalized,
            &normalized_length) == -1) {
        return -1;
    }
    if (notify_animation_input(adapter) != BOXRIG_UI_OK) {
        free(normalized);
        return -1;
    }
    adapter->paste_active = true;
    adapter->paste_state = PASTE_STATE_BEGIN;
    adapter->paste_text = normalized;
    adapter->paste_length = normalized_length;
    adapter->paste_offset = 0;
    return boxrig_win32_input_adapter_retry(adapter);
}

int boxrig_win32_input_adapter_retry(boxrig_win32_input_adapter *adapter)
{
    boxrig_composition_status composition_status;
    boxrig_ui_editable_reconversion_state reconversion_state =
        BOXRIG_UI_EDITABLE_RECONVERSION_NONE;

    if (adapter == NULL ||
        synchronize_reconversion_session(adapter) == -1 ||
        retry_pending_input(adapter) == -1) {
        return -1;
    }
    if (adapter->pending.kind != PENDING_INPUT_NONE) {
        return 0;
    }
    if (retry_wheel(adapter) == -1 ||
        adapter->pending.kind != PENDING_INPUT_NONE) {
        return adapter->pending.kind != PENDING_INPUT_NONE ? 0 : -1;
    }
    composition_status = boxrig_composition_retry_commit(
        adapter->composition);
    if (composition_status == BOXRIG_COMPOSITION_WOULD_BLOCK) {
        return 0;
    }
    if (composition_status != BOXRIG_COMPOSITION_OK) {
        return -1;
    }
    if (synchronize_reconversion_session(adapter) == -1) {
        return -1;
    }
    if (adapter->editable_document != NULL) {
        reconversion_state =
            boxrig_ui_editable_document_reconversion_state(
                adapter->editable_document);
    }
    if (reconversion_state ==
        BOXRIG_UI_EDITABLE_RECONVERSION_AWAITING_COMPLETION) {
        clear_deferred_imm_preedit(&adapter->deferred_preedit);
        return retry_paste(adapter);
    }
    if (apply_deferred_imm_preedit(adapter) == -1) {
        return -1;
    }
    return retry_paste(adapter);
}

bool boxrig_win32_input_adapter_blocked(
    const boxrig_win32_input_adapter *adapter)
{
    boxrig_composition_view view;

    if (adapter == NULL) {
        return false;
    }
    if (adapter->pending.kind != PENDING_INPUT_NONE ||
        adapter->paste_active ||
        adapter->vertical_wheel_delta >= WHEEL_DELTA ||
        adapter->vertical_wheel_delta <= -WHEEL_DELTA ||
        adapter->horizontal_wheel_delta >= WHEEL_DELTA ||
        adapter->horizontal_wheel_delta <= -WHEEL_DELTA) {
        return true;
    }
    if (adapter->editable_document != NULL &&
        boxrig_ui_editable_document_reconversion_state(
            adapter->editable_document) ==
            BOXRIG_UI_EDITABLE_RECONVERSION_AWAITING_COMPLETION) {
        return true;
    }
    return boxrig_composition_get_view(adapter->composition, &view) ==
               BOXRIG_COMPOSITION_OK &&
           view.commit_pending;
}

void boxrig_win32_input_adapter_destroy(boxrig_win32_input_adapter *adapter)
{
    if (adapter == NULL) {
        return;
    }
    if (adapter->reconversion_sink_active) {
        (void)cancel_reconversion_session(adapter, true, true);
    } else if (adapter->native_composing) {
        HIMC input_context = ImmGetContext(adapter->window);

        if (input_context != NULL) {
            (void)ImmNotifyIME(
                input_context,
                NI_COMPOSITIONSTR,
                CPS_CANCEL,
                0);
            (void)ImmReleaseContext(adapter->window, input_context);
        }
    }
    if (adapter->composition != NULL) {
        (void)boxrig_composition_terminate(adapter->composition);
    }
    boxrig_composition_destroy(adapter->composition);
    clear_deferred_imm_preedit(&adapter->deferred_preedit);
    free(adapter->pending.key_text);
    free(adapter->paste_text);
    free(adapter->reconversion_characters);
    free(adapter);
}
