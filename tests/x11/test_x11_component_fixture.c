/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/x11.h"
#if BOXRIG_TEST_X11_OPENGL
#include "boxrig/x11/opengl_component.h"
#endif
#include "../frontend_component_fixture.h"
#include "../../examples/x11/external_scrollbar.h"
#include "../external_scrollbar_example.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct x11_fixture_host {
    Display *display;
    Display *requestor_display;
    Window parent;
    Window requestor;
} x11_fixture_host;

static Display *fixture_display;
static uint64_t fixture_presentation_hash;
static uint64_t fixture_presentation_generation;
static bool fixture_presentation_captured;

static bool take_window_event(
    Display *display,
    Window window,
    int type,
    XEvent *event_out)
{
    XEvent event;

    XSync(display, False);
    while (XCheckTypedWindowEvent(display, window, type, &event)) {
        *event_out = event;
        return true;
    }
    return false;
}

static bool take_atom_property_event(
    Display *display,
    Window window,
    Atom atom,
    int state,
    XEvent *event_out)
{
    XEvent event;

    XSync(display, False);
    while (XCheckTypedWindowEvent(
        display,
        window,
        PropertyNotify,
        &event)) {
        if (event.xproperty.atom == atom &&
            event.xproperty.state == state) {
            *event_out = event;
            return true;
        }
    }
    return false;
}

static void *create_host(void)
{
    x11_fixture_host *host = calloc(1, sizeof(*host));
    int screen;

    if (host == NULL) {
        return NULL;
    }
    host->display = XOpenDisplay(NULL);
    host->requestor_display = XOpenDisplay(NULL);
    if (host->display == NULL || host->requestor_display == NULL) {
        if (host->display != NULL) {
            XCloseDisplay(host->display);
        }
        if (host->requestor_display != NULL) {
            XCloseDisplay(host->requestor_display);
        }
        free(host);
        return NULL;
    }
    screen = DefaultScreen(host->display);
    host->parent = XCreateSimpleWindow(
        host->display,
        RootWindow(host->display, screen),
        0,
        0,
        320,
        160,
        0,
        0,
        0);
    screen = DefaultScreen(host->requestor_display);
    host->requestor = XCreateSimpleWindow(
        host->requestor_display,
        RootWindow(host->requestor_display, screen),
        0,
        0,
        8,
        8,
        0,
        0,
        0);
    if (host->parent == None || host->requestor == None) {
        if (host->parent != None) {
            XDestroyWindow(host->display, host->parent);
        }
        if (host->requestor != None) {
            XDestroyWindow(host->requestor_display, host->requestor);
        }
        XCloseDisplay(host->requestor_display);
        XCloseDisplay(host->display);
        free(host);
        return NULL;
    }
    XSelectInput(
        host->requestor_display,
        host->requestor,
        PropertyChangeMask | StructureNotifyMask);
    XMapWindow(host->display, host->parent);
    XSync(host->display, False);
    XSync(host->requestor_display, False);
    fixture_display = host->display;
    fixture_presentation_hash = 0;
    fixture_presentation_generation = 0;
    fixture_presentation_captured = false;
    return host;
}

static void destroy_host(void *opaque_host)
{
    x11_fixture_host *host = opaque_host;

    if (host == NULL) {
        return;
    }
    if (host->requestor != None) {
        XDestroyWindow(host->requestor_display, host->requestor);
    }
    if (host->parent != None) {
        XDestroyWindow(host->display, host->parent);
    }
    XSync(host->requestor_display, False);
    XSync(host->display, False);
    XCloseDisplay(host->requestor_display);
    XCloseDisplay(host->display);
    fixture_display = NULL;
    free(host);
}

