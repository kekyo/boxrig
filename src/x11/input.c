/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/x11/input.h"

#include "boxrig/ui/input_text.h"
#include "utf8.h"

#include <X11/XF86keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum {
    KEYCODE_COUNT = 256,
    INITIAL_KEY_TEXT_CAPACITY = 64,
    DEFAULT_SELECTION_TRANSFER_CHUNK_SIZE = 64 * 1024,
    DEFAULT_MAXIMUM_ACTIVE_SELECTION_TRANSFERS = 8,
    DEFAULT_MAXIMUM_SELECTION_TRANSFER_BYTES = 64 * 1024 * 1024,
    MAXIMUM_SELECTION_TARGET_COUNT = 64,
    MULTI_CLICK_INTERVAL_MILLISECONDS = 500,
    MAXIMUM_XIM_CONTEXT_BYTES = 4000
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

typedef enum paste_state {
    PASTE_STATE_BEGIN = 0,
    PASTE_STATE_CHUNK,
    PASTE_STATE_END
} paste_state;

typedef enum selection_receive_state {
    SELECTION_RECEIVE_IDLE = 0,
    SELECTION_RECEIVE_TIMESTAMP,
    SELECTION_RECEIVE_TARGETS,
    SELECTION_RECEIVE_DATA,
    SELECTION_RECEIVE_INCR
} selection_receive_state;

typedef struct clipboard_payload {
    boxrig_text_snapshot *snapshot;
    uint8_t *latin1;
    size_t latin1_length;
    size_t reference_count;
    bool string_available;
} clipboard_payload;

typedef struct clipboard_offer {
    Atom selection;
    clipboard_payload *payload;
    clipboard_payload *pending_payload;
    Time timestamp;
    bool acquisition_pending;
    bool owned;
} clipboard_offer;

typedef struct outgoing_selection_transfer {
    Window requestor;
    Atom selection;
    Atom property;
    Atom target;
    clipboard_payload *payload;
    size_t offset;
    size_t length;
    bool active;
} outgoing_selection_transfer;

typedef struct foreign_window_watch {
    Window window;
    long original_event_mask;
    long selected_event_mask;
    size_t reference_count;
    bool active;
} foreign_window_watch;

typedef struct decoded_xim_text {
    char *text;
    size_t text_length;
    size_t *offsets;
    uint32_t *feedback;
    size_t character_count;
    bool feedback_only;
    bool feedback_specified;
} decoded_xim_text;

typedef union xim_start_callback_pointer {
    int (*start)(XIM, XPointer, XPointer);
    XIMProc generic;
} xim_start_callback_pointer;

struct boxrig_x11_input_adapter {
    Display *display;
    Window client_window;
    Window window;
    boxrig_terminal *terminal;
    bool dispatching;
    unsigned int cell_width;
    unsigned int cell_height;
    int viewport_x;
    int viewport_y;
    size_t paste_capacity;
    size_t paste_chunk_size;
    XIM input_method;
    XIC input_context;
    XPoint candidate_spot;
    bool candidate_spot_valid;
    XIMCallback preedit_start_callback;
    XIMCallback preedit_done_callback;
    XIMCallback preedit_draw_callback;
    XIMCallback preedit_caret_callback;
    XIMCallback string_conversion_callback;
    XIMStringConversionText string_conversion_response;
    boxrig_ui_editable_document *editable_document;
    uint64_t document_generation;
    uint64_t commit_generation;
    bool string_conversion_supported;
    bool document_reset_pending;
    bool commit_editable;
    boxrig_composition *composition;
    char *preedit_text;
    size_t preedit_text_length;
    size_t *preedit_offsets;
    uint32_t *preedit_feedback;
    size_t preedit_character_count;
    size_t preedit_caret;
    bool preedit_caret_visible;
    bool preedit_started;
    bool resetting_preedit;
    bool escape_preedit_pending;
    bool reset_preedit_tail_pending;
    KeyCode filtered_final_backspace_keycode;
    bool filtered_final_backspace_preedit_updated;
    bool input_focused;
    bool key_had_composition;
    bool callback_failed;
    bool xkb_available;
    bool detectable_repeat;
    unsigned int alt_mask;
    unsigned int super_mask;
    unsigned int hyper_mask;
    unsigned int meta_mask;
    unsigned int caps_lock_mask;
    unsigned int num_lock_mask;
    bool pressed[KEYCODE_COUNT];
    bool composition_key[KEYCODE_COUNT];
    bool application_key[KEYCODE_COUNT];
    key_identity identities[KEYCODE_COUNT];
    boxrig_input_key_router key_router;
    void *key_router_context;
    boxrig_ui_view *mouse_view;
    boxrig_input_mouse_router mouse_router;
    void *mouse_router_context;
    uint32_t pressed_buttons;
    uint32_t application_buttons;
    Time last_click_time;
    boxrig_mouse_button last_click_button;
    size_t last_click_row;
    size_t last_click_column;
    uint8_t last_click_count;
    bool last_click_valid;
    pending_input pending;
    bool paste_active;
    paste_state paste_state;
    char *paste_text;
    size_t paste_length;
    size_t paste_offset;
    Atom clipboard;
    Atom targets;
    Atom utf8_string;
    Atom incr;
    Atom timestamp;
    Atom paste_property;
    Atom timestamp_property;
    clipboard_offer primary_offer;
    clipboard_offer clipboard_offer;
    outgoing_selection_transfer *outgoing_transfers;
    foreign_window_watch *foreign_window_watches;
    size_t maximum_active_selection_transfers;
    size_t maximum_foreign_window_watches;
    size_t maximum_selection_transfer_bytes;
    size_t selection_transfer_chunk_size;
    size_t active_selection_transfer_count;
    size_t active_selection_transfer_bytes;
    selection_receive_state selection_receive_state;
    Atom selection;
    Atom selection_target;
    Window selection_owner;
    uint8_t *selection_bytes;
    size_t selection_length;
    size_t selection_capacity;
    bool server_timestamp_pending;
    bool input_window_alive;
};

static bool repeated_release(
    boxrig_x11_input_adapter *adapter,
    const XKeyEvent *event);
static void refresh_editable_context(boxrig_x11_input_adapter *adapter);

static bool checked_array_size(size_t count, size_t element_size)
{
    return element_size == 0 || count <= SIZE_MAX / element_size;
}

static size_t selection_property_byte_limit(Display *display)
{
    long request_units = XExtendedMaxRequestSize(display);
    size_t payload_units;

    if (request_units == 0) {
        request_units = XMaxRequestSize(display);
    }
    /* Leave room for ChangeProperty and the extended-request length word. */
    if (request_units <= 8) {
        return 0;
    }
    payload_units = (size_t)request_units - 8;
    if (payload_units > (size_t)INT_MAX / 4) {
        return INT_MAX;
    }
    return payload_units * 4;
}

static bool encode_utf8_scalar(
    uint32_t codepoint,
    char output[4],
    size_t *length_out)
{
    if (codepoint <= 0x7f) {
        output[0] = (char)codepoint;
        *length_out = 1;
        return true;
    }
    if (codepoint <= 0x7ff) {
        output[0] = (char)(0xc0u | (codepoint >> 6));
        output[1] = (char)(0x80u | (codepoint & 0x3fu));
        *length_out = 2;
        return true;
    }
    if (codepoint >= 0xd800 && codepoint <= 0xdfff) {
        return false;
    }
    if (codepoint <= 0xffff) {
        output[0] = (char)(0xe0u | (codepoint >> 12));
        output[1] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
        output[2] = (char)(0x80u | (codepoint & 0x3fu));
        *length_out = 3;
        return true;
    }
    if (codepoint <= 0x10ffff) {
        output[0] = (char)(0xf0u | (codepoint >> 18));
        output[1] = (char)(0x80u | ((codepoint >> 12) & 0x3fu));
        output[2] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
        output[3] = (char)(0x80u | (codepoint & 0x3fu));
        *length_out = 4;
        return true;
    }
    return false;
}

static void clipboard_payload_retain(clipboard_payload *payload)
{
    if (payload != NULL) {
        payload->reference_count++;
    }
}

static void clipboard_payload_release(clipboard_payload *payload)
{
    if (payload == NULL) {
        return;
    }
    payload->reference_count--;
    if (payload->reference_count == 0) {
        boxrig_text_snapshot_destroy(payload->snapshot);
        free(payload->latin1);
        free(payload);
    }
}

