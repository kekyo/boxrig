/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifdef BOXRIG_TEST_D3D11_COMPONENT
#define COBJMACROS
#include <initguid.h>
#include "boxrig/win32/d3d11_component.h"
#endif
#include "boxrig/win32.h"
#ifdef BOXRIG_TEST_OPENGL_COMPONENT
#include "boxrig/win32/opengl_component.h"
#endif
#include "../frontend_component_fixture.h"
#include "../../examples/win32/external_scrollbar.h"
#include "../external_scrollbar_example.h"

#include <windows.h>

#include <limits.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef BOXRIG_TEST_OPENGL_COMPONENT
#include "opengl_component_capture.h"
#elif defined(BOXRIG_TEST_D3D11_COMPONENT)
#include "d3d11_component_capture.h"
#endif

static uint64_t fixture_presentation_hash;
static uint64_t fixture_presentation_generation;
static bool fixture_presentation_captured;

typedef struct win32_fixture_host {
    HINSTANCE instance;
    HWND parent;
} win32_fixture_host;

static void *create_host(void)
{
    win32_fixture_host *host = calloc(1, sizeof(*host));

    if (host == NULL) {
        return NULL;
    }
    host->instance = GetModuleHandleW(NULL);
    host->parent = CreateWindowExW(
        0,
        L"STATIC",
        L"boxrig component fixture",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        320,
        160,
        NULL,
        NULL,
        host->instance,
        NULL);
    if (host->parent == NULL) {
        free(host);
        return NULL;
    }
    ShowWindow(host->parent, SW_SHOWNA);
    fixture_presentation_hash = 0;
    fixture_presentation_generation = 0;
    fixture_presentation_captured = false;
    return host;
}

static void destroy_host(void *opaque_host)
{
    win32_fixture_host *host = opaque_host;

    if (host == NULL) {
        return;
    }
    if (host->parent != NULL) {
        (void)DestroyWindow(host->parent);
    }
    free(host);
}

