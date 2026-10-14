/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <windows.h>
#include <shellapi.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../frontend_sample_scenario.h"

#define BOXRIG_TEST_WIDE_LITERAL_INNER(value) L##value
#define BOXRIG_TEST_WIDE_LITERAL(value) \
    BOXRIG_TEST_WIDE_LITERAL_INNER(value)

typedef struct window_search {
    DWORD process_id;
    HWND window;
} window_search;

static LPARAM key_parameter(UINT virtual_key, bool released)
{
    UINT scan_code = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
    LPARAM parameter = 1 | ((LPARAM)scan_code << 16);

    if (released) {
        parameter |= (LPARAM)1 << 30;
        parameter |= (LPARAM)1 << 31;
    }
    return parameter;
}

static bool post_key(HWND window, UINT virtual_key, bool released)
{
    return PostMessageW(
               window,
               released ? WM_KEYUP : WM_KEYDOWN,
               virtual_key,
               key_parameter(virtual_key, released)) != 0;
}

static bool commit_character(HWND window, wchar_t character)
{
    DWORD_PTR result;

    return SendMessageTimeoutW(
               window,
               WM_IME_CHAR,
               (WPARAM)character,
               1,
               SMTO_ABORTIFHUNG,
               5000,
               &result) != 0;
}

typedef struct scrollbar_window_search {
    HWND host;
    HWND window;
} scrollbar_window_search;

static BOOL CALLBACK find_scrollbar_child(HWND window, LPARAM parameter)
{
    scrollbar_window_search *search =
        (scrollbar_window_search *)parameter;
    wchar_t class_name[32];

    if (IsWindowVisible(window) &&
        GetClassNameW(
            window,
            class_name,
            (int)(sizeof(class_name) / sizeof(class_name[0]))) > 0 &&
        lstrcmpiW(class_name, L"SCROLLBAR") == 0) {
        search->window = window;
        return FALSE;
    }
    return TRUE;
}

static BOOL CALLBACK find_owned_scrollbar(HWND window, LPARAM parameter)
{
    scrollbar_window_search *search =
        (scrollbar_window_search *)parameter;

    if (GetWindow(window, GW_OWNER) == search->host) {
        (void)EnumChildWindows(window, find_scrollbar_child, parameter);
    }
    return search->window == NULL;
}

static HWND terminal_input_window_for_host(HWND host);

static HWND scrollbar_window_for_host(HWND host)
{
    scrollbar_window_search search;

    if (((DWORD)GetWindowLongPtrW(host, GWL_STYLE) & WS_VSCROLL) != 0) {
        return host;
    }
    search.host = host;
    search.window = NULL;
    (void)EnumChildWindows(host, find_scrollbar_child, (LPARAM)&search);
    if (search.window != NULL) {
        return search.window;
    }
    (void)EnumWindows(find_owned_scrollbar, (LPARAM)&search);
    if (search.window != NULL) return search.window;
    {
        HWND component = terminal_input_window_for_host(host);
        return component == NULL ? host : component;
    }
}

static BOOL CALLBACK find_terminal_component_child(
    HWND window,
    LPARAM parameter)
{
    HWND *component_out = (HWND *)parameter;
    wchar_t class_name[64];

    if (GetClassNameW(
            window,
            class_name,
            (int)(sizeof(class_name) / sizeof(class_name[0]))) > 0 &&
        lstrcmpW(class_name, L"boxrig_terminal_component") == 0) {
        *component_out = window;
        return FALSE;
    }
    return TRUE;
}

static HWND terminal_input_window_for_host(HWND host)
{
    HWND component = NULL;

    (void)EnumChildWindows(
        host,
        find_terminal_component_child,
        (LPARAM)&component);
    /* The sample may bind the existing host directly; exercise input on that
     * window when it owns no intermediate component HWND. */
    return component != NULL ? component : IsWindow(host) ? host : NULL;
}

static LONG scrollbar_width_for_host(HWND host)
{
    HWND scrollbar = scrollbar_window_for_host(host);
    RECT client;

    /* Standard sample surfaces now compose their scrollbar with the terminal.
     * Its device-pixel width follows the same OS metric for every renderer. */
    if (scrollbar == terminal_input_window_for_host(host)) {
        return GetSystemMetrics(SM_CXVSCROLL);
    }
    return GetClientRect(scrollbar, &client)
               ? client.right - client.left : 0;
}

static bool read_vertical_scroll_info(
    HWND window,
    SCROLLINFO *information_out)
{
    HWND scrollbar = scrollbar_window_for_host(window);

    ZeroMemory(information_out, sizeof(*information_out));
    information_out->cbSize = sizeof(*information_out);
    information_out->fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    /* A composited scrollbar has no native scroll-control state. Querying
     * SB_CTL on the terminal input HWND can report unrelated zeroed values.
     * Its scrolling is verified below through the actual viewport pixels. */
    if (scrollbar == terminal_input_window_for_host(window) &&
        ((DWORD)GetWindowLongPtrW(scrollbar, GWL_STYLE) & WS_VSCROLL) == 0)
        return false;
    return GetScrollInfo(
               scrollbar,
               scrollbar == window ? SB_VERT : SB_CTL,
               information_out) != 0;
}

static int vertical_scroll_maximum_position(
    const SCROLLINFO *information)
{
    UINT page_adjustment = information->nPage == 0
                               ? 0
                               : information->nPage - 1;

    if (page_adjustment > (UINT)INT_MAX ||
        information->nMax < information->nMin + (int)page_adjustment) {
        return information->nMin;
    }
    return information->nMax - (int)page_adjustment;
}

static bool send_vertical_scroll(HWND window, UINT request)
{
    DWORD_PTR result;

    return SendMessageTimeoutW(
               window,
               WM_VSCROLL,
               MAKEWPARAM((WORD)request, 0),
               0,
               SMTO_ABORTIFHUNG,
               5000,
               &result) != 0;
}

static bool vertical_scroll_position_matches(
    HWND window,
    int expected_position,
    bool information_available)
{
    SCROLLINFO information;

    if (!information_available) {
        return true;
    }
    return read_vertical_scroll_info(window, &information) &&
           information.nPos == expected_position;
}

static LONG normalized_absolute_mouse_coordinate(
    int coordinate,
    int virtual_origin,
    int virtual_extent)
{
    int64_t relative = (int64_t)coordinate - virtual_origin;

    if (relative < 0) {
        relative = 0;
    }
    if (relative >= virtual_extent) {
        relative = virtual_extent - 1;
    }
    /* Target the pixel center, not a boundary that native input can round
     * into the preceding pixel on a multi-monitor desktop. */
    return (LONG)((relative * 65536 + 32768) / virtual_extent);
}

static bool post_scrollbar_control_drag_to_middle(HWND scrollbar)
{
    RECT client;
    SCROLLBARINFO bar;
    SCROLLINFO information;
    int start_y;
    int target_x;
    int target_y;

    ZeroMemory(&client, sizeof(client));
    ZeroMemory(&bar, sizeof(bar));
    ZeroMemory(&information, sizeof(information));
    bar.cbSize = sizeof(bar);
    information.cbSize = sizeof(information);
    information.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    if (!GetClientRect(scrollbar, &client) || client.right <= client.left ||
        client.bottom <= client.top ||
        !GetScrollInfo(scrollbar, SB_CTL, &information)) {
        return false;
    }
    if (GetScrollBarInfo(scrollbar, OBJID_CLIENT, &bar) &&
        bar.xyThumbTop >= client.top &&
        bar.xyThumbBottom <= client.bottom &&
        bar.xyThumbBottom > bar.xyThumbTop) {
        start_y = bar.xyThumbTop +
                  (bar.xyThumbBottom - bar.xyThumbTop) / 2;
    } else {
        int arrow_length = GetSystemMetrics(SM_CYVSCROLL);
        int minimum_thumb_length = GetSystemMetrics(SM_CYVTHUMB);
        int range_length;
        int maximum_position;
        int track_length;
        int thumb_length;
        int movable_length;

        if (arrow_length <= 0 || minimum_thumb_length <= 0 ||
            information.nMax < information.nMin ||
            information.nPage > (UINT)INT_MAX) {
            return false;
        }
        range_length = information.nMax - information.nMin + 1;
        maximum_position = information.nMax -
                           (information.nPage == 0
                                ? 0
                                : (int)information.nPage - 1);
        track_length = client.bottom - client.top - 2 * arrow_length;
        thumb_length = information.nPage == 0
                           ? minimum_thumb_length
                           : (int)((int64_t)track_length *
                                   information.nPage /
                                   (UINT)range_length);
        if (thumb_length < minimum_thumb_length) {
            thumb_length = minimum_thumb_length;
        }
        movable_length = track_length - thumb_length;
        if (range_length <= 0 || maximum_position <= information.nMin ||
            movable_length <= 0) {
            return false;
        }
        start_y = client.top + arrow_length +
                  (int)((int64_t)movable_length *
                        (information.nPos - information.nMin) /
                        (maximum_position - information.nMin)) +
                  thumb_length / 2;
    }
    target_x = client.left + (client.right - client.left) / 2;
    target_y = client.top + (client.bottom - client.top) / 2;

    /* The standard control enters a tracking loop on button-down, so queue
     * the complete gesture before that loop starts consuming messages. */
    return PostMessageW(
               scrollbar,
               WM_LBUTTONDOWN,
               MK_LBUTTON,
               MAKELPARAM(target_x, start_y)) != 0 &&
           PostMessageW(
               scrollbar,
               WM_MOUSEMOVE,
               MK_LBUTTON,
               MAKELPARAM(target_x, target_y)) != 0 &&
           PostMessageW(
               scrollbar,
               WM_LBUTTONUP,
               0,
               MAKELPARAM(target_x, target_y)) != 0;
}

static bool drag_vertical_scroll_thumb_to_middle(
    HWND window,
    bool infer_wine_geometry)
{
    HWND scrollbar = scrollbar_window_for_host(window);
    SCROLLBARINFO bar;
    INPUT input[4];
    int virtual_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int virtual_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int thumb_top;
    int thumb_bottom;
    int start_x;
    int start_y;
    int target_y;
    size_t index;

    if (scrollbar == terminal_input_window_for_host(window)) {
        RECT client;
        LONG width = scrollbar_width_for_host(window);
        if (!GetClientRect(scrollbar, &client) || width <= 0 ||
            client.right <= width || client.bottom <= 0) return false;
        /* The fixture displays 24 of 96 rows and begins at the live end.
         * Drag the center of its visible final-quarter thumb to mid-track. */
        int x = client.right - width / 2;
        int from_y = client.bottom * 7 / 8;
        int to_y = client.bottom / 2;
        return PostMessageW(scrollbar, WM_LBUTTONDOWN, MK_LBUTTON,
                            MAKELPARAM(x, from_y)) != 0 &&
               PostMessageW(scrollbar, WM_MOUSEMOVE, MK_LBUTTON,
                            MAKELPARAM(x, to_y)) != 0 &&
               PostMessageW(scrollbar, WM_LBUTTONUP, 0,
                            MAKELPARAM(x, to_y)) != 0;
    }
    if (scrollbar != window) {
        return post_scrollbar_control_drag_to_middle(scrollbar);
    }
    ZeroMemory(&bar, sizeof(bar));
    bar.cbSize = sizeof(bar);
    if (virtual_width <= 1 || virtual_height <= 1) {
        return false;
    }
    if (GetScrollBarInfo(
            window,
            OBJID_VSCROLL,
            &bar)) {
        thumb_top = bar.rcScrollBar.top + bar.xyThumbTop;
        thumb_bottom = bar.rcScrollBar.top + bar.xyThumbBottom;
        if (thumb_top < bar.rcScrollBar.top ||
            thumb_bottom > bar.rcScrollBar.bottom ||
            thumb_bottom <= thumb_top) {
            thumb_top = bar.xyThumbTop;
            thumb_bottom = bar.xyThumbBottom;
        }
        if (thumb_top < bar.rcScrollBar.top ||
            thumb_bottom > bar.rcScrollBar.bottom ||
            thumb_bottom <= thumb_top) {
            return false;
        }
    } else {
        RECT client;
        POINT top_left;
        POINT bottom_right;
        int arrow_length = GetSystemMetrics(SM_CYVSCROLL);
        int minimum_thumb_length = GetSystemMetrics(SM_CYVTHUMB);
        int track_length;
        int thumb_length;

        /* Wine keeps standard scrollbar metadata in the owning process,
         * so derive this fixture's 24-of-96 live thumb from native metrics. */
        if (!infer_wine_geometry || arrow_length <= 0 ||
            GetSystemMetrics(SM_CXVSCROLL) <= 0 ||
            minimum_thumb_length <= 0) {
            return false;
        }
        top_left.x = 0;
        top_left.y = 0;
        if (!GetClientRect(window, &client)) {
            return false;
        }
        bottom_right.x = client.right;
        bottom_right.y = client.bottom;
        if (!ClientToScreen(window, &top_left) ||
            !ClientToScreen(window, &bottom_right)) {
            return false;
        }
        bar.rcScrollBar.left = bottom_right.x;
        bar.rcScrollBar.top = top_left.y;
        bar.rcScrollBar.right = bottom_right.x +
                                GetSystemMetrics(SM_CXVSCROLL);
        bar.rcScrollBar.bottom = bottom_right.y;
        track_length = bar.rcScrollBar.bottom - bar.rcScrollBar.top -
                       2 * arrow_length;
        thumb_length = track_length / 4;
        if (thumb_length < minimum_thumb_length) {
            thumb_length = minimum_thumb_length;
        }
        if (track_length <= thumb_length) {
            return false;
        }
        thumb_bottom = bar.rcScrollBar.bottom - arrow_length;
        thumb_top = thumb_bottom - thumb_length;
    }
    start_x = bar.rcScrollBar.left +
              (bar.rcScrollBar.right - bar.rcScrollBar.left) / 2;
    start_y = thumb_top + (thumb_bottom - thumb_top) / 2;
    target_y = bar.rcScrollBar.top +
               (bar.rcScrollBar.bottom - bar.rcScrollBar.top) / 2;

    ZeroMemory(input, sizeof(input));
    for (index = 0; index < sizeof(input) / sizeof(input[0]); index++) {
        input[index].type = INPUT_MOUSE;
        input[index].mi.dx = normalized_absolute_mouse_coordinate(
            start_x,
            virtual_left,
            virtual_width);
        input[index].mi.dy = normalized_absolute_mouse_coordinate(
            index < 2 ? start_y : target_y,
            virtual_top,
            virtual_height);
        input[index].mi.dwFlags =
            MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    }
    input[0].mi.dwFlags |= MOUSEEVENTF_MOVE;
    input[1].mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
    input[2].mi.dwFlags |= MOUSEEVENTF_MOVE;
    input[3].mi.dwFlags |= MOUSEEVENTF_LEFTUP;
    return SendInput(
               (UINT)(sizeof(input) / sizeof(input[0])),
               input,
               sizeof(input[0])) ==
           sizeof(input) / sizeof(input[0]);
}

static bool wait_for_client_size(
    HANDLE process,
    HWND window,
    LONG expected_width,
    LONG expected_height,
    bool exact)
{
    size_t attempt;

    for (attempt = 0; attempt < 250; attempt++) {
        RECT rectangle;

        if (!GetClientRect(window, &rectangle)) {
            return false;
        }
        if ((exact && rectangle.right == expected_width &&
             rectangle.bottom == expected_height) ||
            (!exact && rectangle.right > expected_width &&
             rectangle.bottom >= expected_height)) {
            return true;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            return false;
        }
    }
    return false;
}

typedef struct rendering_window_search {
    HWND host;
    HWND window;
    uint64_t area;
    bool gpu;
} rendering_window_search;

static void consider_rendering_window(
    rendering_window_search *search,
    HWND window,
    bool gpu)
{
    RECT rectangle;
    uint64_t area;

    if (!IsWindowVisible(window) || !GetClientRect(window, &rectangle) ||
        rectangle.right <= 0 || rectangle.bottom <= 0) {
        return;
    }
    area = (uint64_t)(unsigned long)rectangle.right *
           (uint64_t)(unsigned long)rectangle.bottom;
    if (area > search->area) {
        search->window = window;
        search->area = area;
        search->gpu = gpu;
    }
}

static BOOL CALLBACK find_presentation_child(
    HWND window,
    LPARAM parameter)
{
    rendering_window_search *search =
        (rendering_window_search *)parameter;

    wchar_t class_name[128];
    bool named = GetClassNameW(window, class_name, 128) != 0;
    bool gpu = named && (wcsncmp(class_name, L"boxrig_wgl_", 11) == 0 ||
                        wcsncmp(class_name, L"boxrig_d3d11_", 13) == 0);
    if (((DWORD)GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED) != 0 ||
        gpu || (named && wcscmp(class_name, L"boxrig_win32_transparency_surface") == 0)) {
        consider_rendering_window(search, window, gpu);
    }
    return TRUE;
}

static BOOL CALLBACK find_owned_rendering_window(
    HWND window,
    LPARAM parameter)
{
    rendering_window_search *search =
        (rendering_window_search *)parameter;

    if (GetWindow(window, GW_OWNER) == search->host) {
        consider_rendering_window(search, window, false);
    }
    return TRUE;
}

static HWND rendering_window_for_host(HWND host, bool *gpu_out)
{
    rendering_window_search search;

    search.host = host;
    search.window = NULL;
    search.area = 0;
    search.gpu = false;
    (void)EnumChildWindows(
        host,
        find_presentation_child,
        (LPARAM)&search);
    if (search.window != NULL) {
        *gpu_out = search.gpu;
        return search.window;
    }
    (void)EnumWindows(find_owned_rendering_window, (LPARAM)&search);
    *gpu_out = search.gpu;
    return search.window == NULL ? host : search.window;
}

typedef struct client_observation {
    HWND window;
    HWND context_window;
    RECT rectangle;
    POINT source;
} client_observation;

static bool prepare_client_observation(
    HWND host,
    client_observation *observation)
{
    HWND scrollbar;
    bool gpu;

    ZeroMemory(observation, sizeof(*observation));
    observation->window = rendering_window_for_host(host, &gpu);
    observation->context_window = observation->window;
    if (!GetClientRect(observation->window, &observation->rectangle) ||
        observation->rectangle.right <= 0 ||
        observation->rectangle.bottom <= 0) {
        return false;
    }
    scrollbar = scrollbar_window_for_host(host);
    if (scrollbar != observation->window &&
        scrollbar != terminal_input_window_for_host(host) &&
        IsChild(observation->window, scrollbar) && IsWindowVisible(scrollbar)) {
        RECT rectangle;
        POINT origin;

        if (!GetWindowRect(scrollbar, &rectangle)) return false;
        origin.x = rectangle.left;
        origin.y = rectangle.top;
        if (!ScreenToClient(observation->window, &origin)) return false;
        /* A borrowed root includes its standard scrollbar child in the
         * client rectangle; observe only the terminal's allocated view. */
        if (origin.x <= 0 || origin.x > observation->rectangle.right) return false;
        observation->rectangle.right = origin.x;
    } else if (scrollbar == terminal_input_window_for_host(host)) {
        LONG width = scrollbar_width_for_host(host);
        if (width <= 0 || width >= observation->rectangle.right) return false;
        observation->rectangle.right -= width;
    }
    if (gpu || ((DWORD)GetWindowLongPtrW(
             observation->window,
             GWL_EXSTYLE) &
         WS_EX_LAYERED) != 0) {
        /* GPU and UpdateLayeredWindow content is observable after composition through
         * the screen DC, at the rendering child's client-screen origin. */
        if (!ClientToScreen(observation->window, &observation->source)) {
            return false;
        }
        observation->context_window = NULL;
    }
    return true;
}

static bool capture_client_rectangle_hash(
    HWND window,
    const RECT *capture_rectangle,
    const char *capture_name,
    uint32_t *origin_rgb_out,
    uint64_t *hash_out,
    size_t *different_pixel_count_out)
{
    client_observation observation;
    RECT rectangle;
    BITMAPINFO information;
    HDC window_context = NULL;
    HDC memory_context = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ previous = NULL;
    uint8_t *pixels = NULL;
    size_t byte_count;
    size_t index;
    size_t different_pixel_count = 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    LONG width;
    LONG height;
    bool result = false;

    if (!prepare_client_observation(window, &observation)) {
        return false;
    }
    rectangle = capture_rectangle == NULL ? observation.rectangle
                                          : *capture_rectangle;
    if (rectangle.left < observation.rectangle.left ||
        rectangle.top < observation.rectangle.top ||
        rectangle.right > observation.rectangle.right ||
        rectangle.bottom > observation.rectangle.bottom ||
        rectangle.right <= rectangle.left ||
        rectangle.bottom <= rectangle.top) {
        return false;
    }
    width = rectangle.right - rectangle.left;
    height = rectangle.bottom - rectangle.top;
    if ((size_t)width > SIZE_MAX / 4 / (size_t)height) {
        return false;
    }
    ZeroMemory(&information, sizeof(information));
    information.bmiHeader.biSize = sizeof(information.bmiHeader);
    information.bmiHeader.biWidth = width;
    information.bmiHeader.biHeight = -height;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    window_context = GetDC(observation.context_window);
    if (window_context == NULL) {
        goto cleanup;
    }
    memory_context = CreateCompatibleDC(window_context);
    if (memory_context == NULL) {
        goto cleanup;
    }
    bitmap = CreateDIBSection(
        window_context,
        &information,
        DIB_RGB_COLORS,
        (void **)&pixels,
        NULL,
        0);
    if (bitmap == NULL || pixels == NULL) {
        goto cleanup;
    }
    previous = SelectObject(memory_context, bitmap);
    if (previous == NULL || previous == HGDI_ERROR ||
        !BitBlt(
            memory_context,
            0,
            0,
            width,
            height,
            window_context,
            observation.source.x + rectangle.left,
            observation.source.y + rectangle.top,
            SRCCOPY) ||
        !GdiFlush()) {
        goto cleanup;
    }
    byte_count = (size_t)width * (size_t)height * 4;
    if (capture_name != NULL) {
        const char *prefix = getenv("BOXRIG_TEST_MODAL_RESIZE_CAPTURE_PREFIX");

        if (prefix != NULL && prefix[0] != '\0') {
            BITMAPFILEHEADER header;
            char path[1024];
            FILE *file;
            bool written;
            int length = snprintf(
                path,
                sizeof(path),
                "%s-%u-%s.bmp",
                prefix,
                (unsigned int)(sizeof(void *) * 8u),
                capture_name);

            if (length < 0 || (size_t)length >= sizeof(path) ||
                byte_count > MAXDWORD - sizeof(header) -
                                 sizeof(information.bmiHeader)) {
                goto cleanup;
            }
            ZeroMemory(&header, sizeof(header));
            header.bfType = 0x4d42;
            header.bfOffBits = sizeof(header) + sizeof(information.bmiHeader);
            header.bfSize = header.bfOffBits + (DWORD)byte_count;
            file = fopen(path, "wb");
            if (file == NULL) {
                goto cleanup;
            }
            /* Preserve every captured byte, including the unused high byte,
             * so a failed visual comparison can be diagnosed independently. */
            written = fwrite(&header, sizeof(header), 1u, file) == 1u &&
                      fwrite(
                          &information.bmiHeader,
                          sizeof(information.bmiHeader),
                          1u,
                          file) == 1u &&
                      fwrite(pixels, byte_count, 1u, file) == 1u;
            if (fclose(file) != 0) {
                written = false;
            }
            if (!written) {
                goto cleanup;
            }
        }
    }
    for (index = 0; index < byte_count; index++) {
        hash ^= pixels[index];
        hash *= UINT64_C(1099511628211);
    }
    for (index = 4; index < byte_count; index += 4) {
        if (pixels[index] != pixels[0] ||
            pixels[index + 1] != pixels[1] ||
            pixels[index + 2] != pixels[2] ||
            pixels[index + 3] != pixels[3]) {
            different_pixel_count++;
        }
    }
    if (origin_rgb_out != NULL) {
        *origin_rgb_out = ((uint32_t)pixels[2] << 16) |
                          ((uint32_t)pixels[1] << 8) | pixels[0];
    }
    *hash_out = hash;
    *different_pixel_count_out = different_pixel_count;
    result = true;

cleanup:
    if (previous != NULL && previous != HGDI_ERROR) {
        (void)SelectObject(memory_context, previous);
    }
    if (bitmap != NULL) {
        (void)DeleteObject(bitmap);
    }
    if (memory_context != NULL) {
        (void)DeleteDC(memory_context);
    }
    if (window_context != NULL) {
        (void)ReleaseDC(observation.context_window, window_context);
    }
    return result;
}

static bool capture_client_hash(
    HWND window,
    uint64_t *hash_out,
    size_t *different_pixel_count_out)
{
    return capture_client_rectangle_hash(
        window,
        NULL,
        NULL,
        NULL,
        hash_out,
        different_pixel_count_out);
}

static bool wait_for_stable_client_hash(
    HANDLE process,
    HWND window,
    uint64_t *hash_out,
    bool require_rendered_content)
{
    uint64_t previous = 0;
    size_t stable_count = 0;
    size_t attempt;

    for (attempt = 0; attempt < 250; attempt++) {
        uint64_t current;
        size_t different_pixel_count;

        if (!RedrawWindow(
                window,
                NULL,
                NULL,
                RDW_INVALIDATE | RDW_UPDATENOW) ||
            !capture_client_hash(
                window,
                &current,
                &different_pixel_count)) {
            return false;
        }
        if (stable_count != 0 && current == previous) {
            stable_count++;
            if (stable_count == 3 &&
                (!require_rendered_content ||
                 different_pixel_count > 100)) {
                *hash_out = current;
                return true;
            }
        } else {
            stable_count = 1;
            previous = current;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            return false;
        }
    }
    return false;
}

