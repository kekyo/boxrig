/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/win32/transparency.h"
#include "transparency_private.h"

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool test_opaque_request_retains_the_standard_window(void)
{
    boxrig_ui_transparency_config config =
        boxrig_ui_transparency_config_default();
    boxrig_win32_transparency_context *context = NULL;
    boxrig_win32_window_requirements requirements;
    boxrig_ui_transparency_state state;
    HINSTANCE instance = GetModuleHandleW(NULL);

    CHECK(instance != NULL);
    CHECK(boxrig_win32_transparency_context_create(
              instance,
              &config,
              true,
              &context) == BOXRIG_UI_OK);
    CHECK(context != NULL);
    CHECK(boxrig_win32_transparency_context_get_window_requirements(
              context,
              &requirements) == BOXRIG_UI_OK);
    CHECK((requirements.style & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW);
    CHECK((requirements.style & WS_VSCROLL) != 0);
    CHECK((requirements.extended_style & WS_EX_NOREDIRECTIONBITMAP) == 0);
    CHECK(!requirements.layered_visual);
    CHECK(!requirements.owned_scrollbar);
    CHECK(boxrig_win32_transparency_context_get_state(context, &state) ==
          BOXRIG_UI_OK);
    CHECK(state.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
    CHECK(state.effective_background_opacity == 255);
    boxrig_win32_transparency_context_destroy(context);
    return true;
}

static bool test_invalid_context_arguments_are_rejected(void)
{
    boxrig_ui_transparency_config config =
        boxrig_ui_transparency_config_default();
    boxrig_win32_transparency_context *context = NULL;
    boxrig_win32_window_requirements requirements;

    CHECK(boxrig_win32_transparency_context_create(
              NULL,
              &config,
              true,
              &context) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_win32_transparency_context_create(
              GetModuleHandleW(NULL),
              NULL,
              true,
              &context) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_win32_transparency_context_get_window_requirements(
              NULL,
              &requirements) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_win32_transparency_context_get_state(NULL, NULL) ==
          BOXRIG_UI_INVALID_ARGUMENT);
    return true;
}

static HRESULT WINAPI fake_composition_enabled(BOOL *enabled_out)
{
    *enabled_out = TRUE;
    return S_OK;
}

static BOOL fake_composition_active = TRUE;
static BOOL fake_layered_present_succeeds = TRUE;
static unsigned int fake_layered_present_count;
static HWND fake_layered_present_window;
static bool fake_layered_destination_was_null;
static SIZE fake_layered_size;
static POINT fake_layered_source;
static BLENDFUNCTION fake_layered_blend;
static DWORD fake_layered_flags;
static bool fake_layered_had_dirty;
static RECT fake_layered_dirty;
static bool fake_layered_window_size_valid;
static SIZE fake_layered_window_size_before_update;
static unsigned int host_scroll_count;
static WPARAM host_scroll_word_parameter;
static LPARAM host_scroll_long_parameter;

static HRESULT WINAPI fake_dynamic_composition(BOOL *enabled_out)
{
    *enabled_out = fake_composition_active;
    return S_OK;
}

static BOOL WINAPI fake_regular_display(
    LPCWSTR device_name,
    DWORD device_index,
    PDISPLAY_DEVICEW device_out,
    DWORD flags)
{
    (void)device_name;
    (void)flags;
    if (device_index != 0) {
        return FALSE;
    }
    device_out->StateFlags =
        DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
    wcscpy(device_out->DeviceString, L"Test display adapter");
    wcscpy(device_out->DeviceID, L"PCI\\VEN_8086&DEV_3E92");
    return TRUE;
}

static BOOL WINAPI fake_update_layered_window_indirect(
    HWND window,
    const UPDATELAYEREDWINDOWINFO *information)
{
    RECT client;

    fake_layered_present_count++;
    fake_layered_present_window = window;
    fake_layered_destination_was_null = information->pptDst == NULL;
    fake_layered_size = *information->psize;
    fake_layered_source = *information->pptSrc;
    fake_layered_blend = *information->pblend;
    fake_layered_flags = information->dwFlags;
    fake_layered_had_dirty = information->prcDirty != NULL;
    if (information->prcDirty != NULL) {
        fake_layered_dirty = *information->prcDirty;
    }
    fake_layered_window_size_valid = GetClientRect(window, &client) != FALSE;
    if (fake_layered_window_size_valid) {
        fake_layered_window_size_before_update.cx = client.right - client.left;
        fake_layered_window_size_before_update.cy = client.bottom - client.top;
    }
    if (!fake_layered_present_succeeds) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    return SetWindowPos(
        window,
        NULL,
        0,
        0,
        information->psize->cx,
        information->psize->cy,
        SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
}

static LRESULT CALLBACK test_host_procedure(
    HWND window,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter)
{
    if (message == WM_VSCROLL) {
        host_scroll_count++;
        host_scroll_word_parameter = word_parameter;
        host_scroll_long_parameter = long_parameter;
        return 91;
    }
    return DefWindowProcW(window, message, word_parameter, long_parameter);
}

static bool register_test_host_class(HINSTANCE instance)
{
    WNDCLASSEXW window_class;

    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = test_host_procedure;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"boxrig_win32_transparency_test_host";
    if (RegisterClassExW(&window_class) != 0) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

typedef struct test_dib {
    HDC context;
    HBITMAP bitmap;
    HGDIOBJ previous_bitmap;
    uint32_t *pixels;
} test_dib;

static bool create_test_dib(SIZE size, test_dib *dib_out)
{
    BITMAPINFO information;

    memset(dib_out, 0, sizeof(*dib_out));
    dib_out->context = CreateCompatibleDC(NULL);
    if (dib_out->context == NULL) {
        return false;
    }
    memset(&information, 0, sizeof(information));
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = size.cx;
    information.bmiHeader.biHeight = -size.cy;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    dib_out->bitmap = CreateDIBSection(
        dib_out->context,
        &information,
        DIB_RGB_COLORS,
        (void **)&dib_out->pixels,
        NULL,
        0);
    if (dib_out->bitmap == NULL || dib_out->pixels == NULL) {
        if (dib_out->bitmap != NULL) {
            (void)DeleteObject(dib_out->bitmap);
        }
        (void)DeleteDC(dib_out->context);
        memset(dib_out, 0, sizeof(*dib_out));
        return false;
    }
    dib_out->previous_bitmap =
        SelectObject(dib_out->context, dib_out->bitmap);
    if (dib_out->previous_bitmap == NULL ||
        dib_out->previous_bitmap == HGDI_ERROR) {
        (void)DeleteObject(dib_out->bitmap);
        (void)DeleteDC(dib_out->context);
        memset(dib_out, 0, sizeof(*dib_out));
        return false;
    }
    return true;
}

static void destroy_test_dib(test_dib *dib)
{
    if (dib->context != NULL && dib->previous_bitmap != NULL) {
        (void)SelectObject(dib->context, dib->previous_bitmap);
    }
    if (dib->bitmap != NULL) {
        (void)DeleteObject(dib->bitmap);
    }
    if (dib->context != NULL) {
        (void)DeleteDC(dib->context);
    }
    memset(dib, 0, sizeof(*dib));
}

static bool test_layered_surface_contract_and_dynamic_fallback(void)
{
    boxrig_win32_transparency_system system;
    boxrig_ui_transparency_config config =
        boxrig_ui_transparency_config_default();
    boxrig_win32_transparency_context *context = NULL;
    boxrig_win32_window_requirements requirements;
    boxrig_win32_transparency_support support;
    boxrig_ui_transparency_state initial_state;
    boxrig_ui_transparency_state lost_state;
    boxrig_ui_transparency_state composition_lost_state;
    boxrig_ui_transparency_state recovered_state;
    HINSTANCE instance = GetModuleHandleW(NULL);
    HWND host;
    HWND visual;
    HWND scrollbar;
    HWND scrollbar_container;
    HWND fallback;
    HDC fallback_context;
    RECT content;
    RECT visual_bounds;
    RECT scrollbar_bounds;
    RECT retained_visual_bounds;
    RECT host_bounds;
    RECT dirty = {2, 3, 9, 11};
    SIZE size;
    SIZE expanded_size;
    SCROLLINFO scroll_information;
    test_dib dib;
    test_dib expanded_dib;
    COLORREF fallback_pixel;
    size_t pixel_index;
    bool state_changed;

    memset(&system, 0, sizeof(system));
    system.layered_child_available = true;
    system.dwm_is_composition_enabled = fake_dynamic_composition;
    system.enum_display_devices = fake_regular_display;
    system.update_layered_window_indirect =
        fake_update_layered_window_indirect;
    fake_composition_active = TRUE;
    fake_layered_present_succeeds = TRUE;
    fake_layered_present_count = 0;
    fake_layered_window_size_valid = false;
    host_scroll_count = 0;
    config.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
    config.background_opacity = 128;

    CHECK(instance != NULL);
    CHECK(register_test_host_class(instance));
    CHECK(boxrig_win32_transparency_context_create_with_system(
              instance,
              &config,
              true,
              &system,
              &context) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_context_get_window_requirements(
              context,
              &requirements) == BOXRIG_UI_OK);
    CHECK(requirements.style == WS_OVERLAPPEDWINDOW);
    CHECK(requirements.extended_style == WS_EX_NOREDIRECTIONBITMAP);
    CHECK(requirements.layered_visual);
    CHECK(requirements.owned_scrollbar);
    host = CreateWindowExW(
        requirements.extended_style,
        L"boxrig_win32_transparency_test_host",
        L"",
        requirements.style,
        20,
        20,
        360,
        280,
        NULL,
        NULL,
        instance,
        NULL);
    CHECK(host != NULL);
    CHECK(boxrig_win32_transparency_context_bind_host(context, host) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_context_synchronize_layout(
              context,
              &content) == BOXRIG_UI_OK);
    CHECK(content.left == 0);
    CHECK(content.top == 0);
    CHECK(content.right > content.left);
    CHECK(content.bottom > content.top);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    CHECK(IsWindowVisible(host));
    CHECK(boxrig_win32_transparency_context_set_visible(context, true) ==
          BOXRIG_UI_OK);

    visual = boxrig_win32_transparency_context_visual_window(context);
    scrollbar = boxrig_win32_transparency_context_scrollbar_window(context);
    fallback = boxrig_win32_transparency_context_fallback_window(context);
    CHECK(visual != NULL);
    CHECK(scrollbar != NULL);
    CHECK(fallback != NULL);
    scrollbar_container = GetParent(scrollbar);
    CHECK(scrollbar_container != NULL);
    RECT full_client, hidden_content;
    CHECK(GetClientRect(host, &full_client));
    SetCapture(scrollbar);
    CHECK(GetCapture() == scrollbar);
    CHECK(boxrig_win32_transparency_context_set_scrollbar_visible(context, false) == BOXRIG_UI_OK);
    CHECK(GetCapture() != scrollbar);
    CHECK(!IsWindowVisible(scrollbar_container));
    CHECK(boxrig_win32_transparency_context_synchronize_layout(context, &hidden_content) == BOXRIG_UI_OK);
    CHECK(EqualRect(&full_client, &hidden_content));
    CHECK(boxrig_win32_transparency_context_set_visible(context, false) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_context_set_visible(context, true) == BOXRIG_UI_OK);
    CHECK(!IsWindowVisible(scrollbar_container));
    CHECK(boxrig_win32_transparency_context_set_scrollbar_visible(context, true) == BOXRIG_UI_OK);
    CHECK(IsWindowVisible(scrollbar_container));
    CHECK(boxrig_win32_transparency_context_synchronize_layout(context, &hidden_content) == BOXRIG_UI_OK);
    CHECK(EqualRect(&content, &hidden_content));
    CHECK(GetParent(visual) == host);
    CHECK(GetWindow(scrollbar_container, GW_OWNER) == host);
    CHECK(GetWindow(fallback, GW_OWNER) == host);
    CHECK(((DWORD)GetWindowLongPtrW(visual, GWL_STYLE) & WS_CHILD) != 0);
    CHECK(((DWORD)GetWindowLongPtrW(visual, GWL_EXSTYLE) & WS_EX_LAYERED) != 0);
    CHECK(((DWORD)GetWindowLongPtrW(scrollbar_container, GWL_STYLE) &
           WS_POPUP) != 0);
    CHECK(((DWORD)GetWindowLongPtrW(scrollbar_container, GWL_EXSTYLE) &
           (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) ==
          (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE));
    CHECK(((DWORD)GetWindowLongPtrW(scrollbar, GWL_STYLE) & WS_CHILD) != 0);
    CHECK(SendMessageW(
              scrollbar_container,
              WM_VSCROLL,
              MAKEWPARAM(SB_THUMBTRACK, 37),
              (LPARAM)scrollbar) == 91);
    CHECK(host_scroll_count == 1);
    CHECK(LOWORD(host_scroll_word_parameter) == SB_THUMBTRACK);
    CHECK(HIWORD(host_scroll_word_parameter) == 37);
    CHECK(host_scroll_long_parameter == (LPARAM)scrollbar);

    memset(&scroll_information, 0, sizeof(scroll_information));
    scroll_information.cbSize = sizeof(scroll_information);
    scroll_information.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll_information.nMin = 0;
    scroll_information.nMax = 100;
    scroll_information.nPage = 10;
    scroll_information.nPos = 37;
    CHECK(boxrig_win32_transparency_context_set_scroll_info(
              context,
              &scroll_information,
              false) == BOXRIG_UI_OK);
    memset(&scroll_information, 0, sizeof(scroll_information));
    scroll_information.cbSize = sizeof(scroll_information);
    scroll_information.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    CHECK(boxrig_win32_transparency_context_get_scroll_info(
              context,
              &scroll_information) == BOXRIG_UI_OK);
    CHECK(scroll_information.nMin == 0);
    CHECK(scroll_information.nMax == 100);
    CHECK(scroll_information.nPage == 10);
    CHECK(scroll_information.nPos == 37);

    size.cx = content.right - content.left;
    size.cy = content.bottom - content.top;
    CHECK(create_test_dib(size, &dib));
    for (pixel_index = 0;
         pixel_index < (size_t)size.cx * (size_t)size.cy;
         pixel_index++) {
        dib.pixels[pixel_index] = UINT32_C(0x001a5c9e);
    }
    CHECK(boxrig_win32_transparency_context_present(
              context,
              dib.context,
              size,
              NULL) == BOXRIG_WIN32_TRANSPARENCY_PRESENTED_ALPHA);
    CHECK(fake_layered_present_count == 1);
    CHECK(fake_layered_present_window == visual);
    CHECK(fake_layered_destination_was_null);
    CHECK(fake_layered_size.cx == size.cx);
    CHECK(fake_layered_size.cy == size.cy);
    CHECK(fake_layered_source.x == 0);
    CHECK(fake_layered_source.y == 0);
    CHECK(fake_layered_blend.BlendOp == AC_SRC_OVER);
    CHECK(fake_layered_blend.SourceConstantAlpha == 255);
    CHECK(fake_layered_blend.AlphaFormat == AC_SRC_ALPHA);
    CHECK(fake_layered_flags == ULW_ALPHA);
    CHECK(!fake_layered_had_dirty);
    CHECK(GetWindowRect(visual, &visual_bounds));
    CHECK(GetWindowRect(scrollbar_container, &scrollbar_bounds));
    CHECK(visual_bounds.right == scrollbar_bounds.left);
    CHECK(visual_bounds.top == scrollbar_bounds.top);
    CHECK(visual_bounds.bottom == scrollbar_bounds.bottom);

    CHECK(boxrig_win32_transparency_context_present(
              context,
              dib.context,
              size,
              &dirty) == BOXRIG_WIN32_TRANSPARENCY_PRESENTED_ALPHA);
    CHECK(fake_layered_present_count == 2);
    CHECK(fake_layered_destination_was_null);
    CHECK((fake_layered_flags & (ULW_ALPHA | ULW_EX_NORESIZE)) ==
          (ULW_ALPHA | ULW_EX_NORESIZE));
    CHECK(fake_layered_had_dirty);
    CHECK(EqualRect(&fake_layered_dirty, &dirty));
    CHECK(boxrig_win32_transparency_context_get_support(context, &support) ==
          BOXRIG_UI_OK);
    CHECK(support.last_alpha_present_succeeded);
    CHECK(boxrig_win32_transparency_context_get_state(context, &initial_state) ==
          BOXRIG_UI_OK);
    CHECK(initial_state.effective ==
          BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA);

    fake_layered_present_succeeds = FALSE;
    CHECK(boxrig_win32_transparency_context_present(
              context,
              dib.context,
              size,
              &dirty) == BOXRIG_WIN32_TRANSPARENCY_FALLBACK_REQUIRED);
    CHECK(GetLastError() == ERROR_NOT_ENOUGH_MEMORY);
    CHECK(boxrig_win32_transparency_context_get_support(context, &support) ==
          BOXRIG_UI_OK);
    CHECK(!support.last_alpha_present_succeeded);
    CHECK(boxrig_win32_transparency_context_get_state(context, &lost_state) ==
          BOXRIG_UI_OK);
    CHECK(lost_state.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
    CHECK(lost_state.revision > initial_state.revision);
    fallback_context = GetDC(fallback);
    CHECK(fallback_context != NULL);
    CHECK(PatBlt(
              fallback_context,
              0,
              0,
              size.cx,
              size.cy,
              BLACKNESS));
    CHECK(ReleaseDC(fallback, fallback_context) == 1);
    CHECK(boxrig_win32_transparency_context_present_opaque(
              context,
              dib.context,
              size,
              NULL) == BOXRIG_WIN32_TRANSPARENCY_PRESENTED_OPAQUE);
    fallback_context = GetDC(fallback);
    CHECK(fallback_context != NULL);
    fallback_pixel = GetPixel(
        fallback_context,
        size.cx / 2,
        size.cy / 2);
    CHECK(ReleaseDC(fallback, fallback_context) == 1);
    CHECK(fallback_pixel == RGB(0x1a, 0x5c, 0x9e));

    config.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    CHECK(boxrig_win32_transparency_context_set_config(context, &config) ==
          BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_win32_transparency_context_get_state(context, &recovered_state) ==
          BOXRIG_UI_OK);
    CHECK(recovered_state.config.request ==
          BOXRIG_UI_TRANSPARENCY_PREFERRED);
    CHECK(recovered_state.revision == lost_state.revision);

    fake_composition_active = FALSE;
    CHECK(boxrig_win32_transparency_context_process_host_message(
              context,
              WM_DWMCOMPOSITIONCHANGED,
              0,
              0,
              &state_changed) == 1);
    CHECK(state_changed);
    CHECK(boxrig_win32_transparency_context_get_state(
              context,
              &composition_lost_state) == BOXRIG_UI_OK);
    CHECK(composition_lost_state.effective ==
          BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
    CHECK(composition_lost_state.revision > lost_state.revision);
    fake_composition_active = TRUE;
    fake_layered_present_succeeds = TRUE;
    CHECK(boxrig_win32_transparency_context_process_host_message(
              context,
              WM_DWMCOMPOSITIONCHANGED,
              0,
              0,
              &state_changed) == 1);
    CHECK(state_changed);
    CHECK(boxrig_win32_transparency_context_get_state(context, &recovered_state) ==
          BOXRIG_UI_OK);
    CHECK(recovered_state.effective ==
          BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA);
    CHECK(recovered_state.revision > composition_lost_state.revision);
    CHECK(boxrig_win32_transparency_context_present(
              context,
              dib.context,
              size,
              NULL) == BOXRIG_WIN32_TRANSPARENCY_PRESENTED_ALPHA);

    CHECK(GetWindowRect(visual, &visual_bounds));
    CHECK(GetWindowRect(host, &host_bounds));
    CHECK(SetWindowPos(
              host,
              NULL,
              0,
              0,
              host_bounds.right - host_bounds.left + 32,
              host_bounds.bottom - host_bounds.top + 24,
              SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER));
    CHECK(boxrig_win32_transparency_context_synchronize_layout(
              context,
              &content) == BOXRIG_UI_OK);
    CHECK(GetWindowRect(visual, &retained_visual_bounds));
    CHECK(retained_visual_bounds.right - retained_visual_bounds.left ==
          visual_bounds.right - visual_bounds.left);
    CHECK(retained_visual_bounds.bottom - retained_visual_bounds.top ==
          visual_bounds.bottom - visual_bounds.top);
    expanded_size.cx = content.right - content.left;
    expanded_size.cy = content.bottom - content.top;
    CHECK(expanded_size.cx > size.cx && expanded_size.cy > size.cy);
    CHECK(create_test_dib(expanded_size, &expanded_dib));
    for (pixel_index = 0;
         pixel_index <
         (size_t)expanded_size.cx * (size_t)expanded_size.cy;
         pixel_index++) {
        expanded_dib.pixels[pixel_index] = UINT32_C(0x001a5c9e);
    }
    CHECK(boxrig_win32_transparency_context_present(
              context,
              expanded_dib.context,
              expanded_size,
              NULL) == BOXRIG_WIN32_TRANSPARENCY_PRESENTED_ALPHA);
    CHECK(fake_layered_window_size_valid);
    CHECK(fake_layered_window_size_before_update.cx ==
          visual_bounds.right - visual_bounds.left);
    CHECK(fake_layered_window_size_before_update.cy ==
          visual_bounds.bottom - visual_bounds.top);
    CHECK(GetWindowRect(visual, &visual_bounds));
    CHECK(visual_bounds.right - visual_bounds.left == expanded_size.cx);
    CHECK(visual_bounds.bottom - visual_bounds.top == expanded_size.cy);

    destroy_test_dib(&expanded_dib);
    destroy_test_dib(&dib);
    boxrig_win32_transparency_context_destroy(context);
    CHECK(DestroyWindow(host));
    return true;
}

static HRESULT WINAPI unavailable_composition(BOOL *enabled_out)
{
    *enabled_out = FALSE;
    return E_NOTIMPL;
}

static bool test_optional_alpha_failure_preserves_opaque_presentation(void)
{
    const HINSTANCE instance = GetModuleHandleW(NULL);
    const SIZE size = {16, 12};
    unsigned int unavailable;
    CHECK(register_test_host_class(instance));
    for (unavailable = 0; unavailable < 5; unavailable++) {
        boxrig_win32_transparency_system system;
        boxrig_ui_transparency_config config = boxrig_ui_transparency_config_default();
        boxrig_win32_transparency_context *context = NULL;
        boxrig_win32_window_requirements requirements;
        boxrig_ui_transparency_state state;
        HWND host;
        HDC target;
        test_dib dib;
        size_t pixel;
        memset(&system, 0, sizeof(system));
        system.enum_display_devices = fake_regular_display;
        system.dwm_is_composition_enabled = unavailable == 0 ? NULL :
            unavailable == 1 ? unavailable_composition : fake_composition_enabled;
        system.update_layered_window_indirect = unavailable == 2 ? NULL :
            fake_update_layered_window_indirect;
        system.layered_child_available = unavailable != 3;
        config.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
        config.background_opacity = 128;
        CHECK(boxrig_win32_transparency_context_create_with_system(
            instance, &config, unavailable != 4, &system, &context) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_transparency_context_get_state(context, &state) == BOXRIG_UI_OK);
        CHECK(state.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
        CHECK(state.config.background_opacity == 128);
        CHECK(state.effective_background_opacity == 255);
        CHECK(boxrig_win32_transparency_context_get_window_requirements(
            context, &requirements) == BOXRIG_UI_OK);
        CHECK(!requirements.layered_visual && !requirements.owned_scrollbar);
        CHECK((requirements.extended_style & WS_EX_NOREDIRECTIONBITMAP) == 0);
        host = CreateWindowExW(requirements.extended_style,
            L"boxrig_win32_transparency_test_host", L"optional alpha unavailable",
            requirements.style, 10, 10, 160, 120, NULL, NULL, instance, NULL);
        CHECK(host != NULL);
        CHECK(boxrig_win32_transparency_context_bind_host(context, host) == BOXRIG_UI_OK);
        ShowWindow(host, SW_SHOWNOACTIVATE);
        UpdateWindow(host);
        CHECK(create_test_dib(size, &dib));
        for (pixel = 0; pixel < (size_t)size.cx * (size_t)size.cy; pixel++)
            dib.pixels[pixel] = UINT32_C(0xff1a5c9e);
        CHECK(boxrig_win32_transparency_context_present_opaque(
            context, dib.context, size, NULL) == BOXRIG_WIN32_TRANSPARENCY_PRESENTED_OPAQUE);
        target = GetDC(host);
        CHECK(target != NULL);
        CHECK(GetPixel(target, 4, 5) == RGB(0x1a, 0x5c, 0x9e));
        CHECK(ReleaseDC(host, target) == 1);
        destroy_test_dib(&dib);
        boxrig_win32_transparency_context_destroy(context);
        CHECK(DestroyWindow(host));
        context = NULL;
        config.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
        CHECK(boxrig_win32_transparency_context_create_with_system(
            instance, &config, unavailable != 4, &system, &context) == BOXRIG_UI_UNAVAILABLE);
        CHECK(context == NULL);
    }
    return true;
}

int main(void)
{
    if (!test_opaque_request_retains_the_standard_window() ||
        !test_invalid_context_arguments_are_rejected() ||
        !test_optional_alpha_failure_preserves_opaque_presentation() ||
        !test_layered_surface_contract_and_dynamic_fallback()) {
        return 1;
    }
    printf("PASS Win32 layered and fallback transparency surfaces\n");
    return 0;
}