static boxrig_ui_status create_clipboard_payload(
    boxrig_ui_view *view,
    clipboard_payload **payload_out)
{
    boxrig_text_snapshot *snapshot = NULL;
    clipboard_payload *payload;
    const uint8_t *utf8;
    size_t utf8_length;
    size_t source = 0;
    size_t destination = 0;
    boxrig_ui_status status;

    *payload_out = NULL;
    status = boxrig_ui_view_create_selection_snapshot(view, &snapshot);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    payload = calloc(1, sizeof(*payload));
    if (payload == NULL) {
        boxrig_text_snapshot_destroy(snapshot);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    payload->snapshot = snapshot;
    payload->reference_count = 1;
    utf8 = boxrig_text_snapshot_data(snapshot, &utf8_length);
    payload->string_available = true;
    if (utf8_length != 0) {
        payload->latin1 = malloc(utf8_length);
        if (payload->latin1 == NULL) {
            payload->string_available = false;
        }
    }
    while (source < utf8_length) {
        uint32_t codepoint;
        size_t decoded;

        if (!boxrig_ui_utf8_decode_scalar(
                utf8 + source,
                utf8_length - source,
                &codepoint,
                &decoded)) {
            clipboard_payload_release(payload);
            return BOXRIG_UI_INVALID_ARGUMENT;
        }
        if (codepoint > 0xff || payload->latin1 == NULL) {
            payload->string_available = false;
        } else {
            payload->latin1[destination++] = (uint8_t)codepoint;
        }
        source += decoded;
    }
    if (!payload->string_available) {
        free(payload->latin1);
        payload->latin1 = NULL;
        payload->latin1_length = 0;
    } else {
        payload->latin1_length = destination;
    }
    *payload_out = payload;
    return BOXRIG_UI_OK;
}

static const uint8_t *clipboard_payload_bytes(
    const clipboard_payload *payload,
    Atom target,
    Atom utf8_string,
    size_t *length_out)
{
    if (target == utf8_string) {
        return boxrig_text_snapshot_data(payload->snapshot, length_out);
    }
    *length_out = payload->latin1_length;
    return payload->latin1;
}

static uint32_t translate_xim_feedback(XIMFeedback feedback)
{
    uint32_t translated = BOXRIG_COMPOSITION_FEEDBACK_NONE;

    if ((feedback & XIMUnderline) != 0) {
        translated |= BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE;
    }
    if ((feedback & XIMReverse) != 0) {
        translated |= BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
                      BOXRIG_COMPOSITION_FEEDBACK_TARGET;
    }
    if ((feedback & XIMHighlight) != 0) {
        translated |= BOXRIG_COMPOSITION_FEEDBACK_HIGHLIGHT;
    }
    if ((feedback & XIMPrimary) != 0) {
        translated |= BOXRIG_COMPOSITION_FEEDBACK_PRIMARY |
                      BOXRIG_COMPOSITION_FEEDBACK_CONVERTED;
    }
    if ((feedback & XIMSecondary) != 0) {
        translated |= BOXRIG_COMPOSITION_FEEDBACK_SECONDARY |
                      BOXRIG_COMPOSITION_FEEDBACK_CONVERTED;
    }
    if ((feedback & XIMTertiary) != 0) {
        translated |= BOXRIG_COMPOSITION_FEEDBACK_TERTIARY |
                      BOXRIG_COMPOSITION_FEEDBACK_CONVERTED;
    }
    return translated;
}

static void decoded_xim_text_destroy(decoded_xim_text *decoded)
{
    free(decoded->text);
    free(decoded->offsets);
    free(decoded->feedback);
    memset(decoded, 0, sizeof(*decoded));
}

static int decode_xim_text(
    const XIMText *source,
    decoded_xim_text *decoded_out)
{
    decoded_xim_text decoded;
    mbstate_t conversion_state;
    const char *multibyte = NULL;
    bool string_is_null;
    size_t index;

    memset(&decoded, 0, sizeof(decoded));
    memset(&conversion_state, 0, sizeof(conversion_state));
    if (source == NULL) {
        decoded.offsets = calloc(1, sizeof(*decoded.offsets));
        if (decoded.offsets == NULL) {
            return -1;
        }
        *decoded_out = decoded;
        return 0;
    }
    decoded.character_count = source->length;
    decoded.feedback_specified = source->feedback != NULL;
    string_is_null = source->encoding_is_wchar
                         ? source->string.wide_char == NULL
                         : source->string.multi_byte == NULL;
    if (string_is_null) {
        if (source->length != 0 && source->feedback == NULL) {
            return -1;
        }
        decoded.feedback_only = true;
        decoded.feedback = malloc(
            (decoded.character_count == 0 ? 1 : decoded.character_count) *
            sizeof(*decoded.feedback));
        if (decoded.feedback == NULL) {
            return -1;
        }
        for (index = 0; index < decoded.character_count; index++) {
            decoded.feedback[index] =
                translate_xim_feedback(source->feedback[index]);
        }
        *decoded_out = decoded;
        return 0;
    }
    if (!checked_array_size(
            decoded.character_count + 1,
            sizeof(*decoded.offsets)) ||
        !checked_array_size(
            decoded.character_count,
            sizeof(*decoded.feedback)) ||
        decoded.character_count > SIZE_MAX / 4) {
        return -1;
    }
    decoded.text = malloc(decoded.character_count * 4 + 1);
    decoded.offsets = malloc(
        (decoded.character_count + 1) * sizeof(*decoded.offsets));
    decoded.feedback = malloc(
        (decoded.character_count == 0 ? 1 : decoded.character_count) *
        sizeof(*decoded.feedback));
    if (decoded.text == NULL || decoded.offsets == NULL ||
        decoded.feedback == NULL) {
        decoded_xim_text_destroy(&decoded);
        return -1;
    }
    multibyte = source->encoding_is_wchar
                    ? NULL
                    : source->string.multi_byte;
    for (index = 0; index < decoded.character_count; index++) {
        uint32_t codepoint;
        char encoded[4];
        size_t encoded_length;

        decoded.offsets[index] = decoded.text_length;
        if (source->encoding_is_wchar) {
            codepoint = (uint32_t)source->string.wide_char[index];
        } else {
            wchar_t character;
            size_t consumed = mbrtowc(
                &character,
                multibyte,
                MB_CUR_MAX,
                &conversion_state);

            if (consumed == (size_t)-1 || consumed == (size_t)-2) {
                decoded_xim_text_destroy(&decoded);
                return -1;
            }
            if (consumed == 0) {
                consumed = 1;
            }
            multibyte += consumed;
            codepoint = (uint32_t)character;
        }
        /* wchar_t may be unsigned. Negative signed values become out-of-range
         * uint32_t values, so the scalar check also rejects them here. */
        if (!encode_utf8_scalar(codepoint, encoded, &encoded_length)) {
            decoded_xim_text_destroy(&decoded);
            return -1;
        }
        memcpy(
            decoded.text + decoded.text_length,
            encoded,
            encoded_length);
        decoded.text_length += encoded_length;
        decoded.feedback[index] =
            source->feedback == NULL
                ? 0
                : translate_xim_feedback(source->feedback[index]);
    }
    decoded.offsets[decoded.character_count] = decoded.text_length;
    decoded.text[decoded.text_length] = '\0';
    *decoded_out = decoded;
    return 0;
}

static void clear_xim_preedit(boxrig_x11_input_adapter *adapter)
{
    free(adapter->preedit_text);
    free(adapter->preedit_offsets);
    free(adapter->preedit_feedback);
    adapter->preedit_text = NULL;
    adapter->preedit_offsets = NULL;
    adapter->preedit_feedback = NULL;
    adapter->preedit_text_length = 0;
    adapter->preedit_character_count = 0;
    adapter->preedit_caret = 0;
    adapter->preedit_caret_visible = true;
}

static void reset_xim_input_context(boxrig_x11_input_adapter *adapter)
{
    char *discarded;

    if (adapter->input_context == NULL) {
        return;
    }
    /* Xutf8ResetIC synchronously clears pending input and the native preedit.
     * Ignore its callbacks because cancellation discards the returned text. */
    adapter->resetting_preedit = true;
    discarded = Xutf8ResetIC(adapter->input_context);
    adapter->resetting_preedit = false;
    if (discarded != NULL) {
        XFree(discarded);
    }
}

static bool current_xim_context(
    boxrig_x11_input_adapter *adapter, XIM input_context)
{
    if ((XIC)input_context != adapter->input_context ||
        adapter->resetting_preedit || !adapter->input_focused ||
        adapter->document_reset_pending) return false;
    if (adapter->document_generation !=
        boxrig_ui_editable_document_input_generation(adapter->editable_document)) {
        adapter->document_reset_pending = true;
        return false;
    }
    return true;
}

static void invalidate_editable_input(boxrig_x11_input_adapter *adapter)
{
    if (adapter->editable_document == NULL) return;
    (void)boxrig_ui_editable_document_invalidate_input(adapter->editable_document);
    adapter->document_reset_pending = true;
    if (adapter->commit_editable)
        (void)boxrig_composition_cancel_pending_commit(adapter->composition);
}

static boxrig_status commit_ime_text(const char *text, size_t length, void *data)
{
    boxrig_x11_input_adapter *adapter = data;
    if (!adapter->commit_editable)
        return boxrig_terminal_send_text(adapter->terminal, text, length);
    boxrig_ui_editable_document_snapshot snapshot;
    if (!current_xim_context(adapter, (XIM)adapter->input_context) ||
        adapter->commit_generation != adapter->document_generation ||
        !boxrig_ui_editable_document_get_snapshot(adapter->editable_document, &snapshot))
        return BOXRIG_STATUS_OK;
    if (length > UINT64_MAX - snapshot.selection.offset) {
        adapter->document_reset_pending = true;
        return BOXRIG_STATUS_OK;
    }
    uint64_t after = snapshot.selection.offset + length;
    boxrig_ui_editable_edit edit = {
        snapshot.input_generation, snapshot.selection, text, length, after, after};
    boxrig_ui_status status = boxrig_ui_editable_document_apply_edit(
        adapter->editable_document, &edit);
    if (status == BOXRIG_UI_BUSY) return BOXRIG_STATUS_WOULD_BLOCK;
    if (status == BOXRIG_UI_OK)
        adapter->document_generation =
            boxrig_ui_editable_document_input_generation(adapter->editable_document);
    else
        adapter->document_reset_pending = true;
    /* Refusal consumes this native commit without transferring it to the PTY. */
    return BOXRIG_STATUS_OK;
}

static bool move_context_characters(
    const char *text, size_t length, size_t *position, int count)
{
    while (count > 0) {
        uint32_t scalar;
        size_t bytes;
        if (*position >= length || !boxrig_ui_utf8_decode_scalar(
                (const uint8_t *)text + *position, length - *position,
                &scalar, &bytes)) return false;
        *position += bytes;
        count--;
    }
    while (count < 0) {
        if (*position == 0) return false;
        do { (*position)--; }
        while (*position != 0 && ((unsigned char)text[*position] & 0xc0) == 0x80);
        count++;
    }
    return true;
}

static uint64_t position_after_deletion(uint64_t position, uint64_t first, uint64_t last)
{
    return position <= first ? position : position < last ? first : position - (last - first);
}

static void xim_string_conversion_callback(
    XIM input_context, XPointer client_data, XPointer call_data)
{
    boxrig_x11_input_adapter *adapter = (boxrig_x11_input_adapter *)client_data;
    XIMStringConversionCallbackStruct *request = (XIMStringConversionCallbackStruct *)call_data;
    if (request == NULL) return;
    /* Xlib does not initialize this output pointer before invoking a callback. */
    request->text = NULL;
    if (!current_xim_context(adapter, input_context)) return;
    bool substitute = request->operation == XIMStringConversionSubstitution;
    if (!substitute && request->operation != XIMStringConversionRetrieval) return;
    boxrig_ui_editable_document_snapshot snapshot;
    if (!boxrig_ui_editable_document_get_snapshot(adapter->editable_document, &snapshot)) return;

    XIMStringConversionText response = {0};
    if (adapter->pending.kind != PENDING_INPUT_NONE || adapter->paste_active ||
        boxrig_composition_retry_commit(adapter->composition) != BOXRIG_COMPOSITION_OK ||
        adapter->document_reset_pending ||
        boxrig_ui_editable_document_get_context(adapter->editable_document,
            MAXIMUM_XIM_CONTEXT_BYTES, &snapshot) != BOXRIG_UI_OK) goto unavailable;
    size_t first = (size_t)(snapshot.cursor - snapshot.context_offset);
    /* The API typedef is unsigned, but Xlib's contract defines a signed
     * character displacement from the cursor (Xlib section 13.5.7.3). */
    int position = request->position <= INT16_MAX ? (int)request->position :
                   (int)request->position - 65536;
    if (!move_context_characters(snapshot.context, snapshot.context_length, &first, position))
        goto unavailable;
    size_t last = first;
    int distance;
    if (request->direction == XIMForwardChar) distance = request->factor;
    else if (request->direction == XIMBackwardChar) distance = -(int)request->factor;
    else goto unavailable;
    if (!move_context_characters(snapshot.context, snapshot.context_length, &last, distance))
        goto unavailable;
    if (last < first) { size_t swap = last; last = first; first = swap; }

    response.encoding_is_wchar = True;
    response.string.wcs = malloc(((size_t)request->factor + 1) * sizeof(wchar_t));
    if (response.string.wcs == NULL) goto unavailable;
    mbstate_t state = {0};
    size_t offset = first;
    for (unsigned int index = 0; index < request->factor; index++) {
        size_t bytes = mbrtowc(&response.string.wcs[index],
                              snapshot.context + offset, last - offset, &state);
        if (bytes == 0 || bytes == (size_t)-1 || bytes == (size_t)-2)
            goto unavailable;
        offset += bytes;
    }
    if (offset != last) goto unavailable;
    response.length = request->factor;
    response.string.wcs[response.length] = L'\0';
    /* All reply memory is allocated before the synchronous owner can accept. */
    if (substitute && last != first) {
        uint64_t absolute_first = snapshot.context_offset + first;
        uint64_t absolute_last = snapshot.context_offset + last;
        boxrig_ui_editable_edit edit = {
            snapshot.input_generation, {absolute_first, last - first}, "", 0,
            position_after_deletion(snapshot.cursor, absolute_first, absolute_last),
            position_after_deletion(snapshot.anchor, absolute_first, absolute_last)};
        if (boxrig_ui_editable_document_apply_edit(adapter->editable_document, &edit) != BOXRIG_UI_OK)
            goto unavailable;
        adapter->document_generation =
            boxrig_ui_editable_document_input_generation(adapter->editable_document);
    }
    /* Xlib transfers only the string/feedback allocations to the input method
     * (section 13.5.7.3). Keep the borrowed descriptor alive until the next
     * callback; never free or reuse the transferred character storage. */
    adapter->string_conversion_response = response;
    request->text = &adapter->string_conversion_response;
    return;

unavailable:
    free(response.string.wcs);
    if (substitute) adapter->document_reset_pending = true;
}

static bool composition_view_is_unconverted_preedit(
    const boxrig_composition_view *view)
{
    size_t index;

    if (!view->active || view->text_length == 0) {
        return false;
    }
    for (index = 0; index < view->segment_count; index++) {
        if ((view->segments[index].feedback &
             (BOXRIG_COMPOSITION_FEEDBACK_CONVERTED |
              BOXRIG_COMPOSITION_FEEDBACK_TARGET)) != 0) {
            return false;
        }
    }
    return true;
}

static bool composition_view_is_final_unconverted_preedit(
    const boxrig_composition_view *view)
{
    return composition_view_is_unconverted_preedit(view) &&
           view->cluster_count == 1 && view->segment_count == 1 &&
           view->caret_visible && view->caret_offset == view->text_length;
}

static void note_xim_preedit_update(boxrig_x11_input_adapter *adapter)
{
    if (adapter->filtered_final_backspace_keycode != 0) {
        adapter->filtered_final_backspace_preedit_updated = true;
    }
}

static bool xim_text_has_replacement(const XIMText *text)
{
    if (text == NULL || text->length == 0) {
        return false;
    }
    return text->encoding_is_wchar
               ? text->string.wide_char != NULL
               : text->string.multi_byte != NULL;
}

static int apply_current_xim_preedit(
    boxrig_x11_input_adapter *adapter,
    const char *text,
    size_t text_length,
    const size_t *offsets,
    const uint32_t *feedback,
    size_t character_count,
    size_t caret,
    bool caret_visible)
{
    boxrig_composition_event event;
    boxrig_composition_segment *segments = NULL;
    size_t selection_offset = 0;
    size_t selection_end = 0;
    size_t segment_count = 0;
    size_t index;
    bool selection_found = false;
    boxrig_composition_status status;

    if (caret > character_count || offsets == NULL ||
        (character_count != 0 && (text == NULL || feedback == NULL))) {
        return -1;
    }
    if (character_count != 0) {
        segments = calloc(character_count, sizeof(*segments));
        if (segments == NULL) {
            return -1;
        }
        for (index = 0; index < character_count; index++) {
            size_t start = offsets[index];
            size_t end = offsets[index + 1];

            if (index == 0 || feedback[index] != feedback[index - 1]) {
                segments[segment_count].text_offset = start;
                segments[segment_count].text_length = end - start;
                segments[segment_count].feedback = feedback[index];
                segment_count++;
            } else {
                segments[segment_count - 1].text_length =
                    end - segments[segment_count - 1].text_offset;
            }
            if ((feedback[index] & BOXRIG_COMPOSITION_FEEDBACK_TARGET) != 0) {
                if (!selection_found) {
                    selection_offset = start;
                    selection_found = true;
                }
                selection_end = end;
            }
        }
    }
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = text;
    event.text_length = text_length;
    event.segments = segments;
    event.segment_count = segment_count;
    event.selection_offset = selection_offset;
    event.selection_length = selection_found
                                 ? selection_end - selection_offset
                                 : 0;
    event.caret_offset = offsets[caret];
    event.caret_visible = caret_visible;
    status = boxrig_composition_apply_event(adapter->composition, &event);
    free(segments);
    return status == BOXRIG_COMPOSITION_OK ? 0 : -1;
}

static int apply_xim_feedback_update(
    boxrig_x11_input_adapter *adapter,
    const decoded_xim_text *update,
    size_t first,
    size_t caret)
{
    uint32_t *feedback;

    if (first > adapter->preedit_character_count ||
        update->character_count >
            adapter->preedit_character_count - first ||
        caret > adapter->preedit_character_count) {
        return -1;
    }
    feedback = malloc(
        (adapter->preedit_character_count == 0
             ? 1
             : adapter->preedit_character_count) *
        sizeof(*feedback));
    if (feedback == NULL) {
        return -1;
    }
    if (adapter->preedit_character_count != 0) {
        memcpy(
            feedback,
            adapter->preedit_feedback,
            adapter->preedit_character_count * sizeof(*feedback));
    }
    if (update->character_count != 0) {
        memcpy(
            feedback + first,
            update->feedback,
            update->character_count * sizeof(*feedback));
    }
    if (apply_current_xim_preedit(
            adapter,
            adapter->preedit_text,
            adapter->preedit_text_length,
            adapter->preedit_offsets,
            feedback,
            adapter->preedit_character_count,
            caret,
            true) == -1) {
        free(feedback);
        return -1;
    }
    free(adapter->preedit_feedback);
    adapter->preedit_feedback = feedback;
    adapter->preedit_caret = caret;
    adapter->preedit_caret_visible = true;
    adapter->preedit_started = true;
    adapter->key_had_composition = true;
    return 0;
}

static int apply_empty_composition_event(
    boxrig_x11_input_adapter *adapter,
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
    case BOXRIG_KEY_ISO_LEVEL5_SHIFT:
        return BOXRIG_MODIFIER_SHIFT;
    default:
        return 0;
    }
}

static bool modifier_still_pressed(
    const boxrig_x11_input_adapter *adapter,
    uint32_t modifier)
{
    size_t keycode;

    for (keycode = 0; keycode < KEYCODE_COUNT; keycode++) {
        if (adapter->pressed[keycode] &&
            modifier_for_key(adapter->identities[keycode].key) == modifier) {
            return true;
        }
    }
    return false;
}

static uint32_t translate_modifiers(
    const boxrig_x11_input_adapter *adapter,
    unsigned int state)
{
    uint32_t modifiers = 0;

    if ((state & ShiftMask) != 0) {
        modifiers |= BOXRIG_MODIFIER_SHIFT;
    }
    if ((state & ControlMask) != 0) {
        modifiers |= BOXRIG_MODIFIER_CONTROL;
    }
    if ((state & adapter->alt_mask) != 0) {
        modifiers |= BOXRIG_MODIFIER_ALT;
    }
    if ((state & adapter->super_mask) != 0) {
        modifiers |= BOXRIG_MODIFIER_SUPER;
    }
    if ((state & adapter->hyper_mask) != 0) {
        modifiers |= BOXRIG_MODIFIER_HYPER;
    }
    if ((state & adapter->meta_mask) != 0) {
        modifiers |= BOXRIG_MODIFIER_META;
    }
    if ((state & adapter->caps_lock_mask) != 0) {
        modifiers |= BOXRIG_MODIFIER_CAPS_LOCK;
    }
    if ((state & adapter->num_lock_mask) != 0) {
        modifiers |= BOXRIG_MODIFIER_NUM_LOCK;
    }
    return modifiers;
}

static boxrig_key translate_functional_key(KeySym symbol)
{
    if (symbol >= XK_F1 && symbol <= XK_F35) {
        return (boxrig_key)(BOXRIG_KEY_F1 + (symbol - XK_F1));
    }
    switch (symbol) {
    case XK_Return:
        return BOXRIG_KEY_ENTER;
    case XK_BackSpace:
        return BOXRIG_KEY_BACKSPACE;
    case XK_Tab:
    case XK_ISO_Left_Tab:
    case XK_KP_Tab:
        return BOXRIG_KEY_TAB;
    case XK_Escape:
        return BOXRIG_KEY_ESCAPE;
    case XK_Up:
        return BOXRIG_KEY_UP;
    case XK_Down:
        return BOXRIG_KEY_DOWN;
    case XK_Right:
        return BOXRIG_KEY_RIGHT;
    case XK_Left:
        return BOXRIG_KEY_LEFT;
    case XK_Home:
        return BOXRIG_KEY_HOME;
    case XK_End:
        return BOXRIG_KEY_END;
    case XK_Insert:
        return BOXRIG_KEY_INSERT;
    case XK_Delete:
        return BOXRIG_KEY_DELETE;
    case XK_Page_Up:
        return BOXRIG_KEY_PAGE_UP;
    case XK_Page_Down:
        return BOXRIG_KEY_PAGE_DOWN;
    case XK_Caps_Lock:
        return BOXRIG_KEY_CAPS_LOCK;
    case XK_Scroll_Lock:
        return BOXRIG_KEY_SCROLL_LOCK;
    case XK_Num_Lock:
        return BOXRIG_KEY_NUM_LOCK;
    case XK_Print:
        return BOXRIG_KEY_PRINT_SCREEN;
    case XK_Pause:
        return BOXRIG_KEY_PAUSE;
    case XK_Menu:
        return BOXRIG_KEY_MENU;
    case XK_KP_0:
        return BOXRIG_KEY_KP_0;
    case XK_KP_1:
        return BOXRIG_KEY_KP_1;
    case XK_KP_2:
        return BOXRIG_KEY_KP_2;
    case XK_KP_3:
        return BOXRIG_KEY_KP_3;
    case XK_KP_4:
        return BOXRIG_KEY_KP_4;
    case XK_KP_5:
        return BOXRIG_KEY_KP_5;
    case XK_KP_6:
        return BOXRIG_KEY_KP_6;
    case XK_KP_7:
        return BOXRIG_KEY_KP_7;
    case XK_KP_8:
        return BOXRIG_KEY_KP_8;
    case XK_KP_9:
        return BOXRIG_KEY_KP_9;
    case XK_KP_Decimal:
        return BOXRIG_KEY_KP_DECIMAL;
    case XK_KP_Divide:
        return BOXRIG_KEY_KP_DIVIDE;
    case XK_KP_Multiply:
        return BOXRIG_KEY_KP_MULTIPLY;
    case XK_KP_Subtract:
        return BOXRIG_KEY_KP_SUBTRACT;
    case XK_KP_Add:
        return BOXRIG_KEY_KP_ADD;
    case XK_KP_Enter:
        return BOXRIG_KEY_KP_ENTER;
    case XK_KP_Equal:
        return BOXRIG_KEY_KP_EQUAL;
    case XK_KP_Separator:
        return BOXRIG_KEY_KP_SEPARATOR;
    case XK_KP_Left:
        return BOXRIG_KEY_KP_LEFT;
    case XK_KP_Right:
        return BOXRIG_KEY_KP_RIGHT;
    case XK_KP_Up:
        return BOXRIG_KEY_KP_UP;
    case XK_KP_Down:
        return BOXRIG_KEY_KP_DOWN;
    case XK_KP_Page_Up:
        return BOXRIG_KEY_KP_PAGE_UP;
    case XK_KP_Page_Down:
        return BOXRIG_KEY_KP_PAGE_DOWN;
    case XK_KP_Home:
        return BOXRIG_KEY_KP_HOME;
    case XK_KP_End:
        return BOXRIG_KEY_KP_END;
    case XK_KP_Insert:
        return BOXRIG_KEY_KP_INSERT;
    case XK_KP_Delete:
        return BOXRIG_KEY_KP_DELETE;
    case XK_KP_Begin:
        return BOXRIG_KEY_KP_BEGIN;
    case XK_Shift_L:
        return BOXRIG_KEY_LEFT_SHIFT;
    case XK_Control_L:
        return BOXRIG_KEY_LEFT_CONTROL;
    case XK_Alt_L:
        return BOXRIG_KEY_LEFT_ALT;
    case XK_Super_L:
        return BOXRIG_KEY_LEFT_SUPER;
    case XK_Hyper_L:
        return BOXRIG_KEY_LEFT_HYPER;
    case XK_Meta_L:
        return BOXRIG_KEY_LEFT_META;
    case XK_Shift_R:
        return BOXRIG_KEY_RIGHT_SHIFT;
    case XK_Control_R:
        return BOXRIG_KEY_RIGHT_CONTROL;
    case XK_Alt_R:
        return BOXRIG_KEY_RIGHT_ALT;
    case XK_Super_R:
        return BOXRIG_KEY_RIGHT_SUPER;
    case XK_Hyper_R:
        return BOXRIG_KEY_RIGHT_HYPER;
    case XK_Meta_R:
        return BOXRIG_KEY_RIGHT_META;
    case XK_ISO_Level3_Shift:
        return BOXRIG_KEY_ISO_LEVEL3_SHIFT;
    case XK_ISO_Level5_Shift:
        return BOXRIG_KEY_ISO_LEVEL5_SHIFT;
    case XF86XK_AudioPlay:
        return BOXRIG_KEY_MEDIA_PLAY;
    case XF86XK_AudioPause:
        return BOXRIG_KEY_MEDIA_PAUSE;
    case XF86XK_AudioStop:
        return BOXRIG_KEY_MEDIA_STOP;
    case XF86XK_AudioForward:
        return BOXRIG_KEY_MEDIA_FAST_FORWARD;
    case XF86XK_AudioRewind:
        return BOXRIG_KEY_MEDIA_REWIND;
    case XF86XK_AudioNext:
        return BOXRIG_KEY_MEDIA_TRACK_NEXT;
    case XF86XK_AudioPrev:
        return BOXRIG_KEY_MEDIA_TRACK_PREVIOUS;
    case XF86XK_AudioRecord:
        return BOXRIG_KEY_MEDIA_RECORD;
    case XF86XK_AudioLowerVolume:
        return BOXRIG_KEY_LOWER_VOLUME;
    case XF86XK_AudioRaiseVolume:
        return BOXRIG_KEY_RAISE_VOLUME;
    case XF86XK_AudioMute:
        return BOXRIG_KEY_MUTE_VOLUME;
    default:
        return BOXRIG_KEY_NONE;
    }
}