static bool wait_for_changed_stable_client_hash(
    HANDLE process,
    HWND window,
    uint64_t baseline)
{
    uint64_t previous = baseline;
    size_t stable_count = 0;
    size_t attempt;

    for (attempt = 0; attempt < 250; attempt++) {
        uint64_t current;
        size_t different_pixel_count;

        if (!RedrawWindow(
                window,
                NULL,
                NULL,
                RDW_INVALIDATE | RDW_UPDATENOW) ||
            !capture_client_hash(
                window,
                &current,
                &different_pixel_count)) {
            return false;
        }
        if (current != baseline) {
            if (stable_count != 0 && current == previous) {
                stable_count++;
                if (stable_count == 3) {
                    return true;
                }
            } else {
                previous = current;
                stable_count = 1;
            }
        } else {
            stable_count = 0;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            return false;
        }
    }
    return false;
}

static bool color_channel_near(uint8_t actual, uint8_t expected)
{
    enum { GRAPHICS_COLOR_CHANNEL_TOLERANCE = 16 };

    /* Filtering varies slightly with the physical terminal cell geometry. */
    return actual > expected
               ? actual - expected <= GRAPHICS_COLOR_CHANNEL_TOLERANCE
               : expected - actual <= GRAPHICS_COLOR_CHANNEL_TOLERANCE;
}

static bool client_has_graphics_fixture(HWND window, bool report_failure)
{
    static const struct expected_color {
        size_t column;
        size_t row;
        size_t columns;
        size_t rows;
        size_t source_x;
        size_t source_y;
        size_t source_width;
        size_t source_height;
        uint8_t red;
        uint8_t green;
        uint8_t blue;
    } expected[] = {
        {0, 3, 4, 2, 4, 1, 16, 16, 255, 220, 64},
        {0, 3, 4, 2, 10, 10, 16, 16, 224, 48, 48},
        {0, 3, 4, 2, 7, 14, 16, 16, 40, 120, 240},
        {6, 3, 2, 2, 2, 10, 8, 16, 224, 48, 48},
        {0, 6, 4, 2, 10, 10, 16, 16, 24, 112, 48},
        {6, 6, 4, 2, 10, 10, 16, 16, 144, 64, 224},
        {0, 9, 4, 2, 10, 10, 16, 16, 224, 176, 40},
        {6, 9, 4, 2, 10, 10, 16, 16, 0, 0, 0},
        {0, 12, 4, 2, 10, 10, 16, 16, 240, 112, 32},
        {0, 15, 4, 2, 10, 10, 16, 16, 32, 192, 208},
        {1, 21, 4, 1, 7, 2, 16, 8, 240, 248, 255},
    };
    client_observation observation;
    BITMAPINFO information;
    HDC window_context = NULL;
    HDC memory_context = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ previous = NULL;
    uint8_t *pixels = NULL;
    size_t cell_width;
    size_t cell_height;
    size_t index;
    bool result = false;

    if (!prepare_client_observation(window, &observation) ||
        observation.rectangle.right % 80 != 0 ||
        observation.rectangle.bottom % 24 != 0) {
        if (report_failure) {
            fprintf(
                stderr,
                "graphics client geometry mismatch: %ldx%ld\n",
                observation.rectangle.right,
                observation.rectangle.bottom);
        }
        return false;
    }
    cell_width = (size_t)observation.rectangle.right / 80;
    cell_height = (size_t)observation.rectangle.bottom / 24;
    if (cell_width < 4 || cell_height < 2) {
        return false;
    }
    ZeroMemory(&information, sizeof(information));
    information.bmiHeader.biSize = sizeof(information.bmiHeader);
    information.bmiHeader.biWidth = observation.rectangle.right;
    information.bmiHeader.biHeight = -observation.rectangle.bottom;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    window_context = GetDC(observation.context_window);
    if (window_context == NULL) {
        goto cleanup;
    }
    memory_context = CreateCompatibleDC(window_context);
    if (memory_context == NULL) {
        goto cleanup;
    }
    bitmap = CreateDIBSection(
        window_context,
        &information,
        DIB_RGB_COLORS,
        (void **)&pixels,
        NULL,
        0);
    if (bitmap == NULL || pixels == NULL) {
        goto cleanup;
    }
    previous = SelectObject(memory_context, bitmap);
    if (previous == NULL || previous == HGDI_ERROR ||
        !BitBlt(
            memory_context,
            0,
            0,
            observation.rectangle.right,
            observation.rectangle.bottom,
            window_context,
            observation.source.x,
            observation.source.y,
            SRCCOPY) ||
        !GdiFlush()) {
        goto cleanup;
    }
    result = true;
    for (index = 0; index < sizeof(expected) / sizeof(expected[0]); index++) {
        size_t destination_width = expected[index].columns * cell_width;
        size_t destination_height = expected[index].rows * cell_height;
        size_t x = expected[index].column * cell_width +
                   (expected[index].source_x * 2 + 1) * destination_width /
                       (expected[index].source_width * 2);
        size_t y = expected[index].row * cell_height +
                   (expected[index].source_y * 2 + 1) * destination_height /
                       (expected[index].source_height * 2);
        size_t offset;

        offset = (y * (size_t)observation.rectangle.right + x) * 4;
        if (!color_channel_near(pixels[offset], expected[index].blue) ||
            !color_channel_near(
                pixels[offset + 1],
                expected[index].green) ||
            !color_channel_near(
                pixels[offset + 2],
                expected[index].red)) {
            if (report_failure) {
                fprintf(
                    stderr,
                    "graphics pixel mismatch: cell=%lux%lu at=%lu,%lu "
                    "pixel=%lu,%lu expected=%u,%u,%u actual=%u,%u,%u\n",
                    (unsigned long)cell_width,
                    (unsigned long)cell_height,
                    (unsigned long)expected[index].column,
                    (unsigned long)expected[index].row,
                    (unsigned long)x,
                    (unsigned long)y,
                    (unsigned int)expected[index].red,
                    (unsigned int)expected[index].green,
                    (unsigned int)expected[index].blue,
                    (unsigned int)pixels[offset + 2],
                    (unsigned int)pixels[offset + 1],
                    (unsigned int)pixels[offset]);
            }
            result = false;
            break;
        }
    }

cleanup:
    if (previous != NULL && previous != HGDI_ERROR) {
        (void)SelectObject(memory_context, previous);
    }
    if (bitmap != NULL) {
        (void)DeleteObject(bitmap);
    }
    if (memory_context != NULL) {
        (void)DeleteDC(memory_context);
    }
    if (window_context != NULL) {
        (void)ReleaseDC(observation.context_window, window_context);
    }
    return result;
}

static bool wait_for_graphics_fixture(
    HANDLE process,
    HWND window)
{
    size_t attempt;

    for (attempt = 0; attempt < 250; attempt++) {
        if (!RedrawWindow(
                window,
                NULL,
                NULL,
                RDW_INVALIDATE | RDW_UPDATENOW)) {
            return false;
        }
        if (client_has_graphics_fixture(window, false)) {
            return true;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            (void)client_has_graphics_fixture(window, true);
            return false;
        }
    }
    (void)client_has_graphics_fixture(window, true);
    return false;
}

static BOOL CALLBACK find_process_window(HWND window, LPARAM parameter)
{
    window_search *search = (window_search *)parameter;
    DWORD process_id = 0;

    (void)GetWindowThreadProcessId(window, &process_id);
    if (process_id == search->process_id && IsWindowVisible(window) &&
        ((DWORD)GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) ==
            WS_CAPTION) {
        search->window = window;
        return FALSE;
    }
    return TRUE;
}

static wchar_t *make_command_line(
    const wchar_t *sample_path,
    const wchar_t *helper_path)
{
    size_t capacity = wcslen(sample_path) + wcslen(helper_path) + 16;
    wchar_t *command_line = calloc(capacity, sizeof(*command_line));
    int length;

    if (command_line == NULL) {
        return NULL;
    }
    length = swprintf(
        command_line,
        capacity,
        L"\"%ls\" -- \"%ls\"",
        sample_path,
        helper_path);
    if (length < 0 || (size_t)length >= capacity) {
        free(command_line);
        return NULL;
    }
    return command_line;
}

static wchar_t *make_default_command_line(const wchar_t *sample_path)
{
    size_t capacity = wcslen(sample_path) + 3;
    wchar_t *command_line = calloc(capacity, sizeof(*command_line));
    int length;

    if (command_line == NULL) {
        return NULL;
    }
    length = swprintf(command_line, capacity, L"\"%ls\"", sample_path);
    if (length < 0 || (size_t)length >= capacity) {
        free(command_line);
        return NULL;
    }
    return command_line;
}

static wchar_t *make_emitter_command_line(
    const wchar_t *sample_path,
    const wchar_t *emitter_path,
    const wchar_t *case_name,
    bool hold)
{
    const wchar_t *hold_argument = hold ? L"" : L" --no-hold";
    size_t capacity = wcslen(sample_path) + wcslen(emitter_path) +
                      wcslen(case_name) + wcslen(hold_argument) + 32;
    wchar_t *command_line = calloc(capacity, sizeof(*command_line));
    int length;

    if (command_line == NULL) {
        return NULL;
    }
    length = swprintf(
        command_line,
        capacity,
        L"\"%ls\" -- \"%ls\" -t \"%ls\"%ls",
        sample_path,
        emitter_path,
        case_name,
        hold_argument);
    if (length < 0 || (size_t)length >= capacity) {
        free(command_line);
        return NULL;
    }
    return command_line;
}

static bool wait_for_window_title(
    HANDLE process,
    HWND window,
    const wchar_t *expected_title)
{
    size_t attempt;

    for (attempt = 0; attempt < 250; attempt++) {
        wchar_t title[64];

        if (GetWindowTextW(
                window,
                title,
                (int)(sizeof(title) / sizeof(title[0]))) > 0 &&
            wcscmp(title, expected_title) == 0) {
            return true;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            return false;
        }
    }
    return false;
}

static bool wait_for_emitter_window(
    HANDLE process,
    DWORD process_id,
    HWND *window_out)
{
    window_search search;
    size_t attempt;

    *window_out = NULL;
    if (WaitForInputIdle(process, 10000) != 0) {
        return false;
    }
    search.process_id = process_id;
    search.window = NULL;
    for (attempt = 0; attempt < 250 && search.window == NULL; attempt++) {
        (void)EnumWindows(find_process_window, (LPARAM)&search);
        if (search.window == NULL &&
            WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            return false;
        }
    }
    *window_out = search.window;
    return search.window != NULL;
}

static bool start_emitter_sample(
    const wchar_t *sample_path,
    const wchar_t *emitter_path,
    const wchar_t *case_name,
    bool hold,
    bool allow_notifications,
    const wchar_t *notification_log_path,
    HANDLE *process_out,
    HWND *window_out)
{
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    wchar_t *command_line = make_emitter_command_line(
        sample_path,
        emitter_path,
        case_name,
        hold);
    bool created;

    *process_out = NULL;
    *window_out = NULL;
    if (command_line == NULL) {
        return false;
    }
    if (notification_log_path != NULL) {
        if (!SetEnvironmentVariableW(
                L"BOXRIG_NOTIFICATION_TEST_LOG",
                notification_log_path)) {
            free(command_line);
            return false;
        }
    } else {
        (void)SetEnvironmentVariableW(
            L"BOXRIG_NOTIFICATION_TEST_LOG",
            NULL);
    }
    if (allow_notifications) {
        if (!SetEnvironmentVariableW(
                L"BOXRIG_ALLOW_NOTIFICATIONS",
                L"1")) {
            (void)SetEnvironmentVariableW(
                L"BOXRIG_NOTIFICATION_TEST_LOG",
                NULL);
            free(command_line);
            return false;
        }
    } else {
        (void)SetEnvironmentVariableW(
            L"BOXRIG_ALLOW_NOTIFICATIONS",
            NULL);
    }
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));
    created = CreateProcessW(
                  NULL,
                  command_line,
                  NULL,
                  NULL,
                  FALSE,
                  0,
                  NULL,
                  NULL,
                  &startup,
                  &process) != 0;
    free(command_line);
    if (!created) {
        (void)SetEnvironmentVariableW(L"BOXRIG_ALLOW_NOTIFICATIONS", NULL);
        (void)SetEnvironmentVariableW(
            L"BOXRIG_NOTIFICATION_TEST_LOG",
            NULL);
        return false;
    }
    CloseHandle(process.hThread);
    if (!wait_for_emitter_window(
            process.hProcess,
            process.dwProcessId,
            window_out)) {
        (void)SetEnvironmentVariableW(L"BOXRIG_ALLOW_NOTIFICATIONS", NULL);
        (void)SetEnvironmentVariableW(
            L"BOXRIG_NOTIFICATION_TEST_LOG",
            NULL);
        (void)TerminateProcess(process.hProcess, 1);
        (void)WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hProcess);
        return false;
    }
    (void)SetEnvironmentVariableW(L"BOXRIG_ALLOW_NOTIFICATIONS", NULL);
    (void)SetEnvironmentVariableW(
        L"BOXRIG_NOTIFICATION_TEST_LOG",
        NULL);
    *process_out = process.hProcess;
    return true;
}

static bool start_default_sample(
    const wchar_t *sample_path,
    HANDLE *process_out,
    HWND *window_out)
{
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    wchar_t *command_line = make_default_command_line(sample_path);
    bool created;

    *process_out = NULL;
    *window_out = NULL;
    if (command_line == NULL) {
        return false;
    }
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));
    created = CreateProcessW(
                  NULL,
                  command_line,
                  NULL,
                  NULL,
                  FALSE,
                  0,
                  NULL,
                  NULL,
                  &startup,
                  &process) != 0;
    free(command_line);
    if (!created) {
        return false;
    }
    CloseHandle(process.hThread);
    if (!wait_for_emitter_window(
            process.hProcess,
            process.dwProcessId,
            window_out)) {
        (void)TerminateProcess(process.hProcess, 1);
        (void)WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hProcess);
        return false;
    }
    *process_out = process.hProcess;
    return true;
}

typedef struct sizing_edge_case {
    WPARAM edge;
    bool quantize_width;
    bool quantize_height;
    bool move_left;
    bool move_top;
} sizing_edge_case;

static bool send_sizing_rectangle(
    HWND window,
    WPARAM edge,
    RECT *rectangle)
{
    DWORD_PTR result = 0;

    return SendMessageTimeoutW(
               window,
               WM_SIZING,
               edge,
               (LPARAM)rectangle,
               SMTO_ABORTIFHUNG,
               5000,
               &result) != 0 &&
           result == TRUE;
}

static bool send_window_message(
    HWND window,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter)
{
    DWORD_PTR result;

    return SendMessageTimeoutW(
               window,
               message,
               word_parameter,
               long_parameter,
               SMTO_ABORTIFHUNG,
               5000,
               &result) != 0;
}

static bool send_key_message(
    HWND window,
    UINT virtual_key,
    bool released)
{
    return send_window_message(
        window,
        released ? WM_KEYUP : WM_KEYDOWN,
        virtual_key,
        key_parameter(virtual_key, released));
}

static bool send_zoom_in(HWND window)
{
    return send_key_message(window, VK_CONTROL, false) &&
           send_key_message(window, VK_ADD, false) &&
           send_key_message(window, VK_ADD, true) &&
           send_key_message(window, VK_CONTROL, true);
}

static bool send_zoom_out(HWND window)
{
    return send_key_message(window, VK_CONTROL, false) &&
           send_key_message(window, VK_SUBTRACT, false) &&
           send_key_message(window, VK_SUBTRACT, true) &&
           send_key_message(window, VK_CONTROL, true);
}

static bool send_zoom_reset(HWND window)
{
    return send_key_message(window, VK_CONTROL, false) &&
           send_key_message(window, VK_NUMPAD0, false) &&
           send_key_message(window, VK_NUMPAD0, true) &&
           send_key_message(window, VK_CONTROL, true);
}

static bool query_grid_title(
    HANDLE process,
    HWND window,
    const wchar_t *expected)
{
    wchar_t actual[64];
    wchar_t reply[64];

    if (GetWindowTextW(window, actual, (int)(sizeof(actual) / sizeof(actual[0]))) == 0 ||
        swprintf(reply, sizeof(reply) / sizeof(reply[0]), L"%ls", expected) < 0) {
        return false;
    }
    /* ConPTY can suppress an unchanged title. Alternate the reply prefix so
     * each query acknowledges fresh child input even when the grid is unchanged. */
    reply[0] = actual[0] == L'g' ? L'G' : L'g';
    if (SetWindowTextW(window, L"awaiting-sizing-grid") != 0 &&
        commit_character(window, reply[0]) &&
        wait_for_window_title(process, window, reply)) {
        return true;
    }
    actual[0] = L'\0';
    (void)GetWindowTextW(
        window,
        actual,
        (int)(sizeof(actual) / sizeof(actual[0])));
    fwprintf(
        stderr,
        L"expected sizing grid %ls but observed %ls\n",
        reply,
        actual);
    return false;
}

static bool rectangles_equal(const RECT *left, const RECT *right)
{
    return left->left == right->left && left->top == right->top &&
           left->right == right->right &&
           left->bottom == right->bottom;
}

static bool run_interactive_sizing_contract(
    HANDLE process,
    HWND window,
    bool wine_runtime)
{
    static const sizing_edge_case edge_cases[] = {
        {WMSZ_LEFT, true, false, true, false},
        {WMSZ_RIGHT, true, false, false, false},
        {WMSZ_TOP, false, true, false, true},
        {WMSZ_BOTTOM, false, true, false, false},
        {WMSZ_TOPLEFT, true, true, true, true},
        {WMSZ_TOPRIGHT, true, true, false, true},
        {WMSZ_BOTTOMLEFT, true, true, true, false},
        {WMSZ_BOTTOMRIGHT, true, true, false, false},
    };
    HWND scrollbar = scrollbar_window_for_host(window);
    RECT initial_client;
    RECT initial_outer;
    RECT current_client;
    RECT zoomed_client;
    LONG frame_width;
    LONG frame_height;
    LONG scrollbar_width;
    LONG cell_width;
    LONG cell_height;
    LONG zoomed_cell_width;
    LONG zoomed_cell_height;
    LONG candidate_outer_width;
    LONG candidate_outer_height;
    LONG target_outer_width;
    LONG target_outer_height;
    LONG superseding_client_width;
    LONG superseding_client_height;
    size_t index;
    const char *stage = "read initial geometry";
    bool passed = false;

    if (scrollbar == NULL ||
        !GetClientRect(window, &initial_client) ||
        !GetWindowRect(window, &initial_outer) ||
        initial_client.right <= 0 || initial_client.bottom <= 0 ||
        initial_outer.right <= initial_outer.left ||
        initial_outer.bottom <= initial_outer.top) {
        goto cleanup;
    }
    frame_width =
        initial_outer.right - initial_outer.left - initial_client.right;
    frame_height =
        initial_outer.bottom - initial_outer.top - initial_client.bottom;
    scrollbar_width = scrollbar_width_for_host(window);
    if (frame_width < 0 || frame_height < 0 || scrollbar_width <= 0 ||
        initial_client.right <= scrollbar_width ||
        (initial_client.right - scrollbar_width) % 80 != 0 ||
        initial_client.bottom % 24 != 0) {
        goto cleanup;
    }
    cell_width = (initial_client.right - scrollbar_width) / 80;
    cell_height = initial_client.bottom / 24;
    if (cell_width <= 1 || cell_height <= 1) {
        goto cleanup;
    }
    candidate_outer_width =
        frame_width + scrollbar_width + 83 * cell_width + cell_width / 2;
    candidate_outer_height =
        frame_height + 27 * cell_height + cell_height / 2;
    target_outer_width = frame_width + scrollbar_width + 83 * cell_width;
    target_outer_height = frame_height + 27 * cell_height;

    stage = "quantize every sizing edge";
    for (index = 0;
         index < sizeof(edge_cases) / sizeof(edge_cases[0]);
         index++) {
        RECT candidate;
        RECT expected;

        candidate.left = 240;
        candidate.top = 180;
        candidate.right = candidate.left + candidate_outer_width;
        candidate.bottom = candidate.top + candidate_outer_height;
        expected = candidate;
        if (edge_cases[index].quantize_width) {
            if (edge_cases[index].move_left) {
                expected.left = expected.right - target_outer_width;
            } else {
                expected.right = expected.left + target_outer_width;
            }
        }
        if (edge_cases[index].quantize_height) {
            if (edge_cases[index].move_top) {
                expected.top = expected.bottom - target_outer_height;
            } else {
                expected.bottom = expected.top + target_outer_height;
            }
        }
        if (!send_sizing_rectangle(
                window,
                edge_cases[index].edge,
                &candidate) ||
            !rectangles_equal(&candidate, &expected)) {
            goto cleanup;
        }
    }

    stage = "clamp sizing to one cell";
    {
        RECT candidate;
        RECT expected;

        candidate.left = 240;
        candidate.top = 180;
        candidate.right = candidate.left + frame_width + scrollbar_width +
                          cell_width / 2;
        candidate.bottom = candidate.top + frame_height + cell_height / 2;
        expected = candidate;
        expected.right = expected.left + frame_width + scrollbar_width +
                         cell_width;
        expected.bottom = expected.top + frame_height + cell_height;
        if (!send_sizing_rectangle(
                window,
                WMSZ_BOTTOMRIGHT,
                &candidate) ||
            !rectangles_equal(&candidate, &expected)) {
            goto cleanup;
        }
    }

    stage = "defer application resize during modal sizing";
    if (!send_window_message(window, WM_ENTERSIZEMOVE, 0, 0) ||
        !send_zoom_in(window) ||
        !query_grid_title(process, window, L"grid-24-80") ||
        !GetClientRect(window, &current_client) ||
        !rectangles_equal(&current_client, &initial_client)) {
        goto cleanup;
    }
    stage = "apply deferred resize after modal sizing";
    if (!send_window_message(window, WM_EXITSIZEMOVE, 0, 0) ||
        !wait_for_client_size(
            process,
            window,
            initial_client.right,
            initial_client.bottom,
            false) ||
        !GetClientRect(window, &zoomed_client) ||
        zoomed_client.right <= scrollbar_width ||
        (zoomed_client.right - scrollbar_width) % 80 != 0 ||
        zoomed_client.bottom % 24 != 0) {
        goto cleanup;
    }
    zoomed_cell_width = (zoomed_client.right - scrollbar_width) / 80;
    zoomed_cell_height = zoomed_client.bottom / 24;
    if (zoomed_cell_width <= cell_width ||
        zoomed_cell_height < cell_height ||
        !send_zoom_reset(window) ||
        !wait_for_client_size(
            process,
            window,
            initial_client.right,
            initial_client.bottom,
            true) ||
        !query_grid_title(process, window, L"grid-24-80")) {
        goto cleanup;
    }

    superseding_client_width = scrollbar_width + 82 * zoomed_cell_width;
    superseding_client_height = 25 * zoomed_cell_height;
    stage = "enter sizing before superseding deferred resize";
    if (!send_window_message(window, WM_ENTERSIZEMOVE, 0, 0)) {
        goto cleanup;
    }
    stage = "defer zoom before superseding resize";
    if (!send_zoom_in(window)) {
        goto cleanup;
    }
    stage = "observe deferred zoom before superseding resize";
    if (!query_grid_title(process, window, L"grid-24-80")) {
        goto cleanup;
    }
    stage = "apply superseding user allocation";
    if (!SetWindowPos(
            window,
            NULL,
            0,
            0,
            frame_width + superseding_client_width,
            frame_height + superseding_client_height,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        goto cleanup;
    }
    stage = "observe superseding user allocation";
    if (!wait_for_client_size(
            process,
            window,
            superseding_client_width,
            superseding_client_height,
            true)) {
        goto cleanup;
    }
    stage = "deliver superseding user allocation";
    if (!send_window_message(
            window,
            WM_SIZE,
            SIZE_RESTORED,
            MAKELPARAM(
                superseding_client_width,
                superseding_client_height))) {
        goto cleanup;
    }
    stage = "exit sizing after superseding user allocation";
    if (!send_window_message(window, WM_EXITSIZEMOVE, 0, 0)) {
        goto cleanup;
    }
    stage = "retain superseding grid after sizing";
    if (!query_grid_title(
            process,
            window,
            wine_runtime ? L"grid-24-80" : L"grid-25-82")) {
        goto cleanup;
    }
    if (wine_runtime) {
        printf(
            "SKIP Win32 interactive sizing ConPTY grid query: "
            "Wine retained its initial console buffer\n");
    }
    stage = "retain superseding geometry after sizing";
    if (!GetClientRect(window, &current_client) ||
        current_client.right != superseding_client_width ||
        current_client.bottom != superseding_client_height) {
        goto cleanup;
    }

    stage = "restore initial font and grid";
    if (!send_zoom_reset(window) ||
        !wait_for_client_size(
            process,
            window,
            scrollbar_width + 82 * cell_width,
            25 * cell_height,
            true) ||
        !SetWindowPos(
            window,
            NULL,
            0,
            0,
            initial_outer.right - initial_outer.left,
            initial_outer.bottom - initial_outer.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) ||
        !wait_for_client_size(
            process,
            window,
            initial_client.right,
            initial_client.bottom,
            true) ||
        !query_grid_title(process, window, L"grid-24-80")) {
        goto cleanup;
    }
    passed = true;

cleanup:
    if (!passed) {
        fprintf(stderr, "Win32 interactive sizing failed at %s\n", stage);
    }
    return passed;
}

static void initialize_absolute_mouse_input(
    INPUT *input,
    int x,
    int y,
    DWORD flags,
    int virtual_left,
    int virtual_top,
    int virtual_width,
    int virtual_height)
{
    ZeroMemory(input, sizeof(*input));
    input->type = INPUT_MOUSE;
    input->mi.dx = normalized_absolute_mouse_coordinate(
        x,
        virtual_left,
        virtual_width);
    input->mi.dy = normalized_absolute_mouse_coordinate(
        y,
        virtual_top,
        virtual_height);
    input->mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE |
                        MOUSEEVENTF_VIRTUALDESK;
}

static bool release_left_mouse_button(void)
{
    INPUT input;

    ZeroMemory(&input, sizeof(input));
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    return SendInput(1, &input, sizeof(input)) == 1;
}

static bool find_bottom_right_sizing_point(
    HWND window,
    const RECT *outer,
    POINT *point_out)
{
    int inset;

    for (inset = 1; inset <= 16; inset++) {
        POINT candidate = {
            outer->right - inset,
            outer->bottom - inset};
        DWORD_PTR result = 0;

        if (SendMessageTimeoutW(
                window,
                WM_NCHITTEST,
                0,
                (LPARAM)MAKELPARAM(
                    (WORD)candidate.x,
                    (WORD)candidate.y),
                SMTO_ABORTIFHUNG,
                5000,
                &result) != 0 &&
            result == HTBOTTOMRIGHT) {
            *point_out = candidate;
            return true;
        }
    }
    return false;
}

static bool wait_for_stable_resize_band(
    HANDLE process,
    HWND window,
    const RECT *expected_outer,
    const RECT *expected_surface,
    const RECT *band,
    const char *capture_name,
    const uint32_t *expected_origin_rgb,
    bool expected_button_down,
    bool expect_blank,
    uint64_t *hash_out)
{
    uint64_t previous = 0;
    size_t stable_count = 0;
    size_t attempt;

    for (attempt = 0; attempt < 250; attempt++) {
        client_observation observation;
        RECT current_outer;
        uint64_t current;
        uint32_t origin_rgb;
        size_t different_pixel_count;
        bool button_down =
            ((unsigned short)GetAsyncKeyState(VK_LBUTTON) & 0x8000u) != 0;

        if (button_down == expected_button_down &&
            GetWindowRect(window, &current_outer) &&
            rectangles_equal(&current_outer, expected_outer) &&
            prepare_client_observation(window, &observation) &&
            rectangles_equal(
                &observation.rectangle,
                expected_surface) &&
            capture_client_rectangle_hash(
                window,
                band,
                capture_name,
                &origin_rgb,
                &current,
                &different_pixel_count) &&
            (expected_origin_rgb == NULL ||
             origin_rgb == *expected_origin_rgb)) {
            if (stable_count != 0 && current == previous) {
                stable_count++;
                if (stable_count == 3 &&
                    (expect_blank ? different_pixel_count == 0
                                  : different_pixel_count > 100)) {
                    *hash_out = current;
                    return true;
                }
            } else {
                previous = current;
                stable_count = 1;
            }
        } else {
            stable_count = 0;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            return false;
        }
    }
    return false;
}

static bool drive_modal_resize_to_target(
    HANDLE process,
    HWND window,
    INPUT *motions,
    UINT motion_count,
    bool wine_runtime,
    int target_x,
    int target_y,
    const RECT *expected_outer,
    const RECT *expected_surface)
{
    size_t stable_count = 0;
    size_t attempt;

    for (attempt = 0; attempt < 250; attempt++) {
        client_observation observation;
        RECT current_outer;
        POINT current_pointer;

        if (stable_count == 0 &&
            SendInput(
                motion_count,
                motions,
                sizeof(motions[0])) != motion_count) {
            return false;
        }
        if (!GetCursorPos(&current_pointer) ||
            current_pointer.x != target_x ||
            current_pointer.y != target_y) {
            return false;
        }
        /* Wine's X11 nonclient frame is not a Win32 HWND under headless
         * Xvfb. Mirror the injected motion into the captured Win32 queue;
         * the actual pointer and Button1 state still come from SendInput. */
        if (stable_count == 0 && wine_runtime &&
            !PostMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, 0)) {
            return false;
        }
        if (((unsigned short)GetAsyncKeyState(VK_LBUTTON) & 0x8000u) ==
                0 ||
            !GetWindowRect(window, &current_outer) ||
            !prepare_client_observation(window, &observation)) {
            return false;
        }
        if (rectangles_equal(&current_outer, expected_outer) &&
            rectangles_equal(
                &observation.rectangle,
                expected_surface)) {
            stable_count++;
            if (stable_count == 3) {
                return true;
            }
        } else {
            stable_count = 0;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            return false;
        }
    }
    return false;
}

