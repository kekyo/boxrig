/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "boxrig/x11.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

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
    boxrig_x11_terminal_component *component,
    boxrig_ui_size_command *command_out)
{
    boxrig_ui_layout_action action;

    if (!boxrig_x11_terminal_component_take_layout_action(
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

static bool take_grid_resize_action(
    boxrig_x11_terminal_component *component,
    boxrig_ui_grid_resize *resize_out)
{
    boxrig_ui_layout_action action;

    if (!boxrig_x11_terminal_component_take_layout_action(
            component, &action) ||
        action.kind != BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
        return false;
    }
    resize_out->revision = action.revision;
    resize_out->rows = action.value.grid_resize.rows;
    resize_out->columns = action.value.grid_resize.columns;
    return true;
}

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

static bool take_property_event(
    Display *display,
    Window window,
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
        if (event.xproperty.state == state) {
            *event_out = event;
            return true;
        }
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

static uint8_t *read_font_file(const char *path, size_t *length_out)
{
    FILE *file = fopen(path, "rb");
    uint8_t *data;
    long length;

    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        return NULL;
    }
    length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = malloc((size_t)length);
    if (data == NULL ||
        fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *length_out = (size_t)length;
    return data;
}

static void count_frame_request(void *context)
{
    size_t *count = context;

    (*count)++;
}

static bool metrics_match(
    const boxrig_font_effective_metrics *left,
    const boxrig_font_effective_metrics *right)
{
    return left->requested_size_dip == right->requested_size_dip &&
           left->primary_resource_id == right->primary_resource_id &&
           left->primary_face_key == right->primary_face_key &&
           left->dpi_x == right->dpi_x && left->dpi_y == right->dpi_y &&
           left->output_scale == right->output_scale &&
           left->glyph_width == right->glyph_width &&
           left->glyph_height == right->glyph_height &&
           left->ascent == right->ascent &&
           left->descent == right->descent &&
           left->line_gap == right->line_gap &&
           left->line_height_adjustment == right->line_height_adjustment &&
           left->cell_width == right->cell_width &&
           left->cell_height == right->cell_height &&
           left->baseline == right->baseline &&
           left->layout_revision == right->layout_revision;
}

static bool window_is_valid(Display *display, Window window)
{
    XWindowAttributes attributes;

    return XGetWindowAttributes(display, window, &attributes) != 0;
}

static bool read_window_hash(
    Display *display,
    Window window,
    unsigned int width,
    unsigned int height,
    uint64_t *hash_out)
{
    XImage *image;
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned int x;
    unsigned int y;

    if (width == 0 || height == 0 || hash_out == NULL) {
        return false;
    }
    XSync(display, False);
    image = XGetImage(
        display,
        window,
        0,
        0,
        width,
        height,
        AllPlanes,
        ZPixmap);
    if (image == NULL) {
        return false;
    }
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            hash ^= (uint64_t)XGetPixel(image, (int)x, (int)y);
            hash *= UINT64_C(1099511628211);
        }
    }
    XDestroyImage(image);
    *hash_out = hash;
    return true;
}

static bool read_window_region_hash(
    Display *display,
    Window window,
    int origin_x,
    int origin_y,
    unsigned int width,
    unsigned int height,
    uint64_t *hash_out)
{
    XImage *image;
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned int x;
    unsigned int y;

    if (origin_x < 0 || origin_y < 0 || width == 0 || height == 0 ||
        hash_out == NULL) {
        return false;
    }
    XSync(display, False);
    image = XGetImage(
        display,
        window,
        origin_x,
        origin_y,
        width,
        height,
        AllPlanes,
        ZPixmap);
    if (image == NULL) {
        return false;
    }
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            hash ^= (uint64_t)XGetPixel(image, (int)x, (int)y);
            hash *= UINT64_C(1099511628211);
        }
    }
    XDestroyImage(image);
    *hash_out = hash;
    return true;
}

static bool read_window_pixel(
    Display *display,
    Window window,
    int x,
    int y,
    unsigned long *pixel_out)
{
    XImage *image;

    if (x < 0 || y < 0 || pixel_out == NULL) {
        return false;
    }
    XSync(display, False);
    image = XGetImage(
        display,
        window,
        x,
        y,
        1,
        1,
        AllPlanes,
        ZPixmap);
    if (image == NULL) {
        return false;
    }
    *pixel_out = XGetPixel(image, 0, 0);
    XDestroyImage(image);
    return true;
}

static bool child_has_parent(
    Display *display,
    Window child,
    Window expected_parent)
{
    Window root = None;
    Window parent = None;
    Window *children = NULL;
    unsigned int child_count = 0;
    bool matches;

    CHECK(XQueryTree(
              display,
              child,
              &root,
              &parent,
              &children,
              &child_count) != 0);
    matches = parent == expected_parent;
    if (children != NULL) {
        XFree(children);
    }
    return matches;
}

static bool only_child_window(
    Display *display,
    Window parent,
    Window *child_out)
{
    Window root = None;
    Window queried_parent = None;
    Window *children = NULL;
    unsigned int child_count = 0;

    CHECK(child_out != NULL);
    CHECK(XQueryTree(
              display,
              parent,
              &root,
              &queried_parent,
              &children,
              &child_count) != 0);
    CHECK(child_count == 1 && children != NULL);
    *child_out = children[0];
    XFree(children);
    return true;
}

static bool component_owns_only_its_child(Display *display)
{
    int screen = DefaultScreen(display);
    Window parent = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        640,
        360,
        0,
        BlackPixel(display, screen),
        WhitePixel(display, screen));
    Window sibling = XCreateSimpleWindow(
        display,
        parent,
        500,
        20,
        100,
        40,
        0,
        BlackPixel(display, screen),
        WhitePixel(display, screen));
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_x11_terminal_component_config config;
    boxrig_x11_terminal_component *authority = NULL;
    boxrig_x11_terminal_component *presentation = NULL;
    boxrig_x11_terminal_component *conflict = NULL;
    boxrig_font_effective_metrics metrics;
    boxrig_font_effective_metrics presentation_before;
    boxrig_font_effective_metrics presentation_after;
    boxrig_font_rendering_policy conflicting_policy;
    boxrig_ui_component_allocation allocation;
    boxrig_ui_component_frame_result frame;
    boxrig_ui_layout_result layout;
    boxrig_ui_dirty dirty;
    XWindowAttributes attributes;
    XEvent event;
    Window child;

    CHECK(parent != None && sibling != None);
    terminal_config.rows = 4;
    terminal_config.columns = 12;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);

    config = boxrig_x11_terminal_component_config_default();
    config.view.blink.text_enabled = false;
    config.view.blink.cursor_enabled = false;
    CHECK(boxrig_x11_terminal_component_create_child(
              display,
              parent,
              model,
              &config,
              &authority) == BOXRIG_UI_OK);
    CHECK(authority != NULL);
    child = boxrig_x11_terminal_component_owned_window(authority);
    CHECK(child != None && child_has_parent(display, child, parent));
    CHECK(boxrig_x11_terminal_component_native_hosting(authority) ==
          BOXRIG_UI_COMPONENT_OWNED_CHILD);
    CHECK(boxrig_x11_terminal_component_presentation_window(authority) ==
          child);
    CHECK(boxrig_x11_terminal_component_input_window(authority) == child);
    CHECK(boxrig_x11_terminal_component_input_window_owned(authority));
    CHECK(boxrig_x11_terminal_component_scrollbar_window(authority) != None);
    CHECK(boxrig_x11_terminal_component_model(authority) == model);
    CHECK(boxrig_ui_view_model(
              boxrig_x11_terminal_component_view(authority)) == model);
    CHECK(boxrig_ui_view_is_layout_authority(
        boxrig_x11_terminal_component_view(authority)));

    CHECK(boxrig_x11_terminal_component_create_child(
              display,
              parent,
              model,
              &config,
              &conflict) == BOXRIG_UI_UNAVAILABLE);
    CHECK(conflict == NULL);
    config.attachment = BOXRIG_UI_COMPONENT_PRESENTATION_ONLY;
    CHECK(boxrig_x11_terminal_component_create_child(
              display,
              parent,
              model,
              &config,
              &presentation) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_is_layout_authority(
        boxrig_x11_terminal_component_view(presentation)));

    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              authority,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.x = 20;
    allocation.y = 30;
    allocation.width = metrics.cell_width * 10 + 14;
    allocation.height = metrics.cell_height * 4;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_x11_terminal_component_allocate(
              authority,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(layout.rows == 4 && layout.columns == 10);
    CHECK(XGetWindowAttributes(display, child, &attributes) != 0);
    CHECK(attributes.x == allocation.x && attributes.y == allocation.y);
    CHECK((unsigned int)attributes.width == allocation.width);
    CHECK((unsigned int)attributes.height == allocation.height);

    allocation.x = 260;
    CHECK(boxrig_x11_terminal_component_allocate(
              presentation,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_set_visible(authority, true) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_set_visible(presentation, true) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_visible(authority));
    XMapWindow(display, parent);
    XMapWindow(display, sibling);
    XSync(display, False);

    CHECK(feed_text(terminal, "first component"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_peek_dirty(
        boxrig_x11_terminal_component_view(authority),
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));
    CHECK(boxrig_ui_view_peek_dirty(
        boxrig_x11_terminal_component_view(presentation),
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));
    CHECK(boxrig_x11_terminal_component_frame(authority, &frame) ==
          BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_peek_dirty(
        boxrig_x11_terminal_component_view(authority),
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));
    CHECK(boxrig_ui_view_peek_dirty(
        boxrig_x11_terminal_component_view(presentation),
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));

    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              presentation,
              &presentation_before) == BOXRIG_FONT_CATALOG_OK);
    conflicting_policy = boxrig_font_rendering_policy_default();
    conflicting_policy.dpi_x = 144.0f;
    conflicting_policy.dpi_y = 144.0f;
    CHECK(boxrig_x11_terminal_component_set_font_rendering_policy(
              presentation,
              &conflicting_policy) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              presentation,
              &presentation_after) == BOXRIG_FONT_CATALOG_OK);
    CHECK(metrics_match(&presentation_before, &presentation_after));

    memset(&event, 0, sizeof(event));
    event.type = Expose;
    event.xexpose.display = display;
    event.xexpose.window = sibling;
    event.xexpose.width = 10;
    event.xexpose.height = 10;
    CHECK(boxrig_x11_terminal_component_dispatch_event(authority, &event) ==
          BOXRIG_UI_COMPONENT_EVENT_UNRELATED);
    event.xexpose.window = child;
    CHECK(boxrig_x11_terminal_component_dispatch_event(authority, &event) ==
          BOXRIG_UI_COMPONENT_EVENT_HANDLED);

    CHECK(boxrig_x11_terminal_component_quiesce(authority) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_dispatch_event(authority, &event) ==
          BOXRIG_UI_COMPONENT_EVENT_UNRELATED);
    boxrig_x11_terminal_component_destroy(authority);
    authority = NULL;
    XSync(display, False);
    CHECK(window_is_valid(display, parent));
    CHECK(window_is_valid(display, sibling));
    CHECK(window_is_valid(
        display,
        boxrig_x11_terminal_component_owned_window(presentation)));
    CHECK(feed_text(terminal, " after destroy"));

    boxrig_x11_terminal_component_destroy(presentation);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, sibling);
    XDestroyWindow(display, parent);
    XSync(display, False);
    return true;
}

