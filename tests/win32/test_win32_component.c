/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "boxrig/win32.h"
#include "boxrig/win32/component.h"
#ifdef BOXRIG_TEST_OMIT_OPTIONAL_INTERFACES
#include "../../src/win32/renderer_private.h"
#endif

#include <windows.h>
#include <imm.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static unsigned int omitted_interface;
static unsigned int omitted_interface_queries;

#ifdef BOXRIG_TEST_OMIT_OPTIONAL_INTERFACES
static HRESULT omit_bitmap_target1(IDWriteBitmapRenderTarget *target,
    IDWriteBitmapRenderTarget1 **out)
{
    (void)target;
    *out = NULL;
    omitted_interface_queries++;
    return E_NOINTERFACE;
}

static HRESULT omit_factory2(IDWriteFactory *factory, IDWriteFactory2 **out)
{
    (void)factory;
    *out = NULL;
    omitted_interface_queries++;
    return E_NOINTERFACE;
}

/* Run the complete component suite through the real opaque renderer even when
 * the optional interfaces used for alpha and color glyphs are absent. */
int __wrap_boxrig_win32_renderer_create(HDC reference,
    boxrig_win32_renderer **out)
{
    boxrig_win32_renderer_system system = boxrig_win32_renderer_system_default();
    if (omitted_interface == 1) system.query_bitmap_render_target1 = omit_bitmap_target1;
    if (omitted_interface == 2) system.query_factory2 = omit_factory2;
    return boxrig_win32_renderer_create_with_system(reference, &system, out);
}
#define OPTIONAL_INTERFACE_SCENARIOS 3
#else
#define OPTIONAL_INTERFACE_SCENARIOS 1
#endif

static bool feed_text(boxrig_terminal *terminal, const char *text)
{
    size_t consumed = 0;
    size_t length = strlen(text);

    return boxrig_terminal_feed(
               terminal,
               (const uint8_t *)text,
               length,
               &consumed) == BOXRIG_STATUS_OK &&
           consumed == length;
}

static bool take_content_size_action(
    boxrig_win32_terminal_component *component,
    boxrig_ui_size_command *command_out)
{
    boxrig_ui_layout_action action;

    if (!boxrig_win32_terminal_component_take_layout_action(
            component, &action) ||
        action.kind != BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE) {
        return false;
    }
    command_out->revision = action.revision;
    command_out->cause = action.value.content_size.cause;
    command_out->width = action.value.content_size.width;
    command_out->height = action.value.content_size.height;
    return true;
}