static bool run_modal_resize_presentation(
    HANDLE process,
    HWND window,
    bool wine_runtime)
{
    HWND scrollbar = scrollbar_window_for_host(window);
    client_observation initial_surface;
    RECT initial_client;
    RECT initial_outer;
    RECT target_outer = {0, 0, 0, 0};
    RECT target_surface = {0, 0, 0, 0};
    RECT right_band;
    RECT bottom_band;
    RECT content_band;
    client_observation current_surface;
    RECT current_outer;
    INPUT input[3];
    POINT sizing_point = {0, 0};
    POINT current_pointer;
    int virtual_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int virtual_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    LONG scrollbar_width;
    LONG cell_width;
    LONG cell_height;
    LONG width_delta;
    LONG height_delta;
    int start_x;
    int start_y;
    int target_x = 0;
    int target_y = 0;
    size_t attempt;
    uint64_t held_right_hash = 0;
    uint64_t held_bottom_hash = 0;
    uint64_t released_right_hash = 0;
    uint64_t released_bottom_hash = 0;
    uint64_t held_content_hash = 0;
    uint64_t released_content_hash = 0;
    /* Resize padding is empty space in the default rendition. Require that
     * exact background throughout the new rows and columns. The original
     * content must remain visible and match before and after Button1 release. */
    const uint32_t resized_column_background = UINT32_C(0x101010);
    const char *stage = "read modal resize baseline";
    bool have_current_outer;
    bool have_current_surface;
    bool current_button_down;
    bool passed = false;

    if (scrollbar == NULL || virtual_width <= 1 ||
        virtual_height <= 1 ||
        ((unsigned short)GetAsyncKeyState(VK_LBUTTON) & 0x8000u) != 0 ||
        !GetClientRect(window, &initial_client) ||
        !GetWindowRect(window, &initial_outer) ||
        initial_client.right <= 0 || initial_client.bottom <= 0) {
        goto cleanup;
    }
    if (!SetWindowPos(
            window,
            NULL,
            120,
            100,
            initial_outer.right - initial_outer.left,
            initial_outer.bottom - initial_outer.top,
            SWP_NOACTIVATE | SWP_NOZORDER) ||
        !GetWindowRect(window, &initial_outer) ||
        initial_outer.left != 120 || initial_outer.top != 100 ||
        !query_grid_title(process, window, L"grid-24-80")) {
        goto cleanup;
    }
    scrollbar_width = scrollbar_width_for_host(window);
    if (scrollbar_width <= 0 ||
        initial_client.right <= scrollbar_width ||
        (initial_client.right - scrollbar_width) % 80 != 0 ||
        initial_client.bottom % 24 != 0) {
        goto cleanup;
    }
    cell_width = (initial_client.right - scrollbar_width) / 80;
    cell_height = initial_client.bottom / 24;
    width_delta = 8 * cell_width;
    height_delta = 4 * cell_height;
    if (cell_width <= 1 || cell_height <= 1 ||
        !commit_character(window, L'p') ||
        !wait_for_window_title(process, window, L"presentation-ready") ||
        !prepare_client_observation(window, &initial_surface)) {
        goto cleanup;
    }
    target_outer = initial_outer;
    target_outer.right += width_delta;
    target_outer.bottom += height_delta;
    target_surface = initial_surface.rectangle;
    target_surface.right += width_delta;
    target_surface.bottom += height_delta;
    right_band.left = initial_surface.rectangle.right + 2;
    right_band.top = cell_height;
    right_band.right = target_surface.right - scrollbar_width - 2;
    right_band.bottom = target_surface.bottom - cell_height;
    bottom_band.left = cell_width;
    bottom_band.top = initial_surface.rectangle.bottom + 2;
    bottom_band.right = target_surface.right - scrollbar_width - cell_width;
    bottom_band.bottom = target_surface.bottom - 2;
    content_band.left = cell_width;
    content_band.top = cell_height;
    content_band.right = initial_surface.rectangle.right - cell_width;
    content_band.bottom = initial_surface.rectangle.bottom - cell_height;
    if (right_band.right <= right_band.left ||
        right_band.bottom <= right_band.top ||
        bottom_band.right <= bottom_band.left ||
        bottom_band.bottom <= bottom_band.top) {
        goto cleanup;
    }

    stage = "activate sample for modal resize";
    if ((!SetForegroundWindow(window) && GetForegroundWindow() != window)) {
        goto cleanup;
    }
    for (attempt = 0; attempt < 250; attempt++) {
        if (GetForegroundWindow() == window) {
            break;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            goto cleanup;
        }
    }
    if (GetForegroundWindow() != window) {
        goto cleanup;
    }
    if (!find_bottom_right_sizing_point(
            window,
            &initial_outer,
            &sizing_point)) {
        goto cleanup;
    }
    start_x = sizing_point.x;
    start_y = sizing_point.y;
    target_x = start_x + width_delta;
    target_y = start_y + height_delta;
    if (target_x < virtual_left || target_y < virtual_top ||
        target_x >= virtual_left + virtual_width ||
        target_y >= virtual_top + virtual_height) {
        goto cleanup;
    }
    initialize_absolute_mouse_input(
        &input[0],
        start_x,
        start_y,
        MOUSEEVENTF_MOVE,
        virtual_left,
        virtual_top,
        virtual_width,
        virtual_height);
    initialize_absolute_mouse_input(
        &input[1],
        start_x,
        start_y,
        MOUSEEVENTF_LEFTDOWN,
        virtual_left,
        virtual_top,
        virtual_width,
        virtual_height);
    initialize_absolute_mouse_input(
        &input[2],
        target_x,
        target_y,
        MOUSEEVENTF_MOVE,
        virtual_left,
        virtual_top,
        virtual_width,
        virtual_height);

    stage = "position pointer over bottom-right sizing border";
    if (SendInput(1, &input[0], sizeof(input[0])) != 1 ||
        !GetCursorPos(&current_pointer) ||
        current_pointer.x != start_x || current_pointer.y != start_y) {
        goto cleanup;
    }
    stage = "press Button1 on bottom-right sizing border";
    if (SendInput(1, &input[1], sizeof(input[1])) != 1 ||
        ((unsigned short)GetAsyncKeyState(VK_LBUTTON) & 0x8000u) == 0) {
        goto cleanup;
    }
    /* Headless Wine exposes the nonclient frame as its desktop HWND to
     * injected hit testing. Mirror the down into the sample queue so its
     * DefWindowProc enters the real moving/sizing modal loop. The following
     * start-position move orders that entry before the target motion. */
    if (wine_runtime) {
        if (!PostMessageW(
                window,
                WM_NCLBUTTONDOWN,
                HTBOTTOMRIGHT,
                (LPARAM)MAKELPARAM((WORD)start_x, (WORD)start_y)) ||
            !PostMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, 0)) {
            goto cleanup;
        }
    }
    stage = "reach stable modal resize target";
    if (!drive_modal_resize_to_target(
            process,
            window,
            &input[2],
            1,
            wine_runtime,
            target_x,
            target_y,
            &target_outer,
            &target_surface)) {
        goto cleanup;
    }
    stage = "capture enlarged bands while Button1 remains held";
    if (!wait_for_stable_resize_band(
            process,
            window,
            &target_outer,
            &target_surface,
            &right_band,
            "held-right",
            &resized_column_background,
            true,
            true,
            &held_right_hash) ||
        !wait_for_stable_resize_band(
            process,
            window,
            &target_outer,
            &target_surface,
            &bottom_band,
            "held-bottom",
            &resized_column_background,
            true,
            true,
            &held_bottom_hash) ||
        !wait_for_stable_resize_band(
            process,
            window,
            &target_outer,
            &target_surface,
            &content_band,
            "held-content",
            NULL,
            true,
            false,
            &held_content_hash)) {
        goto cleanup;
    }
    stage = "release modal resize at unchanged geometry";
    if (!release_left_mouse_button()) {
        goto cleanup;
    }
    if (!query_grid_title(
            process,
            window,
            wine_runtime ? L"grid-24-80" : L"grid-28-88") ||
        !wait_for_stable_resize_band(
            process,
            window,
            &target_outer,
            &target_surface,
            &right_band,
            "released-right",
            &resized_column_background,
            false,
            true,
            &released_right_hash) ||
        !wait_for_stable_resize_band(
            process,
            window,
            &target_outer,
            &target_surface,
            &bottom_band,
            "released-bottom",
            &resized_column_background,
            false,
            true,
            &released_bottom_hash) ||
        !wait_for_stable_resize_band(
            process,
            window,
            &target_outer,
            &target_surface,
            &content_band,
            "released-content",
            NULL,
            false,
            false,
            &released_content_hash)) {
        goto cleanup;
    }
    stage = "compare held and released resize bands";
    if (held_right_hash != released_right_hash ||
        held_bottom_hash != released_bottom_hash ||
        held_content_hash != released_content_hash) {
        goto cleanup;
    }

    stage = "restore modal resize baseline";
    if (!SetWindowPos(
            window,
            NULL,
            initial_outer.left,
            initial_outer.top,
            initial_outer.right - initial_outer.left,
            initial_outer.bottom - initial_outer.top,
            SWP_NOACTIVATE | SWP_NOZORDER) ||
        !wait_for_client_size(
            process,
            window,
            initial_client.right,
            initial_client.bottom,
            true) ||
        !query_grid_title(process, window, L"grid-24-80")) {
        goto cleanup;
    }
    passed = true;

cleanup:
    have_current_outer = GetWindowRect(window, &current_outer) != 0;
    have_current_surface =
        prepare_client_observation(window, &current_surface);
    current_button_down =
        ((unsigned short)GetAsyncKeyState(VK_LBUTTON) & 0x8000u) != 0;
    if (!GetCursorPos(&current_pointer))
        current_pointer.x = current_pointer.y = -1;
    (void)release_left_mouse_button();
    if (!passed) {
        fprintf(
            stderr,
            "Win32 modal resize presentation failed at %s "
            "(held-right=%llu released-right=%llu "
            "held-bottom=%llu released-bottom=%llu "
            "held-content=%llu released-content=%llu "
            "button-down=%d outer=%ld,%ld,%ld,%ld "
            "expected-outer=%ld,%ld,%ld,%ld "
            "surface=%ldx%ld expected-surface=%ldx%ld "
            "start=%ld,%ld target=%ld,%ld pointer=%ld,%ld "
            "desktop=%d,%d,%d,%d)\n",
            stage,
            (unsigned long long)held_right_hash,
            (unsigned long long)released_right_hash,
            (unsigned long long)held_bottom_hash,
            (unsigned long long)released_bottom_hash,
            (unsigned long long)held_content_hash,
            (unsigned long long)released_content_hash,
            current_button_down,
            have_current_outer ? current_outer.left : -1,
            have_current_outer ? current_outer.top : -1,
            have_current_outer ? current_outer.right : -1,
            have_current_outer ? current_outer.bottom : -1,
            target_outer.left,
            target_outer.top,
            target_outer.right,
            target_outer.bottom,
            have_current_surface ? current_surface.rectangle.right : -1,
            have_current_surface ? current_surface.rectangle.bottom : -1,
            target_surface.right,
            target_surface.bottom,
            sizing_point.x,
            sizing_point.y,
            (long)target_x,
            (long)target_y,
            current_pointer.x, current_pointer.y,
            virtual_left, virtual_top, virtual_width, virtual_height);
    }
    return passed;
}

static bool wait_for_successful_process(HANDLE process)
{
    DWORD exit_code = 1;

    return WaitForSingleObject(process, 10000) == WAIT_OBJECT_0 &&
           GetExitCodeProcess(process, &exit_code) && exit_code == 0;
}

static bool run_modal_resize_case(
    const wchar_t *sample_path,
    const wchar_t *helper_path,
    bool wine_runtime)
{
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    wchar_t *command_line = make_command_line(sample_path, helper_path);
    window_search search;
    const char *stage = "start modal resize sample";
    bool passed = false;

    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));
    if (command_line == NULL ||
        !CreateProcessW(
            NULL,
            command_line,
            NULL,
            NULL,
            FALSE,
            0,
            NULL,
            NULL,
            &startup,
            &process)) {
        free(command_line);
        goto cleanup;
    }
    free(command_line);
    CloseHandle(process.hThread);
    stage = "wait for modal resize sample";
    if (WaitForInputIdle(process.hProcess, 10000) != 0) {
        goto cleanup;
    }
    search.process_id = process.dwProcessId;
    search.window = NULL;
    (void)EnumWindows(find_process_window, (LPARAM)&search);
    stage = "find modal resize host";
    if (search.window == NULL ||
        terminal_input_window_for_host(search.window) == NULL ||
        scrollbar_width_for_host(search.window) <= 0) {
        goto cleanup;
    }
    stage = "wait for modal resize helper";
    if (!wait_for_window_title(
            process.hProcess,
            search.window,
            L"helper-ready")) {
        goto cleanup;
    }
    stage = "verify modal resize presentation";
    bool gpu = false;
    (void)rendering_window_for_host(search.window, &gpu);
    /* Wine 11's X11 GPU child capture loses the enlarged bands during the
     * modal loop. Native P2 verifies these frames, including video, for both
     * architectures; the native renderer still runs this check under Wine. */
    if (wine_runtime && gpu) {
        puts("SKIP Wine GPU modal resize capture: verified by native Win32 P2");
    } else if (!run_modal_resize_presentation(
            process.hProcess,
            search.window,
            wine_runtime)) {
        goto cleanup;
    }
    stage = "exit modal resize sample";
    if (!commit_character(search.window, L'q') ||
        !wait_for_successful_process(process.hProcess)) {
        goto cleanup;
    }
    passed = true;

cleanup:
    if (process.hProcess != NULL) {
        if (!passed &&
            WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process.hProcess, 1);
            (void)WaitForSingleObject(process.hProcess, INFINITE);
        }
        CloseHandle(process.hProcess);
    }
    if (!passed) {
        fprintf(stderr, "Win32 modal resize case failed at %s\n", stage);
    }
    return passed;
}

static bool format_ime_probe_object_name(
    wchar_t *name,
    size_t capacity,
    const wchar_t *kind,
    DWORD process_id)
{
    int length = swprintf(
        name,
        capacity,
        L"Local\\boxrig-ime-%ls-%lu",
        kind,
        process_id);

    return length > 0 && (size_t)length < capacity;
}

static HANDLE wait_for_ime_probe_semaphore(
    HANDLE process,
    DWORD process_id)
{
    wchar_t name[96];
    size_t attempt;

    if (!format_ime_probe_object_name(
            name,
            sizeof(name) / sizeof(name[0]),
            L"resynchronization",
            process_id)) {
        return NULL;
    }
    for (attempt = 0; attempt < 250; attempt++) {
        HANDLE semaphore = OpenSemaphoreW(SYNCHRONIZE, FALSE, name);

        if (semaphore != NULL) {
            return semaphore;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            return NULL;
        }
    }
    return NULL;
}

static bool drain_ime_probe_semaphore(HANDLE semaphore)
{
    DWORD wait_result;

    do {
        wait_result = WaitForSingleObject(semaphore, 0);
    } while (wait_result == WAIT_OBJECT_0);
    return wait_result == WAIT_TIMEOUT;
}

static bool expect_one_ime_resynchronization(HANDLE semaphore)
{
    return WaitForSingleObject(semaphore, 5000) == WAIT_OBJECT_0 &&
           WaitForSingleObject(semaphore, 100) == WAIT_TIMEOUT;
}

static bool wait_for_iconic_state(
    HANDLE process,
    HWND window,
    bool expected)
{
    size_t attempt;

    for (attempt = 0; attempt < 250; attempt++) {
        if ((IsIconic(window) != 0) == expected) {
            return true;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            return false;
        }
    }
    return false;
}

static bool run_ime_movement_case(
    const wchar_t *sample_path,
    const wchar_t *helper_path,
    const wchar_t *background_opacity,
    bool expect_placement_error)
{
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    wchar_t *command_line = make_command_line(sample_path, helper_path);
    wchar_t error_name[96];
    window_search search;
    HANDLE resynchronizations = NULL;
    HANDLE error_requests = NULL;
    RECT outer;
    DWORD exit_code = STILL_ACTIVE;
    DWORD_PTR message_result;
    const char *stage = "start IME movement probe";
    bool passed = false;

    ZeroMemory(&process, sizeof(process));
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    if (command_line == NULL ||
        !SetEnvironmentVariableW(
            L"BOXRIG_BACKGROUND_OPACITY",
            background_opacity) ||
        !CreateProcessW(
            NULL,
            command_line,
            NULL,
            NULL,
            FALSE,
            0,
            NULL,
            NULL,
            &startup,
            &process)) {
        free(command_line);
        (void)SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", NULL);
        goto cleanup;
    }
    free(command_line);
    command_line = NULL;
    (void)SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", NULL);
    CloseHandle(process.hThread);
    process.hThread = NULL;
    if (!format_ime_probe_object_name(
            error_name,
            sizeof(error_name) / sizeof(error_name[0]),
            L"error",
            process.dwProcessId)) {
        goto cleanup;
    }
    error_requests = CreateSemaphoreW(NULL, 0, 1, error_name);
    if (error_requests == NULL) {
        goto cleanup;
    }
    stage = "wait for IME movement probe";
    if (WaitForInputIdle(process.hProcess, 10000) != 0) {
        goto cleanup;
    }
    search.process_id = process.dwProcessId;
    search.window = NULL;
    (void)EnumWindows(find_process_window, (LPARAM)&search);
    if (search.window == NULL ||
        !wait_for_window_title(
            process.hProcess,
            search.window,
            L"helper-ready") ||
        !GetWindowRect(search.window, &outer)) {
        goto cleanup;
    }
    stage = "start composition before movement";
    if (!SendMessageTimeoutW(
            search.window,
            WM_IME_STARTCOMPOSITION,
            0,
            0,
            SMTO_ABORTIFHUNG,
            5000,
            &message_result)) {
        goto cleanup;
    }

    stage = "resynchronize after normal movement";
    if (!SetWindowPos(
            search.window,
            NULL,
            outer.left + 24,
            outer.top + 18,
            0,
            0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        goto cleanup;
    }
    resynchronizations = wait_for_ime_probe_semaphore(
        process.hProcess,
        process.dwProcessId);
    if (resynchronizations == NULL ||
        WaitForSingleObject(resynchronizations, 5000) != WAIT_OBJECT_0 ||
        !drain_ime_probe_semaphore(resynchronizations)) {
        goto cleanup;
    }

    stage = "resynchronize each modal movement notification";
    if (!send_window_message(search.window, WM_ENTERSIZEMOVE, 0, 0) ||
        WaitForSingleObject(resynchronizations, 100) != WAIT_TIMEOUT ||
        !send_window_message(
            search.window,
            WM_MOVE,
            0,
            MAKELPARAM(outer.left + 48, outer.top + 36)) ||
        !expect_one_ime_resynchronization(resynchronizations) ||
        !send_window_message(search.window, WM_EXITSIZEMOVE, 0, 0) ||
        !expect_one_ime_resynchronization(resynchronizations)) {
        goto cleanup;
    }

    stage = "suppress IME resynchronization while minimized";
    (void)ShowWindow(search.window, SW_MINIMIZE);
    if (!wait_for_iconic_state(process.hProcess, search.window, true) ||
        !drain_ime_probe_semaphore(resynchronizations) ||
        !send_window_message(
            search.window,
            WM_MOVE,
            0,
            MAKELPARAM(outer.left, outer.top)) ||
        WaitForSingleObject(resynchronizations, 200) != WAIT_TIMEOUT ||
        !send_window_message(search.window, WM_EXITSIZEMOVE, 0, 0) ||
        WaitForSingleObject(resynchronizations, 200) != WAIT_TIMEOUT) {
        goto cleanup;
    }
    (void)ShowWindow(search.window, SW_RESTORE);
    if (!wait_for_iconic_state(process.hProcess, search.window, false) ||
        !drain_ime_probe_semaphore(resynchronizations)) {
        goto cleanup;
    }

    stage = "continue composition and PTY after IMM rejection";
    if (!commit_character(search.window, L'g') ||
        !wait_for_window_title(
            process.hProcess,
            search.window,
            L"grid-24-80")) {
        goto cleanup;
    }
    if (expect_placement_error) {
        stage = "fail application on IME placement error";
        if (!drain_ime_probe_semaphore(resynchronizations) ||
            !ReleaseSemaphore(error_requests, 1, NULL) ||
            !send_window_message(
                search.window,
                WM_MOVE,
                0,
                MAKELPARAM(outer.left + 72, outer.top + 54)) ||
            WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0 ||
            !GetExitCodeProcess(process.hProcess, &exit_code) ||
            exit_code != 1) {
            goto cleanup;
        }
    } else {
        stage = "exit after nonfatal IME placement rejection";
        if (!commit_character(search.window, L'q') ||
            !wait_for_successful_process(process.hProcess)) {
            goto cleanup;
        }
    }
    passed = true;

cleanup:
    (void)SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", NULL);
    if (error_requests != NULL) {
        CloseHandle(error_requests);
    }
    if (resynchronizations != NULL) {
        CloseHandle(resynchronizations);
    }
    if (process.hProcess != NULL) {
        if (!passed &&
            WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process.hProcess, 1);
            (void)WaitForSingleObject(process.hProcess, INFINITE);
        }
        CloseHandle(process.hProcess);
    }
    free(command_line);
    if (!passed) {
        fprintf(stderr, "Win32 IME movement failed at %s\n", stage);
    }
    return passed;
}

