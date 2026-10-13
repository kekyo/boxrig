/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "boxrig/ui/editable.h"
#include "boxrig/ui/input.h"
#include "boxrig/ui/view.h"
#include "boxrig/win32/component.h"
#include "win32_reconversion_control.h"

#include <shellapi.h>
#include <windows.h>

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define HOST_CLASS_NAME L"boxrig_reconversion_host"
#define HOST_TIMER_ID ((UINT_PTR)1)
#define HOST_TIMER_INTERVAL 50U
#define HOST_HEADER_HEIGHT 176
#define HOST_ACTION_MESSAGE (WM_APP + 1)
#define HOST_DOCUMENT_CAPACITY BOXRIG_UI_EDITABLE_CONTEXT_CAPACITY_DEFAULT
#define HOST_TERMINAL_OUTPUT_CAPACITY (64U * 1024U)

static const char initial_document[] =
    "before \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e after";
static const char self_test_replacement[] =
    "\xe5\x86\x8d\xe5\xa4\x89\xe6\x8f\x9b";
static UINT revision_message;

typedef struct reconversion_host {
    HINSTANCE instance;
    HWND window;
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_win32_terminal_component *component;
    boxrig_ui_editable_document *editable;
    wchar_t report_path[MAX_PATH];
    unsigned int report_revision;
    char document[HOST_DOCUMENT_CAPACITY];
    size_t document_length;
    uint64_t document_identity;
    uint64_t document_revision;
    uint64_t context_offset;
    size_t selection_offset;
    size_t selection_length;
    char last_replacement[BOXRIG_UI_EDITABLE_SELECTION_CAPACITY_MAX];
    size_t last_replacement_length;
    char terminal_output[HOST_TERMINAL_OUTPUT_CAPACITY];
    size_t terminal_output_length;
    uint64_t replacement_count;
    uint64_t last_request_id;
    uint64_t last_base_revision;
    uint64_t last_range_offset;
    size_t last_range_length;
    const char *last_outcome;
    const char *last_action;
    boxrig_ui_editable_reconversion_state observed_reconversion_state;
    bool observed_reconversion_state_valid;
    bool reconversion_capable;
    bool reject_next_replacement;
    bool ready;
    bool report_dirty;
    bool failed;
} reconversion_host;

static const char *reconversion_state_name(
    boxrig_ui_editable_reconversion_state state)
{
    switch (state) {
    case BOXRIG_UI_EDITABLE_RECONVERSION_NONE:
        return "none";
    case BOXRIG_UI_EDITABLE_RECONVERSION_ACTIVE:
        return "active";
    case BOXRIG_UI_EDITABLE_RECONVERSION_STALE:
        return "stale";
    case BOXRIG_UI_EDITABLE_RECONVERSION_AWAITING_COMPLETION:
        return "awaitingCompletion";
    default:
        return "invalid";
    }
}

static const char *ui_status_name(boxrig_ui_status status)
{
    switch (status) {
    case BOXRIG_UI_OK:
        return "ok";
    case BOXRIG_UI_INVALID_ARGUMENT:
        return "invalidArgument";
    case BOXRIG_UI_OUT_OF_MEMORY:
        return "outOfMemory";
    case BOXRIG_UI_LIMIT_EXCEEDED:
        return "limitExceeded";
    case BOXRIG_UI_UNAVAILABLE:
        return "unavailable";
    case BOXRIG_UI_BUSY:
        return "busy";
    case BOXRIG_UI_STALE:
        return "stale";
    default:
        return "invalid";
    }
}

static void initialize_document(reconversion_host *host)
{
    memcpy(host->document, initial_document, sizeof(initial_document) - 1);
    host->document_length = sizeof(initial_document) - 1;
    host->document_identity = UINT64_C(7001);
    host->document_revision = UINT64_C(1);
    host->context_offset = UINT64_C(100);
    host->selection_offset = 7;
    host->selection_length = 9;
    host->last_outcome = "none";
    host->last_action = "ready";
    host->report_dirty = true;
}

static boxrig_ui_status publish_document(reconversion_host *host)
{
    boxrig_ui_editable_document_snapshot_input snapshot;

    if (host == NULL || host->editable == NULL ||
        host->selection_length == 0 ||
        host->selection_offset > host->document_length ||
        host->selection_length >
            host->document_length - host->selection_offset ||
        (uint64_t)host->selection_offset >
            UINT64_MAX - host->context_offset) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.document_identity = host->document_identity;
    snapshot.document_revision = host->document_revision;
    snapshot.context = host->document;
    snapshot.context_length = host->document_length;
    snapshot.context_offset = host->context_offset;
    snapshot.selection.offset =
        host->context_offset + (uint64_t)host->selection_offset;
    snapshot.selection.length = host->selection_length;
    return boxrig_ui_editable_document_publish(host->editable, &snapshot);
}