static uint32_t keysym_codepoint(KeySym symbol)
{
    if ((symbol >= 0x20 && symbol <= 0x7e) ||
        (symbol >= 0xa0 && symbol <= 0xff)) {
        return (uint32_t)symbol;
    }
    if (symbol >= 0x01000100 && symbol <= 0x0110ffff) {
        return (uint32_t)(symbol & 0x00ffffffu);
    }
    return 0;
}

static uint32_t ascii_base_layout_codepoint(
    boxrig_x11_input_adapter *adapter,
    KeyCode keycode)
{
    uint32_t candidate = 0;
    int group;

    if (!adapter->xkb_available) {
        return 0;
    }
    for (group = 0; group < XkbNumKbdGroups; group++) {
        uint32_t codepoint = keysym_codepoint(XkbKeycodeToKeysym(
            adapter->display,
            keycode,
            group,
            0));

        if (codepoint < 0x20 || codepoint > 0x7e) {
            continue;
        }
        if (candidate != 0 && candidate != codepoint) {
            /* Different ASCII groups do not identify one PC-101 key. */
            return 0;
        }
        candidate = codepoint;
    }
    return candidate;
}

static void lookup_keysyms(
    boxrig_x11_input_adapter *adapter,
    const XKeyEvent *event,
    KeySym *unshifted_out,
    KeySym *shifted_out,
    KeySym *current_out)
{
    KeySym unshifted = NoSymbol;
    KeySym shifted = NoSymbol;
    KeySym current = NoSymbol;

    if (adapter->xkb_available) {
        int group = XkbGroupForCoreState(event->state);
        unsigned int remaining_modifiers;

        unshifted = XkbKeycodeToKeysym(
            adapter->display,
            event->keycode,
            group,
            0);
        shifted = XkbKeycodeToKeysym(
            adapter->display,
            event->keycode,
            group,
            1);
        (void)XkbLookupKeySym(
            adapter->display,
            event->keycode,
            event->state,
            &remaining_modifiers,
            &current);
    } else {
        XKeyEvent copy = *event;
        XComposeStatus compose;
        char ignored[8];

        memset(&compose, 0, sizeof(compose));
        copy.state = 0;
        (void)XLookupString(
            &copy,
            ignored,
            (int)sizeof(ignored),
            &unshifted,
            &compose);
        memset(&compose, 0, sizeof(compose));
        copy.state = ShiftMask;
        (void)XLookupString(
            &copy,
            ignored,
            (int)sizeof(ignored),
            &shifted,
            &compose);
        memset(&compose, 0, sizeof(compose));
        copy.state = event->state;
        (void)XLookupString(
            &copy,
            ignored,
            (int)sizeof(ignored),
            &current,
            &compose);
    }
    if (shifted == NoSymbol) {
        shifted = unshifted;
    }
    if (current == NoSymbol) {
        current = (event->state & ShiftMask) != 0 ? shifted : unshifted;
    }
    *unshifted_out = unshifted;
    *shifted_out = shifted;
    *current_out = current;
}

static key_identity identify_key(
    boxrig_x11_input_adapter *adapter,
    const XKeyEvent *event)
{
    key_identity identity;
    KeySym unshifted;
    KeySym shifted;
    KeySym current;

    memset(&identity, 0, sizeof(identity));
    lookup_keysyms(adapter, event, &unshifted, &shifted, &current);
    identity.key = translate_functional_key(current);
    if (identity.key == BOXRIG_KEY_NONE) {
        identity.key = translate_functional_key(unshifted);
    }
    if (identity.key == BOXRIG_KEY_NONE) {
        identity.key = translate_functional_key(shifted);
    }
    if (identity.key == BOXRIG_KEY_NONE) {
        identity.codepoint = keysym_codepoint(unshifted);
        identity.shifted_codepoint = keysym_codepoint(shifted);
        if (identity.shifted_codepoint == identity.codepoint) {
            identity.shifted_codepoint = 0;
        }
        identity.base_layout_codepoint = ascii_base_layout_codepoint(
            adapter,
            event->keycode);
    }
    return identity;
}

static int route_key_event(
    boxrig_x11_input_adapter *adapter,
    XKeyEvent *event)
{
    boxrig_key_event logical;
    key_identity identity;
    uint32_t changed_modifier;
    bool repeated;
    boxrig_input_key_route route;

    if (adapter->key_router == NULL) {
        return 0;
    }
    if (event->type == KeyRelease && repeated_release(adapter, event)) {
        return adapter->application_key[event->keycode] ? 1 : 0;
    }
    repeated = adapter->pressed[event->keycode];
    if (event->type == KeyPress && repeated &&
        !adapter->application_key[event->keycode]) {
        return 0;
    }
    if (event->type == KeyRelease &&
        !adapter->application_key[event->keycode]) {
        return 0;
    }
    identity = repeated || event->type == KeyRelease
                   ? adapter->identities[event->keycode]
                   : identify_key(adapter, event);
    changed_modifier = modifier_for_key(identity.key);
    memset(&logical, 0, sizeof(logical));
    logical.key = identity.key;
    logical.codepoint = identity.codepoint;
    logical.shifted_codepoint = identity.shifted_codepoint;
    logical.base_layout_codepoint = identity.base_layout_codepoint;
    logical.action = event->type == KeyRelease
                         ? BOXRIG_KEY_RELEASE
                         : repeated ? BOXRIG_KEY_REPEAT : BOXRIG_KEY_PRESS;
    logical.modifiers = translate_modifiers(adapter, event->state);
    if (event->type == KeyPress && changed_modifier != 0) {
        logical.modifiers |= changed_modifier;
    }
    if (event->type == KeyRelease) {
        adapter->pressed[event->keycode] = false;
        if (changed_modifier != 0 &&
            !modifier_still_pressed(adapter, changed_modifier)) {
            logical.modifiers &= ~changed_modifier;
        }
    }
    route = adapter->key_router(&logical, adapter->key_router_context);
    if (route == BOXRIG_INPUT_KEY_ROUTE_ERROR ||
        (route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL &&
         route != BOXRIG_INPUT_KEY_ROUTE_APPLICATION)) {
        return -1;
    }
    if (event->type == KeyRelease) {
        adapter->application_key[event->keycode] = false;
        return 1;
    }
    if (repeated) {
        return 1;
    }
    if (route == BOXRIG_INPUT_KEY_ROUTE_APPLICATION) {
        adapter->pressed[event->keycode] = true;
        adapter->identities[event->keycode] = identity;
        adapter->application_key[event->keycode] = true;
        return 1;
    }
    return 0;
}

static int lookup_key_text(
    boxrig_x11_input_adapter *adapter,
    XKeyEvent *event,
    char **text_out,
    size_t *length_out,
    Status *status_out)
{
    char *text;
    size_t capacity = INITIAL_KEY_TEXT_CAPACITY;
    int length;

    *text_out = NULL;
    *length_out = 0;
    *status_out = XLookupNone;
    text = malloc(capacity);
    if (text == NULL) {
        return -1;
    }
    if (adapter->input_context != NULL) {
        Status status;
        KeySym symbol;

        length = Xutf8LookupString(
            adapter->input_context,
            event,
            text,
            (int)capacity,
            &symbol,
            &status);
        if (status == XBufferOverflow) {
            char *resized;

            if (length <= 0) {
                free(text);
                return -1;
            }
            capacity = (size_t)length;
            resized = realloc(text, capacity);
            if (resized == NULL) {
                free(text);
                return -1;
            }
            text = resized;
            length = Xutf8LookupString(
                adapter->input_context,
                event,
                text,
                (int)capacity,
                &symbol,
                &status);
        }
        if (length < 0 || status == XBufferOverflow) {
            free(text);
            return -1;
        }
        if (status == XLookupNone || status == XLookupKeySym) {
            length = 0;
        }
        *status_out = status;
    } else {
        XComposeStatus compose;
        KeySym symbol;

        memset(&compose, 0, sizeof(compose));
        length = XLookupString(
            event,
            text,
            (int)capacity,
            &symbol,
            &compose);
        if (length < 0) {
            free(text);
            return -1;
        }
    }
    *text_out = text;
    *length_out = (size_t)length;
    return 0;
}