static bool run_ime_movement_cases(
    const wchar_t *sample_path,
    const wchar_t *helper_path)
{
    return run_ime_movement_case(
               sample_path,
               helper_path,
               NULL,
               false) &&
           run_ime_movement_case(
               sample_path,
               helper_path,
               L"50",
               true);
}

static bool sample_has_windows_8_manifest(const wchar_t *sample_path)
{
    HMODULE module = LoadLibraryExW(
        sample_path,
        NULL,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    HRSRC manifest;

    if (module == NULL) {
        return false;
    }
    manifest = FindResourceW(module, MAKEINTRESOURCEW(1), RT_MANIFEST);
    FreeLibrary(module);
    return manifest != NULL;
}

static bool send_clipboard_shortcut(HWND input_window, UINT virtual_key,
    bool send_character);

static bool wait_for_host_opacity(HANDLE process, HWND window, BYTE expected)
{
    size_t attempt;
    for (attempt = 0; attempt < 500; attempt++) {
        COLORREF key = 0;
        BYTE alpha = 0;
        DWORD flags = 0;
        DWORD style = (DWORD)GetWindowLongPtrW(window, GWL_EXSTYLE);
        if ((style & WS_EX_NOREDIRECTIONBITMAP) == 0 &&
            ((expected == 255 && (style & WS_EX_LAYERED) == 0) ||
             (GetLayeredWindowAttributes(window, &key, &alpha, &flags) &&
              (flags & LWA_ALPHA) != 0 && alpha == expected))) return true;
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) return false;
    }
    return false;
}

static bool run_window_opacity_policy(const wchar_t *sample_path,
    const wchar_t *helper_path, bool failed_frame)
{
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    window_search search = {0};
    wchar_t old_mode[32] = {0};
    wchar_t *command_line = NULL;
    bool passed = false;
    size_t index;
    const char *stage = "launch";
    (void)GetEnvironmentVariableW(L"BOXRIG_TRANSPARENCY_MODE", old_mode, 32);
    if (!SetEnvironmentVariableW(L"BOXRIG_TRANSPARENCY_MODE", L"window") ||
        !SetEnvironmentVariableW(L"BOXRIG_WINDOW_OPACITY", L"50") ||
        !SetEnvironmentVariableW(L"BOXRIG_TEST_WINDOW_FRAME_FAILURE", failed_frame ? L"1" : NULL) ||
        !SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", L"0")) goto cleanup;
    command_line = make_command_line(sample_path, helper_path);
    startup.cb = sizeof(startup);
    if (command_line == NULL || !CreateProcessW(NULL, command_line, NULL, NULL,
            FALSE, 0, NULL, NULL, &startup, &process)) goto cleanup;
    CloseHandle(process.hThread);
    stage = "helper ready";
    if (WaitForInputIdle(process.hProcess, 10000) != 0) goto cleanup;
    search.process_id = process.dwProcessId;
    (void)EnumWindows(find_process_window, (LPARAM)&search);
    if (search.window == NULL || !wait_for_window_title(process.hProcess,
            search.window, L"helper-ready")) goto cleanup;
    stage = "independent window alpha";
    if (!wait_for_host_opacity(process.hProcess, search.window, 128)) goto cleanup;
    stage = "decrease";
    if (!send_clipboard_shortcut(search.window, VK_OEM_COMMA, false)) goto cleanup;
    if (failed_frame) {
        stage = "restore after failed renderer frame";
        for (index = 0; index < 500 &&
            (uintptr_t)GetPropW(search.window, L"boxrig-test-opacity-frame") != 2; index++)
            if (WaitForSingleObject(process.hProcess, 20) != WAIT_TIMEOUT) goto cleanup;
        if ((uintptr_t)GetPropW(search.window, L"boxrig-test-opacity-frame") != 2 ||
            !wait_for_host_opacity(process.hProcess, search.window, 128)) goto cleanup;
    } else if (!wait_for_host_opacity(process.hProcess, search.window, 102)) goto cleanup;
    stage = "increase";
    if (!send_clipboard_shortcut(search.window, VK_OEM_PERIOD, false) ||
        !wait_for_host_opacity(process.hProcess, search.window, 128)) goto cleanup;
    stage = "release whole-window effect";
    if (!send_clipboard_shortcut(search.window, 'O', false) ||
        !wait_for_host_opacity(process.hProcess, search.window, 255)) goto cleanup;
    stage = "restore whole-window effect";
    if (!send_clipboard_shortcut(search.window, 'O', false) ||
        !wait_for_host_opacity(process.hProcess, search.window, 128)) goto cleanup;
    stage = "rapid changes retain nonzero whole alpha";
    for (index = 0; index < 8; index++)
        if (!send_clipboard_shortcut(search.window, VK_OEM_COMMA, false)) goto cleanup;
    if (!wait_for_host_opacity(process.hProcess, search.window, 3)) goto cleanup;
    stage = "same child still accepts input";
    if (!commit_character(search.window, L'q') ||
        !wait_for_successful_process(process.hProcess)) goto cleanup;
    passed = true;
cleanup:
    (void)SetEnvironmentVariableW(L"BOXRIG_TRANSPARENCY_MODE",
        old_mode[0] == L'\0' ? NULL : old_mode);
    (void)SetEnvironmentVariableW(L"BOXRIG_WINDOW_OPACITY", NULL);
    (void)SetEnvironmentVariableW(L"BOXRIG_TEST_WINDOW_FRAME_FAILURE", NULL);
    (void)SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", NULL);
    free(command_line);
    if (!passed) fprintf(stderr, "Win32 window opacity failed at %s (error=%lu)\n",
        stage, (unsigned long)GetLastError());
    if (process.hProcess != NULL) {
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process.hProcess, 1);
            (void)WaitForSingleObject(process.hProcess, INFINITE);
        }
        CloseHandle(process.hProcess);
    }
    return passed;
}

static bool run_fallback_opacity_policy(const wchar_t *sample_path,
    const wchar_t *helper_path)
{
    const struct { const wchar_t *mode, *fallback, *background; BYTE expected; } cases[] = {
        {NULL, NULL, L"50", 128},
        {L"background", NULL, L"50", 128},
        {L"background", L"opaque", L"50", 255},
        {L"background", L"window", L"50", 128},
        {L"background", L"window", L"0", 255},
        {L"opaque", L"window", L"50", 255}};
    size_t index;
    wchar_t old_mode[32] = {0};
    bool passed = true;
    if (!run_window_opacity_policy(sample_path, helper_path, true)) return false;
    (void)GetEnvironmentVariableW(L"BOXRIG_TRANSPARENCY_MODE", old_mode, 32);
    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        STARTUPINFOW startup = {0};
        PROCESS_INFORMATION process = {0};
        window_search search = {0};
        wchar_t *command_line = make_command_line(sample_path, helper_path);
        startup.cb = sizeof(startup);
        passed = command_line != NULL &&
            SetEnvironmentVariableW(L"BOXRIG_TRANSPARENCY_MODE", cases[index].mode) &&
            SetEnvironmentVariableW(L"BOXRIG_TRANSPARENCY_FALLBACK", cases[index].fallback) &&
            SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", cases[index].background) &&
            CreateProcessW(NULL, command_line, NULL, NULL, FALSE, 0,
                NULL, NULL, &startup, &process);
        free(command_line);
        if (passed) {
            CloseHandle(process.hThread);
            search.process_id = process.dwProcessId;
            passed = WaitForInputIdle(process.hProcess, 10000) == 0;
            (void)EnumWindows(find_process_window, (LPARAM)&search);
            passed = passed && search.window != NULL &&
                wait_for_window_title(process.hProcess, search.window, L"helper-ready") &&
                wait_for_host_opacity(process.hProcess, search.window, cases[index].expected) &&
                commit_character(search.window, L'q') && wait_for_successful_process(process.hProcess);
        }
        if (process.hProcess != NULL) {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
                (void)TerminateProcess(process.hProcess, 1);
                (void)WaitForSingleObject(process.hProcess, INFINITE);
            }
            CloseHandle(process.hProcess);
        }
        if (!passed) {
            fprintf(stderr, "Win32 unavailable-alpha policy case %lu failed (error=%lu)\n",
                (unsigned long)index, (unsigned long)GetLastError());
            break;
        }
    }
    (void)SetEnvironmentVariableW(L"BOXRIG_TRANSPARENCY_MODE", old_mode[0] == L'\0' ? NULL : old_mode);
    (void)SetEnvironmentVariableW(L"BOXRIG_TRANSPARENCY_FALLBACK", NULL);
    (void)SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", NULL);
    return passed;
}

static bool run_background_opacity_policy(
    const wchar_t *sample_path,
    const wchar_t *helper_path)
{
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    window_search search;
    wchar_t *command_line = NULL;
    DWORD exit_code = STILL_ACTIVE;
    const char *stage = "inspect manifest";
    bool passed = false;

    ZeroMemory(&search, sizeof(search));
    if (!sample_has_windows_8_manifest(sample_path)) {
        goto cleanup_without_process;
    }
    if (!run_window_opacity_policy(sample_path, helper_path, false)) goto cleanup_without_process;
    stage = "set invalid opacity";
    if (!SetEnvironmentVariableW(
            L"BOXRIG_BACKGROUND_OPACITY",
            L"50x")) {
        goto cleanup_without_process;
    }
    command_line = make_default_command_line(sample_path);
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));
    if (command_line == NULL ||
        !CreateProcessW(
            NULL,
            command_line,
            NULL,
            NULL,
            FALSE,
            0,
            NULL,
            NULL,
            &startup,
            &process)) {
        (void)SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", NULL);
        free(command_line);
        goto cleanup_without_process;
    }
    free(command_line);
    command_line = NULL;
    CloseHandle(process.hThread);
    (void)SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", NULL);
    if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &exit_code) || exit_code != 2) {
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process.hProcess, 1);
            (void)WaitForSingleObject(process.hProcess, INFINITE);
        }
        CloseHandle(process.hProcess);
        stage = "reject invalid opacity";
        goto cleanup_without_process;
    }
    CloseHandle(process.hProcess);

    stage = "set preferred opacity";
    if (!SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", L"50")) {
        goto cleanup_without_process;
    }
    command_line = make_command_line(sample_path, helper_path);
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));
    if (command_line == NULL ||
        !CreateProcessW(
            NULL,
            command_line,
            NULL,
            NULL,
            FALSE,
            0,
            NULL,
            NULL,
            &startup,
            &process)) {
        (void)SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", NULL);
        free(command_line);
        goto cleanup_without_process;
    }
    free(command_line);
    command_line = NULL;
    CloseHandle(process.hThread);
    (void)SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", NULL);
    stage = "wait for preferred input idle";
    if (WaitForInputIdle(process.hProcess, 10000) != 0) {
        goto cleanup;
    }
    search.process_id = process.dwProcessId;
    search.window = NULL;
    (void)EnumWindows(find_process_window, (LPARAM)&search);
    stage = "find preferred host";
    if (search.window == NULL) {
        goto cleanup;
    }
    stage = "wait for preferred helper";
    if (!wait_for_window_title(
            process.hProcess,
            search.window,
            L"helper-ready")) {
        goto cleanup;
    }
    stage = "inspect preferred host";
    {
        wchar_t mode[32];
        DWORD length = GetEnvironmentVariableW(L"BOXRIG_TRANSPARENCY_MODE", mode, 32);
        if (length == 6 && wcscmp(mode, L"opaque") == 0 &&
            ((DWORD)GetWindowLongPtrW(search.window, GWL_EXSTYLE) &
             (WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED)) != 0) {
            stage = "opaque mode must not prepare an alpha host";
            goto cleanup;
        }
    }
    /* Both owned-child and borrowed-root presentation are valid. The ready
     * marker and input/exit below exercise the actual terminal behavior. */
    stage = "exit preferred helper";
    if (!commit_character(search.window, L'q') ||
        !wait_for_successful_process(process.hProcess)) {
        goto cleanup;
    }
    passed = true;

cleanup:
    (void)SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", NULL);
    if (!passed) {
        (void)GetExitCodeProcess(process.hProcess, &exit_code);
        fprintf(
            stderr,
            "Win32 background opacity failed at %s "
            "(exit=%lu error=%lu style=%lx exstyle=%lx)\n",
            stage,
            (unsigned long)exit_code,
            (unsigned long)GetLastError(),
            search.window == NULL
                ? 0UL
                : (unsigned long)GetWindowLongPtrW(
                      search.window,
                      GWL_STYLE),
            search.window == NULL
                ? 0UL
                : (unsigned long)GetWindowLongPtrW(
                      search.window,
                      GWL_EXSTYLE));
    }
    if (!passed &&
        WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
        (void)TerminateProcess(process.hProcess, 1);
        (void)WaitForSingleObject(process.hProcess, INFINITE);
    }
    CloseHandle(process.hProcess);
    return passed;

cleanup_without_process:
    (void)SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", NULL);
    if (!passed) {
        fprintf(
            stderr,
            "Win32 background opacity failed at %s "
            "(exit=%lu error=%lu)\n",
            stage,
            (unsigned long)exit_code,
            (unsigned long)GetLastError());
    }
    return passed;
}

static bool post_key_press(HWND window, UINT virtual_key)
{
    return post_key(window, virtual_key, false) &&
           post_key(window, virtual_key, true);
}

static bool submit_text(
    HANDLE process,
    HWND window,
    const wchar_t *text)
{
    size_t text_length = wcslen(text);
    SIZE_T allocation_size;
    HGLOBAL allocation;
    wchar_t *clipboard_text;
    DWORD_PTR result;
    uint64_t baseline;
    bool clipboard_open = false;
    bool succeeded = false;

    if (!wait_for_stable_client_hash(
            process,
            window,
            &baseline,
            true) ||
        text_length > SIZE_MAX / sizeof(*clipboard_text) - 1) {
        return false;
    }
    allocation_size = (text_length + 1) * sizeof(*clipboard_text);
    allocation = GlobalAlloc(GMEM_MOVEABLE, allocation_size);
    if (allocation == NULL) {
        return false;
    }
    clipboard_text = GlobalLock(allocation);
    if (clipboard_text == NULL) {
        goto cleanup;
    }
    memcpy(clipboard_text, text, text_length * sizeof(*clipboard_text));
    clipboard_text[text_length] = L'\0';
    (void)GlobalUnlock(allocation);

    if (!OpenClipboard(window)) {
        goto cleanup;
    }
    clipboard_open = true;
    if (!EmptyClipboard() ||
        SetClipboardData(CF_UNICODETEXT, allocation) == NULL) {
        goto cleanup;
    }
    allocation = NULL;
    if (!CloseClipboard()) {
        goto cleanup;
    }
    clipboard_open = false;
    succeeded =
        SendMessageTimeoutW(
            window,
            WM_PASTE,
            0,
            0,
            SMTO_ABORTIFHUNG,
            5000,
            &result) != 0 &&
        wait_for_changed_stable_client_hash(
            process,
            window,
            baseline) &&
        post_key_press(window, VK_RETURN);

cleanup:
    if (clipboard_open) {
        (void)CloseClipboard();
    }
    if (allocation != NULL) {
        (void)GlobalFree(allocation);
    }
    return succeeded;
}

static bool send_ctrl_c(HANDLE process, HWND window)
{
    INPUT input[4];
    size_t attempt;

    if (GetForegroundWindow() != window && !SetForegroundWindow(window)) {
        RECT outer;
        POINT client = {0, 0}, point;
        INPUT mouse[3];
        DWORD_PTR hit;
        const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        bool caption = false;
        /* If Windows denies programmatic foreground activation, click the
         * actual caption, leaving terminal selection and MORE input intact. */
        if (width < 2 || height < 2 || !GetWindowRect(window, &outer) ||
            !ClientToScreen(window, &client) ||
            !SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) return false;
        point.x = outer.left + (outer.right - outer.left) / 2;
        for (point.y = outer.top + (client.y - outer.top) / 2;
             point.y < client.y; point.y++) {
            if (!SendMessageTimeoutW(window, WM_NCHITTEST, 0,
                    MAKELPARAM((WORD)point.x, (WORD)point.y),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &hit)) return false;
            if (hit == HTCAPTION && WindowFromPoint(point) == window) {
                caption = true;
                break;
            }
        }
        if (!caption) return false;
        initialize_absolute_mouse_input(&mouse[0], point.x, point.y,
            MOUSEEVENTF_MOVE, left, top, width, height);
        initialize_absolute_mouse_input(&mouse[1], point.x, point.y,
            MOUSEEVENTF_LEFTDOWN, left, top, width, height);
        initialize_absolute_mouse_input(&mouse[2], point.x, point.y,
            MOUSEEVENTF_LEFTUP, left, top, width, height);
        if (SendInput(3, mouse, sizeof(mouse[0])) != 3) {
            (void)release_left_mouse_button();
            return false;
        }
    }
    for (attempt = 0; attempt < 250; attempt++) {
        if (GetForegroundWindow() == window) {
            break;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            return false;
        }
    }
    if (GetForegroundWindow() != window) {
        return false;
    }

    /* SendInput updates the system keyboard state and exercises the same
     * foreground input path as a user pressing the chord. Posting messages
     * directly does not provide that guarantee for modifier keys. */
    ZeroMemory(input, sizeof(input));
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_CONTROL;
    input[1].type = INPUT_KEYBOARD;
    input[1].ki.wVk = L'C';
    input[2].type = INPUT_KEYBOARD;
    input[2].ki.wVk = L'C';
    input[2].ki.dwFlags = KEYEVENTF_KEYUP;
    input[3].type = INPUT_KEYBOARD;
    input[3].ki.wVk = VK_CONTROL;
    input[3].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(
               (UINT)(sizeof(input) / sizeof(input[0])),
               input,
               sizeof(input[0])) ==
           sizeof(input) / sizeof(input[0]);
}

static bool write_more_fixture(const wchar_t *path)
{
    HANDLE file = CreateFileW(
        path,
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    size_t line_number;
    bool passed = false;

    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    for (line_number = 1; line_number <= 80; line_number++) {
        char line[64];
        int length = snprintf(
            line,
            sizeof(line),
            "boxrig more Ctrl-C fixture line %02lu\r\n",
            (unsigned long)line_number);
        DWORD written;

        if (length < 0 || (size_t)length >= sizeof(line) ||
            !WriteFile(
                file,
                line,
                (DWORD)length,
                &written,
                NULL) ||
            written != (DWORD)length) {
            goto cleanup;
        }
    }
    passed = true;

cleanup:
    CloseHandle(file);
    return passed;
}

static bool run_more_ctrl_c_case(
    const wchar_t *sample_path,
    bool allow_wine_session_exit)
{
    wchar_t temporary_directory[MAX_PATH];
    wchar_t fixture_path[MAX_PATH];
    wchar_t more_command[MAX_PATH + 16];
    HANDLE process = NULL;
    HWND window = NULL;
    uint64_t initial_hash = 0;
    uint64_t prompt_hash = 0;
    uint64_t more_hash = 0;
    DWORD temporary_directory_length;
    char observed_title[64] = "";
    const char *stage = "start sample";
    int length;
    bool passed = false;

    temporary_directory_length = GetTempPathW(
        sizeof(temporary_directory) / sizeof(temporary_directory[0]),
        temporary_directory);
    if (temporary_directory_length == 0 ||
        temporary_directory_length >=
            sizeof(temporary_directory) / sizeof(temporary_directory[0]) ||
        GetTempFileNameW(
            temporary_directory,
            L"brg",
            0,
            fixture_path) == 0) {
        return false;
    }
    length = swprintf(
        more_command,
        sizeof(more_command) / sizeof(more_command[0]),
        L"more \"%ls\"",
        fixture_path);
    if (length < 0 ||
        (size_t)length >= sizeof(more_command) / sizeof(more_command[0]) ||
        !write_more_fixture(fixture_path)) {
        (void)DeleteFileW(fixture_path);
        return false;
    }

    /* This follows the interactive path used by a person: type into the
     * frontend, wait until MORE owns the console, then press physical Ctrl+C.
     * Give the first prompt an observable title because an unpainted alpha
     * surface can expose stable desktop pixels before cmd.exe accepts input.
     * Wine uses the opaque fallback and normalizes this prompt title itself.
     * Preserve shell-local state so that a replacement cmd.exe cannot satisfy
     * the continuation check merely by presenting another prompt. */
    if (!allow_wine_session_exit) {
        stage = "configure initial shell prompt";
        if (!SetEnvironmentVariableW(
                L"PROMPT",
                L"$E]0;boxrig-more-initial$E\\@$P$G")) {
            goto cleanup;
        }
    }
    if (!start_default_sample(sample_path, &process, &window)) {
        goto cleanup;
    }
    (void)SetEnvironmentVariableW(L"PROMPT", NULL);
    stage = "wait for initial shell";
    if ((!allow_wine_session_exit &&
         !wait_for_window_title(
             process,
             window,
             L"boxrig-more-initial")) ||
        !wait_for_stable_client_hash(
            process,
            window,
            &initial_hash,
            true)) {
        goto cleanup;
    }
    stage = "set shell prompt and state";
    if (!submit_text(
            process,
            window,
            L"prompt $E]0;boxrig-more-ready$E\\@ & "
            L"set BOXRIG_MORE_CTRL_C_STATE=preserved")) {
        goto cleanup;
    }
    stage = "wait for shell prompt";
    if (!wait_for_window_title(
            process,
            window,
            L"boxrig-more-ready") ||
        !wait_for_stable_client_hash(
            process,
            window,
            &prompt_hash,
            true)) {
        goto cleanup;
    }
    stage = "start MORE";
    if (!SetWindowTextW(window, L"boxrig-more-running") ||
        !submit_text(process, window, more_command)) {
        goto cleanup;
    }
    stage = "wait for MORE page";
    if (!wait_for_stable_client_hash(
            process,
            window,
            &more_hash,
            true) ||
        more_hash == prompt_hash) {
        goto cleanup;
    }
    stage = "post Ctrl-C";
    if (!send_ctrl_c(process, window)) {
        goto cleanup;
    }
    stage = "wait for shell after Ctrl-C";
    if (!wait_for_window_title(
            process,
            window,
            L"boxrig-more-ready")) {
        /* Wine's headless ConPTY currently treats ETX as input termination
         * before cmd.exe receives CTRL_C_EVENT. That external limitation may
         * end the session, but the frontend must never hide it by replacing
         * the shell and discarding shell-local state. */
        if (allow_wine_session_exit &&
            wait_for_successful_process(process)) {
            printf(
                "SKIP Win32 sample MORE Ctrl-C continuation: "
                "Wine ConPTY ended the shell session\n");
            passed = true;
        }
        goto cleanup;
    }
    stage = "verify shell state after Ctrl-C";
    /* Make the next prompt publish the branch result. Observing the title then
     * proves both that the shell-local state survived and that cmd.exe is
     * ready for the final input. */
    if (!submit_text(
            process,
            window,
            L"if \"%BOXRIG_MORE_CTRL_C_STATE%\"==\"preserved\" "
            L"(prompt $E]0;boxrig-more-ctrl-c$E\\@) else "
            L"(prompt $E]0;boxrig-more-state-lost$E\\@)")) {
        if (allow_wine_session_exit) {
            (void)TerminateProcess(process, 0);
            (void)WaitForSingleObject(process, INFINITE);
            printf(
                "SKIP Win32 sample MORE Ctrl-C continuation: "
                "Wine ConPTY did not return control to cmd.exe\n");
            passed = true;
        }
        goto cleanup;
    }
    stage = "verify shell after Ctrl-C";
    if (!wait_for_window_title(
            process,
            window,
            L"boxrig-more-ctrl-c")) {
        if (allow_wine_session_exit) {
            (void)TerminateProcess(process, 0);
            (void)WaitForSingleObject(process, INFINITE);
            printf(
                "SKIP Win32 sample MORE Ctrl-C continuation: "
                "Wine ConPTY did not return control to cmd.exe\n");
            passed = true;
        }
        goto cleanup;
    }
    stage = "submit exit after Ctrl-C";
    if (!submit_text(process, window, L"exit 0")) {
        goto cleanup;
    }
    stage = "wait for successful exit";
    if (!wait_for_successful_process(process)) {
        goto cleanup;
    }
    passed = true;

cleanup:
    (void)SetEnvironmentVariableW(L"PROMPT", NULL);
    if (!passed) {
        DWORD exit_code = STILL_ACTIVE;

        if (process != NULL) {
            (void)GetExitCodeProcess(process, &exit_code);
        }
        if (window != NULL) {
            (void)GetWindowTextA(
                window,
                observed_title,
                (int)(sizeof(observed_title) / sizeof(observed_title[0])));
        }
        fprintf(
            stderr,
            "MORE Ctrl-C sample case failed at %s "
            "(sample_exit=%lu, title=%s, hashes=%llu/%llu/%llu)\n",
            stage,
            (unsigned long)exit_code,
            observed_title,
            (unsigned long long)initial_hash,
            (unsigned long long)prompt_hash,
            (unsigned long long)more_hash);
    }
    if (process != NULL) {
        if (!passed && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process, 1);
            (void)WaitForSingleObject(process, INFINITE);
        }
        CloseHandle(process);
    }
    (void)DeleteFileW(fixture_path);
    return passed;
}