static bool component_font_and_size_transactions(
    Display *display,
    const char *font_path)
{
    int screen = DefaultScreen(display);
    Window parent = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        640,
        480,
        0,
        BlackPixel(display, screen),
        WhitePixel(display, screen));
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_x11_terminal_component_config config =
        boxrig_x11_terminal_component_config_default();
    config.view.blink.text_enabled = false;
    config.view.blink.cursor_enabled = false;
    boxrig_x11_terminal_component *component = NULL;
    boxrig_font_specification specification;
    boxrig_font_specification invalid_specification;
    boxrig_font_rendering_policy policy;
    boxrig_font_rendering_policy changed_policy;
    boxrig_font_effective_metrics initial_metrics;
    boxrig_font_effective_metrics resource_metrics;
    boxrig_font_effective_metrics after_invalid;
    boxrig_font_effective_metrics changed_metrics;
    boxrig_font_effective_metrics line_height_metrics;
    boxrig_font_effective_metrics zoom_baseline;
    boxrig_font_effective_metrics zoomed_metrics;
    boxrig_font_effective_metrics reset_metrics;
    boxrig_font_effective_metrics canonical_metrics;
    boxrig_font_visual_step_policy zoom_policy;
    boxrig_font_family resource_family;
    boxrig_font_resource_id file_resource = BOXRIG_FONT_RESOURCE_NONE;
    boxrig_font_resource_id memory_resource = BOXRIG_FONT_RESOURCE_NONE;
    uint8_t *font_data = NULL;
    size_t font_length = 0;
    uint64_t catalog_revision;
    float line_height = 0.0f;
    bool zoom_changed = false;
    boxrig_ui_component_allocation allocation;
    boxrig_ui_layout_result layout;
    boxrig_ui_size_command request;
    boxrig_ui_size_command_state command_state;
    boxrig_ui_grid_resize resize;
    size_t preferred_rows = 0;
    size_t preferred_columns = 0;
    size_t terminal_rows = 0;
    size_t terminal_columns = 0;
    bool canonical_set = false;
    bool applied = false;

    CHECK(parent != None);
    terminal_config.rows = 4;
    terminal_config.columns = 12;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    CHECK(boxrig_x11_terminal_component_create_child(
              display,
              parent,
              model,
              &config,
              &component) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_get_font_specification(
              component,
              &specification) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_get_font_rendering_policy(
              component,
              &policy) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              component,
              &initial_metrics) == BOXRIG_FONT_CATALOG_OK);
    catalog_revision =
        boxrig_x11_terminal_component_font_catalog_revision(component);
    CHECK(catalog_revision != 0);
    CHECK(boxrig_x11_terminal_component_register_font_file(
              component,
              font_path,
              strlen(font_path),
              0,
              &file_resource) == BOXRIG_FONT_CATALOG_OK);
    CHECK(file_resource != BOXRIG_FONT_RESOURCE_NONE);
    CHECK(boxrig_x11_terminal_component_font_catalog_revision(component) ==
          catalog_revision + 1);
    font_data = read_font_file(font_path, &font_length);
    CHECK(font_data != NULL);
    CHECK(boxrig_x11_terminal_component_register_font_memory(
              component,
              font_data,
              font_length,
              0,
              &memory_resource) == BOXRIG_FONT_CATALOG_OK);
    free(font_data);
    font_data = NULL;
    CHECK(memory_resource != BOXRIG_FONT_RESOURCE_NONE &&
          memory_resource != file_resource);
    CHECK(boxrig_x11_terminal_component_primary_font_resource(component) ==
          BOXRIG_FONT_RESOURCE_NONE);
    memset(&resource_family, 0, sizeof(resource_family));
    resource_family.resource_id = file_resource;
    specification.families = &resource_family;
    specification.family_count = 1;
    CHECK(boxrig_x11_terminal_component_set_font_specification(
              component,
              &specification) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_primary_font_resource(component) ==
          file_resource);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              component,
              &resource_metrics) == BOXRIG_FONT_CATALOG_OK);

    memset(&allocation, 0, sizeof(allocation));
    allocation.width = resource_metrics.cell_width * terminal_config.columns;
    allocation.height = resource_metrics.cell_height * terminal_config.rows;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_x11_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(!take_content_size_action(
        component,
        &request));
    CHECK(boxrig_x11_terminal_component_set_preferred_grid(
              component,
              5,
              9) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_get_size_command_state(
              component,
              &command_state) == BOXRIG_UI_OK);
    CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_READY);
    CHECK(boxrig_x11_terminal_component_get_preferred_grid(
              component,
              &preferred_rows,
              &preferred_columns) == BOXRIG_UI_OK);
    CHECK(preferred_rows == 5 && preferred_columns == 9);
    CHECK(take_content_size_action(
        component,
        &request));
    CHECK(request.width ==
          (boxrig_ui_fixed)(9 * resource_metrics.cell_width) *
              BOXRIG_UI_FIXED_ONE);
    CHECK(request.height ==
          (boxrig_ui_fixed)(5 * resource_metrics.cell_height) *
              BOXRIG_UI_FIXED_ONE);
    CHECK(boxrig_x11_terminal_component_complete_size_command(
              component,
              request.revision,
              BOXRIG_UI_SIZE_ACCEPTED,
              request.width,
              request.height,
              &applied,
              &layout) == BOXRIG_UI_OK);
    CHECK(applied);
    CHECK(take_grid_resize_action(
        component,
        &resize));
    CHECK(resize.rows == 5 && resize.columns == 9);
    boxrig_terminal_size(terminal, &terminal_rows, &terminal_columns);
    CHECK(terminal_rows == 4 && terminal_columns == 12);
    CHECK(boxrig_terminal_resize(terminal, resize.rows, resize.columns) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_terminal_component_complete_grid_resize(
              component, resize.revision, true) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);

    invalid_specification = specification;
    invalid_specification.size_dip = 0.0f;
    CHECK(boxrig_x11_terminal_component_set_font_specification(
              component,
              &invalid_specification) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              component,
              &after_invalid) == BOXRIG_FONT_CATALOG_OK);
    CHECK(metrics_match(&resource_metrics, &after_invalid));

    changed_policy = policy;
    changed_policy.dpi_x = 144.0f;
    changed_policy.dpi_y = 144.0f;
    CHECK(boxrig_x11_terminal_component_set_font_rendering_policy(
              component,
              &changed_policy) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              component,
              &changed_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(changed_metrics.layout_revision > resource_metrics.layout_revision);
    CHECK(changed_metrics.cell_width > resource_metrics.cell_width);
    CHECK(changed_metrics.cell_height > resource_metrics.cell_height);
    CHECK(boxrig_x11_terminal_component_get_font_rendering_policy(
              component,
              &policy) == BOXRIG_UI_OK);
    CHECK(policy.dpi_x == 144.0f && policy.dpi_y == 144.0f);
    CHECK(boxrig_ui_model_get_canonical_cell_metrics(
              model,
              &canonical_set,
              &canonical_metrics) == BOXRIG_UI_OK);
    CHECK(canonical_set);
    CHECK(metrics_match(&changed_metrics, &canonical_metrics));
    CHECK(take_content_size_action(
        component,
        &request));
    CHECK(request.width ==
          (boxrig_ui_fixed)(9 * changed_metrics.cell_width) *
              BOXRIG_UI_FIXED_ONE);
    CHECK(request.height ==
          (boxrig_ui_fixed)(5 * changed_metrics.cell_height) *
              BOXRIG_UI_FIXED_ONE);

    CHECK(boxrig_x11_terminal_component_set_line_height_adjustment(
              component,
              2.0f) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_get_line_height_adjustment(
              component,
              &line_height) == BOXRIG_UI_OK);
    CHECK(line_height == 2.0f);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              component,
              &line_height_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(line_height_metrics.glyph_height == changed_metrics.glyph_height);
    CHECK(line_height_metrics.cell_height > changed_metrics.cell_height);

    zoom_baseline = line_height_metrics;
    zoom_policy = boxrig_font_visual_step_policy_default();
    zoom_policy.minimum_size_dip = zoom_baseline.requested_size_dip - 2.0f;
    zoom_policy.maximum_size_dip = zoom_baseline.requested_size_dip + 2.0f;
    CHECK(boxrig_x11_terminal_component_visual_zoom(
              component,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &zoom_changed) == BOXRIG_UI_OK);
    CHECK(zoom_changed);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              component,
              &zoomed_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(zoomed_metrics.cell_width > zoom_baseline.cell_width);
    CHECK(boxrig_x11_terminal_component_visual_zoom_reset(
              component,
              &zoom_changed) == BOXRIG_UI_OK);
    CHECK(zoom_changed);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              component,
              &reset_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(metrics_match(&reset_metrics, &zoomed_metrics));
    CHECK(boxrig_x11_terminal_component_get_font_specification(
              component,
              &specification) == BOXRIG_UI_OK);
    CHECK(specification.size_dip == zoom_baseline.requested_size_dip);
    CHECK(boxrig_x11_terminal_component_set_text_direction(
              component,
              BOXRIG_TEXT_DIRECTION_FORCE_RTL) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_text_direction(component) ==
          BOXRIG_TEXT_DIRECTION_FORCE_RTL);

    boxrig_x11_terminal_component_destroy(component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, parent);
    XSync(display, False);
    return true;
}

