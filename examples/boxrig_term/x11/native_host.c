/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "application_internal.h"
#include "../common/background_demo.h"

#include <glib.h>
#include <X11/cursorfont.h>
#include "../common/runtime_info.h"

#include "boxrig/ui/shortcut.h"
#include "boxrig/x11/opengl_component.h"

#include <GL/gl.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <cairo/cairo.h>

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static boxrig_ui_status set_color_glyphs(void *context, bool enabled)
{
    boxrig_font_rendering_policy policy;
    boxrig_ui_status status = boxrig_x11_terminal_component_get_font_rendering_policy(context, &policy);
    if (status != BOXRIG_UI_OK) return status;
    policy.color_glyphs = enabled;
    return boxrig_x11_terminal_component_set_font_rendering_policy(context, &policy);
}

static boxrig_ui_status set_scrollbar_policy(void *context, boxrig_ui_scrollbar_visibility policy)
{
    return boxrig_x11_terminal_component_set_scrollbar_visibility(context, policy);
}

static void write_opengl_runtime_info(void)
{
    const char *version = (const char *)glGetString(GL_VERSION);
    bool use_es = version != NULL && strncmp(version, "OpenGL ES ", 10) == 0;
    boxrig_term_runtime_info info = {
        .frontend = "x11",
        .window_system = "x11",
        .renderer = "opengl",
        .graphics_api = use_es ? "OpenGL ES" : "OpenGL",
        .graphics_api_version = version,
        .graphics_vendor = (const char *)glGetString(GL_VENDOR),
        .graphics_device = (const char *)glGetString(GL_RENDERER)};

    (void)boxrig_term_runtime_info_write(stdout, &info);
}

static void write_software_runtime_info(void)
{
    boxrig_term_runtime_info info = {
        .frontend = "x11",
        .window_system = "x11",
        .renderer = "x11",
        .graphics_api = "Cairo",
        .graphics_api_version = cairo_version_string()};

    (void)boxrig_term_runtime_info_write(stdout, &info);
}

static boxrig_ui_transparency_config component_transparency_config(
    const boxrig_ui_transparency_config *config)
{
    boxrig_ui_transparency_config component_config = *config;

    component_config.requested_scopes =
        BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    return component_config;
}

static boxrig_ui_status create_terminal_component(
    boxrig_term_x11_application *application,
    const boxrig_term_x11_config *config,
    const boxrig_x11_terminal_borrowed_target *target,
    const boxrig_x11_terminal_component_config *component_config)
{
    boxrig_ui_status status;

    if (config->renderer != BOXRIG_TERM_POSIX_RENDERER_SOFTWARE) {
        boxrig_x11_opengl_component_config opengl_config =
            boxrig_x11_opengl_component_config_default();

        status = boxrig_x11_opengl_terminal_component_create_on_target(
            application->display,
            target,
            application->model,
            component_config,
            &opengl_config,
            &application->component);
        if (status == BOXRIG_UI_OK) {
            write_opengl_runtime_info();
            return status;
        }
        if (config->renderer != BOXRIG_TERM_POSIX_RENDERER_AUTO ||
            status != BOXRIG_UI_UNAVAILABLE) {
            fprintf(
                stderr,
                "boxrig_term: renderer=opengl creation failed (status=%d)\n",
                (int)status);
            return status;
        }
    }
    status = boxrig_x11_terminal_component_create_on_target(
        application->display,
        target,
        application->model,
        component_config,
        &application->component);
    if (status == BOXRIG_UI_OK) {
        write_software_runtime_info();
    }
    return status;
}

static bool fixed_to_unsigned(
    boxrig_ui_fixed value,
    unsigned int *value_out)
{
    boxrig_ui_fixed pixels;

    if (value <= 0 || value % BOXRIG_UI_FIXED_ONE != 0) {
        return false;
    }
    pixels = value / BOXRIG_UI_FIXED_ONE;
    if (pixels > UINT_MAX) {
        return false;
    }
    *value_out = (unsigned int)pixels;
    return true;
}