static bool write_json_string(FILE *file, const char *text, size_t length)
{
    size_t index;

    if (fputc('"', file) == EOF) {
        return false;
    }
    for (index = 0; index < length; index++) {
        unsigned char byte = (unsigned char)text[index];

        if (byte == '"' || byte == '\\') {
            if (fputc('\\', file) == EOF || fputc(byte, file) == EOF) {
                return false;
            }
        } else if (byte == '\b') {
            if (fputs("\\b", file) == EOF) {
                return false;
            }
        } else if (byte == '\f') {
            if (fputs("\\f", file) == EOF) {
                return false;
            }
        } else if (byte == '\n') {
            if (fputs("\\n", file) == EOF) {
                return false;
            }
        } else if (byte == '\r') {
            if (fputs("\\r", file) == EOF) {
                return false;
            }
        } else if (byte == '\t') {
            if (fputs("\\t", file) == EOF) {
                return false;
            }
        } else if (byte < 0x20) {
            if (fprintf(file, "\\u%04x", (unsigned int)byte) < 0) {
                return false;
            }
        } else if (fputc(byte, file) == EOF) {
            return false;
        }
    }
    return fputc('"', file) != EOF;
}

static bool report_snapshot_path(
    const reconversion_host *host, unsigned int revision, wchar_t *path)
{
    int length = _snwprintf(path, MAX_PATH, L"%ls.%010u",
        host->report_path, revision);
    if (length < 0 || length >= MAX_PATH) {
        path[0] = L'\0';
        return false;
    }
    return true;
}

/* A remote reader may retain a snapshot without allowing deletion. Publish
 * each completed observation under its own name so later input can proceed. */
static int write_report(reconversion_host *host)
{
    wchar_t temporary_path[MAX_PATH];
    wchar_t completed_path[MAX_PATH];
    size_t selected_offset;
    FILE *file;
    bool valid = true;
    int temporary_length;

    if (host == NULL || host->report_path[0] == L'\0' ||
        host->report_revision == UINT_MAX) {
        return -1;
    }
    unsigned int next_revision = host->report_revision + 1u;
    if (!report_snapshot_path(host, next_revision, completed_path)) return -1;
    temporary_length = swprintf(
        temporary_path,
        sizeof(temporary_path) / sizeof(temporary_path[0]),
        L"%ls.tmp",
        host->report_path);
    if (temporary_length <= 0 ||
        (size_t)temporary_length >=
            sizeof(temporary_path) / sizeof(temporary_path[0])) {
        return -1;
    }
    file = _wfopen(temporary_path, L"wb");
    if (file == NULL) {
        return -1;
    }
    selected_offset = host->selection_offset;
    valid = fprintf(
                file,
                "{\n"
                "  \"schema\": 1,\n"
                "  \"ready\": %s,\n"
                "  \"documentIdentity\": %" PRIu64 ",\n"
                "  \"documentRevision\": %" PRIu64 ",\n"
                "  \"contextOffset\": %" PRIu64 ",\n"
                "  \"context\": ",
                host->ready ? "true" : "false",
                host->document_identity,
                host->document_revision,
                host->context_offset) >= 0 &&
            write_json_string(
                file,
                host->document,
                host->document_length) &&
            fprintf(
                file,
                ",\n"
                "  \"selectionOffset\": %" PRIu64 ",\n"
                "  \"selectionLength\": %" PRIu64 ",\n"
                "  \"selectedText\": ",
                host->context_offset + (uint64_t)selected_offset,
                (uint64_t)host->selection_length) >= 0 &&
            write_json_string(
                file,
                host->document + selected_offset,
                host->selection_length) &&
            fprintf(
                file,
                ",\n"
                "  \"replacementCount\": %" PRIu64 ",\n"
                "  \"lastRequestId\": %" PRIu64 ",\n"
                "  \"lastBaseRevision\": %" PRIu64 ",\n"
                "  \"lastRangeOffset\": %" PRIu64 ",\n"
                "  \"lastRangeLength\": %" PRIu64 ",\n"
                "  \"lastReplacement\": ",
                host->replacement_count,
                host->last_request_id,
                host->last_base_revision,
                host->last_range_offset,
                (uint64_t)host->last_range_length) >= 0 &&
            write_json_string(
                file,
                host->last_replacement,
                host->last_replacement_length) &&
            fputs(",\n  \"lastOutcome\": ", file) != EOF &&
            write_json_string(
                file,
                host->last_outcome,
                strlen(host->last_outcome)) &&
            fputs(",\n  \"lastAction\": ", file) != EOF &&
            write_json_string(
                file,
                host->last_action,
                strlen(host->last_action)) &&
            fprintf(
                file,
                ",\n"
                "  \"reconversionCapable\": %s,\n"
                "  \"reconversionState\": ",
                host->reconversion_capable ? "true" : "false") >= 0 &&
            write_json_string(
                file,
                reconversion_state_name(
                    host->observed_reconversion_state_valid
                        ? host->observed_reconversion_state
                        : BOXRIG_UI_EDITABLE_RECONVERSION_NONE),
                strlen(reconversion_state_name(
                    host->observed_reconversion_state_valid
                        ? host->observed_reconversion_state
                        : BOXRIG_UI_EDITABLE_RECONVERSION_NONE))) &&
            fputs(",\n  \"terminalOutput\": ", file) != EOF &&
            write_json_string(
                file,
                host->terminal_output,
                host->terminal_output_length) &&
            fputs("\n}\n", file) != EOF;
    if (fflush(file) != 0 || ferror(file)) {
        valid = false;
    }
    if (fclose(file) != 0) {
        valid = false;
    }
    if (!valid || !MoveFileExW(
                      temporary_path,
                      completed_path,
                      MOVEFILE_WRITE_THROUGH)) {
        (void)DeleteFileW(temporary_path);
        return -1;
    }
    host->report_revision = next_revision;
    host->report_dirty = false;
    return 0;
}