static boxrig_ui_status create_component(
    void *opaque_host,
    boxrig_ui_model *model,
    boxrig_ui_component_attachment attachment,
    void **component_out)
{
    x11_fixture_host *host = opaque_host;
    boxrig_x11_terminal_component_config config =
        boxrig_x11_terminal_component_config_default();
    config.view.blink.text_enabled = false;
    config.view.blink.cursor_enabled = false;
    boxrig_x11_terminal_component *component = NULL;
    boxrig_ui_status status;

    config.attachment = attachment;
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    config.paste_chunk_size = 4;
    config.selection_transfer_chunk_size = 64;
#if BOXRIG_TEST_X11_OPENGL
    status = boxrig_x11_opengl_terminal_component_create_child(
#else
    status = boxrig_x11_terminal_component_create_child(
#endif
        host->display,
        host->parent,
        model,
        &config,
        &component);
    *component_out = component;
    return status;
}

static void destroy_component(void *component)
{
    boxrig_x11_terminal_component_destroy(component);
}

static boxrig_ui_status quiesce_component(void *component)
{
    return boxrig_x11_terminal_component_quiesce(component);
}

static boxrig_ui_view *component_view(void *component)
{
    return boxrig_x11_terminal_component_view(component);
}

static bool get_effective_metrics(
    void *component,
    boxrig_font_effective_metrics *metrics_out)
{
    return boxrig_x11_terminal_component_get_effective_metrics(
               component,
               metrics_out) == BOXRIG_FONT_CATALOG_OK;
}

static boxrig_ui_status allocate_component(
    void *component,
    const boxrig_ui_component_allocation *allocation,
    boxrig_ui_layout_result *layout_out)
{
    return boxrig_x11_terminal_component_allocate(
        component,
        allocation,
        layout_out);
}

static boxrig_ui_status set_preferred_grid(
    void *component,
    size_t rows,
    size_t columns)
{
    return boxrig_x11_terminal_component_set_preferred_grid(
        component,
        rows,
        columns);
}

static bool get_preferred_grid(
    void *component,
    size_t *rows_out,
    size_t *columns_out)
{
    return boxrig_x11_terminal_component_get_preferred_grid(
               component,
               rows_out,
               columns_out) == BOXRIG_UI_OK;
}

static boxrig_ui_size_command_state get_size_command_state(void *component)
{
    boxrig_ui_size_command_state state = BOXRIG_UI_SIZE_COMMAND_IDLE;

    (void)boxrig_x11_terminal_component_get_size_command_state(
        component,
        &state);
    return state;
}

static bool take_layout_action(
    void *component,
    boxrig_ui_layout_action *action_out)
{
    return boxrig_x11_terminal_component_take_layout_action(
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
    return boxrig_x11_terminal_component_complete_size_command(
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
    return boxrig_x11_terminal_component_complete_grid_resize(
        component, revision, succeeded);
}

static boxrig_ui_status set_visible(void *component, bool visible)
{
    return boxrig_x11_terminal_component_set_visible(component, visible);
}

static boxrig_ui_status set_frame_request_callback(
    void *component,
    boxrig_ui_frame_request_callback callback,
    void *context)
{
    return boxrig_x11_terminal_component_set_frame_request_callback(
        component,
        callback,
        context);
}

static boxrig_ui_status frame_component(
    void *component,
    boxrig_ui_component_frame_result *result_out)
{
    return boxrig_x11_terminal_component_frame(component, result_out);
}

static boxrig_ui_status queue_paste(
    void *component,
    const uint8_t *bytes,
    size_t length)
{
    return boxrig_x11_terminal_component_queue_paste(
        component,
        bytes,
        length);
}

static bool run_native_input_transcript(
    void *opaque_component,
    const boxrig_test_frontend_input_script *script,
    boxrig_test_frontend_input_observation *observation_out)
{
    boxrig_x11_terminal_component *component = opaque_component;
    Display *display;
    Window window;
    XEvent event;
    KeyCode keycode;
    int x;
    int y;
    bool passed = false;

    if (component == NULL || script == NULL || observation_out == NULL ||
        script->x % BOXRIG_UI_FIXED_ONE != 0 ||
        script->y % BOXRIG_UI_FIXED_ONE != 0 ||
        script->x / BOXRIG_UI_FIXED_ONE > INT_MAX ||
        script->y / BOXRIG_UI_FIXED_ONE > INT_MAX) {
        return false;
    }
    display = fixture_display;
    window = boxrig_x11_terminal_component_input_window(component);
    x = (int)(script->x / BOXRIG_UI_FIXED_ONE);
    y = (int)(script->y / BOXRIG_UI_FIXED_ONE);
    if (display == NULL || window == None ||
        boxrig_x11_terminal_component_set_key_router(
            component,
            boxrig_test_frontend_capture_key,
            observation_out) != BOXRIG_UI_OK ||
        boxrig_x11_terminal_component_set_mouse_router(
            component,
            boxrig_test_frontend_capture_mouse,
            observation_out) != BOXRIG_UI_OK) {
        goto cleanup;
    }

    memset(&event, 0, sizeof(event));
    event.xfocus.display = display;
    event.xfocus.window = window;
    event.xfocus.type = FocusIn;
    event.xfocus.mode = NotifyNormal;
    event.xfocus.detail = NotifyNonlinear;
    if (boxrig_x11_terminal_component_dispatch_event(component, &event) !=
        BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
        goto cleanup;
    }
    observation_out->focused_after_enter =
        boxrig_x11_terminal_component_focused(component);

    keycode = XKeysymToKeycode(display, XK_F1);
    if (keycode == 0) {
        goto cleanup;
    }
    memset(&event, 0, sizeof(event));
    event.xkey.display = display;
    event.xkey.window = window;
    event.xkey.root = DefaultRootWindow(display);
    event.xkey.same_screen = True;
    event.xkey.keycode = keycode;
    event.xkey.type = KeyPress;
    event.xkey.time = 1;
    if (boxrig_x11_terminal_component_dispatch_event(component, &event) !=
        BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
        goto cleanup;
    }
    event.xkey.time = 2;
    if (boxrig_x11_terminal_component_dispatch_event(component, &event) !=
        BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
        goto cleanup;
    }
    event.xkey.type = KeyRelease;
    event.xkey.time = 3;
    if (boxrig_x11_terminal_component_dispatch_event(component, &event) !=
        BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
        goto cleanup;
    }

    memset(&event, 0, sizeof(event));
    event.xbutton.display = display;
    event.xbutton.window = window;
    event.xbutton.root = DefaultRootWindow(display);
    event.xbutton.same_screen = True;
    event.xbutton.type = ButtonPress;
    event.xbutton.button = Button1;
    event.xbutton.state = ControlMask;
    event.xbutton.x = x;
    event.xbutton.y = y;
    event.xbutton.time = 10;
    if (boxrig_x11_terminal_component_dispatch_event(component, &event) !=
        BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
        goto cleanup;
    }
    event.xmotion.type = MotionNotify;
    event.xmotion.state = Button1Mask | ShiftMask;
    event.xmotion.time = 11;
    if (boxrig_x11_terminal_component_dispatch_event(component, &event) !=
        BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
        goto cleanup;
    }
    event.xbutton.type = ButtonRelease;
    event.xbutton.button = Button1;
    event.xbutton.state = Button1Mask;
    event.xbutton.time = 12;
    if (boxrig_x11_terminal_component_dispatch_event(component, &event) !=
        BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
        goto cleanup;
    }

    memset(&event, 0, sizeof(event));
    event.xfocus.display = display;
    event.xfocus.window = window;
    event.xfocus.type = FocusOut;
    event.xfocus.mode = NotifyNormal;
    event.xfocus.detail = NotifyNonlinear;
    if (boxrig_x11_terminal_component_dispatch_event(component, &event) !=
        BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
        goto cleanup;
    }
    observation_out->focused_after_leave =
        boxrig_x11_terminal_component_focused(component);
    passed = true;

cleanup:
    if (boxrig_x11_terminal_component_set_key_router(
            component,
            NULL,
            NULL) != BOXRIG_UI_OK ||
        boxrig_x11_terminal_component_set_mouse_router(
            component,
            NULL,
            NULL) != BOXRIG_UI_OK) {
        passed = false;
    }
    return passed;
}

static boxrig_font_catalog_status register_font_file(
    void *component,
    const char *path,
    size_t path_length,
    boxrig_font_resource_id *resource_id_out)
{
    return boxrig_x11_terminal_component_register_font_file(
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
    return boxrig_x11_terminal_component_set_font_specification(
        component,
        specification);
}

static boxrig_ui_status get_font_specification(
    void *component,
    boxrig_font_specification *specification_out)
{
    return boxrig_x11_terminal_component_get_font_specification(
        component,
        specification_out);
}

static boxrig_font_resource_id primary_font_resource(void *component)
{
    return boxrig_x11_terminal_component_primary_font_resource(component);
}

static uint64_t font_catalog_revision(void *component)
{
    return boxrig_x11_terminal_component_font_catalog_revision(component);
}

static boxrig_ui_status set_line_height(
    void *component,
    float adjustment_dip)
{
    return boxrig_x11_terminal_component_set_line_height_adjustment(
        component,
        adjustment_dip);
}

static boxrig_ui_status get_line_height(
    void *component,
    float *adjustment_dip_out)
{
    return boxrig_x11_terminal_component_get_line_height_adjustment(
        component,
        adjustment_dip_out);
}

static boxrig_ui_status visual_zoom(
    void *component,
    boxrig_font_zoom_direction direction,
    boxrig_font_visual_step_policy policy,
    bool *changed_out)
{
    return boxrig_x11_terminal_component_visual_zoom(
        component,
        direction,
        policy,
        changed_out);
}

static boxrig_ui_status visual_zoom_reset(
    void *component,
    bool *changed_out)
{
    return boxrig_x11_terminal_component_visual_zoom_reset(
        component,
        changed_out);
}

static boxrig_ui_status set_text_direction(
    void *component,
    boxrig_text_direction_policy policy)
{
    return boxrig_x11_terminal_component_set_text_direction(
        component,
        policy);
}

static boxrig_text_direction_policy text_direction(void *component)
{
    return boxrig_x11_terminal_component_text_direction(component);
}

static bool copy_selection(
    void *opaque_host,
    void *opaque_component,
    const uint8_t *expected,
    size_t expected_length)
{
    x11_fixture_host *host = opaque_host;
    boxrig_x11_terminal_component *component = opaque_component;
    boxrig_ui_view_action_capabilities capabilities;
    boxrig_ui_view_action action;
    Window input_window;
    XEvent event;
    XEvent notify;
    Atom timestamp_property;
    Atom clipboard;
    Atom utf8_string;
    Atom property;
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *value = NULL;
    bool matched;

    capabilities =
        boxrig_x11_terminal_component_view_action_capabilities(component);
    if ((capabilities.copy_targets &
         BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD) == 0) {
        return false;
    }
    action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    if (boxrig_x11_terminal_component_apply_view_action(
            component,
            &action) != BOXRIG_UI_OK) {
        return false;
    }
    input_window = boxrig_x11_terminal_component_input_window(component);
    timestamp_property =
        XInternAtom(host->display, "_BOXRIG_TIMESTAMP", False);
    if (input_window == None || timestamp_property == None ||
        !take_atom_property_event(
            host->display,
            input_window,
            timestamp_property,
            PropertyNewValue,
            &event) ||
        boxrig_x11_terminal_component_dispatch_event(component, &event) !=
            BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
        return false;
    }
    XSync(host->display, False);

    clipboard =
        XInternAtom(host->requestor_display, "CLIPBOARD", False);
    utf8_string =
        XInternAtom(host->requestor_display, "UTF8_STRING", False);
    property = XInternAtom(
        host->requestor_display,
        "_BOXRIG_COMPONENT_FIXTURE",
        False);
    if (clipboard == None || utf8_string == None || property == None ||
        XGetSelectionOwner(host->requestor_display, clipboard) !=
            input_window) {
        return false;
    }
    XConvertSelection(
        host->requestor_display,
        clipboard,
        utf8_string,
        property,
        host->requestor,
        CurrentTime);
    XSync(host->requestor_display, False);
    if (!take_window_event(
            host->display,
            input_window,
            SelectionRequest,
            &event) ||
        boxrig_x11_terminal_component_dispatch_event(component, &event) !=
            BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
        return false;
    }
    XSync(host->display, False);
    if (!take_window_event(
            host->requestor_display,
            host->requestor,
            SelectionNotify,
            &notify) ||
        notify.xselection.property != property ||
        XGetWindowProperty(
            host->requestor_display,
            host->requestor,
            property,
            0,
            64,
            True,
            AnyPropertyType,
            &actual_type,
            &actual_format,
            &item_count,
            &bytes_after,
            &value) != Success) {
        return false;
    }
    matched = actual_type == utf8_string && actual_format == 8 &&
              bytes_after == 0 && item_count == expected_length &&
              value != NULL && memcmp(value, expected, expected_length) == 0;
    if (value != NULL) {
        XFree(value);
    }
    return matched;
}

static boxrig_ui_status set_transparency(
    void *component,
    const boxrig_ui_transparency_config *config)
{
    return boxrig_x11_terminal_component_set_transparency(component, config);
}

static boxrig_ui_status get_transparency(
    void *component,
    boxrig_ui_transparency_state *state_out)
{
    return boxrig_x11_terminal_component_get_transparency(
        component,
        state_out);
}

static uint32_t capabilities(void *component)
{
    return boxrig_x11_terminal_component_capabilities(component);
}

static boxrig_ui_status get_diagnostic(
    void *component,
    boxrig_ui_component_diagnostic *diagnostic_out)
{
    return boxrig_x11_terminal_component_get_diagnostic(
        component,
        diagnostic_out);
}

static boxrig_composition *component_composition(void *component)
{
    return boxrig_x11_terminal_component_composition(component);
}

static bool observe_presentation(
    void *opaque_component,
    const boxrig_test_frontend_presentation_probe *probe,
    boxrig_test_frontend_presentation_observation *observation_out)
{
    boxrig_x11_terminal_component *component = opaque_component;
    Window window;
    XWindowAttributes attributes;
    XImage *image;
    Colormap colormap;
    XColor background;
    XColor meaning;
    unsigned long background_pixel;
    unsigned long meaning_pixel;
    uint64_t full_hash = UINT64_C(1469598103934665603);
    uint64_t stable_hash = UINT64_C(1469598103934665603);
    size_t coverage = 0;
    unsigned int x;
    unsigned int y;

    if (component == NULL || probe == NULL || observation_out == NULL) {
        return false;
    }
    window = boxrig_x11_terminal_component_owned_window(component);
    if (window == None || XGetWindowAttributes(
                              fixture_display,
                              window,
                              &attributes) == 0 ||
        attributes.width <= 0 || attributes.height <= 0 ||
        probe->background_x < 0 || probe->background_y < 0 ||
        probe->meaning_x < 0 || probe->meaning_y < 0 ||
        probe->coverage_x < 0 || probe->coverage_y < 0 ||
        probe->stable_x < 0 || probe->stable_y < 0 ||
        probe->background_x >= attributes.width ||
        probe->background_y >= attributes.height ||
        probe->meaning_x >= attributes.width ||
        probe->meaning_y >= attributes.height ||
        probe->coverage_x >= attributes.width ||
        probe->coverage_y >= attributes.height ||
        probe->stable_x >= attributes.width ||
        probe->stable_y >= attributes.height ||
        probe->coverage_width == 0 || probe->coverage_height == 0 ||
        probe->stable_width == 0 || probe->stable_height == 0 ||
        probe->coverage_width >
            (unsigned int)(attributes.width - probe->coverage_x) ||
        probe->coverage_height >
            (unsigned int)(attributes.height - probe->coverage_y) ||
        probe->stable_width >
            (unsigned int)(attributes.width - probe->stable_x) ||
        probe->stable_height >
            (unsigned int)(attributes.height - probe->stable_y)) {
        return false;
    }
    XSync(fixture_display, False);
    image = XGetImage(
        fixture_display,
        window,
        0,
        0,
        (unsigned int)attributes.width,
        (unsigned int)attributes.height,
        AllPlanes,
        ZPixmap);
    if (image == NULL) {
        return false;
    }
    background_pixel = XGetPixel(
        image,
        probe->background_x,
        probe->background_y);
    meaning_pixel = XGetPixel(image, probe->meaning_x, probe->meaning_y);
    for (y = 0; y < (unsigned int)attributes.height; y++) {
        for (x = 0; x < (unsigned int)attributes.width; x++) {
            full_hash ^= (uint64_t)XGetPixel(image, (int)x, (int)y);
            full_hash *= UINT64_C(1099511628211);
        }
    }
    for (y = 0; y < probe->coverage_height; y++) {
        for (x = 0; x < probe->coverage_width; x++) {
            if (XGetPixel(
                    image,
                    probe->coverage_x + (int)x,
                    probe->coverage_y + (int)y) != background_pixel) {
                coverage++;
            }
        }
    }
    for (y = 0; y < probe->stable_height; y++) {
        for (x = 0; x < probe->stable_width; x++) {
            stable_hash ^= (uint64_t)XGetPixel(
                image,
                probe->stable_x + (int)x,
                probe->stable_y + (int)y);
            stable_hash *= UINT64_C(1099511628211);
        }
    }
    XDestroyImage(image);
    colormap = attributes.colormap != None
                   ? attributes.colormap
                   : DefaultColormap(
                         fixture_display,
                         DefaultScreen(fixture_display));
    background.pixel = background_pixel;
    meaning.pixel = meaning_pixel;
    if (!XQueryColor(fixture_display, colormap, &background) ||
        !XQueryColor(fixture_display, colormap, &meaning)) {
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
    observation_out->width = (unsigned int)attributes.width;
    observation_out->height = (unsigned int)attributes.height;
    observation_out->background_argb =
        UINT32_C(0xff000000) | ((uint32_t)(background.red >> 8) << 16) |
        ((uint32_t)(background.green >> 8) << 8) |
        (uint32_t)(background.blue >> 8);
    observation_out->meaning_argb =
        UINT32_C(0xff000000) | ((uint32_t)(meaning.red >> 8) << 16) |
        ((uint32_t)(meaning.green >> 8) << 8) |
        (uint32_t)(meaning.blue >> 8);
    observation_out->coverage_pixels = coverage;
    observation_out->stable_hash = stable_hash;
    observation_out->alpha_observable = false;
    return true;
}

static boxrig_ui_view_action_capabilities view_action_capabilities(
    void *component)
{
    return boxrig_x11_terminal_component_view_action_capabilities(component);
}

static boxrig_ui_status apply_view_action(
    void *component,
    const boxrig_ui_view_action *action)
{
    return boxrig_x11_terminal_component_apply_view_action(
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
    return boxrig_x11_terminal_component_get_scrollbar_model(
               component,
               model_out) == BOXRIG_UI_OK;
}

static boxrig_ui_status apply_scrollbar_action(
    void *component,
    boxrig_scrollbar_action action,
    size_t position,
    bool *changed_out)
{
    return boxrig_x11_terminal_component_apply_scrollbar_action(
        component,
        action,
        position,
        changed_out);
}

int main(int argc, char **argv)
{
    const boxrig_test_frontend_component_operations operations = {
        .frontend_name = "X11",
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
    bool passed;

    if (argc != 2) {
        fprintf(stderr, "usage: test_x11_component_fixture FONT_FILE\n");
        return 2;
    }
    passed = boxrig_test_run_frontend_component_fixture(&operations);

    cairo_debug_reset_static_data();
    FcFini();
    return passed ? 0 : 1;
}