static boxrig_ui_status create_component(
    void *opaque_host,
    boxrig_ui_model *model,
    boxrig_ui_component_attachment attachment,
    void **component_out)
{
    win32_fixture_host *host = opaque_host;
    boxrig_win32_terminal_component_config config =
        boxrig_win32_terminal_component_config_default();
    boxrig_win32_terminal_component *component = NULL;
    boxrig_ui_status status;

    config.attachment = attachment;
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    config.paste_chunk_size = 4;
#ifdef BOXRIG_TEST_D3D11_COMPONENT
    const boxrig_win32_d3d11_config graphics = boxrig_win32_d3d11_config_default();
    status = boxrig_win32_d3d11_terminal_component_create_child(
        host->instance, host->parent, model, &config, &graphics, &component);
#else
#ifdef BOXRIG_TEST_OPENGL_COMPONENT
    status = boxrig_win32_opengl_terminal_component_create_child(
#else
    status = boxrig_win32_terminal_component_create_child(
#endif
        host->instance,
        host->parent,
        model,
        &config,
        &component);
#endif
    *component_out = component;
    return status;
}

static void destroy_component(void *component)
{
    boxrig_win32_terminal_component_destroy(component);
}

static boxrig_ui_status quiesce_component(void *component)
{
    boxrig_ui_view *view = boxrig_win32_terminal_component_view(component);
    if (boxrig_ui_view_set_focused(view, true) != BOXRIG_UI_OK) return BOXRIG_UI_UNAVAILABLE;
    boxrig_ui_status status = boxrig_win32_terminal_component_quiesce(component);
    return status == BOXRIG_UI_OK && !boxrig_ui_view_focused(view)
        ? BOXRIG_UI_OK : BOXRIG_UI_UNAVAILABLE;
}

static boxrig_ui_view *component_view(void *component)
{
    return boxrig_win32_terminal_component_view(component);
}

static bool get_effective_metrics(
    void *component,
    boxrig_font_effective_metrics *metrics_out)
{
    return boxrig_win32_terminal_component_get_effective_metrics(
               component,
               metrics_out) == BOXRIG_FONT_CATALOG_OK;
}

static boxrig_ui_status allocate_component(
    void *component,
    const boxrig_ui_component_allocation *allocation,
    boxrig_ui_layout_result *layout_out)
{
    return boxrig_win32_terminal_component_allocate(
        component,
        allocation,
        layout_out);
}

static boxrig_ui_status set_preferred_grid(
    void *component,
    size_t rows,
    size_t columns)
{
    return boxrig_win32_terminal_component_set_preferred_grid(
        component,
        rows,
        columns);
}

static bool get_preferred_grid(
    void *component,
    size_t *rows_out,
    size_t *columns_out)
{
    return boxrig_win32_terminal_component_get_preferred_grid(
               component,
               rows_out,
               columns_out) == BOXRIG_UI_OK;
}

static boxrig_ui_size_command_state get_size_command_state(void *component)
{
    boxrig_ui_size_command_state state = BOXRIG_UI_SIZE_COMMAND_IDLE;

    (void)boxrig_win32_terminal_component_get_size_command_state(
        component,
        &state);
    return state;
}

static bool take_layout_action(
    void *component,
    boxrig_ui_layout_action *action_out)
{
    return boxrig_win32_terminal_component_take_layout_action(
        component, action_out);
}

static boxrig_ui_status complete_size_command(
    void *component,
    uint64_t revision,
    boxrig_ui_size_outcome outcome,
    boxrig_ui_fixed allocation_width,
    boxrig_ui_fixed allocation_height,
    bool *applied_out,
    boxrig_ui_layout_result *layout_out)
{
    return boxrig_win32_terminal_component_complete_size_command(
        component,
        revision,
        outcome,
        allocation_width,
        allocation_height,
        applied_out,
        layout_out);
}

static boxrig_ui_status complete_grid_resize(
    void *component,
    uint64_t revision,
    bool succeeded)
{
    return boxrig_win32_terminal_component_complete_grid_resize(
        component, revision, succeeded);
}

static boxrig_ui_status set_visible(void *component, bool visible)
{
    return boxrig_win32_terminal_component_set_visible(component, visible);
}

static boxrig_ui_status set_frame_request_callback(
    void *component,
    boxrig_ui_frame_request_callback callback,
    void *context)
{
    return boxrig_win32_terminal_component_set_frame_request_callback(
        component,
        callback,
        context);
}

static boxrig_ui_status frame_component(
    void *component,
    boxrig_ui_component_frame_result *result_out)
{
#ifdef BOXRIG_TEST_D3D11_COMPONENT
    /* The fixture observes complete frames. Respect the real latency event
     * instead of assuming a synchronous presenter or sleeping between draws. */
    boxrig_ui_status status;
    for (;;) {
        HANDLE activity;
        status = boxrig_win32_terminal_component_frame(component, result_out);
        if (status != BOXRIG_UI_OK || !result_out->frame_pending) return status;
        activity = boxrig_win32_terminal_component_activity_handle(component);
        if (activity == NULL) return status;
        if (WaitForSingleObject(activity, 30000) != WAIT_OBJECT_0)
            return BOXRIG_UI_UNAVAILABLE;
        status = boxrig_win32_terminal_component_dispatch_activity(component);
        if (status != BOXRIG_UI_OK) return status;
    }
#else
    return boxrig_win32_terminal_component_frame(component, result_out);
#endif
}

static boxrig_ui_status queue_paste(
    void *component,
    const uint8_t *bytes,
    size_t length)
{
    return boxrig_win32_terminal_component_queue_paste(
        component,
        bytes,
        length);
}

static LPARAM key_parameter(
    unsigned int virtual_key,
    bool repeat,
    bool released)
{
    unsigned int scan_code = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
    LPARAM parameter = 1 | ((LPARAM)scan_code << 16);

    if (repeat || released) {
        parameter |= (LPARAM)1 << 30;
    }
    if (released) {
        parameter |= (LPARAM)1 << 31;
    }
    return parameter;
}

static bool run_native_input_transcript(
    void *opaque_component,
    const boxrig_test_frontend_input_script *script,
    boxrig_test_frontend_input_observation *observation_out)
{
    boxrig_win32_terminal_component *component = opaque_component;
    HWND window;
    LRESULT result = 0;
    int x;
    int y;
    bool passed = false;
    BYTE saved_keyboard[256];
    BYTE scripted_keyboard[256] = {0};
    bool keyboard_saved = false;

    if (component == NULL || script == NULL || observation_out == NULL ||
        script->x < 0 || script->y < 0 ||
        script->x % BOXRIG_UI_FIXED_ONE != 0 ||
        script->y % BOXRIG_UI_FIXED_ONE != 0 ||
        script->x / BOXRIG_UI_FIXED_ONE > SHRT_MAX ||
        script->y / BOXRIG_UI_FIXED_ONE > SHRT_MAX) {
        return false;
    }
    /* Synthetic messages must not inherit the physical desktop lock keys.
     * This thread-local table leaves the user's keyboard state untouched. */
    if (!GetKeyboardState(saved_keyboard)) return false;
    keyboard_saved = true;
    if (!SetKeyboardState(scripted_keyboard)) goto cleanup;
    window = boxrig_win32_terminal_component_input_window(component);
    x = (int)(script->x / BOXRIG_UI_FIXED_ONE);
    y = (int)(script->y / BOXRIG_UI_FIXED_ONE);
    if (window == NULL ||
        boxrig_win32_terminal_component_set_key_router(
            component,
            boxrig_test_frontend_capture_key,
            observation_out) != BOXRIG_UI_OK ||
        boxrig_win32_terminal_component_set_mouse_router(
            component,
            boxrig_test_frontend_capture_mouse,
            observation_out) != BOXRIG_UI_OK) {
        goto cleanup;
    }
    if (boxrig_win32_terminal_component_dispatch_message(
            component,
            window,
            WM_SETFOCUS,
            0,
            0,
            &result) != BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
        goto cleanup;
    }
    observation_out->focused_after_enter =
        boxrig_win32_terminal_component_focused(component) &&
        boxrig_ui_view_focused(boxrig_win32_terminal_component_view(component));

    if (boxrig_win32_terminal_component_dispatch_message(
            component,
            window,
            WM_KEYDOWN,
            VK_F1,
            key_parameter(VK_F1, false, false),
            &result) != BOXRIG_UI_COMPONENT_EVENT_HANDLED ||
        boxrig_win32_terminal_component_dispatch_message(
            component,
            window,
            WM_KEYDOWN,
            VK_F1,
            key_parameter(VK_F1, true, false),
            &result) != BOXRIG_UI_COMPONENT_EVENT_HANDLED ||
        boxrig_win32_terminal_component_dispatch_message(
            component,
            window,
            WM_KEYUP,
            VK_F1,
            key_parameter(VK_F1, false, true),
            &result) != BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
        goto cleanup;
    }

    if (boxrig_win32_terminal_component_dispatch_message(
            component,
            window,
            WM_LBUTTONDOWN,
            MK_CONTROL | MK_LBUTTON,
            MAKELPARAM(x, y),
            &result) != BOXRIG_UI_COMPONENT_EVENT_HANDLED ||
        boxrig_win32_terminal_component_dispatch_message(
            component,
            window,
            WM_MOUSEMOVE,
            MK_SHIFT | MK_LBUTTON,
            MAKELPARAM(x, y),
            &result) != BOXRIG_UI_COMPONENT_EVENT_HANDLED ||
        boxrig_win32_terminal_component_dispatch_message(
            component,
            window,
            WM_LBUTTONUP,
            0,
            MAKELPARAM(x, y),
            &result) != BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
        goto cleanup;
    }
    if (boxrig_win32_terminal_component_dispatch_message(
            component,
            window,
            WM_KILLFOCUS,
            0,
            0,
            &result) != BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
        goto cleanup;
    }
    observation_out->focused_after_leave =
        boxrig_win32_terminal_component_focused(component) ||
        boxrig_ui_view_focused(boxrig_win32_terminal_component_view(component));
    passed = true;

cleanup:
    if (boxrig_win32_terminal_component_set_key_router(
            component,
            NULL,
            NULL) != BOXRIG_UI_OK ||
        boxrig_win32_terminal_component_set_mouse_router(
            component,
            NULL,
            NULL) != BOXRIG_UI_OK) {
        passed = false;
    }
    if (keyboard_saved && !SetKeyboardState(saved_keyboard)) passed = false;
    return passed;
}

static boxrig_font_catalog_status register_font_file(
    void *component,
    const char *path,
    size_t path_length,
    boxrig_font_resource_id *resource_id_out)
{
    return boxrig_win32_terminal_component_register_font_file(
        component,
        path,
        path_length,
        0,
        resource_id_out);
}

static boxrig_ui_status set_font_specification(
    void *component,
    const boxrig_font_specification *specification)
{
    return boxrig_win32_terminal_component_set_font_specification(
        component,
        specification);
}

static boxrig_ui_status get_font_specification(
    void *component,
    boxrig_font_specification *specification_out)
{
    return boxrig_win32_terminal_component_get_font_specification(
        component,
        specification_out);
}

static boxrig_font_resource_id primary_font_resource(void *component)
{
    return boxrig_win32_terminal_component_primary_font_resource(component);
}

static uint64_t font_catalog_revision(void *component)
{
    return boxrig_win32_terminal_component_font_catalog_revision(component);
}

static boxrig_ui_status set_line_height(
    void *component,
    float adjustment_dip)
{
    return boxrig_win32_terminal_component_set_line_height_adjustment(
        component,
        adjustment_dip);
}

static boxrig_ui_status get_line_height(
    void *component,
    float *adjustment_dip_out)
{
    return boxrig_win32_terminal_component_get_line_height_adjustment(
        component,
        adjustment_dip_out);
}

static boxrig_ui_status visual_zoom(
    void *component,
    boxrig_font_zoom_direction direction,
    boxrig_font_visual_step_policy policy,
    bool *changed_out)
{
    return boxrig_win32_terminal_component_visual_zoom(
        component,
        direction,
        policy,
        changed_out);
}

static boxrig_ui_status visual_zoom_reset(
    void *component,
    bool *changed_out)
{
    return boxrig_win32_terminal_component_visual_zoom_reset(
        component,
        changed_out);
}

static boxrig_ui_status set_text_direction(
    void *component,
    boxrig_text_direction_policy policy)
{
    return boxrig_win32_terminal_component_set_text_direction(
        component,
        policy);
}

static boxrig_text_direction_policy text_direction(void *component)
{
    return boxrig_win32_terminal_component_text_direction(component);
}

static bool copy_selection(
    void *opaque_host,
    void *opaque_component,
    const uint8_t *expected,
    size_t expected_length)
{
    win32_fixture_host *host = opaque_host;
    boxrig_win32_terminal_component *component = opaque_component;
    boxrig_ui_view_action_capabilities capabilities;
    boxrig_ui_view_action action;
    HGLOBAL handle;
    const WCHAR *text;
    SIZE_T allocation_size;
    size_t expected_native_length;
    size_t destination;
    size_t index;
    bool matched = false;

    capabilities =
        boxrig_win32_terminal_component_view_action_capabilities(component);
    if ((capabilities.copy_targets &
         BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD) == 0) {
        return false;
    }
    action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    if (boxrig_win32_terminal_component_apply_view_action(
            component,
            &action) != BOXRIG_UI_OK ||
        !IsClipboardFormatAvailable(CF_UNICODETEXT) ||
        !OpenClipboard(host->parent)) {
        return false;
    }
    handle = (HGLOBAL)GetClipboardData(CF_UNICODETEXT);
    if (handle != NULL) {
        expected_native_length = expected_length;
        for (index = 0; index < expected_length; index++) {
            if (expected[index] == '\n') {
                expected_native_length++;
            }
        }
        allocation_size = GlobalSize(handle);
        text = GlobalLock(handle);
        if (text != NULL && allocation_size >=
                                (expected_native_length + 1) * sizeof(*text)) {
            matched = text[expected_native_length] == L'\0';
            destination = 0;
            for (index = 0; matched && index < expected_length; index++) {
                matched = expected[index] < 0x80;
                if (matched && expected[index] == '\n') {
                    matched = text[destination++] == L'\r';
                }
                if (matched) {
                    matched = text[destination++] == (WCHAR)expected[index];
                }
            }
            (void)GlobalUnlock(handle);
        }
    }
    (void)CloseClipboard();
    return matched;
}

static boxrig_ui_status set_transparency(
    void *component,
    const boxrig_ui_transparency_config *config)
{
    return boxrig_win32_terminal_component_set_transparency(component, config);
}

static boxrig_ui_status get_transparency(
    void *component,
    boxrig_ui_transparency_state *state_out)
{
    return boxrig_win32_terminal_component_get_transparency(
        component,
        state_out);
}

static uint32_t capabilities(void *component)
{
    return boxrig_win32_terminal_component_capabilities(component);
}

static boxrig_ui_status get_diagnostic(
    void *component,
    boxrig_ui_component_diagnostic *diagnostic_out)
{
    return boxrig_win32_terminal_component_get_diagnostic(
        component,
        diagnostic_out);
}

static boxrig_composition *component_composition(void *component)
{
    return boxrig_win32_terminal_component_composition(component);
}

#if !defined(BOXRIG_TEST_OPENGL_COMPONENT) && !defined(BOXRIG_TEST_D3D11_COMPONENT)
static HDC captured_context;
static HBITMAP captured_bitmap;
static HGDIOBJ captured_previous;

static int release_gdi_capture(void)
{
    int result = 1;
    if (captured_previous != NULL &&
        SelectObject(captured_context, captured_previous) == NULL) result = 0;
    if (captured_bitmap != NULL && !DeleteObject(captured_bitmap)) result = 0;
    if (captured_context != NULL && !DeleteDC(captured_context)) result = 0;
    captured_context = NULL; captured_bitmap = NULL; captured_previous = NULL;
    return result;
}

static HDC capture_gdi_window(HWND window, unsigned int width, unsigned int height)
{
    HDC source = GetDC(window);
    BITMAPINFO description = {0};
    RECT clip;
    void *pixels = NULL;
    bool copied = false;
    int region;
    if (source == NULL) return NULL;
    region = GetClipBox(source, &clip);
    /* Do not turn an occluded/ghost HWND into an apparently valid black image.
     * The former per-pixel window reads rejected unavailable pixels too. */
    if (region != SIMPLEREGION || clip.left > 0 || clip.top > 0 ||
        clip.right < (LONG)width || clip.bottom < (LONG)height) {
        fprintf(stderr, "FAIL visible window capture: region=%d client=%ux%u\n",
            region, width, height);
        (void)ReleaseDC(window, source);
        return NULL;
    }
    description.bmiHeader.biSize = sizeof(description.bmiHeader);
    description.bmiHeader.biWidth = (LONG)width;
    description.bmiHeader.biHeight = -(LONG)height;
    description.bmiHeader.biPlanes = 1;
    description.bmiHeader.biBitCount = 32;
    description.bmiHeader.biCompression = BI_RGB;
    captured_context = CreateCompatibleDC(source);
    captured_bitmap = CreateDIBSection(source, &description, DIB_RGB_COLORS,
        &pixels, NULL, 0);
    if (captured_context != NULL && captured_bitmap != NULL && pixels != NULL) {
        captured_previous = SelectObject(captured_context, captured_bitmap);
        if (captured_previous == HGDI_ERROR) captured_previous = NULL;
        if (captured_previous != NULL) {
            /* Capture the actual window once. Reading each display pixel
             * separately stalls its owning message thread for seconds. */
            copied = BitBlt(captured_context, 0, 0, (int)width, (int)height,
                source, 0, 0, SRCCOPY) && GdiFlush();
        }
    }
    (void)ReleaseDC(window, source);
    if (!copied) { (void)release_gdi_capture(); return NULL; }
    return captured_context;
}
#endif

static int release_presentation_context(HWND window, HDC context)
{
#if defined(BOXRIG_TEST_OPENGL_COMPONENT) || defined(BOXRIG_TEST_D3D11_COMPONENT)
    (void)window; (void)context;
    return release_gpu_capture();
#else
    (void)window; (void)context;
    return release_gdi_capture();
#endif
}

static bool observe_presentation(
    void *opaque_component,
    const boxrig_test_frontend_presentation_probe *probe,
    boxrig_test_frontend_presentation_observation *observation_out)
{
    boxrig_win32_terminal_component *component = opaque_component;
    HWND window;
    RECT client;
    HDC context;
    COLORREF background;
    COLORREF meaning;
    uint64_t full_hash = UINT64_C(1469598103934665603);
    uint64_t stable_hash = UINT64_C(1469598103934665603);
    size_t coverage = 0;
    unsigned int width;
    unsigned int height;
    unsigned int x;
    unsigned int y;

    if (component == NULL || probe == NULL || observation_out == NULL) {
        return false;
    }
    window = boxrig_win32_terminal_component_owned_window(component);
    if (window == NULL) {
        return false;
    }
    (void)UpdateWindow(window);
    if (!GetClientRect(window, &client) ||
        client.right <= client.left || client.bottom <= client.top) {
        return false;
    }
    width = (unsigned int)(client.right - client.left);
    height = (unsigned int)(client.bottom - client.top);
    if (probe->background_x < 0 || probe->background_y < 0 ||
        probe->meaning_x < 0 || probe->meaning_y < 0 ||
        probe->coverage_x < 0 || probe->coverage_y < 0 ||
        probe->stable_x < 0 || probe->stable_y < 0 ||
        (unsigned int)probe->background_x >= width ||
        (unsigned int)probe->background_y >= height ||
        (unsigned int)probe->meaning_x >= width ||
        (unsigned int)probe->meaning_y >= height ||
        (unsigned int)probe->coverage_x >= width ||
        (unsigned int)probe->coverage_y >= height ||
        (unsigned int)probe->stable_x >= width ||
        (unsigned int)probe->stable_y >= height ||
        probe->coverage_width == 0 || probe->coverage_height == 0 ||
        probe->stable_width == 0 || probe->stable_height == 0 ||
        probe->coverage_width > width - (unsigned int)probe->coverage_x ||
        probe->coverage_height > height - (unsigned int)probe->coverage_y ||
        probe->stable_width > width - (unsigned int)probe->stable_x ||
        probe->stable_height > height - (unsigned int)probe->stable_y) {
        return false;
    }
#if defined(BOXRIG_TEST_OPENGL_COMPONENT) || defined(BOXRIG_TEST_D3D11_COMPONENT)
    context = capture_gpu_component(component);
#else
    context = capture_gdi_window(window, width, height);
#endif
    if (context == NULL) {
        fprintf(stderr, "FAIL presentation context: client=%ux%u visible=%d\n",
            width, height, IsWindowVisible(window));
        return false;
    }
    background = GetPixel(context, probe->background_x, probe->background_y);
    meaning = GetPixel(context, probe->meaning_x, probe->meaning_y);
    if (background == CLR_INVALID || meaning == CLR_INVALID) {
        fprintf(stderr, "FAIL presentation probe pixels: background=%08lx meaning=%08lx client=%ux%u visible=%d\n",
            (unsigned long)background, (unsigned long)meaning, width, height, IsWindowVisible(window));
        (void)release_presentation_context(window, context);
        return false;
    }
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            COLORREF pixel = GetPixel(context, (int)x, (int)y);

            if (pixel == CLR_INVALID) {
                fprintf(stderr, "FAIL presentation full pixel: x=%u y=%u client=%ux%u visible=%d\n",
                    x, y, width, height, IsWindowVisible(window));
                (void)release_presentation_context(window, context);
                return false;
            }
            full_hash ^= (uint64_t)pixel;
            full_hash *= UINT64_C(1099511628211);
        }
    }
    for (y = 0; y < probe->coverage_height; y++) {
        for (x = 0; x < probe->coverage_width; x++) {
            COLORREF pixel = GetPixel(
                context,
                probe->coverage_x + (int)x,
                probe->coverage_y + (int)y);

            if (pixel == CLR_INVALID) {
                fprintf(stderr, "FAIL presentation coverage pixel: x=%u y=%u\n", x, y);
                (void)release_presentation_context(window, context);
                return false;
            }
            if (pixel != background) {
                coverage++;
            }
        }
    }
    for (y = 0; y < probe->stable_height; y++) {
        for (x = 0; x < probe->stable_width; x++) {
            COLORREF pixel = GetPixel(
                context,
                probe->stable_x + (int)x,
                probe->stable_y + (int)y);

            if (pixel == CLR_INVALID) {
                fprintf(stderr, "FAIL presentation stable pixel: x=%u y=%u\n", x, y);
                (void)release_presentation_context(window, context);
                return false;
            }
            stable_hash ^= (uint64_t)pixel;
            stable_hash *= UINT64_C(1099511628211);
        }
    }
    if (release_presentation_context(window, context) != 1) {
        return false;
    }
    if (!fixture_presentation_captured ||
        fixture_presentation_hash != full_hash) {
        fixture_presentation_generation++;
        fixture_presentation_hash = full_hash;
        fixture_presentation_captured = true;
    }
    memset(observation_out, 0, sizeof(*observation_out));
    observation_out->generation = fixture_presentation_generation;
    observation_out->width = width;
    observation_out->height = height;
    observation_out->background_argb =
        UINT32_C(0xff000000) | ((uint32_t)GetRValue(background) << 16) |
        ((uint32_t)GetGValue(background) << 8) | GetBValue(background);
    observation_out->meaning_argb =
        UINT32_C(0xff000000) | ((uint32_t)GetRValue(meaning) << 16) |
        ((uint32_t)GetGValue(meaning) << 8) | GetBValue(meaning);
    observation_out->coverage_pixels = coverage;
    observation_out->stable_hash = stable_hash;
    observation_out->alpha_observable = false;
    return true;
}

