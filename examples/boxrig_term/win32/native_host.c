/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "application_internal.h"
#include "../common/background_demo.h"
#include <commctrl.h>
#include "../../../src/win32/component_private.h"
#ifdef BOXRIG_TERM_ENABLE_D3D11
#include "boxrig/win32/d3d11_component.h"
#endif
#ifdef BOXRIG_TERM_ENABLE_OPENGL
#include "boxrig/win32/opengl_component.h"
#endif

#include "boxrig/ui/shortcut.h"

#include <stdio.h>
#include <windows.h>

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static boxrig_ui_transparency_config component_transparency_config(
    const boxrig_ui_transparency_state *state)
{
    boxrig_ui_transparency_config config = state->config;

    config.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    if (state->effective != BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA) {
        config.request = BOXRIG_UI_TRANSPARENCY_OPAQUE;
        config.background_opacity = 255;
    }
    return config;
}

static boxrig_ui_status set_scrollbar_policy(void *context, boxrig_ui_scrollbar_visibility policy)
{
    return boxrig_win32_terminal_component_set_scrollbar_visibility(context, policy);
}

static int scrollbar_pixel_width(const boxrig_term_win32_application *application)
{
    boxrig_ui_scrollbar_visibility_state state;
    if (application->component == NULL ||
        boxrig_win32_terminal_component_get_scrollbar_visibility(application->component, &state) != BOXRIG_UI_OK ||
        state.reserved_width > INT_MAX) return -1;
    return (int)state.reserved_width;
}

static bool fixed_to_long(boxrig_ui_fixed value, LONG *value_out)
{
    boxrig_ui_fixed pixels;

    if (value <= 0 || value % BOXRIG_UI_FIXED_ONE != 0) {
        return false;
    }
    pixels = value / BOXRIG_UI_FIXED_ONE;
    if (pixels > LONG_MAX) {
        return false;
    }
    *value_out = (LONG)pixels;
    return true;
}

static int adjust_window_rectangle_for_client(
    const boxrig_term_win32_application *application,
    LONG client_width,
    LONG client_height,
    RECT *rectangle_out)
{
    HWND window = application->window;
    DWORD style;
    DWORD extended_style;
    UINT dpi;

    if (window == NULL || client_width <= 0 || client_height <= 0 ||
        rectangle_out == NULL) {
        return -1;
    }
    style = (DWORD)GetWindowLongPtrW(window, GWL_STYLE);
    extended_style = (DWORD)GetWindowLongPtrW(window, GWL_EXSTYLE);
    dpi = boxrig_win32_window_dpi(&application->dpi, window);
    if (dpi == 0) {
        return -1;
    }
    rectangle_out->left = 0;
    rectangle_out->top = 0;
    rectangle_out->right = client_width;
    rectangle_out->bottom = client_height;
    return boxrig_win32_adjust_window_rectangle(
               &application->dpi,
               rectangle_out,
               style,
               GetMenu(window) != NULL,
               extended_style,
               dpi)
               ? 0
               : -1;
}

static int interactive_sizing_axes(
    WPARAM edge,
    bool *quantize_width_out,
    bool *quantize_height_out,
    bool *move_left_out,
    bool *move_top_out)
{
    *quantize_width_out = false;
    *quantize_height_out = false;
    *move_left_out = false;
    *move_top_out = false;
    switch (edge) {
    case WMSZ_LEFT:
        *quantize_width_out = true;
        *move_left_out = true;
        break;
    case WMSZ_RIGHT:
        *quantize_width_out = true;
        break;
    case WMSZ_TOP:
        *quantize_height_out = true;
        *move_top_out = true;
        break;
    case WMSZ_BOTTOM:
        *quantize_height_out = true;
        break;
    case WMSZ_TOPLEFT:
        *quantize_width_out = true;
        *quantize_height_out = true;
        *move_left_out = true;
        *move_top_out = true;
        break;
    case WMSZ_TOPRIGHT:
        *quantize_width_out = true;
        *quantize_height_out = true;
        *move_top_out = true;
        break;
    case WMSZ_BOTTOMLEFT:
        *quantize_width_out = true;
        *quantize_height_out = true;
        *move_left_out = true;
        break;
    case WMSZ_BOTTOMRIGHT:
        *quantize_width_out = true;
        *quantize_height_out = true;
        break;
    default:
        return 0;
    }
    return 1;
}

/* WM_SIZING owns the proposed screen rectangle. Compute into a copy so an
 * invalid native candidate can never expose a partially quantized edge. */