static bool run_emitter_key_case(
    const wchar_t *sample_path,
    const wchar_t *emitter_path,
    const wchar_t *case_name,
    bool hold,
    UINT virtual_key)
{
    HANDLE process = NULL;
    HWND window = NULL;
    uint64_t rendered_hash;
    bool passed = false;

    if (!start_emitter_sample(
            sample_path,
            emitter_path,
            case_name,
            hold,
            false,
            NULL,
            &process,
            &window) ||
        !wait_for_stable_client_hash(
            process,
            window,
            &rendered_hash,
            true) ||
        !post_key_press(window, virtual_key) ||
        !wait_for_successful_process(process)) {
        goto cleanup;
    }
    passed = true;

cleanup:
    if (process != NULL) {
        if (!passed && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process, 1);
            (void)WaitForSingleObject(process, INFINITE);
        }
        CloseHandle(process);
    }
    if (!passed) {
        fwprintf(stderr, L"emitter key case failed: %ls\n", case_name);
    }
    return passed;
}

static bool run_emitter_ime_case(
    const wchar_t *sample_path,
    const wchar_t *emitter_path)
{
    HANDLE process = NULL;
    HWND window = NULL;
    uint64_t rendered_hash;
    bool passed = false;

    if (!start_emitter_sample(
            sample_path,
            emitter_path,
            L"input-ime-commit",
            false,
            false,
            NULL,
            &process,
            &window) ||
        !wait_for_stable_client_hash(
            process,
            window,
            &rendered_hash,
            true) ||
        !commit_character(window, L'\x65e5') ||
        !commit_character(window, L'\x672c') ||
        !commit_character(window, L'\r') ||
        !wait_for_successful_process(process)) {
        goto cleanup;
    }
    passed = true;

cleanup:
    if (process != NULL) {
        if (!passed && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process, 1);
            (void)WaitForSingleObject(process, INFINITE);
        }
        CloseHandle(process);
    }
    if (!passed) {
        fprintf(stderr, "emitter IME input case failed\n");
    }
    return passed;
}

static bool run_emitter_query_case(
    const wchar_t *sample_path,
    const wchar_t *emitter_path,
    const wchar_t *case_name)
{
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    wchar_t *command_line = make_emitter_command_line(
        sample_path,
        emitter_path,
        case_name,
        false);
    DWORD wait_result;
    DWORD exit_code = STILL_ACTIVE;
    bool passed;

    if (command_line == NULL) {
        return false;
    }
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));
    if (!CreateProcessW(
            NULL,
            command_line,
            NULL,
            NULL,
            FALSE,
            0,
            NULL,
            NULL,
            &startup,
            &process)) {
        free(command_line);
        return false;
    }
    free(command_line);
    CloseHandle(process.hThread);
    wait_result = WaitForSingleObject(process.hProcess, 10000);
    if (wait_result == WAIT_OBJECT_0) {
        (void)GetExitCodeProcess(process.hProcess, &exit_code);
    }
    passed = wait_result == WAIT_OBJECT_0 && exit_code == 0;
    if (!passed &&
        WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
        (void)TerminateProcess(process.hProcess, 1);
        (void)WaitForSingleObject(process.hProcess, INFINITE);
    }
    CloseHandle(process.hProcess);
    if (!passed) {
        fprintf(
            stderr,
            "emitter query case failed (wait=%lu, exit=%lu, error=%lu): %ls\n",
            wait_result,
            exit_code,
            GetLastError(),
            case_name);
    }
    return passed;
}

static bool run_emitter_graphics_case(
    const wchar_t *sample_path,
    const wchar_t *emitter_path)
{
    HANDLE process = NULL;
    HWND window = NULL;
    bool passed = false;

    if (!start_emitter_sample(
            sample_path,
            emitter_path,
            L"kitty-image-static",
            true,
            false,
            NULL,
            &process,
            &window) ||
        !wait_for_graphics_fixture(process, window) ||
        !post_key_press(window, VK_RETURN) ||
        !wait_for_successful_process(process)) {
        goto cleanup;
    }
    passed = true;

cleanup:
    if (process != NULL) {
        if (!passed && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process, 1);
            (void)WaitForSingleObject(process, INFINITE);
        }
        CloseHandle(process);
    }
    if (!passed) {
        fprintf(stderr, "emitter graphics case failed\n");
    }
    return passed;
}

static bool read_emitter_image_pixel(
    HWND window,
    size_t column,
    size_t row,
    size_t columns,
    size_t rows,
    size_t source_x,
    size_t source_y,
    size_t source_width,
    size_t source_height,
    COLORREF *pixel_out)
{
    client_observation observation;
    HDC device_context;
    size_t cell_width;
    size_t cell_height;
    int x;
    int y;
    COLORREF pixel;

    if (!prepare_client_observation(window, &observation) ||
        observation.rectangle.right % 80 != 0 ||
        observation.rectangle.bottom % 24 != 0 || column >= 80 || row >= 24 ||
        columns == 0 || rows == 0 || source_width == 0 ||
        source_height == 0 || source_x >= source_width ||
        source_y >= source_height || columns > 80 - column ||
        rows > 24 - row) {
        return false;
    }
    cell_width = (size_t)observation.rectangle.right / 80;
    cell_height = (size_t)observation.rectangle.bottom / 24;
    if (cell_width < 4 || cell_height < 2) {
        return false;
    }
    x = (int)(column * cell_width +
              (source_x * 2 + 1) * columns * cell_width /
                  (source_width * 2));
    y = (int)(row * cell_height +
              (source_y * 2 + 1) * rows * cell_height /
                  (source_height * 2));
    device_context = GetDC(observation.context_window);
    if (device_context == NULL) {
        return false;
    }
    pixel = GetPixel(
        device_context,
        observation.source.x + x,
        observation.source.y + y);
    (void)ReleaseDC(observation.context_window, device_context);
    if (pixel == CLR_INVALID) {
        return false;
    }
    *pixel_out = pixel;
    return true;
}

static bool emitter_image_pixel_matches(
    HWND window,
    size_t column,
    size_t row,
    size_t columns,
    size_t rows,
    size_t source_x,
    size_t source_y,
    size_t source_width,
    size_t source_height,
    uint8_t red,
    uint8_t green,
    uint8_t blue)
{
    COLORREF pixel;

    return read_emitter_image_pixel(
               window,
               column,
               row,
               columns,
               rows,
               source_x,
               source_y,
               source_width,
               source_height,
               &pixel) &&
           color_channel_near(GetRValue(pixel), red) &&
           color_channel_near(GetGValue(pixel), green) &&
           color_channel_near(GetBValue(pixel), blue);
}

typedef struct fixed_animation_pixel {
    size_t source_x;
    size_t source_y;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    /* The direct frame split is filtered when the 16-pixel image is scaled. */
    bool red_dominant;
} fixed_animation_pixel;

typedef struct fixed_animation_fixture {
    const wchar_t *case_name;
    fixed_animation_pixel pixels[2];
} fixed_animation_fixture;

static bool run_fixed_emitter_animation_case(
    const wchar_t *sample_path,
    const wchar_t *emitter_path,
    const fixed_animation_fixture *fixture)
{
    HANDLE process = NULL;
    HWND window = NULL;
    bool pixels_match = false;
    bool passed = false;
    size_t attempt;

    if (!start_emitter_sample(
            sample_path,
            emitter_path,
            fixture->case_name,
            true,
            false,
            NULL,
            &process,
            &window)) {
        goto cleanup;
    }
    for (attempt = 0; attempt < 250 && !pixels_match; attempt++) {
        size_t index;

        if (!RedrawWindow(
                window,
                NULL,
                NULL,
                RDW_INVALIDATE | RDW_UPDATENOW)) {
            goto cleanup;
        }
        pixels_match = true;
        for (index = 0;
             index < sizeof(fixture->pixels) / sizeof(fixture->pixels[0]);
            index++) {
            if (fixture->pixels[index].red_dominant) {
                COLORREF pixel;

                if (!read_emitter_image_pixel(
                        window,
                        0,
                        2,
                        4,
                        2,
                        fixture->pixels[index].source_x,
                        fixture->pixels[index].source_y,
                        16,
                        16,
                        &pixel) ||
                    (int)GetRValue(pixel) - (int)GetGValue(pixel) < 32 ||
                    (int)GetRValue(pixel) - (int)GetBValue(pixel) < 32) {
                    pixels_match = false;
                    break;
                }
            } else if (!emitter_image_pixel_matches(
                           window,
                           0,
                           2,
                           4,
                           2,
                           fixture->pixels[index].source_x,
                           fixture->pixels[index].source_y,
                           16,
                           16,
                           fixture->pixels[index].red,
                           fixture->pixels[index].green,
                           fixture->pixels[index].blue)) {
                pixels_match = false;
                break;
            }
        }
        if (!pixels_match &&
            WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            goto cleanup;
        }
    }
    if (!pixels_match || !post_key_press(window, VK_RETURN) ||
        !wait_for_successful_process(process)) {
        goto cleanup;
    }
    passed = true;

cleanup:
    if (process != NULL) {
        if (!passed && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process, 1);
            (void)WaitForSingleObject(process, INFINITE);
        }
        CloseHandle(process);
    }
    if (!passed) {
        fprintf(
            stderr,
            "fixed emitter animation failed (%ls pixels=%d)\n",
            fixture->case_name,
            pixels_match);
    }
    return passed;
}

static bool run_emitter_animation_case(
    const wchar_t *sample_path,
    const wchar_t *emitter_path)
{
    static const fixed_animation_fixture fixed[] = {
        {
            L"kitty-animation-direct",
            {
                {7, 12, 224, 48, 48, true},
                {10, 10, 48, 200, 88, false},
            },
        },
        {
            L"kitty-animation-client",
            {
                {4, 1, 255, 220, 64, false},
                {10, 10, 48, 96, 224, false},
            },
        },
        {
            L"kitty-animation-compose",
            {
                {0, 0, 0, 0, 0, false},
                {10, 10, 48, 96, 224, false},
            },
        },
    };
    HANDLE process = NULL;
    HWND window = NULL;
    bool found_red = false;
    bool found_green = false;
    bool passed = false;
    size_t attempt;
    size_t fixed_index;

    for (fixed_index = 0;
         fixed_index < sizeof(fixed) / sizeof(fixed[0]);
         fixed_index++) {
        if (!run_fixed_emitter_animation_case(
                sample_path,
                emitter_path,
                &fixed[fixed_index])) {
            return false;
        }
    }

    if (!start_emitter_sample(
            sample_path,
            emitter_path,
            L"kitty-animation-loop",
            true,
            false,
            NULL,
            &process,
            &window)) {
        goto cleanup;
    }
    for (attempt = 0; attempt < 250 && !(found_red && found_green); attempt++) {
        if (!RedrawWindow(
                window,
                NULL,
                NULL,
                RDW_INVALIDATE | RDW_UPDATENOW)) {
            goto cleanup;
        }
        found_red = found_red || emitter_image_pixel_matches(
            window, 0, 2, 4, 2, 10, 10, 16, 16, 224, 48, 48);
        found_green = found_green || emitter_image_pixel_matches(
            window, 0, 2, 4, 2, 10, 10, 16, 16, 48, 200, 88);
        if (!(found_red && found_green) &&
            WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            goto cleanup;
        }
    }
    if (!found_red || !found_green ||
        !post_key_press(window, VK_RETURN) ||
        !wait_for_successful_process(process)) {
        goto cleanup;
    }
    passed = true;

cleanup:
    if (process != NULL) {
        if (!passed && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process, 1);
            (void)WaitForSingleObject(process, INFINITE);
        }
        CloseHandle(process);
    }
    if (!passed) {
        fprintf(
            stderr,
            "emitter animation case failed (red=%d green=%d)\n",
            found_red,
            found_green);
    }
    return passed;
}

static bool start_external_emitter_sample(
    const wchar_t *sample_path,
    const wchar_t *emitter_path,
    bool allow_external,
    HANDLE *process_out,
    HWND *window_out)
{
    bool environment_set = SetEnvironmentVariableW(
                               L"BOXRIG_ALLOW_GRAPHICS_EXTERNAL",
                               allow_external ? L"1" : NULL) != FALSE;
    bool started;

    if (!environment_set) {
        return false;
    }
    started = start_emitter_sample(
        sample_path,
        emitter_path,
        L"kitty-image-external",
        true,
        false,
        NULL,
        process_out,
        window_out);
    if (!SetEnvironmentVariableW(
            L"BOXRIG_ALLOW_GRAPHICS_EXTERNAL",
            NULL)) {
        if (started) {
            (void)TerminateProcess(*process_out, 1);
            (void)WaitForSingleObject(*process_out, INFINITE);
            CloseHandle(*process_out);
            *process_out = NULL;
            *window_out = NULL;
        }
        return false;
    }
    return started;
}

static bool external_emitter_pixels_match(HWND window)
{
    return emitter_image_pixel_matches(
               window, 0, 3, 4, 2, 10, 10, 16, 16, 224, 48, 48) &&
           emitter_image_pixel_matches(
               window, 6, 3, 4, 2, 10, 10, 16, 16, 48, 200, 88) &&
           emitter_image_pixel_matches(
               window, 12, 3, 4, 2, 10, 10, 16, 16, 48, 96, 224);
}

static bool external_emitter_any_pixel_matches(HWND window)
{
    return emitter_image_pixel_matches(
               window, 0, 3, 4, 2, 10, 10, 16, 16, 224, 48, 48) ||
           emitter_image_pixel_matches(
               window, 6, 3, 4, 2, 10, 10, 16, 16, 48, 200, 88) ||
           emitter_image_pixel_matches(
               window, 12, 3, 4, 2, 10, 10, 16, 16, 48, 96, 224);
}

static bool external_emitter_hold_visible(HWND window)
{
    client_observation observation;
    BITMAPINFO information;
    HDC device_context = NULL;
    HDC memory_context = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ previous = NULL;
    uint8_t *pixels = NULL;
    size_t cell_width;
    size_t cell_height;
    size_t width;
    size_t height;
    size_t bright_pixels = 0;
    size_t index;
    bool result = false;

    if (!prepare_client_observation(window, &observation) ||
        observation.rectangle.right % 80 != 0 ||
        observation.rectangle.bottom % 24 != 0) {
        return false;
    }
    cell_width = (size_t)observation.rectangle.right / 80;
    cell_height = (size_t)observation.rectangle.bottom / 24;
    if (cell_width == 0 || cell_height == 0) {
        return false;
    }
    width = 32 * cell_width;
    height = cell_height;
    ZeroMemory(&information, sizeof(information));
    information.bmiHeader.biSize = sizeof(information.bmiHeader);
    information.bmiHeader.biWidth = (LONG)width;
    information.bmiHeader.biHeight = -(LONG)height;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    device_context = GetDC(observation.context_window);
    if (device_context == NULL) {
        return false;
    }
    memory_context = CreateCompatibleDC(device_context);
    if (memory_context == NULL) {
        goto cleanup;
    }
    bitmap = CreateDIBSection(
        device_context,
        &information,
        DIB_RGB_COLORS,
        (void **)&pixels,
        NULL,
        0);
    if (bitmap == NULL || pixels == NULL) {
        goto cleanup;
    }
    previous = SelectObject(memory_context, bitmap);
    if (previous == NULL || previous == HGDI_ERROR ||
        !BitBlt(
            memory_context,
            0,
            0,
            (int)width,
            (int)height,
            device_context,
            observation.source.x,
            observation.source.y + (int)(22 * cell_height),
            SRCCOPY) ||
        !GdiFlush()) {
        goto cleanup;
    }
    for (index = 0; index < width * height; index++) {
        size_t offset = index * 4;

        if (pixels[offset + 2] >= 128 && pixels[offset + 1] >= 128 &&
            pixels[offset] >= 128) {
            bright_pixels++;
        }
    }
    result = bright_pixels >= cell_width * cell_height;

cleanup:
    if (previous != NULL && previous != HGDI_ERROR) {
        (void)SelectObject(memory_context, previous);
    }
    if (bitmap != NULL) {
        (void)DeleteObject(bitmap);
    }
    if (memory_context != NULL) {
        (void)DeleteDC(memory_context);
    }
    if (device_context != NULL) {
        (void)ReleaseDC(observation.context_window, device_context);
    }
    return result;
}

static bool run_one_external_emitter_case(
    const wchar_t *sample_path,
    const wchar_t *emitter_path,
    bool allow_external)
{
    HANDLE process = NULL;
    HWND window = NULL;
    bool any_pixels_match = false;
    bool hold_visible = false;
    bool pixels_match = false;
    bool passed = false;
    size_t attempt;

    if (!start_external_emitter_sample(
            sample_path,
            emitter_path,
            allow_external,
            &process,
            &window)) {
        goto cleanup;
    }
    for (attempt = 0; attempt < 500; attempt++) {
        if (!RedrawWindow(
                window,
                NULL,
                NULL,
                RDW_INVALIDATE | RDW_UPDATENOW)) {
            goto cleanup;
        }
        any_pixels_match = external_emitter_any_pixel_matches(window);
        pixels_match = external_emitter_pixels_match(window);
        hold_visible = external_emitter_hold_visible(window);
        if ((!allow_external && any_pixels_match) ||
            (hold_visible && (pixels_match == allow_external))) {
            break;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            goto cleanup;
        }
    }
    if (attempt == 500 || !hold_visible ||
        pixels_match != allow_external ||
        (!allow_external && any_pixels_match) ||
        !post_key_press(window, VK_RETURN) ||
        !wait_for_successful_process(process)) {
        goto cleanup;
    }
    passed = true;

cleanup:
    if (process != NULL) {
        if (!passed && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process, 1);
            (void)WaitForSingleObject(process, INFINITE);
        }
        CloseHandle(process);
    }
    if (!passed) {
        fprintf(
            stderr,
            "emitter external policy failed "
            "(allow=%d hold=%d pixels=%d any=%d)\n",
            allow_external,
            hold_visible,
            pixels_match,
            any_pixels_match);
    }
    return passed;
}

static bool run_emitter_external_policy(
    const wchar_t *sample_path,
    const wchar_t *emitter_path)
{
    return run_one_external_emitter_case(
               sample_path,
               emitter_path,
               false) &&
           run_one_external_emitter_case(
               sample_path,
               emitter_path,
               true);
}

static bool run_emitter_title_case(
    const wchar_t *sample_path,
    const wchar_t *emitter_path)
{
    HANDLE process = NULL;
    HWND window = NULL;
    bool passed = false;

    if (!start_emitter_sample(
            sample_path,
            emitter_path,
            L"osc-titles",
            true,
            false,
            NULL,
            &process,
            &window) ||
        !wait_for_window_title(
            process,
            window,
            L"boxrig emitter window") ||
        !post_key_press(window, VK_RETURN) ||
        !wait_for_successful_process(process)) {
        goto cleanup;
    }
    passed = true;

cleanup:
    if (process != NULL) {
        if (!passed && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process, 1);
            (void)WaitForSingleObject(process, INFINITE);
        }
        CloseHandle(process);
    }
    if (!passed) {
        fprintf(stderr, "emitter title case failed\n");
    }
    return passed;
}

static bool clipboard_text_matches(
    HWND expected_owner,
    const wchar_t *expected_text)
{
    HGLOBAL handle;
    const wchar_t *text;
    SIZE_T allocation_size;
    size_t expected_length = wcslen(expected_text);
    bool matches = false;

    if (GetClipboardOwner() != expected_owner ||
        !OpenClipboard(expected_owner)) {
        return false;
    }
    handle = (HGLOBAL)GetClipboardData(CF_UNICODETEXT);
    if (handle == NULL) {
        goto cleanup;
    }
    allocation_size = GlobalSize(handle);
    text = GlobalLock(handle);
    if (text == NULL) {
        goto cleanup;
    }
    if (allocation_size / sizeof(*text) > expected_length &&
        text[expected_length] == L'\0' &&
        wmemcmp(text, expected_text, expected_length) == 0) {
        matches = true;
    }
    (void)GlobalUnlock(handle);

cleanup:
    (void)CloseClipboard();
    return matches;
}

static bool wait_for_emitter_clipboard(HANDLE process, HWND window)
{
    static const wchar_t expected[] = L"boxrig clipboard fixture";
    size_t attempt;

    for (attempt = 0; attempt < 250; attempt++) {
        if (clipboard_text_matches(window, expected)) {
            return true;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            return false;
        }
    }
    return false;
}

static bool run_emitter_clipboard_case(
    const wchar_t *sample_path,
    const wchar_t *emitter_path)
{
    HANDLE process = NULL;
    HWND window = NULL;
    HWND clipboard_owner;
    bool passed = false;

    if (!start_emitter_sample(
            sample_path,
            emitter_path,
            L"clipboard-write",
            true,
            false,
            NULL,
            &process,
            &window)) {
        goto cleanup;
    }
    clipboard_owner = terminal_input_window_for_host(window);
    if (clipboard_owner == NULL ||
        !wait_for_emitter_clipboard(process, clipboard_owner) ||
        !post_key_press(window, VK_RETURN) ||
        !wait_for_successful_process(process)) {
        goto cleanup;
    }
    passed = true;

cleanup:
    if (process != NULL) {
        if (!passed && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process, 1);
            (void)WaitForSingleObject(process, INFINITE);
        }
        CloseHandle(process);
    }
    if (!passed) {
        fprintf(stderr, "emitter clipboard case failed\n");
    }
    return passed;
}

static bool set_clipboard_text(HWND owner, const wchar_t *text)
{
    size_t length = wcslen(text);
    SIZE_T allocation_size;
    HGLOBAL allocation;
    wchar_t *destination;
    bool clipboard_open = false;
    bool passed = false;

    if (length > SIZE_MAX / sizeof(*destination) - 1) {
        return false;
    }
    allocation_size = (length + 1) * sizeof(*destination);
    allocation = GlobalAlloc(GMEM_MOVEABLE, allocation_size);
    if (allocation == NULL) {
        return false;
    }
    destination = GlobalLock(allocation);
    if (destination == NULL) {
        goto cleanup;
    }
    memcpy(destination, text, allocation_size);
    (void)GlobalUnlock(allocation);
    if (!OpenClipboard(owner)) {
        goto cleanup;
    }
    clipboard_open = true;
    if (!EmptyClipboard() ||
        SetClipboardData(CF_UNICODETEXT, allocation) == NULL) {
        goto cleanup;
    }
    allocation = NULL;
    passed = CloseClipboard() != 0;
    clipboard_open = false;

cleanup:
    if (clipboard_open) {
        (void)CloseClipboard();
    }
    if (allocation != NULL) {
        (void)GlobalFree(allocation);
    }
    return passed;
}

static bool wait_for_clipboard_text(
    HANDLE process,
    HWND owner,
    const wchar_t *text)
{
    size_t attempt;

    for (attempt = 0; attempt < 250; attempt++) {
        if (clipboard_text_matches(owner, text)) {
            return true;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            return false;
        }
    }
    return false;
}

static bool send_clipboard_shortcut(
    HWND input_window,
    UINT virtual_key,
    bool send_character)
{
    return send_key_message(input_window, VK_CONTROL, false) &&
           send_key_message(input_window, VK_SHIFT, false) &&
           send_key_message(input_window, virtual_key, false) &&
           (!send_character ||
            send_window_message(
                input_window,
                WM_CHAR,
                virtual_key,
                key_parameter(virtual_key, false))) &&
           send_key_message(input_window, virtual_key, true) &&
           send_key_message(input_window, VK_SHIFT, true) &&
           send_key_message(input_window, VK_CONTROL, true);
}

static bool send_shift_insert(HWND input_window)
{
    LPARAM press = key_parameter(VK_INSERT, false) | ((LPARAM)1 << 24);
    LPARAM release = key_parameter(VK_INSERT, true) | ((LPARAM)1 << 24);

    return send_key_message(input_window, VK_SHIFT, false) &&
           send_window_message(
               input_window,
               WM_KEYDOWN,
               VK_INSERT,
               press) &&
           send_window_message(
               input_window,
               WM_KEYUP,
               VK_INSERT,
               release) &&
           send_key_message(input_window, VK_SHIFT, true);
}

static bool send_terminal_mouse_button(
    HWND input_window,
    UINT message,
    WPARAM state,
    int x,
    int y)
{
    return send_window_message(
        input_window,
        message,
        state,
        MAKELPARAM(x, y));
}