static boxrig_ui_view_action_capabilities view_action_capabilities(
    void *component)
{
    return boxrig_win32_terminal_component_view_action_capabilities(
        component);
}

static boxrig_ui_status apply_view_action(
    void *component,
    const boxrig_ui_view_action *action)
{
    return boxrig_win32_terminal_component_apply_view_action(
        component,
        action);
}

static bool get_scrollbar_state(void *component, boxrig_scrollbar_state *state_out)
{
    example_external_scrollbar bar = {.component = component};
    return check_external_scrollbar_example(&bar, state_out);
}

static bool get_scrollbar_model(
    void *component,
    boxrig_scrollbar_model *model_out)
{
    return boxrig_win32_terminal_component_get_scrollbar_model(
               component,
               model_out) == BOXRIG_UI_OK;
}

static boxrig_ui_status apply_scrollbar_action(
    void *component,
    boxrig_scrollbar_action action,
    size_t position,
    bool *changed_out)
{
    return boxrig_win32_terminal_component_apply_scrollbar_action(
        component,
        action,
        position,
        changed_out);
}

int main(int argc, char **argv)
{
    const boxrig_test_frontend_component_operations operations = {
        .frontend_name = "Win32",
        .lifecycle = {
            .create_host = create_host,
            .destroy_host = destroy_host,
            .create_component = create_component,
            .quiesce_component = quiesce_component,
            .destroy_component = destroy_component},
        .layout = {
            .component_view = component_view,
            .get_effective_metrics = get_effective_metrics,
            .allocate = allocate_component,
            .set_preferred_grid = set_preferred_grid,
            .get_preferred_grid = get_preferred_grid,
            .get_size_command_state = get_size_command_state,
            .take_layout_action = take_layout_action,
            .complete_size_command = complete_size_command,
            .complete_grid_resize = complete_grid_resize,
            .set_visible = set_visible},
        .frame = {
            .set_frame_request_callback = set_frame_request_callback,
            .frame = frame_component},
        .input = {
            .queue_paste = queue_paste,
            .run_native_transcript = run_native_input_transcript},
        .font = {
            .fixture_font_path = argc == 2 ? argv[1] : NULL,
            .register_font_file = register_font_file,
            .set_specification = set_font_specification,
            .get_specification = get_font_specification,
            .primary_resource = primary_font_resource,
            .catalog_revision = font_catalog_revision,
            .set_line_height = set_line_height,
            .get_line_height = get_line_height,
            .visual_zoom = visual_zoom,
            .visual_zoom_reset = visual_zoom_reset,
            .set_text_direction = set_text_direction,
            .text_direction = text_direction},
        .view_action = {
            .copy_selection = copy_selection,
            .capabilities = view_action_capabilities,
            .apply = apply_view_action,
            .get_scrollbar_model = get_scrollbar_model,
            .get_scrollbar_state = get_scrollbar_state,
            .apply_scrollbar_action = apply_scrollbar_action},
        .presentation = {
            .set_transparency = set_transparency,
            .get_transparency = get_transparency,
            .capabilities = capabilities,
            .get_diagnostic = get_diagnostic,
            .composition = component_composition,
            .observe = observe_presentation}};

    if (argc != 2) {
        return 2;
    }
#ifdef BOXRIG_TEST_OPENGL_COMPONENT
    puts("Win32 component fixture: OpenGL presentation");
#elif defined(BOXRIG_TEST_D3D11_COMPONENT)
    puts("Win32 component fixture: Direct3D 11 presentation");
#endif
    return boxrig_test_run_frontend_component_fixture(&operations) ? 0 : 1;
}
