/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "../editable_fixture.h"
#include "boxrig/terminal.h"
#include "boxrig/ui/editable.h"
#include "../composition_fixture.h"
#include "boxrig/win32.h"

#include <imm.h>
#include <windows.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

typedef struct routed_key_trace {
    boxrig_key_action actions[8];
    boxrig_key_event events[8];
    size_t count;
} routed_key_trace;

typedef struct routed_mouse_trace {
    boxrig_mouse_event events[8];
    boxrig_ui_hit_test_result hits[8];
    size_t count;
} routed_mouse_trace;

typedef struct mock_imm_snapshot {
    const WCHAR *result_text;
    size_t result_count;
    const WCHAR *preedit_text;
    size_t preedit_count;
    const BYTE *attributes;
    size_t attribute_count;
    const DWORD *clauses;
    size_t clause_count;
    LONG caret;
} mock_imm_snapshot;

static bool mock_imm_active;
static mock_imm_snapshot mock_imm;
static COMPOSITIONFORM mock_composition_form;
static CANDIDATEFORM mock_candidate_form;
static size_t mock_composition_form_calls;
static size_t mock_candidate_form_calls;
static size_t mock_notify_ime_calls;
static DWORD mock_reconversion_calls[4];
static DWORD mock_reconversion_sizes[4];
static size_t mock_reconversion_call_count;
static bool mock_reconversion_supported;
static bool mock_reconversion_query_rejected;
static bool mock_reconversion_set_rejected;
static bool mock_reconversion_adjust_query;
static UINT mock_imm_virtual_key;
static bool mock_composition_form_rejected;
static bool mock_candidate_form_rejected;
static bool clipboard_open_rejected;
static bool clipboard_set_rejected;
static bool clipboard_global_alloc_rejected;
static bool clipboard_global_unlock_rejected;
static bool clipboard_global_size_overridden;
static SIZE_T clipboard_global_size_override;
static size_t clipboard_open_calls;
static size_t clipboard_close_calls;
static size_t clipboard_empty_calls;
static size_t clipboard_set_calls;
static size_t clipboard_global_free_calls;
static bool key_translation_overridden;
static UINT overridden_virtual_key;
static WCHAR overridden_unshifted_character;
static WCHAR overridden_shifted_character;

static FARPROC system_procedure(const WCHAR *module_name, const char *name)
{
    HMODULE module = GetModuleHandleW(module_name);

    return module == NULL ? NULL : GetProcAddress(module, name);
}

int WINAPI boxrig_test_ToUnicodeEx(
    UINT virtual_key,
    UINT scan_code,
    const BYTE *keyboard_state,
    LPWSTR buffer,
    int buffer_length,
    UINT flags,
    HKL layout)
{
    union {
        FARPROC generic;
        int(WINAPI *typed)(
            UINT,
            UINT,
            const BYTE *,
            LPWSTR,
            int,
            UINT,
            HKL);
    } procedure;

    if (key_translation_overridden &&
        virtual_key == overridden_virtual_key) {
        if (keyboard_state == NULL || buffer == NULL || buffer_length < 1) {
            return 0;
        }
        buffer[0] = (keyboard_state[VK_SHIFT] & 0x80) != 0
                        ? overridden_shifted_character
                        : overridden_unshifted_character;
        return 1;
    }
    procedure.generic = system_procedure(L"user32.dll", "ToUnicodeEx");
    return procedure.generic == NULL
               ? 0
               : procedure.typed(
                     virtual_key,
                     scan_code,
                     keyboard_state,
                     buffer,
                     buffer_length,
                     flags,
                     layout);
}

WINBOOL WINAPI boxrig_test_OpenClipboard(HWND window)
{
    union {
        FARPROC generic;
        WINBOOL(WINAPI *typed)(HWND);
    } procedure;

    clipboard_open_calls++;
    if (clipboard_open_rejected) {
        return FALSE;
    }
    procedure.generic = system_procedure(L"user32.dll", "OpenClipboard");
    return procedure.generic == NULL ? FALSE : procedure.typed(window);
}

WINBOOL WINAPI boxrig_test_CloseClipboard(void)
{
    union {
        FARPROC generic;
        WINBOOL(WINAPI *typed)(void);
    } procedure;

    clipboard_close_calls++;
    procedure.generic = system_procedure(L"user32.dll", "CloseClipboard");
    return procedure.generic == NULL ? FALSE : procedure.typed();
}

WINBOOL WINAPI boxrig_test_EmptyClipboard(void)
{
    union {
        FARPROC generic;
        WINBOOL(WINAPI *typed)(void);
    } procedure;

    clipboard_empty_calls++;
    procedure.generic = system_procedure(L"user32.dll", "EmptyClipboard");
    return procedure.generic == NULL ? FALSE : procedure.typed();
}

HANDLE WINAPI boxrig_test_SetClipboardData(UINT format, HANDLE memory)
{
    union {
        FARPROC generic;
        HANDLE(WINAPI *typed)(UINT, HANDLE);
    } procedure;

    clipboard_set_calls++;
    if (clipboard_set_rejected) {
        return NULL;
    }
    procedure.generic =
        system_procedure(L"user32.dll", "SetClipboardData");
    return procedure.generic == NULL
               ? NULL
               : procedure.typed(format, memory);
}

HGLOBAL WINAPI boxrig_test_GlobalAlloc(UINT flags, SIZE_T byte_count)
{
    union {
        FARPROC generic;
        HGLOBAL(WINAPI *typed)(UINT, SIZE_T);
    } procedure;

    if (clipboard_global_alloc_rejected) {
        return NULL;
    }
    procedure.generic = system_procedure(L"kernel32.dll", "GlobalAlloc");
    return procedure.generic == NULL
               ? NULL
               : procedure.typed(flags, byte_count);
}

SIZE_T WINAPI boxrig_test_GlobalSize(HGLOBAL memory)
{
    union {
        FARPROC generic;
        SIZE_T(WINAPI *typed)(HGLOBAL);
    } procedure;

    if (clipboard_global_size_overridden) {
        return clipboard_global_size_override;
    }
    procedure.generic = system_procedure(L"kernel32.dll", "GlobalSize");
    return procedure.generic == NULL ? 0 : procedure.typed(memory);
}

WINBOOL WINAPI boxrig_test_GlobalUnlock(HGLOBAL memory)
{
    union {
        FARPROC generic;
        WINBOOL(WINAPI *typed)(HGLOBAL);
    } procedure;
    WINBOOL result;

    procedure.generic = system_procedure(L"kernel32.dll", "GlobalUnlock");
    if (procedure.generic == NULL) {
        return FALSE;
    }
    result = procedure.typed(memory);
    if (clipboard_global_unlock_rejected) {
        SetLastError(ERROR_NOT_LOCKED);
        return FALSE;
    }
    return result;
}

HGLOBAL WINAPI boxrig_test_GlobalFree(HGLOBAL memory)
{
    union {
        FARPROC generic;
        HGLOBAL(WINAPI *typed)(HGLOBAL);
    } procedure;

    clipboard_global_free_calls++;
    procedure.generic = system_procedure(L"kernel32.dll", "GlobalFree");
    return procedure.generic == NULL ? memory : procedure.typed(memory);
}

static void reset_clipboard_trace(void)
{
    clipboard_open_calls = 0;
    clipboard_close_calls = 0;
    clipboard_empty_calls = 0;
    clipboard_set_calls = 0;
    clipboard_global_free_calls = 0;
}

static bool clipboard_equals(HWND window, const WCHAR *expected)
{
    HGLOBAL handle;
    const WCHAR *text;
    bool equals = false;

    if (!OpenClipboard(window)) {
        return false;
    }
    handle = (HGLOBAL)GetClipboardData(CF_UNICODETEXT);
    if (handle != NULL) {
        text = GlobalLock(handle);
        if (text != NULL) {
            equals = wcscmp(text, expected) == 0;
            (void)GlobalUnlock(handle);
        }
    }
    (void)CloseClipboard();
    return equals;
}

HIMC WINAPI boxrig_test_ImmGetContext(HWND window)
{
    (void)window;
    if (mock_imm_active) {
        return (HIMC)(uintptr_t)1;
    }
    return NULL;
}

WINBOOL WINAPI boxrig_test_ImmReleaseContext(HWND window, HIMC input_context)
{
    (void)window;
    (void)input_context;
    return TRUE;
}

DWORD WINAPI boxrig_test_ImmGetProperty(HKL layout, DWORD index)
{
    (void)layout;
    if (index == IGP_SETCOMPSTR && mock_reconversion_supported) {
        return SCS_CAP_SETRECONVERTSTRING;
    }
    return 0;
}

UINT WINAPI boxrig_test_ImmGetVirtualKey(HWND window)
{
    (void)window;
    return mock_imm_virtual_key;
}

WINBOOL WINAPI boxrig_test_ImmSetCompositionStringW(
    HIMC input_context,
    DWORD index,
    LPVOID composition,
    DWORD composition_size,
    LPVOID reading,
    DWORD reading_size)
{
    RECONVERTSTRING *request = composition;

    if (!mock_imm_active || input_context != (HIMC)(uintptr_t)1 ||
        composition == NULL || composition_size < sizeof(*request) ||
        reading != NULL || reading_size != 0 ||
        mock_reconversion_call_count >=
            sizeof(mock_reconversion_calls) /
                sizeof(mock_reconversion_calls[0])) {
        return FALSE;
    }
    mock_reconversion_calls[mock_reconversion_call_count] = index;
    mock_reconversion_sizes[mock_reconversion_call_count] =
        composition_size;
    mock_reconversion_call_count++;
    if (index == SCS_QUERYRECONVERTSTRING) {
        if (mock_reconversion_query_rejected) {
            return FALSE;
        }
        if (mock_reconversion_adjust_query) {
            request->dwCompStrOffset = 4;
            request->dwCompStrLen = 5;
            request->dwTargetStrOffset = 6;
            request->dwTargetStrLen = 2;
        }
        return TRUE;
    }
    if (index == SCS_SETRECONVERTSTRING) {
        return mock_reconversion_set_rejected ? FALSE : TRUE;
    }
    return FALSE;
}

LONG WINAPI boxrig_test_ImmGetCompositionStringW(
    HIMC input_context,
    DWORD index,
    LPVOID buffer,
    DWORD buffer_length)
{
    const void *source = NULL;
    size_t byte_count = 0;

    if (!mock_imm_active || input_context != (HIMC)(uintptr_t)1) {
        return IMM_ERROR_NODATA;
    }
    if (index == GCS_CURSORPOS) {
        return mock_imm.caret;
    }
    if (index == GCS_RESULTSTR) {
        source = mock_imm.result_text;
        byte_count = mock_imm.result_count * sizeof(WCHAR);
    } else if (index == GCS_COMPSTR) {
        source = mock_imm.preedit_text;
        byte_count = mock_imm.preedit_count * sizeof(WCHAR);
    } else if (index == GCS_COMPATTR) {
        source = mock_imm.attributes;
        byte_count = mock_imm.attribute_count * sizeof(BYTE);
    } else if (index == GCS_COMPCLAUSE) {
        source = mock_imm.clauses;
        byte_count = mock_imm.clause_count * sizeof(DWORD);
    } else {
        return IMM_ERROR_NODATA;
    }
    if (byte_count > LONG_MAX) {
        return IMM_ERROR_GENERAL;
    }
    if (buffer == NULL) {
        return (LONG)byte_count;
    }
    if (source == NULL || byte_count > buffer_length) {
        return IMM_ERROR_GENERAL;
    }
    memcpy(buffer, source, byte_count);
    return (LONG)byte_count;
}

WINBOOL WINAPI boxrig_test_ImmSetCompositionWindow(
    HIMC input_context,
    LPCOMPOSITIONFORM form)
{
    if (mock_imm_active && input_context == (HIMC)(uintptr_t)1) {
        mock_composition_form = *form;
        mock_composition_form_calls++;
        return mock_composition_form_rejected ? FALSE : TRUE;
    }
    return TRUE;
}

WINBOOL WINAPI boxrig_test_ImmSetCandidateWindow(
    HIMC input_context,
    LPCANDIDATEFORM form)
{
    if (mock_imm_active && input_context == (HIMC)(uintptr_t)1) {
        mock_candidate_form = *form;
        mock_candidate_form_calls++;
        return mock_candidate_form_rejected ? FALSE : TRUE;
    }
    return TRUE;
}

WINBOOL WINAPI boxrig_test_ImmNotifyIME(
    HIMC input_context,
    DWORD action,
    DWORD index,
    DWORD value)
{
    (void)input_context;
    (void)action;
    (void)index;
    (void)value;
    mock_notify_ime_calls++;
    return TRUE;
}