static bool constrained_zoom_uses_one_layout_action(void)
{
    HWND parent;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_win32_terminal_component_config config =
        boxrig_win32_terminal_component_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_win32_terminal_component *component = NULL;
    boxrig_font_effective_metrics before;
    boxrig_font_effective_metrics after;
    boxrig_ui_component_allocation allocation;
    boxrig_ui_component_frame_result frame;
    boxrig_ui_layout_result layout;
    boxrig_ui_layout_action action;
    boxrig_ui_size_command_state size_state;
    size_t rows;
    size_t columns;
    bool changed = false;

    parent = CreateWindowExW(
        0,
        L"STATIC",
        L"constrained zoom host",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        640,
        480,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(parent != NULL);
    terminal_config.rows = 12;
    terminal_config.columns = 40;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    CHECK(boxrig_win32_terminal_component_create_child(
              GetModuleHandleW(NULL),
              parent,
              model,
              &config,
              &component) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_get_effective_metrics(
              component, &before) == BOXRIG_FONT_CATALOG_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.width = before.cell_width * terminal_config.columns;
    allocation.height = before.cell_height * terminal_config.rows;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_win32_terminal_component_allocate(
              component, &allocation, &layout) == BOXRIG_UI_OK);
    allocation.window_state = BOXRIG_UI_WINDOW_MAXIMIZED;
    CHECK(boxrig_win32_terminal_component_allocate(
              component, &allocation, &layout) == BOXRIG_UI_OK);

    CHECK(boxrig_win32_terminal_component_visual_zoom(
              component,
              BOXRIG_FONT_ZOOM_DECREASE,
              boxrig_font_visual_step_policy_default(),
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(boxrig_win32_terminal_component_get_effective_metrics(
              component, &after) == BOXRIG_FONT_CATALOG_OK);
    CHECK(after.layout_revision > before.layout_revision);
    CHECK(boxrig_win32_terminal_component_get_size_command_state(
              component, &size_state) == BOXRIG_UI_OK);
    CHECK(size_state == BOXRIG_UI_SIZE_COMMAND_IDLE);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(frame.frame_pending && frame.additional_dispatch);
    CHECK(boxrig_win32_terminal_component_take_layout_action(
        component, &action));
    CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
    CHECK(action.value.grid_resize.rows == allocation.height / after.cell_height);
    CHECK(action.value.grid_resize.columns == allocation.width / after.cell_width);
    CHECK(!boxrig_win32_terminal_component_take_layout_action(
        component, &action));
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(frame.frame_pending && !frame.additional_dispatch);
    CHECK(boxrig_win32_terminal_component_complete_grid_resize(
              component, action.revision, true) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_resize(
              terminal,
              action.value.grid_resize.rows,
              action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_terminal_component_complete_grid_resize(
              component, action.revision, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_complete_grid_resize(
              component, action.revision, true) == BOXRIG_UI_STALE);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    boxrig_terminal_size(terminal, &rows, &columns);
    CHECK(rows == action.value.grid_resize.rows);
    CHECK(columns == action.value.grid_resize.columns);
    CHECK(rows != terminal_config.rows || columns != terminal_config.columns);

    boxrig_win32_terminal_component_destroy(component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(parent));
    return true;
}

static bool clipboard_text_equals(HWND window, const WCHAR *expected)
{
    HGLOBAL handle;
    const WCHAR *text;
    SIZE_T allocation_size;
    size_t expected_length = wcslen(expected);
    bool matches = false;

    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) ||
        !OpenClipboard(window)) {
        return false;
    }
    handle = (HGLOBAL)GetClipboardData(CF_UNICODETEXT);
    if (handle != NULL) {
        allocation_size = GlobalSize(handle);
        text = GlobalLock(handle);
        if (text != NULL) {
            matches = allocation_size >=
                          (expected_length + 1) * sizeof(*text) &&
                      memcmp(
                          text,
                          expected,
                          (expected_length + 1) * sizeof(*text)) == 0;
            (void)GlobalUnlock(handle);
        }
    }
    (void)CloseClipboard();
    return matches;
}

static bool copy_survives_component_destruction(void)
{
    static const char text[] =
        "copy\r\ntext \xf0\x9f\x98\x80";
    static const uint8_t pasted_text[] =
        "copy\ntext \xf0\x9f\x98\x80";
    static const WCHAR expected[] =
        L"copy\r\ntext \xd83d\xde00";
    HWND parent;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_win32_terminal_component_config config =
        boxrig_win32_terminal_component_config_default();
    boxrig_win32_terminal_component *component = NULL;
    boxrig_ui_view *view;
    boxrig_ui_hit_test_result anchor;
    boxrig_ui_hit_test_result focus;
    boxrig_ui_view_action action;
    boxrig_ui_view_action_capabilities capabilities;
    const uint8_t *output;
    size_t output_length;

    parent = CreateWindowExW(
        0,
        L"STATIC",
        L"clipboard component host",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        320,
        200,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(parent != NULL);
    terminal_config.rows = 2;
    terminal_config.columns = 16;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(feed_text(terminal, text));
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    CHECK(boxrig_win32_terminal_component_create_child(
              GetModuleHandleW(NULL),
              parent,
              model,
              &config,
              &component) == BOXRIG_UI_OK);
    view = boxrig_win32_terminal_component_view(component);
    CHECK(view != NULL);
    memset(&anchor, 0, sizeof(anchor));
    anchor.inside_grid = true;
    anchor.text_position_valid = true;
    CHECK(boxrig_terminal_get_text_position(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              0,
              0,
              &anchor.leading_position) == BOXRIG_STATUS_OK);
    anchor.trailing_position = anchor.leading_position;
    anchor.position = anchor.leading_position;
    CHECK(boxrig_ui_view_begin_selection(
              view,
              BOXRIG_UI_SELECTION_LINE,
              &anchor) == BOXRIG_UI_OK);
    memset(&focus, 0, sizeof(focus));
    focus.inside_grid = true;
    focus.text_position_valid = true;
    CHECK(boxrig_terminal_get_text_position(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              1,
              0,
              &focus.leading_position) == BOXRIG_STATUS_OK);
    focus.trailing_position = focus.leading_position;
    focus.position = focus.leading_position;
    CHECK(boxrig_ui_view_update_selection(view, &focus) == BOXRIG_UI_OK);

    capabilities =
        boxrig_win32_terminal_component_view_action_capabilities(component);
    CHECK(capabilities.copy_targets ==
          BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD);
    CHECK(capabilities.paste_targets ==
          BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD);
    action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    CHECK(boxrig_win32_terminal_component_apply_view_action(
              component,
              &action) == BOXRIG_UI_OK);
    CHECK(clipboard_text_equals(parent, expected));
    action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
    CHECK(boxrig_win32_terminal_component_apply_view_action(
              component,
              &action) == BOXRIG_UI_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(pasted_text) - 1);
    CHECK(memcmp(output, pasted_text, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    boxrig_win32_terminal_component_destroy(component);
    component = NULL;
    CHECK(clipboard_text_equals(parent, expected));
    CHECK(OpenClipboard(parent));
    CHECK(EmptyClipboard());
    CHECK(CloseClipboard());
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(parent));
    return true;
}

static bool awaiting_reconversion_allows_focus_lifecycle(void)
{
    static const char context[] = "abc";
    static const char replacement[] = "X";
    HWND parent;
    HWND target;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_win32_terminal_component_config config =
        boxrig_win32_terminal_component_config_default();
    boxrig_win32_terminal_component *component = NULL;
    boxrig_ui_editable_document *document;
    boxrig_ui_editable_document_snapshot_input snapshot;
    boxrig_ui_editable_replacement_request request;
    boxrig_ui_editable_replacement_completion completion;
    const uint8_t *output;
    LRESULT message_result = 0;

    parent = CreateWindowExW(
        0,
        L"STATIC",
        L"reconversion focus lifecycle host",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        320,
        200,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(parent != NULL);
    terminal_config.rows = 2;
    terminal_config.columns = 8;
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_create_child(
              GetModuleHandleW(NULL),
              parent,
              model,
              &config,
              &component) == BOXRIG_UI_OK);
    target = boxrig_win32_terminal_component_input_window(component);
    CHECK(target != NULL);
    document = boxrig_ui_view_editable_document(
        boxrig_win32_terminal_component_view(component));
    CHECK(document != NULL);
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.document_identity = 41;
    snapshot.document_revision = 7;
    snapshot.context = context;
    snapshot.context_length = sizeof(context) - 1;
    snapshot.selection.offset = 1;
    snapshot.selection.length = 1;
    CHECK(boxrig_ui_editable_document_publish(document, &snapshot) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_set_editable_document_provider(
              component,
              true) == BOXRIG_UI_OK);

    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              target,
              WM_SETFOCUS,
              0,
              0,
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(boxrig_win32_terminal_component_focused(component));
    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              target,
              WM_IME_REQUEST,
              IMR_RECONVERTSTRING,
              0,
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(message_result > (LRESULT)sizeof(RECONVERTSTRING));
    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              target,
              WM_IME_STARTCOMPOSITION,
              0,
              0,
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              target,
              WM_IME_CHAR,
              L'X',
              0,
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              target,
              WM_IME_ENDCOMPOSITION,
              0,
              0,
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(boxrig_ui_editable_document_reconversion_state(document) ==
          BOXRIG_UI_EDITABLE_RECONVERSION_AWAITING_COMPLETION);
    CHECK(boxrig_ui_editable_document_take_replacement(document, &request));
    CHECK(request.replacement_length == sizeof(replacement) - 1);
    CHECK(memcmp(
              request.replacement,
              replacement,
              sizeof(replacement) - 1) == 0);

    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              target,
              WM_KILLFOCUS,
              0,
              0,
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(!boxrig_win32_terminal_component_focused(component));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    memset(&completion, 0, sizeof(completion));
    completion.request_id = request.request_id;
    completion.outcome = BOXRIG_UI_EDITABLE_REPLACEMENT_REJECTED;
    CHECK(boxrig_ui_editable_document_complete_replacement(
              document,
              &completion) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              target,
              WM_NULL,
              0,
              0,
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_UNRELATED);
    CHECK(boxrig_win32_terminal_component_set_editable_document_provider(
              component,
              false) == BOXRIG_UI_OK);

    boxrig_win32_terminal_component_destroy(component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(parent));
    return true;
}

static bool read_window_hash(
    HWND window,
    unsigned int width,
    unsigned int height,
    uint64_t *hash_out)
{
    HDC context;
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned int x;
    unsigned int y;

    if (window == NULL || width == 0 || height == 0 || hash_out == NULL) {
        return false;
    }
    context = GetDC(window);
    if (context == NULL) {
        return false;
    }
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            COLORREF pixel = GetPixel(context, (int)x, (int)y);

            if (pixel == CLR_INVALID) {
                (void)ReleaseDC(window, context);
                return false;
            }
            hash ^= (uint64_t)pixel;
            hash *= UINT64_C(1099511628211);
        }
    }
    if (ReleaseDC(window, context) != 1) {
        return false;
    }
    *hash_out = hash;
    return true;
}

static void count_frame_request(void *context)
{
    size_t *count = context;

    (*count)++;
}

typedef struct presentation_probe {
    size_t count;
    bool valid;
    bool pixels_valid;
    unsigned int scrollbar_width;
    SIZE size;
    RECT dirty;
    uint64_t terminal_hash;
    uint64_t scrollbar_hash;
    COLORREF track_pixel;
    COLORREF thumb_pixel;
} presentation_probe;

typedef struct ime_resynchronization_probe {
    boxrig_win32_ime_placement_status status;
    size_t entry_count;
    size_t composition_form_count;
    size_t candidate_form_count;
    bool candidate_visible;
    bool input_context_available;
} ime_resynchronization_probe;

static ime_resynchronization_probe ime_resynchronization;

boxrig_win32_ime_placement_status
__wrap_boxrig_win32_input_adapter_resynchronize_candidate(
    boxrig_win32_input_adapter *adapter)
{
    if (adapter == NULL) {
        return BOXRIG_WIN32_IME_PLACEMENT_ERROR;
    }
    ime_resynchronization.entry_count++;
    if (!ime_resynchronization.candidate_visible ||
        !ime_resynchronization.input_context_available) {
        return BOXRIG_WIN32_IME_PLACEMENT_OK;
    }
    ime_resynchronization.composition_form_count++;
    ime_resynchronization.candidate_form_count++;
    return ime_resynchronization.status;
}

static bool component_resynchronizes_ime_without_side_effects(
    boxrig_win32_terminal_component *component,
    boxrig_terminal *terminal,
    const size_t *present_count)
{
    const uint8_t *output_before;
    const uint8_t *output_after;
    boxrig_cursor_view cursor_before;
    boxrig_cursor_view cursor_after;
    uint64_t terminal_revision;
    size_t scrollback_rows;
    size_t viewport_offset;
    size_t output_length;
    size_t committed_present_count =
        present_count == NULL ? 0 : *present_count;
    bool frame_requested;

    memset(&ime_resynchronization, 0, sizeof(ime_resynchronization));
    ime_resynchronization.status = BOXRIG_WIN32_IME_PLACEMENT_OK;
    terminal_revision = boxrig_terminal_active_buffer_revision(terminal);
    scrollback_rows = boxrig_terminal_scrollback_rows(terminal);
    viewport_offset = boxrig_terminal_viewport_offset(terminal);
    boxrig_terminal_get_cursor(terminal, &cursor_before);
    output_length = boxrig_terminal_output_peek(terminal, &output_before);
    frame_requested = boxrig_ui_view_frame_requested(
        boxrig_win32_terminal_component_view(component));

    CHECK(boxrig_win32_terminal_component_resynchronize_ime_placement(NULL) ==
          BOXRIG_WIN32_IME_PLACEMENT_ERROR);
    CHECK(boxrig_win32_terminal_component_resynchronize_ime_placement(
              component) == BOXRIG_WIN32_IME_PLACEMENT_OK);
    CHECK(ime_resynchronization.entry_count == 1 &&
          ime_resynchronization.composition_form_count == 0 &&
          ime_resynchronization.candidate_form_count == 0);

    ime_resynchronization.candidate_visible = true;
    CHECK(boxrig_win32_terminal_component_resynchronize_ime_placement(
              component) == BOXRIG_WIN32_IME_PLACEMENT_OK);
    CHECK(ime_resynchronization.entry_count == 2 &&
          ime_resynchronization.composition_form_count == 0 &&
          ime_resynchronization.candidate_form_count == 0);

    ime_resynchronization.input_context_available = true;
    CHECK(boxrig_win32_terminal_component_resynchronize_ime_placement(
              component) == BOXRIG_WIN32_IME_PLACEMENT_OK);
    CHECK(ime_resynchronization.entry_count == 3 &&
          ime_resynchronization.composition_form_count == 1 &&
          ime_resynchronization.candidate_form_count == 1);
    CHECK(boxrig_win32_terminal_component_ime_placement_status(component) ==
          BOXRIG_WIN32_IME_PLACEMENT_OK);

    ime_resynchronization.status =
        BOXRIG_WIN32_IME_PLACEMENT_COMPOSITION_REJECTED;
    CHECK(boxrig_win32_terminal_component_resynchronize_ime_placement(
              component) ==
          BOXRIG_WIN32_IME_PLACEMENT_COMPOSITION_REJECTED);
    CHECK(ime_resynchronization.entry_count == 4 &&
          ime_resynchronization.composition_form_count == 2 &&
          ime_resynchronization.candidate_form_count == 2);
    CHECK(boxrig_win32_terminal_component_ime_placement_status(component) ==
          BOXRIG_WIN32_IME_PLACEMENT_COMPOSITION_REJECTED);

    ime_resynchronization.status =
        BOXRIG_WIN32_IME_PLACEMENT_CANDIDATE_REJECTED;
    CHECK(boxrig_win32_terminal_component_resynchronize_ime_placement(
              component) == BOXRIG_WIN32_IME_PLACEMENT_CANDIDATE_REJECTED);
    CHECK(ime_resynchronization.entry_count == 5 &&
          ime_resynchronization.composition_form_count == 3 &&
          ime_resynchronization.candidate_form_count == 3);
    CHECK(boxrig_win32_terminal_component_ime_placement_status(component) ==
          BOXRIG_WIN32_IME_PLACEMENT_CANDIDATE_REJECTED);

    ime_resynchronization.status =
        BOXRIG_WIN32_IME_PLACEMENT_BOTH_REJECTED;
    CHECK(boxrig_win32_terminal_component_resynchronize_ime_placement(
              component) == BOXRIG_WIN32_IME_PLACEMENT_BOTH_REJECTED);
    CHECK(ime_resynchronization.entry_count == 6 &&
          ime_resynchronization.composition_form_count == 4 &&
          ime_resynchronization.candidate_form_count == 4);
    CHECK(boxrig_win32_terminal_component_ime_placement_status(component) ==
          BOXRIG_WIN32_IME_PLACEMENT_BOTH_REJECTED);

    ime_resynchronization.status = BOXRIG_WIN32_IME_PLACEMENT_ERROR;
    CHECK(boxrig_win32_terminal_component_resynchronize_ime_placement(
              component) == BOXRIG_WIN32_IME_PLACEMENT_ERROR);
    CHECK(ime_resynchronization.entry_count == 7 &&
          ime_resynchronization.composition_form_count == 5 &&
          ime_resynchronization.candidate_form_count == 5);
    CHECK(boxrig_win32_terminal_component_ime_placement_status(component) ==
          BOXRIG_WIN32_IME_PLACEMENT_ERROR);

    ime_resynchronization.candidate_visible = false;
    ime_resynchronization.status = BOXRIG_WIN32_IME_PLACEMENT_OK;
    CHECK(boxrig_win32_terminal_component_resynchronize_ime_placement(
              component) == BOXRIG_WIN32_IME_PLACEMENT_OK);
    CHECK(ime_resynchronization.entry_count == 8 &&
          ime_resynchronization.composition_form_count == 5 &&
          ime_resynchronization.candidate_form_count == 5);
    CHECK(boxrig_win32_terminal_component_ime_placement_status(component) ==
          BOXRIG_WIN32_IME_PLACEMENT_OK);

    CHECK(boxrig_terminal_active_buffer_revision(terminal) ==
          terminal_revision);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == scrollback_rows);
    CHECK(boxrig_terminal_viewport_offset(terminal) == viewport_offset);
    boxrig_terminal_get_cursor(terminal, &cursor_after);
    CHECK(cursor_after.row == cursor_before.row &&
          cursor_after.column == cursor_before.column &&
          cursor_after.visible == cursor_before.visible &&
          cursor_after.shape == cursor_before.shape &&
          cursor_after.blinking == cursor_before.blinking);
    CHECK(boxrig_terminal_output_peek(terminal, &output_after) ==
          output_length);
    CHECK(output_after == output_before);
    CHECK(boxrig_ui_view_frame_requested(
              boxrig_win32_terminal_component_view(component)) ==
          frame_requested);
    CHECK(present_count == NULL || *present_count == committed_present_count);
    return true;
}

static bool read_context_region_hash(
    HDC context,
    int origin_x,
    int origin_y,
    unsigned int width,
    unsigned int height,
    uint64_t *hash_out)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned int x;
    unsigned int y;

    if (context == NULL || origin_x < 0 || origin_y < 0 || width == 0 ||
        height == 0 || hash_out == NULL) {
        return false;
    }
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            COLORREF pixel = GetPixel(
                context,
                origin_x + (int)x,
                origin_y + (int)y);

            if (pixel == CLR_INVALID) {
                return false;
            }
            hash ^= (uint64_t)pixel;
            hash *= UINT64_C(1099511628211);
        }
    }
    *hash_out = hash;
    return true;
}