static int constrain_interactive_sizing_rectangle(
    boxrig_term_win32_application *application,
    WPARAM edge,
    RECT *rectangle)
{
    boxrig_font_effective_metrics metrics;
    RECT unit_outer;
    RECT target_outer;
    RECT adjusted;
    int64_t outer_width;
    int64_t outer_height;
    int64_t frame_width;
    int64_t frame_height;
    int64_t candidate_client_width;
    int64_t candidate_client_height;
    int64_t candidate_content_width;
    int64_t columns;
    int64_t rows;
    int64_t target_client_width;
    int64_t target_client_height;
    int64_t target_outer_width;
    int64_t target_outer_height;
    int64_t moving_coordinate;
    int scroll_width;
    bool quantize_width;
    bool quantize_height;
    bool move_left;
    bool move_top;
    int axes_result;

    if (application == NULL || application->window == NULL ||
        application->component == NULL || rectangle == NULL) {
        return -1;
    }
    axes_result = interactive_sizing_axes(
        edge,
        &quantize_width,
        &quantize_height,
        &move_left,
        &move_top);
    if (axes_result != 1) {
        return axes_result;
    }
    if (boxrig_win32_terminal_component_get_effective_metrics(
            application->component,
            &metrics) != BOXRIG_FONT_CATALOG_OK ||
        metrics.cell_width == 0 || metrics.cell_width > LONG_MAX ||
        metrics.cell_height == 0 || metrics.cell_height > LONG_MAX) {
        return -1;
    }
    scroll_width = scrollbar_pixel_width(application);
    if (scroll_width < 0 ||
        adjust_window_rectangle_for_client(
            application,
            1,
            1,
            &unit_outer) == -1) {
        return -1;
    }
    outer_width = (int64_t)rectangle->right - rectangle->left;
    outer_height = (int64_t)rectangle->bottom - rectangle->top;
    frame_width =
        (int64_t)unit_outer.right - unit_outer.left - 1;
    frame_height =
        (int64_t)unit_outer.bottom - unit_outer.top - 1;
    candidate_client_width = outer_width - frame_width;
    candidate_client_height = outer_height - frame_height;
    candidate_content_width = candidate_client_width - scroll_width;
    if (outer_width <= 0 || outer_height <= 0 || frame_width < 0 ||
        frame_height < 0 || candidate_client_width <= scroll_width ||
        candidate_client_height <= 0 || candidate_content_width <= 0) {
        return 0;
    }
    columns = candidate_content_width / metrics.cell_width;
    rows = candidate_client_height / metrics.cell_height;
    if (columns < 1) {
        columns = 1;
    }
    if (rows < 1) {
        rows = 1;
    }
    target_client_width =
        columns * metrics.cell_width + scroll_width;
    target_client_height = rows * metrics.cell_height;
    if (target_client_width <= 0 || target_client_width > LONG_MAX ||
        target_client_height <= 0 || target_client_height > LONG_MAX ||
        adjust_window_rectangle_for_client(
            application,
            (LONG)target_client_width,
            (LONG)target_client_height,
            &target_outer) == -1) {
        return 0;
    }
    target_outer_width =
        (int64_t)target_outer.right - target_outer.left;
    target_outer_height =
        (int64_t)target_outer.bottom - target_outer.top;
    if (target_outer_width <= 0 || target_outer_width > LONG_MAX ||
        target_outer_height <= 0 || target_outer_height > LONG_MAX) {
        return 0;
    }
    adjusted = *rectangle;
    if (quantize_width) {
        moving_coordinate = move_left
                                ? (int64_t)adjusted.right -
                                      target_outer_width
                                : (int64_t)adjusted.left +
                                      target_outer_width;
        if (moving_coordinate < LONG_MIN || moving_coordinate > LONG_MAX) {
            return 0;
        }
        if (move_left) {
            adjusted.left = (LONG)moving_coordinate;
        } else {
            adjusted.right = (LONG)moving_coordinate;
        }
    }
    if (quantize_height) {
        moving_coordinate = move_top
                                ? (int64_t)adjusted.bottom -
                                      target_outer_height
                                : (int64_t)adjusted.top +
                                      target_outer_height;
        if (moving_coordinate < LONG_MIN || moving_coordinate > LONG_MAX) {
            return 0;
        }
        if (move_top) {
            adjusted.top = (LONG)moving_coordinate;
        } else {
            adjusted.bottom = (LONG)moving_coordinate;
        }
    }
    *rectangle = adjusted;
    return 1;
}

static int apply_hidden_window_requirements(
    HWND window,
    const boxrig_win32_window_requirements *requirements)
{
    LONG_PTR previous;
    DWORD style = requirements->style & ~(DWORD)WS_VSCROLL;

    SetLastError(ERROR_SUCCESS);
    previous = SetWindowLongPtrW(window, GWL_STYLE, (LONG_PTR)style);
    if (previous == 0 && GetLastError() != ERROR_SUCCESS) {
        return -1;
    }
    SetLastError(ERROR_SUCCESS);
    previous = SetWindowLongPtrW(
        window,
        GWL_EXSTYLE,
        (LONG_PTR)requirements->extended_style);
    if (previous == 0 && GetLastError() != ERROR_SUCCESS) {
        return -1;
    }
    return SetWindowPos(
               window,
               NULL,
               0,
               0,
               0,
               0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                   SWP_FRAMECHANGED)
               ? 0
               : -1;
}

static int register_host_class(HINSTANCE instance)
{
    static const wchar_t class_name[] = L"boxrig_win32_sample";
    WNDCLASSEXW window_class;

    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    /* Borrowed-input presentation uses this class instead of the child class. */
    window_class.style = CS_DBLCLKS;
    window_class.lpfnWndProc = boxrig_term_win32_window_procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(NULL, IDC_IBEAM);
    window_class.lpszClassName = class_name;
    if (window_class.hCursor == NULL) {
        return -1;
    }
    if (RegisterClassExW(&window_class) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        return 0;
    }
    return -1;
}

static int create_transparency_host(
    boxrig_term_win32_application *application,
    const boxrig_ui_transparency_config *config,
    boxrig_win32_window_requirements *requirements_out,
    boxrig_ui_transparency_state *state_out)
{
    static const wchar_t class_name[] = L"boxrig_win32_sample";
    boxrig_ui_status bind_status;

    if (boxrig_win32_transparency_context_create(
            application->instance,
            config,
            application->renderer == BOXRIG_TERM_WIN32_RENDERER_SOFTWARE,
            &application->transparency) != BOXRIG_UI_OK ||
        boxrig_win32_transparency_context_get_window_requirements(
            application->transparency,
            requirements_out) != BOXRIG_UI_OK ||
        register_host_class(application->instance) == -1) {
        return -1;
    }
    if (application->renderer == BOXRIG_TERM_WIN32_RENDERER_SOFTWARE)
        requirements_out->style |= WS_CLIPCHILDREN;
    application->window = CreateWindowExW(
        requirements_out->extended_style,
        class_name,
        L"boxrig",
        requirements_out->style & ~(DWORD)WS_VSCROLL,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        640,
        480,
        NULL,
        NULL,
        application->instance,
        application);
    if (application->window == NULL) {
        return -1;
    }
    bind_status = boxrig_win32_transparency_context_bind_host(
        application->transparency,
        application->window);
    if (bind_status != BOXRIG_UI_OK ||
        boxrig_win32_transparency_context_get_state(
            application->transparency,
            state_out) != BOXRIG_UI_OK) {
        return -1;
    }
    if (requirements_out->layered_visual &&
        state_out->effective !=
            BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA) {
        if (config->request == BOXRIG_UI_TRANSPARENCY_REQUIRED) {
            return -1;
        }
        boxrig_win32_transparency_context_destroy(
            application->transparency);
        application->transparency = NULL;
        if (boxrig_win32_transparency_context_create(
                application->instance,
                config,
                false,
                &application->transparency) != BOXRIG_UI_OK ||
            boxrig_win32_transparency_context_get_window_requirements(
                application->transparency,
                requirements_out) != BOXRIG_UI_OK ||
            apply_hidden_window_requirements(
                application->window,
                requirements_out) == -1 ||
            boxrig_win32_transparency_context_bind_host(
                application->transparency,
                application->window) != BOXRIG_UI_OK ||
            boxrig_win32_transparency_context_get_state(
                application->transparency,
                state_out) != BOXRIG_UI_OK) {
            return -1;
        }
    }
    application->transparency_bound = true;
    return 0;
}