static bool build_cell_resize_hints(
    const boxrig_font_effective_metrics *metrics,
    unsigned int scrollbar_width,
    XSizeHints *hints_out)
{
    XSizeHints hints;

    if (metrics == NULL || hints_out == NULL ||
        scrollbar_width > INT_MAX || metrics->cell_width == 0 ||
        metrics->cell_width > INT_MAX || metrics->cell_height == 0 ||
        metrics->cell_height > INT_MAX ||
        metrics->cell_width > INT_MAX - scrollbar_width) {
        return false;
    }
    memset(&hints, 0, sizeof(hints));
    hints.flags = PBaseSize | PResizeInc | PMinSize;
    hints.base_width = (int)scrollbar_width;
    hints.base_height = 0;
    hints.width_inc = (int)metrics->cell_width;
    hints.height_inc = (int)metrics->cell_height;
    hints.min_width = hints.base_width + hints.width_inc;
    hints.min_height = hints.height_inc;
    *hints_out = hints;
    return true;
}

static bool cell_client_extent(
    const XSizeHints *hints,
    size_t rows,
    size_t columns,
    unsigned int *width_out,
    unsigned int *height_out)
{
    unsigned int base_width;
    unsigned int width_increment;
    unsigned int height_increment;

    if (hints == NULL || rows == 0 || columns == 0 ||
        width_out == NULL || height_out == NULL || hints->base_width < 0 ||
        hints->base_height != 0 || hints->width_inc <= 0 ||
        hints->height_inc <= 0) {
        return false;
    }
    base_width = (unsigned int)hints->base_width;
    width_increment = (unsigned int)hints->width_inc;
    height_increment = (unsigned int)hints->height_inc;
    if (columns > (UINT_MAX - base_width) / width_increment ||
        rows > UINT_MAX / height_increment) {
        return false;
    }
    *width_out = base_width + (unsigned int)columns * width_increment;
    *height_out = (unsigned int)rows * height_increment;
    return true;
}

int boxrig_term_x11_native_host_synchronize_transparency(
    boxrig_term_x11_application *application)
{
    boxrig_ui_transparency_state state;
    boxrig_ui_transparency_config config;

    if (application == NULL || application->transparency == NULL ||
        application->component == NULL ||
        boxrig_x11_transparency_context_get_state(
            application->transparency,
            &state) != BOXRIG_UI_OK) {
        return -1;
    }
    config = component_transparency_config(&state.config);
    return boxrig_x11_terminal_component_set_transparency(
               application->component,
               &config) == BOXRIG_UI_OK
               ? 0
               : -1;
}

static boxrig_ui_window_state current_window_state(
    const boxrig_term_x11_application *application)
{
    long offset = 0;

    for (;;) {
        Atom type;
        int format;
        unsigned long count;
        unsigned long remaining;
        unsigned char *property = NULL;
        unsigned long index;
        bool maximized = false;

        if (XGetWindowProperty(application->display, application->window,
                application->wm_state, offset, 32, False, XA_ATOM,
                &type, &format, &count, &remaining, &property) != Success ||
            type != XA_ATOM || format != 32) {
            if (property != NULL) {
                XFree(property);
            }
            return BOXRIG_UI_WINDOW_NORMAL;
        }
        for (index = 0; index < count; index++) {
            Atom state = ((Atom *)property)[index];

            /* A one-axis EWMH maximization also constrains the normal anchor. */
            maximized = maximized || state == application->wm_maximized_vertical ||
                        state == application->wm_maximized_horizontal;
        }
        XFree(property);
        if (maximized) {
            return BOXRIG_UI_WINDOW_MAXIMIZED;
        }
        if (remaining == 0 || count == 0 || count > (unsigned long)LONG_MAX ||
            offset > LONG_MAX - (long)count) {
            return BOXRIG_UI_WINDOW_NORMAL;
        }
        offset += (long)count;
    }
}