static bool component_controls(Display *display)
{
    int screen = DefaultScreen(display);
    Window parent = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        320,
        160,
        0,
        BlackPixel(display, screen),
        WhitePixel(display, screen));
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_x11_terminal_component_config config =
        boxrig_x11_terminal_component_config_default();
    config.view.blink.text_enabled = false;
    config.view.blink.cursor_enabled = false;
    boxrig_x11_terminal_component *component = NULL;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_component_allocation allocation;
    boxrig_ui_layout_result layout;
    boxrig_ui_hit_test_result hit;
    boxrig_ui_component_frame_result frame;
    boxrig_ui_transparency_config transparency;
    boxrig_ui_transparency_state transparency_state;
    boxrig_ui_component_diagnostic diagnostic;
    boxrig_scrollbar_model scrollbar;
    const uint8_t paste[] = {'o', 'k'};
    const uint8_t *output = NULL;
    XEvent event;
    size_t frame_requests = 0;
    uint32_t capabilities;
    bool changed = true;

    config.paste_chunk_size = 1;
    terminal_config.output_capacity = 6;
    terminal_config.rows = 3;
    terminal_config.columns = 8;
    CHECK(parent != None);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_create_child(
              display,
              parent,
              model,
              &config,
              &component) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              component,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.width = metrics.cell_width * terminal_config.columns +
                       boxrig_x11_scrollbar_pixel_width();
    allocation.height = metrics.cell_height * terminal_config.rows;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_x11_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_set_visible(component, true) ==
          BOXRIG_UI_OK);
    XMapWindow(display, parent);
    XSync(display, False);
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_set_frame_request_callback(
              component,
              count_frame_request,
              &frame_requests) == BOXRIG_UI_OK);
    CHECK(feed_text(terminal, "abc"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(frame_requests == 1);
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_hit_test(
              component,
              BOXRIG_UI_FIXED_ONE,
              BOXRIG_UI_FIXED_ONE,
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.inside_grid && hit.row == 0 && hit.logical_column == 0);

    memset(&event, 0, sizeof(event));
    event.type = FocusIn;
    event.xfocus.display = display;
    event.xfocus.window = boxrig_x11_terminal_component_owned_window(component);
    event.xfocus.mode = NotifyNormal;
    event.xfocus.detail = NotifyNonlinear;
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &event) ==
          BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(boxrig_x11_terminal_component_focused(component));
    CHECK(boxrig_ui_view_focused(boxrig_x11_terminal_component_view(component)));
    CHECK(boxrig_x11_terminal_component_request_focus(component) ==
          BOXRIG_UI_OK);
    event.type = FocusOut;
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &event) ==
          BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(!boxrig_x11_terminal_component_focused(component));
    CHECK(!boxrig_ui_view_focused(boxrig_x11_terminal_component_view(component)));

    CHECK(boxrig_x11_terminal_component_set_key_router(
              component,
              NULL,
              NULL) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_queue_paste(
              component,
              paste,
              sizeof(paste)) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(paste));
    CHECK(memcmp(output, paste, sizeof(paste)) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(paste)) ==
          BOXRIG_STATUS_OK);


    CHECK(feed_text(terminal, "\x1b[?2004h"));
    CHECK(boxrig_x11_terminal_component_queue_paste(component, (const uint8_t *)"discard", 7) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_set_input_enabled(component, false) == BOXRIG_UI_BUSY);
    boxrig_ui_input_state input_state;
    CHECK(boxrig_x11_terminal_component_get_input_state(component, &input_state) == BOXRIG_UI_OK);
    CHECK(input_state.phase == BOXRIG_UI_INPUT_STOPPING);
    event.type = FocusIn;
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &event) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(boxrig_x11_terminal_component_focused(component));
    event.type = FocusOut;
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &event) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(!boxrig_x11_terminal_component_focused(component));
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    CHECK(frame.input_blocked);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 6 && memcmp(output, "\x1b[200~", 6) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_get_input_state(component, &input_state) == BOXRIG_UI_OK);
    CHECK(input_state.phase == BOXRIG_UI_INPUT_DISABLED);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 6 && memcmp(output, "\x1b[201~", 6) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_terminal_component_queue_paste(component, paste, sizeof(paste)) == BOXRIG_UI_UNAVAILABLE);

    CHECK(boxrig_x11_terminal_component_get_scrollbar_model(
              component,
              &scrollbar) == BOXRIG_UI_OK);
    CHECK(scrollbar.viewport_rows == terminal_config.rows);
    CHECK(boxrig_x11_terminal_component_apply_scrollbar_action(
              component,
              BOXRIG_SCROLLBAR_BOTTOM,
              0,
              &changed) == BOXRIG_UI_OK);
    CHECK(!changed);

    CHECK(boxrig_x11_terminal_component_get_transparency(
              component,
              &transparency_state) == BOXRIG_UI_OK);
    CHECK(transparency_state.effective ==
          BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
    transparency = boxrig_ui_transparency_config_default();
    transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    transparency.requested_scopes =
        BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
    transparency.background_opacity = 128;
    CHECK(boxrig_x11_terminal_component_set_transparency(
              component,
              &transparency) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_x11_terminal_component_get_transparency(
              component,
              &transparency_state) == BOXRIG_UI_OK);
    CHECK(transparency_state.effective ==
          BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);

    capabilities = boxrig_x11_terminal_component_capabilities(component);
    CHECK((capabilities &
           BOXRIG_UI_COMPONENT_CAPABILITY_OWNED_VISIBLE_CHILD) != 0);
    CHECK((capabilities & BOXRIG_UI_COMPONENT_CAPABILITY_VIEW_DIRTY) != 0);
    CHECK((capabilities & BOXRIG_UI_COMPONENT_CAPABILITY_FONT_RESOURCES) != 0);
    CHECK((capabilities &
           BOXRIG_UI_COMPONENT_CAPABILITY_NATIVE_CHILD_SCROLLBAR) != 0);
    CHECK(boxrig_x11_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.running && diagnostic.visible && !diagnostic.focused);
    CHECK(diagnostic.layout_authority && diagnostic.layout_configured);
    CHECK(diagnostic.metrics_revision != 0);

    event.type = FocusIn;
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &event) ==
          BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(boxrig_ui_view_focused(boxrig_x11_terminal_component_view(component)));
    CHECK(boxrig_x11_terminal_component_quiesce(component) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_focused(boxrig_x11_terminal_component_view(component)));

    boxrig_x11_terminal_component_destroy(component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, parent);
    XSync(display, False);
    return true;
}

static bool constrained_zoom_uses_one_layout_action(Display *display)
{
    int screen = DefaultScreen(display);
    Window parent = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        640,
        480,
        0,
        BlackPixel(display, screen),
        WhitePixel(display, screen));
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_x11_terminal_component_config config =
        boxrig_x11_terminal_component_config_default();
    config.view.blink.text_enabled = false;
    config.view.blink.cursor_enabled = false;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_x11_terminal_component *component = NULL;
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

    CHECK(parent != None);
    terminal_config.rows = 12;
    terminal_config.columns = 40;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    CHECK(boxrig_x11_terminal_component_create_child(
              display,
              parent,
              model,
              &config,
              &component) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              component, &before) == BOXRIG_FONT_CATALOG_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.width = before.cell_width * terminal_config.columns;
    allocation.height = before.cell_height * terminal_config.rows;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_x11_terminal_component_allocate(
              component, &allocation, &layout) == BOXRIG_UI_OK);
    allocation.window_state = BOXRIG_UI_WINDOW_MAXIMIZED;
    CHECK(boxrig_x11_terminal_component_allocate(
              component, &allocation, &layout) == BOXRIG_UI_OK);

    CHECK(boxrig_x11_terminal_component_visual_zoom(
              component,
              BOXRIG_FONT_ZOOM_DECREASE,
              boxrig_font_visual_step_policy_default(),
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              component, &after) == BOXRIG_FONT_CATALOG_OK);
    CHECK(after.layout_revision > before.layout_revision);
    CHECK(boxrig_x11_terminal_component_get_size_command_state(
              component, &size_state) == BOXRIG_UI_OK);
    CHECK(size_state == BOXRIG_UI_SIZE_COMMAND_IDLE);
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(frame.frame_pending && frame.additional_dispatch);
    CHECK(boxrig_x11_terminal_component_take_layout_action(
        component, &action));
    CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
    CHECK(action.value.grid_resize.rows == allocation.height / after.cell_height);
    CHECK(action.value.grid_resize.columns == allocation.width / after.cell_width);
    CHECK(!boxrig_x11_terminal_component_take_layout_action(
        component, &action));
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(frame.frame_pending && !frame.additional_dispatch);
    CHECK(boxrig_x11_terminal_component_complete_grid_resize(
              component, action.revision, true) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_resize(
              terminal,
              action.value.grid_resize.rows,
              action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_terminal_component_complete_grid_resize(
              component, action.revision, true) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_complete_grid_resize(
              component, action.revision, true) == BOXRIG_UI_STALE);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    boxrig_terminal_size(terminal, &rows, &columns);
    CHECK(rows == action.value.grid_resize.rows);
    CHECK(columns == action.value.grid_resize.columns);
    CHECK(rows != terminal_config.rows || columns != terminal_config.columns);

    boxrig_x11_terminal_component_destroy(component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, parent);
    XSync(display, False);
    return true;
}