static bool run_host_input_policy(
    HANDLE process,
    HWND host_window)
{
    HWND input_window = terminal_input_window_for_host(host_window);
    HWND scrollbar = scrollbar_window_for_host(host_window);
    RECT client;
    uint64_t rendered_hash;
    LONG content_width;
    LONG scrollbar_width;
    int cell_width;
    int cell_height;
    const char *stage = "find input component";
    bool passed = false;

    if (input_window == NULL || scrollbar == NULL ||
        !GetClientRect(input_window, &client) ||
        client.right <= 0 || client.bottom <= 0) {
        goto cleanup;
    }
    scrollbar_width = scrollbar_width_for_host(host_window);
    content_width = client.right - scrollbar_width;
    if (content_width <= 0 || content_width % 80 != 0 ||
        client.bottom % 24 != 0) {
        goto cleanup;
    }
    cell_width = content_width / 80;
    cell_height = client.bottom / 24;

    stage = "render selection fixture";
    if (!commit_character(host_window, L'x') ||
        !wait_for_window_title(
            process,
            host_window,
            L"host-input-ready") ||
        !wait_for_stable_client_hash(
            process,
            host_window,
            &rendered_hash,
            true)) {
        goto cleanup;
    }
    stage = "select copy fixture";
    if (!send_terminal_mouse_button(
            input_window,
            WM_LBUTTONDOWN,
            MK_LBUTTON,
            cell_width / 2,
            cell_height / 2) ||
        !send_terminal_mouse_button(
            input_window,
            WM_MOUSEMOVE,
            MK_LBUTTON,
            10 * cell_width + cell_width / 2,
            cell_height / 2) ||
        !send_terminal_mouse_button(
            input_window,
            WM_LBUTTONUP,
            0,
            10 * cell_width + cell_width / 2,
            cell_height / 2)) {
        goto cleanup;
    }
    stage = "copy shortcut";
    if (!send_clipboard_shortcut(input_window, L'C', true) ||
        !wait_for_clipboard_text(
            process,
            input_window,
            L"copy-target")) {
        goto cleanup;
    }

    stage = "double click word selection";
    if (!send_terminal_mouse_button(
            input_window,
            WM_LBUTTONDBLCLK,
            MK_LBUTTON,
            cell_width / 2,
            cell_height / 2) ||
        !send_terminal_mouse_button(
            input_window,
            WM_LBUTTONUP,
            0,
            cell_width / 2,
            cell_height / 2) ||
        !send_clipboard_shortcut(input_window, L'C', true) ||
        !wait_for_clipboard_text(process, input_window, L"copy")) {
        goto cleanup;
    }
    stage = "triple click logical line selection";
    if (!send_terminal_mouse_button(
            input_window,
            WM_LBUTTONDOWN,
            MK_LBUTTON,
            cell_width / 2,
            cell_height / 2) ||
        !send_terminal_mouse_button(
            input_window,
            WM_LBUTTONUP,
            0,
            cell_width / 2,
            cell_height / 2) ||
        !send_clipboard_shortcut(input_window, L'C', true) ||
        !wait_for_clipboard_text(
            process,
            input_window,
            L"copy-target\r\n")) {
        goto cleanup;
    }

    stage = "clipboard paste shortcut";
    if (!set_clipboard_text(host_window, L"P") ||
        !send_clipboard_shortcut(input_window, L'V', true) ||
        !wait_for_window_title(
            process,
            host_window,
            L"clipboard-paste-received")) {
        goto cleanup;
    }
    stage = "shift insert paste shortcut";
    if (!set_clipboard_text(host_window, L"I") ||
        !send_shift_insert(input_window) ||
        !wait_for_window_title(
            process,
            host_window,
            L"insert-paste-received")) {
        goto cleanup;
    }

    stage = "enable terminal mouse reporting";
    if (!commit_character(host_window, L'r') ||
        !wait_for_window_title(
            process,
            host_window,
            L"mouse-reporting-ready")) {
        goto cleanup;
    }
    stage = "send terminal mouse report";
    if (!send_terminal_mouse_button(
            input_window,
            WM_LBUTTONDOWN,
            MK_LBUTTON,
            20 * cell_width + cell_width / 2,
            cell_height + cell_height / 2) ||
        !send_terminal_mouse_button(
            input_window,
            WM_LBUTTONUP,
            0,
            20 * cell_width + cell_width / 2,
            cell_height + cell_height / 2)) {
        goto cleanup;
    }
    stage = "receive terminal mouse report";
    if (!wait_for_window_title(
            process,
            host_window,
            L"mouse-terminal-received")) {
        goto cleanup;
    }

    stage = "shift mouse override";
    if (!send_terminal_mouse_button(
            input_window,
            WM_LBUTTONDOWN,
            MK_SHIFT | MK_LBUTTON,
            cell_width / 2,
            cell_height / 2) ||
        !send_terminal_mouse_button(
            input_window,
            WM_MOUSEMOVE,
            MK_LBUTTON,
            4 * cell_width + cell_width / 2,
            cell_height / 2) ||
        !send_terminal_mouse_button(
            input_window,
            WM_LBUTTONUP,
            0,
            4 * cell_width + cell_width / 2,
            cell_height / 2) ||
        !send_clipboard_shortcut(input_window, L'C', true) ||
        !wait_for_clipboard_text(process, input_window, L"mouse")) {
        goto cleanup;
    }

    stage = "control rectangular mouse override";
    if (!send_terminal_mouse_button(
            input_window,
            WM_LBUTTONDOWN,
            MK_CONTROL | MK_LBUTTON,
            cell_width / 4,
            cell_height / 2) ||
        !send_terminal_mouse_button(
            input_window,
            WM_MOUSEMOVE,
            MK_SHIFT | MK_LBUTTON,
            4 * cell_width + (3 * cell_width) / 4,
            cell_height + cell_height / 2) ||
        !send_terminal_mouse_button(
            input_window,
            WM_LBUTTONUP,
            0,
            4 * cell_width + (3 * cell_width) / 4,
            cell_height + cell_height / 2) ||
        !send_clipboard_shortcut(input_window, L'C', true) ||
        !wait_for_clipboard_text(
            process,
            input_window,
            L"mouse\r\nblock")) {
        goto cleanup;
    }
    passed = true;

cleanup:
    if (!passed) {
        fprintf(stderr, "Win32 host input policy failed at %s\n", stage);
    }
    return passed;
}

static bool wait_for_emitter_notification_delivery(
    HANDLE process,
    const wchar_t *log_path)
{
    size_t attempt;

    for (attempt = 0; attempt < 250; attempt++) {
        HANDLE file = CreateFileW(
            log_path,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL);

        if (file != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size;
            bool delivered = GetFileSizeEx(file, &size) &&
                             size.QuadPart >=
                                 (LONGLONG)(3 * sizeof(DWORD));

            CloseHandle(file);
            if (delivered) {
                return true;
            }
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            return false;
        }
    }
    return false;
}

static bool run_emitter_notification_case(
    const wchar_t *sample_path,
    const wchar_t *emitter_path,
    const wchar_t *log_path,
    bool allow_notifications)
{
    HANDLE process = NULL;
    HWND window = NULL;
    uint64_t rendered_hash;
    bool passed = false;

    if (!start_emitter_sample(
            sample_path,
            emitter_path,
            L"desktop-notification",
            true,
            allow_notifications,
            log_path,
            &process,
            &window) ||
        !wait_for_stable_client_hash(
            process,
            window,
            &rendered_hash,
            true) ||
        (allow_notifications &&
         !wait_for_emitter_notification_delivery(process, log_path)) ||
        !post_key_press(window, VK_RETURN) ||
        !wait_for_successful_process(process)) {
        goto cleanup;
    }
    passed = true;

cleanup:
    if (process != NULL) {
        if (!passed && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process, 1);
            (void)WaitForSingleObject(process, INFINITE);
        }
        CloseHandle(process);
    }
    if (!passed) {
        fprintf(
            stderr,
            "emitter notification case failed (allow=%d)\n",
            allow_notifications ? 1 : 0);
    }
    return passed;
}

static bool start_notification_sample(
    const wchar_t *sample_path,
    const wchar_t *helper_path,
    const wchar_t *log_path,
    bool allow_notifications,
    HANDLE *process_out,
    HWND *window_out)
{
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    wchar_t *command_line = make_command_line(sample_path, helper_path);
    window_search search;
    size_t attempt;

    *process_out = NULL;
    *window_out = NULL;
    if (command_line == NULL ||
        !SetEnvironmentVariableW(
            L"BOXRIG_NOTIFICATION_TEST_LOG",
            log_path)) {
        free(command_line);
        return false;
    }
    if (allow_notifications) {
        if (!SetEnvironmentVariableW(
                L"BOXRIG_ALLOW_NOTIFICATIONS",
                L"1")) {
            (void)SetEnvironmentVariableW(
                L"BOXRIG_NOTIFICATION_TEST_LOG",
                NULL);
            free(command_line);
            return false;
        }
    } else {
        (void)SetEnvironmentVariableW(
            L"BOXRIG_ALLOW_NOTIFICATIONS",
            NULL);
    }
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));
    if (!CreateProcessW(
            NULL,
            command_line,
            NULL,
            NULL,
            FALSE,
            0,
            NULL,
            NULL,
            &startup,
            &process)) {
        (void)SetEnvironmentVariableW(
            L"BOXRIG_ALLOW_NOTIFICATIONS",
            NULL);
        (void)SetEnvironmentVariableW(
            L"BOXRIG_NOTIFICATION_TEST_LOG",
            NULL);
        free(command_line);
        return false;
    }
    free(command_line);
    CloseHandle(process.hThread);
    if (WaitForInputIdle(process.hProcess, 10000) != 0) {
        (void)SetEnvironmentVariableW(L"BOXRIG_ALLOW_NOTIFICATIONS", NULL);
        (void)SetEnvironmentVariableW(L"BOXRIG_NOTIFICATION_TEST_LOG", NULL);
        (void)TerminateProcess(process.hProcess, 1);
        (void)WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hProcess);
        return false;
    }
    search.process_id = process.dwProcessId;
    search.window = NULL;
    for (attempt = 0; attempt < 250 && search.window == NULL; attempt++) {
        (void)EnumWindows(find_process_window, (LPARAM)&search);
        if (search.window == NULL &&
            WaitForSingleObject(process.hProcess, 20) != WAIT_TIMEOUT) {
            break;
        }
    }
    if (search.window == NULL ||
        !wait_for_window_title(
            process.hProcess,
            search.window,
            L"helper-ready")) {
        (void)SetEnvironmentVariableW(L"BOXRIG_ALLOW_NOTIFICATIONS", NULL);
        (void)SetEnvironmentVariableW(L"BOXRIG_NOTIFICATION_TEST_LOG", NULL);
        (void)TerminateProcess(process.hProcess, 1);
        (void)WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hProcess);
        return false;
    }
    (void)SetEnvironmentVariableW(L"BOXRIG_ALLOW_NOTIFICATIONS", NULL);
    (void)SetEnvironmentVariableW(L"BOXRIG_NOTIFICATION_TEST_LOG", NULL);
    *process_out = process.hProcess;
    *window_out = search.window;
    return true;
}

static bool run_notification_sample(
    const wchar_t *sample_path,
    const wchar_t *helper_path,
    const wchar_t *log_path,
    bool allow_notifications)
{
    HANDLE process = NULL;
    HWND window = NULL;
    DWORD exit_code = 1;
    const char *stage = "start";
    bool passed = false;

    if (!start_notification_sample(
            sample_path,
            helper_path,
            log_path,
            allow_notifications,
            &process,
            &window)) {
        goto cleanup;
    }
    stage = "notification input";
    if (!commit_character(window, L'n')) {
        goto cleanup;
    }
    stage = "notification processing";
    if (!wait_for_window_title(
            process,
            window,
            L"notification-processed")) {
        goto cleanup;
    }
    stage = "quit input";
    if (!commit_character(window, L'q')) {
        goto cleanup;
    }
    stage = "child exit";
    if (WaitForSingleObject(process, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process, &exit_code) || exit_code != 0) {
        goto cleanup;
    }
    passed = true;

cleanup:
    if (process != NULL) {
        if (!passed && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process, 1);
            (void)WaitForSingleObject(process, INFINITE);
        }
        CloseHandle(process);
    }
    if (!passed) {
        fprintf(
            stderr,
            "notification sample failed at %s (allow=%d, error=%lu)\n",
            stage,
            allow_notifications ? 1 : 0,
            GetLastError());
    }
    return passed;
}

static bool notification_log_matches(
    const wchar_t *path,
    const DWORD *expected_messages,
    size_t expected_count)
{
    DWORD messages[4];
    LARGE_INTEGER size;
    HANDLE file;
    DWORD read_length = 0;
    size_t index;
    bool matches = false;

    if (expected_count > sizeof(messages) / sizeof(messages[0])) {
        return false;
    }
    size.QuadPart = -1;
    file = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (!GetFileSizeEx(file, &size) ||
        size.QuadPart != (LONGLONG)(expected_count * sizeof(DWORD))) {
        fprintf(
            stderr,
            "notification log size was %lld, expected %llu\n",
            (long long)size.QuadPart,
            (unsigned long long)(expected_count * sizeof(DWORD)));
        goto cleanup;
    }
    if (expected_count != 0 &&
        (!ReadFile(
             file,
             messages,
             (DWORD)(expected_count * sizeof(DWORD)),
             &read_length,
             NULL) ||
         read_length != expected_count * sizeof(DWORD))) {
        goto cleanup;
    }
    for (index = 0; index < expected_count; index++) {
        if (messages[index] != expected_messages[index]) {
            fprintf(
                stderr,
                "notification log message %llu was %lu, expected %lu\n",
                (unsigned long long)index,
                messages[index],
                expected_messages[index]);
            goto cleanup;
        }
    }
    matches = true;

cleanup:
    CloseHandle(file);
    return matches;
}

static bool run_notification_policy(
    const wchar_t *sample_path,
    const wchar_t *helper_path)
{
    static const DWORD expected_messages[] = {
        NIM_ADD,
        NIM_SETVERSION,
        NIM_MODIFY,
        NIM_DELETE,
    };
    wchar_t temporary_directory[MAX_PATH];
    wchar_t log_path[MAX_PATH];
    DWORD temporary_length;
    bool passed = false;

    temporary_length = GetTempPathW(
        (DWORD)(sizeof(temporary_directory) /
                sizeof(temporary_directory[0])),
        temporary_directory);
    if (temporary_length == 0 ||
        temporary_length >=
            sizeof(temporary_directory) / sizeof(temporary_directory[0]) ||
        GetTempFileNameW(
            temporary_directory,
            L"brg",
            0,
            log_path) == 0) {
        return false;
    }
    if (!run_notification_sample(
            sample_path,
            helper_path,
            log_path,
            false)) {
        fprintf(stderr, "default-deny notification sample failed\n");
        goto cleanup;
    }
    if (!notification_log_matches(log_path, NULL, 0)) {
        fprintf(stderr, "default-deny notification sample called Shell\n");
        goto cleanup;
    }
    if (!run_notification_sample(
            sample_path,
            helper_path,
            log_path,
            true)) {
        fprintf(stderr, "opt-in notification sample failed\n");
        goto cleanup;
    }
    if (!notification_log_matches(
            log_path,
            expected_messages,
            sizeof(expected_messages) / sizeof(expected_messages[0]))) {
        fprintf(stderr, "opt-in notification Shell lifecycle differed\n");
        goto cleanup;
    }
    passed = true;

cleanup:
    (void)DeleteFileW(log_path);
    return passed;
}

static bool run_emitter_notification_policy(
    const wchar_t *sample_path,
    const wchar_t *emitter_path)
{
    static const DWORD expected_messages[] = {
        NIM_ADD,
        NIM_SETVERSION,
        NIM_MODIFY,
        NIM_DELETE,
    };
    wchar_t temporary_directory[MAX_PATH];
    wchar_t log_path[MAX_PATH];
    DWORD temporary_length;
    bool passed = false;

    temporary_length = GetTempPathW(
        (DWORD)(sizeof(temporary_directory) /
                sizeof(temporary_directory[0])),
        temporary_directory);
    if (temporary_length == 0 ||
        temporary_length >=
            sizeof(temporary_directory) / sizeof(temporary_directory[0]) ||
        GetTempFileNameW(
            temporary_directory,
            L"brg",
            0,
            log_path) == 0) {
        return false;
    }
    if (!run_emitter_notification_case(
            sample_path,
            emitter_path,
            log_path,
            false) ||
        !notification_log_matches(log_path, NULL, 0)) {
        fprintf(stderr, "default-deny emitter notification failed\n");
        goto cleanup;
    }
    if (!run_emitter_notification_case(
            sample_path,
            emitter_path,
            log_path,
            true) ||
        !notification_log_matches(
            log_path,
            expected_messages,
            sizeof(expected_messages) / sizeof(expected_messages[0]))) {
        fprintf(stderr, "opt-in emitter notification failed\n");
        goto cleanup;
    }
    passed = true;

cleanup:
    (void)DeleteFileW(log_path);
    return passed;
}

static bool wine_conpty_normalization_is_enabled(void)
{
    wchar_t value[16];
    DWORD length = GetEnvironmentVariableW(
        L"BOXRIG_TEST_ALLOW_CONPTY_NORMALIZATION",
        value,
        (DWORD)(sizeof(value) / sizeof(value[0])));

    return length == 4 && wcscmp(value, L"wine") == 0;
}

static bool run_emitter_conpty_cases(
    const wchar_t *sample_path,
    const wchar_t *emitter_path)
{
    bool wine_conpty = wine_conpty_normalization_is_enabled();

    if (!run_emitter_key_case(
            sample_path,
            emitter_path,
            L"text-cjk",
            true,
            VK_RETURN)) {
        return false;
    }
    if (wine_conpty) {
        printf("SKIP Win32 emitter query: Wine ConPTY consumes DSR without a response\n");
    } else if (!run_emitter_query_case(
                   sample_path,
                   emitter_path,
                   L"query-cursor-position")) {
        return false;
    }
    if (wine_conpty) {
        printf(
            "SKIP Win32 Kitty graphics query: Wine ConPTY does not return "
            "APC responses\n");
    } else if (!run_emitter_query_case(
                   sample_path,
                   emitter_path,
                   L"query-kitty-graphics")) {
        return false;
    }
    if (wine_conpty) {
        printf(
            "SKIP Win32 Kitty graphics fixture: Wine ConPTY does not "
            "preserve APC output\n");
    } else if (!run_emitter_graphics_case(sample_path, emitter_path)) {
        return false;
    }
    if (wine_conpty) {
        printf(
            "SKIP Win32 Kitty animation and external fixtures: Wine ConPTY "
            "does not preserve APC output\n");
    } else if (!run_emitter_animation_case(sample_path, emitter_path) ||
               !run_emitter_external_policy(sample_path, emitter_path)) {
        return false;
    }
    if (!run_emitter_ime_case(sample_path, emitter_path)) {
        return false;
    }
    if (wine_conpty) {
        printf("SKIP Win32 emitter title: Wine ConPTY owns OSC title normalization\n");
    } else if (!run_emitter_title_case(sample_path, emitter_path)) {
        return false;
    }
    return true;
}

static bool run_emitter_sample_cases(
    const wchar_t *sample_path,
    const wchar_t *notification_sample_path,
    const wchar_t *emitter_path)
{
    if (!run_emitter_conpty_cases(sample_path, emitter_path)) {
        return false;
    }
    return run_emitter_clipboard_case(sample_path, emitter_path) &&
           run_emitter_notification_policy(
               notification_sample_path,
               emitter_path);
}

static wchar_t *make_shared_scenario_command_line(
    const wchar_t *sample_path,
    const wchar_t *helper_path)
{
    const wchar_t *renderer = _wgetenv(L"BOXRIG_TEST_SAMPLE_RENDERER");
    if (renderer == NULL) renderer = L"";
    size_t capacity = wcslen(sample_path) + wcslen(helper_path) + wcslen(renderer) + 48u;
    wchar_t *command_line = calloc(capacity, sizeof(*command_line));
    int length;

    if (command_line == NULL) {
        return NULL;
    }
    length = swprintf(
        command_line,
        capacity,
        L"\"%ls\" %ls -- \"%ls\" --sample-scenario",
        sample_path,
        renderer,
        helper_path);
    if (length < 0 || (size_t)length >= capacity) {
        free(command_line);
        return NULL;
    }
    return command_line;
}

static bool start_shared_scenario_sample(
    const wchar_t *sample_path,
    const wchar_t *helper_path,
    HANDLE *process_out,
    HWND *window_out)
{
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    wchar_t *command_line = make_shared_scenario_command_line(
        sample_path,
        helper_path);
    bool created;

    *process_out = NULL;
    *window_out = NULL;
    if (command_line == NULL) {
        return false;
    }
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));
    (void)SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", NULL);
    created = CreateProcessW(
                  NULL,
                  command_line,
                  NULL,
                  NULL,
                  FALSE,
                  0,
                  NULL,
                  NULL,
                  &startup,
                  &process) != 0;
    free(command_line);
    if (!created) {
        return false;
    }
    CloseHandle(process.hThread);
    if (!wait_for_emitter_window(
            process.hProcess,
            process.dwProcessId,
            window_out)) {
        (void)TerminateProcess(process.hProcess, 1);
        (void)WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hProcess);
        *window_out = NULL;
        return false;
    }
    *process_out = process.hProcess;
    return true;
}

static bool wait_for_shared_scenario_title(
    HANDLE process,
    HWND window,
    const wchar_t *expected_title)
{
    static const wchar_t unexpected_prefix[] =
        BOXRIG_TEST_WIDE_LITERAL(
            BOXRIG_TEST_FRONTEND_SAMPLE_UNEXPECTED_PREFIX);
    size_t attempt;
    wchar_t title[160] = L"";

    for (attempt = 0u; attempt < 750u; attempt++) {
        (void)GetWindowTextW(
            window,
            title,
            (int)(sizeof(title) / sizeof(title[0])));
        if (wcscmp(title, expected_title) == 0) {
            return true;
        }
        if (wcsncmp(
                title,
                unexpected_prefix,
                (sizeof(unexpected_prefix) / sizeof(unexpected_prefix[0])) -
                    1u) == 0) {
            fwprintf(stderr, L"controlled child reported %ls\n", title);
            return false;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            DWORD exit_code = 0;

            (void)GetExitCodeProcess(process, &exit_code);
            fwprintf(stderr, L"sample exited %lu awaiting %ls; last title %ls\n",
                     (unsigned long)exit_code, expected_title, title);
            return false;
        }
    }
    fwprintf(stderr, L"sample awaiting %ls; last title %ls\n", expected_title, title);
    return false;
}

static bool wait_for_shared_geometry_zoom_title(
    HANDLE process,
    HWND window,
    unsigned int maximized_rows,
    unsigned int maximized_columns,
    bool fixed_console)
{
    static const wchar_t prefix[] = BOXRIG_TEST_WIDE_LITERAL(
        BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PREFIX "maximize-zoom-grid-");
    static const wchar_t pending_prefix[] = BOXRIG_TEST_WIDE_LITERAL(
        BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PENDING_PREFIX "maximize-zoom-");
    static const wchar_t fixed_suffix[] = BOXRIG_TEST_WIDE_LITERAL(
        BOXRIG_TEST_FRONTEND_SAMPLE_FIXED_GRID_SUFFIX);
    static const wchar_t unexpected_prefix[] = BOXRIG_TEST_WIDE_LITERAL(
        BOXRIG_TEST_FRONTEND_SAMPLE_UNEXPECTED_PREFIX);
    size_t prefix_length =
        (sizeof(prefix) / sizeof(prefix[0])) - 1u;
    unsigned int last_pending_sequence = 0u;
    size_t attempt;

    for (attempt = 0u; attempt < 750u; attempt++) {
        wchar_t title[160] = L"";

        (void)GetWindowTextW(
            window,
            title,
            (int)(sizeof(title) / sizeof(title[0])));
        if (wcsncmp(
                title,
                pending_prefix,
                (sizeof(pending_prefix) / sizeof(pending_prefix[0])) - 1u) == 0) {
            unsigned int pending_sequence;
            unsigned int pending_rows;
            unsigned int pending_columns;
            int pending_parsed = 0;
            size_t pending_prefix_length =
                (sizeof(pending_prefix) / sizeof(pending_prefix[0])) - 1u;

            if (swscanf(title + pending_prefix_length, L"%u-grid-%u-%u%n",
                    &pending_sequence, &pending_rows, &pending_columns,
                    &pending_parsed) != 3 ||
                title[pending_prefix_length + (size_t)pending_parsed] != L'\0') {
                return false;
            }
            if (pending_sequence > last_pending_sequence) {
                if (!commit_character(window, L'z')) {
                    return false;
                }
                last_pending_sequence = pending_sequence;
            }
        } else if (wcsncmp(title, prefix, prefix_length) == 0) {
            unsigned int rows;
            unsigned int columns;
            int parsed = 0;
            const wchar_t *suffix;

            if (swscanf(title + prefix_length, L"%u-%u%n",
                    &rows, &columns, &parsed) != 2) {
                return false;
            }
            suffix = title + prefix_length + (size_t)parsed;
            if (fixed_console) {
                return rows == BOXRIG_TEST_FRONTEND_SAMPLE_ROWS &&
                       columns == BOXRIG_TEST_FRONTEND_SAMPLE_COLUMNS &&
                       wcscmp(suffix, fixed_suffix) == 0;
            }
            return suffix[0] == L'\0' &&
                   (rows > maximized_rows || columns > maximized_columns);
        } else if (wcsncmp(
                       title,
                       unexpected_prefix,
                       (sizeof(unexpected_prefix) /
                        sizeof(unexpected_prefix[0])) - 1u) == 0) {
            fwprintf(stderr, L"controlled child reported %ls\n", title);
            return false;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            return false;
        }
    }
    return false;
}

