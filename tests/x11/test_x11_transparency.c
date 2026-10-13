/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui.h"
#include "boxrig/x11.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>

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

static bool process_selection_events(
    Display *display,
    boxrig_x11_transparency_context *context,
    size_t expected_changes)
{
    size_t handled = 0;
    size_t changes = 0;

    XSync(display, False);
    while (XPending(display) != 0) {
        XEvent event;
        bool changed = false;
        int result;

        XNextEvent(display, &event);
        result = boxrig_x11_transparency_context_process_event(
            context,
            &event,
            &changed);
        CHECK(result >= 0);
        if (result == 1) {
            handled++;
            if (changed) {
                changes++;
            }
        }
    }
    CHECK(handled != 0);
    CHECK(changes == expected_changes);
    return true;
}

static Window create_window(
    Display *display,
    int screen,
    const boxrig_x11_window_requirements *requirements)
{
    return XCreateWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        64,
        48,
        0,
        requirements->depth,
        InputOutput,
        requirements->visual,
        requirements->attribute_mask,
        (XSetWindowAttributes *)&requirements->attributes);
}

static bool negotiate_and_track_compositor(Display *display)
{
    const uint32_t all_scopes =
        BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE |
        BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
    int screen = DefaultScreen(display);
    char selection_name[64];
    Atom selection;
    boxrig_ui_transparency_config config =
        boxrig_ui_transparency_config_default();
    boxrig_x11_transparency_context *context = NULL;
    boxrig_x11_transparency_context *rejected = NULL;
    boxrig_x11_transparency_support support;
    boxrig_x11_window_requirements requirements;
    boxrig_ui_transparency_state state;
    boxrig_ui_transparency_state previous;
    boxrig_x11_renderer *renderer = NULL;
    boxrig_x11_scrollbar *scrollbar = NULL;
    boxrig_terminal *terminal = NULL;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    XWindowAttributes attributes;
    Window query_root = None;
    Window query_parent = None;
    Window *children = NULL;
    unsigned int child_count = 0;
    Display *owner_display = NULL;
    Window argb_window = None;
    Window default_window = None;
    Window owner = None;
    Window remote_owner = None;
    Atom remote_selection;
    uint64_t revision;
    bool changed = true;
    bool success = false;

    config.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
    config.background_opacity = 128;
    CHECK(boxrig_x11_transparency_context_create(
              display,
              screen,
              &config,
              &context) == BOXRIG_UI_OK);
    CHECK(context != NULL);
    CHECK(boxrig_x11_transparency_context_get_support(
              context,
              &support) == BOXRIG_UI_OK);
    CHECK(support.composite_extension);
    CHECK(support.composite_major_version > 0 ||
          support.composite_minor_version > 0);
    CHECK(support.fixes_extension);
    CHECK(support.fixes_major_version > 0 ||
          support.fixes_minor_version > 0);
    CHECK(support.render_extension);
    CHECK(support.render_major_version > 0 ||
          support.render_minor_version > 0);
    CHECK(support.argb_visual);
    CHECK(!support.compositor_owner);
    CHECK(!support.window_bound);

    CHECK(boxrig_x11_transparency_context_get_state(context, &state) ==
          BOXRIG_UI_OK);
    CHECK(state.capabilities.premultiplied_argb32_target);
    CHECK(state.capabilities.supported_scopes == all_scopes);
    CHECK(state.capabilities.active_scopes ==
          BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE);
    CHECK(state.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
    CHECK(state.reason == BOXRIG_UI_TRANSPARENCY_REASON_SCOPE_INACTIVE);
    CHECK(state.effective_background_opacity == 255);
    revision = state.revision;

    CHECK(boxrig_x11_transparency_context_refresh(context, &changed) ==
          BOXRIG_UI_OK);
    CHECK(!changed);
    CHECK(boxrig_x11_transparency_context_get_state(context, &state) ==
          BOXRIG_UI_OK);
    CHECK(state.revision == revision);

    CHECK(boxrig_x11_transparency_context_get_window_requirements(
              context,
              &requirements) == BOXRIG_UI_OK);
    CHECK(requirements.visual != NULL);
    CHECK(requirements.depth == 32);
    CHECK(requirements.colormap != None);
    CHECK(requirements.colormap != DefaultColormap(display, screen));
    CHECK(requirements.premultiplied_argb32);
    CHECK((requirements.attribute_mask & CWColormap) != 0);
    argb_window = create_window(display, screen, &requirements);
    CHECK(argb_window != None);
    CHECK(boxrig_x11_transparency_context_bind_window(
              context,
              argb_window) == BOXRIG_UI_OK);
    CHECK(XGetWindowAttributes(display, argb_window, &attributes) != 0);
    CHECK(attributes.visual == requirements.visual);
    CHECK(attributes.depth == requirements.depth);
    CHECK(attributes.colormap == requirements.colormap);
    CHECK(boxrig_x11_transparency_context_get_support(
              context,
              &support) == BOXRIG_UI_OK);
    CHECK(support.window_bound);
    CHECK(support.window_argb_compatible);
    CHECK(boxrig_x11_renderer_create(display, argb_window, &renderer) == 0);
    boxrig_x11_renderer_destroy(renderer);
    renderer = NULL;
    terminal_config.rows = 1;
    terminal_config.columns = 1;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_scrollbar_create(
              display,
              argb_window,
              terminal,
              &scrollbar) == 0);
    XSync(display, False);
    CHECK(XQueryTree(
              display,
              argb_window,
              &query_root,
              &query_parent,
              &children,
              &child_count) != 0);
    CHECK(child_count == 1);
    CHECK(XGetWindowAttributes(display, children[0], &attributes) != 0);
    CHECK(attributes.visual == DefaultVisual(display, screen));
    CHECK(attributes.depth == DefaultDepth(display, screen));
    CHECK(attributes.colormap == DefaultColormap(display, screen));
    XFree(children);
    children = NULL;
    boxrig_x11_scrollbar_destroy(scrollbar);
    scrollbar = NULL;
    boxrig_terminal_destroy(terminal);
    terminal = NULL;

    default_window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        64,
        48,
        0,
        BlackPixel(display, screen),
        BlackPixel(display, screen));
    CHECK(default_window != None);
    CHECK(boxrig_x11_transparency_context_bind_window(
              context,
              default_window) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_transparency_context_get_support(
              context,
              &support) == BOXRIG_UI_OK);
    CHECK(support.window_bound);
    CHECK(!support.window_argb_compatible);
    CHECK(boxrig_x11_transparency_context_get_state(context, &state) ==
          BOXRIG_UI_OK);
    CHECK(state.capabilities.active_scopes ==
          BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE);
    CHECK(state.capabilities.recreate_required_scopes ==
          BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL);
    CHECK(state.reason ==
          BOXRIG_UI_TRANSPARENCY_REASON_RESOURCE_RECREATION_REQUIRED);
    CHECK(boxrig_x11_transparency_context_bind_window(
              context,
              argb_window) == BOXRIG_UI_OK);

    previous = state;
    config.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    CHECK(boxrig_x11_transparency_context_set_config(context, &config) ==
          BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_x11_transparency_context_get_state(context, &state) ==
          BOXRIG_UI_OK);
    CHECK(state.config.request == previous.config.request);
    CHECK(state.revision != 0);

    CHECK(snprintf(
              selection_name,
              sizeof(selection_name),
              "_NET_WM_CM_S%d",
              screen) > 0);
    selection = XInternAtom(display, selection_name, False);
    CHECK(selection != None);
    owner = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        1,
        1,
        0,
        0,
        0);
    CHECK(owner != None);
    XSetSelectionOwner(display, selection, owner, CurrentTime);
    CHECK(XGetSelectionOwner(display, selection) == owner);
    CHECK(process_selection_events(display, context, 1));
    CHECK(boxrig_x11_transparency_context_get_state(context, &state) ==
          BOXRIG_UI_OK);
    CHECK(state.effective ==
          BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA);
    CHECK(state.effective_scopes == all_scopes);
    CHECK(state.effective_background_opacity == 128);
    CHECK(state.revision > revision);
    revision = state.revision;
    CHECK(boxrig_x11_transparency_context_set_config(context, &config) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_x11_transparency_context_get_state(context, &state) ==
          BOXRIG_UI_OK);
    CHECK(state.config.request == BOXRIG_UI_TRANSPARENCY_REQUIRED);

    XDestroyWindow(display, owner);
    owner = None;
    CHECK(process_selection_events(display, context, 1));
    CHECK(boxrig_x11_transparency_context_get_state(context, &state) ==
          BOXRIG_UI_OK);
    CHECK(state.effective == BOXRIG_UI_TRANSPARENCY_UNAVAILABLE);
    CHECK(state.reason == BOXRIG_UI_TRANSPARENCY_REASON_SCOPE_INACTIVE);
    CHECK(state.revision > revision);
    revision = state.revision;

    config.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
    CHECK(boxrig_x11_transparency_context_set_config(context, &config) ==
          BOXRIG_UI_OK);
    owner_display = XOpenDisplay(DisplayString(display));
    CHECK(owner_display != NULL);
    remote_selection = XInternAtom(owner_display, selection_name, False);
    CHECK(remote_selection == selection);
    remote_owner = XCreateSimpleWindow(
        owner_display,
        RootWindow(owner_display, screen),
        0,
        0,
        1,
        1,
        0,
        0,
        0);
    CHECK(remote_owner != None);
    XSetSelectionOwner(
        owner_display,
        remote_selection,
        remote_owner,
        CurrentTime);
    XSync(owner_display, False);
    CHECK(process_selection_events(display, context, 1));
    CHECK(boxrig_x11_transparency_context_get_state(context, &state) ==
          BOXRIG_UI_OK);
    CHECK(state.effective ==
          BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA);
    CHECK(state.revision > revision);
    revision = state.revision;

    XCloseDisplay(owner_display);
    owner_display = NULL;
    remote_owner = None;
    CHECK(process_selection_events(display, context, 1));
    CHECK(boxrig_x11_transparency_context_get_state(context, &state) ==
          BOXRIG_UI_OK);
    CHECK(state.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
    CHECK(state.reason == BOXRIG_UI_TRANSPARENCY_REASON_SCOPE_INACTIVE);
    CHECK(state.revision > revision);

    config.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    CHECK(boxrig_x11_transparency_context_create(
              display,
              screen,
              &config,
              &rejected) == BOXRIG_UI_UNAVAILABLE);
    CHECK(rejected == NULL);

    config = boxrig_ui_transparency_config_default();
    CHECK(boxrig_x11_transparency_context_set_config(context, &config) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_x11_transparency_context_get_window_requirements(
              context,
              &requirements) == BOXRIG_UI_OK);
    CHECK(requirements.visual == DefaultVisual(display, screen));
    CHECK(requirements.depth == DefaultDepth(display, screen));
    CHECK(requirements.colormap == DefaultColormap(display, screen));
    CHECK(!requirements.premultiplied_argb32);

    success = true;
    boxrig_x11_scrollbar_destroy(scrollbar);
    boxrig_terminal_destroy(terminal);
    if (children != NULL) {
        XFree(children);
    }
    if (default_window != None) {
        XDestroyWindow(display, default_window);
    }
    if (argb_window != None) {
        XDestroyWindow(display, argb_window);
    }
    boxrig_x11_transparency_context_destroy(context);
    XSync(display, False);
    return success;
}

int main(void)
{
    Display *display = XOpenDisplay(NULL);

    if (display == NULL) {
        fprintf(stderr, "FAIL could not open X display\n");
        return 1;
    }
    if (!negotiate_and_track_compositor(display)) {
        cairo_debug_reset_static_data();
        XCloseDisplay(display);
        FcFini();
        return 1;
    }
    cairo_debug_reset_static_data();
    XCloseDisplay(display);
    FcFini();
    printf("PASS X11 transparency negotiates Visuals and compositor ownership\n");
    return 0;
}