static boxrig_input_key_route route_control_shortcut(
    const boxrig_key_event *event,
    void *opaque_context)
{
    routed_key_trace *trace = opaque_context;

    if (!((event->key == BOXRIG_KEY_NONE && event->codepoint == 'a') ||
          event->key == BOXRIG_KEY_KP_ADD) ||
        (event->modifiers & BOXRIG_MODIFIER_CONTROL) == 0) {
        return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    }
    if (trace->count >= sizeof(trace->actions) / sizeof(trace->actions[0])) {
        return BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    trace->events[trace->count] = *event;
    trace->actions[trace->count++] = event->action;
    return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
}

static boxrig_input_key_route capture_key_identity(
    const boxrig_key_event *event,
    void *opaque_context)
{
    routed_key_trace *trace = opaque_context;

    if (trace->count >= sizeof(trace->events) / sizeof(trace->events[0])) {
        return BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    trace->events[trace->count++] = *event;
    return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
}

static boxrig_input_mouse_route route_left_drag(
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    void *opaque_context)
{
    routed_mouse_trace *trace = opaque_context;

    if (hit == NULL ||
        trace->count >= sizeof(trace->events) / sizeof(trace->events[0])) {
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    if (event->action == BOXRIG_MOUSE_PRESS &&
        event->button == BOXRIG_MOUSE_BUTTON_LEFT) {
        trace->events[trace->count] = *event;
        trace->hits[trace->count++] = *hit;
        return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
    }
    if (trace->count != 0 && trace->count < 3) {
        trace->events[trace->count] = *event;
        trace->hits[trace->count++] = *hit;
    }
    return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
}

static LPARAM key_parameter(UINT virtual_key, bool repeat, bool released)
{
    UINT scan_code = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
    LPARAM parameter = 1 | ((LPARAM)scan_code << 16);

    if (repeat || released) {
        parameter |= (LPARAM)1 << 30;
    }
    if (released) {
        parameter |= (LPARAM)1 << 31;
    }
    return parameter;
}

static bool test_application_key_routing_captures_the_key_series(void)
{
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    routed_key_trace trace;
    LRESULT message_result = 0;
    const uint8_t mode[] = "\x1b[=27u";
    const uint8_t expected[] = "\x1b[98;;98u";
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;

    memset(&trace, 0, sizeof(trace));
    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig routed key test",
        WS_OVERLAPPED,
        0,
        0,
        40,
        60,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 3;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    CHECK(boxrig_win32_input_adapter_set_key_router(
              adapter,
              route_control_shortcut,
              &trace) == 0);
    CHECK(boxrig_terminal_feed(
              terminal,
              mode,
              sizeof(mode) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYDOWN,
              VK_CONTROL,
              key_parameter(VK_CONTROL, false, false),
              &message_result) == 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYDOWN,
              'A',
              key_parameter('A', false, false),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_CHAR,
              L'a',
              key_parameter('A', false, false),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYDOWN,
              'A',
              key_parameter('A', true, false),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_CHAR,
              L'a',
              key_parameter('A', true, false),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYUP,
              'A',
              key_parameter('A', false, true),
              &message_result) == 1);
    CHECK(trace.count == 3);
    CHECK(trace.actions[0] == BOXRIG_KEY_PRESS);
    CHECK(trace.actions[1] == BOXRIG_KEY_REPEAT);
    CHECK(trace.actions[2] == BOXRIG_KEY_RELEASE);
    CHECK(trace.events[0].codepoint == 'a');
    CHECK(trace.events[0].shifted_codepoint == 'A');
    CHECK(trace.events[0].base_layout_codepoint == 'a');
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYDOWN,
              VK_ADD,
              key_parameter(VK_ADD, false, false),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_CHAR,
              L'+',
              key_parameter(VK_ADD, false, false),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYUP,
              VK_ADD,
              key_parameter(VK_ADD, false, true),
              &message_result) == 1);
    CHECK(trace.count == 5);
    CHECK(trace.actions[3] == BOXRIG_KEY_PRESS);
    CHECK(trace.actions[4] == BOXRIG_KEY_RELEASE);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYUP,
              VK_CONTROL,
              key_parameter(VK_CONTROL, false, true),
              &message_result) == 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYDOWN,
              'B',
              key_parameter('B', false, false),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_CHAR,
              L'b',
              key_parameter('B', false, false),
              &message_result) == 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);
    CHECK(boxrig_win32_input_adapter_set_cell_size(adapter, 5, 10) == 0);
    CHECK(boxrig_win32_input_adapter_set_cell_size(adapter, 5, 0) == -1);

    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_key_identity_preserves_pc101_base_layout(void)
{
    static const struct {
        UINT virtual_key;
        uint32_t expected;
    } cases[] = {
        {'A', 'a'},
        {'2', '2'},
        {VK_SPACE, ' '},
        {VK_OEM_1, ';'},
        {VK_OEM_PLUS, '='},
        {VK_OEM_COMMA, ','},
        {VK_OEM_MINUS, '-'},
        {VK_OEM_PERIOD, '.'},
        {VK_OEM_2, '/'},
        {VK_OEM_3, '`'},
        {VK_OEM_4, '['},
        {VK_OEM_5, '\\'},
        {VK_OEM_6, ']'},
        {VK_OEM_7, '\''},
        {VK_OEM_102, 0},
    };
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    routed_key_trace trace;
    LRESULT message_result = 0;
    size_t index;

    memset(&trace, 0, sizeof(trace));
    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig key identity test",
        WS_OVERLAPPED,
        0,
        0,
        40,
        60,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 3;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    CHECK(boxrig_win32_input_adapter_set_key_router(
              adapter,
              capture_key_identity,
              &trace) == 0);

    key_translation_overridden = true;
    overridden_virtual_key = VK_OEM_2;
    overridden_unshifted_character = 0x044e;
    overridden_shifted_character = 0x042e;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYDOWN,
              VK_OEM_2,
              key_parameter(VK_OEM_2, false, false),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYUP,
              VK_OEM_2,
              key_parameter(VK_OEM_2, false, true),
              &message_result) == 1);
    CHECK(trace.count == 2);
    CHECK(trace.events[0].codepoint == 0x044e);
    CHECK(trace.events[0].shifted_codepoint == 0x042e);
    CHECK(trace.events[0].base_layout_codepoint == '/');
    CHECK(trace.events[1].codepoint == 0x044e);
    CHECK(trace.events[1].shifted_codepoint == 0x042e);
    CHECK(trace.events[1].base_layout_codepoint == '/');
    key_translation_overridden = false;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        memset(&trace, 0, sizeof(trace));
        CHECK(boxrig_win32_input_adapter_process_message(
                  adapter,
                  WM_KEYDOWN,
                  cases[index].virtual_key,
                  key_parameter(cases[index].virtual_key, false, false),
                  &message_result) == 1);
        CHECK(boxrig_win32_input_adapter_process_message(
                  adapter,
                  WM_KEYUP,
                  cases[index].virtual_key,
                  key_parameter(cases[index].virtual_key, false, true),
                  &message_result) == 1);
        CHECK(trace.count == 2);
        CHECK(trace.events[0].base_layout_codepoint ==
              cases[index].expected);
        CHECK(trace.events[1].base_layout_codepoint ==
              cases[index].expected);
    }

    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_legacy_control_input_reaches_terminal(void)
{
    static const UINT lock_keys[] = {VK_NUMLOCK, VK_CAPITAL, VK_SCROLL};
    static const struct {
        UINT virtual_key;
        WCHAR character;
        bool shifted;
    } cases[] = {
        {'2', L'\0', false},
        {'3', 0x1b, false},
        {VK_OEM_2, 0x1f, false},
        {VK_OEM_2, L'?', true},
        {'C', 0x03, false},
        {'Z', 0x1a, false},
    };
    static const uint8_t expected[] = {
        0x00,
        0x1b,
        0x1f,
        0x7f,
        0x03,
        0x1a,
    };
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    LRESULT message_result = 0;
    const uint8_t *output = NULL;
    size_t output_length;
    size_t index;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig legacy control input test",
        WS_OVERLAPPED,
        0,
        0,
        40,
        60,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 3;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    /* Wine can synchronize lock states before delivering the first Ctrl. */
    for (index = 0;
         index < sizeof(lock_keys) / sizeof(lock_keys[0]);
         index++) {
        CHECK(boxrig_win32_input_adapter_process_message(
                  adapter,
                  WM_KEYDOWN,
                  lock_keys[index],
                  key_parameter(lock_keys[index], false, false),
                  &message_result) == 1);
        CHECK(boxrig_win32_input_adapter_process_message(
                  adapter,
                  WM_KEYUP,
                  lock_keys[index],
                  key_parameter(lock_keys[index], false, true),
                  &message_result) == 1);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    }
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYDOWN,
              VK_CONTROL,
              key_parameter(VK_CONTROL, false, false),
              &message_result) == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        if (cases[index].shifted) {
            CHECK(boxrig_win32_input_adapter_process_message(
                      adapter,
                      WM_KEYDOWN,
                      VK_SHIFT,
                      key_parameter(VK_SHIFT, false, false),
                      &message_result) == 1);
        }
        CHECK(boxrig_win32_input_adapter_process_message(
                  adapter,
                  WM_KEYDOWN,
                  cases[index].virtual_key,
                  key_parameter(cases[index].virtual_key, false, false),
                  &message_result) == 1);
        CHECK(boxrig_win32_input_adapter_process_message(
                  adapter,
                  WM_CHAR,
                  cases[index].character,
                  key_parameter(cases[index].virtual_key, false, false),
                  &message_result) == 1);
        CHECK(boxrig_win32_input_adapter_process_message(
                  adapter,
                  WM_KEYUP,
                  cases[index].virtual_key,
                  key_parameter(cases[index].virtual_key, false, true),
                  &message_result) == 1);
        if (cases[index].shifted) {
            CHECK(boxrig_win32_input_adapter_process_message(
                      adapter,
                      WM_KEYUP,
                      VK_SHIFT,
                      key_parameter(VK_SHIFT, false, true),
                      &message_result) == 1);
        }
    }
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYUP,
              VK_CONTROL,
              key_parameter(VK_CONTROL, false, true),
              &message_result) == 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected));
    CHECK(memcmp(output, expected, sizeof(expected)) == 0);

    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_application_mouse_routing_captures_the_drag_series(void)
{
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input allocation;
    boxrig_ui_layout_result layout;
    boxrig_win32_input_adapter *adapter = NULL;
    routed_mouse_trace trace;
    LRESULT message_result = 0;
    const uint8_t modes[] = "\x1b[?1003;1006h";
    const uint8_t *output = NULL;
    size_t consumed = 0;

    memset(&trace, 0, sizeof(trace));
    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig routed mouse test",
        WS_OVERLAPPED,
        0,
        0,
        40,
        60,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 3;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.allocation_width = 40 * BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = 60 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = 10 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = 20 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    CHECK(boxrig_win32_input_adapter_set_mouse_router(
              adapter,
              view,
              route_left_drag,
              &trace) == 0);
    CHECK(boxrig_win32_input_adapter_set_viewport_origin(adapter, 7, 11) == 0);
    CHECK(boxrig_terminal_feed(
              terminal,
              modes,
              sizeof(modes) - 1,
              &consumed) == BOXRIG_STATUS_OK);

    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_LBUTTONDOWN,
              MK_CONTROL | MK_LBUTTON,
              MAKELPARAM(15, 25),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_MOUSEMOVE,
              MK_SHIFT | MK_LBUTTON,
              MAKELPARAM(16, 26),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_LBUTTONUP,
              0,
              MAKELPARAM(16, 26),
              &message_result) == 1);
    CHECK(trace.count == 3);
    CHECK(trace.events[0].action == BOXRIG_MOUSE_PRESS);
    CHECK(trace.events[0].x == 8);
    CHECK(trace.events[0].y == 14);
    CHECK(trace.events[0].row == 0);
    CHECK(trace.events[0].column == 0);
    CHECK(trace.events[0].click_count == 1);
    CHECK(trace.events[0].modifiers == BOXRIG_MODIFIER_CONTROL);
    CHECK(trace.hits[0].inside_grid && trace.hits[0].row == 0 &&
          trace.hits[0].visual_column == 0);
    CHECK(trace.hits[0].x == 8 * BOXRIG_UI_FIXED_ONE &&
          trace.hits[0].y == 14 * BOXRIG_UI_FIXED_ONE);
    CHECK(trace.hits[0].layout_generation != 0 &&
          trace.hits[0].annotation_generation != 0);
    CHECK(trace.events[1].action == BOXRIG_MOUSE_MOTION);
    CHECK(trace.events[1].x == 9);
    CHECK(trace.events[1].y == 15);
    CHECK(trace.events[1].modifiers == BOXRIG_MODIFIER_SHIFT);
    CHECK(trace.events[1].click_count == 0);
    CHECK(trace.events[2].action == BOXRIG_MOUSE_RELEASE);
    CHECK(trace.events[2].modifiers == 0u);
    CHECK(trace.events[2].click_count == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_LBUTTONDBLCLK,
              MK_LBUTTON,
              MAKELPARAM(15, 25),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_LBUTTONUP,
              0,
              MAKELPARAM(15, 25),
              &message_result) == 1);
    CHECK(trace.count == 4);
    CHECK(trace.events[3].action == BOXRIG_MOUSE_PRESS);
    CHECK(trace.events[3].click_count == 2);

    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_RBUTTONDOWN,
              MK_RBUTTON,
              MAKELPARAM(16, 26),
              &message_result) == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) != 0);

    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_key_focus_and_mouse_messages(void)
{
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    LRESULT message_result = 0;
    POINT wheel_point = {12, 16};
    WCHAR canceled_result[] = {0x53d6};
    WCHAR accepted_result[] = {0x65e5};
    const uint8_t modes[] = "\x1b[=27u\x1b[?1004;1003;1006h";
    const uint8_t expected[] =
        "\x1b[97;;97u"
        "\x1b[97;1:2;97u"
        "\x1b[97;1:3u"
        "\x1b[57442;5u"
        "\x1b[57442;1:3u"
        "\x1b[I\x1b[O"
        "\xe6\x97\xa5"
        "\x1b[<0;4;3M"
        "\x1b[<32;4;3M"
        "\x1b[<0;4;3m"
        "\x1b[<64;2;1M";
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig input test",
        WS_OVERLAPPED,
        0,
        0,
        40,
        60,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 3;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    CHECK(boxrig_terminal_feed(
              terminal,
              modes,
              sizeof(modes) - 1,
              &consumed) == BOXRIG_STATUS_OK);

    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYDOWN,
              'A',
              key_parameter('A', false, false),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_CHAR,
              L'a',
              key_parameter('A', false, false),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYDOWN,
              'A',
              key_parameter('A', true, false),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_CHAR,
              L'a',
              key_parameter('A', true, false),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYUP,
              'A',
              key_parameter('A', false, true),
              &message_result) == 1);

    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYDOWN,
              VK_CONTROL,
              key_parameter(VK_CONTROL, false, false),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYUP,
              VK_CONTROL,
              key_parameter(VK_CONTROL, false, true),
              &message_result) == 1);
    mock_imm_active = true;
    memset(&mock_imm, 0, sizeof(mock_imm));
    mock_notify_ime_calls = 0;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_SETCONTEXT,
              TRUE,
              0,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_SETFOCUS,
              0,
              0,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_STARTCOMPOSITION,
              0,
              0,
              &message_result) == 1);
    mock_imm.result_text = canceled_result;
    mock_imm.result_count =
        sizeof(canceled_result) / sizeof(canceled_result[0]);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_SETCONTEXT,
              FALSE,
              0,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KILLFOCUS,
              0,
              0,
              &message_result) == 1);
    CHECK(mock_notify_ime_calls == 0);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_COMPOSITION,
              0,
              GCS_RESULTSTR,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_CHAR,
              0x8a9e,
              0,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_ENDCOMPOSITION,
              0,
              0,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_SETCONTEXT,
              TRUE,
              0,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_STARTCOMPOSITION,
              0,
              0,
              &message_result) == 1);
    mock_imm.result_text = accepted_result;
    mock_imm.result_count =
        sizeof(accepted_result) / sizeof(accepted_result[0]);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_COMPOSITION,
              0,
              GCS_RESULTSTR,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_ENDCOMPOSITION,
              0,
              0,
              &message_result) == 1);
    mock_imm_active = false;

    CHECK(boxrig_win32_input_adapter_set_cell_size(adapter, 5, 10) == 0);
    CHECK(boxrig_win32_input_adapter_set_viewport_origin(adapter, 7, 11) ==
          0);
    CHECK(boxrig_win32_input_adapter_set_viewport_origin(NULL, 7, 11) == -1);

    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_LBUTTONDOWN,
              MK_LBUTTON,
              MAKELPARAM(22, 36),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_MOUSEMOVE,
              MK_LBUTTON,
              MAKELPARAM(32, 56),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_LBUTTONUP,
              0,
              MAKELPARAM(32, 56),
              &message_result) == 1);
    CHECK(ClientToScreen(window, &wheel_point));
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_MOUSEWHEEL,
              MAKEWPARAM(0, WHEEL_DELTA),
              MAKELPARAM(wheel_point.x, wheel_point.y),
              &message_result) == 1);

    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);

    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_pending_key_retries_atomically(void)
{
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    LRESULT message_result = 0;
    const uint8_t mode[] = "\x1b[=11u";
    const uint8_t *output = NULL;
    size_t consumed = 0;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig input retry test",
        WS_OVERLAPPED,
        0,
        0,
        10,
        10,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 1;
    config.columns = 1;
    config.output_capacity = 5;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              10,
              1024,
              config.output_capacity,
              &adapter) == 0);
    CHECK(boxrig_terminal_feed(
              terminal,
              mode,
              sizeof(mode) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYDOWN,
              VK_ESCAPE,
              key_parameter(VK_ESCAPE, false, false),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_blocked(adapter));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(memcmp(output, "x", 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_retry(adapter) == 0);
    CHECK(!boxrig_win32_input_adapter_blocked(adapter));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 5);
    CHECK(memcmp(output, "\x1b[27u", 5) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 5) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_retry(adapter) == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_streamed_paste_normalizes_and_retries(void)
{
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    const uint8_t mode[] = "\x1b[?2004h";
    const uint8_t raw[] = {
        'a', '\r', '\n', 'b', '\r', 'c', '\t', 0x1b, '[', '2', '0', '1',
        '~', 0x01, 0xc2, 0x80, 'z', 0xe6, 0x97, 0xa5, 0xf0, 0x9f, 0x99, 0x82};
    const uint8_t invalid[] = {0xff};
    const uint8_t expected[] =
        "\x1b[200~a\nb\nc\t[201~z日🙂\x1b[201~";
    const uint8_t *output = NULL;
    uint8_t observed[128];
    size_t observed_length = 0;
    size_t consumed = 0;
    size_t attempts;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig paste retry test",
        WS_OVERLAPPED,
        0,
        0,
        10,
        10,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 1;
    config.columns = 1;
    config.output_capacity = 6;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              10,
              1024,
              1,
              &adapter) == 0);
    CHECK(boxrig_terminal_feed(
              terminal,
              mode,
              sizeof(mode) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_queue_paste(
              adapter,
              raw,
              sizeof(raw)) == 0);
    CHECK(boxrig_win32_input_adapter_blocked(adapter));
    for (attempts = 0; attempts < 32; attempts++) {
        size_t length = boxrig_terminal_output_peek(terminal, &output);

        if (length != 0) {
            CHECK(length <= sizeof(observed) - observed_length);
            memcpy(observed + observed_length, output, length);
            observed_length += length;
            CHECK(boxrig_terminal_output_consume(terminal, length) ==
                  BOXRIG_STATUS_OK);
        }
        CHECK(boxrig_win32_input_adapter_retry(adapter) == 0);
        if (!boxrig_win32_input_adapter_blocked(adapter) &&
            boxrig_terminal_output_peek(terminal, &output) == 0) {
            break;
        }
    }
    CHECK(attempts < 32);
    CHECK(observed_length == sizeof(expected) - 1);
    CHECK(memcmp(observed, expected, observed_length) == 0);
    CHECK(boxrig_win32_input_adapter_queue_paste(
              adapter,
              invalid,
              sizeof(invalid)) == -1);
    CHECK(!boxrig_win32_input_adapter_blocked(adapter));

    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_unicode_clipboard_paste(void)
{
    static const wchar_t clipboard_text[] = {
        L'p', L'a', L's', L't', L'e', L'\r', L'\n',
        L't', L'e', L'x', L't', L' ', 0xd83d, 0xde00, L'\0',
        L'i', L'g', L'n', L'o', L'r', L'e', L'd', L'\0'};
    static const wchar_t invalid_surrogate[] = {0xd800, L'\0'};
    static const wchar_t short_text[] = {L'x', L'\0'};
    HWND window;
    HGLOBAL allocation;
    wchar_t *destination;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    boxrig_ui_view_action action;
    boxrig_ui_view_action_capabilities capabilities;
    LRESULT message_result = 0;
    const uint8_t mode[] = "\x1b[?2004h";
    const uint8_t expected[] =
        "\x1b[200~paste\ntext \xf0\x9f\x98\x80\x1b[201~";
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig clipboard test",
        WS_OVERLAPPED,
        0,
        0,
        10,
        10,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              10,
              1024,
              config.output_capacity,
              &adapter) == 0);
    CHECK(boxrig_terminal_feed(
              terminal,
              mode,
              sizeof(mode) - 1,
              &consumed) == BOXRIG_STATUS_OK);

    allocation = GlobalAlloc(GMEM_MOVEABLE, sizeof(clipboard_text));
    CHECK(allocation != NULL);
    destination = GlobalLock(allocation);
    CHECK(destination != NULL);
    memcpy(destination, clipboard_text, sizeof(clipboard_text));
    (void)GlobalUnlock(allocation);
    CHECK(OpenClipboard(window));
    CHECK(EmptyClipboard());
    CHECK(SetClipboardData(CF_UNICODETEXT, allocation) != NULL);
    CHECK(CloseClipboard());
    allocation = NULL;

    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYDOWN,
              VK_SHIFT,
              key_parameter(VK_SHIFT, false, false),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYDOWN,
              VK_INSERT,
              key_parameter(VK_INSERT, false, false),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYUP,
              VK_INSERT,
              key_parameter(VK_INSERT, false, true),
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYUP,
              VK_SHIFT,
              key_parameter(VK_SHIFT, false, true),
              &message_result) == 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length != sizeof(expected) - 1 ||
          memcmp(output, expected, output_length) != 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    capabilities =
        boxrig_win32_input_adapter_view_action_capabilities(adapter);
    CHECK(capabilities.copy_targets ==
          BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD);
    CHECK(capabilities.paste_targets ==
          BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD);
    action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    CHECK(boxrig_win32_input_adapter_apply_view_action(
              adapter,
              NULL,
              &action) == BOXRIG_UI_INVALID_ARGUMENT);
    action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    CHECK(boxrig_ui_view_action_supported(&capabilities, &action));
    CHECK(boxrig_win32_input_adapter_apply_view_action(
              adapter,
              NULL,
              &action) == BOXRIG_UI_OK);
    action.clipboard_target =
        BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
    CHECK(!boxrig_ui_view_action_supported(&capabilities, &action));
    CHECK(boxrig_win32_input_adapter_apply_view_action(
              adapter,
              NULL,
              &action) == BOXRIG_UI_UNAVAILABLE);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_PASTE,
              0,
              0,
              &message_result) == 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    allocation = GlobalAlloc(GMEM_MOVEABLE, sizeof(invalid_surrogate));
    CHECK(allocation != NULL);
    destination = GlobalLock(allocation);
    CHECK(destination != NULL);
    memcpy(destination, invalid_surrogate, sizeof(invalid_surrogate));
    (void)GlobalUnlock(allocation);
    CHECK(OpenClipboard(window));
    CHECK(EmptyClipboard());
    CHECK(SetClipboardData(CF_UNICODETEXT, allocation) != NULL);
    CHECK(CloseClipboard());
    allocation = NULL;
    action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    CHECK(boxrig_win32_input_adapter_apply_view_action(
              adapter,
              NULL,
              &action) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    allocation = GlobalAlloc(GMEM_MOVEABLE, sizeof(short_text));
    CHECK(allocation != NULL);
    destination = GlobalLock(allocation);
    CHECK(destination != NULL);
    memcpy(destination, short_text, sizeof(short_text));
    (void)GlobalUnlock(allocation);
    CHECK(OpenClipboard(window));
    CHECK(EmptyClipboard());
    CHECK(SetClipboardData(CF_UNICODETEXT, allocation) != NULL);
    CHECK(CloseClipboard());
    allocation = NULL;
    clipboard_global_size_overridden = true;
    clipboard_global_size_override = 3;
    CHECK(boxrig_win32_input_adapter_apply_view_action(
              adapter,
              NULL,
              &action) == BOXRIG_UI_UNAVAILABLE);
    clipboard_global_size_overridden = false;
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    clipboard_open_rejected = true;
    CHECK(boxrig_win32_input_adapter_apply_view_action(
              adapter,
              NULL,
              &action) == BOXRIG_UI_UNAVAILABLE);
    clipboard_open_rejected = false;
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_clipboard_copy_failure_boundaries(void)
{
    static const WCHAR original[] = L"original";
    static const WCHAR expected[] = L"copy";
    HWND window;
    HGLOBAL allocation;
    WCHAR *destination;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    boxrig_ui_hit_test_result hit;
    boxrig_ui_view_action action;
    size_t consumed = 0;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig clipboard copy failure test",
        WS_OVERLAPPED,
        0,
        0,
        10,
        10,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 1;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"copy",
              sizeof("copy") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    memset(&hit, 0, sizeof(hit));
    hit.inside_grid = true;
    hit.text_position_valid = true;
    CHECK(boxrig_terminal_get_text_position(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              0,
              0,
              &hit.leading_position) == BOXRIG_STATUS_OK);
    hit.trailing_position = hit.leading_position;
    hit.position = hit.leading_position;
    CHECK(boxrig_ui_view_begin_selection(
              view,
              BOXRIG_UI_SELECTION_LINE,
              &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              10,
              1024,
              config.output_capacity,
              &adapter) == 0);

    allocation = GlobalAlloc(GMEM_MOVEABLE, sizeof(original));
    CHECK(allocation != NULL);
    destination = GlobalLock(allocation);
    CHECK(destination != NULL);
    memcpy(destination, original, sizeof(original));
    (void)GlobalUnlock(allocation);
    CHECK(OpenClipboard(window));
    CHECK(EmptyClipboard());
    CHECK(SetClipboardData(CF_UNICODETEXT, allocation) != NULL);
    CHECK(CloseClipboard());
    allocation = NULL;

    action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    reset_clipboard_trace();
    clipboard_global_alloc_rejected = true;
    CHECK(boxrig_win32_input_adapter_apply_view_action(
              adapter,
              view,
              &action) == BOXRIG_UI_OUT_OF_MEMORY);
    clipboard_global_alloc_rejected = false;
    CHECK(clipboard_empty_calls == 0 && clipboard_set_calls == 0);
    CHECK(clipboard_equals(window, original));

    reset_clipboard_trace();
    clipboard_global_unlock_rejected = true;
    CHECK(boxrig_win32_input_adapter_apply_view_action(
              adapter,
              view,
              &action) == BOXRIG_UI_UNAVAILABLE);
    clipboard_global_unlock_rejected = false;
    CHECK(clipboard_empty_calls == 0 && clipboard_set_calls == 0 &&
          clipboard_global_free_calls == 1);
    CHECK(clipboard_equals(window, original));

    reset_clipboard_trace();
    clipboard_open_rejected = true;
    CHECK(boxrig_win32_input_adapter_apply_view_action(
              adapter,
              view,
              &action) == BOXRIG_UI_UNAVAILABLE);
    clipboard_open_rejected = false;
    CHECK(clipboard_open_calls == 1 && clipboard_close_calls == 0 &&
          clipboard_empty_calls == 0 && clipboard_set_calls == 0 &&
          clipboard_global_free_calls == 1);
    CHECK(clipboard_equals(window, original));

    reset_clipboard_trace();
    clipboard_set_rejected = true;
    CHECK(boxrig_win32_input_adapter_apply_view_action(
              adapter,
              view,
              &action) == BOXRIG_UI_UNAVAILABLE);
    clipboard_set_rejected = false;
    CHECK(clipboard_open_calls == 1 && clipboard_close_calls == 1 &&
          clipboard_empty_calls == 1 && clipboard_set_calls == 1 &&
          clipboard_global_free_calls == 1);
    CHECK(!IsClipboardFormatAvailable(CF_UNICODETEXT));

    reset_clipboard_trace();
    CHECK(boxrig_win32_input_adapter_apply_view_action(
              adapter,
              view,
              &action) == BOXRIG_UI_OK);
    CHECK(clipboard_open_calls == 1 && clipboard_close_calls == 1 &&
          clipboard_empty_calls == 1 && clipboard_set_calls == 1 &&
          clipboard_global_free_calls == 0);
    CHECK(clipboard_equals(window, expected));
    boxrig_win32_input_adapter_destroy(adapter);
    adapter = NULL;
    CHECK(clipboard_equals(window, expected));

    CHECK(OpenClipboard(window));
    CHECK(EmptyClipboard());
    CHECK(CloseClipboard());
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_imm_updates_follow_the_common_composition_fixture(void)
{
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    boxrig_composition *composition;
    boxrig_composition_view view;
    WCHAR characters[4];
    BYTE attributes[4];
    DWORD empty_clauses[] = {0, 0};
    DWORD clauses[] = {0, 1, 2, 4};
    WCHAR commit[2] = {0x65e5, 0x672c};
    boxrig_text_cluster_input layout_clusters[8];
    boxrig_text_layout *layout = NULL;
    LRESULT message_result = 0;
    const uint8_t *output = NULL;
    size_t output_length;
    size_t index;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig IMM composition fixture",
        WS_OVERLAPPED,
        0,
        0,
        80,
        60,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    composition = boxrig_win32_input_adapter_composition(adapter);
    CHECK(composition != NULL);
    CHECK(boxrig_win32_input_adapter_composition_start(adapter) == 0);
    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              NULL,
              0,
              NULL,
              0,
              empty_clauses,
              sizeof(empty_clauses) / sizeof(empty_clauses[0]),
              0) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active);
    CHECK(view.text_length == 0);
    CHECK(view.segment_count == 0);
    CHECK(view.caret_offset == 0);
    CHECK(view.caret_visible);
    for (index = 0; index < boxrig_composition_fixture.character_count;
         index++) {
        characters[index] =
            (WCHAR)boxrig_composition_fixture.codepoints[index];
        if (boxrig_composition_fixture.attributes[index] ==
            BOXRIG_COMPOSITION_FIXTURE_INPUT) {
            attributes[index] = ATTR_INPUT;
        } else if (boxrig_composition_fixture.attributes[index] ==
                   BOXRIG_COMPOSITION_FIXTURE_CONVERTED) {
            attributes[index] = ATTR_CONVERTED;
        } else {
            attributes[index] = ATTR_TARGET_CONVERTED;
        }
    }

    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              characters,
              4,
              attributes,
              4,
              clauses,
              sizeof(clauses) / sizeof(clauses[0]),
              2) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active);
    CHECK(view.text_length == boxrig_composition_fixture.utf8_length);
    CHECK(memcmp(
              view.text,
              boxrig_composition_fixture.utf8,
              view.text_length) == 0);
    CHECK(view.segment_count == boxrig_composition_fixture.segment_count);
    CHECK(memcmp(
              view.segments,
              boxrig_composition_fixture.segments,
              view.segment_count * sizeof(*view.segments)) == 0);
    CHECK(view.selection_offset ==
          boxrig_composition_fixture.selection_offset);
    CHECK(view.selection_length ==
          boxrig_composition_fixture.selection_length);
    CHECK(view.caret_offset == boxrig_composition_fixture.caret_offset);
    CHECK(view.caret_visible);
    memset(layout_clusters, 0, sizeof(layout_clusters));
    for (index = 0; index < sizeof(layout_clusters) /
                                sizeof(layout_clusters[0]);
         index++) {
        layout_clusters[index].logical_column = index;
        layout_clusters[index].column_count = 1;
        layout_clusters[index].text_offset = index;
        layout_clusters[index].text_length = 1;
    }
    CHECK(boxrig_text_layout_create(
              layout_clusters,
              sizeof(layout_clusters) / sizeof(layout_clusters[0]),
              sizeof(layout_clusters) / sizeof(layout_clusters[0]),
              BOXRIG_TEXT_DIRECTION_LTR,
              &layout) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(boxrig_composition_synchronize(composition, layout, 10, 20, 15) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_input_adapter_synchronize_candidate(adapter) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.candidate.visible);
    CHECK(view.candidate.column == 3);
    CHECK(view.candidate.spot_x == 30);
    CHECK(view.candidate.spot_y == 15);
    boxrig_text_layout_destroy(layout);
    layout = NULL;

    CHECK(boxrig_win32_input_adapter_composition_complete(adapter) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);
    CHECK(view.last_event == BOXRIG_COMPOSITION_PREEDIT_COMPLETE);
    CHECK(boxrig_win32_input_adapter_composition_start(adapter) == 0);
    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              characters,
              4,
              attributes,
              4,
              clauses,
              sizeof(clauses) / sizeof(clauses[0]),
              2) == 0);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_COMPOSITION,
              0,
              0,
              &message_result) == 1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);
    CHECK(view.last_event == BOXRIG_COMPOSITION_CANCEL);

    CHECK(boxrig_win32_input_adapter_composition_start(adapter) == 0);
    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              characters,
              4,
              attributes,
              4,
              clauses,
              sizeof(clauses) / sizeof(clauses[0]),
              2) == 0);
    CHECK(boxrig_win32_input_adapter_composition_commit_utf16(
              adapter,
              commit,
              sizeof(commit) / sizeof(commit[0])) == 0);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == boxrig_composition_fixture.commit_utf8_length);
    CHECK(memcmp(
              output,
              boxrig_composition_fixture.commit_utf8,
              output_length) == 0);
    CHECK(boxrig_win32_input_adapter_retry(adapter) == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == output_length);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active && !view.commit_pending);
    CHECK(view.last_event == BOXRIG_COMPOSITION_COMMIT);

    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_text_layout_destroy(layout);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_imm_clause_snapshots_and_surrogate_boundaries(void)
{
    static const char supplementary_utf8[] =
        "\360\237\230\200\347\225\214";
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    boxrig_composition *composition;
    boxrig_composition_view view;
    WCHAR phrase[] = {0x5909, 0x63db, 0x6587, 0x7bc0};
    BYTE phrase_attributes[] = {
        ATTR_TARGET_CONVERTED,
        ATTR_TARGET_CONVERTED,
        ATTR_CONVERTED,
        ATTR_CONVERTED,
    };
    DWORD phrase_clauses[] = {0, 2, 4};
    WCHAR supplementary[] = {0xd83d, 0xde00, 0x754c};
    BYTE supplementary_attributes[] = {
        ATTR_INPUT,
        ATTR_INPUT,
        ATTR_CONVERTED,
    };
    BYTE split_surrogate_attributes[] = {
        ATTR_INPUT,
        ATTR_CONVERTED,
        ATTR_CONVERTED,
    };
    DWORD supplementary_clauses[] = {0, 2, 3};
    DWORD split_surrogate_clauses[] = {0, 1, 3};
    uint64_t accepted_revision;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig IMM clause snapshot test",
        WS_OVERLAPPED,
        0,
        0,
        80,
        60,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    composition = boxrig_win32_input_adapter_composition(adapter);
    CHECK(composition != NULL);
    CHECK(boxrig_win32_input_adapter_composition_start(adapter) == 0);
    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              phrase,
              sizeof(phrase) / sizeof(phrase[0]),
              phrase_attributes,
              sizeof(phrase_attributes) / sizeof(phrase_attributes[0]),
              phrase_clauses,
              sizeof(phrase_clauses) / sizeof(phrase_clauses[0]),
              2) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.segment_count == 2);
    CHECK(view.selection_offset == 0 && view.selection_length == 6);
    CHECK(view.caret_offset == 6);

    phrase_attributes[0] = ATTR_CONVERTED;
    phrase_attributes[1] = ATTR_CONVERTED;
    phrase_attributes[2] = ATTR_TARGET_CONVERTED;
    phrase_attributes[3] = ATTR_TARGET_CONVERTED;
    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              phrase,
              sizeof(phrase) / sizeof(phrase[0]),
              phrase_attributes,
              sizeof(phrase_attributes) / sizeof(phrase_attributes[0]),
              phrase_clauses,
              sizeof(phrase_clauses) / sizeof(phrase_clauses[0]),
              4) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.selection_offset == 6 && view.selection_length == 6);
    CHECK(view.caret_offset == 12);

    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              supplementary,
              sizeof(supplementary) / sizeof(supplementary[0]),
              supplementary_attributes,
              sizeof(supplementary_attributes) /
                  sizeof(supplementary_attributes[0]),
              supplementary_clauses,
              sizeof(supplementary_clauses) /
                  sizeof(supplementary_clauses[0]),
              2) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.text_length == sizeof(supplementary_utf8) - 1);
    CHECK(memcmp(
              view.text,
              supplementary_utf8,
              view.text_length) == 0);
    CHECK(view.cluster_count == 2 && view.caret_offset == 4);
    accepted_revision = view.revision;

    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              supplementary,
              sizeof(supplementary) / sizeof(supplementary[0]),
              supplementary_attributes,
              sizeof(supplementary_attributes) /
                  sizeof(supplementary_attributes[0]),
              split_surrogate_clauses,
              sizeof(split_surrogate_clauses) /
                  sizeof(split_surrogate_clauses[0]),
              2) == -1);
    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              supplementary,
              sizeof(supplementary) / sizeof(supplementary[0]),
              split_surrogate_attributes,
              sizeof(split_surrogate_attributes) /
                  sizeof(split_surrogate_attributes[0]),
              supplementary_clauses,
              sizeof(supplementary_clauses) /
                  sizeof(supplementary_clauses[0]),
              2) == -1);
    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              supplementary,
              sizeof(supplementary) / sizeof(supplementary[0]),
              supplementary_attributes,
              sizeof(supplementary_attributes) /
                  sizeof(supplementary_attributes[0]),
              supplementary_clauses,
              sizeof(supplementary_clauses) /
                  sizeof(supplementary_clauses[0]),
              1) == -1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.revision == accepted_revision);
    CHECK(view.text_length == sizeof(supplementary_utf8) - 1);
    CHECK(memcmp(
              view.text,
              supplementary_utf8,
              view.text_length) == 0);

    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_imm_optional_metadata_and_malformed_clauses(void)
{
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    boxrig_composition *composition;
    boxrig_composition_view view;
    WCHAR phrase[] = {0x5909, 0x63db, 0x6587, 0x7bc0};
    BYTE attributes[] = {
        ATTR_CONVERTED,
        ATTR_CONVERTED,
        ATTR_TARGET_CONVERTED,
        ATTR_TARGET_CONVERTED,
    };
    DWORD empty_clauses[] = {0, 0};
    DWORD clauses[] = {0, 2, 4};
    DWORD duplicate_clauses[] = {0, 2, 2, 4};
    DWORD missing_tail[] = {0, 2, 3};
    DWORD missing_start[] = {1, 4};
    DWORD single_boundary[] = {0};
    DWORD out_of_range[] = {0, 5};
    uint64_t accepted_revision;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig IMM metadata policy test",
        WS_OVERLAPPED,
        0,
        0,
        80,
        60,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    composition = boxrig_win32_input_adapter_composition(adapter);
    CHECK(composition != NULL);
    CHECK(boxrig_win32_input_adapter_composition_start(adapter) == 0);

    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              NULL,
              0,
              NULL,
              0,
              empty_clauses,
              sizeof(empty_clauses) / sizeof(empty_clauses[0]),
              0) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && view.text_length == 0 && view.segment_count == 0);

    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              phrase,
              sizeof(phrase) / sizeof(phrase[0]),
              NULL,
              0,
              clauses,
              sizeof(clauses) / sizeof(clauses[0]),
              UINT32_MAX) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.segment_count == 2);
    CHECK(view.selection_length == 0 && !view.caret_visible);
    CHECK((view.segments[0].feedback &
           BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE) != 0);
    CHECK((view.segments[1].feedback &
           BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE) != 0);

    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              phrase,
              sizeof(phrase) / sizeof(phrase[0]),
              attributes,
              sizeof(attributes) / sizeof(attributes[0]),
              NULL,
              0,
              4) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.segment_count == 2);
    CHECK(view.selection_offset == 6 && view.selection_length == 6);
    CHECK(view.caret_offset == 12 && view.caret_visible);

    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              phrase,
              sizeof(phrase) / sizeof(phrase[0]),
              NULL,
              0,
              NULL,
              0,
              2) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.segment_count == 1);
    CHECK(view.selection_length == 0 && view.caret_offset == 6);
    accepted_revision = view.revision;

    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              phrase,
              sizeof(phrase) / sizeof(phrase[0]),
              attributes,
              sizeof(attributes) / sizeof(attributes[0]),
              duplicate_clauses,
              sizeof(duplicate_clauses) / sizeof(duplicate_clauses[0]),
              2) == -1);
    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              phrase,
              sizeof(phrase) / sizeof(phrase[0]),
              attributes,
              sizeof(attributes) / sizeof(attributes[0]),
              missing_tail,
              sizeof(missing_tail) / sizeof(missing_tail[0]),
              2) == -1);
    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              phrase,
              sizeof(phrase) / sizeof(phrase[0]),
              attributes,
              sizeof(attributes) / sizeof(attributes[0]),
              missing_start,
              sizeof(missing_start) / sizeof(missing_start[0]),
              2) == -1);
    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              phrase,
              sizeof(phrase) / sizeof(phrase[0]),
              attributes,
              sizeof(attributes) / sizeof(attributes[0]),
              single_boundary,
              sizeof(single_boundary) / sizeof(single_boundary[0]),
              2) == -1);
    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              phrase,
              sizeof(phrase) / sizeof(phrase[0]),
              attributes,
              sizeof(attributes) / sizeof(attributes[0]),
              out_of_range,
              sizeof(out_of_range) / sizeof(out_of_range[0]),
              2) == -1);
    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              phrase,
              sizeof(phrase) / sizeof(phrase[0]),
              NULL,
              1,
              NULL,
              0,
              2) == -1);
    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              phrase,
              sizeof(phrase) / sizeof(phrase[0]),
              attributes,
              3,
              NULL,
              0,
              2) == -1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.revision == accepted_revision);
    CHECK(view.segment_count == 1 && view.caret_offset == 6);
    CHECK(view.text_length == 12);

    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_imm_candidate_forms_report_rejection_and_retry(void)
{
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    boxrig_composition *composition;
    boxrig_composition_view view;
    WCHAR text[] = {L'A', 0x754c, L'e'};
    BYTE attributes[] = {ATTR_INPUT, ATTR_INPUT, ATTR_INPUT};
    DWORD clauses[] = {0, 3};
    boxrig_text_cluster_input layout_clusters[8];
    boxrig_text_layout *layout = NULL;
    const uint8_t *output = NULL;
    boxrig_cursor_view cursor_before;
    boxrig_cursor_view cursor_after;
    uint64_t accepted_revision;
    uint64_t terminal_revision;
    size_t scrollback_rows;
    size_t viewport_offset;
    size_t consumed = 0;
    size_t index;
    boxrig_win32_ime_placement_status composition_rejected_status;
    boxrig_win32_ime_placement_status candidate_rejected_status;
    boxrig_win32_ime_placement_status both_rejected_status;
    COMPOSITIONFORM accepted_composition_form;
    CANDIDATEFORM accepted_candidate_form;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig IMM candidate form test",
        WS_OVERLAPPED,
        0,
        0,
        80,
        60,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              11,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    CHECK(boxrig_win32_input_adapter_set_viewport_origin(adapter, 13, 17) ==
          0);
    composition = boxrig_win32_input_adapter_composition(adapter);
    CHECK(composition != NULL);
    CHECK(boxrig_win32_input_adapter_composition_start(adapter) == 0);
    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              text,
              sizeof(text) / sizeof(text[0]),
              attributes,
              sizeof(attributes) / sizeof(attributes[0]),
              clauses,
              sizeof(clauses) / sizeof(clauses[0]),
              2) == 0);
    memset(layout_clusters, 0, sizeof(layout_clusters));
    for (index = 0;
         index < sizeof(layout_clusters) / sizeof(layout_clusters[0]);
         index++) {
        layout_clusters[index].logical_column = index;
        layout_clusters[index].column_count = 1;
        layout_clusters[index].text_offset = index;
        layout_clusters[index].text_length = 1;
    }
    CHECK(boxrig_text_layout_create(
              layout_clusters,
              sizeof(layout_clusters) / sizeof(layout_clusters[0]),
              sizeof(layout_clusters) / sizeof(layout_clusters[0]),
              BOXRIG_TEXT_DIRECTION_LTR,
              &layout) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\033[2;8H",
              sizeof("\033[2;8H") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof("\033[2;8H") - 1);
    CHECK(boxrig_composition_synchronize(composition, layout, 11, 20, 15) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.candidate.visible);
    CHECK(view.candidate.row == 2 && view.candidate.column == 2);
    CHECK(view.candidate.spot_x == 22 && view.candidate.spot_y == 55);
    CHECK(view.candidate.exclude_x == 22 &&
          view.candidate.exclude_y == 40);
    CHECK(view.candidate.exclude_width == 11 &&
          view.candidate.exclude_height == 20);
    accepted_revision = view.revision;
    terminal_revision = boxrig_terminal_active_buffer_revision(terminal);
    scrollback_rows = boxrig_terminal_scrollback_rows(terminal);
    viewport_offset = boxrig_terminal_viewport_offset(terminal);
    boxrig_terminal_get_cursor(terminal, &cursor_before);

    memset(&mock_composition_form, 0, sizeof(mock_composition_form));
    memset(&mock_candidate_form, 0, sizeof(mock_candidate_form));
    mock_composition_form_calls = 0;
    mock_candidate_form_calls = 0;
    mock_composition_form_rejected = true;
    mock_candidate_form_rejected = false;
    mock_imm_active = true;
    composition_rejected_status =
        boxrig_win32_input_adapter_synchronize_candidate(adapter);
    CHECK(composition_rejected_status ==
          BOXRIG_WIN32_IME_PLACEMENT_COMPOSITION_REJECTED);
    CHECK(mock_composition_form_calls == 1);
    CHECK(mock_candidate_form_calls == 1);
    CHECK(mock_composition_form.dwStyle == CFS_POINT);
    CHECK(mock_composition_form.ptCurrentPos.x == 35 &&
          mock_composition_form.ptCurrentPos.y == 72);
    CHECK(mock_candidate_form.dwIndex == 0);
    CHECK(mock_candidate_form.dwStyle == CFS_EXCLUDE);
    CHECK(mock_candidate_form.ptCurrentPos.x == 35 &&
          mock_candidate_form.ptCurrentPos.y == 72);
    CHECK(mock_candidate_form.rcArea.left == 35 &&
          mock_candidate_form.rcArea.top == 57 &&
          mock_candidate_form.rcArea.right == 46 &&
          mock_candidate_form.rcArea.bottom == 77);
    CHECK(boxrig_win32_input_adapter_synchronize_candidate(adapter) ==
          composition_rejected_status);
    CHECK(mock_composition_form_calls == 2);
    CHECK(mock_candidate_form_calls == 1);
    mock_composition_form_rejected = false;
    CHECK(boxrig_win32_input_adapter_synchronize_candidate(adapter) == 0);
    CHECK(mock_composition_form_calls == 3);
    CHECK(mock_candidate_form_calls == 1);
    CHECK(boxrig_win32_input_adapter_synchronize_candidate(adapter) == 0);
    CHECK(mock_composition_form_calls == 3);
    CHECK(mock_candidate_form_calls == 1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.revision == accepted_revision);
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
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    CHECK(boxrig_composition_synchronize(composition, layout, 11, 20, 16) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    accepted_revision = view.revision;
    mock_candidate_form_rejected = true;
    candidate_rejected_status =
        boxrig_win32_input_adapter_synchronize_candidate(adapter);
    CHECK(candidate_rejected_status ==
          BOXRIG_WIN32_IME_PLACEMENT_CANDIDATE_REJECTED);
    CHECK(mock_composition_form_calls == 4);
    CHECK(mock_candidate_form_calls == 2);
    CHECK(boxrig_win32_input_adapter_synchronize_candidate(adapter) ==
          candidate_rejected_status);
    CHECK(mock_composition_form_calls == 4);
    CHECK(mock_candidate_form_calls == 3);
    mock_candidate_form_rejected = false;
    CHECK(boxrig_win32_input_adapter_synchronize_candidate(adapter) == 0);
    CHECK(mock_composition_form_calls == 4);
    CHECK(mock_candidate_form_calls == 4);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.revision == accepted_revision);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    CHECK(boxrig_composition_synchronize(composition, layout, 11, 20, 17) ==
          BOXRIG_COMPOSITION_OK);
    mock_composition_form_rejected = true;
    mock_candidate_form_rejected = true;
    both_rejected_status =
        boxrig_win32_input_adapter_synchronize_candidate(adapter);
    CHECK(both_rejected_status ==
          BOXRIG_WIN32_IME_PLACEMENT_BOTH_REJECTED);
    CHECK(mock_composition_form_calls == 5);
    CHECK(mock_candidate_form_calls == 5);
    mock_composition_form_rejected = false;
    mock_candidate_form_rejected = false;
    CHECK(boxrig_win32_input_adapter_synchronize_candidate(adapter) == 0);
    CHECK(mock_composition_form_calls == 6);
    CHECK(mock_candidate_form_calls == 6);

    CHECK(boxrig_win32_input_adapter_composition_complete(adapter) == 0);
    CHECK(boxrig_win32_input_adapter_composition_start(adapter) == 0);
    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              text,
              sizeof(text) / sizeof(text[0]),
              attributes,
              sizeof(attributes) / sizeof(attributes[0]),
              clauses,
              sizeof(clauses) / sizeof(clauses[0]),
              2) == 0);
    CHECK(boxrig_composition_synchronize(composition, layout, 11, 20, 17) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_input_adapter_synchronize_candidate(adapter) == 0);
    CHECK(mock_composition_form_calls == 7);
    CHECK(mock_candidate_form_calls == 7);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\033[3;8H",
              sizeof("\033[3;8H") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof("\033[3;8H") - 1);
    CHECK(boxrig_composition_synchronize(composition, layout, 11, 20, 15) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.candidate.visible);
    CHECK(boxrig_win32_input_adapter_synchronize_candidate(adapter) == 0);
    CHECK(mock_composition_form_calls == 7);
    CHECK(mock_candidate_form_calls == 7);
    CHECK(boxrig_win32_input_adapter_resynchronize_candidate(adapter) == 0);
    CHECK(mock_composition_form_calls == 7);
    CHECK(mock_candidate_form_calls == 7);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\033[2;8H",
              sizeof("\033[2;8H") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof("\033[2;8H") - 1);
    CHECK(boxrig_composition_synchronize(composition, layout, 11, 20, 17) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_input_adapter_synchronize_candidate(adapter) == 0);
    CHECK(mock_composition_form_calls == 8);
    CHECK(mock_candidate_form_calls == 8);

    mock_imm_active = false;
    CHECK(boxrig_win32_input_adapter_synchronize_candidate(adapter) == 0);
    CHECK(mock_composition_form_calls == 8);
    CHECK(mock_candidate_form_calls == 8);
    CHECK(boxrig_win32_input_adapter_resynchronize_candidate(adapter) == 0);
    CHECK(mock_composition_form_calls == 8);
    CHECK(mock_candidate_form_calls == 8);
    mock_imm_active = true;
    CHECK(boxrig_win32_input_adapter_synchronize_candidate(adapter) == 0);
    CHECK(mock_composition_form_calls == 9);
    CHECK(mock_candidate_form_calls == 9);

    accepted_composition_form = mock_composition_form;
    accepted_candidate_form = mock_candidate_form;
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    accepted_revision = view.revision;
    terminal_revision = boxrig_terminal_active_buffer_revision(terminal);
    scrollback_rows = boxrig_terminal_scrollback_rows(terminal);
    viewport_offset = boxrig_terminal_viewport_offset(terminal);
    boxrig_terminal_get_cursor(terminal, &cursor_before);
    CHECK(boxrig_win32_input_adapter_resynchronize_candidate(adapter) == 0);
    CHECK(mock_composition_form_calls == 10);
    CHECK(mock_candidate_form_calls == 10);
    CHECK(mock_composition_form.dwStyle ==
              accepted_composition_form.dwStyle &&
          mock_composition_form.ptCurrentPos.x ==
              accepted_composition_form.ptCurrentPos.x &&
          mock_composition_form.ptCurrentPos.y ==
              accepted_composition_form.ptCurrentPos.y &&
          mock_composition_form.rcArea.left ==
              accepted_composition_form.rcArea.left &&
          mock_composition_form.rcArea.top ==
              accepted_composition_form.rcArea.top &&
          mock_composition_form.rcArea.right ==
              accepted_composition_form.rcArea.right &&
          mock_composition_form.rcArea.bottom ==
              accepted_composition_form.rcArea.bottom);
    CHECK(mock_candidate_form.dwIndex == accepted_candidate_form.dwIndex &&
          mock_candidate_form.dwStyle == accepted_candidate_form.dwStyle &&
          mock_candidate_form.ptCurrentPos.x ==
              accepted_candidate_form.ptCurrentPos.x &&
          mock_candidate_form.ptCurrentPos.y ==
              accepted_candidate_form.ptCurrentPos.y &&
          mock_candidate_form.rcArea.left ==
              accepted_candidate_form.rcArea.left &&
          mock_candidate_form.rcArea.top ==
              accepted_candidate_form.rcArea.top &&
          mock_candidate_form.rcArea.right ==
              accepted_candidate_form.rcArea.right &&
          mock_candidate_form.rcArea.bottom ==
              accepted_candidate_form.rcArea.bottom);
    CHECK(boxrig_win32_input_adapter_synchronize_candidate(adapter) == 0);
    CHECK(mock_composition_form_calls == 10);
    CHECK(mock_candidate_form_calls == 10);

    mock_composition_form_rejected = true;
    CHECK(boxrig_win32_input_adapter_resynchronize_candidate(adapter) ==
          BOXRIG_WIN32_IME_PLACEMENT_COMPOSITION_REJECTED);
    CHECK(mock_composition_form_calls == 11);
    CHECK(mock_candidate_form_calls == 11);
    CHECK(boxrig_win32_input_adapter_synchronize_candidate(adapter) ==
          BOXRIG_WIN32_IME_PLACEMENT_COMPOSITION_REJECTED);
    CHECK(mock_composition_form_calls == 12);
    CHECK(mock_candidate_form_calls == 11);
    mock_composition_form_rejected = false;
    CHECK(boxrig_win32_input_adapter_synchronize_candidate(adapter) == 0);
    CHECK(mock_composition_form_calls == 13);
    CHECK(mock_candidate_form_calls == 11);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.revision == accepted_revision);
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
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              text,
              sizeof(text) / sizeof(text[0]),
              attributes,
              sizeof(attributes) / sizeof(attributes[0]),
              clauses,
              sizeof(clauses) / sizeof(clauses[0]),
              1) == 0);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\033[1;1H",
              sizeof("\033[1;1H") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof("\033[1;1H") - 1);
    CHECK(boxrig_composition_synchronize(
              composition,
              layout,
              (unsigned int)LONG_MAX + 1U,
              20,
              17) == BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_input_adapter_synchronize_candidate(adapter) ==
          BOXRIG_WIN32_IME_PLACEMENT_ERROR);
    CHECK(mock_composition_form_calls == 13);
    CHECK(mock_candidate_form_calls == 11);
    CHECK(boxrig_win32_input_adapter_synchronize_candidate(NULL) ==
          BOXRIG_WIN32_IME_PLACEMENT_ERROR);
    CHECK(boxrig_win32_input_adapter_resynchronize_candidate(NULL) ==
          BOXRIG_WIN32_IME_PLACEMENT_ERROR);
    mock_imm_active = false;

    boxrig_text_layout_destroy(layout);
    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_imm_result_and_following_preedit_survive_backpressure(void)
{
    static const uint8_t result_utf8[] = {0xe6, 0x97, 0xa5};
    static const uint8_t next_result_utf8[] = {0xe8, 0xaa, 0x9e};
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    boxrig_composition *composition;
    boxrig_composition_view view;
    WCHAR result[] = {0x65e5};
    WCHAR preedit[] = {0x672c};
    WCHAR next_result[] = {0x8a9e};
    WCHAR next_preedit[] = {L'A'};
    BYTE attributes[] = {ATTR_INPUT};
    DWORD clauses[] = {0, 1};
    LRESULT message_result = 0;
    const uint8_t *output = NULL;
    size_t output_length;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig IMM result and preedit test",
        WS_OVERLAPPED,
        0,
        0,
        80,
        60,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 3;
    config.columns = 8;
    config.output_capacity = 3;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    composition = boxrig_win32_input_adapter_composition(adapter);
    CHECK(composition != NULL);
    CHECK(boxrig_terminal_send_text(terminal, "xyz", 3) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_composition_start(adapter) == 0);

    memset(&mock_imm, 0, sizeof(mock_imm));
    mock_imm.result_text = result;
    mock_imm.result_count = sizeof(result) / sizeof(result[0]);
    mock_imm.preedit_text = preedit;
    mock_imm.preedit_count = sizeof(preedit) / sizeof(preedit[0]);
    mock_imm.attributes = attributes;
    mock_imm.attribute_count = sizeof(attributes) / sizeof(attributes[0]);
    mock_imm.clauses = clauses;
    mock_imm.clause_count = sizeof(clauses) / sizeof(clauses[0]);
    mock_imm.caret = 1;
    mock_imm_active = true;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_COMPOSITION,
              0,
              GCS_RESULTSTR | GCS_COMPSTR | GCS_COMPATTR |
                  GCS_COMPCLAUSE | GCS_CURSORPOS,
              &message_result) == 1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.commit_pending && !view.active);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 3 && memcmp(output, "xyz", 3) == 0);

    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_COMPOSITION,
              0,
              GCS_RESULTSTR | GCS_COMPSTR | GCS_COMPATTR |
                  GCS_COMPCLAUSE | GCS_CURSORPOS,
              &message_result) == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 3);
    CHECK(boxrig_terminal_output_consume(terminal, 3) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_retry(adapter) == 0);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(result_utf8));
    CHECK(memcmp(output, result_utf8, output_length) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.commit_pending && view.active);
    CHECK(view.text_length == sizeof(result_utf8));
    CHECK(memcmp(view.text, "\346\234\254", view.text_length) == 0);
    CHECK(view.caret_offset == sizeof(result_utf8));
    CHECK(boxrig_win32_input_adapter_retry(adapter) == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(result_utf8));
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(result_utf8)) ==
          BOXRIG_STATUS_OK);

    mock_imm.result_text = next_result;
    mock_imm.result_count = sizeof(next_result) / sizeof(next_result[0]);
    mock_imm.preedit_text = next_preedit;
    mock_imm.preedit_count =
        sizeof(next_preedit) / sizeof(next_preedit[0]);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_COMPOSITION,
              0,
              GCS_RESULTSTR | GCS_COMPSTR | GCS_COMPATTR |
                  GCS_COMPCLAUSE | GCS_CURSORPOS,
              &message_result) == 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(next_result_utf8));
    CHECK(memcmp(output, next_result_utf8, output_length) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.commit_pending && view.active);
    CHECK(view.text_length == 1 && view.text[0] == 'A');
    CHECK(view.caret_offset == 1);
    mock_imm_active = false;

    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_wm_ime_char_commits_every_utf16_character(void)
{
    static const char expected[] =
        "\346\227\245\346\234\254\360\237\230\200\350\252\236";
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    boxrig_composition *composition;
    boxrig_composition_view view;
    LRESULT message_result = 0;
    const uint8_t *output = NULL;
    size_t output_length;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig WM_IME_CHAR sequence test",
        WS_OVERLAPPED,
        0,
        0,
        80,
        60,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    composition = boxrig_win32_input_adapter_composition(adapter);
    CHECK(composition != NULL);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_STARTCOMPOSITION,
              0,
              0,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_CHAR,
              0x65e5,
              0,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_CHAR,
              0x672c,
              0,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_CHAR,
              0xd83d,
              0,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_CHAR,
              0xde00,
              0,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_ENDCOMPOSITION,
              0,
              0,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_CHAR,
              0x8a9e,
              0,
              &message_result) == 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active && !view.commit_pending);
    CHECK(view.last_event == BOXRIG_COMPOSITION_COMMIT);

    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_unsupported_imm_reconversion_requests_are_inert(void)
{
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    boxrig_composition *composition;
    boxrig_composition_view before;
    boxrig_composition_view after;
    RECONVERTSTRING request;
    RECONVERTSTRING original_request;
    LRESULT message_result;
    const uint8_t *output = NULL;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig unsupported reconversion test",
        WS_OVERLAPPED,
        0,
        0,
        80,
        60,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    composition = boxrig_win32_input_adapter_composition(adapter);
    CHECK(composition != NULL);
    CHECK(boxrig_composition_get_view(composition, &before) ==
          BOXRIG_COMPOSITION_OK);

    memset(&request, 0x5a, sizeof(request));
    request.dwSize = sizeof(request);
    original_request = request;

    message_result = 123;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_RECONVERTSTRING,
              0,
              &message_result) == 1);
    CHECK(message_result == 0);
    message_result = 123;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_RECONVERTSTRING,
              (LPARAM)&request,
              &message_result) == 1);
    CHECK(message_result == 0);
    CHECK(memcmp(&request, &original_request, sizeof(request)) == 0);

    message_result = 123;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_CONFIRMRECONVERTSTRING,
              (LPARAM)&request,
              &message_result) == 1);
    CHECK(message_result == 0);
    CHECK(memcmp(&request, &original_request, sizeof(request)) == 0);

    message_result = 123;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_DOCUMENTFEED,
              0,
              &message_result) == 1);
    CHECK(message_result == 0);
    message_result = 123;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_DOCUMENTFEED,
              (LPARAM)&request,
              &message_result) == 1);
    CHECK(message_result == 0);
    CHECK(memcmp(&request, &original_request, sizeof(request)) == 0);

    message_result = 123;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_QUERYCHARPOSITION,
              0,
              &message_result) == 0);
    CHECK(message_result == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_composition_get_view(composition, &after) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!after.active && !after.commit_pending);
    CHECK(after.revision == before.revision);

    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