static bool prepare_document_replacement(
    reconversion_host *host,
    const boxrig_ui_editable_replacement_request *request,
    size_t relative_offset,
    char *updated,
    size_t *updated_length_out)
{
    size_t suffix_offset;
    size_t suffix_length;
    size_t prefix_and_replacement;
    size_t updated_length;

    if (relative_offset > host->document_length ||
        request->range.length > host->document_length - relative_offset) {
        return false;
    }
    suffix_offset = relative_offset + request->range.length;
    suffix_length = host->document_length - suffix_offset;
    if (request->replacement_length >
            HOST_DOCUMENT_CAPACITY - relative_offset ||
        relative_offset + request->replacement_length >
            HOST_DOCUMENT_CAPACITY - suffix_length) {
        return false;
    }
    prefix_and_replacement = relative_offset + request->replacement_length;
    updated_length = prefix_and_replacement + suffix_length;
    memcpy(updated, host->document, relative_offset);
    if (request->replacement_length != 0) {
        memcpy(
            updated + relative_offset,
            request->replacement,
            request->replacement_length);
    }
    memcpy(
        updated + prefix_and_replacement,
        host->document + suffix_offset,
        suffix_length);
    *updated_length_out = updated_length;
    return true;
}

static int process_replacement(reconversion_host *host)
{
    boxrig_ui_editable_replacement_request request;
    boxrig_ui_editable_replacement_completion completion;
    char *updated;
    size_t updated_length = 0;
    size_t relative_offset = 0;
    uint64_t resulting_revision;
    bool current;
    bool replaceable = false;
    boxrig_ui_status completion_status;

    if (!boxrig_ui_editable_document_take_replacement(
            host->editable,
            &request)) {
        return 0;
    }
    if (request.replacement_length > sizeof(host->last_replacement)) {
        return -1;
    }
    host->replacement_count++;
    host->last_request_id = request.request_id;
    host->last_base_revision = request.base_document_revision;
    host->last_range_offset = request.range.offset;
    host->last_range_length = request.range.length;
    host->last_replacement_length = request.replacement_length;
    if (request.replacement_length != 0) {
        memcpy(
            host->last_replacement,
            request.replacement,
            request.replacement_length);
    }
    current = request.document_identity == host->document_identity &&
              request.base_document_revision == host->document_revision &&
              request.range.offset >= host->context_offset &&
              request.range.offset - host->context_offset <= SIZE_MAX;
    if (current) {
        relative_offset =
            (size_t)(request.range.offset - host->context_offset);
    }
    updated = malloc(HOST_DOCUMENT_CAPACITY);
    if (updated == NULL) {
        return -1;
    }
    if (current && !host->reject_next_replacement) {
        replaceable = prepare_document_replacement(
            host,
            &request,
            relative_offset,
            updated,
            &updated_length);
    }
    memset(&completion, 0, sizeof(completion));
    completion.request_id = request.request_id;
    resulting_revision = host->document_revision + UINT64_C(1);
    if (!current || resulting_revision == 0) {
        completion.outcome = BOXRIG_UI_EDITABLE_REPLACEMENT_STALE;
        host->last_outcome = "stale";
    } else if (host->reject_next_replacement) {
        completion.outcome = BOXRIG_UI_EDITABLE_REPLACEMENT_REJECTED;
        host->last_outcome = "rejected";
    } else if (!replaceable) {
        completion.outcome = BOXRIG_UI_EDITABLE_REPLACEMENT_STALE;
        host->last_outcome = "stale";
    } else {
        completion.outcome = BOXRIG_UI_EDITABLE_REPLACEMENT_ACCEPTED;
        completion.new_document_revision = resulting_revision;
        host->last_outcome = "accepted";
    }
    completion_status = boxrig_ui_editable_document_complete_replacement(
        host->editable,
        &completion);
    if (completion_status != BOXRIG_UI_OK) {
        free(updated);
        return -1;
    }
    if (completion.outcome == BOXRIG_UI_EDITABLE_REPLACEMENT_ACCEPTED) {
        memcpy(host->document, updated, updated_length);
        host->document_length = updated_length;
        host->document_revision = resulting_revision;
        if (request.replacement_length != 0) {
            host->selection_offset = relative_offset;
            host->selection_length = request.replacement_length;
        } else if (host->document_length != 0) {
            host->selection_offset = 0;
            host->selection_length = 1;
        } else {
            free(updated);
            return -1;
        }
        if (publish_document(host) != BOXRIG_UI_OK) {
            free(updated);
            return -1;
        }
    }
    free(updated);
    host->reject_next_replacement = false;
    host->report_dirty = true;
    return 1;
}

