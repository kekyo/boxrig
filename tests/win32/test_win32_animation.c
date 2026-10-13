/* boxrig - Under MIT. */
#include "boxrig/win32/component.h"
#include "boxrig/ui/animation.h"

#include <stdlib.h>
#include <stdio.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(EXIT_FAILURE); \
} } while (0)

static uint64_t now = 1000;
static HWND timer_window;
static UINT_PTR timer_id;
static UINT timer_delay;
static bool reject_timer;
static unsigned requests;

ULONGLONG WINAPI __wrap_GetTickCount64(void)
{
    return now;
}

UINT_PTR WINAPI __wrap_SetTimer(HWND window, UINT_PTR id, UINT delay, TIMERPROC callback)
{
    CHECK(window != NULL && id != 0 && callback == NULL);
    if (reject_timer) return 0;
    timer_window = window;
    timer_id = id;
    timer_delay = delay;
    return 1;
}

BOOL WINAPI __wrap_KillTimer(HWND window, UINT_PTR id)
{
    CHECK(window == timer_window);
    if (id == timer_id) timer_id = 0;
    return TRUE;
}

/* The scheduler object is compiled with -mnop-fun-dllimport for this test,
 * allowing ordinary linker wrappers without assembly-specific symbol names.
 * Dispatch still uses the real native window procedure. */
static void request_frame(void *context)
{
    (void)context;
    requests++;
}