/* DOCUMENTFEED must not invoke a synchronous editor, even when one exists. */
static boxrig_ui_status observe_unexpected_imm_edit(
    const boxrig_ui_editable_replacement_request *request,
    const boxrig_ui_editable_document_snapshot *proposed, void *context)
{
    (void)request;
    (void)proposed;
    *(bool *)context = true;
    return BOXRIG_UI_UNAVAILABLE;
}

static bool test_imm_document_feed_distinguishes_context_from_reconversion(void)
{
    static const char text[] = "前😀e\xcc\x81\n後";
    static const WCHAR expected[] = {
        0x524d, 0xd83d, 0xde00, L'e', 0x0301, L'\n', 0x5f8c};
    static const struct {
        size_t cursor;
        size_t anchor;
        DWORD offset;
        DWORD length;
        bool reconversion;
    } cases[] = {
        {0, 0, 0, 0, false},
        {7, 7, 6, 0, false},
        {8, 8, 8, 0, false},
        {11, 3, 2, 5, true},
        {7, 8, 6, 1, false},
        {14, 14, 14, 0, false},
    };
    union {
        RECONVERTSTRING request;
        BYTE bytes[256];
    } buffer, original;
    HWND window = CreateWindowExW(
        0, L"STATIC", L"boxrig document feed test", WS_OVERLAPPED,
        0, 0, 80, 60, NULL, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(window != NULL);
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    boxrig_ui_editable_document *document =
        boxrig_ui_view_editable_document(view);
    CHECK(boxrig_win32_input_adapter_create(
              window, terminal, 10, 20, 1024,
              config.output_capacity, &adapter) == 0);
    CHECK(boxrig_win32_input_adapter_set_editable_document(
              adapter, document) == 0);
    mock_reconversion_supported = true;

    bool unexpected_edit = false;
    for (size_t index = 0; index < sizeof(boxrig_editable_cases) / sizeof(boxrig_editable_cases[0]); index++) {
        CHECK(boxrig_ui_editable_document_clear(document) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_editable_document_set_apply_handler(document,
            boxrig_editable_cases[index].synchronous_edit_available ? observe_unexpected_imm_edit : NULL,
            boxrig_editable_cases[index].synchronous_edit_available ? &unexpected_edit : NULL) == BOXRIG_UI_OK);
        boxrig_ui_editable_context_input input = {111, 1, boxrig_editable_fixture.text,
            strlen(boxrig_editable_fixture.text), 100,
            100 + boxrig_editable_fixture.cursor, 100 + boxrig_editable_fixture.anchor};
        if (boxrig_editable_cases[index].context_available)
            CHECK(boxrig_ui_editable_document_publish_context(document, &input) == BOXRIG_UI_OK);
        boxrig_ui_editable_capabilities caps = boxrig_ui_editable_document_capabilities(document);
        CHECK(caps.context_available == boxrig_editable_cases[index].context_available);
        CHECK(caps.synchronous_edit_available == boxrig_editable_cases[index].synchronous_edit_available);
        CHECK(!caps.reconversion_available && !caps.busy);
        LRESULT result = -1;
        memset(&buffer, 0, sizeof(buffer));
        buffer.request.dwSize = sizeof(buffer);
        CHECK(boxrig_win32_input_adapter_process_message(adapter, WM_IME_REQUEST,
            IMR_DOCUMENTFEED, (LPARAM)&buffer, &result) == 1);
        if (caps.context_available) {
            static const WCHAR shared_text[] = {0x524d, 0x304b, 0x306a, 0x5f8c};
            CHECK(result == sizeof(RECONVERTSTRING) + sizeof(shared_text));
            CHECK(buffer.request.dwStrLen == 4 && buffer.request.dwCompStrOffset == 6);
            CHECK(buffer.request.dwCompStrLen == 0 && buffer.request.dwTargetStrLen == 0);
            CHECK(memcmp(buffer.bytes + buffer.request.dwStrOffset, shared_text, sizeof(shared_text)) == 0);
        } else CHECK(result == 0);
        CHECK(!unexpected_edit);
        CHECK(!boxrig_win32_input_adapter_view_action_capabilities(adapter).reconversion);
        const uint8_t *output;
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
        printf("PASS IMM shared editable state=%s\n", boxrig_editable_cases[index].name);
    }
    CHECK(boxrig_ui_editable_document_clear(document) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_editable_document_set_apply_handler(document, NULL, NULL) == BOXRIG_UI_OK);

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        bool empty = index == 0;
        boxrig_ui_editable_context_input input = {
            .document_identity = empty ? 112 : 113,
            .document_revision = 1,
            .context = empty ? "" : text,
            .context_length = empty ? 0 : sizeof(text) - 1,
            .context_offset = 100,
            .cursor = 100 + cases[index].cursor,
            .anchor = 100 + cases[index].anchor,
        };
        CHECK(boxrig_ui_editable_document_publish_context(
                  document, &input) == BOXRIG_UI_OK);
        uint64_t generation =
            boxrig_ui_editable_document_input_generation(document);
        LRESULT required = 0;
        CHECK(boxrig_win32_input_adapter_process_message(
                  adapter, WM_IME_REQUEST, IMR_DOCUMENTFEED,
                  0, &required) == 1);
        CHECK(required == (LRESULT)(sizeof(RECONVERTSTRING) +
                                   (empty ? 0 : sizeof(expected))));
        memset(&buffer, 0x5a, sizeof(buffer));
        buffer.request.dwSize = sizeof(buffer);
        LRESULT result = 0;
        CHECK(boxrig_win32_input_adapter_process_message(
                  adapter, WM_IME_REQUEST, IMR_DOCUMENTFEED,
                  (LPARAM)&buffer, &result) == 1);
        CHECK(result == required);
        CHECK(buffer.request.dwSize == (DWORD)required);
        CHECK(buffer.request.dwVersion == 0);
        CHECK(buffer.request.dwStrLen == (empty ? 0 : 7));
        CHECK(buffer.request.dwStrOffset == sizeof(RECONVERTSTRING));
        CHECK(buffer.request.dwCompStrOffset == cases[index].offset);
        CHECK(buffer.request.dwCompStrLen == cases[index].length);
        CHECK(buffer.request.dwTargetStrOffset == cases[index].offset);
        CHECK(buffer.request.dwTargetStrLen == cases[index].length);
        CHECK(empty || memcmp(buffer.bytes + buffer.request.dwStrOffset,
                              expected, sizeof(expected)) == 0);

        buffer.request.dwSize = (DWORD)required - 1;
        original = buffer;
        CHECK(boxrig_win32_input_adapter_process_message(
                  adapter, WM_IME_REQUEST, IMR_DOCUMENTFEED,
                  (LPARAM)&buffer, &result) == 1);
        CHECK(result == 0);
        CHECK(memcmp(&buffer, &original, sizeof(buffer)) == 0);
        CHECK(boxrig_ui_editable_document_input_generation(document) ==
              generation);
        CHECK(boxrig_ui_editable_document_reconversion_state(document) ==
              BOXRIG_UI_EDITABLE_RECONVERSION_NONE);
        boxrig_ui_view_action_capabilities capabilities =
            boxrig_win32_input_adapter_view_action_capabilities(adapter);
        CHECK(capabilities.reconversion == cases[index].reconversion);
        if (!cases[index].reconversion) {
            boxrig_ui_view_action action = {
                .kind = BOXRIG_UI_VIEW_ACTION_RECONVERT,
            };
            CHECK(boxrig_win32_input_adapter_apply_view_action(
                      adapter, view, &action) == BOXRIG_UI_UNAVAILABLE);
            CHECK(boxrig_win32_input_adapter_process_message(
                      adapter, WM_IME_REQUEST, IMR_RECONVERTSTRING,
                      0, &result) == 1);
            CHECK(result == 0);
        }
        mock_reconversion_supported = false;
        capabilities =
            boxrig_win32_input_adapter_view_action_capabilities(adapter);
        CHECK(!capabilities.reconversion);
        mock_reconversion_supported = true;
        const uint8_t *output = NULL;
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    }

    mock_reconversion_supported = false;
    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_imm_reconversion_requests_map_utf16_ranges(void)
{
    static const char context[] =
        "A\xe6\x97\xa5\xe6\x9c\xac\xf0\x9f\x98\x80"
        "e\xcc\x81\xe2\x80\xa8Z";
    static const WCHAR expected_context[] = {
        L'A', 0x65e5, 0x672c, 0xd83d, 0xde00,
        L'e', 0x0301, 0x2028, L'Z'};
    typedef union reconvert_buffer {
        RECONVERTSTRING request;
        BYTE bytes[256];
    } reconvert_buffer;
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_editable_document *document;
    boxrig_ui_editable_document_snapshot_input snapshot;
    boxrig_ui_editable_replacement_request replacement;
    boxrig_ui_editable_replacement_completion completion;
    boxrig_win32_input_adapter *adapter = NULL;
    reconvert_buffer buffer;
    reconvert_buffer original_buffer;
    LRESULT required;
    LRESULT message_result;
    const WCHAR *returned_context;
    const uint8_t *output = NULL;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig reconversion mapping test",
        WS_OVERLAPPED,
        0,
        0,
        80,
        60,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    document = boxrig_ui_view_editable_document(view);
    CHECK(document != NULL);
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.document_identity = 19;
    snapshot.document_revision = 41;
    snapshot.context = context;
    snapshot.context_length = sizeof(context) - 1;
    snapshot.context_offset = 100;
    snapshot.selection.offset = 101;
    snapshot.selection.length = 13;
    CHECK(boxrig_ui_editable_document_publish(document, &snapshot) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    CHECK(boxrig_win32_input_adapter_set_editable_document(
              adapter,
              document) == 0);

    required = 0;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_RECONVERTSTRING,
              0,
              &required) == 1);
    CHECK(required ==
          (LRESULT)(sizeof(RECONVERTSTRING) + sizeof(expected_context)));

    memset(&buffer, 0x5a, sizeof(buffer));
    buffer.request.dwSize = sizeof(buffer.bytes);
    message_result = 0;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_RECONVERTSTRING,
              (LPARAM)&buffer,
              &message_result) == 1);
    CHECK(message_result == required);
    CHECK(buffer.request.dwSize == (DWORD)required);
    CHECK(buffer.request.dwVersion == 0);
    CHECK(buffer.request.dwStrLen == 9);
    CHECK(buffer.request.dwStrOffset == sizeof(RECONVERTSTRING));
    CHECK(buffer.request.dwCompStrLen == 6);
    CHECK(buffer.request.dwCompStrOffset == sizeof(WCHAR));
    CHECK(buffer.request.dwTargetStrLen == 6);
    CHECK(buffer.request.dwTargetStrOffset == sizeof(WCHAR));
    returned_context = (const WCHAR *)(
        buffer.bytes + buffer.request.dwStrOffset);
    CHECK(memcmp(
              returned_context,
              expected_context,
              sizeof(expected_context)) == 0);

    message_result = 0;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_DOCUMENTFEED,
              0,
              &message_result) == 1);
    CHECK(message_result == required);
    memset(&buffer, 0, sizeof(buffer));
    buffer.request.dwSize = sizeof(buffer.bytes);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_DOCUMENTFEED,
              (LPARAM)&buffer,
              &message_result) == 1);
    CHECK(message_result == required);
    returned_context = (const WCHAR *)(
        buffer.bytes + buffer.request.dwStrOffset);
    CHECK(memcmp(
              returned_context,
              expected_context,
              sizeof(expected_context)) == 0);

    memset(&buffer, 0x5a, sizeof(buffer));
    buffer.request.dwSize = (DWORD)required - 1;
    original_buffer = buffer;
    message_result = 123;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_RECONVERTSTRING,
              (LPARAM)&buffer,
              &message_result) == 1);
    CHECK(message_result == 0);
    CHECK(memcmp(&buffer, &original_buffer, sizeof(buffer)) == 0);

    memset(&buffer, 0, sizeof(buffer));
    buffer.request.dwSize = sizeof(buffer.bytes);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_RECONVERTSTRING,
              (LPARAM)&buffer,
              &message_result) == 1);
    CHECK(message_result == required);
    original_buffer = buffer;

    buffer.request.dwVersion = 1;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_CONFIRMRECONVERTSTRING,
              (LPARAM)&buffer,
              &message_result) == 1);
    CHECK(message_result == 0);
    buffer = original_buffer;
    buffer.request.dwSize = sizeof(RECONVERTSTRING) - 1;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_CONFIRMRECONVERTSTRING,
              (LPARAM)&buffer,
              &message_result) == 1);
    CHECK(message_result == 0);
    buffer = original_buffer;
    buffer.request.dwStrOffset++;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_CONFIRMRECONVERTSTRING,
              (LPARAM)&buffer,
              &message_result) == 1);
    CHECK(message_result == 0);
    buffer = original_buffer;
    buffer.request.dwStrLen = UINT32_MAX;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_CONFIRMRECONVERTSTRING,
              (LPARAM)&buffer,
              &message_result) == 1);
    CHECK(message_result == 0);
    buffer = original_buffer;
    buffer.request.dwTargetStrOffset = 8;
    buffer.request.dwTargetStrLen = 1;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_CONFIRMRECONVERTSTRING,
              (LPARAM)&buffer,
              &message_result) == 1);
    CHECK(message_result == 0);
    buffer = original_buffer;
    buffer.request.dwTargetStrOffset = 12;
    buffer.request.dwTargetStrLen = 1;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_CONFIRMRECONVERTSTRING,
              (LPARAM)&buffer,
              &message_result) == 1);
    CHECK(message_result == 0);
    buffer = original_buffer;
    buffer.request.dwTargetStrOffset = 18;
    buffer.request.dwTargetStrLen = 1;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_CONFIRMRECONVERTSTRING,
              (LPARAM)&buffer,
              &message_result) == 1);
    CHECK(message_result == 0);
    buffer = original_buffer;
    ((WCHAR *)(buffer.bytes + buffer.request.dwStrOffset))[3] = 0xd800;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_CONFIRMRECONVERTSTRING,
              (LPARAM)&buffer,
              &message_result) == 1);
    CHECK(message_result == 0);

    buffer = original_buffer;
    buffer.request.dwCompStrOffset = 4;
    buffer.request.dwCompStrLen = 5;
    buffer.request.dwTargetStrOffset = 6;
    buffer.request.dwTargetStrLen = 2;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_CONFIRMRECONVERTSTRING,
              (LPARAM)&buffer,
              &message_result) == 1);
    CHECK(message_result == TRUE);
    CHECK(boxrig_ui_editable_document_commit_reconversion("x", 1, document) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_editable_document_take_replacement(
        document,
        &replacement));
    CHECK(replacement.document_identity == 19);
    CHECK(replacement.base_document_revision == 41);
    CHECK(replacement.range.offset == 104);
    CHECK(replacement.range.length == 10);
    CHECK(replacement.replacement_length == 1);
    CHECK(replacement.replacement[0] == 'x');
    memset(&completion, 0, sizeof(completion));
    completion.request_id = replacement.request_id;
    completion.outcome = BOXRIG_UI_EDITABLE_REPLACEMENT_REJECTED;
    CHECK(boxrig_ui_editable_document_complete_replacement(
              document,
              &completion) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_application_reconversion_routes_one_replacement(void)
{
    static const char context[] =
        "A\xe6\x97\xa5\xe6\x9c\xac\xf0\x9f\x98\x80"
        "e\xcc\x81\xe2\x80\xa8Z";
    static const char expected_replacement[] =
        "\xe5\xa4\x89\xe6\x9b\xb4";
    static const char expected_terminal[] = "\xe8\xaa\x9e";
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_editable_document *document;
    boxrig_ui_editable_limits limits = boxrig_ui_editable_limits_default();
    boxrig_ui_editable_document_snapshot_input snapshot;
    boxrig_ui_editable_replacement_request replacement;
    boxrig_ui_editable_replacement_completion completion;
    boxrig_win32_input_adapter *adapter = NULL;
    boxrig_ui_view_action_capabilities capabilities;
    boxrig_ui_view_action action;
    WCHAR result_text[] = {0x5909, 0x66f4};
    LRESULT message_result = 0;
    const uint8_t *output = NULL;
    size_t output_length;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig application reconversion test",
        WS_OVERLAPPED,
        0,
        0,
        80,
        60,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    document = boxrig_ui_view_editable_document(view);
    limits.maximum_pending_replacements = 0;
    CHECK(boxrig_ui_editable_document_set_limits(document, limits) ==
          BOXRIG_UI_OK);
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.document_identity = 73;
    snapshot.document_revision = 41;
    snapshot.context = context;
    snapshot.context_length = sizeof(context) - 1;
    snapshot.context_offset = 100;
    snapshot.selection.offset = 101;
    snapshot.selection.length = 13;
    CHECK(boxrig_ui_editable_document_publish(document, &snapshot) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    CHECK(boxrig_win32_input_adapter_set_editable_document(
              adapter,
              document) == 0);

    mock_imm_active = true;
    mock_reconversion_supported = true;
    mock_reconversion_adjust_query = true;
    mock_reconversion_query_rejected = false;
    mock_reconversion_set_rejected = false;
    mock_reconversion_call_count = 0;
    memset(&mock_imm, 0, sizeof(mock_imm));
    capabilities =
        boxrig_win32_input_adapter_view_action_capabilities(adapter);
    CHECK(capabilities.reconversion);
    action.kind = BOXRIG_UI_VIEW_ACTION_RECONVERT;
    action.clipboard_target = 0;
    CHECK(boxrig_ui_view_action_supported(&capabilities, &action));
    CHECK(boxrig_win32_input_adapter_apply_view_action(
              adapter,
              view,
              &action) == BOXRIG_UI_OK);
    CHECK(mock_reconversion_call_count == 2);
    CHECK(mock_reconversion_calls[0] == SCS_QUERYRECONVERTSTRING);
    CHECK(mock_reconversion_calls[1] == SCS_SETRECONVERTSTRING);
    CHECK(mock_reconversion_sizes[0] == mock_reconversion_sizes[1]);
    CHECK(mock_reconversion_sizes[0] ==
          sizeof(RECONVERTSTRING) + 9 * sizeof(WCHAR));
    CHECK(boxrig_ui_editable_document_reconversion_state(document) ==
          BOXRIG_UI_EDITABLE_RECONVERSION_ACTIVE);

    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_STARTCOMPOSITION,
              0,
              0,
              &message_result) == 1);
    mock_imm.result_text = result_text;
    mock_imm.result_count = sizeof(result_text) / sizeof(result_text[0]);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_COMPOSITION,
              0,
              GCS_RESULTSTR,
              &message_result) == 1);
    CHECK(!boxrig_ui_editable_document_take_replacement(
        document,
        &replacement));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_win32_input_adapter_blocked(adapter));
    CHECK(boxrig_win32_input_adapter_retry(adapter) == 0);
    CHECK(boxrig_win32_input_adapter_retry(adapter) == 0);
    CHECK(!boxrig_ui_editable_document_take_replacement(
        document,
        &replacement));

    limits.maximum_pending_replacements = 1;
    CHECK(boxrig_ui_editable_document_set_limits(document, limits) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_win32_input_adapter_retry(adapter) == 0);
    CHECK(boxrig_ui_editable_document_take_replacement(
        document,
        &replacement));
    CHECK(replacement.document_identity == 73);
    CHECK(replacement.base_document_revision == 41);
    CHECK(replacement.range.offset == 104);
    CHECK(replacement.range.length == 10);
    CHECK(replacement.replacement_length ==
          sizeof(expected_replacement) - 1);
    CHECK(memcmp(
              replacement.replacement,
              expected_replacement,
              sizeof(expected_replacement) - 1) == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_CHAR,
              0x8a9e,
              0,
              &message_result) == 1);
    CHECK(!boxrig_ui_editable_document_take_replacement(
        document,
        &replacement));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    memset(&completion, 0, sizeof(completion));
    completion.request_id = replacement.request_id;
    completion.outcome = BOXRIG_UI_EDITABLE_REPLACEMENT_ACCEPTED;
    completion.new_document_revision = 42;
    CHECK(boxrig_ui_editable_document_complete_replacement(
              document,
              &completion) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_input_adapter_retry(adapter) == 0);
    CHECK(!boxrig_win32_input_adapter_blocked(adapter));

    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_STARTCOMPOSITION,
              0,
              0,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_CHAR,
              0x8a9e,
              0,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_ENDCOMPOSITION,
              0,
              0,
              &message_result) == 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected_terminal) - 1);
    CHECK(memcmp(output, expected_terminal, output_length) == 0);

    mock_imm_active = false;
    mock_reconversion_supported = false;
    mock_reconversion_adjust_query = false;
    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_reconversion_cancellation_discards_late_results(void)
{
    static const char context[] =
        "A\xe6\x97\xa5\xe6\x9c\xac\xf0\x9f\x98\x80"
        "e\xcc\x81\xe2\x80\xa8Z";
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_editable_document *document;
    boxrig_ui_editable_limits limits = boxrig_ui_editable_limits_default();
    boxrig_ui_editable_document_snapshot_input snapshot;
    boxrig_ui_editable_replacement_request replacement;
    boxrig_win32_input_adapter *adapter = NULL;
    boxrig_ui_view_action action;
    WCHAR result_text[] = {0x518d};
    WCHAR ordinary_text[] = {L'x'};
    MSG process_key_message;
    LRESULT message_result = 0;
    const uint8_t *output = NULL;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig reconversion cancellation test",
        WS_OVERLAPPED,
        0,
        0,
        80,
        60,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    document = boxrig_ui_view_editable_document(view);
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.document_identity = 91;
    snapshot.document_revision = 41;
    snapshot.context = context;
    snapshot.context_length = sizeof(context) - 1;
    snapshot.context_offset = 100;
    snapshot.selection.offset = 101;
    snapshot.selection.length = 13;
    CHECK(boxrig_ui_editable_document_publish(document, &snapshot) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    CHECK(boxrig_win32_input_adapter_set_editable_document(
              adapter,
              document) == 0);

    mock_imm_active = true;
    mock_reconversion_supported = true;
    mock_reconversion_adjust_query = false;
    mock_reconversion_query_rejected = true;
    mock_reconversion_set_rejected = false;
    mock_reconversion_call_count = 0;
    mock_notify_ime_calls = 0;
    memset(&mock_imm, 0, sizeof(mock_imm));
    action.kind = BOXRIG_UI_VIEW_ACTION_RECONVERT;
    action.clipboard_target = 0;
    CHECK(boxrig_win32_input_adapter_apply_view_action(
              adapter,
              view,
              &action) == BOXRIG_UI_UNAVAILABLE);
    CHECK(mock_reconversion_call_count == 1);
    CHECK(boxrig_ui_editable_document_reconversion_state(document) ==
          BOXRIG_UI_EDITABLE_RECONVERSION_NONE);
    CHECK(!boxrig_ui_editable_document_take_replacement(
        document,
        &replacement));
    CHECK(boxrig_win32_input_adapter_composition_start(adapter) == 0);
    CHECK(boxrig_win32_input_adapter_composition_commit_utf16(
              adapter,
              ordinary_text,
              sizeof(ordinary_text) / sizeof(ordinary_text[0])) == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(output[0] == 'x');
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    mock_reconversion_query_rejected = false;
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_RECONVERTSTRING,
              0,
              &message_result) == 1);
    CHECK(message_result > 0);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_STARTCOMPOSITION,
              0,
              0,
              &message_result) == 1);
    memset(&process_key_message, 0, sizeof(process_key_message));
    process_key_message.hwnd = window;
    process_key_message.message = WM_KEYDOWN;
    process_key_message.wParam = VK_PROCESSKEY;
    mock_imm_virtual_key = VK_ESCAPE;
    CHECK(boxrig_win32_input_adapter_pretranslate_message(
              adapter,
              &process_key_message) == 1);
    CHECK(boxrig_ui_editable_document_reconversion_state(document) ==
          BOXRIG_UI_EDITABLE_RECONVERSION_NONE);
    CHECK(!boxrig_ui_editable_document_take_replacement(
        document,
        &replacement));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(mock_notify_ime_calls != 0);

    mock_notify_ime_calls = 0;
    mock_reconversion_call_count = 0;
    CHECK(boxrig_win32_input_adapter_apply_view_action(
              adapter,
              view,
              &action) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_STARTCOMPOSITION,
              0,
              0,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_KEYDOWN,
              VK_ESCAPE,
              0,
              &message_result) == 1);
    CHECK(boxrig_ui_editable_document_reconversion_state(document) ==
          BOXRIG_UI_EDITABLE_RECONVERSION_NONE);
    CHECK(!boxrig_ui_editable_document_take_replacement(
        document,
        &replacement));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(mock_notify_ime_calls != 0);

    mock_notify_ime_calls = 0;
    mock_reconversion_call_count = 0;
    CHECK(boxrig_win32_input_adapter_apply_view_action(
              adapter,
              view,
              &action) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_STARTCOMPOSITION,
              0,
              0,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KILLFOCUS,
              0,
              0,
              &message_result) == 1);
    CHECK(boxrig_ui_editable_document_reconversion_state(document) ==
          BOXRIG_UI_EDITABLE_RECONVERSION_NONE);
    CHECK(!boxrig_ui_editable_document_take_replacement(
        document,
        &replacement));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(mock_notify_ime_calls != 0);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_SETFOCUS,
              0,
              0,
              &message_result) == 1);

    limits.maximum_pending_replacements = 0;
    CHECK(boxrig_ui_editable_document_set_limits(document, limits) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_RECONVERTSTRING,
              0,
              &message_result) == 1);
    CHECK(message_result > 0);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_STARTCOMPOSITION,
              0,
              0,
              &message_result) == 1);
    mock_imm.result_text = result_text;
    mock_imm.result_count = sizeof(result_text) / sizeof(result_text[0]);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_COMPOSITION,
              0,
              GCS_RESULTSTR,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_blocked(adapter));
    CHECK(!boxrig_ui_editable_document_take_replacement(
        document,
        &replacement));
    snapshot.document_revision = 42;
    CHECK(boxrig_ui_editable_document_publish(document, &snapshot) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_editable_document_reconversion_state(document) ==
          BOXRIG_UI_EDITABLE_RECONVERSION_STALE);
    CHECK(boxrig_win32_input_adapter_retry(adapter) == 0);
    CHECK(boxrig_ui_editable_document_reconversion_state(document) ==
          BOXRIG_UI_EDITABLE_RECONVERSION_NONE);
    CHECK(!boxrig_win32_input_adapter_blocked(adapter));
    CHECK(!boxrig_ui_editable_document_take_replacement(
        document,
        &replacement));
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_COMPOSITION,
              0,
              GCS_RESULTSTR,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_CHAR,
              0x518d,
              0,
              &message_result) == 1);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_ENDCOMPOSITION,
              0,
              0,
              &message_result) == 1);
    CHECK(!boxrig_ui_editable_document_take_replacement(
        document,
        &replacement));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    limits.maximum_pending_replacements = 1;
    CHECK(boxrig_ui_editable_document_set_limits(document, limits) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_IME_REQUEST,
              IMR_RECONVERTSTRING,
              0,
              &message_result) == 1);
    CHECK(message_result > 0);
    boxrig_win32_input_adapter_destroy(adapter);
    adapter = NULL;
    CHECK(boxrig_ui_editable_document_reconversion_state(document) ==
          BOXRIG_UI_EDITABLE_RECONVERSION_NONE);
    CHECK(!boxrig_ui_editable_document_take_replacement(
        document,
        &replacement));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    mock_imm_active = false;
    mock_reconversion_supported = false;
    mock_reconversion_query_rejected = false;
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_stale_final_imm_preedit_clears_on_backspace_release(void)
{
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    boxrig_composition *composition;
    boxrig_composition_view view;
    WCHAR character[] = {0x304d};
    BYTE attribute[] = {ATTR_INPUT};
    DWORD clauses[] = {0, 1};
    LRESULT message_result = 0;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig stale IMM preedit test",
        WS_OVERLAPPED,
        0,
        0,
        80,
        60,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    composition = boxrig_win32_input_adapter_composition(adapter);
    CHECK(composition != NULL);
    CHECK(boxrig_win32_input_adapter_composition_start(adapter) == 0);
    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              character,
              sizeof(character) / sizeof(character[0]),
              attribute,
              sizeof(attribute) / sizeof(attribute[0]),
              clauses,
              sizeof(clauses) / sizeof(clauses[0]),
              1) == 0);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYUP,
              'I',
              key_parameter('I', false, true),
              &message_result) == 1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active);
    CHECK(view.text_length == 3);

    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYUP,
              VK_BACK,
              key_parameter(VK_BACK, false, true),
              &message_result) == 1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);
    CHECK(view.text_length == 0);
    CHECK(view.last_event == BOXRIG_COMPOSITION_CANCEL);

    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}