static int drain_terminal_output(reconversion_host *host)
{
    const uint8_t *output;
    size_t output_length;

    while ((output_length =
                boxrig_terminal_output_peek(host->terminal, &output)) != 0) {
        if (output_length >
            sizeof(host->terminal_output) - host->terminal_output_length) {
            return -1;
        }
        memcpy(
            host->terminal_output + host->terminal_output_length,
            output,
            output_length);
        host->terminal_output_length += output_length;
        if (boxrig_terminal_output_consume(
                host->terminal,
                output_length) != BOXRIG_STATUS_OK) {
            return -1;
        }
        host->report_dirty = true;
    }
    return 0;
}

static int synchronize_observable_state(reconversion_host *host)
{
    boxrig_ui_editable_reconversion_state state;
    bool capable = false;

    state = boxrig_ui_editable_document_reconversion_state(host->editable);
    if (host->component != NULL) {
        boxrig_ui_view_action_capabilities capabilities =
            boxrig_win32_terminal_component_view_action_capabilities(
                host->component);

        capable = capabilities.reconversion;
    }
    if (!host->observed_reconversion_state_valid ||
        host->observed_reconversion_state != state ||
        host->reconversion_capable != capable) {
        host->observed_reconversion_state = state;
        host->observed_reconversion_state_valid = true;
        host->reconversion_capable = capable;
        host->report_dirty = true;
    }
    return 0;
}

static int pump_host(reconversion_host *host)
{
    boxrig_ui_component_frame_result frame;

    if (host->component != NULL) {
        if (boxrig_ui_model_synchronize(host->model) != BOXRIG_UI_OK ||
            boxrig_win32_terminal_component_frame(
                host->component,
                &frame) != BOXRIG_UI_OK) {
            return -1;
        }
    }
    if (process_replacement(host) == -1 ||
        drain_terminal_output(host) == -1 ||
        synchronize_observable_state(host) == -1) {
        return -1;
    }
    if (host->report_dirty && write_report(host) == -1) {
        return -1;
    }
    return 0;
}