static BOOL CALLBACK count_descendant_window(HWND window, LPARAM parameter)
{
    size_t *count = (size_t *)parameter;

    (void)window;
    (*count)++;
    return TRUE;
}

static size_t descendant_window_count(HWND window)
{
    size_t count = 0;

    (void)EnumChildWindows(window, count_descendant_window, (LPARAM)&count);
    return count;
}

static boxrig_ui_status present_external_surface(
    void *context,
    HDC surface,
    SIZE size,
    const RECT *dirty)
{
    presentation_probe *probe = context;

    probe->count++;
    probe->valid = surface != NULL && size.cx > 0 && size.cy > 0 &&
                   dirty != NULL && dirty->left >= 0 &&
                   dirty->top >= 0 && dirty->right <= size.cx &&
                   dirty->bottom <= size.cy &&
                   dirty->right > dirty->left &&
                   dirty->bottom > dirty->top;
    probe->size = size;
    if (dirty != NULL) {
        probe->dirty = *dirty;
    }
    if (probe->valid && probe->scrollbar_width != 0) {
        unsigned int surface_width = (unsigned int)size.cx;
        unsigned int surface_height = (unsigned int)size.cy;
        unsigned int scrollbar_width = probe->scrollbar_width;

        probe->pixels_valid = scrollbar_width < surface_width &&
                              read_context_region_hash(
                                  surface,
                                  0,
                                  0,
                                  surface_width - scrollbar_width,
                                  surface_height,
                                  &probe->terminal_hash) &&
                              read_context_region_hash(
                                  surface,
                                  (int)(surface_width - scrollbar_width),
                                  0,
                                  scrollbar_width,
                                  surface_height,
                                  &probe->scrollbar_hash);
        if (probe->pixels_valid) {
            probe->track_pixel = GetPixel(
                surface,
                (int)surface_width - 1,
                0);
            probe->thumb_pixel = GetPixel(
                surface,
                (int)surface_width - 1,
                (int)surface_height - 1);
            probe->pixels_valid = probe->track_pixel != CLR_INVALID &&
                                  probe->thumb_pixel != CLR_INVALID;
        }
        probe->valid = probe->pixels_valid;
    }
    return probe->valid ? BOXRIG_UI_OK : BOXRIG_UI_INVALID_ARGUMENT;
}

static bool metrics_match(
    const boxrig_font_effective_metrics *left,
    const boxrig_font_effective_metrics *right)
{
    return left->requested_size_dip == right->requested_size_dip &&
           left->primary_resource_id == right->primary_resource_id &&
           left->primary_face_key == right->primary_face_key &&
           left->cell_width == right->cell_width &&
           left->cell_height == right->cell_height &&
           left->baseline == right->baseline &&
           left->layout_revision == right->layout_revision;
}