static bool test_escape_cancels_stale_unconverted_imm_preedit(void)
{
    HWND window;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_input_adapter *adapter = NULL;
    boxrig_composition *composition;
    boxrig_composition_view view;
    WCHAR text[] = {0x3068, 0x308a};
    BYTE converted_attributes[] = {
        ATTR_TARGET_CONVERTED,
        ATTR_CONVERTED};
    BYTE input_attributes[] = {ATTR_INPUT, ATTR_INPUT};
    DWORD clauses[] = {0, 1, 2};
    LRESULT message_result = 0;
    const uint8_t *output = NULL;

    window = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig stale Escape IMM preedit test",
        WS_OVERLAPPED,
        0,
        0,
        80,
        60,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    CHECK(window != NULL);
    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_input_adapter_create(
              window,
              terminal,
              10,
              20,
              1024,
              config.output_capacity,
              &adapter) == 0);
    composition = boxrig_win32_input_adapter_composition(adapter);
    CHECK(composition != NULL);
    CHECK(boxrig_win32_input_adapter_composition_start(adapter) == 0);
    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              text,
              sizeof(text) / sizeof(text[0]),
              converted_attributes,
              sizeof(converted_attributes) /
                  sizeof(converted_attributes[0]),
              clauses,
              sizeof(clauses) / sizeof(clauses[0]),
              1) == 0);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYUP,
              'I',
              key_parameter('I', false, true),
              &message_result) == 1);

    CHECK(boxrig_win32_input_adapter_composition_update_utf16(
              adapter,
              text,
              sizeof(text) / sizeof(text[0]),
              input_attributes,
              sizeof(input_attributes) / sizeof(input_attributes[0]),
              clauses,
              sizeof(clauses) / sizeof(clauses[0]),
              2) == 0);
    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYUP,
              VK_ESCAPE,
              key_parameter(VK_ESCAPE, false, true),
              &message_result) == 1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active);
    CHECK(view.text_length == 6);
    CHECK(view.segment_count == 2);
    CHECK((view.segments[0].feedback &
           (BOXRIG_COMPOSITION_FEEDBACK_TARGET |
            BOXRIG_COMPOSITION_FEEDBACK_CONVERTED)) == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    CHECK(boxrig_win32_input_adapter_process_message(
              adapter,
              WM_KEYUP,
              VK_ESCAPE,
              key_parameter(VK_ESCAPE, false, true),
              &message_result) == 1);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);
    CHECK(view.text_length == 0);
    CHECK(view.last_event == BOXRIG_COMPOSITION_CANCEL);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    boxrig_win32_input_adapter_destroy(adapter);
    boxrig_terminal_destroy(terminal);
    CHECK(DestroyWindow(window));
    return true;
}