static int resize_component(reconversion_host *host)
{
    boxrig_ui_component_allocation allocation;
    boxrig_ui_layout_result layout;
    boxrig_ui_layout_action action;
    RECT client;

    if (host->component == NULL || !GetClientRect(host->window, &client) ||
        client.right <= client.left ||
        client.bottom <= HOST_HEADER_HEIGHT) {
        return 0;
    }
    memset(&allocation, 0, sizeof(allocation));
    allocation.x = client.left;
    allocation.y = HOST_HEADER_HEIGHT;
    allocation.width = (unsigned int)(client.right - client.left);
    allocation.height =
        (unsigned int)(client.bottom - HOST_HEADER_HEIGHT);
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    if (boxrig_win32_terminal_component_allocate(
            host->component,
            &allocation,
            &layout) != BOXRIG_UI_OK) {
        return -1;
    }
    if (boxrig_win32_terminal_component_take_layout_action(
            host->component, &action)) {
        bool succeeded =
            action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE &&
            boxrig_terminal_resize(
                host->terminal,
                action.value.grid_resize.rows,
                action.value.grid_resize.columns) == BOXRIG_STATUS_OK;

        if (action.kind != BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE ||
            boxrig_win32_terminal_component_complete_grid_resize(
                host->component, action.revision, succeeded) != BOXRIG_UI_OK ||
            !succeeded) {
            return -1;
        }
    }
    return 0;
}

static int reset_document(reconversion_host *host)
{
    uint64_t revision = host->document_revision + UINT64_C(1);

    if (revision == 0) {
        return -1;
    }
    memcpy(host->document, initial_document, sizeof(initial_document) - 1);
    host->document_length = sizeof(initial_document) - 1;
    host->document_revision = revision;
    host->selection_offset = 7;
    host->selection_length = 9;
    host->last_action = "reset";
    host->report_dirty = true;
    return publish_document(host) == BOXRIG_UI_OK ? 0 : -1;
}

static void handle_host_action(reconversion_host *host, boxrig_key key)
{
    boxrig_ui_view_action action;
    boxrig_ui_status status;

    switch (key) {
    case BOXRIG_KEY_F2:
        memset(&action, 0, sizeof(action));
        action.kind = BOXRIG_UI_VIEW_ACTION_RECONVERT;
        status = boxrig_win32_terminal_component_apply_view_action(
            host->component,
            &action);
        host->last_action = ui_status_name(status);
        host->report_dirty = true;
        break;
    case BOXRIG_KEY_F3:
        host->reject_next_replacement = true;
        host->last_action = "rejectNext";
        host->report_dirty = true;
        break;
    case BOXRIG_KEY_F4:
        if (host->document_revision == UINT64_MAX) {
            host->failed = true;
            break;
        }
        host->document_revision++;
        host->last_outcome = "stale";
        host->last_action = "revisionChanged";
        if (publish_document(host) != BOXRIG_UI_OK) {
            host->failed = true;
        }
        host->report_dirty = true;
        break;
    case BOXRIG_KEY_F5:
        if (reset_document(host) == -1) {
            host->failed = true;
        }
        break;
    default:
        break;
    }
    if (host->window != NULL) {
        (void)InvalidateRect(host->window, NULL, FALSE);
    }
}

static boxrig_input_key_route route_host_key(
    const boxrig_key_event *event,
    void *context)
{
    reconversion_host *host = context;

    if (event == NULL || host == NULL) {
        return BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    if (event->key < BOXRIG_KEY_F2 || event->key > BOXRIG_KEY_F5) {
        return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    }
    if (event->action == BOXRIG_KEY_PRESS &&
        !PostMessageW(
            host->window,
            HOST_ACTION_MESSAGE,
            (WPARAM)event->key,
            0)) {
        return BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
}

static void paint_header(reconversion_host *host)
{
    PAINTSTRUCT paint;
    HDC context;
    RECT client;
    RECT header;
    WCHAR document[HOST_DOCUMENT_CAPACITY];
    WCHAR selected[BOXRIG_UI_EDITABLE_SELECTION_CAPACITY_MAX];
    WCHAR status[256];
    int document_length;
    int selected_length;
    int previous_mode;

    context = BeginPaint(host->window, &paint);
    if (context == NULL) {
        host->failed = true;
        return;
    }
    if (!GetClientRect(host->window, &client)) {
        EndPaint(host->window, &paint);
        host->failed = true;
        return;
    }
    header = client;
    header.bottom = HOST_HEADER_HEIGHT;
    (void)FillRect(context, &header, (HBRUSH)(COLOR_WINDOW + 1));
    document_length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        host->document,
        (int)host->document_length,
        document,
        (int)(sizeof(document) / sizeof(document[0]) - 1));
    selected_length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        host->document + host->selection_offset,
        (int)host->selection_length,
        selected,
        (int)(sizeof(selected) / sizeof(selected[0]) - 1));
    if (document_length <= 0 || selected_length <= 0) {
        EndPaint(host->window, &paint);
        host->failed = true;
        return;
    }
    document[document_length] = L'\0';
    selected[selected_length] = L'\0';
    (void)swprintf(
        status,
        sizeof(status) / sizeof(status[0]),
        L"Identity: %llu   Revision: %llu   Replacements: %llu",
        (unsigned long long)host->document_identity,
        (unsigned long long)host->document_revision,
        (unsigned long long)host->replacement_count);
    previous_mode = SetBkMode(context, TRANSPARENT);
    (void)TextOutW(
        context,
        12,
        10,
        L"Application-owned editable document",
        35);
    (void)TextOutW(context, 12, 34, L"Context:", 8);
    (void)TextOutW(context, 82, 34, document, document_length);
    (void)TextOutW(context, 12, 58, L"Selected:", 9);
    (void)SetTextColor(context, RGB(0, 80, 180));
    (void)TextOutW(context, 82, 58, selected, selected_length);
    (void)SetTextColor(context, GetSysColor(COLOR_WINDOWTEXT));
    (void)TextOutW(context, 12, 82, status, (int)wcslen(status));
    (void)TextOutW(
        context,
        12,
        108,
        L"F2: reconvert   F3: reject next   F4: change revision   F5: reset",
        65);
    (void)TextOutW(
        context,
        12,
        132,
        L"IME commits during reconversion replace only the selected range.",
        64);
    (void)SetBkMode(context, previous_mode);
    EndPaint(host->window, &paint);
}