static int resize_component(
    boxrig_term_win32_application *application)
{
    RECT content;
    RECT target_client;
    boxrig_ui_component_allocation allocation;
    boxrig_ui_layout_result layout;
    unsigned int view_width;
    int local_scrollbar_width = 0;

    if (application == NULL || application->window == NULL ||
        application->component == NULL ||
        !application->transparency_bound ||
        !GetClientRect(application->window, &target_client) ||
        target_client.right <= target_client.left ||
        target_client.bottom <= target_client.top) {
        return -1;
    }
    boxrig_ui_scrollbar_visibility_state scrollbar;
    if (boxrig_win32_terminal_component_get_scrollbar_visibility(application->component, &scrollbar) != BOXRIG_UI_OK)
        return -1;
    if (scrollbar.pending) {
        application->pending_allocation = true;
        return 0;
    }
    application->pending_allocation = false;
    content = target_client;
    if (application->external_presentation) {
        if (boxrig_win32_terminal_component_set_target_extent(
             application->component,
             (unsigned int)(target_client.right - target_client.left),
             (unsigned int)(target_client.bottom - target_client.top)) !=
            BOXRIG_UI_OK) {
            return -1;
        }
    }
    memset(&allocation, 0, sizeof(allocation));
    allocation.x = content.left;
    allocation.y = content.top;
    allocation.width = (unsigned int)(content.right - content.left);
    allocation.height = (unsigned int)(content.bottom - content.top);
    allocation.window_state = IsZoomed(application->window)
                                  ? BOXRIG_UI_WINDOW_MAXIMIZED
                                  : BOXRIG_UI_WINDOW_NORMAL;
    view_width = allocation.width;
    local_scrollbar_width = scrollbar_pixel_width(application);
    if (local_scrollbar_width < 0 || view_width <= (unsigned int)local_scrollbar_width) return -1;
    view_width -= (unsigned int)local_scrollbar_width;
    if (application->pending_size) {
        boxrig_ui_fixed fixed_width =
            (boxrig_ui_fixed)view_width * BOXRIG_UI_FIXED_ONE;
        boxrig_ui_fixed fixed_height =
            (boxrig_ui_fixed)allocation.height * BOXRIG_UI_FIXED_ONE;
        boxrig_ui_size_outcome outcome =
            fixed_width == application->pending_content_width &&
                    fixed_height == application->pending_content_height
                ? BOXRIG_UI_SIZE_ACCEPTED
                : BOXRIG_UI_SIZE_CONSTRAINED;
        bool applied = false;

        if (boxrig_win32_terminal_component_complete_size_command(
                application->component,
                application->pending_size_revision,
                outcome,
                fixed_width,
                fixed_height,
                &applied,
                &layout) != BOXRIG_UI_OK ||
            !applied) {
            return -1;
        }
        application->pending_size = false;
    }
    if (boxrig_win32_terminal_component_allocate(
            application->component,
            &allocation,
            &layout) != BOXRIG_UI_OK ||
        boxrig_win32_transparency_context_synchronize_layout(
            application->transparency,
            &content) != BOXRIG_UI_OK) {
        return -1;
    }
    if (application->external_presentation) {
        HWND visual = boxrig_win32_transparency_context_visual_window(
            application->transparency);

        if (visual != NULL &&
            !SetWindowPos(
                visual,
                HWND_TOP,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
            return -1;
        }
    }
    return 0;
}

int boxrig_term_win32_native_host_resize_component(boxrig_term_win32_application *application)
{
    int result;
    if (application == NULL || boxrig_term_win32_begin_presentation_work(application) == -1) return -1;
    result = resize_component(application);
    if (boxrig_term_win32_end_presentation_work(application) == -1) return -1;
    return result;
}

int boxrig_term_win32_native_host_process_layout_action(
    boxrig_term_win32_application *application)
{
    boxrig_ui_layout_action action;
    boxrig_ui_size_command_state size_state;
    RECT rectangle;
    LONG content_width;
    LONG content_height;
    int scroll_width;

    if (application == NULL || application->window == NULL ||
        application->component == NULL) {
        return -1;
    }
    boxrig_ui_scrollbar_visibility_state scrollbar;
    if (boxrig_win32_terminal_component_get_scrollbar_visibility(application->component, &scrollbar) != BOXRIG_UI_OK)
        return -1;
    if (application->pending_allocation && !scrollbar.pending &&
        resize_component(application) == -1) return -1;
    if (application->interactive_sizing) {
        if (boxrig_win32_terminal_component_get_size_command_state(
                application->component, &size_state) != BOXRIG_UI_OK) {
            return -1;
        }
        if (size_state != BOXRIG_UI_SIZE_COMMAND_IDLE) {
            return 0;
        }
    }
    if (!boxrig_win32_terminal_component_take_layout_action(
            application->component, &action)) {
        return 0;
    }
    if (action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
        bool succeeded = false;

        if (application->session != NULL) {
            SetLastError(ERROR_SUCCESS);
            if (boxrig_win32_pty_session_resize(
                    application->session,
                    action.value.grid_resize.rows,
                    action.value.grid_resize.columns) == BOXRIG_SESSION_OK) {
                succeeded = true;
            } else if (GetLastError() != ERROR_CALL_NOT_IMPLEMENTED) {
                (void)boxrig_win32_terminal_component_complete_grid_resize(
                    application->component, action.revision, false);
                return -1;
            }
        }
        if (!succeeded) {
            succeeded = boxrig_terminal_resize(
                            application->terminal,
                            action.value.grid_resize.rows,
                            action.value.grid_resize.columns) ==
                        BOXRIG_STATUS_OK;
        }
        if (!succeeded) {
            (void)boxrig_win32_terminal_component_complete_grid_resize(
                application->component, action.revision, false);
            return -1;
        }
        return boxrig_win32_terminal_component_complete_grid_resize(
                   application->component,
                   action.revision,
                   true) == BOXRIG_UI_OK
                   ? 1
                   : -1;
    }
    if (application->pending_size &&
        application->pending_size_revision == action.revision) {
        return 0;
    }
    scroll_width = scrollbar_pixel_width(application);
    if (!fixed_to_long(action.value.content_size.width, &content_width) ||
        !fixed_to_long(action.value.content_size.height, &content_height) ||
        scroll_width < 0 || content_width > LONG_MAX - scroll_width ||
        adjust_window_rectangle_for_client(
            application,
            content_width + scroll_width,
            content_height,
            &rectangle) == -1) {
        return -1;
    }
    application->pending_size = true;
    application->pending_size_revision = action.revision;
    application->pending_content_width = action.value.content_size.width;
    application->pending_content_height = action.value.content_size.height;
    if (!SetWindowPos(
            application->window,
            NULL,
            0,
            0,
            rectangle.right - rectangle.left,
            rectangle.bottom - rectangle.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        return -1;
    }
    if (application->pending_size &&
        boxrig_term_win32_native_host_resize_component(application) == -1) {
        return -1;
    }
    return 1;
}

int boxrig_term_win32_native_host_synchronize_transparency(
    boxrig_term_win32_application *application)
{
    boxrig_ui_transparency_state state;
    boxrig_ui_transparency_config component_config;

    if (application != NULL && application->opacity_host != NULL) {
        application->opacity_request_pending = true;
        return 0;
    }

    if (application == NULL || application->component == NULL ||
        boxrig_win32_transparency_context_get_state(
            application->transparency,
            &state) != BOXRIG_UI_OK) {
        return -1;
    }
    if (application->renderer == BOXRIG_TERM_WIN32_RENDERER_D3D11) {
        boxrig_terminal_invalidate(application->terminal);
        return 0;
    }
    component_config = component_transparency_config(&state);
    if (boxrig_win32_terminal_component_set_transparency(
            application->component,
            &component_config) != BOXRIG_UI_OK) {
        return -1;
    }
    boxrig_terminal_invalidate(application->terminal);
    return 0;
}

static boxrig_ui_status present_component_surface(
    void *context,
    HDC surface,
    SIZE size,
    const RECT *dirty)
{
    boxrig_term_win32_application *application = context;
    boxrig_ui_transparency_state state;
    boxrig_win32_transparency_present_result result;

    (void)dirty;
    if (application == NULL || IsIconic(application->window) ||
        boxrig_win32_transparency_context_get_state(
            application->transparency,
            &state) != BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (state.effective ==
        BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA) {
        result = boxrig_win32_transparency_context_present(
            application->transparency,
            surface,
            size,
            NULL);
        if (result == BOXRIG_WIN32_TRANSPARENCY_FALLBACK_REQUIRED) {
            DWORD error = GetLastError();
            application->opacity_failure.reason = BOXRIG_WIN32_OPACITY_REASON_API_FAILED;
            application->opacity_failure.error = error;
            application->opacity_failure.result = error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error);
            (void)snprintf(application->opacity_failure.operation,
                sizeof(application->opacity_failure.operation), "GDI alpha presentation");
            application->presentation_fallback_required = true;
            return BOXRIG_UI_OK;
        }
        if (result == BOXRIG_WIN32_TRANSPARENCY_PRESENTED_ALPHA)
            application->opacity_presented = true;
        return result == BOXRIG_WIN32_TRANSPARENCY_PRESENTED_ALPHA
                   ? BOXRIG_UI_OK
                   : BOXRIG_UI_UNAVAILABLE;
    }
    result = boxrig_win32_transparency_context_present_opaque(
               application->transparency,
               surface,
               size,
               NULL);
    if (result == BOXRIG_WIN32_TRANSPARENCY_PRESENTED_OPAQUE)
        application->opacity_presented = true;
    return result == BOXRIG_WIN32_TRANSPARENCY_PRESENTED_OPAQUE
               ? BOXRIG_UI_OK
               : BOXRIG_UI_UNAVAILABLE;
}

static bool view_action_status_consumes_input(boxrig_ui_status status)
{
    switch (status) {
    case BOXRIG_UI_OK:
    case BOXRIG_UI_OUT_OF_MEMORY:
    case BOXRIG_UI_LIMIT_EXCEEDED:
    case BOXRIG_UI_UNAVAILABLE:
    case BOXRIG_UI_BUSY:
    case BOXRIG_UI_STALE:
        return true;
    case BOXRIG_UI_INVALID_ARGUMENT:
    default:
        return false;
    }
}

boxrig_ui_status boxrig_term_win32_apply_view_action(
    boxrig_term_win32_application *application,
    const boxrig_ui_view_action *action)
{
    if (application == NULL || application->component == NULL ||
        action == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_win32_terminal_component_apply_view_action(
        application->component,
        action);
}

static bool update_hyperlink_hover(
    boxrig_term_win32_application *application,
    const boxrig_ui_hit_test_result *hit)
{
    boxrig_term_text_pointer_target target;

    if (hit == NULL || application->text_policy == NULL) {
        return false;
    }
    return boxrig_term_text_policy_update_pointer_hover(
               application->text_policy,
               hit,
               &target) == 0;
}

static bool apply_zoom_shortcut(
    boxrig_term_win32_application *application,
    boxrig_ui_zoom_shortcut zoom_shortcut)
{
    boxrig_ui_status status;
    bool changed = false;

    if (zoom_shortcut == BOXRIG_UI_ZOOM_SHORTCUT_RESET) {
        status = boxrig_win32_terminal_component_visual_zoom_reset(
            application->component,
            &changed);
    } else {
        boxrig_font_zoom_direction direction =
            zoom_shortcut == BOXRIG_UI_ZOOM_SHORTCUT_IN
                ? BOXRIG_FONT_ZOOM_INCREASE
                : BOXRIG_FONT_ZOOM_DECREASE;

        status = boxrig_win32_terminal_component_visual_zoom(
            application->component,
            direction,
            boxrig_font_visual_step_policy_default(),
            &changed);
    }
    return status == BOXRIG_UI_OK;
}


static void open_link(void *context, const char *uri, size_t length)
{
    boxrig_term_win32_application *application = context;
    (void)boxrig_term_win32_uri_launcher_open(application->uri_launcher, uri, length);
}

static LRESULT CALLBACK observe_link_input(HWND window, UINT message, WPARAM word,
    LPARAM long_value, UINT_PTR identifier, DWORD_PTR reference)
{
    boxrig_term_win32_application *application = (boxrig_term_win32_application *)reference;
    if (message == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT tracking = {sizeof(tracking), TME_LEAVE, window, 0};
        if (!TrackMouseEvent(&tracking)) boxrig_term_link_gesture_cancel(application->link_gesture);
    } else if (message == WM_MOUSELEAVE || message == WM_KILLFOCUS ||
            message == WM_CANCELMODE || message == WM_CAPTURECHANGED ||
            (message == WM_SHOWWINDOW && word == FALSE)) {
        boxrig_term_link_gesture_cancel(application->link_gesture);
    } else if (message == WM_NCDESTROY) {
        boxrig_term_link_gesture_cancel(application->link_gesture);
        (void)RemoveWindowSubclass(window, observe_link_input, identifier);
        application->link_input_window = NULL;
    }
    return DefSubclassProc(window, message, word, long_value);
}

static int start_links(boxrig_term_win32_application *application)
{
    if (boxrig_term_win32_uri_launcher_create(application->window, application->error,
            &application->uri_launcher) != 0 ||
        boxrig_term_link_gesture_create(application->terminal,
            boxrig_win32_terminal_component_view(application->component),
            application->text_policy, open_link, application, &application->link_gesture) != 0)
        return -1;
    HWND input = boxrig_win32_terminal_component_input_window(application->component);
    if (input == NULL || !SetWindowSubclass(input, observe_link_input, 1, (DWORD_PTR)application)) return -1;
    application->link_input_window = input;
    return 0;
}

void boxrig_term_win32_stop_links(boxrig_term_win32_application *application)
{
    if (application->link_input_window != NULL) {
        (void)RemoveWindowSubclass(application->link_input_window, observe_link_input, 1);
        application->link_input_window = NULL;
    }
    boxrig_term_link_gesture_destroy(application->link_gesture);
    application->link_gesture = NULL;
    boxrig_term_win32_uri_launcher_destroy(application->uri_launcher);
    application->uri_launcher = NULL;
}

boxrig_input_mouse_route boxrig_term_win32_route_application_mouse(
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    void *context)
{
    boxrig_term_win32_application *application = context;
    boxrig_input_mouse_route selection_route;
    boxrig_ui_view *view;
    bool selection_completed;

    if (application == NULL || application->component == NULL ||
        application->terminal == NULL || event == NULL || hit == NULL) {
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    view = boxrig_win32_terminal_component_view(application->component);
    if (view == NULL) {
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    int drag_x = GetSystemMetrics(SM_CXDRAG), drag_y = GetSystemMetrics(SM_CYDRAG);
    int click_x = GetSystemMetrics(SM_CXDOUBLECLK), click_y = GetSystemMetrics(SM_CYDOUBLECLK);
    int drag = drag_x > drag_y ? drag_x : drag_y;
    int click = click_x > click_y ? click_x : click_y;
    boxrig_input_mouse_route link_route = boxrig_term_link_gesture_route(
        application->link_gesture, &application->selection_drag_active, event, hit,
        GetTickCount64(), GetDoubleClickTime(),
        (boxrig_ui_fixed)(click > 0 ? (click - 1) / 2 : 0) * BOXRIG_UI_FIXED_ONE,
        (boxrig_ui_fixed)(drag > 0 ? drag : 4) * BOXRIG_UI_FIXED_ONE);
    if (link_route != BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL) return link_route;
    bool copy_requested;
    boxrig_ui_status copy_status;
    boxrig_input_mouse_route copy_route = boxrig_term_mouse_policy_route_copy(
        view, application->selection_drag_active,
        &application->copy_button_active, event, hit, &copy_requested,
        &copy_status);
    if (copy_route != BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL) {
        if (copy_requested) {
            boxrig_ui_view_action action;
            action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
            action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
            copy_status = boxrig_term_win32_apply_view_action(application, &action);
        }
        if (copy_status != BOXRIG_UI_OK && application->error != NULL) {
            fprintf(application->error,
                "boxrig_term: could not copy selection (status=%d).\n",
                (int)copy_status);
            (void)fflush(application->error);
        }
        return copy_route;
    }
    if (application->selection_drag_active) {
        selection_route = boxrig_term_mouse_policy_route_selection(
            application->terminal,
            view,
            &application->selection_drag_active,
            event,
            hit,
            &selection_completed);
        return selection_route;
    }
    boxrig_ui_zoom_shortcut wheel_zoom;
    boxrig_input_mouse_route wheel_route =
        boxrig_term_mouse_policy_route_wheel(
            application->terminal,
            false,
            event,
            hit,
            &wheel_zoom);
    if (wheel_route != BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL) {
        if (wheel_route == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION &&
            wheel_zoom != BOXRIG_UI_ZOOM_SHORTCUT_NONE &&
            !apply_zoom_shortcut(application, wheel_zoom)) {
            return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
        }
        return wheel_route;
    }
    if (event->action == BOXRIG_MOUSE_MOTION) {
        return update_hyperlink_hover(application, hit)
                   ? BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL
                   : BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    if (event->action != BOXRIG_MOUSE_PRESS ||
        event->button != BOXRIG_MOUSE_BUTTON_LEFT) {
        return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    }
    return boxrig_term_mouse_policy_route_selection(
        application->terminal,
        view,
        &application->selection_drag_active,
        event,
        hit,
        &selection_completed);
}

boxrig_input_key_route boxrig_term_win32_route_application_shortcut(
    const boxrig_key_event *event,
    void *context)
{
    boxrig_term_win32_application *application = context;
    boxrig_ui_background_opacity_shortcut opacity_shortcut =
        boxrig_ui_background_opacity_shortcut_match(event);
    boxrig_ui_clipboard_shortcut clipboard_shortcut =
        boxrig_ui_clipboard_shortcut_match(event);
    boxrig_ui_zoom_shortcut zoom_shortcut =
        boxrig_ui_zoom_shortcut_match(event);
    boxrig_ui_view_action action;
    boxrig_ui_status status;

    if (application == NULL) {
        return BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    boxrig_input_key_route scrollbar_route = boxrig_term_scrollbar_demo_route_key(
        &application->scrollbar_demo, event);
    if (scrollbar_route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL) return scrollbar_route;
    boxrig_input_key_route decoration_route = boxrig_term_decoration_demo_route_key(
        boxrig_win32_terminal_component_view(application->component), event);
    if (decoration_route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL) return decoration_route;
    boxrig_input_key_route color_route = boxrig_term_color_demo_route_key(
        &application->color_demo,
        boxrig_win32_terminal_component_view(application->component), event);
    if (color_route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL) return color_route;
    if (clipboard_shortcut != BOXRIG_UI_CLIPBOARD_SHORTCUT_NONE) {
        action.kind =
            clipboard_shortcut == BOXRIG_UI_CLIPBOARD_SHORTCUT_COPY
                ? BOXRIG_UI_VIEW_ACTION_COPY
                : BOXRIG_UI_VIEW_ACTION_PASTE;
        action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
        status = boxrig_term_win32_apply_view_action(application, &action);
        return view_action_status_consumes_input(status)
                   ? BOXRIG_INPUT_KEY_ROUTE_APPLICATION
                   : BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    if (opacity_shortcut !=
            BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_NONE) {
        if (application->enable_background_opacity &&
            event->action != BOXRIG_KEY_RELEASE) {
            boxrig_term_win32_opacity_shortcut(application, opacity_shortcut);
        }
        return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
    }
    if (zoom_shortcut == BOXRIG_UI_ZOOM_SHORTCUT_NONE) {
        return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    }
    if (event->action == BOXRIG_KEY_RELEASE) {
        return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
    }
    return apply_zoom_shortcut(application, zoom_shortcut)
               ? BOXRIG_INPUT_KEY_ROUTE_APPLICATION
               : BOXRIG_INPUT_KEY_ROUTE_ERROR;
}

static void fail_application(boxrig_term_win32_application *application)
{
    application->failed = true;
    if (application->window != NULL) {
        (void)PostMessageW(application->window, WM_CLOSE, 0, 0);
    }
}

int boxrig_term_win32_native_host_update_pointer(
    boxrig_term_win32_application *application)
{
    if (application->component == NULL || application->text_policy == NULL) return 0;
    HWND input = boxrig_win32_terminal_component_input_window(application->component);
    POINT point = {0};
    RECT client;
    /* Only the input client owns the shared native cursor. In particular,
     * stationary frame updates must not replace another window's cursor. */
    bool inside = input != NULL && IsWindowVisible(input) &&
        GetCursorPos(&point) && WindowFromPoint(point) == input &&
        ScreenToClient(input, &point) && GetClientRect(input, &client) &&
        PtInRect(&client, point);
    if (!inside || !boxrig_ui_view_focused(boxrig_win32_terminal_component_view(application->component)))
        boxrig_term_link_gesture_cancel(application->link_gesture);
    bool hand = false;
    int changed = boxrig_term_text_policy_refresh_pointer(application->text_policy,
        inside && boxrig_ui_view_focused(
            boxrig_win32_terminal_component_view(application->component)),
        (boxrig_ui_fixed)point.x * BOXRIG_UI_FIXED_ONE,
        (boxrig_ui_fixed)point.y * BOXRIG_UI_FIXED_ONE, &hand);
    if (changed < 0) return -1;
    if (inside) {
        HCURSOR cursor = LoadCursorW(NULL, hand ? IDC_HAND : IDC_IBEAM);
        if (cursor == NULL) return -1;
        (void)SetCursor(cursor);
    }
    return changed;
}

static int resynchronize_ime_placement(
    boxrig_term_win32_application *application)
{
    boxrig_win32_ime_placement_status status;

    if (application == NULL) {
        return -1;
    }
    if (application->component == NULL || application->window == NULL ||
        IsIconic(application->window)) {
        return 0;
    }
    status = boxrig_win32_terminal_component_resynchronize_ime_placement(
        application->component);
    return status == BOXRIG_WIN32_IME_PLACEMENT_ERROR ? -1 : 0;
}

static LRESULT window_procedure(
    HWND window,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter)
{
    boxrig_term_win32_application *application =
        (boxrig_term_win32_application *)GetWindowLongPtrW(
            window,
            GWLP_USERDATA);

    if (message == WM_NCCREATE) {
        CREATESTRUCTW *create = (CREATESTRUCTW *)long_parameter;

        application = create->lpCreateParams;
        application->window = window;
        (void)SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            (LONG_PTR)application);
    }
    if (application == NULL) {
        return DefWindowProcW(
            window,
            message,
            word_parameter,
            long_parameter);
    }
    switch (message) {
    case WM_CLOSE:
        application->interactive_sizing = false;
        if (boxrig_ui_lifecycle_begin_shutdown(
                &application->lifecycle) == -1) {
            application->failed = true;
        }
        application->window_closed = true;
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        application->interactive_sizing = false;
        application->window_closed = true;
        (void)boxrig_ui_lifecycle_begin_shutdown(&application->lifecycle);
        PostQuitMessage(0);
        break;
    case WM_NCDESTROY:
        application->interactive_sizing = false;
        application->window = NULL;
        (void)SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(
            window,
            message,
            word_parameter,
            long_parameter);
    default:
        break;
    }
    /* An opacity transaction repaints the owner's backing synchronously,
     * after its replacement frame is complete. Permit only that paint while
     * preventing input, layout and transparency changes from reentering the
     * unfinished transaction or erasing the previous image during preparation. */
    if (!boxrig_ui_lifecycle_accepts_events(&application->lifecycle) ||
        (application->opacity_applying &&
         !(application->opacity_prepared && application->opacity_presented &&
           (message == WM_PAINT || message == WM_ERASEBKGND)))) {
        return DefWindowProcW(
            window,
            message,
            word_parameter,
            long_parameter);
    }
    if (!application->opacity_applying && application->transparency_bound &&
        !(message == WM_SIZE && word_parameter != SIZE_MINIMIZED &&
          application->component != NULL)) {
        bool state_changed = false;
        int transparency_result =
            boxrig_win32_transparency_context_process_host_message(
                application->transparency,
                message,
                word_parameter,
                long_parameter,
                &state_changed);

        if (transparency_result == -1 ||
            (state_changed &&
             boxrig_term_win32_native_host_synchronize_transparency(
                 application) == -1)) {
            fail_application(application);
            return 0;
        }
    }
    if ((message == WM_SHOWWINDOW && word_parameter == FALSE) ||
        (message == WM_SIZE && word_parameter == SIZE_MINIMIZED) ||
        message == WM_CLOSE || message == WM_CANCELMODE)
        boxrig_term_link_gesture_cancel(application->link_gesture);
    switch (message) {
    case BOXRIG_TERM_PRESENTATION_READY:
        if (application->interactive_sizing && boxrig_term_win32_frame_component(application) == -1)
            fail_application(application);
        return 0;
    case WM_SETCURSOR:
        if (application->component != NULL &&
            (HWND)word_parameter == boxrig_win32_terminal_component_input_window(
                application->component) && LOWORD(long_parameter) == HTCLIENT) {
            /* Returning TRUE also stops the child's DefWindowProc from
             * restoring its registered I-beam after this parent callback. */
            if (boxrig_term_win32_native_host_update_pointer(application) < 0)
                fail_application(application);
            return TRUE;
        }
        break;
    case WM_DWMCOMPOSITIONCHANGED:
        if (application->background_effect != NULL)
            (void)boxrig_win32_background_effect_refresh(application->background_effect);
        if (application->opacity_host != NULL) {
            application->opacity_alpha_failed = false;
            application->opacity_refresh_pending = true;
            application->opacity_request_pending = true;
        }
        break;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        if (application->background_effect != NULL)
            (void)boxrig_win32_background_effect_refresh(application->background_effect);
        break;
    case WM_ENTERSIZEMOVE:
        application->interactive_sizing = true;
        return 0;
    case WM_EXITSIZEMOVE:
        application->interactive_sizing = false;
        if (resynchronize_ime_placement(application) == -1) {
            fail_application(application);
        }
        return 0;
    case WM_MOVE:
        if (resynchronize_ime_placement(application) == -1) {
            fail_application(application);
        }
        return 0;
    case WM_SIZING:
        {
            int sizing_result = constrain_interactive_sizing_rectangle(
                application,
                word_parameter,
                (RECT *)long_parameter);

            if (sizing_result == -1) {
                fail_application(application);
                return FALSE;
            }
            return sizing_result == 1 ? TRUE : FALSE;
        }
    case WM_SIZE:
        if (application->component != NULL &&
            word_parameter != SIZE_MINIMIZED) {
            if (boxrig_term_win32_native_host_resize_component(application) ==
                    -1 ||
                (application->interactive_sizing &&
                 boxrig_term_win32_frame_component(application) == -1)) {
                fail_application(application);
            }
        }
        return 0;
    case WM_SETFOCUS:
        if (application->component == NULL ||
            boxrig_win32_terminal_component_input_window(
                application->component) != window) {
            (void)boxrig_win32_terminal_component_request_focus(
                application->component);
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        {
            PAINTSTRUCT paint;

            HDC target = BeginPaint(window, &paint);
            /* Presentation children retain both opaque and alpha frames. Keep
             * only uncovered backing pixels clear for the owner's DWM effect. */
            if (application->opacity_host != NULL && target != NULL &&
                paint.rcPaint.right > paint.rcPaint.left && paint.rcPaint.bottom > paint.rcPaint.top &&
                !FillRect(target, &paint.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH)))
                fail_application(application);
            EndPaint(window, &paint);
            if (application->opacity_host != NULL)
                boxrig_terminal_invalidate(application->terminal);
        }
        return 0;
    default:
        break;
    }
    if (application->component != NULL) {
        HWND input_window =
            boxrig_win32_terminal_component_input_window(
                application->component);
        LRESULT result = 0;
        boxrig_ui_component_event_result dispatch =
            boxrig_win32_terminal_component_dispatch_message(
                application->component,
                input_window,
                message,
                word_parameter,
                long_parameter,
                &result);

        if (dispatch == BOXRIG_UI_COMPONENT_EVENT_FAILURE) {
            fail_application(application);
            return 0;
        }
        if (dispatch == BOXRIG_UI_COMPONENT_EVENT_HANDLED ||
            dispatch == BOXRIG_UI_COMPONENT_EVENT_BLOCKED) {
            return result;
        }
    }
    return DefWindowProcW(window, message, word_parameter, long_parameter);
}

LRESULT CALLBACK boxrig_term_win32_window_procedure(HWND window, UINT message,
    WPARAM word_parameter, LPARAM long_parameter)
{
    boxrig_term_win32_application *application =
        (boxrig_term_win32_application *)GetWindowLongPtrW(window, GWLP_USERDATA);
    LRESULT result;
    if (application != NULL && boxrig_term_win32_suspend_presentation_wait(application) == -1) {
        fail_application(application);
        return 0;
    }
    /* DefWindowProc can enter a modal loop. Do not hold a work-depth guard
     * across it: nested sizing messages must be able to rearm notification. */
    result = window_procedure(window, message, word_parameter, long_parameter);
    if (application != NULL && boxrig_term_win32_resume_presentation_wait(application) == -1)
        fail_application(application);
    return result;
}

int boxrig_term_win32_native_host_create(
    boxrig_term_win32_application *application,
    const boxrig_term_win32_config *config)
{
    boxrig_win32_window_requirements requirements;
    boxrig_ui_transparency_state transparency_state;
    boxrig_win32_terminal_component_config component_config;
    boxrig_win32_terminal_borrowed_target borrowed_target;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_status component_status = BOXRIG_UI_UNAVAILABLE;
    RECT rectangle;
    int scroll_width;
    LONG client_width;
    LONG client_height;
    boxrig_ui_transparency_config host_transparency = config->transparency;
    bool software = application->renderer == BOXRIG_TERM_WIN32_RENDERER_SOFTWARE;
    application->opacity_applying = true;
    {
        host_transparency.request = BOXRIG_UI_TRANSPARENCY_OPAQUE;
        host_transparency.background_opacity = 255;
    }

    boxrig_win32_dpi_api_initialize(&application->dpi, GetProcAddress);
    if (create_transparency_host(
            application,
            &host_transparency,
            &requirements,
            &transparency_state) == -1) {
        return -1;
    }
    application->external_presentation = software;
    component_config = boxrig_win32_terminal_component_config_default();
    /* The same composed control owns geometry and input for every renderer. */
    component_config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED;
    component_config.paste_capacity = config->terminal.clipboard_capacity;
    component_config.paste_chunk_size = config->terminal.output_capacity;
    component_config.transparency =
        component_transparency_config(&transparency_state);
#ifdef BOXRIG_TERM_ENABLE_D3D11
    if (application->renderer == BOXRIG_TERM_WIN32_RENDERER_D3D11) {
        boxrig_win32_d3d11_config graphics = boxrig_win32_d3d11_config_default();
        graphics.creation_result = &application->renderer_creation_result;
        component_status = boxrig_win32_d3d11_terminal_component_create_child(
            application->instance, application->window, application->model,
            &component_config, &graphics, &application->component);
    }
#endif
#ifdef BOXRIG_TERM_ENABLE_OPENGL
    if (application->renderer == BOXRIG_TERM_WIN32_RENDERER_OPENGL) {
        component_status = boxrig_win32_opengl_terminal_component_create_child(
            application->instance, application->window, application->model,
            &component_config, &application->component);
        if (component_status == BOXRIG_UI_UNAVAILABLE &&
            config->renderer == BOXRIG_TERM_WIN32_RENDERER_AUTO) {
            application->renderer = BOXRIG_TERM_WIN32_RENDERER_SOFTWARE;
            application->external_presentation = software = true;
        }
    }
#endif
    if (software) {
        borrowed_target.presentation_window = application->window;
        borrowed_target.input_window = application->window;
        borrowed_target.presenter = present_component_surface;
        borrowed_target.presenter_context = application;
        component_status = boxrig_win32_terminal_component_create_on_target(
            application->instance,
            &borrowed_target,
            application->model,
            &component_config,
            &application->component);
    }
    application->renderer_status = component_status;
    if (software && config->enable_renderer_trace &&
        component_status == BOXRIG_UI_OK &&
        !boxrig_win32_terminal_component_set_renderer_phase_observer(
            application->component,
            boxrig_term_win32_renderer_trace_observer(
                &application->renderer_trace))) return -1;
    if (component_status == BOXRIG_UI_OK &&
        boxrig_term_win32_opacity_initialize(application, config) == -1) return -1;
    if (component_status != BOXRIG_UI_OK ||
        (boxrig_term_color_demo_initialize(&application->color_demo,
            boxrig_win32_terminal_component_view(application->component)) == -1 ||
            boxrig_term_scrollbar_demo_initialize(&application->scrollbar_demo,
                boxrig_win32_terminal_component_view(application->component),
                set_scrollbar_policy, application->component) == -1 ||
            boxrig_term_decoration_demo_initialize(boxrig_win32_terminal_component_view(application->component)) == -1 ||
            boxrig_term_background_demo_initialize(boxrig_win32_terminal_component_view(application->component)) == -1) ||
        boxrig_term_text_policy_create(
            application->terminal,
            boxrig_win32_terminal_component_view(application->component),
            &application->text_policy) == -1 ||
        start_links(application) == -1 ||
        boxrig_win32_terminal_component_get_effective_metrics(
            application->component,
            &metrics) != BOXRIG_FONT_CATALOG_OK ||
        metrics.cell_width == 0 || metrics.cell_height == 0 ||
        config->terminal.columns > (size_t)LONG_MAX / metrics.cell_width ||
        config->terminal.rows > (size_t)LONG_MAX / metrics.cell_height) {
        return -1;
    }
    if (boxrig_win32_terminal_component_set_key_router(
            application->component,
            boxrig_term_win32_route_application_shortcut,
            application) != BOXRIG_UI_OK ||
        boxrig_win32_terminal_component_set_mouse_router(
            application->component,
            boxrig_term_win32_route_application_mouse,
            application) != BOXRIG_UI_OK) {
        return -1;
    }
    scroll_width = scrollbar_pixel_width(application);
    client_width = (LONG)(config->terminal.columns * metrics.cell_width);
    client_height = (LONG)(config->terminal.rows * metrics.cell_height);
    if (scroll_width < 0 || client_width > LONG_MAX - scroll_width ||
        adjust_window_rectangle_for_client(
            application,
            client_width + scroll_width,
            client_height,
            &rectangle) == -1) {
        return -1;
    }
    if (!SetWindowPos(
            application->window,
            NULL,
            0,
            0,
            rectangle.right - rectangle.left,
            rectangle.bottom - rectangle.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) ||
        boxrig_term_win32_native_host_resize_component(application) == -1 ||
        boxrig_win32_host_adapter_create(
            &(boxrig_win32_host_targets){
                application->window,
                boxrig_win32_terminal_component_input_window(
                    application->component),
                boxrig_win32_terminal_component_presentation_window(
                    application->component),
            },
            application->host_consumer,
            config->terminal.clipboard_capacity,
            &config->host,
            &application->host_adapter) == -1 ||
        boxrig_win32_terminal_component_set_visible(
            application->component,
            true) != BOXRIG_UI_OK ||
        boxrig_win32_transparency_context_set_visible(
            application->transparency,
            true) != BOXRIG_UI_OK) {
        return -1;
    }
    application->opacity_applying = false;
    boxrig_ui_background_effect_request effect_request;
    if (boxrig_term_background_effect_from_environment(&effect_request) == -1) return -1;
    if (effect_request != BOXRIG_UI_BACKGROUND_EFFECT_NONE) {
        if (boxrig_win32_background_effect_create(application->window, &application->background_effect) != BOXRIG_UI_OK)
            return -1;
        boxrig_ui_status status = boxrig_win32_background_effect_set(application->background_effect, effect_request);
        boxrig_ui_background_effect_state state = boxrig_win32_background_effect_get_state(application->background_effect);
        if (config->error != NULL) {
            fprintf(config->error, "boxrig_term: background-effect=acrylic supported=%d accepted=%d reason=%d hresult=0x%lx\n",
                state.supported, state.accepted, state.reason, (unsigned long)state.native_result);
            fflush(config->error);
        }
        if (status != BOXRIG_UI_OK) return -1;
    }
    return boxrig_term_win32_opacity_prepare(application);
}

void boxrig_term_win32_native_host_show(
    boxrig_term_win32_application *application,
    int show_command)
{
    ShowWindow(application->window, show_command);
    (void)UpdateWindow(application->window);
}