static bool zoom_waits_for_matching_allocation(Display *display)
{
    int screen = DefaultScreen(display);
    Window parent = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        640,
        360,
        0,
        BlackPixel(display, screen),
        WhitePixel(display, screen));
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_x11_terminal_component_config config =
        boxrig_x11_terminal_component_config_default();
    config.view.blink.text_enabled = false;
    config.view.blink.cursor_enabled = false;
    boxrig_x11_terminal_component *component = NULL;
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
    Window child;
    Window scrollbar;
    XWindowAttributes parent_before_completion;
    XWindowAttributes parent_while_pending;
    XWindowAttributes scrollbar_before_completion;
    XWindowAttributes scrollbar_while_pending;
    unsigned int scrollbar_width;
    uint64_t committed_hash;
    uint64_t pending_hash;
    uint64_t resize_preview_hash;
    uint64_t completed_hash;
    uint64_t latest_hash;
    unsigned long committed_background;
    unsigned long resize_background;
    bool changed = false;
    bool applied = false;

    CHECK(parent != None);
    terminal_config.rows = 3;
    terminal_config.columns = 8;
    config.scrollbar_presentation =
        BOXRIG_UI_COMPONENT_SCROLLBAR_NATIVE_CHILD;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_create_child(
              display,
              parent,
              model,
              &config,
              &component) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              component,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    memset(&allocation, 0, sizeof(allocation));
    scrollbar_width = boxrig_x11_scrollbar_pixel_width();
    allocation.width = metrics.cell_width * terminal_config.columns +
                       scrollbar_width;
    allocation.height = metrics.cell_height * terminal_config.rows;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_x11_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_set_visible(component, true) ==
          BOXRIG_UI_OK);
    XMapWindow(display, parent);
    XSync(display, False);
    CHECK(feed_text(
        terminal,
        "\x1b[?25l\x1b[2J\x1b[H\x1b[48;2;8;16;24m"
        "\x1b[38;2;255;255;255mMMMMMMMM\r\nWWWWWWWW"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    child = boxrig_x11_terminal_component_owned_window(component);
    CHECK(only_child_window(display, child, &scrollbar));
    CHECK(read_window_hash(
        display,
        child,
        allocation.width,
        allocation.height,
        &committed_hash));
    CHECK(read_window_pixel(
        display,
        child,
        (int)allocation.width - (int)scrollbar_width - 1,
        (int)allocation.height - 1,
        &committed_background));

    zoom_policy.minimum_size_dip = metrics.requested_size_dip;
    zoom_policy.maximum_size_dip = metrics.requested_size_dip + 30.0f;
    CHECK(boxrig_x11_terminal_component_visual_zoom(
              component,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(!take_content_size_action(
        component,
        &request));
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              component,
              &first_zoom_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_x11_terminal_component_visual_zoom(
              component,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(boxrig_x11_terminal_component_visual_zoom(
              component,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(!take_content_size_action(
        component,
        &request));
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              component,
              &latest_zoom_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(latest_zoom_metrics.requested_size_dip ==
          first_zoom_metrics.requested_size_dip);
    CHECK(feed_text(terminal, "\x1b[?2026h"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(!frame.additional_dispatch);
    CHECK(!take_content_size_action(
        component,
        &request));
    CHECK(feed_text(terminal, "\x1b[?2026l"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(read_window_hash(
        display,
        child,
        allocation.width,
        allocation.height,
        &pending_hash));
    CHECK(pending_hash == committed_hash);
    CHECK(take_content_size_action(
        component,
        &request));
    first_request = request;

    CHECK(XGetWindowAttributes(
              display,
              parent,
              &parent_before_completion) != 0);
    CHECK(XGetWindowAttributes(
              display,
              scrollbar,
              &scrollbar_before_completion) != 0);

    CHECK(request.width % BOXRIG_UI_FIXED_ONE == 0);
    CHECK(request.height % BOXRIG_UI_FIXED_ONE == 0);
    XSync(display, False);
    {
        XWindowAttributes pending_attributes;

        CHECK(XGetWindowAttributes(
                  display,
                  child,
                  &pending_attributes) != 0);
        CHECK(pending_attributes.width == (int)allocation.width &&
              pending_attributes.height == (int)allocation.height);
    }
    CHECK(XGetWindowAttributes(
              display,
              parent,
              &parent_while_pending) != 0);
    CHECK(XGetWindowAttributes(
              display,
              scrollbar,
              &scrollbar_while_pending) != 0);
    CHECK(parent_while_pending.width == parent_before_completion.width &&
          parent_while_pending.height == parent_before_completion.height);
    CHECK(scrollbar_while_pending.x == scrollbar_before_completion.x &&
          scrollbar_while_pending.height ==
              scrollbar_before_completion.height);
    CHECK(read_window_hash(
        display,
        child,
        allocation.width,
        allocation.height,
        &pending_hash));
    CHECK(pending_hash == committed_hash);
    CHECK(boxrig_x11_terminal_component_complete_size_command(
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
    CHECK(boxrig_x11_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(read_window_hash(
        display,
        child,
        allocation.width,
        allocation.height,
        &resize_preview_hash));
    CHECK(resize_preview_hash != committed_hash);
    CHECK(read_window_pixel(
        display,
        child,
        (int)allocation.width - (int)scrollbar_width - 1,
        (int)allocation.height - 1,
        &resize_background));
    CHECK(resize_background == committed_background);
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(read_window_hash(
        display,
        child,
        allocation.width,
        allocation.height,
        &completed_hash));
    CHECK(resize_preview_hash != completed_hash);
    CHECK(completed_hash != committed_hash);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              component,
              &latest_zoom_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(latest_zoom_metrics.requested_size_dip >
          first_zoom_metrics.requested_size_dip);
    CHECK(!take_content_size_action(
        component,
        &request));
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(read_window_hash(
        display,
        child,
        allocation.width,
        allocation.height,
        &latest_hash));
    CHECK(latest_hash == completed_hash);
    CHECK(take_content_size_action(
        component,
        &request));
    CHECK(request.revision != first_request.revision);
    CHECK(request.width > first_request.width);
    CHECK(request.height > first_request.height);

    applied = false;
    CHECK(boxrig_x11_terminal_component_complete_size_command(
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
    CHECK(boxrig_x11_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(!take_content_size_action(
        component,
        &request));
    CHECK(read_window_hash(
        display,
        child,
        allocation.width,
        allocation.height,
        &latest_hash));
    CHECK(latest_hash != completed_hash);

    CHECK(boxrig_x11_terminal_component_visual_zoom_reset(
              component,
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(!take_content_size_action(
        component,
        &request));
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(take_content_size_action(
        component,
        &request));
    applied = false;
    CHECK(boxrig_x11_terminal_component_complete_size_command(
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
    CHECK(boxrig_x11_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              component,
              &latest_zoom_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(latest_zoom_metrics.requested_size_dip == metrics.requested_size_dip);
    CHECK(latest_zoom_metrics.cell_width == metrics.cell_width);
    CHECK(latest_zoom_metrics.cell_height == metrics.cell_height);
    CHECK(!take_content_size_action(
        component,
        &request));
    CHECK(read_window_hash(
        display,
        child,
        allocation.width,
        allocation.height,
        &latest_hash));
    CHECK(latest_hash == committed_hash);

    boxrig_x11_terminal_component_destroy(component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, parent);
    XSync(display, False);
    return true;
}

static bool borrowed_components_share_one_target(Display *display)
{
    int screen = DefaultScreen(display);
    Window target = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        90,
        70,
        520,
        260,
        0,
        BlackPixel(display, screen),
        WhitePixel(display, screen));
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_x11_terminal_component_config config =
        boxrig_x11_terminal_component_config_default();
    config.view.blink.text_enabled = false;
    config.view.blink.cursor_enabled = false;
    boxrig_x11_terminal_borrowed_target borrowed_target;
    boxrig_x11_terminal_component *first = NULL;
    boxrig_x11_terminal_component *second = NULL;
    boxrig_x11_terminal_component *invalid = NULL;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_component_allocation first_allocation;
    boxrig_ui_component_allocation second_allocation;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame;
    XWindowAttributes before;
    XWindowAttributes after;
    XEvent event;
    const uint8_t expected_mouse[] = "\x1b[<0;2;2M";
    const uint8_t *output = NULL;
    uint64_t second_before;
    uint64_t second_unchanged;
    uint64_t second_updated;
    unsigned long outside_pixel;
    Window root = None;
    Window parent = None;
    Window *children = NULL;
    unsigned int child_count_before = 0;
    unsigned int child_count_after = 0;
    size_t output_length;
    uint32_t capabilities;

    CHECK(target != None);
    XSelectInput(
        display,
        target,
        ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
            ButtonReleaseMask | PointerMotionMask | FocusChangeMask |
            StructureNotifyMask);
    CHECK(XGetWindowAttributes(display, target, &before) != 0);
    CHECK(XQueryTree(
              display,
              target,
              &root,
              &parent,
              &children,
              &child_count_before) != 0);
    if (children != NULL) {
        XFree(children);
        children = NULL;
    }
    terminal_config.rows = 2;
    terminal_config.columns = 4;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    borrowed_target.presentation_window = target;
    borrowed_target.input_window = target;
    config.scrollbar_presentation =
        BOXRIG_UI_COMPONENT_SCROLLBAR_NATIVE_CHILD;
    CHECK(boxrig_x11_terminal_component_create_on_target(
              display,
              &borrowed_target,
              model,
              &config,
              &invalid) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(invalid == NULL);

    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    CHECK(boxrig_x11_terminal_component_create_on_target(
              display,
              &borrowed_target,
              model,
              &config,
              &first) == BOXRIG_UI_OK);
    config.attachment = BOXRIG_UI_COMPONENT_PRESENTATION_ONLY;
    CHECK(boxrig_x11_terminal_component_create_on_target(
              display,
              &borrowed_target,
              model,
              &config,
              &second) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_native_hosting(first) ==
          BOXRIG_UI_COMPONENT_BORROWED_TARGET);
    CHECK(boxrig_x11_terminal_component_owned_window(first) == None);
    CHECK(boxrig_x11_terminal_component_presentation_window(first) == target);
    CHECK(boxrig_x11_terminal_component_input_window(first) == target);
    CHECK(!boxrig_x11_terminal_component_input_window_owned(first));
    CHECK(boxrig_x11_terminal_component_scrollbar_window(first) == None);
    capabilities = boxrig_x11_terminal_component_capabilities(first);
    CHECK((capabilities & BOXRIG_UI_COMPONENT_CAPABILITY_BORROWED_TARGET) !=
          0);
    CHECK((capabilities &
           BOXRIG_UI_COMPONENT_CAPABILITY_EXTERNAL_PRESENTATION) != 0);
    CHECK((capabilities &
           BOXRIG_UI_COMPONENT_CAPABILITY_OWNED_VISIBLE_CHILD) == 0);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              first,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_x11_terminal_component_set_target_extent(
              first,
              (unsigned int)before.width,
              (unsigned int)before.height) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_set_target_extent(
              second,
              (unsigned int)before.width,
              (unsigned int)before.height) == BOXRIG_UI_OK);

    memset(&first_allocation, 0, sizeof(first_allocation));
    first_allocation.x = 31;
    first_allocation.y = 27;
    first_allocation.width = metrics.cell_width * terminal_config.columns;
    first_allocation.height = metrics.cell_height * terminal_config.rows;
    first_allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    second_allocation = first_allocation;
    second_allocation.x = first_allocation.x +
                          (int)first_allocation.width + 43;
    CHECK(boxrig_x11_terminal_component_allocate(
              first,
              &first_allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_allocate(
              second,
              &second_allocation,
              &layout) == BOXRIG_UI_OK);
    XMapWindow(display, target);
    XClearWindow(display, target);
    XSync(display, False);
    CHECK(boxrig_x11_terminal_component_set_visible(first, true) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_set_visible(second, true) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_request_focus(first) ==
          BOXRIG_UI_UNAVAILABLE);
    CHECK(feed_text(
        terminal,
        "\x1b[?25l\x1b[2J\x1b[H\x1b[48;2;8;16;24mABCD\r\nEFGH"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_frame(first, &frame) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_frame(second, &frame) ==
          BOXRIG_UI_OK);
    CHECK(read_window_pixel(display, target, 1, 1, &outside_pixel));
    CHECK(outside_pixel == WhitePixel(display, screen));
    CHECK(read_window_region_hash(
        display,
        target,
        second_allocation.x,
        second_allocation.y,
        second_allocation.width,
        second_allocation.height,
        &second_before));

    CHECK(feed_text(terminal, "Z"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_frame(first, &frame) == BOXRIG_UI_OK);
    CHECK(read_window_region_hash(
        display,
        target,
        second_allocation.x,
        second_allocation.y,
        second_allocation.width,
        second_allocation.height,
        &second_unchanged));
    CHECK(second_unchanged == second_before);

    CHECK(feed_text(terminal, "\x1b[?1000;1006h"));
    memset(&event, 0, sizeof(event));
    event.xbutton.display = display;
    event.xbutton.window = target;
    event.xbutton.root = RootWindow(display, screen);
    event.xbutton.same_screen = True;
    event.xbutton.type = ButtonPress;
    event.xbutton.button = Button1;
    event.xbutton.x = first_allocation.x + (int)metrics.cell_width + 1;
    event.xbutton.y = first_allocation.y + (int)metrics.cell_height + 1;
    CHECK(boxrig_x11_terminal_component_dispatch_event(first, &event) ==
          BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected_mouse) - 1);
    CHECK(memcmp(output, expected_mouse, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    boxrig_x11_terminal_component_destroy(first);
    first = NULL;
    CHECK(window_is_valid(display, target));
    CHECK(boxrig_x11_terminal_component_frame(second, &frame) ==
          BOXRIG_UI_OK);
    CHECK(read_window_region_hash(
        display,
        target,
        second_allocation.x,
        second_allocation.y,
        second_allocation.width,
        second_allocation.height,
        &second_updated));
    CHECK(second_updated != second_before);
    boxrig_x11_terminal_component_destroy(second);
    second = NULL;
    XSync(display, False);
    CHECK(XGetWindowAttributes(display, target, &after) != 0);
    CHECK(after.x == before.x && after.y == before.y &&
          after.width == before.width && after.height == before.height &&
          after.your_event_mask == before.your_event_mask &&
          after.map_state == IsViewable);
    CHECK(XQueryTree(
              display,
              target,
              &root,
              &parent,
              &children,
              &child_count_after) != 0);
    CHECK(child_count_after == child_count_before);
    if (children != NULL) {
        XFree(children);
    }
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, target);
    XSync(display, False);
    return true;
}

static bool composited_scrollbar_shares_the_component_generation(
    Display *display)
{
    int screen = DefaultScreen(display);
    Window target = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        30,
        40,
        420,
        220,
        0,
        BlackPixel(display, screen),
        WhitePixel(display, screen));
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_x11_terminal_component_config config =
        boxrig_x11_terminal_component_config_default();
    config.view.blink.text_enabled = false;
    config.view.blink.cursor_enabled = false;
    boxrig_x11_terminal_borrowed_target borrowed_target;
    boxrig_x11_terminal_component *component = NULL;
    boxrig_x11_terminal_component *owned = NULL;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_component_allocation allocation;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame;
    boxrig_scrollbar_model scrollbar_model;
    boxrig_scrollbar_thumb thumb;
    XEvent event;
    Window child = None;
    Window root = None;
    Window parent = None;
    Window *children = NULL;
    unsigned int child_count = 0;
    const uint8_t *output = NULL;
    uint64_t terminal_before;
    uint64_t terminal_after;
    uint64_t scrollbar_before;
    uint64_t scrollbar_after;
    unsigned long track_pixel;
    unsigned long thumb_pixel;
    unsigned int scrollbar_width = boxrig_x11_scrollbar_pixel_width();
    uint32_t capabilities;

    CHECK(target != None);
    XSelectInput(
        display,
        target,
        ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
            ButtonReleaseMask | PointerMotionMask | FocusChangeMask |
            StructureNotifyMask);
    XMapWindow(display, target);
    terminal_config.rows = 3;
    terminal_config.columns = 6;
    terminal_config.scrollback_capacity = 32;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    config.scrollbar_presentation =
        BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED;
    borrowed_target.presentation_window = target;
    borrowed_target.input_window = target;
    CHECK(boxrig_x11_terminal_component_create_on_target(
              display,
              &borrowed_target,
              model,
              &config,
              &component) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              component,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.x = 23;
    allocation.y = 19;
    allocation.width =
        metrics.cell_width * terminal_config.columns + scrollbar_width;
    allocation.height = metrics.cell_height * terminal_config.rows;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_x11_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(layout.columns == terminal_config.columns);
    CHECK(layout.excess_right == 0);
    CHECK(boxrig_x11_terminal_component_scrollbar_window(component) == None);
    capabilities = boxrig_x11_terminal_component_capabilities(component);
    CHECK((capabilities &
           BOXRIG_UI_COMPONENT_CAPABILITY_COMPOSITED_SCROLLBAR) != 0);
    CHECK((capabilities &
           BOXRIG_UI_COMPONENT_CAPABILITY_NATIVE_CHILD_SCROLLBAR) == 0);
    CHECK(XQueryTree(
              display,
              target,
              &root,
              &parent,
              &children,
              &child_count) != 0);
    CHECK(child_count == 0);
    if (children != NULL) {
        XFree(children);
        children = NULL;
    }

    XClearWindow(display, target);
    XSync(display, False);
    CHECK(feed_text(
        terminal,
        "\x1b[?25l\x1b[2J\x1b[H"
        "00\r\n01\r\n02\r\n03\r\n04\r\n05\r\n06\r\n07\r\n"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_get_scrollbar_model(
              component,
              &scrollbar_model) == BOXRIG_UI_OK);
    CHECK(scrollbar_model.enabled && scrollbar_model.viewport_offset == 0);
    boxrig_scrollbar_calculate_thumb(
        &scrollbar_model,
        allocation.height,
        scrollbar_width,
        &thumb);
    CHECK(thumb.length != 0 && thumb.start != 0);
    CHECK(read_window_pixel(
        display,
        target,
        allocation.x + (int)allocation.width - 1,
        allocation.y,
        &track_pixel));
    CHECK(read_window_pixel(
        display,
        target,
        allocation.x + (int)allocation.width - 1,
        allocation.y + (int)allocation.height - 1,
        &thumb_pixel));
    CHECK(track_pixel != thumb_pixel);
    CHECK(read_window_region_hash(
        display,
        target,
        allocation.x,
        allocation.y,
        allocation.width - scrollbar_width,
        allocation.height,
        &terminal_before));
    CHECK(read_window_region_hash(
        display,
        target,
        allocation.x + (int)allocation.width - (int)scrollbar_width,
        allocation.y,
        scrollbar_width,
        allocation.height,
        &scrollbar_before));

    CHECK(feed_text(terminal, "\x1b[?1000;1006h"));
    memset(&event, 0, sizeof(event));
    event.xbutton.display = display;
    event.xbutton.window = target;
    event.xbutton.root = RootWindow(display, screen);
    event.xbutton.same_screen = True;
    event.xbutton.type = ButtonPress;
    event.xbutton.button = Button1;
    event.xbutton.x = allocation.x + (int)allocation.width - 1;
    event.xbutton.y = allocation.y;
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &event) ==
          BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_terminal_viewport_offset(terminal) != 0);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);
    CHECK(read_window_region_hash(
        display,
        target,
        allocation.x,
        allocation.y,
        allocation.width - scrollbar_width,
        allocation.height,
        &terminal_after));
    CHECK(read_window_region_hash(
        display,
        target,
        allocation.x + (int)allocation.width - (int)scrollbar_width,
        allocation.y,
        scrollbar_width,
        allocation.height,
        &scrollbar_after));
    CHECK(terminal_after != terminal_before);
    CHECK(scrollbar_after != scrollbar_before);

    CHECK(boxrig_x11_terminal_component_get_scrollbar_model(
              component,
              &scrollbar_model) == BOXRIG_UI_OK);
    boxrig_scrollbar_calculate_thumb(
        &scrollbar_model,
        allocation.height,
        scrollbar_width,
        &thumb);
    event.xbutton.type = ButtonPress;
    event.xbutton.button = Button1;
    event.xbutton.x = allocation.x + (int)allocation.width - 1;
    event.xbutton.y = allocation.y + (int)thumb.start;
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &event) ==
          BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    event.xmotion.type = MotionNotify;
    event.xmotion.x = allocation.x + (int)allocation.width - 1;
    event.xmotion.y = allocation.y + (int)allocation.height - 1;
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &event) ==
          BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    event.xbutton.type = ButtonRelease;
    event.xbutton.button = Button1;
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &event) ==
          BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 0);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) ==
          BOXRIG_UI_OK);

    boxrig_x11_terminal_component_destroy(component);
    component = NULL;
    CHECK(boxrig_x11_terminal_component_create_child(
              display,
              target,
              model,
              &config,
              &owned) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_scrollbar_window(owned) == None);
    capabilities = boxrig_x11_terminal_component_capabilities(owned);
    CHECK((capabilities &
           BOXRIG_UI_COMPONENT_CAPABILITY_COMPOSITED_SCROLLBAR) != 0);
    allocation.x = 0;
    allocation.y = 0;
    CHECK(boxrig_x11_terminal_component_allocate(
              owned,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(layout.columns == terminal_config.columns);
    CHECK(boxrig_x11_terminal_component_set_visible(owned, true) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_frame(owned, &frame) == BOXRIG_UI_OK);
    CHECK(only_child_window(display, target, &child));
    CHECK(child == boxrig_x11_terminal_component_owned_window(owned));
    CHECK(XQueryTree(
              display,
              child,
              &root,
              &parent,
              &children,
              &child_count) != 0);
    CHECK(child_count == 0);
    if (children != NULL) {
        XFree(children);
    }
    boxrig_x11_terminal_component_destroy(owned);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, target);
    XSync(display, False);
    return true;
}

static bool clipboard_events_forward_for_hosting_mode(
    Display *display,
    bool borrowed)
{
    static const uint8_t copied_text[] = "forwarded selection";
    int screen = DefaultScreen(display);
    Display *requestor_display = XOpenDisplay(NULL);
    Window parent = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        64,
        64,
        0,
        0,
        0);
    Window requestor = None;
    Window input_window;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_x11_terminal_component_config config =
        boxrig_x11_terminal_component_config_default();
    config.view.blink.text_enabled = false;
    config.view.blink.cursor_enabled = false;
    boxrig_x11_terminal_borrowed_target target;
    boxrig_x11_terminal_component *component = NULL;
    boxrig_ui_view *view;
    boxrig_ui_hit_test_result hit;
    boxrig_ui_view_action action;
    XEvent event;
    XEvent notify;
    Atom clipboard;
    Atom utf8_string;
    Atom incr;
    Atom property;
    Atom timestamp_property;
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *value = NULL;
    uint8_t received[sizeof(copied_text)];
    size_t received_length = 0;
    size_t consumed = 0;

    CHECK(requestor_display != NULL && parent != None);
    requestor = XCreateSimpleWindow(
        requestor_display,
        RootWindow(requestor_display, DefaultScreen(requestor_display)),
        0,
        0,
        8,
        8,
        0,
        0,
        0);
    CHECK(requestor != None);
    XSelectInput(
        requestor_display,
        requestor,
        PropertyChangeMask | StructureNotifyMask);
    terminal_config.rows = 1;
    terminal_config.columns = 32;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              copied_text,
              sizeof(copied_text) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    config.scrollbar_presentation =
        BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    config.selection_transfer_chunk_size = 4;
    if (borrowed) {
        XSelectInput(
            display,
            parent,
            ExposureMask | KeyPressMask | KeyReleaseMask |
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                FocusChangeMask | PropertyChangeMask |
                StructureNotifyMask);
        target.presentation_window = parent;
        target.input_window = parent;
        CHECK(boxrig_x11_terminal_component_create_on_target(
                  display,
                  &target,
                  model,
                  &config,
                  &component) == BOXRIG_UI_OK);
    } else {
        CHECK(boxrig_x11_terminal_component_create_child(
                  display,
                  parent,
                  model,
                  &config,
                  &component) == BOXRIG_UI_OK);
    }
    input_window = boxrig_x11_terminal_component_input_window(component);
    CHECK(input_window != None);
    timestamp_property =
        XInternAtom(display, "_BOXRIG_TIMESTAMP", False);
    CHECK(timestamp_property != None);
    view = boxrig_x11_terminal_component_view(component);
    CHECK(view != NULL);
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
    action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    CHECK(boxrig_x11_terminal_component_apply_view_action(
              component,
              &action) == BOXRIG_UI_OK);
    CHECK(take_atom_property_event(
        display,
        input_window,
        timestamp_property,
        PropertyNewValue,
        &event));
    CHECK(event.xproperty.time != CurrentTime);
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &event) ==
          BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    XSync(display, False);

    clipboard = XInternAtom(requestor_display, "CLIPBOARD", False);
    utf8_string = XInternAtom(requestor_display, "UTF8_STRING", False);
    incr = XInternAtom(requestor_display, "INCR", False);
    property = XInternAtom(
        requestor_display,
        borrowed ? "_BOXRIG_BORROWED_FORWARD"
                 : "_BOXRIG_OWNED_FORWARD",
        False);
    CHECK(clipboard != None && utf8_string != None && incr != None &&
          property != None);
    CHECK(XGetSelectionOwner(requestor_display, clipboard) == input_window);
    XConvertSelection(
        requestor_display,
        clipboard,
        utf8_string,
        property,
        requestor,
        CurrentTime);
    XSync(requestor_display, False);
    CHECK(take_window_event(
        display,
        input_window,
        SelectionRequest,
        &event));
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &event) ==
          BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    XSync(display, False);
    CHECK(take_window_event(
        requestor_display,
        requestor,
        SelectionNotify,
        &notify));
    CHECK(notify.xselection.property == property);
    CHECK(XGetWindowProperty(
              requestor_display,
              requestor,
              property,
              0,
              1,
              True,
              AnyPropertyType,
              &actual_type,
              &actual_format,
              &item_count,
              &bytes_after,
              &value) == Success);
    CHECK(actual_type == incr && actual_format == 32 && item_count == 1);
    XFree(value);
    value = NULL;
    while (true) {
        CHECK(take_property_event(
            display,
            requestor,
            PropertyDelete,
            &event));
        CHECK(boxrig_x11_terminal_component_dispatch_event(
                  component,
                  &event) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
        XSync(display, False);
        CHECK(take_property_event(
            requestor_display,
            requestor,
            PropertyNewValue,
            &event));
        CHECK(XGetWindowProperty(
                  requestor_display,
                  requestor,
                  property,
                  0,
                  64,
                  True,
                  AnyPropertyType,
                  &actual_type,
                  &actual_format,
                  &item_count,
                  &bytes_after,
                  &value) == Success);
        CHECK(actual_type == utf8_string && actual_format == 8 &&
              bytes_after == 0);
        if (item_count == 0) {
            XFree(value);
            value = NULL;
            break;
        }
        CHECK(received_length + item_count <= sizeof(received));
        memcpy(received + received_length, value, item_count);
        received_length += item_count;
        XFree(value);
        value = NULL;
    }
    CHECK(received_length == sizeof(copied_text) - 1);
    CHECK(memcmp(received, copied_text, received_length) == 0);

    XSetSelectionOwner(
        requestor_display,
        clipboard,
        requestor,
        CurrentTime);
    XSync(requestor_display, False);
    CHECK(take_window_event(
        display,
        input_window,
        SelectionClear,
        &event));
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &event) ==
          BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(boxrig_x11_terminal_component_apply_view_action(
              component,
              &action) == BOXRIG_UI_OK);
    CHECK(take_atom_property_event(
        display,
        input_window,
        timestamp_property,
        PropertyNewValue,
        &event));
    CHECK(event.xproperty.time != CurrentTime);
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &event) ==
          BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    XSync(display, False);
    CHECK(XGetSelectionOwner(requestor_display, clipboard) == input_window);
    CHECK(boxrig_x11_terminal_component_quiesce(component) == BOXRIG_UI_OK);
    CHECK(XGetSelectionOwner(requestor_display, clipboard) == None);

    boxrig_x11_terminal_component_destroy(component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(requestor_display, requestor);
    XDestroyWindow(display, parent);
    XCloseDisplay(requestor_display);
    XSync(display, False);
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

static bool scrollbar_visibility_changes_layout(Display *display)
{
    Window parent = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 600, 200, 0, 0, 0);
    CHECK(parent != None);
    for (int presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
         presentation <= BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED; presentation++) {
        boxrig_terminal_config tc = boxrig_terminal_config_default();
        tc.rows = 3; tc.columns = 20; tc.scrollback_capacity = 100;
        boxrig_terminal *terminal = NULL;
        boxrig_ui_model *model = NULL;
        boxrig_x11_terminal_component *component = NULL;
        boxrig_x11_terminal_component_config config = boxrig_x11_terminal_component_config_default();
        config.scrollbar_presentation = (boxrig_ui_component_scrollbar_presentation)presentation;
        config.view.blink.text_enabled = false;
        config.view.blink.cursor_enabled = false;
        CHECK(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
        CHECK(boxrig_x11_terminal_component_create_child(display, parent, model, &config, &component) == BOXRIG_UI_OK);
        boxrig_font_effective_metrics metrics;
        CHECK(boxrig_x11_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
        boxrig_ui_scrollbar_visibility_state state;
        CHECK(boxrig_x11_terminal_component_get_scrollbar_visibility(component, &state) == BOXRIG_UI_OK);
        if (presentation == BOXRIG_UI_COMPONENT_SCROLLBAR_NONE) {
            CHECK(!state.visible && state.reserved_width == 0 && !state.pending);
            CHECK(boxrig_x11_terminal_component_set_scrollbar_visibility(component, BOXRIG_UI_SCROLLBAR_NEVER) == BOXRIG_UI_OK);
            CHECK(boxrig_x11_terminal_component_set_scrollbar_visibility(component, BOXRIG_UI_SCROLLBAR_ALWAYS) == BOXRIG_UI_OK);
            CHECK(boxrig_x11_terminal_component_get_scrollbar_visibility(component, &state) == BOXRIG_UI_OK);
            CHECK(!state.visible && state.reserved_width == 0 && !state.pending);
            boxrig_x11_terminal_component_destroy(component);
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
        CHECK(boxrig_x11_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
        boxrig_ui_layout_action action;
        /* Drain the initial canonical allocation, if it produced one. */
        if (boxrig_x11_terminal_component_take_layout_action(component, &action)) {
            CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
            CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows, action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
            CHECK(boxrig_x11_terminal_component_complete_grid_resize(component, action.revision, true) == BOXRIG_UI_OK);
        }
        CHECK(feed_text(terminal, "0\r\n1\r\n2\r\n3\r\n4\r\n5"));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        boxrig_ui_component_frame_result frame;
        CHECK(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        XMapWindow(display, parent);
        CHECK(boxrig_x11_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
        size_t history = boxrig_terminal_scrollback_rows(terminal);
        CHECK(history > 0);
        CHECK(feed_text(terminal, "\033[?1003;1006h"));
        unsigned int mouse_calls = 0;
        XEvent pointer = {0};
        pointer.xbutton.display = display;
        pointer.xbutton.window = boxrig_x11_terminal_component_input_window(component);
        if (presentation == BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED) {
            CHECK(boxrig_x11_terminal_component_set_mouse_router(component, scrollbar_mouse_probe, &mouse_calls) == BOXRIG_UI_OK);
            pointer.type = ButtonPress;
            pointer.xbutton.button = Button1;
            pointer.xbutton.x = (int)allocation.width - 2;
            pointer.xbutton.y = (int)allocation.height - 2;
            CHECK(boxrig_x11_terminal_component_dispatch_event(component, &pointer) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
            pointer.type = MotionNotify;
            pointer.xmotion.x = (int)allocation.width - 2;
            pointer.xmotion.y = 0;
            pointer.xmotion.state = Button1Mask;
            CHECK(boxrig_x11_terminal_component_dispatch_event(component, &pointer) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
            boxrig_scrollbar_model scroll;
            CHECK(boxrig_x11_terminal_component_get_scrollbar_model(component, &scroll) == BOXRIG_UI_OK && scroll.position == 0);
            CHECK(mouse_calls == 0);
        }

        CHECK(boxrig_x11_terminal_component_set_scrollbar_visibility(component, BOXRIG_UI_SCROLLBAR_NEVER) == BOXRIG_UI_OK);
        CHECK(boxrig_x11_terminal_component_get_scrollbar_visibility(component, &state) == BOXRIG_UI_OK);
        CHECK(state.requested == BOXRIG_UI_SCROLLBAR_NEVER);
        CHECK(boxrig_x11_terminal_component_take_layout_action(component, &action));
        CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(action.value.grid_resize.columns == allocation.width / metrics.cell_width);
        CHECK(state.pending && state.visible && state.reserved_width == bar_width);
        CHECK(boxrig_x11_terminal_component_set_scrollbar_visibility(component, BOXRIG_UI_SCROLLBAR_ALWAYS) == BOXRIG_UI_BUSY);
        boxrig_font_specification specification;
        boxrig_font_rendering_policy policy;
        CHECK(boxrig_x11_terminal_component_get_font_specification(component, &specification) == BOXRIG_UI_OK);
        CHECK(boxrig_x11_terminal_component_get_font_rendering_policy(component, &policy) == BOXRIG_UI_OK);
        CHECK(boxrig_x11_terminal_component_set_font_specification(component, &specification) == BOXRIG_UI_BUSY);
        policy.output_scale = 2.0f;
        CHECK(boxrig_x11_terminal_component_set_font_rendering_policy(component, &policy) == BOXRIG_UI_BUSY);
        bool zoom_changed = true;
        CHECK(boxrig_x11_terminal_component_visual_zoom(component, BOXRIG_FONT_ZOOM_INCREASE,
            boxrig_font_visual_step_policy_default(), &zoom_changed) == BOXRIG_UI_BUSY && !zoom_changed);
        CHECK(boxrig_x11_terminal_component_visual_zoom_reset(component, &zoom_changed) == BOXRIG_UI_BUSY && !zoom_changed);

        CHECK(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        CHECK(frame.frame_pending);
        CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows, action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(boxrig_x11_terminal_component_complete_grid_resize(component, action.revision, true) == BOXRIG_UI_OK);
        CHECK(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        CHECK(boxrig_x11_terminal_component_get_scrollbar_visibility(component, &state) == BOXRIG_UI_OK);
        CHECK(!state.visible && state.reserved_width == 0 && !state.pending);
        if (presentation == BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED) {
            pointer.type = ButtonRelease;
            pointer.xbutton.button = Button1;
            pointer.xbutton.x = 2; pointer.xbutton.y = 2; pointer.xbutton.state = Button1Mask;
            CHECK(boxrig_x11_terminal_component_dispatch_event(component, &pointer) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
            CHECK(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
            CHECK(mouse_calls == 0);
        }

        CHECK(!boxrig_x11_terminal_component_take_layout_action(component, &action));
        if (presentation == BOXRIG_UI_COMPONENT_SCROLLBAR_NATIVE_CHILD) {
            XWindowAttributes bar;
            CHECK(XGetWindowAttributes(display, boxrig_x11_terminal_component_scrollbar_window(component), &bar) != 0);
            CHECK(bar.map_state == IsUnmapped);
        }
        CHECK(boxrig_x11_terminal_component_set_visible(component, false) == BOXRIG_UI_OK);
        CHECK(boxrig_x11_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
        CHECK(boxrig_x11_terminal_component_get_scrollbar_visibility(component, &state) == BOXRIG_UI_OK);
        CHECK(!state.visible && state.reserved_width == 0);
        CHECK(boxrig_terminal_scrollback_rows(terminal) == history);
        const boxrig_ui_scrollbar_visibility policies[] = {
            BOXRIG_UI_SCROLLBAR_AUTO, BOXRIG_UI_SCROLLBAR_ALWAYS,
            BOXRIG_UI_SCROLLBAR_NEVER, BOXRIG_UI_SCROLLBAR_AUTO};
        for (size_t i = 0; i < sizeof(policies)/sizeof(*policies); i++) {
            CHECK(boxrig_x11_terminal_component_set_scrollbar_visibility(component, policies[i]) == BOXRIG_UI_OK);
            if (boxrig_x11_terminal_component_take_layout_action(component, &action)) {
                CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
                CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows, action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
                CHECK(boxrig_x11_terminal_component_complete_grid_resize(component, action.revision, true) == BOXRIG_UI_OK);
            }
            CHECK(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
            CHECK(boxrig_x11_terminal_component_get_scrollbar_visibility(component, &state) == BOXRIG_UI_OK);
            CHECK(state.visible == (policies[i] != BOXRIG_UI_SCROLLBAR_NEVER));
            CHECK(!state.pending && !boxrig_x11_terminal_component_take_layout_action(component, &action));
        }
        /* AUTO follows history availability, including alternate-screen changes. */
        const char *sequences[] = {"\033[?1049h", "\033[?1049l", "\033[3J"};
        for (size_t i = 0; i < 3; i++) {
            CHECK(feed_text(terminal, sequences[i]));
            CHECK(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
            if (boxrig_x11_terminal_component_take_layout_action(component, &action)) {
                CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
                CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows, action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
                CHECK(boxrig_x11_terminal_component_complete_grid_resize(component, action.revision, true) == BOXRIG_UI_OK);
            }
            CHECK(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
            CHECK(boxrig_x11_terminal_component_get_scrollbar_visibility(component, &state) == BOXRIG_UI_OK);
            CHECK(state.visible == (i == 1) && !state.pending);
            CHECK(!boxrig_x11_terminal_component_take_layout_action(component, &action));
        }
        CHECK(boxrig_x11_terminal_component_set_scrollbar_visibility(component, (boxrig_ui_scrollbar_visibility)99) == BOXRIG_UI_INVALID_ARGUMENT);
        CHECK(boxrig_x11_terminal_component_set_scrollbar_visibility(component, BOXRIG_UI_SCROLLBAR_ALWAYS) == BOXRIG_UI_OK);
        CHECK(boxrig_x11_terminal_component_take_layout_action(component, &action));
        CHECK(boxrig_x11_terminal_component_complete_grid_resize(component, action.revision, false) == BOXRIG_UI_OK);
        CHECK(boxrig_x11_terminal_component_get_scrollbar_visibility(component, &state) == BOXRIG_UI_OK);
        CHECK(!state.visible && !state.pending && state.requested == BOXRIG_UI_SCROLLBAR_AUTO);
        boxrig_ui_component_diagnostic diagnostic;
        CHECK(boxrig_x11_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
        CHECK(!diagnostic.running);
        boxrig_x11_terminal_component_destroy(component);
        boxrig_ui_model_destroy(model);
        boxrig_terminal_destroy(terminal);
    }
    XDestroyWindow(display, parent);
    return true;
}

typedef struct reparent_callback_probe {
    boxrig_x11_terminal_component *component;
    Window destination;
    boxrig_ui_status result;
    size_t calls;
} reparent_callback_probe;

static void reparent_from_frame_request(void *context)
{
    reparent_callback_probe *probe = context;
    probe->calls++;
    probe->result = boxrig_x11_terminal_component_reparent(probe->component, probe->destination);
}

static bool component_reparents_between_windows(Display *display)
{
    Window root = DefaultRootWindow(display);
    Window first = XCreateSimpleWindow(display, root, 0, 0, 400, 240, 0, 0, 0);
    Window second = XCreateSimpleWindow(display, root, 420, 0, 400, 240, 0, 0, 0);
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    tc.rows = 3;
    tc.columns = 12;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_x11_terminal_component *component = NULL;
    boxrig_x11_terminal_component_config config = boxrig_x11_terminal_component_config_default();
    config.view.blink.text_enabled = false;
    config.view.blink.cursor_enabled = false;
    CHECK(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_create_child(display, first, model, &config, &component) == BOXRIG_UI_OK);
    boxrig_ui_view *view = boxrig_x11_terminal_component_view(component);
    Window child = boxrig_x11_terminal_component_owned_window(component);
    Window scrollbar = boxrig_x11_terminal_component_scrollbar_window(component);
    boxrig_font_effective_metrics metrics;
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
    boxrig_ui_component_allocation allocation = {0};
    allocation.x = 8;
    allocation.y = 10;
    allocation.width = metrics.cell_width * tc.columns + boxrig_x11_scrollbar_pixel_width();
    allocation.height = metrics.cell_height * tc.rows;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    boxrig_ui_layout_result layout;
    CHECK(boxrig_x11_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    CHECK(feed_text(terminal, "persistent"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    boxrig_ui_hit_test_result hit = {0};
    hit.inside_grid = hit.text_position_valid = true;
    CHECK(boxrig_terminal_get_text_position(terminal, BOXRIG_TEXT_SCREEN_PRIMARY, 0, 0, &hit.leading_position) == BOXRIG_STATUS_OK);
    hit.trailing_position = hit.position = hit.leading_position;
    CHECK(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_LINE, &hit) == BOXRIG_UI_OK);
    XMapWindow(display, first);
    XMapWindow(display, second);
    CHECK(boxrig_x11_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    XSync(display, False);
    CHECK(boxrig_x11_terminal_component_request_focus(component) == BOXRIG_UI_OK);
    boxrig_ui_component_frame_result frame;
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    XSync(display, False);
    while (XPending(display)) {
        XEvent event;
        XNextEvent(display, &event);
        CHECK(boxrig_x11_terminal_component_dispatch_event(component, &event) != BOXRIG_UI_COMPONENT_EVENT_FAILURE);
    }
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    uint64_t before;
    CHECK(read_window_hash(display, child, allocation.width, allocation.height, &before));
    for (size_t i = 0; i < 4; i++) {
        Window destination = i % 2 == 0 ? second : first;
        boxrig_ui_input_state old_input, new_input;
        CHECK(boxrig_x11_terminal_component_get_input_state(component, &old_input) == BOXRIG_UI_OK);
        CHECK(boxrig_x11_terminal_component_reparent(component, destination) == BOXRIG_UI_OK);
        CHECK(boxrig_x11_terminal_component_request_focus(component) == BOXRIG_UI_OK);
        CHECK(child_has_parent(display, child, destination));
        CHECK(child_has_parent(display, scrollbar, child));
        CHECK(boxrig_x11_terminal_component_view(component) == view);
        CHECK(boxrig_ui_view_is_layout_authority(view));
        boxrig_ui_selection selection;
        CHECK(boxrig_ui_view_get_selection(view, &selection));
        CHECK(selection.range_valid);
        CHECK(boxrig_x11_terminal_component_get_input_state(component, &new_input) == BOXRIG_UI_OK);
        CHECK(new_input.generation > old_input.generation && new_input.phase == old_input.phase);
        CHECK(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        XSync(display, False);
        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);
            CHECK(boxrig_x11_terminal_component_dispatch_event(component, &event) != BOXRIG_UI_COMPONENT_EVENT_FAILURE);
        }
        CHECK(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        uint64_t after;
        CHECK(read_window_hash(display, child, allocation.width, allocation.height, &after));
        CHECK(after == before);
        boxrig_font_effective_metrics after_metrics;
        CHECK(boxrig_x11_terminal_component_get_effective_metrics(component, &after_metrics) == BOXRIG_FONT_CATALOG_OK);
        CHECK(metrics_match(&metrics, &after_metrics));
        CHECK(boxrig_x11_terminal_component_reparent(component, child) == BOXRIG_UI_UNAVAILABLE);
        CHECK(boxrig_x11_terminal_component_reparent(component, scrollbar) == BOXRIG_UI_UNAVAILABLE);
        CHECK(child_has_parent(display, child, destination));
    }
    /* Input from the old attachment must not be replayed after the move. */
    XEvent key = {0};
    key.xkey.type = KeyPress;
    key.xkey.display = display;
    key.xkey.window = child;
    key.xkey.root = root;
    key.xkey.keycode = XKeysymToKeycode(display, XK_a);
    key.xkey.same_screen = True;
    key.xkey.serial = 1;
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &key) == BOXRIG_UI_COMPONENT_EVENT_UNRELATED);
    const uint8_t *key_output = NULL;
    CHECK(boxrig_terminal_output_peek(terminal, &key_output) == 0);
    key.xkey.serial = NextRequest(display);
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &key) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    CHECK(boxrig_terminal_output_peek(terminal, &key_output) == 1 && key_output[0] == 'a');
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_terminal_component_reparent(component, second) == BOXRIG_UI_BUSY);
    CHECK(boxrig_x11_terminal_component_reparent(component, first) == BOXRIG_UI_BUSY);
    CHECK(child_has_parent(display, child, first));
    key.xkey.type = KeyRelease;
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &key) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    XEvent focus = {0};
    focus.xfocus.type = FocusIn;
    focus.xfocus.display = display;
    focus.xfocus.window = child;
    focus.xfocus.serial = NextRequest(display);
    focus.xfocus.mode = NotifyNormal;
    focus.xfocus.detail = NotifyNonlinear;
    CHECK(boxrig_x11_terminal_component_dispatch_event(component, &focus) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    boxrig_composition_event preedit = {0};
    preedit.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    preedit.text = "pending";
    preedit.text_length = 7;
    CHECK(boxrig_composition_apply_event(boxrig_x11_terminal_component_composition(component), &preedit) == BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_terminal_component_reparent(component, second) == BOXRIG_UI_BUSY);
    CHECK(child_has_parent(display, child, first));
    memset(&preedit, 0, sizeof(preedit));
    preedit.type = BOXRIG_COMPOSITION_CANCEL;
    CHECK(boxrig_composition_apply_event(boxrig_x11_terminal_component_composition(component), &preedit) == BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    reparent_callback_probe probe = {component, second, BOXRIG_UI_OK, 0};
    CHECK(boxrig_x11_terminal_component_set_frame_request_callback(component, reparent_from_frame_request, &probe) == BOXRIG_UI_OK);
    CHECK(feed_text(terminal, "x"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(probe.calls > 0 && probe.result == BOXRIG_UI_BUSY);
    CHECK(child_has_parent(display, child, first));
    CHECK(boxrig_x11_terminal_component_set_frame_request_callback(component, NULL, NULL) == BOXRIG_UI_OK);
    /* Destruction of the former parent must not destroy the moved child. */
    CHECK(boxrig_x11_terminal_component_reparent(component, second) == BOXRIG_UI_OK);
    XDestroyWindow(display, first);
    CHECK(window_is_valid(display, child));
    CHECK(feed_text(terminal, "\r\nafter move"));
    CHECK(boxrig_x11_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    const uint8_t *output = NULL;
    const uint8_t paste[] = "input after move";
    CHECK(boxrig_x11_terminal_component_queue_paste(component, paste, sizeof(paste) - 1) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(paste) - 1);
    CHECK(memcmp(output, paste, sizeof(paste) - 1) == 0);
    CHECK(boxrig_x11_terminal_component_reparent(NULL, second) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_x11_terminal_component_reparent(component, None) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_x11_terminal_component_quiesce(component) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_reparent(component, second) == BOXRIG_UI_INVALID_ARGUMENT);
    boxrig_x11_terminal_component_destroy(component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, second);
    return true;
}

int main(int argc, char **argv)
{
    Display *display = XOpenDisplay(NULL);
    bool passed;

    if (argc != 2 || display == NULL) {
        fprintf(stderr, "FAIL: usage or XOpenDisplay\n");
        return 1;
    }
    passed = component_reparents_between_windows(display) &&
             scrollbar_visibility_changes_layout(display) &&
             component_owns_only_its_child(display) &&
             component_font_and_size_transactions(display, argv[1]) &&
             component_controls(display) &&
             constrained_zoom_uses_one_layout_action(display) &&
             zoom_waits_for_matching_allocation(display) &&
             borrowed_components_share_one_target(display) &&
             composited_scrollbar_shares_the_component_generation(display) &&
             clipboard_events_forward_for_hosting_mode(display, false) &&
             clipboard_events_forward_for_hosting_mode(display, true);
    cairo_debug_reset_static_data();
    XCloseDisplay(display);
    FcFini();
    if (!passed) {
        return 1;
    }
    puts("PASS: X11 terminal component");
    return 0;
}