static LRESULT CALLBACK host_window_procedure(
    HWND window,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter)
{
    reconversion_host *host = (reconversion_host *)GetWindowLongPtrW(
        window,
        GWLP_USERDATA);

    if (message == WM_NCCREATE) {
        CREATESTRUCTW *create = (CREATESTRUCTW *)long_parameter;

        host = create->lpCreateParams;
        host->window = window;
        (void)SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)host);
    }
    if (host == NULL) {
        return DefWindowProcW(
            window,
            message,
            word_parameter,
            long_parameter);
    }
    if (message == revision_message) {
        handle_host_action(host, BOXRIG_KEY_F4);
        return 0;
    }
    switch (message) {
    case WM_SETFOCUS:
        if (host->component != NULL &&
            boxrig_win32_terminal_component_request_focus(
                host->component) != BOXRIG_UI_OK) {
            host->failed = true;
        }
        return 0;
    case WM_SIZE:
        if (host->component != NULL && resize_component(host) == -1) {
            host->failed = true;
        }
        return 0;
    case WM_PAINT:
        paint_header(host);
        return 0;
    case WM_TIMER:
        if (word_parameter == HOST_TIMER_ID && pump_host(host) == -1) {
            host->failed = true;
            (void)DestroyWindow(window);
        }
        return 0;
    case HOST_ACTION_MESSAGE:
        handle_host_action(host, (boxrig_key)word_parameter);
        return 0;
    case WM_CLOSE:
        (void)DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        (void)KillTimer(window, HOST_TIMER_ID);
        host->window = NULL;
        PostQuitMessage(host->failed ? 1 : 0);
        return 0;
    default:
        return DefWindowProcW(
            window,
            message,
            word_parameter,
            long_parameter);
    }
}

static int register_host_class(HINSTANCE instance)
{
    WNDCLASSEXW window_class;

    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = host_window_procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    window_class.lpszClassName = HOST_CLASS_NAME;
    if (window_class.hCursor == NULL) {
        return -1;
    }
    if (RegisterClassExW(&window_class) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        return 0;
    }
    return -1;
}