static bool test_terminal_io_configuration(void)
{
    HWND window = CreateWindowExW(0, L"STATIC", L"boxrig terminal IO settings",
        WS_OVERLAPPED, 0, 0, 80, 60, NULL, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(window != NULL);
    for (unsigned encoding = 0; encoding < 2; encoding++) {

        const uint8_t mapped[] = {0x1b, 'U', 0};
        const boxrig_key_mapping mapping = {BOXRIG_KEY_UP, 0, mapped, sizeof(mapped)};
        boxrig_terminal_config config = boxrig_terminal_config_default();
        config.encoding = (boxrig_encoding)encoding;
        config.enter_newline = BOXRIG_NEWLINE_LF;
        config.paste_newline = BOXRIG_NEWLINE_CRLF;
        config.key_mappings = &mapping;
        config.key_mapping_count = 1;
        boxrig_terminal *terminal = NULL;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);

        boxrig_win32_input_adapter *adapter = NULL;
        CHECK(boxrig_win32_input_adapter_create(window, terminal,
                  10, 20, 1024, config.output_capacity, &adapter) == 0);
        const WCHAR committed[] = {0x65e5, 0x672c};
        CHECK(boxrig_win32_input_adapter_composition_start(adapter) == 0);
        CHECK(boxrig_win32_input_adapter_composition_commit_utf16(adapter, committed, 2) == 0);
        const WPARAM keys[] = {VK_UP, VK_RETURN};
        LRESULT result;
        for (unsigned index = 0; index < 2; index++) {
            CHECK(boxrig_win32_input_adapter_process_message(adapter, WM_KEYDOWN,
                      keys[index], key_parameter(keys[index], false, false), &result) == 1);
            CHECK(boxrig_win32_input_adapter_process_message(adapter, WM_KEYUP,
                      keys[index], key_parameter(keys[index], false, true), &result) == 1);
        }
        CHECK(boxrig_win32_input_adapter_queue_paste(adapter, (const uint8_t *)"日本\n", 7) == 0);

        const uint8_t utf8_expected[] = "日本\033U\0\n日本\r\n";
        const uint8_t sjis_expected[] = "\x93\xfa\x96\x7b\033U\0\n\x93\xfa\x96\x7b\r\n";
        const uint8_t *expected = encoding ? sjis_expected : utf8_expected;
        size_t length = encoding ? sizeof(sjis_expected) - 1 : sizeof(utf8_expected) - 1;
        const uint8_t *output;
        CHECK(boxrig_terminal_output_peek(terminal, &output) == length);
        CHECK(memcmp(output, expected, length) == 0);

        boxrig_win32_input_adapter_destroy(adapter);
        boxrig_terminal_destroy(terminal);
    }
    CHECK(DestroyWindow(window));
    return true;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 2 && wcscmp(argv[1], L"--terminal-io-only") == 0) {
        if (!test_terminal_io_configuration()) return 1;
        puts("PASS native Win32 UTF-8/Shift_JIS IME, key and paste settings");
        return 0;
    }
    static const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"terminal UTF-8/Shift_JIS IME, key and paste settings", test_terminal_io_configuration},
        {"application key routing captures the key series",
         test_application_key_routing_captures_the_key_series},
        {"key identity preserves PC-101 base layout",
         test_key_identity_preserves_pc101_base_layout},
        {"legacy control input reaches terminal",
         test_legacy_control_input_reaches_terminal},
        {"application mouse routing captures the drag series",
         test_application_mouse_routing_captures_the_drag_series},
        {"key focus and mouse messages", test_key_focus_and_mouse_messages},
        {"pending key retries atomically", test_pending_key_retries_atomically},
        {"streamed paste normalizes and retries",
         test_streamed_paste_normalizes_and_retries},
        {"Unicode clipboard paste", test_unicode_clipboard_paste},
        {"clipboard copy failure boundaries",
         test_clipboard_copy_failure_boundaries},
        {"IMM updates follow the common composition fixture",
         test_imm_updates_follow_the_common_composition_fixture},
        {"IMM clause snapshots preserve surrogate boundaries",
         test_imm_clause_snapshots_and_surrogate_boundaries},
        {"IMM optional metadata and malformed clauses",
         test_imm_optional_metadata_and_malformed_clauses},
        {"IMM candidate forms report rejection and retry",
         test_imm_candidate_forms_report_rejection_and_retry},
        {"IMM result and following preedit survive backpressure",
         test_imm_result_and_following_preedit_survive_backpressure},
        {"WM_IME_CHAR commits every UTF-16 character",
         test_wm_ime_char_commits_every_utf16_character},
        {"unsupported IMM reconversion requests are inert",
         test_unsupported_imm_reconversion_requests_are_inert},
        {"IMM document feed distinguishes context from reconversion",
         test_imm_document_feed_distinguishes_context_from_reconversion},
        {"IMM reconversion requests map UTF-16 ranges",
         test_imm_reconversion_requests_map_utf16_ranges},
        {"application reconversion routes one replacement",
         test_application_reconversion_routes_one_replacement},
        {"reconversion cancellation discards late results",
         test_reconversion_cancellation_discards_late_results},
        {"stale final IMM preedit clears on Backspace release",
         test_stale_final_imm_preedit_clears_on_backspace_release},
        {"Escape cancels stale unconverted IMM preedit",
         test_escape_cancels_stale_unconverted_imm_preedit},
    };
    size_t index;

    for (index = 0; index < sizeof(tests) / sizeof(tests[0]); index++) {
        if (!tests[index].run()) {
            fprintf(stderr, "Test failed: %s\n", tests[index].name);
            return 1;
        }
        printf("PASS %s\n", tests[index].name);
    }
    return 0;
}