static bool register_host_class(HINSTANCE instance, const WCHAR *class_name)
{
    WNDCLASSEXW window_class;

    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    return RegisterClassExW(&window_class) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static bool component_is_embedded_and_independent(const char *font_path)
{
    static const WCHAR host_class[] = L"boxrig_component_test_host";
    HINSTANCE instance = GetModuleHandleW(NULL);
    HWND parent;
    HWND sibling;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_terminal *other_terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_model *other_model = NULL;
    boxrig_win32_terminal_component_config config;
    boxrig_win32_terminal_component *component = NULL;
    boxrig_win32_terminal_component *conflict = NULL;
    boxrig_win32_terminal_component *presentation = NULL;
    boxrig_win32_terminal_component *other = NULL;
    boxrig_font_effective_metrics initial_metrics;
    boxrig_font_effective_metrics presentation_before;
    boxrig_font_effective_metrics presentation_after;
    boxrig_font_effective_metrics before_invalid;
    boxrig_font_effective_metrics after_invalid;
    boxrig_font_effective_metrics changed_metrics;
    boxrig_font_specification specification;
    boxrig_font_specification invalid_specification;
    boxrig_font_rendering_policy policy;
    boxrig_font_resource_id font_resource = BOXRIG_FONT_RESOURCE_NONE;
    boxrig_font_family resource_family;
    boxrig_font_visual_step_policy zoom_policy;
    boxrig_ui_component_allocation allocation;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame;
    boxrig_ui_hit_test_result hit;
    boxrig_ui_component_diagnostic diagnostic;
    boxrig_ui_transparency_config transparency;
    boxrig_ui_transparency_state transparency_state;
    boxrig_scrollbar_model scrollbar;
    MSG message;
    RECT rectangle;
    LRESULT message_result = 0;
    size_t frame_requests = 0;
    float line_height = 0.0f;
    bool changed = false;

    CHECK(instance != NULL && register_host_class(instance, host_class));
    parent = CreateWindowExW(
        0,
        host_class,
        L"host",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        640,
        360,
        NULL,
        NULL,
        instance,
        NULL);
    sibling = CreateWindowExW(
        0,
        L"BUTTON",
        L"sibling",
        WS_CHILD | WS_VISIBLE,
        500,
        10,
        100,
        30,
        parent,
        NULL,
        instance,
        NULL);
    CHECK(parent != NULL && sibling != NULL);
    terminal_config.rows = 4;
    terminal_config.columns = 12;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_create(&terminal_config, &other_terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_model_create(other_terminal, &other_model) == BOXRIG_UI_OK);

    config = boxrig_win32_terminal_component_config_default();
    CHECK(boxrig_win32_terminal_component_create_child(
              instance,
              parent,
              model,
              &config,
              &component) == BOXRIG_UI_OK);
    CHECK(GetParent(boxrig_win32_terminal_component_owned_window(component)) ==
          parent);
    CHECK(boxrig_win32_terminal_component_native_hosting(component) ==
          BOXRIG_UI_COMPONENT_OWNED_CHILD);
    CHECK(boxrig_win32_terminal_component_presentation_window(component) ==
          boxrig_win32_terminal_component_owned_window(component));
    CHECK(boxrig_win32_terminal_component_input_window(component) ==
          boxrig_win32_terminal_component_owned_window(component));
    CHECK(boxrig_win32_terminal_component_input_window_owned(component));
    CHECK(boxrig_win32_terminal_component_scrollbar_window(component) != NULL);
    CHECK(boxrig_win32_terminal_component_model(component) == model);
    CHECK(boxrig_ui_view_model(
              boxrig_win32_terminal_component_view(component)) == model);
    CHECK(boxrig_win32_terminal_component_create_child(
              instance,
              parent,
              model,
              &config,
              &conflict) == BOXRIG_UI_UNAVAILABLE);
    CHECK(conflict == NULL);
    config.attachment = BOXRIG_UI_COMPONENT_PRESENTATION_ONLY;
    CHECK(boxrig_win32_terminal_component_create_child(
              instance,
              parent,
              model,
              &config,
              &presentation) == BOXRIG_UI_OK);
    config.attachment = BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY;
    CHECK(boxrig_win32_terminal_component_create_child(
              instance,
              parent,
              other_model,
              &config,
              &other) == BOXRIG_UI_OK);

    CHECK(boxrig_win32_terminal_component_get_effective_metrics(
              component,
              &initial_metrics) == BOXRIG_FONT_CATALOG_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.x = 20;
    allocation.y = 30;
    allocation.width = initial_metrics.cell_width * 10 + 20;
    allocation.height = initial_metrics.cell_height * 4;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_win32_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(layout.rows == 4 && layout.columns >= 8);
    CHECK(GetClientRect(
              boxrig_win32_terminal_component_owned_window(component),
              &rectangle));
    CHECK((unsigned int)(rectangle.right - rectangle.left) ==
          allocation.width);
    allocation.x = 280;
    CHECK(boxrig_win32_terminal_component_allocate(
              presentation,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    allocation.y = 180;
    CHECK(boxrig_win32_terminal_component_allocate(
              other,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_set_visible(component, true) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_set_visible(presentation, true) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_set_visible(other, true) ==
          BOXRIG_UI_OK);
    ShowWindow(parent, SW_SHOWNA);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_set_frame_request_callback(
              component,
              count_frame_request,
              &frame_requests) == BOXRIG_UI_OK);
    CHECK(feed_text(terminal, "component"));
    CHECK(feed_text(other_terminal, "other"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_model_synchronize(other_model) == BOXRIG_UI_OK);
    CHECK(frame_requests == 1);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    (void)UpdateWindow(boxrig_win32_terminal_component_owned_window(component));
    {
        size_t committed_frame_requests = frame_requests;

        CHECK(component_resynchronizes_ime_without_side_effects(
            component,
            terminal,
            NULL));
        CHECK(frame_requests == committed_frame_requests);
    }
    CHECK(boxrig_win32_terminal_component_hit_test(
              component,
              BOXRIG_UI_FIXED_ONE,
              BOXRIG_UI_FIXED_ONE,
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.inside_grid && hit.logical_column == 0);
    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              sibling,
              WM_KEYDOWN,
              'X',
              0,
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_UNRELATED);

    CHECK(boxrig_win32_terminal_component_get_effective_metrics(
              presentation,
              &presentation_before) == BOXRIG_FONT_CATALOG_OK);
    policy = boxrig_font_rendering_policy_default();
    policy.dpi_x = 144.0f;
    policy.dpi_y = 144.0f;
    CHECK(boxrig_win32_terminal_component_set_font_rendering_policy(
              presentation,
              &policy) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_win32_terminal_component_get_effective_metrics(
              presentation,
              &presentation_after) == BOXRIG_FONT_CATALOG_OK);
    CHECK(metrics_match(&presentation_before, &presentation_after));
    boxrig_win32_terminal_component_destroy(presentation);
    presentation = NULL;

    CHECK(boxrig_win32_terminal_component_register_font_file(
              component,
              font_path,
              strlen(font_path),
              0,
              &font_resource) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_win32_terminal_component_get_font_specification(
              component,
              &specification) == BOXRIG_UI_OK);
    memset(&resource_family, 0, sizeof(resource_family));
    resource_family.resource_id = font_resource;
    specification.families = &resource_family;
    specification.family_count = 1;
    CHECK(boxrig_win32_terminal_component_set_font_specification(
              component,
              &specification) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_primary_font_resource(component) ==
          font_resource);
    CHECK(boxrig_win32_terminal_component_get_effective_metrics(
              component,
              &before_invalid) == BOXRIG_FONT_CATALOG_OK);
    invalid_specification = specification;
    invalid_specification.size_dip = 0.0f;
    CHECK(boxrig_win32_terminal_component_set_font_specification(
              component,
              &invalid_specification) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_win32_terminal_component_get_effective_metrics(
              component,
              &after_invalid) == BOXRIG_FONT_CATALOG_OK);
    CHECK(metrics_match(&before_invalid, &after_invalid));
    CHECK(boxrig_win32_terminal_component_get_font_rendering_policy(
              component,
              &policy) == BOXRIG_UI_OK);
    policy.dpi_x = 144.0f;
    policy.dpi_y = 144.0f;
    CHECK(boxrig_win32_terminal_component_set_font_rendering_policy(
              component,
              &policy) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_set_line_height_adjustment(
              component,
              2.0f) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_get_line_height_adjustment(
              component,
              &line_height) == BOXRIG_UI_OK &&
          line_height == 2.0f);
    CHECK(boxrig_win32_terminal_component_get_effective_metrics(
              component,
              &changed_metrics) == BOXRIG_FONT_CATALOG_OK);
    zoom_policy = boxrig_font_visual_step_policy_default();
    zoom_policy.minimum_size_dip = changed_metrics.requested_size_dip - 2.0f;
    zoom_policy.maximum_size_dip = changed_metrics.requested_size_dip + 2.0f;
    CHECK(boxrig_win32_terminal_component_visual_zoom(
              component,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &changed) == BOXRIG_UI_OK &&
          changed);
    CHECK(boxrig_win32_terminal_component_visual_zoom_reset(
              component,
              &changed) == BOXRIG_UI_OK &&
          changed);
    CHECK(boxrig_win32_terminal_component_set_text_direction(
              component,
              BOXRIG_TEXT_DIRECTION_FORCE_RTL) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_text_direction(component) ==
          BOXRIG_TEXT_DIRECTION_FORCE_RTL);

    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              boxrig_win32_terminal_component_owned_window(component),
              WM_SETFOCUS,
              0,
              0,
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(boxrig_win32_terminal_component_focused(component));
    CHECK(boxrig_win32_terminal_component_set_key_router(
              component,
              NULL,
              NULL) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_get_scrollbar_model(
              component,
              &scrollbar) == BOXRIG_UI_OK);
    CHECK(scrollbar.viewport_rows == terminal_config.rows);
    CHECK(boxrig_win32_terminal_component_apply_scrollbar_action(
              component,
              BOXRIG_SCROLLBAR_BOTTOM,
              0,
              &changed) == BOXRIG_UI_OK &&
          !changed);
    CHECK(boxrig_win32_terminal_component_get_transparency(
              component,
              &transparency_state) == BOXRIG_UI_OK);
    transparency = boxrig_ui_transparency_config_default();
    transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    transparency.requested_scopes =
        BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
    transparency.background_opacity = 128;
    CHECK(boxrig_win32_terminal_component_set_transparency(
              component,
              &transparency) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_win32_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.running && diagnostic.visible && diagnostic.focused);
    CHECK((boxrig_win32_terminal_component_capabilities(component) &
           BOXRIG_UI_COMPONENT_CAPABILITY_OWNED_VISIBLE_CHILD) != 0);

    boxrig_win32_terminal_component_destroy(component);
    component = NULL;
    CHECK(IsWindow(parent) && IsWindow(sibling));
    CHECK(IsWindow(boxrig_win32_terminal_component_owned_window(other)));
    CHECK(feed_text(other_terminal, " after destroy"));
    CHECK(!PeekMessageW(&message, NULL, WM_QUIT, WM_QUIT, PM_REMOVE));

    CHECK(boxrig_win32_terminal_component_create_child(
              instance,
              parent,
              model,
              &config,
              &component) == BOXRIG_UI_OK);
    CHECK(DestroyWindow(boxrig_win32_terminal_component_owned_window(component)));
    CHECK(boxrig_win32_terminal_component_owned_window(component) == NULL);
    CHECK(IsWindow(parent) && IsWindow(sibling));
    boxrig_win32_terminal_component_destroy(component);
    boxrig_win32_terminal_component_destroy(presentation);
    boxrig_win32_terminal_component_destroy(other);
    boxrig_ui_model_destroy(model);
    boxrig_ui_model_destroy(other_model);
    boxrig_terminal_destroy(terminal);
    boxrig_terminal_destroy(other_terminal);
    CHECK(DestroyWindow(sibling));
    CHECK(DestroyWindow(parent));
    return true;
}

static bool borrowed_target_preserves_caller_windows(void)
{
    static const WCHAR host_class[] = L"boxrig_component_borrowed_test_host";
    HINSTANCE instance = GetModuleHandleW(NULL);
    HWND parent;
    HWND presentation_window;
    HWND input_window;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_win32_terminal_component_config config =
        boxrig_win32_terminal_component_config_default();
    boxrig_win32_terminal_borrowed_target target;
    boxrig_win32_terminal_component *component = NULL;
    boxrig_win32_terminal_component *invalid = NULL;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_component_allocation allocation;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame;
    boxrig_ui_size_command request;
    boxrig_font_visual_step_policy zoom_policy =
        boxrig_font_visual_step_policy_default();
    presentation_probe present_probe = {0};
    SIZE committed_surface_size;
    RECT presentation_before;
    RECT presentation_after;
    RECT input_before;
    RECT input_after;
    LONG_PTR presentation_style;
    LONG_PTR presentation_extended_style;
    LONG_PTR presentation_procedure;
    LONG_PTR presentation_user_data;
    LONG_PTR input_style;
    LONG_PTR input_extended_style;
    LONG_PTR input_procedure;
    LONG_PTR input_user_data;
    LRESULT message_result = 0;
    size_t descendant_count;
    size_t committed_present_count;
    uint32_t capabilities;
    bool changed = false;
    bool applied = false;

    CHECK(instance != NULL && register_host_class(instance, host_class));
    parent = CreateWindowExW(
        0,
        host_class,
        L"borrowed host",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        640,
        360,
        NULL,
        NULL,
        instance,
        NULL);
    presentation_window = CreateWindowExW(
        0,
        host_class,
        L"presentation target",
        WS_CHILD | WS_VISIBLE,
        17,
        23,
        320,
        160,
        parent,
        NULL,
        instance,
        NULL);
    input_window = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        host_class,
        L"input target",
        WS_CHILD | WS_VISIBLE,
        381,
        29,
        180,
        96,
        parent,
        NULL,
        instance,
        NULL);
    CHECK(parent != NULL && presentation_window != NULL &&
          input_window != NULL);
    ShowWindow(parent, SW_SHOWNA);
    (void)SetWindowLongPtrW(
        presentation_window,
        GWLP_USERDATA,
        (LONG_PTR)UINT32_C(0x12345678));
    (void)SetWindowLongPtrW(
        input_window,
        GWLP_USERDATA,
        (LONG_PTR)UINT32_C(0x23456789));
    CHECK(GetWindowRect(presentation_window, &presentation_before));
    CHECK(GetWindowRect(input_window, &input_before));
    presentation_style = GetWindowLongPtrW(presentation_window, GWL_STYLE);
    presentation_extended_style =
        GetWindowLongPtrW(presentation_window, GWL_EXSTYLE);
    presentation_procedure =
        GetWindowLongPtrW(presentation_window, GWLP_WNDPROC);
    presentation_user_data =
        GetWindowLongPtrW(presentation_window, GWLP_USERDATA);
    input_style = GetWindowLongPtrW(input_window, GWL_STYLE);
    input_extended_style = GetWindowLongPtrW(input_window, GWL_EXSTYLE);
    input_procedure = GetWindowLongPtrW(input_window, GWLP_WNDPROC);
    input_user_data = GetWindowLongPtrW(input_window, GWLP_USERDATA);
    descendant_count = descendant_window_count(parent);

    terminal_config.rows = 3;
    terminal_config.columns = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    target.presentation_window = presentation_window;
    target.input_window = input_window;
    target.presenter = present_external_surface;
    target.presenter_context = &present_probe;
    CHECK(boxrig_win32_terminal_component_create_on_target(
              instance,
              &target,
              model,
              &config,
              &invalid) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(invalid == NULL);

    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    CHECK(boxrig_win32_terminal_component_create_on_target(
              instance,
              &target,
              model,
              &config,
              &component) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_native_hosting(component) ==
          BOXRIG_UI_COMPONENT_BORROWED_TARGET);
    CHECK(boxrig_win32_terminal_component_owned_window(component) == NULL);
    CHECK(boxrig_win32_terminal_component_presentation_window(component) ==
          presentation_window);
    CHECK(boxrig_win32_terminal_component_input_window(component) ==
          input_window);
    CHECK(!boxrig_win32_terminal_component_input_window_owned(component));
    CHECK(boxrig_win32_terminal_component_scrollbar_window(component) == NULL);
    capabilities = boxrig_win32_terminal_component_capabilities(component);
    CHECK((capabilities & BOXRIG_UI_COMPONENT_CAPABILITY_BORROWED_TARGET) != 0);
    CHECK((capabilities &
           BOXRIG_UI_COMPONENT_CAPABILITY_EXTERNAL_PRESENTATION) != 0);
    CHECK((capabilities &
           BOXRIG_UI_COMPONENT_CAPABILITY_OWNED_VISIBLE_CHILD) == 0);
    CHECK(descendant_window_count(parent) == descendant_count);
    CHECK(boxrig_win32_terminal_component_set_target_extent(
              component,
              320,
              160) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_get_effective_metrics(
              component,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.width = metrics.cell_width * terminal_config.columns;
    allocation.height = metrics.cell_height * terminal_config.rows;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_win32_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_set_visible(component, false) ==
          BOXRIG_UI_OK);
    CHECK(IsWindowVisible(presentation_window) &&
          IsWindowVisible(input_window));
    CHECK(boxrig_win32_terminal_component_set_visible(component, true) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_request_focus(component) ==
          BOXRIG_UI_OK);
    CHECK(GetFocus() == input_window);
    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              presentation_window,
              WM_SETFOCUS,
              0,
              0,
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_UNRELATED);
    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              input_window,
              WM_SETFOCUS,
              0,
              0,
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(boxrig_win32_terminal_component_focused(component));
    CHECK(feed_text(terminal, "borrowed"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(present_probe.count > 0 && present_probe.valid);
    CHECK(component_resynchronizes_ime_without_side_effects(
        component,
        terminal,
        &present_probe.count));
    committed_present_count = present_probe.count;
    committed_surface_size = present_probe.size;
    zoom_policy.minimum_size_dip = metrics.requested_size_dip;
    zoom_policy.maximum_size_dip = metrics.requested_size_dip + 4.0f;
    CHECK(boxrig_win32_terminal_component_visual_zoom(
              component,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(present_probe.count >= committed_present_count &&
          present_probe.valid &&
          present_probe.size.cx == committed_surface_size.cx &&
          present_probe.size.cy == committed_surface_size.cy);
    committed_present_count = present_probe.count;
    CHECK(take_content_size_action(
        component,
        &request));
    CHECK(boxrig_win32_terminal_component_complete_size_command(
              component,
              request.revision,
              BOXRIG_UI_SIZE_ACCEPTED,
              request.width,
              request.height,
              &applied,
              &layout) == BOXRIG_UI_OK);
    CHECK(applied);
    allocation.width =
        (unsigned int)(request.width / BOXRIG_UI_FIXED_ONE);
    allocation.height =
        (unsigned int)(request.height / BOXRIG_UI_FIXED_ONE);
    CHECK(boxrig_win32_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(present_probe.count > committed_present_count &&
          present_probe.valid &&
          present_probe.size.cx == (LONG)allocation.width &&
          present_probe.size.cy == (LONG)allocation.height &&
          present_probe.dirty.left == 0 &&
          present_probe.dirty.top == 0 &&
          present_probe.dirty.right == present_probe.size.cx &&
          present_probe.dirty.bottom == present_probe.size.cy);

    committed_present_count = present_probe.count;
    allocation.width++;
    allocation.height++;
    CHECK(boxrig_win32_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(present_probe.count == committed_present_count + 1 &&
          present_probe.valid &&
          present_probe.size.cx == (LONG)allocation.width &&
          present_probe.size.cy == (LONG)allocation.height &&
          present_probe.dirty.left == 0 &&
          present_probe.dirty.top == 0 &&
          present_probe.dirty.right == present_probe.size.cx &&
          present_probe.dirty.bottom == present_probe.size.cy);

    CHECK(GetWindowRect(presentation_window, &presentation_after));
    CHECK(GetWindowRect(input_window, &input_after));
    CHECK(EqualRect(&presentation_before, &presentation_after));
    CHECK(EqualRect(&input_before, &input_after));
    CHECK(GetWindowLongPtrW(presentation_window, GWL_STYLE) ==
          presentation_style);
    CHECK(GetWindowLongPtrW(presentation_window, GWL_EXSTYLE) ==
          presentation_extended_style);
    CHECK(GetWindowLongPtrW(presentation_window, GWLP_WNDPROC) ==
          presentation_procedure);
    CHECK(GetWindowLongPtrW(presentation_window, GWLP_USERDATA) ==
          presentation_user_data);
    CHECK(GetWindowLongPtrW(input_window, GWL_STYLE) == input_style);
    CHECK(GetWindowLongPtrW(input_window, GWL_EXSTYLE) ==
          input_extended_style);
    CHECK(GetWindowLongPtrW(input_window, GWLP_WNDPROC) == input_procedure);
    CHECK(GetWindowLongPtrW(input_window, GWLP_USERDATA) == input_user_data);

    boxrig_win32_terminal_component_destroy(component);
    component = NULL;
    CHECK(IsWindow(presentation_window) && IsWindow(input_window));
    CHECK(descendant_window_count(parent) == descendant_count);
    CHECK(GetWindowLongPtrW(presentation_window, GWLP_USERDATA) ==
          presentation_user_data);
    CHECK(GetWindowLongPtrW(input_window, GWLP_USERDATA) == input_user_data);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(presentation_window));
    CHECK(DestroyWindow(input_window));
    CHECK(DestroyWindow(parent));
    return true;
}

static bool borrowed_composited_scrollbar_shares_generation(void)
{
    static const WCHAR host_class[] =
        L"boxrig_component_composited_scrollbar_host";
    HINSTANCE instance = GetModuleHandleW(NULL);
    HWND target_window;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_win32_terminal_component_config config =
        boxrig_win32_terminal_component_config_default();
    boxrig_win32_terminal_borrowed_target target;
    boxrig_win32_terminal_component *component = NULL;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_component_allocation allocation;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame;
    boxrig_ui_size_command request;
    boxrig_font_visual_step_policy zoom_policy =
        boxrig_font_visual_step_policy_default();
    boxrig_scrollbar_model scrollbar_model;
    boxrig_scrollbar_thumb thumb;
    presentation_probe present_probe = {0};
    POINT wheel_point;
    RECT client;
    LRESULT message_result = 0;
    const uint8_t *output = NULL;
    const uint8_t expected_press[] = "\x1b[<0;3;2M";
    const uint8_t expected_wheel[] = "\x1b[<64;4;3M";
    uint64_t terminal_before;
    uint64_t scrollbar_before;
    size_t output_length;
    size_t committed_present_count;
    int scrollbar_metric = GetSystemMetrics(SM_CXVSCROLL);
    unsigned int scrollbar_width =
        scrollbar_metric > 0 ? (unsigned int)scrollbar_metric : 1;
    uint32_t capabilities;
    bool changed = false;
    bool applied = false;

    CHECK(instance != NULL && register_host_class(instance, host_class));
    target_window = CreateWindowExW(
        0,
        host_class,
        L"composited scrollbar target",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        420,
        220,
        NULL,
        NULL,
        instance,
        NULL);
    CHECK(target_window != NULL && GetClientRect(target_window, &client));
    terminal_config.rows = 3;
    terminal_config.columns = 6;
    terminal_config.scrollback_capacity = 32;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    config.scrollbar_presentation =
        BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED;
    present_probe.scrollbar_width = scrollbar_width;
    target.presentation_window = target_window;
    target.input_window = target_window;
    target.presenter = present_external_surface;
    target.presenter_context = &present_probe;
    CHECK(boxrig_win32_terminal_component_create_on_target(
              instance,
              &target,
              model,
              &config,
              &component) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_set_target_extent(
              component,
              (unsigned int)(client.right - client.left),
              (unsigned int)(client.bottom - client.top)) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_get_effective_metrics(
              component,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.x = 23;
    allocation.y = 19;
    allocation.width =
        metrics.cell_width * terminal_config.columns + scrollbar_width;
    allocation.height = metrics.cell_height * terminal_config.rows;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_win32_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(layout.columns == terminal_config.columns &&
          layout.excess_right == 0);
    CHECK(boxrig_win32_terminal_component_scrollbar_window(component) ==
          NULL);
    capabilities = boxrig_win32_terminal_component_capabilities(component);
    CHECK((capabilities &
           BOXRIG_UI_COMPONENT_CAPABILITY_COMPOSITED_SCROLLBAR) != 0);
    CHECK((capabilities &
           BOXRIG_UI_COMPONENT_CAPABILITY_NATIVE_CHILD_SCROLLBAR) == 0);
    CHECK(boxrig_win32_terminal_component_set_visible(component, true) ==
          BOXRIG_UI_OK);

    CHECK(feed_text(
        terminal,
        "\x1b[?25l\x1b[2J\x1b[H"
        "00\r\n01\r\n02\r\n03\r\n04\r\n05\r\n06\r\n07\r\n"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(present_probe.count > 0 && present_probe.valid &&
          present_probe.pixels_valid);
    CHECK(present_probe.size.cx == (LONG)allocation.width &&
          present_probe.size.cy == (LONG)allocation.height);
    CHECK(boxrig_win32_terminal_component_get_scrollbar_model(
              component,
              &scrollbar_model) == BOXRIG_UI_OK);
    CHECK(scrollbar_model.enabled && scrollbar_model.viewport_offset == 0);
    boxrig_scrollbar_calculate_thumb(
        &scrollbar_model,
        allocation.height,
        scrollbar_width,
        &thumb);
    CHECK(thumb.length != 0 && thumb.start != 0);
    CHECK(present_probe.track_pixel != present_probe.thumb_pixel);
    terminal_before = present_probe.terminal_hash;
    scrollbar_before = present_probe.scrollbar_hash;

    CHECK(feed_text(terminal, "\x1b[?1000;1006h"));
    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              target_window,
              WM_LBUTTONDOWN,
              MK_LBUTTON,
              MAKELPARAM(
                  allocation.x + (int)(metrics.cell_width * 2) + 1,
                  allocation.y + (int)metrics.cell_height + 1),
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected_press) - 1 &&
          memcmp(output, expected_press, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    wheel_point.x =
        allocation.x + (int)(metrics.cell_width * 3) + 1;
    wheel_point.y =
        allocation.y + (int)(metrics.cell_height * 2) + 1;
    CHECK(ClientToScreen(target_window, &wheel_point));
    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              target_window,
              WM_MOUSEWHEEL,
              MAKEWPARAM(0, WHEEL_DELTA),
              MAKELPARAM(wheel_point.x, wheel_point.y),
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected_wheel) - 1 &&
          memcmp(output, expected_wheel, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              target_window,
              WM_LBUTTONDOWN,
              MK_LBUTTON,
              MAKELPARAM(allocation.x - 1, allocation.y),
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_UNRELATED);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    message_result = 0;
    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              target_window,
              WM_XBUTTONDOWN,
              MAKEWPARAM(0, XBUTTON1),
              MAKELPARAM(
                  allocation.x + (int)allocation.width - 1,
                  allocation.y),
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(message_result == TRUE);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              target_window,
              WM_LBUTTONDOWN,
              MK_LBUTTON,
              MAKELPARAM(
                  allocation.x + (int)allocation.width - 1,
                  allocation.y),
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_terminal_viewport_offset(terminal) != 0);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(present_probe.terminal_hash != terminal_before);
    CHECK(present_probe.scrollbar_hash != scrollbar_before);

    CHECK(boxrig_win32_terminal_component_get_scrollbar_model(
              component,
              &scrollbar_model) == BOXRIG_UI_OK);
    boxrig_scrollbar_calculate_thumb(
        &scrollbar_model,
        allocation.height,
        scrollbar_width,
        &thumb);
    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              target_window,
              WM_LBUTTONDOWN,
              MK_LBUTTON,
              MAKELPARAM(
                  allocation.x + (int)allocation.width - 1,
                  allocation.y + (int)thumb.start),
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              target_window,
              WM_MOUSEMOVE,
              MK_LBUTTON,
              MAKELPARAM(
                  allocation.x + (int)allocation.width - 1,
                  allocation.y + (int)allocation.height - 1),
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(boxrig_win32_terminal_component_dispatch_message(
              component,
              target_window,
              WM_LBUTTONUP,
              0,
              MAKELPARAM(
                  allocation.x + (int)allocation.width - 1,
                  allocation.y + (int)allocation.height - 1),
              &message_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 0);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);

    committed_present_count = present_probe.count;
    zoom_policy.minimum_size_dip = metrics.requested_size_dip;
    zoom_policy.maximum_size_dip = metrics.requested_size_dip + 4.0f;
    CHECK(boxrig_win32_terminal_component_visual_zoom(
              component,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(take_content_size_action(
        component,
        &request));
    CHECK(boxrig_win32_terminal_component_complete_size_command(
              component,
              request.revision,
              BOXRIG_UI_SIZE_ACCEPTED,
              request.width,
              request.height,
              &applied,
              &layout) == BOXRIG_UI_OK);
    CHECK(applied);
    allocation.width =
        (unsigned int)(request.width / BOXRIG_UI_FIXED_ONE) +
        scrollbar_width;
    allocation.height =
        (unsigned int)(request.height / BOXRIG_UI_FIXED_ONE);
    CHECK(boxrig_win32_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(present_probe.count > committed_present_count &&
          present_probe.valid && present_probe.pixels_valid &&
          present_probe.size.cx == (LONG)allocation.width &&
          present_probe.size.cy == (LONG)allocation.height &&
          present_probe.dirty.left == 0 &&
          present_probe.dirty.top == 0 &&
          present_probe.dirty.right == present_probe.size.cx &&
          present_probe.dirty.bottom == present_probe.size.cy);

    boxrig_win32_terminal_component_destroy(component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(IsWindow(target_window));
    CHECK(DestroyWindow(target_window));
    return true;
}

static bool zoom_waits_for_matching_allocation(void)
{
    static const WCHAR host_class[] = L"boxrig_component_zoom_test_host";
    HINSTANCE instance = GetModuleHandleW(NULL);
    HWND parent;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_win32_terminal_component_config config =
        boxrig_win32_terminal_component_config_default();
    boxrig_win32_terminal_component *component = NULL;
    boxrig_font_effective_metrics metrics;
    boxrig_font_effective_metrics first_zoom_metrics;
    boxrig_font_effective_metrics latest_zoom_metrics;
    boxrig_font_visual_step_policy zoom_policy =
        boxrig_font_visual_step_policy_default();
    boxrig_ui_component_allocation allocation;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame;
    boxrig_ui_size_command request;
    boxrig_ui_size_command first_request;
    HWND child;
    uint64_t committed_hash;
    uint64_t pending_hash;
    uint64_t allocation_hash;
    uint64_t completed_hash;
    bool changed = false;
    bool applied = false;

    CHECK(instance != NULL && register_host_class(instance, host_class));
    parent = CreateWindowExW(
        0,
        host_class,
        L"zoom host",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        640,
        360,
        NULL,
        NULL,
        instance,
        NULL);
    CHECK(parent != NULL);
    terminal_config.rows = 3;
    terminal_config.columns = 8;
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_create_child(
              instance,
              parent,
              model,
              &config,
              &component) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_get_effective_metrics(
              component,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.width = metrics.cell_width * terminal_config.columns;
    allocation.height = metrics.cell_height * terminal_config.rows;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_win32_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_set_visible(component, true) ==
          BOXRIG_UI_OK);
    ShowWindow(parent, SW_SHOWNA);
    CHECK(feed_text(
        terminal,
        "\x1b[?25l\x1b[2J\x1b[HMMMMMMMM\r\nWWWWWWWW"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    child = boxrig_win32_terminal_component_owned_window(component);
    CHECK(InvalidateRect(child, NULL, FALSE));
    CHECK(UpdateWindow(child));
    CHECK(read_window_hash(
        child,
        allocation.width,
        allocation.height,
        &committed_hash));

    zoom_policy.minimum_size_dip = metrics.requested_size_dip;
    zoom_policy.maximum_size_dip = metrics.requested_size_dip + 30.0f;
    CHECK(boxrig_win32_terminal_component_visual_zoom(
              component,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(!take_content_size_action(
        component,
        &request));
    CHECK(boxrig_win32_terminal_component_get_effective_metrics(
              component,
              &first_zoom_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_win32_terminal_component_visual_zoom(
              component,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(boxrig_win32_terminal_component_visual_zoom(
              component,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(!take_content_size_action(
        component,
        &request));
    CHECK(boxrig_win32_terminal_component_get_effective_metrics(
              component,
              &latest_zoom_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(latest_zoom_metrics.requested_size_dip ==
          first_zoom_metrics.requested_size_dip);
    CHECK(feed_text(terminal, "\x1b[?2026h"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(!frame.additional_dispatch);
    CHECK(!take_content_size_action(
        component,
        &request));
    CHECK(feed_text(terminal, "\x1b[?2026l"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(InvalidateRect(child, NULL, FALSE));
    CHECK(UpdateWindow(child));
    CHECK(read_window_hash(
        child,
        allocation.width,
        allocation.height,
        &pending_hash));
    CHECK(pending_hash == committed_hash);
    CHECK(take_content_size_action(
        component,
        &request));
    first_request = request;

    CHECK(request.width % BOXRIG_UI_FIXED_ONE == 0);
    CHECK(request.height % BOXRIG_UI_FIXED_ONE == 0);
    CHECK(boxrig_win32_terminal_component_complete_size_command(
              component,
              request.revision,
              BOXRIG_UI_SIZE_ACCEPTED,
              request.width,
              request.height,
              &applied,
              &layout) == BOXRIG_UI_OK);
    CHECK(applied);
    allocation.width =
        (unsigned int)(request.width / BOXRIG_UI_FIXED_ONE);
    allocation.height =
        (unsigned int)(request.height / BOXRIG_UI_FIXED_ONE);
    CHECK(boxrig_win32_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(InvalidateRect(child, NULL, FALSE));
    CHECK(UpdateWindow(child));
    CHECK(read_window_hash(
        child,
        allocation.width,
        allocation.height,
        &allocation_hash));
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(InvalidateRect(child, NULL, FALSE));
    CHECK(UpdateWindow(child));
    CHECK(read_window_hash(
        child,
        allocation.width,
        allocation.height,
        &completed_hash));
    CHECK(allocation_hash == completed_hash);
    CHECK(boxrig_win32_terminal_component_get_effective_metrics(
              component,
              &latest_zoom_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(latest_zoom_metrics.requested_size_dip >
          first_zoom_metrics.requested_size_dip);
    CHECK(!take_content_size_action(
        component,
        &request));
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(take_content_size_action(
        component,
        &request));
    CHECK(request.revision != first_request.revision);
    CHECK(request.width > first_request.width);
    CHECK(request.height > first_request.height);

    applied = false;
    CHECK(boxrig_win32_terminal_component_complete_size_command(
              component,
              request.revision,
              BOXRIG_UI_SIZE_ACCEPTED,
              request.width,
              request.height,
              &applied,
              &layout) == BOXRIG_UI_OK);
    CHECK(applied);
    allocation.width =
        (unsigned int)(request.width / BOXRIG_UI_FIXED_ONE);
    allocation.height =
        (unsigned int)(request.height / BOXRIG_UI_FIXED_ONE);
    CHECK(boxrig_win32_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(!take_content_size_action(
        component,
        &request));

    CHECK(boxrig_win32_terminal_component_visual_zoom_reset(
              component,
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(!take_content_size_action(
        component,
        &request));
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(take_content_size_action(
        component,
        &request));
    applied = false;
    CHECK(boxrig_win32_terminal_component_complete_size_command(
              component,
              request.revision,
              BOXRIG_UI_SIZE_ACCEPTED,
              request.width,
              request.height,
              &applied,
              &layout) == BOXRIG_UI_OK);
    CHECK(applied);
    allocation.width =
        (unsigned int)(request.width / BOXRIG_UI_FIXED_ONE);
    allocation.height =
        (unsigned int)(request.height / BOXRIG_UI_FIXED_ONE);
    CHECK(boxrig_win32_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_get_effective_metrics(
              component,
              &latest_zoom_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(latest_zoom_metrics.requested_size_dip == metrics.requested_size_dip);
    CHECK(latest_zoom_metrics.cell_width == metrics.cell_width);
    CHECK(latest_zoom_metrics.cell_height == metrics.cell_height);
    CHECK(!take_content_size_action(
        component,
        &request));

    boxrig_win32_terminal_component_destroy(component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(parent));
    return true;
}

static bool resize_keeps_retained_interior_valid(void)
{
    static const WCHAR host_class[] = L"boxrig_component_resize_region_host";
    HINSTANCE instance = GetModuleHandleW(NULL);
    HWND parent;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_win32_terminal_component_config config =
        boxrig_win32_terminal_component_config_default();
    boxrig_win32_terminal_component *component = NULL;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_component_allocation allocation;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame;
    HWND child;
    HDC child_context = NULL;
    HRGN update_region = NULL;
    unsigned int old_width;
    unsigned int old_height;
    int region_kind;

    CHECK(instance != NULL && register_host_class(instance, host_class));
    parent = CreateWindowExW(
        0,
        host_class,
        L"resize region host",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        320,
        200,
        NULL,
        NULL,
        instance,
        NULL);
    CHECK(parent != NULL);
    terminal_config.rows = 2;
    terminal_config.columns = 4;
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_create_child(
              instance,
              parent,
              model,
              &config,
              &component) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_get_effective_metrics(
              component,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.width = metrics.cell_width * terminal_config.columns;
    allocation.height = metrics.cell_height * terminal_config.rows;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_win32_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_set_visible(component, true) ==
          BOXRIG_UI_OK);
    ShowWindow(parent, SW_SHOWNA);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    child = boxrig_win32_terminal_component_owned_window(component);
    (void)UpdateWindow(child);
    CHECK(ValidateRect(child, NULL));

    old_width = allocation.width;
    old_height = allocation.height;
    allocation.width += metrics.cell_width;
    allocation.height += metrics.cell_height;
    CHECK(boxrig_win32_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    update_region = CreateRectRgn(0, 0, 0, 0);
    CHECK(update_region != NULL);
    region_kind = GetUpdateRgn(child, update_region, FALSE);
    CHECK(region_kind == NULLREGION);
    child_context = GetDC(child);
    CHECK(child_context != NULL);
    CHECK(GetPixel(
              child_context,
              (int)allocation.width - 1,
              (int)(old_height / 2)) == RGB(0x10, 0x10, 0x10));
    CHECK(GetPixel(
              child_context,
              (int)(old_width / 2),
              (int)allocation.height - 1) == RGB(0x10, 0x10, 0x10));
    CHECK(ReleaseDC(child, child_context) == 1);

    CHECK(DeleteObject(update_region));
    boxrig_win32_terminal_component_destroy(component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(parent));
    return true;
}


static boxrig_input_mouse_route scrollbar_mouse_probe(const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit, void *context)
{
    (void)event; (void)hit;
    unsigned int *calls = context;
    (*calls)++;
    return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
}

static bool scrollbar_visibility_changes_layout(void)
{
    HWND parent = CreateWindowExW(0, L"STATIC", L"scrollbar visibility", WS_OVERLAPPEDWINDOW, 0, 0, 600, 200, NULL, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(parent != NULL);
    for (int presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
         presentation <= BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED; presentation++) {
        boxrig_terminal_config tc = boxrig_terminal_config_default();
        tc.rows = 3; tc.columns = 20; tc.scrollback_capacity = 100;
        boxrig_terminal *terminal = NULL;
        boxrig_ui_model *model = NULL;
        boxrig_win32_terminal_component *component = NULL;
        boxrig_win32_terminal_component_config config = boxrig_win32_terminal_component_config_default();
        config.scrollbar_presentation = (boxrig_ui_component_scrollbar_presentation)presentation;
        CHECK(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_terminal_component_create_child(GetModuleHandleW(NULL), parent, model, &config, &component) == BOXRIG_UI_OK);
        boxrig_font_effective_metrics metrics;
        CHECK(boxrig_win32_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
        boxrig_ui_scrollbar_visibility_state state;
        CHECK(boxrig_win32_terminal_component_get_scrollbar_visibility(component, &state) == BOXRIG_UI_OK);
        if (presentation == BOXRIG_UI_COMPONENT_SCROLLBAR_NONE) {
            CHECK(!state.visible && state.reserved_width == 0 && !state.pending);
            CHECK(boxrig_win32_terminal_component_set_scrollbar_visibility(component, BOXRIG_UI_SCROLLBAR_NEVER) == BOXRIG_UI_OK);
            CHECK(boxrig_win32_terminal_component_set_scrollbar_visibility(component, BOXRIG_UI_SCROLLBAR_ALWAYS) == BOXRIG_UI_OK);
            CHECK(boxrig_win32_terminal_component_get_scrollbar_visibility(component, &state) == BOXRIG_UI_OK);
            CHECK(!state.visible && state.reserved_width == 0 && !state.pending);
            boxrig_win32_terminal_component_destroy(component);
            boxrig_ui_model_destroy(model);
            boxrig_terminal_destroy(terminal);
            continue;
        }
        CHECK(state.requested == BOXRIG_UI_SCROLLBAR_ALWAYS && state.visible && state.reserved_width > 0 && !state.pending);
        const unsigned int bar_width = state.reserved_width;
        boxrig_ui_component_allocation allocation = {0};
        allocation.width = metrics.cell_width * 20 + bar_width;
        allocation.height = metrics.cell_height * 3;
        allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
        boxrig_ui_layout_result layout;
        CHECK(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
        boxrig_ui_layout_action action;
        /* Drain the initial canonical allocation, if it produced one. */
        if (boxrig_win32_terminal_component_take_layout_action(component, &action)) {
            CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
            CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows, action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
            CHECK(boxrig_win32_terminal_component_complete_grid_resize(component, action.revision, true) == BOXRIG_UI_OK);
        }
        CHECK(feed_text(terminal, "0\r\n1\r\n2\r\n3\r\n4\r\n5"));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        boxrig_ui_component_frame_result frame;
        CHECK(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        ShowWindow(parent, SW_SHOWNA);
        CHECK(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
        size_t history = boxrig_terminal_scrollback_rows(terminal);
        CHECK(history > 0);
        CHECK(feed_text(terminal, "\033[?1003;1006h"));
        unsigned int mouse_calls = 0;
        HWND input = boxrig_win32_terminal_component_input_window(component);
        LRESULT native_result;
        if (presentation == BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED) {
            CHECK(boxrig_win32_terminal_component_set_mouse_router(component, scrollbar_mouse_probe, &mouse_calls) == BOXRIG_UI_OK);
            CHECK(boxrig_win32_terminal_component_dispatch_message(component, input, WM_LBUTTONDOWN,
                MK_LBUTTON, MAKELPARAM(allocation.width - 2, allocation.height - 2), &native_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
            CHECK(GetCapture() == input);
            CHECK(boxrig_win32_terminal_component_dispatch_message(component, input, WM_MOUSEMOVE,
                MK_LBUTTON, MAKELPARAM(allocation.width - 2, 0), &native_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
            boxrig_scrollbar_model scroll;
            CHECK(boxrig_win32_terminal_component_get_scrollbar_model(component, &scroll) == BOXRIG_UI_OK && scroll.position == 0);
            CHECK(mouse_calls == 0);
        }

        CHECK(boxrig_win32_terminal_component_set_scrollbar_visibility(component, BOXRIG_UI_SCROLLBAR_NEVER) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_terminal_component_get_scrollbar_visibility(component, &state) == BOXRIG_UI_OK);
        CHECK(state.requested == BOXRIG_UI_SCROLLBAR_NEVER);
        CHECK(boxrig_win32_terminal_component_take_layout_action(component, &action));
        CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(action.value.grid_resize.columns == allocation.width / metrics.cell_width);
        CHECK(state.pending && state.visible && state.reserved_width == bar_width);
        CHECK(boxrig_win32_terminal_component_set_scrollbar_visibility(component, BOXRIG_UI_SCROLLBAR_ALWAYS) == BOXRIG_UI_BUSY);
        boxrig_font_specification specification;
        boxrig_font_rendering_policy policy;
        CHECK(boxrig_win32_terminal_component_get_font_specification(component, &specification) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_terminal_component_get_font_rendering_policy(component, &policy) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_terminal_component_set_font_specification(component, &specification) == BOXRIG_UI_BUSY);
        policy.output_scale = 2.0f;
        CHECK(boxrig_win32_terminal_component_set_font_rendering_policy(component, &policy) == BOXRIG_UI_BUSY);
        bool zoom_changed = true;
        CHECK(boxrig_win32_terminal_component_visual_zoom(component, BOXRIG_FONT_ZOOM_INCREASE,
            boxrig_font_visual_step_policy_default(), &zoom_changed) == BOXRIG_UI_BUSY && !zoom_changed);
        CHECK(boxrig_win32_terminal_component_visual_zoom_reset(component, &zoom_changed) == BOXRIG_UI_BUSY && !zoom_changed);

        CHECK(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        CHECK(frame.frame_pending);
        CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows, action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(boxrig_win32_terminal_component_complete_grid_resize(component, action.revision, true) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_terminal_component_get_scrollbar_visibility(component, &state) == BOXRIG_UI_OK);
        CHECK(!state.visible && state.reserved_width == 0 && !state.pending);
        if (presentation == BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED) {
            CHECK(GetCapture() != input);
            CHECK(boxrig_win32_terminal_component_dispatch_message(component, input, WM_LBUTTONUP,
                0, MAKELPARAM(2, 2), &native_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
            CHECK(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
            CHECK(mouse_calls == 0);
        }

        CHECK(!boxrig_win32_terminal_component_take_layout_action(component, &action));
        if (presentation == BOXRIG_UI_COMPONENT_SCROLLBAR_NATIVE_CHILD) {
            CHECK(!IsWindowVisible(boxrig_win32_terminal_component_scrollbar_window(component)));
        }
        CHECK(boxrig_win32_terminal_component_set_visible(component, false) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_terminal_component_get_scrollbar_visibility(component, &state) == BOXRIG_UI_OK);
        CHECK(!state.visible && state.reserved_width == 0);
        CHECK(boxrig_terminal_scrollback_rows(terminal) == history);
        const boxrig_ui_scrollbar_visibility policies[] = {
            BOXRIG_UI_SCROLLBAR_AUTO, BOXRIG_UI_SCROLLBAR_ALWAYS,
            BOXRIG_UI_SCROLLBAR_NEVER, BOXRIG_UI_SCROLLBAR_AUTO};
        for (size_t i = 0; i < sizeof(policies)/sizeof(*policies); i++) {
            CHECK(boxrig_win32_terminal_component_set_scrollbar_visibility(component, policies[i]) == BOXRIG_UI_OK);
            if (boxrig_win32_terminal_component_take_layout_action(component, &action)) {
                CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
                CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows, action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
                CHECK(boxrig_win32_terminal_component_complete_grid_resize(component, action.revision, true) == BOXRIG_UI_OK);
            }
            CHECK(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
            CHECK(boxrig_win32_terminal_component_get_scrollbar_visibility(component, &state) == BOXRIG_UI_OK);
            CHECK(state.visible == (policies[i] != BOXRIG_UI_SCROLLBAR_NEVER));
            CHECK(!state.pending && !boxrig_win32_terminal_component_take_layout_action(component, &action));
        }
        /* AUTO follows history availability, including alternate-screen changes. */
        const char *sequences[] = {"\033[?1049h", "\033[?1049l", "\033[3J"};
        for (size_t i = 0; i < 3; i++) {
            CHECK(feed_text(terminal, sequences[i]));
            CHECK(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
            if (boxrig_win32_terminal_component_take_layout_action(component, &action)) {
                CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
                CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows, action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
                CHECK(boxrig_win32_terminal_component_complete_grid_resize(component, action.revision, true) == BOXRIG_UI_OK);
            }
            CHECK(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
            CHECK(boxrig_win32_terminal_component_get_scrollbar_visibility(component, &state) == BOXRIG_UI_OK);
            CHECK(state.visible == (i == 1) && !state.pending);
            CHECK(!boxrig_win32_terminal_component_take_layout_action(component, &action));
        }
        CHECK(boxrig_win32_terminal_component_set_scrollbar_visibility(component, (boxrig_ui_scrollbar_visibility)99) == BOXRIG_UI_INVALID_ARGUMENT);
        CHECK(boxrig_win32_terminal_component_set_scrollbar_visibility(component, BOXRIG_UI_SCROLLBAR_ALWAYS) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_terminal_component_take_layout_action(component, &action));
        CHECK(boxrig_win32_terminal_component_complete_grid_resize(component, action.revision, false) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_terminal_component_get_scrollbar_visibility(component, &state) == BOXRIG_UI_OK);
        CHECK(!state.visible && !state.pending && state.requested == BOXRIG_UI_SCROLLBAR_AUTO);
        boxrig_ui_component_diagnostic diagnostic;
        CHECK(boxrig_win32_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
        CHECK(!diagnostic.running);
        boxrig_win32_terminal_component_destroy(component);
        boxrig_ui_model_destroy(model);
        boxrig_terminal_destroy(terminal);
    }
    DestroyWindow(parent);
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 1;
    for (omitted_interface = 0; omitted_interface < OPTIONAL_INTERFACE_SCENARIOS; omitted_interface++) {
        omitted_interface_queries = 0;
        if (!scrollbar_visibility_changes_layout() ||
            !component_is_embedded_and_independent(argv[1]) ||
            !constrained_zoom_uses_one_layout_action() ||
            !borrowed_target_preserves_caller_windows() ||
            !borrowed_composited_scrollbar_shares_generation() ||
            !zoom_waits_for_matching_allocation() ||
            !resize_keeps_retained_interior_valid() ||
            !awaiting_reconversion_allows_focus_lifecycle() ||
            !copy_survives_component_destruction()) {
            return 1;
        }
        if (omitted_interface != 0 && omitted_interface_queries == 0) return 1;
        printf("PASS: Win32 component with omitted optional interface %u\n", omitted_interface);
    }
    puts("PASS: Win32 terminal component");
    return 0;
}