static int initialize_live_host(reconversion_host *host)
{
    static const uint8_t terminal_text[] =
        "\x1b[2J\x1b[H\x1b[?25h"
        "Normal IME input is delivered to this terminal area.\r\n"
        "Reconversion output is never forwarded here.\r\n";
    boxrig_terminal_config terminal_config =
        boxrig_terminal_config_default();
    boxrig_win32_terminal_component_config component_config =
        boxrig_win32_terminal_component_config_default();
    size_t consumed = 0;

    terminal_config.rows = 12;
    terminal_config.columns = 72;
    if (boxrig_terminal_create(
            &terminal_config,
            &host->terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(host->terminal, &host->model) !=
            BOXRIG_UI_OK ||
        register_host_class(host->instance) == -1) {
        return -1;
    }
    host->window = CreateWindowExW(
        0,
        HOST_CLASS_NAME,
        L"boxrig editable document reconversion host",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        760,
        520,
        NULL,
        NULL,
        host->instance,
        host);
    if (host->window == NULL) {
        return -1;
    }
    component_config.scrollbar_presentation =
        BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    if (boxrig_win32_terminal_component_create_child(
            host->instance,
            host->window,
            host->model,
            &component_config,
            &host->component) != BOXRIG_UI_OK) {
        return -1;
    }
    host->editable = boxrig_ui_view_editable_document(
        boxrig_win32_terminal_component_view(host->component));
    if (host->editable == NULL || publish_document(host) != BOXRIG_UI_OK ||
        boxrig_win32_terminal_component_set_editable_document_provider(
            host->component,
            true) != BOXRIG_UI_OK ||
        boxrig_win32_terminal_component_set_key_router(
            host->component,
            route_host_key,
            host) != BOXRIG_UI_OK ||
        boxrig_terminal_feed(
            host->terminal,
            terminal_text,
            sizeof(terminal_text) - 1,
            &consumed) != BOXRIG_STATUS_OK ||
        consumed != sizeof(terminal_text) - 1 ||
        resize_component(host) == -1 ||
        boxrig_win32_terminal_component_set_visible(
            host->component,
            true) != BOXRIG_UI_OK) {
        return -1;
    }
    ShowWindow(host->window, SW_SHOW);
    if (!UpdateWindow(host->window) ||
        boxrig_win32_terminal_component_request_focus(host->component) !=
            BOXRIG_UI_OK ||
        SetTimer(
            host->window,
            HOST_TIMER_ID,
            HOST_TIMER_INTERVAL,
            NULL) == 0) {
        return -1;
    }
    host->ready = true;
    host->report_dirty = true;
    return pump_host(host);
}

static void destroy_host(reconversion_host *host)
{
    if (host == NULL) {
        return;
    }
    if (host->component != NULL) {
        boxrig_win32_terminal_component_destroy(host->component);
    }
    boxrig_ui_model_destroy(host->model);
    boxrig_terminal_destroy(host->terminal);
    host->component = NULL;
    host->model = NULL;
    host->terminal = NULL;
    host->editable = NULL;
}

/* Keep an old reader open through the next publication and compare both
 * results. The self-test exports one final file only after all readers close. */
static int verify_report_publication(reconversion_host *host)
{
    wchar_t exported_path[MAX_PATH];
    wchar_t directory[MAX_PATH];
    wchar_t first[MAX_PATH] = L"", second[MAX_PATH] = L"";
    HANDLE reader = INVALID_HANDLE_VALUE;
    FILE *current = NULL;
    int result = -1;
    bool temporary_owned = false;
    wcscpy(exported_path, host->report_path);
    DWORD directory_length = GetTempPathW(MAX_PATH, directory);
    if (directory_length == 0 || directory_length >= MAX_PATH ||
        !GetTempFileNameW(directory, L"brc", 0, host->report_path)) goto cleanup;
    temporary_owned = true;
    if (!DeleteFileW(host->report_path) || write_report(host) == -1 ||
        !report_snapshot_path(host, host->report_revision, first)) goto cleanup;
    reader = CreateFileW(first, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (reader == INVALID_HANDLE_VALUE) goto cleanup;
    host->last_action = "retainedReader";
    if (write_report(host) == -1 ||
        !report_snapshot_path(host, host->report_revision, second)) goto cleanup;
    char previous_bytes[2048] = {0}, current_bytes[2048] = {0};
    DWORD previous_length = 0;
    if (!ReadFile(reader, previous_bytes, sizeof(previous_bytes) - 1u,
            &previous_length, NULL)) goto cleanup;
    current = _wfopen(second, L"rb");
    if (current == NULL) goto cleanup;
    (void)fread(current_bytes, 1u, sizeof(current_bytes) - 1u, current);
    if (ferror(current) ||
        strstr(previous_bytes, "\"lastAction\": \"ready\"") == NULL ||
        strstr(current_bytes, "\"lastAction\": \"retainedReader\"") == NULL)
        goto cleanup;
    if (fclose(current) != 0) { current = NULL; goto cleanup; }
    current = NULL;
    if (!CloseHandle(reader)) goto cleanup;
    reader = INVALID_HANDLE_VALUE;
    if (!CopyFileW(second, exported_path, FALSE)) goto cleanup;
    puts("PASS reconversion observations advance while the previous report remains open");
    result = 0;
cleanup:
    if (current != NULL) fclose(current);
    if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
    if (first[0] != L'\0') DeleteFileW(first);
    if (second[0] != L'\0') DeleteFileW(second);
    if (temporary_owned) {
        wchar_t temporary[MAX_PATH];
        int length = _snwprintf(temporary, MAX_PATH, L"%ls.tmp", host->report_path);
        if (length >= 0 && length < MAX_PATH) DeleteFileW(temporary);
        DeleteFileW(host->report_path);
    }
    wcscpy(host->report_path, exported_path);
    return result;
}

static int run_self_test(reconversion_host *host)
{
    boxrig_terminal_config terminal_config =
        boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_view *view = NULL;
    int result = -1;

    terminal_config.rows = 2;
    terminal_config.columns = 8;
    if (boxrig_terminal_create(
            &terminal_config,
            &host->terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(host->terminal, &host->model) !=
            BOXRIG_UI_OK ||
        boxrig_ui_view_create(
            host->model,
            &view_config,
            &view) != BOXRIG_UI_OK) {
        goto cleanup;
    }
    host->editable = boxrig_ui_view_editable_document(view);
    if (host->editable == NULL || publish_document(host) != BOXRIG_UI_OK ||
        boxrig_ui_editable_document_begin_reconversion(host->editable) !=
            BOXRIG_UI_OK ||
        boxrig_ui_editable_document_commit_reconversion(
            self_test_replacement,
            sizeof(self_test_replacement) - 1,
            host->editable) != BOXRIG_STATUS_OK ||
        process_replacement(host) != 1 ||
        drain_terminal_output(host) == -1 ||
        synchronize_observable_state(host) == -1) {
        goto cleanup;
    }
    host->ready = true;
    host->report_dirty = true;
    if (verify_report_publication(host) == -1 || host->document_revision != 2 ||
        host->selection_offset != 7 || host->selection_length != 9 ||
        host->replacement_count != 1 ||
        host->terminal_output_length != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    boxrig_ui_view_destroy(view);
    host->editable = NULL;
    boxrig_ui_model_destroy(host->model);
    boxrig_terminal_destroy(host->terminal);
    host->model = NULL;
    host->terminal = NULL;
    return result;
}

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE previous_instance,
    PWSTR command_line,
    int show_command)
{
    reconversion_host *host;
    LPWSTR *arguments;
    MSG message;
    BOOL message_result;
    int argument_count = 0;
    int result = 1;
    bool self_test;

    (void)previous_instance;
    (void)command_line;
    (void)show_command;
    arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments == NULL) {
        return 1;
    }
    self_test = argument_count == 3 &&
                wcscmp(arguments[1], L"--self-test") == 0;
    if ((!self_test && argument_count != 2) ||
        (self_test && arguments[2][0] == L'\0') ||
        (!self_test && arguments[1][0] == L'\0')) {
        LocalFree(arguments);
        return 2;
    }
    host = calloc(1, sizeof(*host));
    if (host == NULL) {
        LocalFree(arguments);
        return 1;
    }
    host->instance = instance;
    if (wcslen(arguments[self_test ? 2 : 1]) >=
        sizeof(host->report_path) / sizeof(host->report_path[0])) {
        goto cleanup;
    }
    (void)wcscpy(host->report_path, arguments[self_test ? 2 : 1]);
    initialize_document(host);
    if (self_test) {
        result = run_self_test(host) == 0 ? 0 : 1;
        goto cleanup;
    }
    revision_message = RegisterWindowMessageW(
        BOXRIG_P2_RECONVERSION_REVISION_MESSAGE);
    if (revision_message == 0) {
        goto cleanup;
    }
    if (initialize_live_host(host) == -1) {
        goto cleanup;
    }
    while ((message_result = GetMessageW(&message, NULL, 0, 0)) > 0) {
        boxrig_ui_component_event_result event_result =
            boxrig_win32_terminal_component_pretranslate_message(
                host->component,
                &message);

        if (event_result == BOXRIG_UI_COMPONENT_EVENT_FAILURE) {
            host->failed = true;
            break;
        }
        if (event_result == BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
            continue;
        }
        (void)TranslateMessage(&message);
        (void)DispatchMessageW(&message);
    }
    result = host->failed || message_result == -1 || message.wParam != 0
                 ? 1
                 : 0;

cleanup:
    if (host->window != NULL) {
        (void)DestroyWindow(host->window);
    }
    destroy_host(host);
    free(host);
    LocalFree(arguments);
    return result;
}