static int queue_key(
    boxrig_x11_input_adapter *adapter,
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
    boxrig_x11_input_adapter *adapter,
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
    boxrig_x11_input_adapter *adapter,
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

static int retry_pending_input(boxrig_x11_input_adapter *adapter)
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

static int retry_paste(boxrig_x11_input_adapter *adapter)
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

static unsigned int modifier_mask(
    boxrig_x11_input_adapter *adapter,
    KeySym left,
    KeySym right)
{
    if (!adapter->xkb_available) {
        return 0;
    }
    return XkbKeysymToModifiers(adapter->display, left) |
           XkbKeysymToModifiers(adapter->display, right);
}

static bool repeated_release(
    boxrig_x11_input_adapter *adapter,
    const XKeyEvent *event)
{
    XEvent next;

    if (adapter->detectable_repeat || XPending(adapter->display) == 0) {
        return false;
    }
    XPeekEvent(adapter->display, &next);
    return next.type == KeyPress &&
           next.xkey.keycode == event->keycode &&
           next.xkey.time == event->time;
}

static foreign_window_watch *find_foreign_window_watch(
    boxrig_x11_input_adapter *adapter,
    Window window)
{
    size_t index;

    for (index = 0; index < adapter->maximum_foreign_window_watches;
         index++) {
        foreign_window_watch *watch =
            &adapter->foreign_window_watches[index];

        if (watch->active && watch->window == window) {
            return watch;
        }
    }
    return NULL;
}

static int watch_foreign_window(
    boxrig_x11_input_adapter *adapter,
    Window window,
    long event_mask)
{
    foreign_window_watch *watch;
    XWindowAttributes attributes;
    size_t index;

    if (window == None || window == adapter->window) {
        return 0;
    }
    watch = find_foreign_window_watch(adapter, window);
    if (watch != NULL) {
        long selected_event_mask = watch->selected_event_mask | event_mask;

        if (selected_event_mask != watch->selected_event_mask) {
            XSelectInput(adapter->display, window, selected_event_mask);
            watch->selected_event_mask = selected_event_mask;
        }
        watch->reference_count++;
        return 0;
    }
    for (index = 0; index < adapter->maximum_foreign_window_watches;
         index++) {
        if (!adapter->foreign_window_watches[index].active) {
            break;
        }
    }
    if (index == adapter->maximum_foreign_window_watches ||
        XGetWindowAttributes(adapter->display, window, &attributes) == 0) {
        return -1;
    }
    watch = &adapter->foreign_window_watches[index];
    memset(watch, 0, sizeof(*watch));
    watch->window = window;
    watch->original_event_mask = attributes.your_event_mask;
    watch->selected_event_mask = attributes.your_event_mask | event_mask;
    watch->reference_count = 1;
    watch->active = true;
    XSelectInput(adapter->display, window, watch->selected_event_mask);
    return 0;
}

static void unwatch_foreign_window(
    boxrig_x11_input_adapter *adapter,
    Window window,
    bool window_alive)
{
    foreign_window_watch *watch =
        find_foreign_window_watch(adapter, window);

    if (watch == NULL) {
        return;
    }
    if (watch->reference_count != 0) {
        watch->reference_count--;
    }
    if (watch->reference_count != 0) {
        return;
    }
    if (window_alive) {
        XSelectInput(
            adapter->display,
            window,
            watch->original_event_mask);
    }
    memset(watch, 0, sizeof(*watch));
}

static void reset_incoming_selection(
    boxrig_x11_input_adapter *adapter,
    bool owner_alive,
    bool delete_property)
{
    if (delete_property && adapter->input_window_alive) {
        XDeleteProperty(
            adapter->display,
            adapter->window,
            adapter->paste_property);
    }
    unwatch_foreign_window(
        adapter,
        adapter->selection_owner,
        owner_alive);
    free(adapter->selection_bytes);
    adapter->selection_bytes = NULL;
    adapter->selection_length = 0;
    adapter->selection_capacity = 0;
    adapter->selection_receive_state = SELECTION_RECEIVE_IDLE;
    adapter->selection = None;
    adapter->selection_target = None;
    adapter->selection_owner = None;
}

static int request_server_timestamp(boxrig_x11_input_adapter *adapter)
{
    if (!adapter->input_window_alive) {
        return -1;
    }
    if (adapter->server_timestamp_pending) {
        return 0;
    }
    adapter->server_timestamp_pending = true;
    XChangeProperty(
        adapter->display,
        adapter->window,
        adapter->timestamp_property,
        XA_INTEGER,
        8,
        PropModeAppend,
        NULL,
        0);
    XFlush(adapter->display);
    return 0;
}

static int begin_selection(
    boxrig_x11_input_adapter *adapter,
    Atom selection,
    Time time)
{
    Window owner;

    if (adapter->selection_receive_state != SELECTION_RECEIVE_IDLE ||
        !adapter->input_window_alive) {
        return -1;
    }
    owner = XGetSelectionOwner(adapter->display, selection);
    if (owner == None ||
        watch_foreign_window(
            adapter,
            owner,
            StructureNotifyMask) == -1) {
        return -1;
    }
    XDeleteProperty(
        adapter->display,
        adapter->window,
        adapter->paste_property);
    adapter->selection_receive_state = SELECTION_RECEIVE_TARGETS;
    adapter->selection = selection;
    adapter->selection_target = adapter->targets;
    adapter->selection_owner = owner;
    (void)XConvertSelection(
        adapter->display,
        selection,
        adapter->selection_target,
        adapter->paste_property,
        adapter->window,
        time);
    XFlush(adapter->display);
    return 0;
}

static int process_key_press(
    boxrig_x11_input_adapter *adapter,
    XKeyEvent *event)
{
    boxrig_key_event logical;
    key_identity identity = identify_key(adapter, event);
    uint32_t changed_modifier = modifier_for_key(identity.key);
    char *text = NULL;
    size_t text_length = 0;
    bool repeated = adapter->pressed[event->keycode];
    Status lookup_status = XLookupNone;
    int result;

    adapter->pressed[event->keycode] = true;
    adapter->identities[event->keycode] = identity;
    if ((identity.key == BOXRIG_KEY_NONE ||
         adapter->key_had_composition) &&
        lookup_key_text(
            adapter,
            event,
            &text,
            &text_length,
            &lookup_status) == -1) {
        return -1;
    }
    if (adapter->document_reset_pending) {
        free(text);
        return 1;
    }
    if (text_length != 0 && lookup_status == XLookupChars) {
        adapter->composition_key[event->keycode] = true;
        result = boxrig_x11_input_adapter_commit_utf8(
            adapter,
            text,
            text_length);
        free(text);
        return result == 0 ? 1 : -1;
    }
    /* XLookupBoth includes the ordinary key symbol. In particular, IBus
     * returns BackSpace plus 0x08 when deletion ends preediting. Complete the
     * hidden preedit, then route that key instead of committing its control
     * byte as composition text. */
    if (adapter->key_had_composition && adapter->preedit_started &&
        lookup_status == XLookupBoth &&
        boxrig_x11_input_adapter_preedit_done(adapter) == -1) {
        free(text);
        return -1;
    }
    memset(&logical, 0, sizeof(logical));
    logical.key = identity.key;
    logical.codepoint = identity.codepoint;
    logical.shifted_codepoint = identity.shifted_codepoint;
    logical.base_layout_codepoint = identity.base_layout_codepoint;
    logical.action = repeated ? BOXRIG_KEY_REPEAT : BOXRIG_KEY_PRESS;
    logical.modifiers = translate_modifiers(adapter, event->state);
    if (changed_modifier != 0) {
        logical.modifiers |= changed_modifier;
    }
    logical.text = text;
    logical.text_length = text_length;
    result = queue_key(adapter, &logical);
    free(text);
    return result == 0 ? 1 : -1;
}

static int process_key_release(
    boxrig_x11_input_adapter *adapter,
    XKeyEvent *event)
{
    boxrig_key_event logical;
    key_identity identity;
    uint32_t changed_modifier;

    if (repeated_release(adapter, event)) {
        return 1;
    }
    identity = adapter->pressed[event->keycode]
                   ? adapter->identities[event->keycode]
                   : identify_key(adapter, event);
    adapter->pressed[event->keycode] = false;
    if (adapter->composition_key[event->keycode]) {
        adapter->composition_key[event->keycode] = false;
        return 1;
    }
    memset(&logical, 0, sizeof(logical));
    logical.key = identity.key;
    logical.codepoint = identity.codepoint;
    logical.shifted_codepoint = identity.shifted_codepoint;
    logical.base_layout_codepoint = identity.base_layout_codepoint;
    logical.action = BOXRIG_KEY_RELEASE;
    logical.modifiers = translate_modifiers(adapter, event->state);
    changed_modifier = modifier_for_key(identity.key);
    if (changed_modifier != 0 &&
        !modifier_still_pressed(adapter, changed_modifier)) {
        logical.modifiers &= ~changed_modifier;
    }
    return queue_key(adapter, &logical) == 0 ? 1 : -1;
}

static uint32_t pressed_buttons_from_state(unsigned int state)
{
    uint32_t buttons = 0;

    if ((state & Button1Mask) != 0) {
        buttons |= BOXRIG_MOUSE_BUTTON_MASK_LEFT;
    }
    if ((state & Button2Mask) != 0) {
        buttons |= BOXRIG_MOUSE_BUTTON_MASK_MIDDLE;
    }
    if ((state & Button3Mask) != 0) {
        buttons |= BOXRIG_MOUSE_BUTTON_MASK_RIGHT;
    }
    return buttons;
}

static boxrig_mouse_button translate_button(unsigned int button)
{
    switch (button) {
    case Button1:
        return BOXRIG_MOUSE_BUTTON_LEFT;
    case Button2:
        return BOXRIG_MOUSE_BUTTON_MIDDLE;
    case Button3:
        return BOXRIG_MOUSE_BUTTON_RIGHT;
    case Button4:
        return BOXRIG_MOUSE_BUTTON_WHEEL_UP;
    case Button5:
        return BOXRIG_MOUSE_BUTTON_WHEEL_DOWN;
    case 6:
        return BOXRIG_MOUSE_BUTTON_WHEEL_LEFT;
    case 7:
        return BOXRIG_MOUSE_BUTTON_WHEEL_RIGHT;
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

static int route_mouse_transition(
    boxrig_x11_input_adapter *adapter,
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

static void translate_coordinates(
    boxrig_x11_input_adapter *adapter,
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
    size_t row = local_y < 0 ? 0 : (size_t)local_y / adapter->cell_height;
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

static uint8_t press_click_count(
    boxrig_x11_input_adapter *adapter,
    boxrig_mouse_button button,
    Time time,
    size_t row,
    size_t column)
{
    uint8_t count = 1;
    uint32_t elapsed = (uint32_t)time - (uint32_t)adapter->last_click_time;

    if (button >= BOXRIG_MOUSE_BUTTON_WHEEL_UP &&
        button <= BOXRIG_MOUSE_BUTTON_WHEEL_RIGHT) {
        return 0;
    }
    if (time != CurrentTime && adapter->last_click_valid &&
        adapter->last_click_time != CurrentTime &&
        adapter->last_click_button == button &&
        adapter->last_click_row == row &&
        adapter->last_click_column == column &&
        elapsed <= MULTI_CLICK_INTERVAL_MILLISECONDS &&
        adapter->last_click_count < 3) {
        count = (uint8_t)(adapter->last_click_count + 1);
    }
    adapter->last_click_time = time;
    adapter->last_click_button = button;
    adapter->last_click_row = row;
    adapter->last_click_column = column;
    adapter->last_click_count = count;
    adapter->last_click_valid = true;
    return count;
}

static int process_button_event(
    boxrig_x11_input_adapter *adapter,
    const XButtonEvent *event)
{
    boxrig_mouse_event logical;
    boxrig_mouse_button button = translate_button(event->button);
    uint32_t mask = button_mask(button);
    uint32_t state_buttons = pressed_buttons_from_state(event->state);

    if (button == BOXRIG_MOUSE_BUTTON_NONE) {
        return 1;
    }
    if (event->type == ButtonRelease &&
        button >= BOXRIG_MOUSE_BUTTON_WHEEL_UP &&
        button <= BOXRIG_MOUSE_BUTTON_WHEEL_RIGHT) {
        return 1;
    }
    adapter->pressed_buttons &=
        ~(BOXRIG_MOUSE_BUTTON_MASK_LEFT |
          BOXRIG_MOUSE_BUTTON_MASK_MIDDLE |
          BOXRIG_MOUSE_BUTTON_MASK_RIGHT);
    adapter->pressed_buttons |= state_buttons;
    if (event->type == ButtonPress) {
        adapter->pressed_buttons |= mask;
    } else {
        adapter->pressed_buttons &= ~mask;
    }
    memset(&logical, 0, sizeof(logical));
    logical.action = event->type == ButtonPress ? BOXRIG_MOUSE_PRESS
                                                : BOXRIG_MOUSE_RELEASE;
    logical.button = button;
    logical.pressed_buttons = adapter->pressed_buttons;
    logical.modifiers = translate_modifiers(adapter, event->state);
    translate_coordinates(
        adapter,
        event->x,
        event->y,
        &logical.x,
        &logical.y,
        &logical.row,
        &logical.column);
    if (logical.action == BOXRIG_MOUSE_PRESS) {
        logical.click_count = press_click_count(
            adapter,
            button,
            event->time,
            logical.row,
            logical.column);
    }
    {
        int route = route_mouse_transition(adapter, &logical, mask);

        if (route != 0) {
            return route;
        }
    }
    return queue_mouse(adapter, &logical) == 0 ? 1 : -1;
}

static int process_motion_event(
    boxrig_x11_input_adapter *adapter,
    const XMotionEvent *event)
{
    boxrig_mouse_event logical;
    uint32_t extra_buttons = adapter->pressed_buttons &
                             ~(BOXRIG_MOUSE_BUTTON_MASK_LEFT |
                               BOXRIG_MOUSE_BUTTON_MASK_MIDDLE |
                               BOXRIG_MOUSE_BUTTON_MASK_RIGHT);

    adapter->pressed_buttons = pressed_buttons_from_state(event->state) |
                               extra_buttons;
    memset(&logical, 0, sizeof(logical));
    logical.action = BOXRIG_MOUSE_MOTION;
    logical.button = BOXRIG_MOUSE_BUTTON_NONE;
    logical.pressed_buttons = adapter->pressed_buttons;
    logical.modifiers = translate_modifiers(adapter, event->state);
    translate_coordinates(
        adapter,
        event->x,
        event->y,
        &logical.x,
        &logical.y,
        &logical.row,
        &logical.column);
    {
        int route = route_mouse_transition(adapter, &logical, 0);

        if (route != 0) {
            return route;
        }
    }
    return queue_mouse(adapter, &logical) == 0 ? 1 : -1;
}

static int get_bounded_property(
    boxrig_x11_input_adapter *adapter,
    Window window,
    Atom property,
    size_t byte_capacity,
    bool delete_property,
    Atom *actual_type_out,
    int *actual_format_out,
    unsigned long *item_count_out,
    unsigned char **value_out)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *value = NULL;
    size_t long_length = byte_capacity / 4;
    size_t actual_bytes;
    int status;

    *actual_type_out = None;
    *actual_format_out = 0;
    *item_count_out = 0;
    *value_out = NULL;
    if (byte_capacity % 4 != 0) {
        long_length++;
    }
    if (long_length > LONG_MAX) {
        long_length = LONG_MAX;
    }
    status = XGetWindowProperty(
        adapter->display,
        window,
        property,
        0,
        (long)long_length,
        delete_property ? True : False,
        AnyPropertyType,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &value);
    if (actual_format == 8) {
        actual_bytes = (size_t)item_count;
    } else if (actual_format == 16 &&
               item_count <= SIZE_MAX / 2) {
        actual_bytes = (size_t)item_count * 2;
    } else if (actual_format == 32 &&
               item_count <= SIZE_MAX / 4) {
        actual_bytes = (size_t)item_count * 4;
    } else {
        actual_bytes = SIZE_MAX;
    }
    if (status != Success || bytes_after != 0 ||
        actual_bytes > byte_capacity) {
        if (value != NULL) {
            XFree(value);
        }
        return -1;
    }
    *actual_type_out = actual_type;
    *actual_format_out = actual_format;
    *item_count_out = item_count;
    *value_out = value;
    return 0;
}

static int latin1_to_utf8(
    const unsigned char *value,
    size_t length,
    size_t capacity,
    uint8_t **utf8_out,
    size_t *utf8_length_out)
{
    uint8_t *utf8;
    size_t required = 0;
    size_t source;
    size_t destination = 0;

    for (source = 0; source < length; source++) {
        required += value[source] < 0x80 ? 1 : 2;
        if (required > capacity) {
            return -1;
        }
    }
    utf8 = malloc(required == 0 ? 1 : required);
    if (utf8 == NULL) {
        return -1;
    }
    for (source = 0; source < length; source++) {
        if (value[source] < 0x80) {
            utf8[destination++] = value[source];
        } else {
            utf8[destination++] = (uint8_t)(0xc0u | (value[source] >> 6));
            utf8[destination++] = (uint8_t)(0x80u | (value[source] & 0x3fu));
        }
    }
    *utf8_out = utf8;
    *utf8_length_out = required;
    return 0;
}

static clipboard_offer *find_clipboard_offer(
    boxrig_x11_input_adapter *adapter,
    Atom selection)
{
    if (selection == XA_PRIMARY) {
        return &adapter->primary_offer;
    }
    if (selection == adapter->clipboard) {
        return &adapter->clipboard_offer;
    }
    return NULL;
}

static void release_outgoing_transfer(
    boxrig_x11_input_adapter *adapter,
    outgoing_selection_transfer *transfer,
    bool requestor_alive)
{
    Window requestor;

    if (!transfer->active) {
        return;
    }
    requestor = transfer->requestor;
    if (adapter->active_selection_transfer_count != 0) {
        adapter->active_selection_transfer_count--;
    }
    if (adapter->active_selection_transfer_bytes >= transfer->length) {
        adapter->active_selection_transfer_bytes -= transfer->length;
    } else {
        adapter->active_selection_transfer_bytes = 0;
    }
    clipboard_payload_release(transfer->payload);
    memset(transfer, 0, sizeof(*transfer));
    unwatch_foreign_window(adapter, requestor, requestor_alive);
}

static bool cancel_outgoing_transfers_for_window(
    boxrig_x11_input_adapter *adapter,
    Window requestor,
    bool requestor_alive)
{
    size_t index;
    bool cancelled = false;

    for (index = 0;
         index < adapter->maximum_active_selection_transfers;
         index++) {
        outgoing_selection_transfer *transfer =
            &adapter->outgoing_transfers[index];

        if (transfer->active && transfer->requestor == requestor) {
            release_outgoing_transfer(
                adapter,
                transfer,
                requestor_alive);
            cancelled = true;
        }
    }
    return cancelled;
}

static void cancel_outgoing_transfers_for_selection(
    boxrig_x11_input_adapter *adapter,
    Atom selection)
{
    size_t index;

    for (index = 0;
         index < adapter->maximum_active_selection_transfers;
         index++) {
        outgoing_selection_transfer *transfer =
            &adapter->outgoing_transfers[index];

        if (transfer->active && transfer->selection == selection) {
            release_outgoing_transfer(adapter, transfer, true);
        }
    }
}

static void lose_clipboard_offer(
    boxrig_x11_input_adapter *adapter,
    clipboard_offer *offer)
{
    if (offer == NULL) {
        return;
    }
    cancel_outgoing_transfers_for_selection(adapter, offer->selection);
    clipboard_payload_release(offer->payload);
    offer->payload = NULL;
    offer->timestamp = CurrentTime;
    offer->owned = false;
}

static void clear_clipboard_offer(
    boxrig_x11_input_adapter *adapter,
    clipboard_offer *offer)
{
    if (offer == NULL) {
        return;
    }
    lose_clipboard_offer(adapter, offer);
    clipboard_payload_release(offer->pending_payload);
    offer->pending_payload = NULL;
    offer->acquisition_pending = false;
}

static int send_selection_notify(
    boxrig_x11_input_adapter *adapter,
    const XSelectionRequestEvent *request,
    Atom property)
{
    XEvent response;

    memset(&response, 0, sizeof(response));
    response.xselection.type = SelectionNotify;
    response.xselection.display = request->display;
    response.xselection.requestor = request->requestor;
    response.xselection.selection = request->selection;
    response.xselection.target = request->target;
    response.xselection.property = property;
    response.xselection.time = request->time;
    if (XSendEvent(
            adapter->display,
            request->requestor,
            False,
            0,
            &response) == 0) {
        return -1;
    }
    XFlush(adapter->display);
    return 1;
}

static outgoing_selection_transfer *reserve_outgoing_transfer(
    boxrig_x11_input_adapter *adapter,
    const XSelectionRequestEvent *request,
    Atom property,
    clipboard_payload *payload,
    size_t length)
{
    size_t index;

    if (length > adapter->maximum_selection_transfer_bytes ||
        adapter->active_selection_transfer_count >=
            adapter->maximum_active_selection_transfers ||
        adapter->active_selection_transfer_bytes >
            adapter->maximum_selection_transfer_bytes - length) {
        return NULL;
    }
    for (index = 0;
         index < adapter->maximum_active_selection_transfers;
         index++) {
        outgoing_selection_transfer *transfer =
            &adapter->outgoing_transfers[index];

        if (transfer->active &&
            transfer->requestor == request->requestor &&
            transfer->property == property) {
            return NULL;
        }
    }
    for (index = 0;
         index < adapter->maximum_active_selection_transfers;
         index++) {
        if (!adapter->outgoing_transfers[index].active) {
            break;
        }
    }
    if (index == adapter->maximum_active_selection_transfers ||
        watch_foreign_window(
            adapter,
            request->requestor,
            PropertyChangeMask | StructureNotifyMask) == -1) {
        return NULL;
    }
    adapter->outgoing_transfers[index].requestor = request->requestor;
    adapter->outgoing_transfers[index].selection = request->selection;
    adapter->outgoing_transfers[index].property = property;
    adapter->outgoing_transfers[index].target = request->target;
    adapter->outgoing_transfers[index].payload = payload;
    adapter->outgoing_transfers[index].length = length;
    adapter->outgoing_transfers[index].active = true;
    clipboard_payload_retain(payload);
    adapter->active_selection_transfer_count++;
    adapter->active_selection_transfer_bytes += length;
    return &adapter->outgoing_transfers[index];
}

static bool timestamp_precedes(Time candidate, Time reference)
{
    return (int32_t)((uint32_t)candidate - (uint32_t)reference) < 0;
}

static int process_selection_request(
    boxrig_x11_input_adapter *adapter,
    const XSelectionRequestEvent *request)
{
    clipboard_offer *offer =
        find_clipboard_offer(adapter, request->selection);
    Atom property = request->property == None
                        ? request->target
                        : request->property;
    outgoing_selection_transfer *transfer = NULL;
    const uint8_t *bytes;
    size_t length;
    bool success = false;

    if (request->owner != adapter->window) {
        return 0;
    }
    if (offer != NULL && offer->owned && offer->payload != NULL &&
        (request->time == CurrentTime ||
         !timestamp_precedes(request->time, offer->timestamp)) &&
        XGetSelectionOwner(adapter->display, request->selection) ==
            adapter->window) {
        if (request->target == adapter->targets) {
            Atom supported[4];
            int supported_count = 0;

            supported[supported_count++] = adapter->targets;
            supported[supported_count++] = adapter->utf8_string;
            supported[supported_count++] = adapter->timestamp;
            if (offer->payload->string_available) {
                supported[supported_count++] = XA_STRING;
            }
            XChangeProperty(
                adapter->display,
                request->requestor,
                property,
                XA_ATOM,
                32,
                PropModeReplace,
                (const unsigned char *)supported,
                supported_count);
            success = true;
        } else if (request->target == adapter->timestamp) {
            unsigned long timestamp = (unsigned long)offer->timestamp;

            XChangeProperty(
                adapter->display,
                request->requestor,
                property,
                XA_INTEGER,
                32,
                PropModeReplace,
                (const unsigned char *)&timestamp,
                1);
            success = true;
        } else if (request->target == adapter->utf8_string ||
                   (request->target == XA_STRING &&
                    offer->payload->string_available)) {
            unsigned long announced_length;

            bytes = clipboard_payload_bytes(
                offer->payload,
                request->target,
                adapter->utf8_string,
                &length);
            if (length > adapter->maximum_selection_transfer_bytes) {
                success = false;
            } else if (length <= adapter->selection_transfer_chunk_size &&
                length <= INT_MAX) {
                XChangeProperty(
                    adapter->display,
                    request->requestor,
                    property,
                    request->target,
                    8,
                    PropModeReplace,
                    bytes,
                    (int)length);
                success = true;
            } else {
                transfer = reserve_outgoing_transfer(
                    adapter,
                    request,
                    property,
                    offer->payload,
                    length);
                if (transfer != NULL) {
                    announced_length = (unsigned long)length;
                    XChangeProperty(
                        adapter->display,
                        request->requestor,
                        property,
                        adapter->incr,
                        32,
                        PropModeReplace,
                        (const unsigned char *)&announced_length,
                        1);
                    success = true;
                }
            }
        }
    }
    if (send_selection_notify(
            adapter,
            request,
            success ? property : None) == -1) {
        if (transfer != NULL) {
            release_outgoing_transfer(adapter, transfer, true);
        }
        return -1;
    }
    return 1;
}

static int process_outgoing_property_notify(
    boxrig_x11_input_adapter *adapter,
    const XPropertyEvent *event)
{
    size_t index;

    if (event->state != PropertyDelete) {
        return 0;
    }
    for (index = 0;
         index < adapter->maximum_active_selection_transfers;
         index++) {
        outgoing_selection_transfer *transfer =
            &adapter->outgoing_transfers[index];
        const uint8_t *bytes;
        size_t payload_length;
        size_t chunk_length;

        if (!transfer->active || transfer->requestor != event->window ||
            transfer->property != event->atom) {
            continue;
        }
        bytes = clipboard_payload_bytes(
            transfer->payload,
            transfer->target,
            adapter->utf8_string,
            &payload_length);
        if (payload_length != transfer->length ||
            transfer->offset > transfer->length) {
            release_outgoing_transfer(adapter, transfer, true);
            return 1;
        }
        if (transfer->offset == transfer->length) {
            XChangeProperty(
                adapter->display,
                transfer->requestor,
                transfer->property,
                transfer->target,
                8,
                PropModeAppend,
                NULL,
                0);
            release_outgoing_transfer(adapter, transfer, true);
            XFlush(adapter->display);
            return 1;
        }
        chunk_length = transfer->length - transfer->offset;
        if (chunk_length > adapter->selection_transfer_chunk_size) {
            chunk_length = adapter->selection_transfer_chunk_size;
        }
        if (chunk_length > INT_MAX) {
            chunk_length = INT_MAX;
        }
        XChangeProperty(
            adapter->display,
            transfer->requestor,
            transfer->property,
            transfer->target,
            8,
            PropModeAppend,
            bytes + transfer->offset,
            (int)chunk_length);
        transfer->offset += chunk_length;
        XFlush(adapter->display);
        return 1;
    }
    return 0;
}

static void acquire_clipboard_offer(
    boxrig_x11_input_adapter *adapter,
    clipboard_offer *offer,
    Time timestamp)
{
    clipboard_payload *payload;
    Window owner;

    if (!offer->acquisition_pending) {
        return;
    }
    payload = offer->pending_payload;
    offer->pending_payload = NULL;
    offer->acquisition_pending = false;
    XSetSelectionOwner(
        adapter->display,
        offer->selection,
        adapter->window,
        timestamp);
    owner = XGetSelectionOwner(adapter->display, offer->selection);
    if (owner == adapter->window) {
        clipboard_payload_release(offer->payload);
        offer->payload = payload;
        offer->timestamp = timestamp;
        offer->owned = true;
        return;
    }
    clipboard_payload_release(payload);
    if (offer->owned) {
        lose_clipboard_offer(adapter, offer);
    }
}

static int process_server_timestamp(
    boxrig_x11_input_adapter *adapter,
    const XPropertyEvent *event)
{
    Atom selection;

    if (!adapter->server_timestamp_pending ||
        event->window != adapter->window ||
        event->atom != adapter->timestamp_property ||
        event->state != PropertyNewValue) {
        return 0;
    }
    adapter->server_timestamp_pending = false;
    XDeleteProperty(
        adapter->display,
        adapter->window,
        adapter->timestamp_property);
    acquire_clipboard_offer(
        adapter,
        &adapter->primary_offer,
        event->time);
    acquire_clipboard_offer(
        adapter,
        &adapter->clipboard_offer,
        event->time);
    if (adapter->selection_receive_state == SELECTION_RECEIVE_TIMESTAMP) {
        selection = adapter->selection;
        reset_incoming_selection(adapter, true, false);
        (void)begin_selection(adapter, selection, event->time);
    }
    XFlush(adapter->display);
    return 1;
}

static int queue_incoming_selection(
    boxrig_x11_input_adapter *adapter,
    const uint8_t *bytes,
    size_t length)
{
    Atom target = adapter->selection_target;
    uint8_t *converted = NULL;
    const uint8_t *utf8 = bytes;
    size_t utf8_length = length;

    if (target == XA_STRING &&
        latin1_to_utf8(
            bytes,
            length,
            adapter->paste_capacity,
            &converted,
            &utf8_length) == -1) {
        reset_incoming_selection(adapter, true, false);
        return 1;
    }
    if (converted != NULL) {
        utf8 = converted;
    }
    reset_incoming_selection(adapter, true, false);
    (void)boxrig_x11_input_adapter_queue_paste(
        adapter,
        utf8,
        utf8_length);
    free(converted);
    return 1;
}

static int request_selection_data(
    boxrig_x11_input_adapter *adapter,
    const XSelectionEvent *event,
    const unsigned char *value,
    unsigned long item_count)
{
    const Atom *offered = (const Atom *)value;
    Atom target = None;
    unsigned long index;

    for (index = 0; index < item_count; index++) {
        if (offered[index] == adapter->utf8_string) {
            target = adapter->utf8_string;
            break;
        }
        if (offered[index] == XA_STRING) {
            target = XA_STRING;
        }
    }
    if (target == None) {
        reset_incoming_selection(adapter, true, false);
        return 1;
    }
    adapter->selection_receive_state = SELECTION_RECEIVE_DATA;
    adapter->selection_target = target;
    XConvertSelection(
        adapter->display,
        adapter->selection,
        target,
        adapter->paste_property,
        adapter->window,
        event->time);
    XFlush(adapter->display);
    return 1;
}

static int process_selection_notify(
    boxrig_x11_input_adapter *adapter,
    const XSelectionEvent *event)
{
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned char *value = NULL;

    if (adapter->selection_receive_state == SELECTION_RECEIVE_IDLE ||
        adapter->selection_receive_state == SELECTION_RECEIVE_INCR ||
        event->requestor != adapter->window ||
        event->selection != adapter->selection ||
        event->target != adapter->selection_target) {
        return 0;
    }
    if (event->property == None ||
        event->property != adapter->paste_property) {
        reset_incoming_selection(adapter, true, true);
        return 1;
    }
    if (adapter->selection_receive_state == SELECTION_RECEIVE_TARGETS) {
        int result;

        if (get_bounded_property(
                adapter,
                adapter->window,
                event->property,
                MAXIMUM_SELECTION_TARGET_COUNT * 4u,
                true,
                &actual_type,
                &actual_format,
                &item_count,
                &value) == -1 ||
            actual_type != XA_ATOM || actual_format != 32) {
            if (value != NULL) {
                XFree(value);
            }
            reset_incoming_selection(adapter, true, true);
            return 1;
        }
        result = request_selection_data(
            adapter,
            event,
            value,
            item_count);
        XFree(value);
        return result;
    }
    if (get_bounded_property(
            adapter,
            adapter->window,
            event->property,
            adapter->paste_capacity,
            true,
            &actual_type,
            &actual_format,
            &item_count,
            &value) == -1) {
        reset_incoming_selection(adapter, true, true);
        return 1;
    }
    if (actual_type == adapter->incr) {
        unsigned long announced_length;

        if (actual_format != 32 || item_count != 1) {
            XFree(value);
            reset_incoming_selection(adapter, true, true);
            return 1;
        }
        announced_length = ((const unsigned long *)value)[0];
        XFree(value);
        if (announced_length > adapter->paste_capacity) {
            reset_incoming_selection(adapter, true, true);
            return 1;
        }
        if (announced_length != 0) {
            adapter->selection_bytes = malloc((size_t)announced_length);
            if (adapter->selection_bytes == NULL) {
                reset_incoming_selection(adapter, true, true);
                return 1;
            }
        }
        adapter->selection_capacity = (size_t)announced_length;
        adapter->selection_receive_state = SELECTION_RECEIVE_INCR;
        XFlush(adapter->display);
        return 1;
    }
    if (actual_type != adapter->selection_target || actual_format != 8) {
        XFree(value);
        reset_incoming_selection(adapter, true, true);
        return 1;
    }
    {
        int result = queue_incoming_selection(
            adapter,
            value,
            (size_t)item_count);

        XFree(value);
        return result;
    }
}

static int append_incoming_selection(
    boxrig_x11_input_adapter *adapter,
    const unsigned char *value,
    size_t length)
{
    size_t required;

    if (length > adapter->paste_capacity - adapter->selection_length) {
        return -1;
    }
    required = adapter->selection_length + length;
    if (required > adapter->selection_capacity) {
        size_t capacity = adapter->selection_capacity == 0
                              ? adapter->paste_capacity < 64
                                    ? adapter->paste_capacity
                                    : 64
                              : adapter->selection_capacity;
        uint8_t *replacement;

        while (capacity < required) {
            if (capacity > adapter->paste_capacity / 2) {
                capacity = adapter->paste_capacity;
                break;
            }
            capacity *= 2;
        }
        if (capacity < required) {
            return -1;
        }
        replacement = realloc(adapter->selection_bytes, capacity);
        if (replacement == NULL) {
            return -1;
        }
        adapter->selection_bytes = replacement;
        adapter->selection_capacity = capacity;
    }
    if (length != 0) {
        memcpy(
            adapter->selection_bytes + adapter->selection_length,
            value,
            length);
    }
    adapter->selection_length = required;
    return 0;
}

static int process_incoming_property_notify(
    boxrig_x11_input_adapter *adapter,
    const XPropertyEvent *event)
{
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned char *value = NULL;
    size_t remaining;

    if (adapter->selection_receive_state != SELECTION_RECEIVE_INCR ||
        event->window != adapter->window ||
        event->atom != adapter->paste_property ||
        event->state != PropertyNewValue) {
        return 0;
    }
    remaining = adapter->paste_capacity - adapter->selection_length;
    if (get_bounded_property(
            adapter,
            adapter->window,
            event->atom,
            remaining,
            true,
            &actual_type,
            &actual_format,
            &item_count,
            &value) == -1 ||
        actual_type != adapter->selection_target || actual_format != 8) {
        if (value != NULL) {
            XFree(value);
        }
        reset_incoming_selection(adapter, true, true);
        return 1;
    }
    if (item_count == 0) {
        uint8_t *bytes = adapter->selection_bytes;
        size_t length = adapter->selection_length;
        int result;

        adapter->selection_bytes = NULL;
        adapter->selection_length = 0;
        adapter->selection_capacity = 0;
        XFree(value);
        result = queue_incoming_selection(adapter, bytes, length);
        free(bytes);
        return result;
    }
    if (append_incoming_selection(
            adapter,
            value,
            (size_t)item_count) == -1) {
        XFree(value);
        reset_incoming_selection(adapter, true, true);
        return 1;
    }
    XFree(value);
    XFlush(adapter->display);
    return 1;
}

static int process_selection_clear(
    boxrig_x11_input_adapter *adapter,
    const XSelectionClearEvent *event)
{
    clipboard_offer *offer =
        find_clipboard_offer(adapter, event->selection);

    if (event->window != adapter->window || offer == NULL ||
        !offer->owned) {
        return 0;
    }
    if (XGetSelectionOwner(adapter->display, event->selection) ==
        adapter->window) {
        return 1;
    }
    lose_clipboard_offer(adapter, offer);
    return 1;
}

static int process_destroy_notify(
    boxrig_x11_input_adapter *adapter,
    const XDestroyWindowEvent *event)
{
    bool handled = false;

    if (event->window == adapter->window) {
        adapter->input_window_alive = false;
        adapter->server_timestamp_pending = false;
        clear_clipboard_offer(adapter, &adapter->primary_offer);
        clear_clipboard_offer(adapter, &adapter->clipboard_offer);
        if (adapter->selection_receive_state != SELECTION_RECEIVE_IDLE) {
            reset_incoming_selection(adapter, true, false);
        }
        return 1;
    }
    if (cancel_outgoing_transfers_for_window(
            adapter,
            event->window,
            false)) {
        handled = true;
    }
    if (adapter->selection_receive_state != SELECTION_RECEIVE_IDLE &&
        adapter->selection_owner == event->window) {
        reset_incoming_selection(adapter, false, true);
        handled = true;
    }
    return handled ? 1 : 0;
}

boxrig_composition *boxrig_x11_input_adapter_composition(
    boxrig_x11_input_adapter *adapter)
{
    return adapter == NULL ? NULL : adapter->composition;
}

int boxrig_x11_input_adapter_preedit_start(
    boxrig_x11_input_adapter *adapter)
{
    char *empty_text;
    size_t *empty_offsets;
    uint32_t *empty_feedback;

    if (adapter == NULL || adapter->composition == NULL) {
        return -1;
    }
    note_xim_preedit_update(adapter);
    empty_text = malloc(1);
    empty_offsets = calloc(1, sizeof(*empty_offsets));
    empty_feedback = malloc(sizeof(*empty_feedback));
    if (empty_text == NULL || empty_offsets == NULL ||
        empty_feedback == NULL) {
        free(empty_text);
        free(empty_offsets);
        free(empty_feedback);
        return -1;
    }
    empty_text[0] = '\0';
    empty_feedback[0] = 0;
    if (apply_current_xim_preedit(
            adapter,
            empty_text,
            0,
            empty_offsets,
            empty_feedback,
            0,
            0,
            true) == -1) {
        free(empty_text);
        free(empty_offsets);
        free(empty_feedback);
        return -1;
    }
    clear_xim_preedit(adapter);
    adapter->preedit_text = empty_text;
    adapter->preedit_offsets = empty_offsets;
    adapter->preedit_feedback = empty_feedback;
    adapter->preedit_caret_visible = true;
    adapter->preedit_started = true;
    adapter->key_had_composition = true;
    adapter->escape_preedit_pending = false;
    adapter->reset_preedit_tail_pending = false;
    return 0;
}

int boxrig_x11_input_adapter_preedit_draw(
    boxrig_x11_input_adapter *adapter,
    const XIMPreeditDrawCallbackStruct *draw)
{
    decoded_xim_text replacement;
    char *new_text = NULL;
    size_t *new_offsets = NULL;
    uint32_t *new_feedback = NULL;
    size_t first;
    size_t removed;
    size_t prefix_bytes;
    size_t suffix_offset;
    size_t suffix_bytes;
    size_t new_text_length;
    size_t new_character_count;
    size_t index;
    int result = -1;

    memset(&replacement, 0, sizeof(replacement));
    if (adapter == NULL || draw == NULL || draw->chg_first < 0 ||
        draw->chg_length < 0 || draw->caret < 0) {
        return -1;
    }
    note_xim_preedit_update(adapter);
    if (adapter->reset_preedit_tail_pending &&
        !adapter->preedit_started) {
        /* Some XIM servers deliver the deletion and done callbacks after
         * Xutf8ResetIC has returned. Ignore that empty tail, but let a real
         * replacement start the next preedit even if start was omitted. */
        if (!xim_text_has_replacement(draw->text)) {
            return 0;
        }
        adapter->reset_preedit_tail_pending = false;
    }
    if (!adapter->preedit_started && adapter->preedit_text == NULL &&
        boxrig_x11_input_adapter_preedit_start(adapter) == -1) {
        return -1;
    }
    first = (size_t)draw->chg_first;
    removed = (size_t)draw->chg_length;
    if (first > adapter->preedit_character_count ||
        removed > adapter->preedit_character_count - first ||
        decode_xim_text(draw->text, &replacement) == -1 ||
        replacement.character_count >
            SIZE_MAX - (adapter->preedit_character_count - removed)) {
        goto cleanup;
    }
    if (replacement.feedback_only) {
        result = apply_xim_feedback_update(
            adapter,
            &replacement,
            first,
            (size_t)draw->caret);
        if (result == 0 && adapter->escape_preedit_pending) {
            adapter->escape_preedit_pending = false;
            if (adapter->preedit_character_count == 0) {
                result = boxrig_x11_input_adapter_preedit_cancel(adapter);
            }
        }
        goto cleanup;
    }
    new_character_count = adapter->preedit_character_count - removed +
                          replacement.character_count;
    if ((size_t)draw->caret > new_character_count ||
        !checked_array_size(
            new_character_count + 1,
            sizeof(*new_offsets)) ||
        !checked_array_size(
            new_character_count,
            sizeof(*new_feedback))) {
        goto cleanup;
    }
    prefix_bytes = adapter->preedit_offsets[first];
    suffix_offset = adapter->preedit_offsets[first + removed];
    suffix_bytes = adapter->preedit_text_length - suffix_offset;
    if (!replacement.feedback_specified &&
        replacement.character_count != 0) {
        uint32_t inherited_feedback = 0;

        /* Xlib permits either side at a highlight boundary. Prefer the
         * preceding run so insertion inside a conversion clause stays in
         * that clause; use the following run for insertion at the start. */
        if (first != 0) {
            inherited_feedback = adapter->preedit_feedback[first - 1];
        } else if (removed < adapter->preedit_character_count) {
            inherited_feedback = adapter->preedit_feedback[removed];
        }
        for (index = 0; index < replacement.character_count; index++) {
            replacement.feedback[index] = inherited_feedback;
        }
    }
    if (replacement.text_length > SIZE_MAX - prefix_bytes ||
        suffix_bytes >
            SIZE_MAX - prefix_bytes - replacement.text_length) {
        goto cleanup;
    }
    new_text_length = prefix_bytes + replacement.text_length + suffix_bytes;
    new_text = malloc(new_text_length + 1);
    new_offsets = malloc(
        (new_character_count + 1) * sizeof(*new_offsets));
    new_feedback = malloc(
        (new_character_count == 0 ? 1 : new_character_count) *
        sizeof(*new_feedback));
    if (new_text == NULL || new_offsets == NULL || new_feedback == NULL) {
        goto cleanup;
    }
    memcpy(new_text, adapter->preedit_text, prefix_bytes);
    if (replacement.text_length != 0) {
        memcpy(
            new_text + prefix_bytes,
            replacement.text,
            replacement.text_length);
    }
    memcpy(
        new_text + prefix_bytes + replacement.text_length,
        adapter->preedit_text + suffix_offset,
        suffix_bytes);
    new_text[new_text_length] = '\0';
    for (index = 0; index <= first; index++) {
        new_offsets[index] = adapter->preedit_offsets[index];
    }
    for (index = 1; index <= replacement.character_count; index++) {
        new_offsets[first + index] =
            prefix_bytes + replacement.offsets[index];
    }
    for (index = first + removed + 1;
         index <= adapter->preedit_character_count;
         index++) {
        size_t destination = first + replacement.character_count +
                             index - first - removed;

        new_offsets[destination] =
            prefix_bytes + replacement.text_length +
            adapter->preedit_offsets[index] - suffix_offset;
    }
    if (first != 0) {
        memcpy(
            new_feedback,
            adapter->preedit_feedback,
            first * sizeof(*new_feedback));
    }
    if (replacement.character_count != 0) {
        memcpy(
            new_feedback + first,
            replacement.feedback,
            replacement.character_count * sizeof(*new_feedback));
    }
    if (adapter->preedit_character_count != first + removed) {
        memcpy(
            new_feedback + first + replacement.character_count,
            adapter->preedit_feedback + first + removed,
            (adapter->preedit_character_count - first - removed) *
                sizeof(*new_feedback));
    }
    if (apply_current_xim_preedit(
            adapter,
            new_text,
            new_text_length,
            new_offsets,
            new_feedback,
            new_character_count,
            (size_t)draw->caret,
            true) == -1) {
        goto cleanup;
    }
    clear_xim_preedit(adapter);
    adapter->preedit_text = new_text;
    adapter->preedit_text_length = new_text_length;
    adapter->preedit_offsets = new_offsets;
    adapter->preedit_feedback = new_feedback;
    adapter->preedit_character_count = new_character_count;
    adapter->preedit_caret = (size_t)draw->caret;
    adapter->preedit_caret_visible = true;
    adapter->preedit_started = true;
    adapter->key_had_composition = true;
    new_text = NULL;
    new_offsets = NULL;
    new_feedback = NULL;
    result = 0;
    if (adapter->escape_preedit_pending) {
        adapter->escape_preedit_pending = false;
        if (adapter->preedit_character_count == 0) {
            result = boxrig_x11_input_adapter_preedit_cancel(adapter);
        }
    }

cleanup:
    decoded_xim_text_destroy(&replacement);
    free(new_text);
    free(new_offsets);
    free(new_feedback);
    return result;
}

int boxrig_x11_input_adapter_preedit_caret(
    boxrig_x11_input_adapter *adapter,
    XIMPreeditCaretCallbackStruct *caret)
{
    size_t position;
    bool visible;

    if (adapter == NULL || caret == NULL) {
        return -1;
    }
    note_xim_preedit_update(adapter);
    if (adapter->reset_preedit_tail_pending &&
        !adapter->preedit_started) {
        return 0;
    }
    if (!adapter->preedit_started) {
        return -1;
    }
    position = adapter->preedit_caret;
    switch (caret->direction) {
    case XIMForwardChar:
        if (position < adapter->preedit_character_count) {
            position++;
        }
        break;
    case XIMBackwardChar:
        if (position != 0) {
            position--;
        }
        break;
    case XIMForwardWord:
        if (position < adapter->preedit_character_count) {
            uint32_t feedback = adapter->preedit_feedback[position];

            position++;
            while (position < adapter->preedit_character_count &&
                   adapter->preedit_feedback[position] == feedback) {
                position++;
            }
        }
        break;
    case XIMBackwardWord:
        if (position != 0) {
            uint32_t feedback = adapter->preedit_feedback[position - 1];

            position--;
            while (position != 0 &&
                   adapter->preedit_feedback[position - 1] == feedback) {
                position--;
            }
        }
        break;
    case XIMLineStart:
        position = 0;
        break;
    case XIMLineEnd:
        position = adapter->preedit_character_count;
        break;
    case XIMAbsolutePosition:
        if (caret->position < 0 ||
            (size_t)caret->position > adapter->preedit_character_count) {
            return -1;
        }
        position = (size_t)caret->position;
        break;
    case XIMDontChange:
    case XIMCaretUp:
    case XIMCaretDown:
    case XIMNextLine:
    case XIMPreviousLine:
        break;
    }
    visible = caret->style != XIMIsInvisible;
    if (apply_current_xim_preedit(
            adapter,
            adapter->preedit_text,
            adapter->preedit_text_length,
            adapter->preedit_offsets,
            adapter->preedit_feedback,
            adapter->preedit_character_count,
            position,
            visible) == -1 ||
        position > INT_MAX) {
        return -1;
    }
    adapter->preedit_caret = position;
    adapter->preedit_caret_visible = visible;
    caret->position = (int)position;
    return 0;
}

int boxrig_x11_input_adapter_preedit_done(
    boxrig_x11_input_adapter *adapter)
{
    if (adapter != NULL) {
        note_xim_preedit_update(adapter);
    }
    if (adapter != NULL && adapter->reset_preedit_tail_pending &&
        !adapter->preedit_started) {
        adapter->reset_preedit_tail_pending = false;
        return 0;
    }
    if (adapter != NULL && adapter->escape_preedit_pending) {
        return boxrig_x11_input_adapter_preedit_cancel(adapter);
    }
    if (adapter == NULL || adapter->composition == NULL ||
        apply_empty_composition_event(
            adapter,
            BOXRIG_COMPOSITION_PREEDIT_COMPLETE) == -1) {
        return -1;
    }
    /* IBus can omit the next start callback and send a draw whose replacement
     * range still refers to the completed preedit buffer. Keep that buffer as
     * the hidden replacement base until a start, cancel, or reset. */
    adapter->preedit_started = false;
    return 0;
}

int boxrig_x11_input_adapter_preedit_cancel(
    boxrig_x11_input_adapter *adapter)
{
    if (adapter != NULL) {
        note_xim_preedit_update(adapter);
        invalidate_editable_input(adapter);
    }
    if (adapter == NULL || adapter->composition == NULL ||
        apply_empty_composition_event(
            adapter,
            BOXRIG_COMPOSITION_CANCEL) == -1) {
        return -1;
    }
    clear_xim_preedit(adapter);
    adapter->preedit_started = false;
    adapter->key_had_composition = false;
    adapter->escape_preedit_pending = false;
    adapter->reset_preedit_tail_pending = false;
    return 0;
}

int boxrig_x11_input_adapter_commit_utf8(
    boxrig_x11_input_adapter *adapter,
    const char *text,
    size_t text_length)
{
    boxrig_composition_event event;
    boxrig_composition_status status;

    if (adapter == NULL || adapter->composition == NULL ||
        (text == NULL && text_length != 0)) {
        return -1;
    }
    if (adapter->document_reset_pending) return 0;
    boxrig_composition_view current;
    if (boxrig_composition_get_view(adapter->composition, &current) != BOXRIG_COMPOSITION_OK ||
        current.commit_pending) return -1;
    adapter->commit_editable = boxrig_ui_editable_document_capabilities(
        adapter->editable_document).context_available;
    adapter->commit_generation = adapter->document_generation;
    note_xim_preedit_update(adapter);
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_COMMIT;
    event.text = text;
    event.text_length = text_length;
    status = boxrig_composition_apply_event(adapter->composition, &event);
    if (status != BOXRIG_COMPOSITION_OK &&
        status != BOXRIG_COMPOSITION_WOULD_BLOCK) {
        return -1;
    }
    /* IBus can likewise base the first draw after a commit on the committed
     * preedit buffer, so keep it hidden until the next replacement arrives. */
    adapter->preedit_started = false;
    adapter->key_had_composition = false;
    adapter->escape_preedit_pending = false;
    adapter->reset_preedit_tail_pending = false;
    return 0;
}

int boxrig_x11_input_adapter_synchronize_candidate(
    boxrig_x11_input_adapter *adapter)
{
    boxrig_composition_view view;
    XPoint spot;
    XVaNestedList attributes;
    int64_t spot_x;
    int64_t spot_y;

    if (adapter == NULL || adapter->composition == NULL ||
        boxrig_composition_get_view(adapter->composition, &view) !=
            BOXRIG_COMPOSITION_OK) {
        return -1;
    }
    if (adapter->preedit_started && !view.active) {
        reset_xim_input_context(adapter);
        clear_xim_preedit(adapter);
        adapter->preedit_started = false;
        adapter->escape_preedit_pending = false;
        adapter->reset_preedit_tail_pending = true;
    }
    if (!view.candidate.visible || adapter->input_context == NULL) {
        return 0;
    }
    spot_x = (int64_t)view.candidate.spot_x + adapter->viewport_x;
    spot_y = (int64_t)view.candidate.spot_y + adapter->viewport_y;
    if (spot_x < SHRT_MIN || spot_x > SHRT_MAX || spot_y < SHRT_MIN ||
        spot_y > SHRT_MAX) {
        return -1;
    }
    spot.x = (short)spot_x;
    spot.y = (short)spot_y;
    /* IBus can defer preedit callbacks while identical spot updates keep
     * arriving, so only submit an XIC value when the position changes. */
    if (adapter->candidate_spot_valid &&
        adapter->candidate_spot.x == spot.x &&
        adapter->candidate_spot.y == spot.y) {
        return 0;
    }
    attributes = XVaCreateNestedList(0, XNSpotLocation, &spot, NULL);
    if (attributes == NULL) {
        return -1;
    }
    /* Callback-style input methods may ignore XNSpotLocation. */
    (void)XSetICValues(
        adapter->input_context,
        XNPreeditAttributes,
        attributes,
        NULL);
    XFree(attributes);
    adapter->candidate_spot = spot;
    adapter->candidate_spot_valid = true;
    return 0;
}

static int xim_preedit_start_callback(
    XIM input_method,
    XPointer client_data,
    XPointer call_data)
{
    boxrig_x11_input_adapter *adapter =
        (boxrig_x11_input_adapter *)client_data;

    (void)call_data;
    if (!current_xim_context(adapter, input_method)) {
        return (int)boxrig_composition_limits_default().maximum_text_bytes;
    }
    if (boxrig_x11_input_adapter_preedit_start(adapter) == -1) {
        fprintf(stderr, "boxrig_term: XIM preedit start callback failed\n");
        adapter->callback_failed = true;
    }
    return (int)boxrig_composition_limits_default().maximum_text_bytes;
}

static void xim_preedit_done_callback(
    XIM input_method,
    XPointer client_data,
    XPointer call_data)
{
    boxrig_x11_input_adapter *adapter =
        (boxrig_x11_input_adapter *)client_data;

    (void)call_data;
    if (!current_xim_context(adapter, input_method)) {
        return;
    }
    if (boxrig_x11_input_adapter_preedit_done(adapter) == -1) {
        fprintf(stderr, "boxrig_term: XIM preedit done callback failed\n");
        adapter->callback_failed = true;
    }
}

static void xim_preedit_draw_callback(
    XIM input_method,
    XPointer client_data,
    XPointer call_data)
{
    boxrig_x11_input_adapter *adapter =
        (boxrig_x11_input_adapter *)client_data;
    XIMPreeditDrawCallbackStruct *draw =
        (XIMPreeditDrawCallbackStruct *)call_data;

    if (!current_xim_context(adapter, input_method)) {
        return;
    }
    if (boxrig_x11_input_adapter_preedit_draw(
            adapter,
            draw) == -1) {
        fprintf(
            stderr,
            "boxrig_term: XIM preedit draw callback failed "
            "(first=%d length=%d caret=%d text=%s text_length=%u "
            "encoding=%s feedback=%s)\n",
            draw == NULL ? -1 : draw->chg_first,
            draw == NULL ? -1 : draw->chg_length,
            draw == NULL ? -1 : draw->caret,
            draw == NULL || draw->text == NULL ? "null" : "present",
            draw == NULL || draw->text == NULL
                ? 0u
                : (unsigned int)draw->text->length,
            draw == NULL || draw->text == NULL
                ? "unknown"
                : draw->text->encoding_is_wchar ? "wide" : "multibyte",
            draw == NULL || draw->text == NULL ||
                    draw->text->feedback == NULL
                ? "null"
                : "present");
        adapter->callback_failed = true;
    }
}

static void xim_preedit_caret_callback(
    XIM input_method,
    XPointer client_data,
    XPointer call_data)
{
    boxrig_x11_input_adapter *adapter =
        (boxrig_x11_input_adapter *)client_data;

    if (!current_xim_context(adapter, input_method)) {
        return;
    }
    if (boxrig_x11_input_adapter_preedit_caret(
            adapter,
            (XIMPreeditCaretCallbackStruct *)call_data) == -1) {
        fprintf(stderr, "boxrig_term: XIM preedit caret callback failed\n");
        adapter->callback_failed = true;
    }
}

static XIMStyle select_input_style(
    const XIMStyles *styles,
    bool callbacks)
{
    size_t index;

    if (styles == NULL) {
        return 0;
    }
    for (index = 0; index < styles->count_styles; index++) {
        XIMStyle style = styles->supported_styles[index];

        if (callbacks &&
            (style & XIMPreeditCallbacks) != 0 &&
            (style & XIMStatusNothing) != 0) {
            return style;
        }
        if (!callbacks &&
            (style & XIMPreeditNothing) != 0 &&
            (style & XIMStatusNothing) != 0) {
            return style;
        }
    }
    return 0;
}

static int create_input_context(boxrig_x11_input_adapter *adapter)
{
    XIMStyles *styles = NULL;
    XIMValuesList *values = NULL;
    XIMStyle style;
    XVaNestedList preedit_attributes = NULL;
    char *unsupported;

    /* Query the native styles and the locally described optional values in
     * one call. libX11 1.8.7 leaks its wire buffer for a local-only query. */
    unsupported = XGetIMValues(
        adapter->input_method,
        XNQueryInputStyle,
        &styles,
        XNQueryICValuesList,
        &values,
        NULL);
    if (styles == NULL) {
        /* An unavailable optional query must not disable ordinary XIM input. */
        (void)XGetIMValues(adapter->input_method, XNQueryInputStyle, &styles, NULL);
    }
    style = select_input_style(styles, true);
    if (style != 0) {
        xim_start_callback_pointer start_callback;

        adapter->preedit_start_callback.client_data = (XPointer)adapter;
        start_callback.start = xim_preedit_start_callback;
        adapter->preedit_start_callback.callback = start_callback.generic;
        adapter->preedit_done_callback.client_data = (XPointer)adapter;
        adapter->preedit_done_callback.callback = xim_preedit_done_callback;
        adapter->preedit_draw_callback.client_data = (XPointer)adapter;
        adapter->preedit_draw_callback.callback = xim_preedit_draw_callback;
        adapter->preedit_caret_callback.client_data = (XPointer)adapter;
        adapter->preedit_caret_callback.callback = xim_preedit_caret_callback;
        preedit_attributes = XVaCreateNestedList(
            0,
            XNPreeditStartCallback,
            &adapter->preedit_start_callback,
            XNPreeditDoneCallback,
            &adapter->preedit_done_callback,
            XNPreeditDrawCallback,
            &adapter->preedit_draw_callback,
            XNPreeditCaretCallback,
            &adapter->preedit_caret_callback,
            NULL);
        if (preedit_attributes != NULL) {
            adapter->input_context = XCreateIC(
                adapter->input_method,
                XNInputStyle,
                style,
                XNClientWindow,
                adapter->client_window,
                XNFocusWindow,
                adapter->window,
                XNPreeditAttributes,
                preedit_attributes,
                NULL);
            XFree(preedit_attributes);
        }
    }
    if (adapter->input_context == NULL) {
        style = select_input_style(styles, false);
        if (style != 0) {
            adapter->input_context = XCreateIC(
                adapter->input_method,
                XNInputStyle,
                style,
                XNClientWindow,
                adapter->client_window,
                XNFocusWindow,
                adapter->window,
                NULL);
        }
    }
    if (styles != NULL) {
        XFree(styles);
    }
    adapter->string_conversion_supported = false;
    if (adapter->input_context != NULL) {
        if (unsupported == NULL && values != NULL) {
            for (unsigned short index = 0; index < values->count_values; index++) {
                if (strcmp(values->supported_values[index], XNStringConversionCallback) != 0)
                    continue;
                adapter->string_conversion_callback.client_data = (XPointer)adapter;
                adapter->string_conversion_callback.callback = xim_string_conversion_callback;
                adapter->string_conversion_supported = XSetICValues(adapter->input_context,
                    XNStringConversionCallback, &adapter->string_conversion_callback, NULL) == NULL;
                break;
            }
        }
    }
    if (values != NULL) XFree(values);
    return adapter->input_context == NULL ? -1 : 0;
}

static void refresh_editable_context(boxrig_x11_input_adapter *adapter)
{
    uint64_t generation =
        boxrig_ui_editable_document_input_generation(adapter->editable_document);
    if (!adapter->document_reset_pending && generation == adapter->document_generation) return;
    if (adapter->commit_editable)
        (void)boxrig_composition_cancel_pending_commit(adapter->composition);
    boxrig_composition_event cancel = {.type = BOXRIG_COMPOSITION_CANCEL};
    (void)boxrig_composition_apply_event(adapter->composition, &cancel);
    clear_xim_preedit(adapter);
    adapter->preedit_started = false;
    adapter->key_had_composition = false;
    adapter->escape_preedit_pending = false;
    adapter->reset_preedit_tail_pending = false;
    if (adapter->input_context != NULL) {
        /* Reset/destroy can dispatch native callbacks. Keep them excluded until
         * a fresh XIC owns the new document generation. Old wire IC IDs then
         * cannot deliver a commit into the replacement document. */
        adapter->document_reset_pending = true;
        if (adapter->input_focused) XUnsetICFocus(adapter->input_context);
        reset_xim_input_context(adapter);
        XIC old = adapter->input_context;
        adapter->input_context = NULL;
        XDestroyIC(old);
    }
    adapter->string_conversion_supported = false;
    adapter->candidate_spot_valid = false;
    adapter->document_generation = generation;
    if (adapter->input_method != NULL) (void)create_input_context(adapter);
    adapter->document_reset_pending = false;
    if (adapter->input_focused && adapter->input_context != NULL)
        XSetICFocus(adapter->input_context);
}

int boxrig_x11_input_adapter_create(
    Display *display,
    Window client_window,
    Window focus_window,
    boxrig_terminal *terminal,
    unsigned int cell_width,
    unsigned int cell_height,
    size_t paste_capacity,
    size_t paste_chunk_size,
    boxrig_x11_input_adapter **adapter_out)
{
    boxrig_x11_input_adapter *adapter;
    int opcode;
    int event_base;
    int error_base;
    int major = XkbMajorVersion;
    int minor = XkbMinorVersion;
    Bool repeat_supported = False;
    size_t property_byte_limit;

    if (display == NULL || client_window == None || focus_window == None ||
        terminal == NULL ||
        cell_width == 0 || cell_height == 0 || paste_chunk_size == 0 ||
        adapter_out == NULL) {
        return -1;
    }
    *adapter_out = NULL;
    adapter = calloc(1, sizeof(*adapter));
    if (adapter == NULL) {
        return -1;
    }
    adapter->display = display;
    adapter->client_window = client_window;
    adapter->window = focus_window;
    adapter->terminal = terminal;
    adapter->cell_width = cell_width;
    adapter->cell_height = cell_height;
    adapter->paste_capacity = paste_capacity;
    adapter->paste_chunk_size = paste_chunk_size;
    property_byte_limit = selection_property_byte_limit(display);
    if (property_byte_limit == 0) {
        free(adapter);
        return -1;
    }
    adapter->selection_transfer_chunk_size =
        property_byte_limit < DEFAULT_SELECTION_TRANSFER_CHUNK_SIZE
            ? property_byte_limit
            : DEFAULT_SELECTION_TRANSFER_CHUNK_SIZE;
    adapter->maximum_active_selection_transfers =
        DEFAULT_MAXIMUM_ACTIVE_SELECTION_TRANSFERS;
    adapter->maximum_foreign_window_watches =
        DEFAULT_MAXIMUM_ACTIVE_SELECTION_TRANSFERS + 1;
    adapter->maximum_selection_transfer_bytes =
        DEFAULT_MAXIMUM_SELECTION_TRANSFER_BYTES;
    adapter->input_window_alive = true;
    adapter->primary_offer.selection = XA_PRIMARY;
    adapter->outgoing_transfers = calloc(
        adapter->maximum_active_selection_transfers,
        sizeof(*adapter->outgoing_transfers));
    adapter->foreign_window_watches = calloc(
        adapter->maximum_foreign_window_watches,
        sizeof(*adapter->foreign_window_watches));
    if (adapter->outgoing_transfers == NULL ||
        adapter->foreign_window_watches == NULL) {
        free(adapter->foreign_window_watches);
        free(adapter->outgoing_transfers);
        free(adapter);
        return -1;
    }
    if (boxrig_composition_create(
            terminal,
            boxrig_composition_limits_default(),
            &adapter->composition) != BOXRIG_COMPOSITION_OK) {
        free(adapter->foreign_window_watches);
        free(adapter->outgoing_transfers);
        free(adapter);
        return -1;
    }
    (void)boxrig_composition_set_commit_sink(adapter->composition, commit_ime_text, adapter);
    adapter->xkb_available = XkbQueryExtension(
        display,
        &opcode,
        &event_base,
        &error_base,
        &major,
        &minor);
    if (adapter->xkb_available) {
        (void)XkbSetDetectableAutoRepeat(
            display,
            True,
            &repeat_supported);
        adapter->detectable_repeat = repeat_supported != False;
    }
    adapter->alt_mask = modifier_mask(adapter, XK_Alt_L, XK_Alt_R);
    adapter->super_mask = modifier_mask(adapter, XK_Super_L, XK_Super_R);
    adapter->hyper_mask = modifier_mask(adapter, XK_Hyper_L, XK_Hyper_R);
    adapter->meta_mask = modifier_mask(adapter, XK_Meta_L, XK_Meta_R);
    adapter->caps_lock_mask = modifier_mask(
        adapter,
        XK_Caps_Lock,
        XK_Caps_Lock);
    adapter->num_lock_mask = modifier_mask(
        adapter,
        XK_Num_Lock,
        XK_Num_Lock);
    if (adapter->alt_mask == 0) {
        adapter->alt_mask = Mod1Mask;
    }
    if (adapter->super_mask == 0) {
        adapter->super_mask = Mod4Mask;
    }
    adapter->super_mask &= ~adapter->alt_mask;
    adapter->hyper_mask &= ~(adapter->alt_mask | adapter->super_mask);
    adapter->meta_mask &= ~(adapter->alt_mask | adapter->super_mask |
                            adapter->hyper_mask);
    adapter->clipboard = XInternAtom(display, "CLIPBOARD", False);
    adapter->targets = XInternAtom(display, "TARGETS", False);
    adapter->utf8_string = XInternAtom(display, "UTF8_STRING", False);
    adapter->incr = XInternAtom(display, "INCR", False);
    adapter->timestamp = XInternAtom(display, "TIMESTAMP", False);
    adapter->paste_property = XInternAtom(display, "_BOXRIG_PASTE", False);
    adapter->timestamp_property =
        XInternAtom(display, "_BOXRIG_TIMESTAMP", False);
    adapter->clipboard_offer.selection = adapter->clipboard;
    if (adapter->clipboard == None || adapter->targets == None ||
        adapter->utf8_string == None || adapter->incr == None ||
        adapter->timestamp == None || adapter->paste_property == None ||
        adapter->timestamp_property == None) {
        boxrig_composition_destroy(adapter->composition);
        free(adapter->foreign_window_watches);
        free(adapter->outgoing_transfers);
        free(adapter);
        return -1;
    }
    adapter->input_method = XOpenIM(display, NULL, NULL, NULL);
    if (adapter->input_method != NULL) {
        (void)create_input_context(adapter);
    }
    *adapter_out = adapter;
    return 0;
}

int boxrig_x11_input_adapter_set_editable_document(
    boxrig_x11_input_adapter *adapter,
    boxrig_ui_editable_document *document)
{
    if (adapter == NULL) return -1;
    if (adapter->editable_document == document) return 0;
    if (boxrig_ui_editable_document_capabilities(adapter->editable_document).busy ||
        boxrig_ui_editable_document_capabilities(document).busy) return -1;
    invalidate_editable_input(adapter);
    adapter->editable_document = document;
    adapter->document_reset_pending = true;
    refresh_editable_context(adapter);
    return 0;
}

bool boxrig_x11_input_adapter_string_conversion_supported(
    const boxrig_x11_input_adapter *adapter)
{
    return adapter != NULL && adapter->string_conversion_supported;
}

boxrig_ui_status boxrig_x11_input_adapter_set_input_config(
    boxrig_x11_input_adapter *adapter, const boxrig_input_config *config)
{
    if (adapter == NULL || config == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    boxrig_composition_view composition;
    if (boxrig_composition_get_view(adapter->composition, &composition) != BOXRIG_COMPOSITION_OK) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (adapter->dispatching || adapter->pending.kind != PENDING_INPUT_NONE ||
        adapter->paste_active || composition.active || composition.commit_pending ||
        adapter->preedit_started || adapter->document_reset_pending ||
        adapter->reset_preedit_tail_pending || adapter->escape_preedit_pending ||
        adapter->selection_receive_state != SELECTION_RECEIVE_IDLE) {
        return BOXRIG_UI_BUSY;
    }
    for (size_t i = 0u; i < KEYCODE_COUNT; i++) {
        if (adapter->pressed[i]) return BOXRIG_UI_BUSY;
    }
    boxrig_status status = boxrig_terminal_set_input_config(adapter->terminal, config);
    if (status == BOXRIG_STATUS_OK) return BOXRIG_UI_OK;
    if (status == BOXRIG_STATUS_OUT_OF_MEMORY) return BOXRIG_UI_OUT_OF_MEMORY;
    if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) return BOXRIG_UI_LIMIT_EXCEEDED;
    return BOXRIG_UI_INVALID_ARGUMENT;
}

int boxrig_x11_input_adapter_set_key_router(
    boxrig_x11_input_adapter *adapter,
    boxrig_input_key_router router,
    void *context)
{
    size_t keycode;

    if (adapter == NULL || (router == NULL && context != NULL)) {
        return -1;
    }
    for (keycode = 0; keycode < KEYCODE_COUNT; keycode++) {
        if (adapter->pressed[keycode]) {
            return -1;
        }
    }
    adapter->key_router = router;
    adapter->key_router_context = context;
    memset(adapter->application_key, 0, sizeof(adapter->application_key));
    return 0;
}

int boxrig_x11_input_adapter_set_mouse_router(
    boxrig_x11_input_adapter *adapter,
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
        adapter->pressed_buttons != 0 ||
        adapter->application_buttons != 0) {
        return -1;
    }
    adapter->mouse_view = view;
    adapter->mouse_router = router;
    adapter->mouse_router_context = context;
    return 0;
}

boxrig_ui_view_action_capabilities
boxrig_x11_input_adapter_view_action_capabilities(
    const boxrig_x11_input_adapter *adapter)
{
    boxrig_ui_view_action_capabilities capabilities;

    memset(&capabilities, 0, sizeof(capabilities));
    if (adapter != NULL) {
        capabilities.copy_targets =
            BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION |
            BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
        capabilities.paste_targets =
            BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION |
            BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    }
    return capabilities;
}

boxrig_ui_status boxrig_x11_input_adapter_apply_view_action(
    boxrig_x11_input_adapter *adapter,
    boxrig_ui_view *view,
    const boxrig_ui_view_action *action)
{
    boxrig_ui_view_action_capabilities capabilities;
    Atom selection;

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
    capabilities =
        boxrig_x11_input_adapter_view_action_capabilities(adapter);
    if (!boxrig_ui_view_action_supported(&capabilities, action)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    selection = action->clipboard_target ==
                        BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION
                    ? XA_PRIMARY
                    : adapter->clipboard;
    if (action->kind == BOXRIG_UI_VIEW_ACTION_COPY) {
        clipboard_payload *payload = NULL;
        clipboard_offer *offer;
        boxrig_ui_status status;

        if (view == NULL || !adapter->input_window_alive) {
            return BOXRIG_UI_INVALID_ARGUMENT;
        }
        status = create_clipboard_payload(view, &payload);
        if (status != BOXRIG_UI_OK) {
            return status;
        }
        offer = find_clipboard_offer(adapter, selection);
        clipboard_payload_release(offer->pending_payload);
        offer->pending_payload = payload;
        offer->acquisition_pending = true;
        if (request_server_timestamp(adapter) == -1) {
            clipboard_payload_release(offer->pending_payload);
            offer->pending_payload = NULL;
            offer->acquisition_pending = false;
            return BOXRIG_UI_UNAVAILABLE;
        }
        return BOXRIG_UI_OK;
    }
    if (adapter->selection_receive_state != SELECTION_RECEIVE_IDLE ||
        adapter->paste_active) {
        return BOXRIG_UI_BUSY;
    }
    adapter->selection_receive_state = SELECTION_RECEIVE_TIMESTAMP;
    adapter->selection = selection;
    if (request_server_timestamp(adapter) == -1) {
        reset_incoming_selection(adapter, true, false);
        return BOXRIG_UI_UNAVAILABLE;
    }
    return BOXRIG_UI_OK;
}

int boxrig_x11_input_adapter_set_clipboard_transfer_limits(
    boxrig_x11_input_adapter *adapter,
    size_t chunk_size,
    size_t maximum_active_transfers,
    size_t maximum_aggregate_bytes)
{
    outgoing_selection_transfer *transfers;
    foreign_window_watch *watches;
    size_t property_byte_limit;

    if (adapter == NULL || chunk_size == 0 ||
        maximum_active_transfers == 0 || maximum_aggregate_bytes == 0 ||
        maximum_active_transfers == SIZE_MAX ||
        !checked_array_size(
            maximum_active_transfers,
            sizeof(*transfers)) ||
        !checked_array_size(
            maximum_active_transfers + 1,
            sizeof(*watches)) ||
        adapter->active_selection_transfer_count != 0 ||
        adapter->selection_receive_state != SELECTION_RECEIVE_IDLE) {
        return -1;
    }
    property_byte_limit = selection_property_byte_limit(adapter->display);
    if (property_byte_limit == 0) {
        return -1;
    }
    transfers = calloc(maximum_active_transfers, sizeof(*transfers));
    watches = calloc(maximum_active_transfers + 1, sizeof(*watches));
    if (transfers == NULL || watches == NULL) {
        free(watches);
        free(transfers);
        return -1;
    }
    free(adapter->foreign_window_watches);
    free(adapter->outgoing_transfers);
    adapter->outgoing_transfers = transfers;
    adapter->foreign_window_watches = watches;
    adapter->selection_transfer_chunk_size =
        chunk_size < property_byte_limit ? chunk_size : property_byte_limit;
    adapter->maximum_active_selection_transfers =
        maximum_active_transfers;
    adapter->maximum_foreign_window_watches =
        maximum_active_transfers + 1;
    adapter->maximum_selection_transfer_bytes = maximum_aggregate_bytes;
    return 0;
}

int boxrig_x11_input_adapter_quiesce(boxrig_x11_input_adapter *adapter)
{
    size_t index;

    if (adapter == NULL) {
        return 0;
    }
    adapter->input_focused = false;
    invalidate_editable_input(adapter);
    if (adapter->input_window_alive) {
        if (adapter->primary_offer.owned &&
            XGetSelectionOwner(adapter->display, XA_PRIMARY) ==
            adapter->window) {
            XSetSelectionOwner(
                adapter->display,
                XA_PRIMARY,
                None,
                adapter->primary_offer.timestamp);
        }
        if (adapter->clipboard_offer.owned &&
            XGetSelectionOwner(adapter->display, adapter->clipboard) ==
            adapter->window) {
            XSetSelectionOwner(
                adapter->display,
                adapter->clipboard,
                None,
                adapter->clipboard_offer.timestamp);
        }
        XDeleteProperty(
            adapter->display,
            adapter->window,
            adapter->timestamp_property);
    }
    adapter->server_timestamp_pending = false;
    clear_clipboard_offer(adapter, &adapter->primary_offer);
    clear_clipboard_offer(adapter, &adapter->clipboard_offer);
    if (adapter->selection_receive_state != SELECTION_RECEIVE_IDLE) {
        reset_incoming_selection(adapter, true, adapter->input_window_alive);
    }
    for (index = 0;
         index < adapter->maximum_active_selection_transfers;
         index++) {
        release_outgoing_transfer(
            adapter,
            &adapter->outgoing_transfers[index],
            true);
    }
    if (adapter->foreign_window_watches != NULL) {
        for (index = 0;
             index < adapter->maximum_foreign_window_watches;
             index++) {
            foreign_window_watch *watch =
                &adapter->foreign_window_watches[index];

            if (watch->active) {
                if (adapter->input_window_alive) {
                    XSelectInput(
                        adapter->display,
                        watch->window,
                        watch->original_event_mask);
                }
                memset(watch, 0, sizeof(*watch));
            }
        }
    }
    XSync(adapter->display, False);
    return 0;
}

int boxrig_x11_input_adapter_set_cell_size(
    boxrig_x11_input_adapter *adapter,
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

int boxrig_x11_input_adapter_set_viewport_origin(
    boxrig_x11_input_adapter *adapter,
    int x,
    int y)
{
    if (adapter == NULL) {
        return -1;
    }
    adapter->viewport_x = x;
    adapter->viewport_y = y;
    adapter->candidate_spot_valid = false;
    return 0;
}

static int recover_filtered_final_backspace(
    boxrig_x11_input_adapter *adapter,
    const XKeyEvent *event)
{
    boxrig_composition_view view;
    bool preedit_updated;

    if (event->type != KeyRelease ||
        adapter->filtered_final_backspace_keycode != event->keycode) {
        return 0;
    }
    preedit_updated = adapter->filtered_final_backspace_preedit_updated;
    adapter->filtered_final_backspace_keycode = 0;
    adapter->filtered_final_backspace_preedit_updated = false;
    if (preedit_updated) {
        return 0;
    }
    if (boxrig_composition_get_view(adapter->composition, &view) !=
        BOXRIG_COMPOSITION_OK) {
        return -1;
    }
    if (!adapter->preedit_started ||
        !composition_view_is_final_unconverted_preedit(&view)) {
        return 0;
    }
    /* IBus/Mozc can consume the final Backspace and hide its candidate popup
     * without sending the empty draw callback. Recover only when the same
     * single unconverted cluster survived the complete key stroke unchanged. */
    reset_xim_input_context(adapter);
    if (boxrig_x11_input_adapter_preedit_cancel(adapter) == -1) {
        return -1;
    }
    adapter->reset_preedit_tail_pending = true;
    adapter->composition_key[event->keycode] = false;
    adapter->pressed[event->keycode] = false;
    adapter->key_had_composition = false;
    return 1;
}

static bool event_targets_input_window(
    const XEvent *event,
    Window window)
{
    switch (event->type) {
    case SelectionNotify:
        return event->xselection.requestor == window;
    case SelectionRequest:
        return event->xselectionrequest.owner == window;
    case SelectionClear:
        return event->xselectionclear.window == window;
    default:
        return event->xany.window == window;
    }
}

static int process_clipboard_event(
    boxrig_x11_input_adapter *adapter,
    const XEvent *event)
{
    int result;

    switch (event->type) {
    case SelectionRequest:
        return process_selection_request(
            adapter,
            &event->xselectionrequest);
    case SelectionNotify:
        return process_selection_notify(adapter, &event->xselection);
    case SelectionClear:
        return process_selection_clear(adapter, &event->xselectionclear);
    case PropertyNotify:
        result = process_server_timestamp(adapter, &event->xproperty);
        if (result != 0) {
            return result;
        }
        result = process_outgoing_property_notify(
            adapter,
            &event->xproperty);
        return result != 0
                   ? result
                   : process_incoming_property_notify(
                         adapter,
                         &event->xproperty);
    case DestroyNotify:
        return process_destroy_notify(adapter, &event->xdestroywindow);
    default:
        return 0;
    }
}

static int process_event(boxrig_x11_input_adapter *adapter, const XEvent *event);

int boxrig_x11_input_adapter_process_event(boxrig_x11_input_adapter *adapter, const XEvent *event)
{
    if (adapter == NULL) return -1;
    bool was_dispatching = adapter->dispatching;
    adapter->dispatching = true;
    int result = process_event(adapter, event);
    adapter->dispatching = was_dispatching;
    return result;
}

static int process_event(
    boxrig_x11_input_adapter *adapter,
    const XEvent *event)
{
    XEvent copy;
    Bool filtered;
    bool blocked;
    bool escape_started_with_composition = false;
    bool escape_cancels_unconverted_preedit = false;
    bool backspace_started_with_final_preedit = false;
    int route_result;

    if (adapter == NULL || event == NULL) {
        return -1;
    }
    if (event->type == FocusOut && event->xfocus.window == adapter->window) {
        adapter->input_focused = false;
        invalidate_editable_input(adapter);
    }
    refresh_editable_context(adapter);
    route_result = process_clipboard_event(adapter, event);
    if (route_result != 0) {
        return route_result;
    }
    copy = *event;
    if (!event_targets_input_window(event, adapter->window)) {
        filtered = XFilterEvent(&copy, None);
        if (adapter->callback_failed) {
            adapter->callback_failed = false;
            return -1;
        }
        return filtered ? 1 : 0;
    }
    blocked = boxrig_x11_input_adapter_blocked(adapter);
    if (!blocked && event->type == KeyPress) {
        key_identity identity = identify_key(adapter, &copy.xkey);

        adapter->filtered_final_backspace_keycode = 0;
        adapter->filtered_final_backspace_preedit_updated = false;
        adapter->key_had_composition = adapter->preedit_started;
        if (identity.key == BOXRIG_KEY_BACKSPACE &&
            adapter->preedit_started) {
            boxrig_composition_view view;

            if (boxrig_composition_get_view(adapter->composition, &view) !=
                BOXRIG_COMPOSITION_OK) {
                return -1;
            }
            backspace_started_with_final_preedit =
                composition_view_is_final_unconverted_preedit(&view);
        }
        if (identity.key == BOXRIG_KEY_ESCAPE &&
            adapter->preedit_started) {
            boxrig_composition_view view;

            if (boxrig_composition_get_view(adapter->composition, &view) !=
                BOXRIG_COMPOSITION_OK) {
                return -1;
            }
            escape_cancels_unconverted_preedit =
                composition_view_is_unconverted_preedit(&view);
            escape_started_with_composition = true;
            adapter->escape_preedit_pending = true;
        } else if (identity.key != BOXRIG_KEY_ESCAPE) {
            adapter->escape_preedit_pending = false;
        }
    }
    if (!blocked &&
        (event->type == KeyPress || event->type == KeyRelease)) {
        if (event->type == KeyPress &&
            !adapter->pressed[event->xkey.keycode]) {
            adapter->application_key[event->xkey.keycode] = false;
        }
        route_result = route_key_event(adapter, &copy.xkey);
        if (route_result != 0) {
            return route_result;
        }
    }
    if (backspace_started_with_final_preedit) {
        adapter->filtered_final_backspace_keycode = event->xkey.keycode;
        adapter->filtered_final_backspace_preedit_updated = false;
    }
    filtered = XFilterEvent(&copy, None);
    if (adapter->document_reset_pending) return 1;
    if (event->type == KeyPress &&
        adapter->filtered_final_backspace_keycode == event->xkey.keycode &&
        !filtered) {
        adapter->filtered_final_backspace_keycode = 0;
        adapter->filtered_final_backspace_preedit_updated = false;
    }
    if (event->type == KeyPress && adapter->preedit_started) {
        adapter->key_had_composition = true;
    }
    if (adapter->callback_failed) {
        adapter->callback_failed = false;
        adapter->key_had_composition = false;
        adapter->filtered_final_backspace_keycode = 0;
        adapter->filtered_final_backspace_preedit_updated = false;
        return -1;
    }
    route_result = recover_filtered_final_backspace(adapter, &copy.xkey);
    if (route_result != 0) {
        return route_result;
    }
    if (event->type == KeyPress && escape_started_with_composition) {
        boxrig_composition_view view;

        if (boxrig_composition_get_view(adapter->composition, &view) !=
            BOXRIG_COMPOSITION_OK) {
            return -1;
        }
        /* The first Escape may replace converted feedback with an
         * unconverted preedit, which remains visible. Some XIM servers then
         * consume the canceling Escape without a final draw or done callback.
         * Reset that native preedit and converge the common overlay here. */
        if (escape_cancels_unconverted_preedit) {
            reset_xim_input_context(adapter);
            if (boxrig_x11_input_adapter_preedit_cancel(adapter) == -1) {
                return -1;
            }
            adapter->reset_preedit_tail_pending = true;
        } else if (!adapter->preedit_started ||
            (view.active && view.text_length == 0)) {
            if (boxrig_x11_input_adapter_preedit_cancel(adapter) == -1) {
                return -1;
            }
        }
        adapter->composition_key[event->xkey.keycode] = true;
        adapter->key_had_composition = false;
        return 1;
    }
    if (filtered) {
        if (event->type == KeyPress) {
            adapter->key_had_composition = false;
        } else if (event->type == KeyRelease) {
            adapter->composition_key[event->xkey.keycode] = false;
            adapter->pressed[event->xkey.keycode] = false;
        }
        return 1;
    }
    if (blocked) {
        return 0;
    }
    switch (event->type) {
    case KeyPress: {
        int result = process_key_press(adapter, &copy.xkey);

        adapter->key_had_composition = false;
        return result;
    }
    case KeyRelease:
        return process_key_release(adapter, &copy.xkey);
    case FocusIn: {
        boxrig_focus_event focus = {true};

        adapter->input_focused = true;
        if (adapter->input_context != NULL) {
            XSetICFocus(adapter->input_context);
        }
        if (boxrig_composition_focus_changed(
                adapter->composition,
                true) != BOXRIG_COMPOSITION_OK) {
            return -1;
        }
        return queue_focus(adapter, &focus) == 0 ? 1 : -1;
    }
    case FocusOut: {
        boxrig_focus_event focus = {false};

        adapter->input_focused = false;
        if (adapter->input_context != NULL) {
            reset_xim_input_context(adapter);
            XUnsetICFocus(adapter->input_context);
        }
        if (boxrig_composition_focus_changed(
                adapter->composition,
                false) != BOXRIG_COMPOSITION_OK) {
            return -1;
        }
        clear_xim_preedit(adapter);
        adapter->preedit_started = false;
        adapter->escape_preedit_pending = false;
        adapter->reset_preedit_tail_pending = true;
        adapter->filtered_final_backspace_keycode = 0;
        adapter->filtered_final_backspace_preedit_updated = false;
        memset(adapter->pressed, 0, sizeof(adapter->pressed));
        memset(adapter->composition_key, 0, sizeof(adapter->composition_key));
        adapter->pressed_buttons = 0;
        adapter->application_buttons = 0;
        adapter->last_click_valid = false;
        return queue_focus(adapter, &focus) == 0 ? 1 : -1;
    }
    case ButtonPress:
    case ButtonRelease:
        return process_button_event(adapter, &event->xbutton);
    case MotionNotify:
        return process_motion_event(adapter, &event->xmotion);
    default:
        return 0;
    }
}

int boxrig_x11_input_adapter_queue_paste(
    boxrig_x11_input_adapter *adapter,
    const uint8_t *bytes,
    size_t length)
{
    char *normalized;
    size_t normalized_length;

    if (adapter == NULL || adapter->paste_active ||
        adapter->selection_receive_state != SELECTION_RECEIVE_IDLE ||
        boxrig_input_text_normalize_utf8(
            bytes,
            length,
            adapter->paste_capacity,
            &normalized,
            &normalized_length) == -1) {
        return -1;
    }
    adapter->paste_active = true;
    adapter->paste_state = PASTE_STATE_BEGIN;
    adapter->paste_text = normalized;
    adapter->paste_length = normalized_length;
    adapter->paste_offset = 0;
    return boxrig_x11_input_adapter_retry(adapter);
}

int boxrig_x11_input_adapter_retry(boxrig_x11_input_adapter *adapter)
{
    boxrig_composition_status composition_status;

    if (adapter == NULL) return -1;
    refresh_editable_context(adapter);
    if (retry_pending_input(adapter) == -1) {
        return -1;
    }
    if (adapter->pending.kind != PENDING_INPUT_NONE) {
        return 0;
    }
    composition_status = boxrig_composition_retry_commit(
        adapter->composition);
    if (composition_status == BOXRIG_COMPOSITION_WOULD_BLOCK) {
        return 0;
    }
    if (composition_status != BOXRIG_COMPOSITION_OK) {
        return -1;
    }
    return retry_paste(adapter);
}

bool boxrig_x11_input_adapter_blocked(
    const boxrig_x11_input_adapter *adapter)
{
    boxrig_composition_view view;

    if (adapter == NULL) {
        return false;
    }
    if (adapter->pending.kind != PENDING_INPUT_NONE ||
        adapter->paste_active) {
        return true;
    }
    return boxrig_composition_get_view(adapter->composition, &view) ==
               BOXRIG_COMPOSITION_OK &&
           view.commit_pending;
}

void boxrig_x11_input_adapter_destroy(boxrig_x11_input_adapter *adapter)
{
    if (adapter == NULL) {
        return;
    }
    (void)boxrig_x11_input_adapter_quiesce(adapter);
    if (adapter->input_context != NULL) {
        adapter->resetting_preedit = true;
        XDestroyIC(adapter->input_context);
        adapter->resetting_preedit = false;
    }
    if (adapter->input_method != NULL) {
        (void)XCloseIM(adapter->input_method);
    }
    if (adapter->composition != NULL) {
        (void)boxrig_composition_terminate(adapter->composition);
    }
    clear_xim_preedit(adapter);
    boxrig_composition_destroy(adapter->composition);
    free(adapter->pending.key_text);
    free(adapter->paste_text);
    free(adapter->selection_bytes);
    free(adapter->foreign_window_watches);
    free(adapter->outgoing_transfers);
    free(adapter);
}