int boxrig_term_x11_native_host_allocate(
    boxrig_term_x11_application *application,
    unsigned long serial,
    unsigned int width,
    unsigned int height)
{
    if (application != NULL && application->background_effect != NULL && width != 0 && height != 0 && width <= INT32_MAX && height <= INT32_MAX) {
        boxrig_ui_background_effect_rectangle region = {0, 0, (int32_t)width, (int32_t)height};
        boxrig_ui_background_effect_state state = boxrig_x11_background_effect_get_state(application->background_effect);
        (void)boxrig_x11_background_effect_set(application->background_effect, state.request, &region, 1);
    }

    boxrig_ui_component_allocation allocation;
    boxrig_ui_layout_result layout;
    boxrig_ui_scrollbar_visibility_state scrollbar;
    unsigned int scrollbar_width;
    unsigned int content_width;

    if (application == NULL || application->component == NULL ||
        width == 0 || height == 0) {
        return -1;
    }
    if (boxrig_x11_terminal_component_get_scrollbar_visibility(
            application->component, &scrollbar) != BOXRIG_UI_OK) return -1;
    scrollbar_width = scrollbar.reserved_width;
    if (scrollbar.pending) {
        application->pending_allocation = true;
        application->pending_allocation_serial = serial;
        application->pending_allocation_width = width;
        application->pending_allocation_height = height;
        return 0;
    }
    application->pending_allocation = false;
    if (application->pending_size &&
        serial < application->pending_size_serial) {
        return 0;
    }
    content_width = width > scrollbar_width
                        ? width - scrollbar_width
                        : 1;
    if (boxrig_x11_terminal_component_set_target_extent(
            application->component,
            width,
            height) != BOXRIG_UI_OK) {
        return -1;
    }
    if (application->pending_size) {
        boxrig_ui_size_outcome outcome;
        bool applied = false;
        boxrig_ui_fixed fixed_width =
            (boxrig_ui_fixed)content_width * BOXRIG_UI_FIXED_ONE;
        boxrig_ui_fixed fixed_height =
            (boxrig_ui_fixed)height * BOXRIG_UI_FIXED_ONE;

        outcome = fixed_width == application->pending_content_width &&
                          fixed_height ==
                              application->pending_content_height
                      ? BOXRIG_UI_SIZE_ACCEPTED
                      : BOXRIG_UI_SIZE_CONSTRAINED;
        if (boxrig_x11_terminal_component_complete_size_command(
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
    memset(&allocation, 0, sizeof(allocation));
    allocation.width = width;
    allocation.height = height;
    allocation.window_state = current_window_state(application);
    if (boxrig_x11_terminal_component_allocate(
            application->component,
            &allocation,
            &layout) != BOXRIG_UI_OK) {
        return -1;
    }
    return 0;
}

int boxrig_term_x11_native_host_process_layout_action(
    boxrig_term_x11_application *application)
{
    boxrig_ui_layout_action action;
    boxrig_font_effective_metrics metrics;
    XSizeHints hints;
    unsigned int content_width;
    unsigned int height;
    boxrig_ui_scrollbar_visibility_state scrollbar;
    unsigned int scrollbar_width;

    if (application == NULL || application->display == NULL ||
        application->window == None || application->component == NULL) {
        return -1;
    }
    if (boxrig_x11_terminal_component_get_scrollbar_visibility(
            application->component, &scrollbar) != BOXRIG_UI_OK) return -1;
    scrollbar_width = scrollbar.reserved_width;
    if (application->pending_allocation && !scrollbar.pending &&
        boxrig_term_x11_native_host_allocate(application,
            application->pending_allocation_serial, application->pending_allocation_width,
            application->pending_allocation_height) == -1) return -1;
    if (!boxrig_x11_terminal_component_take_layout_action(
            application->component, &action)) {
        /* A bar narrower than one cell can change without a grid resize.
         * Keep the WM's base size in step with that committed allocation. */
        if (!scrollbar.pending &&
            application->resize_hints_scrollbar_width != scrollbar_width) {
            if (boxrig_x11_terminal_component_get_effective_metrics(
                    application->component, &metrics) != BOXRIG_FONT_CATALOG_OK ||
                !build_cell_resize_hints(&metrics, scrollbar_width, &hints)) return -1;
            XSetWMNormalHints(application->display, application->window, &hints);
            application->resize_hints_scrollbar_width = scrollbar_width;
        }
        return 0;
    }
    if (action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
        bool succeeded = application->session != NULL
                             ? boxrig_session_resize(
                                   application->session,
                                   action.value.grid_resize.rows,
                                   action.value.grid_resize.columns) ==
                                   BOXRIG_SESSION_OK
                             : boxrig_terminal_resize(
                                   application->terminal,
                                   action.value.grid_resize.rows,
                                   action.value.grid_resize.columns) ==
                                   BOXRIG_STATUS_OK;
        boxrig_ui_status completion =
            boxrig_x11_terminal_component_complete_grid_resize(
                application->component,
                action.revision,
                succeeded);

        if (!succeeded || completion != BOXRIG_UI_OK ||
            boxrig_x11_terminal_component_get_scrollbar_visibility(
                application->component, &scrollbar) != BOXRIG_UI_OK ||
            boxrig_x11_terminal_component_get_effective_metrics(
                application->component, &metrics) != BOXRIG_FONT_CATALOG_OK ||
            !build_cell_resize_hints(&metrics, scrollbar.reserved_width, &hints)) return -1;
        XSetWMNormalHints(application->display, application->window, &hints);
        application->resize_hints_scrollbar_width = scrollbar.reserved_width;
        if (application->pending_allocation)
            return boxrig_term_x11_native_host_allocate(application,
                application->pending_allocation_serial, application->pending_allocation_width,
                application->pending_allocation_height);
        return 0;
    }
    if (application->pending_size &&
        application->pending_size_revision == action.revision) {
        return 0;
    }
    if (!fixed_to_unsigned(action.value.content_size.width, &content_width) ||
        !fixed_to_unsigned(action.value.content_size.height, &height) ||
        content_width > UINT_MAX - scrollbar_width ||
        boxrig_x11_terminal_component_get_effective_metrics(
            application->component,
            &metrics) != BOXRIG_FONT_CATALOG_OK ||
        !build_cell_resize_hints(
            &metrics,
            scrollbar_width,
            &hints)) {
        return -1;
    }
    /* The WM must observe the new cell progression before the native resize.
     * Queue the property between the extended freeze and ConfigureRequest on
     * the same X connection. */
    if (boxrig_term_x11_wm_sync_begin_frame(
            &application->wm_sync) == -1) {
        return -1;
    }
    XSetWMNormalHints(
        application->display,
        application->window,
        &hints);
    application->pending_size = true;
    application->resize_hints_scrollbar_width = scrollbar_width;
    application->pending_size_revision = action.revision;
    application->pending_size_serial = NextRequest(application->display);
    application->pending_content_width = action.value.content_size.width;
    application->pending_content_height = action.value.content_size.height;
    XResizeWindow(
        application->display,
        application->window,
        content_width + scrollbar_width,
        height);
    XFlush(application->display);
    return 0;
}

static int apply_background_opacity_shortcut(
    boxrig_term_x11_application *application,
    boxrig_ui_background_opacity_shortcut shortcut)
{
    boxrig_ui_transparency_state state;
    boxrig_ui_transparency_config config;
    uint8_t next_opacity;
    uint8_t next_remembered_opacity;

    if (boxrig_x11_transparency_context_get_state(
            application->transparency,
            &state) != BOXRIG_UI_OK) {
        return -1;
    }
    if (!boxrig_ui_background_opacity_shortcut_next(
            shortcut,
            state.config.background_opacity,
            application->remembered_background_opacity,
            &next_opacity,
            &next_remembered_opacity)) {
        return 0;
    }
    config = state.config;
    config.background_opacity = next_opacity;
    if (boxrig_x11_transparency_context_set_config(
            application->transparency,
            &config) != BOXRIG_UI_OK ||
        boxrig_term_x11_native_host_synchronize_transparency(
            application) == -1) {
        return -1;
    }
    application->remembered_background_opacity =
        next_remembered_opacity;
    return 0;
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

boxrig_ui_status boxrig_term_x11_apply_view_action(
    boxrig_term_x11_application *application,
    const boxrig_ui_view_action *action)
{
    if (application == NULL || application->component == NULL ||
        action == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_x11_terminal_component_apply_view_action(
        application->component,
        action);
}

static bool update_hyperlink_hover(
    boxrig_term_x11_application *application,
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
    boxrig_term_x11_application *application,
    boxrig_ui_zoom_shortcut zoom_shortcut)
{
    boxrig_ui_status status;
    bool changed = false;

    if (zoom_shortcut == BOXRIG_UI_ZOOM_SHORTCUT_RESET) {
        status = boxrig_x11_terminal_component_visual_zoom_reset(
            application->component,
            &changed);
    } else {
        boxrig_font_zoom_direction direction =
            zoom_shortcut == BOXRIG_UI_ZOOM_SHORTCUT_IN
                ? BOXRIG_FONT_ZOOM_INCREASE
                : BOXRIG_FONT_ZOOM_DECREASE;

        status = boxrig_x11_terminal_component_visual_zoom(
            application->component,
            direction,
            boxrig_font_visual_step_policy_default(),
            &changed);
    }
    return status == BOXRIG_UI_OK;
}

static void open_link(void *context, const char *uri, size_t length)
{
    boxrig_term_x11_application *application = context;
    (void)boxrig_term_uri_launcher_open(application->uri_launcher, uri, length);
}

boxrig_input_mouse_route boxrig_term_x11_route_application_mouse(
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    void *context)
{
    boxrig_term_x11_application *application = context;
    boxrig_input_mouse_route selection_route;
    boxrig_ui_view_action action;
    boxrig_ui_view *view;
    boxrig_ui_status status;
    bool override;
    bool selection_completed;

    if (application == NULL || application->component == NULL ||
        application->terminal == NULL || event == NULL || hit == NULL) {
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    view = boxrig_x11_terminal_component_view(application->component);
    if (view == NULL) {
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    application->pointer_x = hit->x;
    application->pointer_y = hit->y;
    application->pointer_inside = hit->inside_grid;
    boxrig_input_mouse_route link_route = boxrig_term_link_gesture_route(
        application->link_gesture, &application->selection_drag_active, event, hit,
        (uint64_t)(g_get_monotonic_time() / 1000), 500, 4 * BOXRIG_UI_FIXED_ONE, 4 * BOXRIG_UI_FIXED_ONE);
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
            copy_status = boxrig_term_x11_apply_view_action(application, &action);
        }
        if (copy_status != BOXRIG_UI_OK) {
            fprintf(stderr,
                "boxrig_term: could not copy selection (status=%d).\n",
                (int)copy_status);
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
        if (selection_route == BOXRIG_INPUT_MOUSE_ROUTE_ERROR) {
            return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
        }
        if (selection_completed) {
            action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
            action.clipboard_target =
                BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
            status = boxrig_term_x11_apply_view_action(
                application,
                &action);
            if (!view_action_status_consumes_input(status)) {
                return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
            }
        }
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
    if (event->action != BOXRIG_MOUSE_PRESS) {
        return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    }
    override = (event->modifiers & BOXRIG_MODIFIER_SHIFT) != 0;
    if (event->button == BOXRIG_MOUSE_BUTTON_MIDDLE) {
        if (boxrig_terminal_mouse_reporting_enabled(application->terminal) &&
            !override) {
            return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
        }
        action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
        action.clipboard_target =
            BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
        status = boxrig_term_x11_apply_view_action(application, &action);
        return view_action_status_consumes_input(status)
                   ? BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION
                   : BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    if (event->button != BOXRIG_MOUSE_BUTTON_LEFT) {
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

boxrig_input_key_route boxrig_term_x11_route_application_shortcut(
    const boxrig_key_event *event,
    void *context)
{
    boxrig_term_x11_application *application = context;
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
        boxrig_x11_terminal_component_view(application->component), event);
    if (decoration_route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL) return decoration_route;
    boxrig_input_key_route font_route =
        boxrig_term_font_demo_route_key(&application->font_demo, event);
    if (font_route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL) return font_route;
    boxrig_input_key_route color_route = boxrig_term_color_demo_route_key(
        &application->color_demo, boxrig_x11_terminal_component_view(application->component), event);
    if (color_route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL) return color_route;
    if (clipboard_shortcut != BOXRIG_UI_CLIPBOARD_SHORTCUT_NONE) {
        action.kind =
            clipboard_shortcut == BOXRIG_UI_CLIPBOARD_SHORTCUT_COPY
                ? BOXRIG_UI_VIEW_ACTION_COPY
                : BOXRIG_UI_VIEW_ACTION_PASTE;
        action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
        status = boxrig_term_x11_apply_view_action(application, &action);
        return view_action_status_consumes_input(status)
                   ? BOXRIG_INPUT_KEY_ROUTE_APPLICATION
                   : BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    if (application->enable_background_opacity &&
        opacity_shortcut !=
            BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_NONE) {
        if (event->action != BOXRIG_KEY_RELEASE &&
            apply_background_opacity_shortcut(
                application,
                opacity_shortcut) == -1) {
            return BOXRIG_INPUT_KEY_ROUTE_ERROR;
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

int boxrig_term_x11_native_host_create(
    boxrig_term_x11_application *application,
    const boxrig_term_x11_config *config)
{
    boxrig_x11_window_requirements requirements;
    boxrig_x11_opengl_window_requirements opengl_requirements;
    boxrig_x11_terminal_component_config component_config;
    boxrig_x11_terminal_borrowed_target borrowed_target;
    boxrig_ui_transparency_config component_transparency;
    boxrig_x11_host_policy host_policy;
    boxrig_font_effective_metrics metrics;
    XSizeHints hints;
    boxrig_ui_scrollbar_visibility_state scrollbar;
    unsigned int width;
    unsigned int height;
    bool transparency_changed = false;

    application->display = XOpenDisplay(NULL);
    if (application->display == NULL) {
        return -1;
    }
    application->screen = DefaultScreen(application->display);
    if (boxrig_x11_transparency_context_create(
            application->display,
            application->screen,
            &config->transparency,
            &application->transparency) != BOXRIG_UI_OK ||
        boxrig_x11_transparency_context_get_window_requirements(
            application->transparency,
            &requirements) != BOXRIG_UI_OK) {
        return -1;
    }
    component_transparency =
        component_transparency_config(&config->transparency);
    if (config->renderer != BOXRIG_TERM_POSIX_RENDERER_SOFTWARE &&
        boxrig_x11_opengl_terminal_component_get_window_requirements(
            application->display,
            application->screen,
            &component_transparency,
            &opengl_requirements) == BOXRIG_UI_OK) {
        application->window_colormap = XCreateColormap(
            application->display,
            RootWindow(application->display, application->screen),
            opengl_requirements.visual,
            AllocNone);
        if (application->window_colormap != None) {
            requirements.visual = opengl_requirements.visual;
            requirements.depth = opengl_requirements.depth;
            requirements.colormap = application->window_colormap;
            requirements.attributes.background_pixel = 0;
            requirements.attributes.border_pixel = 0;
            requirements.attributes.colormap = application->window_colormap;
            requirements.attribute_mask =
                CWBackPixel | CWBorderPixel | CWColormap;
            requirements.premultiplied_argb32 =
                opengl_requirements.premultiplied_argb32;
        }
    }
    application->window = XCreateWindow(
        application->display,
        RootWindow(application->display, application->screen),
        0,
        0,
        1,
        1,
        0,
        requirements.depth,
        InputOutput,
        requirements.visual,
        requirements.attribute_mask,
        &requirements.attributes);
    if (application->window == None ||
        boxrig_x11_transparency_context_bind_window(
            application->transparency,
            application->window) != BOXRIG_UI_OK) {
        return -1;
    }
    application->host_clipboard_window = XCreateSimpleWindow(
        application->display,
        RootWindow(application->display, application->screen),
        0,
        0,
        1,
        1,
        0,
        0,
        0);
    if (application->host_clipboard_window == None) {
        return -1;
    }
    XStoreName(application->display, application->window, config->window_title);
    XSelectInput(
        application->display,
        application->window,
        ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
            ButtonReleaseMask | PointerMotionMask | FocusChangeMask |
            PropertyChangeMask | StructureNotifyMask |
            VisibilityChangeMask | EnterWindowMask | LeaveWindowMask);
    application->delete_window =
        XInternAtom(application->display, "WM_DELETE_WINDOW", False);
    application->wm_state =
        XInternAtom(application->display, "_NET_WM_STATE", False);
    application->wm_maximized_vertical =
        XInternAtom(application->display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    application->wm_maximized_horizontal =
        XInternAtom(application->display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    if (application->delete_window == None ||
        application->wm_state == None ||
        application->wm_maximized_vertical == None ||
        application->wm_maximized_horizontal == None ||
        boxrig_term_x11_wm_sync_initialize(
            &application->wm_sync,
            application->display,
            application->screen,
            application->window,
            application->delete_window) == -1) {
        return -1;
    }
    if (boxrig_x11_animation_poll_create(&application->animation_loop) != BOXRIG_UI_OK) {
        return -1;
    }
    component_config = boxrig_x11_terminal_component_config_default();
    component_config.animation_runtime =
        boxrig_ui_animation_poll_runtime(application->animation_loop);
    component_config.paste_capacity = config->terminal.clipboard_capacity;
    component_config.paste_chunk_size = config->terminal.output_capacity;
    component_config.scrollbar_presentation =
        BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED;
    component_config.transparency = component_transparency;
    borrowed_target.presentation_window = application->window;
    borrowed_target.input_window = application->window;
    host_policy.allow_title_change = config->host.allow_title_change;
    host_policy.allow_clipboard_write = config->host.allow_clipboard_write;
    host_policy.allow_clipboard_read = config->host.allow_clipboard_read;
    host_policy.allow_graphics_external =
        config->host.allow_graphics_external;
    host_policy.notification = config->host.notification;
    if (create_terminal_component(
            application,
            config,
            &borrowed_target,
            &component_config) != BOXRIG_UI_OK ||
        boxrig_term_font_demo_initialize(&application->font_demo,
            boxrig_x11_terminal_component_view(application->component),
            set_color_glyphs, application->component) == -1 ||
        boxrig_term_scrollbar_demo_initialize(&application->scrollbar_demo,
            boxrig_x11_terminal_component_view(application->component),
            set_scrollbar_policy, application->component) == -1 ||
            boxrig_term_text_policy_create(
            application->terminal,
            boxrig_x11_terminal_component_view(application->component),
            &application->text_policy) == -1 ||
        boxrig_term_uri_launcher_create(stderr, &application->uri_launcher) == -1 ||
        boxrig_term_link_gesture_create(application->terminal,
            boxrig_x11_terminal_component_view(application->component),
            application->text_policy, open_link, application,
            &application->link_gesture) == -1 ||
        boxrig_x11_terminal_component_get_effective_metrics(
            application->component,
            &metrics) != BOXRIG_FONT_CATALOG_OK ||
        boxrig_x11_terminal_component_get_scrollbar_visibility(
            application->component, &scrollbar) != BOXRIG_UI_OK ||
        !build_cell_resize_hints(
            &metrics,
            scrollbar.reserved_width,
            &hints) ||
        !cell_client_extent(
            &hints,
            config->terminal.rows,
            config->terminal.columns,
            &width,
            &height)) {
        return -1;
    }
    if ((boxrig_term_color_demo_initialize(
            &application->color_demo, boxrig_x11_terminal_component_view(application->component)) == -1 ||
            boxrig_term_decoration_demo_initialize(boxrig_x11_terminal_component_view(application->component)) == -1 ||
            boxrig_term_background_demo_initialize(boxrig_x11_terminal_component_view(application->component)) == -1)) {
        return -1;
    }
    XSetWMNormalHints(
        application->display,
        application->window,
        &hints);
    application->resize_hints_scrollbar_width = scrollbar.reserved_width;
    XResizeWindow(application->display, application->window, width, height);
    if (boxrig_term_x11_native_host_allocate(
            application,
            0,
            width,
            height) == -1 ||
        boxrig_x11_terminal_component_set_key_router(
            application->component,
            boxrig_term_x11_route_application_shortcut,
            application) != BOXRIG_UI_OK ||
        boxrig_x11_terminal_component_set_mouse_router(
            application->component,
            boxrig_term_x11_route_application_mouse,
            application) != BOXRIG_UI_OK ||
        boxrig_x11_host_adapter_create(
            application->display,
            &(boxrig_x11_host_targets){
                application->window,
                application->host_clipboard_window,
                application->window,
            },
            application->host_consumer,
            config->terminal.clipboard_capacity,
            &host_policy,
            &application->host_adapter) == -1 ||
        boxrig_x11_transparency_context_refresh(
            application->transparency,
            &transparency_changed) != BOXRIG_UI_OK ||
        boxrig_term_x11_native_host_synchronize_transparency(
            application) == -1 ||
        boxrig_x11_terminal_component_set_visible(
            application->component,
            true) != BOXRIG_UI_OK) {
        return -1;
    }
    boxrig_ui_background_effect_request effect_request;
    if (boxrig_term_background_effect_from_environment(&effect_request) == -1) return -1;
    if (effect_request != BOXRIG_UI_BACKGROUND_EFFECT_NONE) {
        XWindowAttributes attributes;
        if (!XGetWindowAttributes(application->display, application->window, &attributes) ||
            boxrig_x11_background_effect_create(application->display, application->window,
                &application->background_effect) != BOXRIG_UI_OK) return -1;
        boxrig_ui_background_effect_rectangle region = {0, 0, attributes.width, attributes.height};
        boxrig_ui_status status = boxrig_x11_background_effect_set(application->background_effect, effect_request, &region, 1);
        boxrig_ui_background_effect_state state = boxrig_x11_background_effect_get_state(application->background_effect);
        fprintf(stderr, "boxrig_term: background-effect=kde supported=%d accepted=%d reason=%d\n",
            state.supported, state.accepted, state.reason);
        if (status != BOXRIG_UI_OK) return -1;
    }
    return 0;
}

void boxrig_term_x11_native_host_show(
    boxrig_term_x11_application *application)
{
    XMapWindow(application->display, application->window);
    XFlush(application->display);
}

int boxrig_term_x11_update_pointer(boxrig_term_x11_application *application)
{
    if (application->text_policy == NULL || application->component == NULL ||
        application->window_destroyed || application->window == None) return 0;
    if (application->pointer_text == None)
        application->pointer_text = XCreateFontCursor(application->display, XC_xterm);
    if (application->pointer_link == None)
        application->pointer_link = XCreateFontCursor(application->display, XC_hand2);
    if (application->pointer_text == None || application->pointer_link == None) return -1;
    if (!application->pointer_inside || !boxrig_ui_view_focused(
            boxrig_x11_terminal_component_view(application->component)))
        boxrig_term_link_gesture_cancel(application->link_gesture);
    bool hand;
    int changed = boxrig_term_text_policy_refresh_pointer(application->text_policy,
        application->pointer_inside && boxrig_ui_view_focused(
            boxrig_x11_terminal_component_view(application->component)),
        application->pointer_x, application->pointer_y, &hand);
    if (changed < 0) return -1;
    if (!application->pointer_initialized || hand != application->pointer_hand) {
        XDefineCursor(application->display, application->window,
            hand ? application->pointer_link : application->pointer_text);
        application->pointer_hand = hand;
        application->pointer_initialized = true;
    }
    return changed;
}