static void test_native_input_failure(unsigned kind)
{
    reject_timer = false;
    now += 1000;
    HWND parent = CreateWindowExW(0, L"STATIC", L"failure host",
        WS_OVERLAPPEDWINDOW, 0, 0, 320, 160, NULL, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(parent != NULL);
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_win32_terminal_component *component = NULL;
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    tc.rows = 2;
    tc.columns = 8;
    boxrig_win32_terminal_component_config config = boxrig_win32_terminal_component_config_default();
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    CHECK(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_create_child(GetModuleHandleW(NULL),
        parent, model, &config, &component) == BOXRIG_UI_OK);
    boxrig_font_effective_metrics metrics;
    CHECK(boxrig_win32_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
    boxrig_ui_component_allocation allocation = {0};
    allocation.width = metrics.cell_width * (unsigned)tc.columns;
    allocation.height = metrics.cell_height * (unsigned)tc.rows;
    boxrig_ui_layout_result layout;
    CHECK(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    HWND input = boxrig_win32_terminal_component_input_window(component);
    SendMessageW(input, WM_SETFOCUS, 0, 0);
    if (kind == 2) SendMessageW(input, WM_KILLFOCUS, 0, 0);
    boxrig_ui_component_frame_result frame;
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_set_frame_request_callback(component, request_frame, NULL) == BOXRIG_UI_OK);
    requests = 0;
    now += 100;
    reject_timer = true;
    if (kind == 0) SendMessageW(input, WM_KEYDOWN, VK_LEFT, 0);
    else if (kind == 1) CHECK(boxrig_win32_terminal_component_queue_paste(component, (const uint8_t *)"x", 1) != BOXRIG_UI_OK);
    else SendMessageW(input, WM_SETFOCUS, 0, 0);
    boxrig_ui_component_diagnostic diagnostic;
    CHECK(boxrig_win32_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.failed && !diagnostic.running);
    CHECK(diagnostic.failure_reason == BOXRIG_UI_COMPONENT_FAILURE_ANIMATION);
    CHECK(requests != 0);
    reject_timer = false;
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_UNAVAILABLE);
    boxrig_win32_terminal_component_destroy(component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    DestroyWindow(parent);
}

int main(void)
{
    HWND parent = CreateWindowExW(0, L"STATIC", L"animation host",
        WS_OVERLAPPEDWINDOW, 0, 0, 320, 160, NULL, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(parent != NULL);
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    tc.rows = 2;
    tc.columns = 8;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_win32_terminal_component *component = NULL, *second = NULL;
    boxrig_win32_terminal_component_config config =
        boxrig_win32_terminal_component_config_default();
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    CHECK(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_create_child(GetModuleHandleW(NULL),
        parent, model, &config, &component) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, now) == BOXRIG_STATUS_BUSY);
    CHECK(boxrig_win32_terminal_component_set_frame_request_callback(
        component, request_frame, NULL) == BOXRIG_UI_OK);
    boxrig_ui_component_allocation allocation = {0};
    boxrig_font_effective_metrics metrics;
    CHECK(boxrig_win32_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
    allocation.width = metrics.cell_width * (unsigned)tc.columns;
    allocation.height = metrics.cell_height * (unsigned)tc.rows;
    boxrig_ui_layout_result layout;
    CHECK(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    HWND input = boxrig_win32_terminal_component_input_window(component);
    LRESULT result;
    CHECK(boxrig_win32_terminal_component_dispatch_message(component, input,
        WM_SETFOCUS, 0, 0, &result) != BOXRIG_UI_COMPONENT_EVENT_FAILURE);
    boxrig_ui_view *view = boxrig_win32_terminal_component_view(component);
    CHECK(timer_id != 0 && timer_delay == 500);
    CHECK(timer_window != parent && timer_window != input);
    CHECK(boxrig_ui_view_cursor_blink_visible(view));
    boxrig_ui_component_frame_result frame;
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    UINT_PTR old_id = timer_id;
    now = 1500;
    requests = 0;
    SendMessageW(timer_window, WM_TIMER, old_id, 0);
    CHECK(!boxrig_ui_view_cursor_blink_visible(view) && requests != 0);
    requests = 0;
    SendMessageW(timer_window, WM_TIMER, old_id, 0);
    CHECK(!boxrig_ui_view_cursor_blink_visible(view) && requests == 0);
    CHECK(boxrig_win32_terminal_component_dispatch_message(component, input,
        WM_KEYDOWN, VK_LEFT, 0, &result) != BOXRIG_UI_COMPONENT_EVENT_FAILURE);
    CHECK(boxrig_ui_view_cursor_blink_visible(view));
    now = 2000;
    SendMessageW(timer_window, WM_TIMER, timer_id, 0);
    CHECK(!boxrig_ui_view_cursor_blink_visible(view));
    CHECK(boxrig_win32_terminal_component_queue_paste(component,
        (const uint8_t *)"x", 1) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_cursor_blink_visible(view));
    boxrig_composition *composition = boxrig_win32_terminal_component_composition(component);
    CHECK(boxrig_composition_focus_changed(composition, true) == BOXRIG_COMPOSITION_OK);
    boxrig_composition_event event = {0};
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = "a";
    event.text_length = 1;
    CHECK(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    CHECK(timer_id == 0 && !frame.has_animation_deadline);
    event = (boxrig_composition_event){0};
    event.type = BOXRIG_COMPOSITION_CANCEL;
    CHECK(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    CHECK(timer_id != 0 && frame.has_animation_deadline);
    CHECK(boxrig_ui_view_cursor_blink_visible(view));
    old_id = timer_id;
    CHECK(boxrig_win32_terminal_component_set_visible(component, false) == BOXRIG_UI_OK);
    CHECK(timer_id == 0);
    now = 2500;
    SendMessageW(timer_window, WM_TIMER, old_id, 0);
    CHECK(timer_id == 0);
    CHECK(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_cursor_blink_visible(view) && timer_id != 0);
    old_id = timer_id;
    boxrig_ui_blink_config blink = boxrig_ui_blink_config_default();
    blink.cursor.visible_milliseconds = 100;
    reject_timer = true;
    CHECK(boxrig_ui_view_set_blink_config(view, &blink) == BOXRIG_UI_UNAVAILABLE);
    CHECK(timer_id == old_id);
    CHECK(boxrig_ui_view_get_blink_config(view, &blink) == BOXRIG_UI_OK);
    CHECK(blink.cursor.visible_milliseconds == 500);
    reject_timer = false;
    config.attachment = BOXRIG_UI_COMPONENT_PRESENTATION_ONLY;
    CHECK(boxrig_win32_terminal_component_create_child(GetModuleHandleW(NULL),
        parent, model, &config, &second) == BOXRIG_UI_OK);
    HWND notification = timer_window;
    now = 3000;
    reject_timer = true;
    SendMessageW(timer_window, WM_TIMER, timer_id, 0);
    boxrig_ui_component_diagnostic diagnostic;
    CHECK(boxrig_win32_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.failed && !diagnostic.running);
    CHECK(diagnostic.failure_reason == BOXRIG_UI_COMPONENT_FAILURE_ANIMATION);
    CHECK(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_win32_terminal_component_quiesce(component) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, now) == BOXRIG_STATUS_BUSY);
    CHECK(IsWindow(notification));
    CHECK(boxrig_win32_terminal_component_quiesce(second) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, now) == BOXRIG_STATUS_OK);
    CHECK(!IsWindow(notification));
    boxrig_win32_terminal_component_destroy(second);
    boxrig_win32_terminal_component_destroy(component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    DestroyWindow(parent);
    for (unsigned kind = 0; kind < 3; kind++) test_native_input_failure(kind);
    puts("PASS Win32 automatic cursor phases, input, IME, stale timers, failures and shared lifetime");
    return 0;
}