static bool wait_for_shared_geometry_restore_title(
    HANDLE process,
    HWND window,
    const wchar_t *expected_title)
{
    static const wchar_t pending_prefix[] = BOXRIG_TEST_WIDE_LITERAL(
        BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PENDING_PREFIX "maximize-reset-");
    size_t pending_prefix_length =
        (sizeof(pending_prefix) / sizeof(pending_prefix[0])) - 1u;
    unsigned int last_pending_sequence = 0u;
    size_t attempt;
    wchar_t title[160] = L"";

    for (attempt = 0u; attempt < 750u; attempt++) {
        (void)GetWindowTextW(
            window,
            title,
            (int)(sizeof(title) / sizeof(title[0])));
        if (wcscmp(title, expected_title) == 0) {
            return true;
        }
        if (wcsncmp(title, pending_prefix, pending_prefix_length) == 0) {
            unsigned int pending_sequence;
            unsigned int pending_rows;
            unsigned int pending_columns;
            int pending_parsed = 0;

            if (swscanf(title + pending_prefix_length, L"%u-grid-%u-%u%n",
                    &pending_sequence, &pending_rows, &pending_columns,
                    &pending_parsed) != 3 ||
                title[pending_prefix_length + (size_t)pending_parsed] != L'\0') {
                return false;
            }
            if (pending_sequence > last_pending_sequence) {
                if (!commit_character(window, L'm')) {
                    return false;
                }
                last_pending_sequence = pending_sequence;
            }
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            DWORD code = STILL_ACTIVE;
            (void)GetExitCodeProcess(process, &code);
            fwprintf(stderr, L"restore process exited: code=%lu expected=%ls last=%ls\n",
                (unsigned long)code, expected_title, title);
            return false;
        }
    }
    {
        RECT client = {0};
        (void)GetClientRect(window, &client);
        fwprintf(stderr, L"restore pending: expected=%ls last=%ls maximized=%d client=%ldx%ld\n",
            expected_title, title, IsZoomed(window), client.right, client.bottom);
    }
    return false;
}

static bool wait_for_shared_scenario_ready(
    HANDLE process,
    HWND window,
    unsigned int *cell_height_out,
    unsigned int *cell_width_out)
{
    static const wchar_t prefix[] =
        BOXRIG_TEST_WIDE_LITERAL(BOXRIG_TEST_FRONTEND_SAMPLE_READY_PREFIX);
    size_t prefix_length =
        (sizeof(prefix) / sizeof(prefix[0])) - 1u;
    size_t attempt;

    for (attempt = 0u; attempt < 750u; attempt++) {
        wchar_t title[160] = L"";
        int parsed_length = 0;

        (void)GetWindowTextW(
            window,
            title,
            (int)(sizeof(title) / sizeof(title[0])));
        if (wcsncmp(title, prefix, prefix_length) == 0 &&
            swscanf(
                title + prefix_length,
                L"%u-%u%n",
                cell_height_out,
                cell_width_out,
                &parsed_length) == 2 &&
            parsed_length > 0 &&
            title[prefix_length + (size_t)parsed_length] == L'\0' &&
            *cell_height_out != 0u && *cell_width_out != 0u) {
            return true;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            return false;
        }
    }
    return false;
}

static bool send_shared_scenario_drag(
    HWND input_window,
    WPARAM modifier,
    int start_x,
    int start_y,
    int end_x,
    int end_y)
{
    return send_terminal_mouse_button(
               input_window,
               WM_MOUSEMOVE,
               0,
               start_x,
               start_y) &&
           send_terminal_mouse_button(
               input_window,
               WM_LBUTTONDOWN,
               modifier | MK_LBUTTON,
               start_x,
               start_y) &&
           send_terminal_mouse_button(
               input_window,
               WM_MOUSEMOVE,
               modifier | MK_LBUTTON,
               end_x,
               end_y) &&
           send_terminal_mouse_button(
               input_window,
               WM_LBUTTONUP,
               modifier,
               end_x,
               end_y);
}

static bool round_trip_shared_scenario_selection(
    HANDLE process,
    HWND host_window,
    HWND input_window,
    const wchar_t *expected_clipboard,
    const wchar_t *expected_title)
{
    return set_clipboard_text(host_window, L"stale-selection") &&
           send_clipboard_shortcut(input_window, L'C', true) &&
           wait_for_clipboard_text(
               process,
               input_window,
               expected_clipboard) &&
           send_clipboard_shortcut(input_window, L'V', true) &&
           wait_for_shared_scenario_title(
               process,
               host_window,
               expected_title);
}

static bool read_sample_overlay_region(
    HWND window, unsigned int cell_width, unsigned int cell_height,
    unsigned int column, unsigned int row, unsigned int columns,
    uint64_t *value_out)
{
    RECT rectangle;
    uint32_t rgb;
    uint64_t hash;
    size_t different;

    rectangle.left = (LONG)(column * cell_width);
    rectangle.top = (LONG)(row * cell_height);
    if (columns == 0u) {
        rectangle.left += (LONG)(cell_width / 2u);
        rectangle.top += (LONG)(cell_height / 2u);
    }
    rectangle.right = rectangle.left +
                      (columns == 0u ? 1 : (LONG)(columns * cell_width));
    rectangle.bottom = rectangle.top +
                       (columns == 0u ? 1 : (LONG)cell_height);
    if (!capture_client_rectangle_hash(
            window, &rectangle, NULL, &rgb, &hash, &different)) {
        return false;
    }
    *value_out = columns == 0u ? rgb : hash;
    return true;
}

static bool wait_for_sample_overlay_region(
    HANDLE process, HWND window, unsigned int cell_width, unsigned int cell_height,
    unsigned int column, unsigned int row, unsigned int columns,
    uint64_t expected, bool equal)
{
    size_t attempt;
    uint64_t observed = 0u;

    for (attempt = 0u; attempt < 750u; attempt++) {
        if (read_sample_overlay_region(window, cell_width, cell_height,
                column, row, columns, &observed) &&
            ((observed == expected) == equal)) {
            return true;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            break;
        }
    }
    fprintf(stderr, "overlay region %u,%u width %u: %llx expected %s %llx\n",
            column, row, columns, (unsigned long long)observed,
            equal ? "==" : "!=", (unsigned long long)expected);
    return false;
}

static bool run_color_demo(HANDLE process, HWND input_window,
    unsigned int width, unsigned int height)
{
    uint64_t unregistered, identity, corrected, repeated, proof_hash;
    size_t proof_pixels;
#define COLOR_PROOF(name) (getenv("BOXRIG_TEST_MODAL_RESIZE_CAPTURE_PREFIX") == NULL || \
    capture_client_rectangle_hash(input_window, NULL, name, NULL, &proof_hash, &proof_pixels))
#define COLOR_WAIT(value) wait_for_sample_overlay_region(process, input_window, \
    width, height, 70, 15, 0, value, true)
#define COLOR_CAPTURE(value) read_sample_overlay_region(input_window, width, height, \
    0, 0, 60, value)
#define COLOR_TOGGLE() send_clipboard_shortcut(input_window, VK_F12, false)
    bool passed = COLOR_WAIT(0x404040) && COLOR_PROOF("color-corrected") &&
        COLOR_TOGGLE() && COLOR_WAIT(0x101010) && COLOR_PROOF("color-unregistered") &&
        COLOR_CAPTURE(&unregistered) && COLOR_TOGGLE() && COLOR_WAIT(0x101010) &&
        COLOR_CAPTURE(&identity) && identity == unregistered &&
        COLOR_TOGGLE() && COLOR_WAIT(0x404040) && COLOR_CAPTURE(&corrected) &&
        corrected != unregistered && COLOR_TOGGLE() && COLOR_WAIT(0x101010) &&
        COLOR_TOGGLE() && COLOR_WAIT(0x101010) && COLOR_TOGGLE() &&
        COLOR_WAIT(0x404040) && COLOR_CAPTURE(&repeated) && repeated == corrected &&
        COLOR_TOGGLE() && COLOR_WAIT(0x101010);
#undef COLOR_PROOF
#undef COLOR_WAIT
#undef COLOR_CAPTURE
#undef COLOR_TOGGLE
    if (passed) puts("PASS Win32 sample color filter off identity corrected cycle");
    return passed;
}

static bool move_shared_scenario_pointer(HWND input_window, int x, int y)
{
    POINT point = {x, y};

    /* Frame updates refresh hover from the OS pointer. Keep that position and
     * the synchronous input message consistent when observing the pixels. */
    return ClientToScreen(input_window, &point) &&
           SetCursorPos(point.x, point.y) &&
           send_terminal_mouse_button(input_window, WM_MOUSEMOVE, 0, x, y);
}

static bool run_shared_scenario_overlays(
    HANDLE process, HWND window, HWND input_window,
    unsigned int width, unsigned int height, const char **stage_out)
{
    uint64_t hyperlink;
    uint64_t search;
    uint64_t preedit;
    uint64_t cursor;
    uint64_t background;
    wchar_t mode[32];
    DWORD mode_length = GetEnvironmentVariableW(L"BOXRIG_TRANSPARENCY_MODE", mode, 32);
    bool opaque_mode = mode_length == 6 && wcscmp(mode, L"opaque") == 0;

#define CAPTURE(column, row, columns, output)                                   \
    read_sample_overlay_region(input_window, width, height,                    \
        column, row, columns, output)
#define WAIT(column, row, columns, value, equal)                                \
    wait_for_sample_overlay_region(process, input_window, width, height,       \
        column, row, columns, value, equal)
#define MOVE(column, row)                                                       \
    move_shared_scenario_pointer(input_window,                               \
        (int)((column) * width + width / 2u),                                  \
        (int)((row) * height + height / 2u))
#define CHECKPOINT(title)                                                       \
    (commit_character(window, L'v') &&                                         \
     wait_for_shared_scenario_title(process, window, BOXRIG_TEST_WIDE_LITERAL(title)))

    *stage_out = "present initial sample overlays";
    if (!MOVE(1u, 10u) ||
        !WAIT(14u, 11u, 0u, 0x205080u, true) ||
        !WAIT(0u, 14u, 0u, 0x00ff00u, true) ||
        !CAPTURE(8u, 9u, 19u, &hyperlink) ||
        !CAPTURE(0u, 11u, 14u, &search) ||
        !CAPTURE(0u, 13u, 16u, &preedit) ||
        !CAPTURE(30u, 18u, 0u, &background)) {
        return false;
    }
    *stage_out = "route native OSC8 URL and annotation hover";
    if (!MOVE(1u, 9u) || !WAIT(8u, 9u, 19u, hyperlink, false) ||
        !MOVE(1u, 10u) || !WAIT(8u, 9u, 19u, hyperlink, true) ||
        !MOVE(14u, 11u)) {
        return false;
    }
    *stage_out = "receive exact overlay IME commit";
    if (!CHECKPOINT(BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_COMMIT_TITLE)) {
        return false;
    }
    *stage_out = "present changed search preedit annotation and image";
    if (!WAIT(14u, 11u, 0u, 0x804020u, true) ||
        !WAIT(0u, 14u, 0u, 0x0000ffu, true) ||
        !WAIT(0u, 11u, 14u, search, false) ||
        !WAIT(0u, 13u, 16u, preedit, false) ||
        !CAPTURE(0u, 13u, 16u, &cursor)) {
        return false;
    }
    *stage_out = "change and restore opacity with native fallback semantics";
    if (!send_clipboard_shortcut(input_window, VK_OEM_COMMA, false) ||
        !CHECKPOINT(BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_OPACITY_TITLE) ||
        !WAIT(30u, 18u, 0u, background, opaque_mode) ||
        !send_clipboard_shortcut(input_window, VK_OEM_PERIOD, false) ||
        !WAIT(30u, 18u, 0u, background, true) ||
        !CHECKPOINT(BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_CLEARED_TITLE)) {
        return false;
    }
    *stage_out = "present cleared overlays and changed cursor";
    return WAIT(14u, 11u, 0u, background, true) &&
           WAIT(0u, 14u, 0u, background, true) &&
           WAIT(0u, 13u, 16u, cursor, false) &&
           CHECKPOINT(BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_VERIFIED_TITLE);
#undef CAPTURE
#undef WAIT
#undef MOVE
#undef CHECKPOINT
}

static bool run_shared_scenario_geometry(
    HANDLE process, HWND window, HWND input_window,
    unsigned int cell_width, unsigned int cell_height,
    LONG scrollbar_width, const char **stage_out)
{
    static const wchar_t *const names[] = {
        L"resize", L"resize-reset", L"maximize", L"maximize-reset"};
    static const char *const stages[] = {
        "resize", "resize-reset", "maximize", "maximize-reset"};
    RECT initial_outer;
    bool fixed_console = wine_conpty_normalization_is_enabled();
    unsigned int maximized_rows = 0u;
    unsigned int maximized_columns = 0u;
    size_t index;

    if (!GetWindowRect(window, &initial_outer)) {
        return false;
    }
    for (index = 0u; index < 4u; index++) {
        RECT client;
        unsigned int rows = index == 0u ? 28u : 24u;
        unsigned int columns = index == 0u ? 88u : 80u;
        wchar_t title[160];

        *stage_out = stages[index];
        if (index < 2u) {
            if (!SetWindowPos(window, NULL, 0, 0,
                    initial_outer.right - initial_outer.left +
                        (index == 0u ? 8 * (int)cell_width : 0),
                    initial_outer.bottom - initial_outer.top +
                        (index == 0u ? 4 * (int)cell_height : 0),
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
                return false;
            }
        } else {
            (void)ShowWindow(window, index == 2u ? SW_MAXIMIZE : SW_RESTORE);
        }
        if (index == 3u) {
            size_t attempt;

            for (attempt = 0u; attempt < 750u && IsZoomed(window); attempt++) {
                if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
                    return false;
                }
            }
            if (IsZoomed(window)) {
                return false;
            }
        } else if (!wait_for_client_size(process, input_window,
                       (LONG)(columns * cell_width) + scrollbar_width,
                       (LONG)(rows * cell_height), index != 2u) ||
                   !GetClientRect(input_window, &client) ||
                   (IsZoomed(window) != 0) != (index == 2u)) {
            return false;
        }
        if (index == 2u) {
            rows = (unsigned int)client.bottom / cell_height;
            columns = (unsigned int)(client.right - scrollbar_width) / cell_width;
            if (rows <= 24u || columns <= 80u) {
                return false;
            }
            maximized_rows = rows;
            maximized_columns = columns;
        }
        if (fixed_console) {
            rows = 24u;
            columns = 80u;
        }
        if (swprintf(title, sizeof(title) / sizeof(title[0]),
                L"%ls%ls-grid-%u-%u%ls",
                BOXRIG_TEST_WIDE_LITERAL(BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PREFIX),
                names[index], rows, columns,
                fixed_console ? BOXRIG_TEST_WIDE_LITERAL(
                    BOXRIG_TEST_FRONTEND_SAMPLE_FIXED_GRID_SUFFIX) : L"") < 0 ||
            !commit_character(window, index < 2u ? L'r' : L'm') ||
            !(index == 3u
                  ? wait_for_shared_geometry_restore_title(
                        process, window, title)
                  : wait_for_shared_scenario_title(process, window, title))) {
            return false;
        }
        if (index == 2u &&
            (!send_zoom_out(window) ||
             !commit_character(window, L'z') ||
             !wait_for_shared_geometry_zoom_title(
                 process,
                 window,
                 maximized_rows,
                 maximized_columns,
                 fixed_console))) {
            return false;
        }
    }
    if (fixed_console) {
        puts("P2 Win32 shared geometry PTY query: Wine reports the initial console grid; "
             "native allocation and public core/view grids remain verified");
    }
    return true;
}

static bool run_shared_scenario_child_exit(
    const wchar_t *sample_path,
    const wchar_t *helper_path,
    const char **stage_out)
{
    static const wchar_t key_title[] =
        BOXRIG_TEST_WIDE_LITERAL(BOXRIG_TEST_FRONTEND_SAMPLE_KEY_TITLE);
    static const wchar_t linear_title[] =
        BOXRIG_TEST_WIDE_LITERAL(BOXRIG_TEST_FRONTEND_SAMPLE_LINEAR_TITLE);
    static const wchar_t shift_title[] =
        BOXRIG_TEST_WIDE_LITERAL(BOXRIG_TEST_FRONTEND_SAMPLE_SHIFT_TITLE);
    static const wchar_t rectangle_title[] =
        BOXRIG_TEST_WIDE_LITERAL(
            BOXRIG_TEST_FRONTEND_SAMPLE_RECTANGLE_TITLE);
    static const wchar_t zoom_title[] =
        BOXRIG_TEST_WIDE_LITERAL(BOXRIG_TEST_FRONTEND_SAMPLE_ZOOM_TITLE);
    static const wchar_t reset_title[] =
        BOXRIG_TEST_WIDE_LITERAL(BOXRIG_TEST_FRONTEND_SAMPLE_RESET_TITLE);
    HANDLE process = NULL;
    HWND window = NULL;
    HWND input_window;
    HWND scrollbar;
    RECT input_client;
    RECT scrollbar_client;
    RECT initial_client;
    uint64_t rendered_hash;
    unsigned int cell_height = 0u;
    unsigned int cell_width = 0u;
    LONG content_width;
    LONG scrollbar_width;
    bool passed = false;
    const char *overlays = getenv("BOXRIG_TEST_SAMPLE_OVERLAYS");
    bool expect_overlays = overlays != NULL && strcmp(overlays, "1") == 0;
    const char *geometry = getenv("BOXRIG_TEST_SAMPLE_GEOMETRY");
    POINT saved_pointer = {0};
    bool pointer_saved = false;

    const char *colors = getenv("BOXRIG_TEST_SAMPLE_COLORS");
    bool expect_colors = colors != NULL && strcmp(colors, "1") == 0;
    *stage_out = "start controlled child";
    if (expect_colors && !SetEnvironmentVariableW(L"BOXRIG_COLOR_FILTER_DEMO", L"1")) return false;
    if (expect_overlays) {
        pointer_saved = GetCursorPos(&saved_pointer) != 0;
        if (!pointer_saved) goto cleanup;
    }
    if (!start_shared_scenario_sample(
            sample_path,
            helper_path,
            &process,
            &window) ||
        !wait_for_shared_scenario_ready(
            process,
            window,
            &cell_height,
            &cell_width)) {
        goto cleanup;
    }
    (void)SetEnvironmentVariableW(L"BOXRIG_COLOR_FILTER_DEMO", NULL);
    *stage_out = "observe shared scene and geometry";
    input_window = terminal_input_window_for_host(window);
    scrollbar = scrollbar_window_for_host(window);
    if (input_window == NULL || scrollbar == NULL ||
        !GetClientRect(input_window, &input_client) ||
        !GetClientRect(scrollbar, &scrollbar_client) ||
        !GetClientRect(window, &initial_client) ||
        scrollbar_client.right <= scrollbar_client.left ||
        cell_width > (unsigned int)LONG_MAX /
                         BOXRIG_TEST_FRONTEND_SAMPLE_COLUMNS ||
        cell_height > (unsigned int)LONG_MAX /
                          BOXRIG_TEST_FRONTEND_SAMPLE_ROWS) {
        fprintf(stderr, "shared scene input=%p host=%p scrollbar=%p error=%lu\n",
                (void *)input_window, (void *)window, (void *)scrollbar,
                (unsigned long)GetLastError());
        goto cleanup;
    }
    /* Standard components draw the scrollbar inside their own client instead of
     * allocating a separate SCROLLBAR child. Observe the remaining strip. */
    scrollbar_width = scrollbar == input_window
        ? input_client.right -
            (LONG)(BOXRIG_TEST_FRONTEND_SAMPLE_COLUMNS * cell_width)
        : scrollbar_client.right - scrollbar_client.left;
    if (scrollbar_width <= 0) goto cleanup;
    content_width = input_client.right - scrollbar_width;
    fprintf(stderr, "shared scene native client=%ldx%ld scrollbar=%ld "
            "child cell=%ux%u\n", input_client.right, input_client.bottom,
            scrollbar_width, cell_width, cell_height);
    if (content_width !=
            (LONG)(BOXRIG_TEST_FRONTEND_SAMPLE_COLUMNS * cell_width) ||
        input_client.bottom !=
            (LONG)(BOXRIG_TEST_FRONTEND_SAMPLE_ROWS * cell_height) ||
        !wait_for_stable_client_hash(
            process,
            window,
            &rendered_hash,
            true)) {
        goto cleanup;
    }
    *stage_out = "deliver native IME key";
    if (!commit_character(window, L'k') ||
        !wait_for_shared_scenario_title(process, window, key_title)) {
        goto cleanup;
    }
    *stage_out = "color filter off identity corrected cycle";
    if (expect_colors && !run_color_demo(process, input_window, cell_width, cell_height)) goto cleanup;
    *stage_out = "round trip linear selection";
    if (!send_shared_scenario_drag(
            input_window,
            0,
            (int)cell_width / 4,
            (int)cell_height / 2,
            (int)(10u * cell_width + 3u * cell_width / 4u),
            (int)cell_height / 2) ||
        !round_trip_shared_scenario_selection(
            process,
            window,
            input_window,
            L"copy-target",
            linear_title)) {
        goto cleanup;
    }
    *stage_out = "enable mouse reporting after native input barrier";
    if (!commit_character(window, L'm') ||
        !wait_for_shared_scenario_title(
            process,
            window,
            BOXRIG_TEST_WIDE_LITERAL(
                BOXRIG_TEST_FRONTEND_SAMPLE_MOUSE_READY_TITLE))) {
        goto cleanup;
    }
    *stage_out = "deliver unmodified terminal mouse click";
    if (!send_terminal_mouse_button(
            input_window,
            WM_MOUSEMOVE,
            0,
            (int)(20u * cell_width + cell_width / 2u),
            (int)(8u * cell_height + cell_height / 2u)) ||
        !send_terminal_mouse_button(
            input_window,
            WM_LBUTTONDOWN,
            MK_LBUTTON,
            (int)(20u * cell_width + cell_width / 2u),
            (int)(8u * cell_height + cell_height / 2u)) ||
        !send_terminal_mouse_button(
            input_window,
            WM_LBUTTONUP,
            0,
            (int)(20u * cell_width + cell_width / 2u),
            (int)(8u * cell_height + cell_height / 2u)) ||
        !wait_for_shared_scenario_title(
            process,
            window,
            BOXRIG_TEST_WIDE_LITERAL(
                BOXRIG_TEST_FRONTEND_SAMPLE_MOUSE_TITLE))) {
        goto cleanup;
    }
    *stage_out = "round trip Shift selection override";
    if (!send_shared_scenario_drag(
            input_window,
            MK_SHIFT,
            (int)cell_width / 4,
            (int)(2u * cell_height + cell_height / 2u),
            (int)(10u * cell_width + 3u * cell_width / 4u),
            (int)(2u * cell_height + cell_height / 2u)) ||
        !round_trip_shared_scenario_selection(
            process,
            window,
            input_window,
            L"copy-target",
            shift_title)) {
        goto cleanup;
    }
    *stage_out = "round trip rectangular selection override";
    if (!send_shared_scenario_drag(
            input_window,
            MK_CONTROL,
            (int)cell_width / 4,
            (int)cell_height / 2,
            (int)(4u * cell_width + 3u * cell_width / 4u),
            (int)(cell_height + cell_height / 2u)) ||
        !round_trip_shared_scenario_selection(
            process,
            window,
            input_window,
            L"copy-\r\nblock",
            rectangle_title)) {
        goto cleanup;
    }
    *stage_out = "converge zoom grid";
    if (!send_zoom_in(window) ||
        !wait_for_client_size(
            process,
            window,
            initial_client.right,
            initial_client.bottom,
            false) ||
        !commit_character(window, L'g') ||
        !wait_for_shared_scenario_title(process, window, zoom_title)) {
        goto cleanup;
    }
    *stage_out = "converge reset grid";
    if (!send_zoom_reset(window) ||
        !wait_for_client_size(
            process,
            window,
            initial_client.right,
            initial_client.bottom,
            true) ||
        !commit_character(window, L'g') ||
        !wait_for_shared_scenario_title(process, window,
            expect_overlays ? BOXRIG_TEST_WIDE_LITERAL(
                BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_READY_TITLE) : reset_title)) {
        goto cleanup;
    }
    if (expect_overlays &&
        !run_shared_scenario_overlays(process, window, input_window,
            cell_width, cell_height, stage_out)) {
        goto cleanup;
    }
    if (geometry != NULL && strcmp(geometry, "1") == 0 &&
        !run_shared_scenario_geometry(process, window, input_window,
            cell_width, cell_height, scrollbar_width, stage_out)) {
        goto cleanup;
    }
    *stage_out = "follow controlled child exit";
    if (!commit_character(window, L'q') ||
        !wait_for_successful_process(process)) {
        goto cleanup;
    }
    passed = true;

cleanup:
    (void)SetEnvironmentVariableW(L"BOXRIG_COLOR_FILTER_DEMO", NULL);
    if (process != NULL) {
        if (!passed && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process, 1);
            (void)WaitForSingleObject(process, INFINITE);
        }
        CloseHandle(process);
    }
    if (pointer_saved && !SetCursorPos(saved_pointer.x, saved_pointer.y)) {
        passed = false;
    }
    return passed;
}

static bool run_shared_scenario_window_close(
    const wchar_t *sample_path,
    const wchar_t *helper_path,
    bool copy_before_close,
    const char **stage_out)
{
    HANDLE process = NULL;
    HWND window = NULL;
    unsigned int cell_height = 0u;
    unsigned int cell_width = 0u;
    bool passed = false;

    *stage_out = "start close-path child";
    if (!start_shared_scenario_sample(
            sample_path,
            helper_path,
            &process,
            &window) ||
        !wait_for_shared_scenario_ready(
            process,
            window,
            &cell_height,
            &cell_width)) {
        goto cleanup;
    }
    if (copy_before_close) {
        HWND input_window = terminal_input_window_for_host(window);
        *stage_out = "copy and paste before native close";
        if (input_window == NULL || !commit_character(window, L'k') ||
            !wait_for_shared_scenario_title(process, window,
                BOXRIG_TEST_WIDE_LITERAL(BOXRIG_TEST_FRONTEND_SAMPLE_KEY_TITLE)) ||
            !send_shared_scenario_drag(input_window, 0,
                (int)cell_width / 4, (int)cell_height / 2,
                (int)(10u * cell_width + 3u * cell_width / 4u),
                (int)cell_height / 2) ||
            !round_trip_shared_scenario_selection(process, window, input_window,
                L"copy-target",
                BOXRIG_TEST_WIDE_LITERAL(BOXRIG_TEST_FRONTEND_SAMPLE_LINEAR_TITLE))) {
            goto cleanup;
        }
    }
    *stage_out = "close native window";
    if (!PostMessageW(window, WM_CLOSE, 0, 0) ||
        !wait_for_successful_process(process)) {
        goto cleanup;
    }
    if (copy_before_close) {
        *stage_out = "read OS-owned clipboard after source process exit";
        /* Source HWND ownership ends, but transferred CF_UNICODETEXT storage
         * belongs to Windows. Read the current owner without clearing data. */
        if (!clipboard_text_matches(GetClipboardOwner(), L"copy-target")) {
            goto cleanup;
        }
        printf("PASS Win32 sample closes with its copied clipboard payload retained\n");
    }
    passed = true;

cleanup:
    if (process != NULL) {
        if (!passed && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process, 1);
            (void)WaitForSingleObject(process, INFINITE);
        }
        CloseHandle(process);
    }
    return passed;
}

static bool run_pointer_visibility(const wchar_t *sample_path, const wchar_t *helper_path)
{
    HANDLE process = NULL;
    HWND window = NULL;
    unsigned int cell_height = 0;
    unsigned int cell_width = 0;
    bool passed = false;
    CURSORINFO cursor = {0};
    POINT point = {100, 150};

    if (!start_shared_scenario_sample(sample_path, helper_path, &process, &window) ||
        !wait_for_shared_scenario_ready(process, window, &cell_height, &cell_width) ||
        !ClientToScreen(window, &point)) goto cleanup;
    (void)SetForegroundWindow(window);
    int virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (virtual_width <= 1 || virtual_height <= 1) goto cleanup;
    INPUT movement;
    /* SetCursorPos alone does not activate the mouse after touch/pen input.
     * Exercise the mouse input stream before checking pointer visibility. */
    initialize_absolute_mouse_input(&movement, point.x, point.y, MOUSEEVENTF_MOVE,
        GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        virtual_width, virtual_height);
    if (SendInput(1, &movement, sizeof(movement)) != 1) goto cleanup;
    cursor.cbSize = sizeof(cursor);
    for (unsigned int attempt = 0; attempt < 250; attempt++) {
        if (GetCursorInfo(&cursor) && cursor.flags == CURSOR_SHOWING &&
            cursor.hCursor != NULL) {
            passed = true;
            break;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) break;
    }
    if (!passed) {
        wchar_t class_name[80] = L"";
        HWND under_pointer = WindowFromPoint(point);
        (void)GetClassNameW(under_pointer, class_name, 80);
        fwprintf(stderr, L"pointer hidden: flags=%lu cursor=%p window=%p class=%ls\n",
            cursor.flags, (void *)cursor.hCursor, (void *)under_pointer, class_name);
    }
    if (!PostMessageW(window, WM_CLOSE, 0, 0) ||
        !wait_for_successful_process(process)) passed = false;
cleanup:
    if (process != NULL) {
        if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process, 1);
            (void)WaitForSingleObject(process, INFINITE);
        }
        CloseHandle(process);
    }
    return passed;
}

static bool run_shared_frontend_sample_scenario(
    const wchar_t *sample_path,
    const wchar_t *helper_path)
{
    const char *stage = "start";

    if (!run_shared_scenario_child_exit(
            sample_path,
            helper_path,
            &stage) ||
        !run_shared_scenario_window_close(
            sample_path,
            helper_path,
            false,
            &stage) ||
        !run_shared_scenario_window_close(
            sample_path,
            helper_path,
            true,
            &stage)) {
        fprintf(stderr, "Win32 shared sample scenario failed at %s\n", stage);
        return false;
    }
    return true;
}

static char *read_first_frame_file(const wchar_t *path)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size;
    char *bytes = NULL;
    DWORD offset = 0;

    if (file == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 1048576) goto cleanup;
    bytes = malloc((size_t)size.QuadPart + 1);
    if (bytes == NULL) goto cleanup;
    while (offset < (DWORD)size.QuadPart) {
        DWORD count = 0;
        if (!ReadFile(file, bytes + offset, (DWORD)size.QuadPart - offset,
                &count, NULL) || count == 0) {
            free(bytes);
            bytes = NULL;
            goto cleanup;
        }
        offset += count;
    }
    bytes[offset] = '\0';

cleanup:
    (void)CloseHandle(file);
    return bytes;
}

static bool first_frame_field(
    const char *source, const char *field, unsigned long long *value)
{
    char pattern[64];
    const char *start;
    char *end;

    if (snprintf(pattern, sizeof(pattern), "\"%s\":\"", field) < 0)
        return false;
    start = strstr(source, pattern);
    if (start == NULL) return false;
    start += strlen(pattern);
    *value = strtoull(start, &end, 10);
    return end != start && *end == '"';
}

static uint64_t first_frame_qpc_ns(
    unsigned long long ticks, unsigned long long frequency)
{
    return ticks / frequency * UINT64_C(1000000000) +
        ticks % frequency * UINT64_C(1000000000) / frequency;
}

static bool run_first_frame_fence_product(
    const wchar_t *sample_path, const wchar_t *producer_path)
{
    static const wchar_t *prefixes[] = {L"bfo", L"bfc", L"bfp"};
    wchar_t directory[MAX_PATH];
    wchar_t paths[3][MAX_PATH] = {{0}};
    wchar_t base[96];
    wchar_t command_line[4096];
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    HANDLE output = INVALID_HANDLE_VALUE;
    char *product_text = NULL;
    char *child_text = NULL;
    char *producer_text = NULL;
    unsigned long long frequency = 0, product_frequency = 0;
    unsigned long long arm_request = 0, arm_ack = 0;
    unsigned long long write_start = 0, write_end = 0;
    unsigned long long release_request = 0, release_seen = 0;
    DWORD exit_code = 1;
    const char *cursor;
    const char *stage = "temporary files";
    size_t attempt;
    bool passed = false;

    if (GetTempPathW(MAX_PATH, directory) == 0) goto cleanup;
    for (attempt = 0; attempt < 3; attempt++)
        if (GetTempFileNameW(directory, prefixes[attempt], 0,
                paths[attempt]) == 0) goto cleanup;
    /* The producer publishes its log by rename, so its destination must
     * remain absent after the unique temporary name has been reserved. */
    if (!DeleteFileW(paths[2])) goto cleanup;
    stage = "launch";
    if (swprintf(base, sizeof(base) / sizeof(base[0]),
            L"Local\\boxrig-bench-fence-%08lx%08lx"
            L"000000000000000000000000000000000000000000000000",
            GetCurrentProcessId(), GetTickCount()) < 0 ||
        swprintf(command_line,
            sizeof(command_line) / sizeof(command_line[0]),
            L"\"%ls\" -- \"%ls\" \"%ls\" \"%ls\" \"%ls\"",
            sample_path, producer_path, base, paths[1], paths[2]) < 0)
        goto cleanup;
    output = CreateFileW(paths[0], GENERIC_WRITE, FILE_SHARE_READ,
        &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE ||
        !SetEnvironmentVariableW(L"BOXRIG_FIRST_FRAME_FENCE", base))
        goto cleanup;
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = output;
    startup.hStdError = output;
    if (!CreateProcessW(NULL, command_line, NULL, NULL, TRUE, 0,
            NULL, NULL, &startup, &process)) goto cleanup;
    (void)SetEnvironmentVariableW(L"BOXRIG_FIRST_FRAME_FENCE", NULL);
    (void)CloseHandle(process.hThread);
    process.hThread = NULL;
    stage = "shutdown";
    if (WaitForSingleObject(process.hProcess, 15000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &exit_code) || exit_code != 0)
        goto cleanup;
    (void)CloseHandle(output);
    output = INVALID_HANDLE_VALUE;
    stage = "source files";
    product_text = read_first_frame_file(paths[0]);
    child_text = read_first_frame_file(paths[1]);
    producer_text = read_first_frame_file(paths[2]);
    if (product_text == NULL || child_text == NULL || producer_text == NULL ||
        strstr(product_text, "boxrig_term: first-frame-fence-status=ok") == NULL)
        goto cleanup;
    stage = "fence timestamps";
    cursor = strstr(product_text,
        "boxrig_term: first-frame-fence qpc-frequency=");
    if (cursor == NULL ||
        sscanf(cursor,
            "boxrig_term: first-frame-fence qpc-frequency=%llu arm-ack-qpc=%llu release-seen-qpc=%llu",
            &product_frequency, &arm_ack, &release_seen) != 3 ||
        !first_frame_field(producer_text, "frequency", &frequency) ||
        !first_frame_field(producer_text, "armRequestedNs", &arm_request) ||
        !first_frame_field(producer_text, "releaseRequestedNs", &release_request))
        goto cleanup;
    stage = "first write";
    cursor = strstr(child_text, "\"writes\":[");
    if (cursor == NULL ||
        !first_frame_field(cursor, "startNs", &write_start) ||
        !first_frame_field(cursor, "endNs", &write_end) ||
        frequency == 0 || frequency != product_frequency)
        goto cleanup;
    stage = "handshake order";
    passed = arm_request < first_frame_qpc_ns(arm_ack, frequency) &&
        first_frame_qpc_ns(arm_ack, frequency) < write_start &&
        write_start < write_end && write_end <= release_request &&
        release_request < first_frame_qpc_ns(release_seen, frequency);

cleanup:
    (void)SetEnvironmentVariableW(L"BOXRIG_FIRST_FRAME_FENCE", NULL);
    if (process.hProcess != NULL) {
        if (!passed) {
            (void)GetExitCodeProcess(process.hProcess, &exit_code);
            if (exit_code == STILL_ACTIVE)
                (void)TerminateProcess(process.hProcess, 1);
        }
        (void)WaitForSingleObject(process.hProcess, 10000);
        (void)CloseHandle(process.hProcess);
    }
    if (process.hThread != NULL) (void)CloseHandle(process.hThread);
    if (output != INVALID_HANDLE_VALUE) (void)CloseHandle(output);
    if (!passed) {
        char *diagnostic = paths[0][0] == L'\0'
            ? NULL : read_first_frame_file(paths[0]);
        fprintf(stderr,
            "Win32 product first-frame fence failed at %s (exit=%lu)\n",
            stage, exit_code);
        if (diagnostic != NULL) {
            fprintf(stderr, "product output: %.2000s\n", diagnostic);
            free(diagnostic);
        }
        diagnostic = paths[1][0] == L'\0'
            ? NULL : read_first_frame_file(paths[1]);
        if (diagnostic != NULL) {
            fprintf(stderr, "producer output: %.2000s\n", diagnostic);
            free(diagnostic);
        }
    }
    free(product_text);
    free(child_text);
    free(producer_text);
    for (attempt = 0; attempt < 3; attempt++)
        if (paths[attempt][0] != L'\0') (void)DeleteFileW(paths[attempt]);
    return passed;
}

int wmain(int argument_count, wchar_t **arguments)
{
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    wchar_t *command_line = NULL;
    window_search search;
    DWORD wait_result;
    DWORD exit_code = 1;
    RECT initial_client;
    SCROLLINFO scroll_information;
    int maximum_scroll_position;
    uint64_t live_hash = 0;
    uint64_t page_hash = 0;
    uint64_t oldest_hash = 0;
    uint64_t dragged_hash = 0;
    uint64_t normal_hash = 0;
    uint64_t composition_hash = 0;
    DWORD_PTR message_result;
    bool wine_runtime = wine_conpty_normalization_is_enabled();
    bool scroll_information_available;
    bool passed = false;

    /* Screen capture and absolute mouse input use physical desktop pixels. */
    if (SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == NULL)
        return 1;

    if (argument_count == 4 && wcscmp(arguments[1], L"--pointer") == 0) {
        if (!run_pointer_visibility(arguments[2], arguments[3])) return 1;
        puts("PASS Win32 sample pointer remains visible over the terminal");
        return 0;
    }

    /* Select the product's explicit opaque policy for Wine UI regression.
     * Native alpha remains covered by the independent probe and physical P2. */
    if (wine_runtime &&
        !SetEnvironmentVariableW(L"BOXRIG_TRANSPARENCY_MODE", L"opaque")) return 1;
    if (argument_count == 4 &&
        wcscmp(arguments[1], L"--notifications") == 0) {
        if (!run_notification_policy(arguments[2], arguments[3])) return 1;
        puts("PASS Win32 sample notification opt-in and Shell lifecycle");
        return 0;
    }
    if (argument_count == 4 &&
        wcscmp(arguments[1], L"--shared-scenario") == 0) {
        if (!run_shared_frontend_sample_scenario(
                arguments[2],
                arguments[3])) {
            return 1;
        }
        printf(
            "PASS Win32 sample follows shared PTY scene input selection geometry and shutdown\n");
        return 0;
    }
    if (argument_count == 4 &&
        wcscmp(arguments[1], L"--first-frame-fence") == 0) {
        if (!run_first_frame_fence_product(arguments[2], arguments[3]))
            return 1;
        puts("PASS Win32 product and producer first-frame fence handshake");
        return 0;
    }
    if (argument_count == 3 &&
        wcscmp(arguments[1], L"--more-ctrl-c") == 0) {
        if (!run_more_ctrl_c_case(arguments[2], wine_runtime)) {
            fprintf(
                stderr,
                "boxrig_term did not complete Win32 MORE Ctrl-C case\n");
            return 1;
        }
        printf("PASS Win32 sample MORE Ctrl-C case\n");
        return 0;
    }
    if (argument_count == 4 &&
        wcscmp(arguments[1], L"--emitter-conpty") == 0) {
        if (!run_emitter_conpty_cases(arguments[2], arguments[3])) {
            fprintf(
                stderr,
                "boxrig_term did not complete Win32 ConPTY cases\n");
            return 1;
        }
        printf("PASS Win32 sample emitter ConPTY cases\n");
        return 0;
    }
    if (argument_count == 4 && wcscmp(arguments[1], L"--fallback-opacity") == 0) {
        if (!run_fallback_opacity_policy(arguments[2], arguments[3])) return 1;
        puts("PASS Win32 unavailable-alpha fallback and disabled policies");
        return 0;
    }
    if (argument_count == 4 &&
        wcscmp(arguments[1], L"--background-opacity") == 0) {
        if (!run_background_opacity_policy(arguments[2], arguments[3])) {
            fprintf(stderr, "Win32 sample background opacity policy failed\n");
            return 1;
        }
        printf("PASS Win32 sample background opacity policy\n");
        return 0;
    }
    if (argument_count == 4 &&
        wcscmp(arguments[1], L"--ime-movement") == 0) {
        if (!run_ime_movement_cases(arguments[2], arguments[3])) {
            fprintf(stderr, "Win32 sample IME movement policy failed\n");
            return 1;
        }
        printf("PASS Win32 sample IME movement synchronization\n");
        return 0;
    }
    if (argument_count != 5) {
        fwprintf(
            stderr,
            L"usage: %ls [--more-ctrl-c SAMPLE | "
            L"--emitter-conpty SAMPLE EMITTER | "
            L"--background-opacity SAMPLE PTY_HELPER | "
            L"--ime-movement SAMPLE PTY_HELPER | "
            L"--pointer SAMPLE PTY_HELPER | "
            L"--shared-scenario SAMPLE PTY_HELPER | "
            L"SAMPLE NOTIFICATION_SAMPLE PTY_HELPER EMITTER_OR_DASH]\n",
            arguments[0]);
        return 2;
    }
    (void)SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", NULL);
    if (wcscmp(arguments[4], L"-") != 0 &&
        !run_background_opacity_policy(arguments[1], arguments[3])) {
        fprintf(stderr, "Win32 sample background opacity policy failed\n");
        return 1;
    }
    command_line = make_command_line(arguments[1], arguments[3]);
    if (command_line == NULL) {
        fprintf(stderr, "could not build sample command line\n");
        return 1;
    }
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));
    if (!CreateProcessW(
            NULL,
            command_line,
            NULL,
            NULL,
            FALSE,
            0,
            NULL,
            NULL,
            &startup,
            &process)) {
        fprintf(stderr, "could not start sample: %lu\n", GetLastError());
        free(command_line);
        return 1;
    }
    free(command_line);
    CloseHandle(process.hThread);

    wait_result = WaitForInputIdle(process.hProcess, 10000);
    if (wait_result != 0) {
        fprintf(stderr, "sample did not reach an input-idle state: %lu\n", wait_result);
        goto cleanup;
    }
    search.process_id = process.dwProcessId;
    search.window = NULL;
    (void)EnumWindows(find_process_window, (LPARAM)&search);
    if (search.window == NULL) {
        fprintf(stderr, "sample did not create a visible window\n");
        goto cleanup;
    }
    if (terminal_input_window_for_host(search.window) == NULL) {
        fprintf(stderr, "sample did not expose a terminal input window\n");
        goto cleanup;
    }
    if (!wait_for_window_title(
            process.hProcess,
            search.window,
            L"helper-ready") ||
        scrollbar_width_for_host(search.window) <= 0) {
        fprintf(
            stderr,
            "sample did not expose a vertical scrollbar surface\n");
        goto cleanup;
    }
    if (!commit_character(search.window, L'g') ||
        !wait_for_window_title(
            process.hProcess,
            search.window,
            L"grid-24-80")) {
        wchar_t actual_title[64];

        (void)GetWindowTextW(
            search.window,
            actual_title,
            (int)(sizeof(actual_title) / sizeof(actual_title[0])));
        fwprintf(
            stderr,
            L"sample changed its initial terminal grid: %ls\n",
            actual_title);
        goto cleanup;
    }
    if (!run_interactive_sizing_contract(
            process.hProcess,
            search.window,
            wine_runtime)) {
        fprintf(stderr, "sample interactive sizing contract failed\n");
        goto cleanup;
    }
    if (!commit_character(search.window, L'h') ||
        !wait_for_window_title(
            process.hProcess,
            search.window,
            L"history-ready")) {
        fprintf(stderr, "sample did not generate terminal history\n");
        goto cleanup;
    }
    if (!wait_for_stable_client_hash(
            process.hProcess,
            search.window,
            &live_hash,
            true)) {
        fprintf(stderr, "sample did not render generated history\n");
        goto cleanup;
    }
    scroll_information_available = read_vertical_scroll_info(
        search.window,
        &scroll_information);
    if (!scroll_information_available && !wine_runtime &&
        scrollbar_window_for_host(search.window) !=
            terminal_input_window_for_host(search.window)) {
        fprintf(stderr, "sample did not expose generated history range\n");
        goto cleanup;
    }
    maximum_scroll_position = scroll_information_available
                                  ? vertical_scroll_maximum_position(
                                        &scroll_information)
                                  : 72;
    if ((scroll_information_available &&
         (scroll_information.nMin != 0 ||
          scroll_information.nMax != 95 ||
          scroll_information.nPage != 24 ||
          scroll_information.nPos != maximum_scroll_position)) ||
        maximum_scroll_position != 72) {
        fprintf(
            stderr,
            "sample exposed an invalid live scrollbar range "
            "(available=%d min=%d max=%d page=%u pos=%d "
            "maximum-position=%d)\n",
            scroll_information_available,
            scroll_information.nMin,
            scroll_information.nMax,
            scroll_information.nPage,
            scroll_information.nPos,
            maximum_scroll_position);
        goto cleanup;
    }
    if (!send_vertical_scroll(search.window, SB_PAGEUP) ||
        !wait_for_stable_client_hash(
            process.hProcess,
            search.window,
            &page_hash,
            true) ||
        page_hash == live_hash ||
        !vertical_scroll_position_matches(
            search.window,
            maximum_scroll_position - 24,
            scroll_information_available)) {
        fprintf(stderr, "sample did not page upward through history\n");
        goto cleanup;
    }
    if (!send_vertical_scroll(search.window, SB_TOP) ||
        !wait_for_stable_client_hash(
            process.hProcess,
            search.window,
            &oldest_hash,
            true) ||
        oldest_hash == page_hash || oldest_hash == live_hash ||
        !vertical_scroll_position_matches(
            search.window,
            0,
            scroll_information_available)) {
        fprintf(stderr, "sample did not reach oldest history\n");
        goto cleanup;
    }
    if (!send_vertical_scroll(search.window, SB_BOTTOM) ||
        !wait_for_stable_client_hash(
            process.hProcess,
            search.window,
            &normal_hash,
            true) ||
        normal_hash != live_hash ||
        !vertical_scroll_position_matches(
            search.window,
            maximum_scroll_position,
            scroll_information_available)) {
        fprintf(stderr, "sample did not restore the live viewport\n");
        goto cleanup;
    }
    if (!drag_vertical_scroll_thumb_to_middle(
            search.window,
            wine_runtime) ||
        !wait_for_stable_client_hash(
            process.hProcess,
            search.window,
            &dragged_hash,
            true) ||
        dragged_hash == live_hash || dragged_hash == oldest_hash ||
        (scroll_information_available &&
         (!read_vertical_scroll_info(
              search.window,
              &scroll_information) ||
          scroll_information.nPos <= 0 ||
          scroll_information.nPos >= maximum_scroll_position)) ||
        !send_vertical_scroll(search.window, SB_BOTTOM) ||
        !wait_for_stable_client_hash(
            process.hProcess,
            search.window,
            &normal_hash,
            true) ||
        normal_hash != live_hash) {
        fprintf(
            stderr,
            "sample did not track and restore a dragged thumb "
            "(live=%llu oldest=%llu dragged=%llu normal=%llu pos=%d)\n",
            (unsigned long long)live_hash,
            (unsigned long long)oldest_hash,
            (unsigned long long)dragged_hash,
            (unsigned long long)normal_hash,
            scroll_information.nPos);
        goto cleanup;
    }
    if (!GetClientRect(search.window, &initial_client) ||
        !post_key(search.window, VK_CONTROL, false) ||
        !post_key(search.window, VK_ADD, false) ||
        !wait_for_client_size(
            process.hProcess,
            search.window,
            initial_client.right,
            initial_client.bottom,
            false) ||
        !post_key(search.window, VK_ADD, true) ||
        !post_key(search.window, VK_CONTROL, true) ||
        !post_key(search.window, VK_CONTROL, false) ||
        !post_key(search.window, VK_NUMPAD0, false) ||
        !wait_for_client_size(
            process.hProcess,
            search.window,
            initial_client.right,
            initial_client.bottom,
            true) ||
        !post_key(search.window, VK_NUMPAD0, true) ||
        !post_key(search.window, VK_CONTROL, true) ||
        !commit_character(search.window, L'g') ||
        !wait_for_window_title(
            process.hProcess,
            search.window,
            L"grid-24-80")) {
        fprintf(stderr, "font zoom shortcuts did not resize and reset window\n");
        goto cleanup;
    }
    if (!run_host_input_policy(process.hProcess, search.window)) {
        goto cleanup;
    }
    if (!wait_for_stable_client_hash(
            process.hProcess,
            search.window,
            &normal_hash,
            false) ||
        !SendMessageTimeoutW(
            search.window,
            WM_IME_STARTCOMPOSITION,
            0,
            0,
            SMTO_ABORTIFHUNG,
            5000,
            &message_result) ||
        !wait_for_stable_client_hash(
            process.hProcess,
            search.window,
            &composition_hash,
            false) ||
        composition_hash == normal_hash) {
        fprintf(stderr, "IME preedit did not replace the terminal cursor\n");
        goto cleanup;
    }
    if (!SendMessageTimeoutW(
            search.window,
            WM_IME_CHAR,
            (WPARAM)L'q',
            1,
            SMTO_ABORTIFHUNG,
            5000,
            &message_result)) {
        fprintf(stderr, "could not commit IME terminal input\n");
        goto cleanup;
    }
    if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &exit_code) || exit_code != 0) {
        fprintf(stderr, "sample did not follow successful child exit\n");
        goto cleanup;
    }
    passed = true;

cleanup:
    if (!passed && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
        (void)TerminateProcess(process.hProcess, 1);
        (void)WaitForSingleObject(process.hProcess, INFINITE);
    }
    CloseHandle(process.hProcess);
    if (!passed) {
        return 1;
    }
    if (!run_modal_resize_case(
            arguments[1],
            arguments[3],
            wine_runtime)) {
        return 1;
    }
    if (!run_notification_policy(arguments[2], arguments[3])) {
        fprintf(stderr, "notification opt-in policy did not reach fake Shell\n");
        return 1;
    }
    if (wcscmp(arguments[4], L"-") != 0 &&
        (!run_more_ctrl_c_case(arguments[1], wine_runtime) ||
         !run_emitter_sample_cases(
             arguments[1],
             arguments[2],
             arguments[4]))) {
        fprintf(
            stderr,
            "boxrig_term did not complete Win32 sample cases\n");
        return 1;
    }
    printf(
        "PASS Win32 sample scrollbar MORE Ctrl-C emitter notifications zoom IME and child exit\n");
    return 0;
}
