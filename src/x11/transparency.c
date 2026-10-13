/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/x11/transparency.h"

#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct boxrig_x11_transparency_context {
    Display *display;
    int screen;
    Window root;
    Window tracker_window;
    Atom compositor_selection;
    int fixes_event_base;
    Visual *argb_visual;
    int argb_depth;
    Colormap argb_colormap;
    Window bound_window;
    boxrig_x11_transparency_support support;
    boxrig_ui_transparency_state state;
};

static bool visual_has_argb32(Display *display, Visual *visual, int depth)
{
    XRenderPictFormat *format;

    if (visual == NULL || depth != 32 || visual->class != TrueColor) {
        return false;
    }
    format = XRenderFindVisualFormat(display, visual);
    return format != NULL && format->type == PictTypeDirect &&
           format->depth == 32 && format->direct.alphaMask != 0;
}

static bool select_argb_visual(
    boxrig_x11_transparency_context *context)
{
    XVisualInfo template;
    XVisualInfo *visuals;
    int visual_count = 0;
    int index;

    memset(&template, 0, sizeof(template));
    template.screen = context->screen;
    template.depth = 32;
    template.class = TrueColor;
    visuals = XGetVisualInfo(
        context->display,
        VisualScreenMask | VisualDepthMask | VisualClassMask,
        &template,
        &visual_count);
    if (visuals == NULL) {
        return false;
    }
    for (index = 0; index < visual_count; index++) {
        if (visual_has_argb32(
                context->display,
                visuals[index].visual,
                visuals[index].depth)) {
            context->argb_visual = visuals[index].visual;
            context->argb_depth = visuals[index].depth;
            break;
        }
    }
    XFree(visuals);
    if (context->argb_visual == NULL) {
        return false;
    }
    context->argb_colormap = XCreateColormap(
        context->display,
        context->root,
        context->argb_visual,
        AllocNone);
    if (context->argb_colormap == None) {
        context->argb_visual = NULL;
        context->argb_depth = 0;
        return false;
    }
    return true;
}

static void probe_extensions(boxrig_x11_transparency_context *context)
{
    int event_base;
    int error_base;

    if (XCompositeQueryExtension(
            context->display,
            &event_base,
            &error_base) &&
        XCompositeQueryVersion(
            context->display,
            &context->support.composite_major_version,
            &context->support.composite_minor_version)) {
        context->support.composite_extension = true;
    }
    if (XFixesQueryExtension(
            context->display,
            &context->fixes_event_base,
            &error_base) &&
        XFixesQueryVersion(
            context->display,
            &context->support.fixes_major_version,
            &context->support.fixes_minor_version)) {
        context->support.fixes_extension = true;
    }
    if (XRenderQueryExtension(
            context->display,
            &event_base,
            &error_base) &&
        XRenderQueryVersion(
            context->display,
            &context->support.render_major_version,
            &context->support.render_minor_version)) {
        context->support.render_extension = true;
    }
}

static boxrig_ui_transparency_capabilities derive_capabilities(
    const boxrig_x11_transparency_context *context)
{
    boxrig_ui_transparency_capabilities capabilities;
    bool native_supported;

    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.premultiplied_argb32_target =
        context->support.render_extension && context->support.argb_visual;
    if (capabilities.premultiplied_argb32_target) {
        capabilities.supported_scopes |=
            BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
        capabilities.active_scopes |=
            BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    }
    native_supported = context->support.composite_extension &&
                       context->support.fixes_extension &&
                       context->support.render_extension &&
                       context->support.argb_visual;
    if (native_supported) {
        capabilities.supported_scopes |=
            BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
        if (context->support.window_bound &&
            !context->support.window_argb_compatible) {
            capabilities.recreate_required_scopes |=
                BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
        } else if (context->support.compositor_owner) {
            capabilities.active_scopes |=
                BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
        }
    }
    return capabilities;
}

static boxrig_ui_status update_resolved_state(
    boxrig_x11_transparency_context *context,
    bool *changed_out)
{
    boxrig_ui_transparency_capabilities capabilities =
        derive_capabilities(context);
    boxrig_ui_transparency_state resolved;
    boxrig_ui_status status = boxrig_ui_transparency_resolve(
        &context->state.config,
        &capabilities,
        &context->state,
        &resolved);

    if (status != BOXRIG_UI_OK && status != BOXRIG_UI_UNAVAILABLE) {
        return status;
    }
    *changed_out = resolved.revision != context->state.revision;
    context->state = resolved;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_transparency_context_create(
    Display *display,
    int screen,
    const boxrig_ui_transparency_config *config,
    boxrig_x11_transparency_context **context_out)
{
    boxrig_x11_transparency_context *context;
    boxrig_ui_transparency_capabilities capabilities;
    boxrig_ui_status status;
    char selection_name[64];
    int selection_name_length;

    if (display == NULL || screen < 0 || screen >= ScreenCount(display) ||
        config == NULL || context_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *context_out = NULL;
    context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    context->display = display;
    context->screen = screen;
    context->root = RootWindow(display, screen);
    probe_extensions(context);
    if (context->support.render_extension && select_argb_visual(context)) {
        context->support.argb_visual = true;
    }
    selection_name_length = snprintf(
        selection_name,
        sizeof(selection_name),
        "_NET_WM_CM_S%d",
        screen);
    if (selection_name_length <= 0 ||
        (size_t)selection_name_length >= sizeof(selection_name)) {
        boxrig_x11_transparency_context_destroy(context);
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    context->compositor_selection =
        XInternAtom(display, selection_name, False);
    if (context->compositor_selection == None) {
        boxrig_x11_transparency_context_destroy(context);
        return BOXRIG_UI_UNAVAILABLE;
    }
    context->support.compositor_owner =
        XGetSelectionOwner(display, context->compositor_selection) != None;
    if (context->support.fixes_extension) {
        context->tracker_window = XCreateWindow(
            display,
            context->root,
            -1,
            -1,
            1,
            1,
            0,
            0,
            InputOnly,
            CopyFromParent,
            0,
            NULL);
        if (context->tracker_window == None) {
            boxrig_x11_transparency_context_destroy(context);
            return BOXRIG_UI_UNAVAILABLE;
        }
        XFixesSelectSelectionInput(
            display,
            context->tracker_window,
            context->compositor_selection,
            XFixesSetSelectionOwnerNotifyMask |
                XFixesSelectionWindowDestroyNotifyMask |
                XFixesSelectionClientCloseNotifyMask);
    }
    capabilities = derive_capabilities(context);
    status = boxrig_ui_transparency_resolve(
        config,
        &capabilities,
        NULL,
        &context->state);
    if (status != BOXRIG_UI_OK) {
        boxrig_x11_transparency_context_destroy(context);
        return status;
    }
    *context_out = context;
    return BOXRIG_UI_OK;
}

void boxrig_x11_transparency_context_destroy(
    boxrig_x11_transparency_context *context)
{
    if (context == NULL) {
        return;
    }
    if (context->tracker_window != None) {
        XFixesSelectSelectionInput(
            context->display,
            context->tracker_window,
            context->compositor_selection,
            0);
        XDestroyWindow(context->display, context->tracker_window);
    }
    if (context->argb_colormap != None) {
        XFreeColormap(context->display, context->argb_colormap);
    }
    free(context);
}

boxrig_ui_status boxrig_x11_transparency_context_get_support(
    const boxrig_x11_transparency_context *context,
    boxrig_x11_transparency_support *support_out)
{
    if (context == NULL || support_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *support_out = context->support;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_transparency_context_get_state(
    const boxrig_x11_transparency_context *context,
    boxrig_ui_transparency_state *state_out)
{
    if (context == NULL || state_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *state_out = context->state;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_transparency_context_set_config(
    boxrig_x11_transparency_context *context,
    const boxrig_ui_transparency_config *config)
{
    boxrig_ui_transparency_capabilities capabilities;
    boxrig_ui_transparency_state resolved;
    boxrig_ui_status status;

    if (context == NULL || config == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    capabilities = derive_capabilities(context);
    status = boxrig_ui_transparency_resolve(
        config,
        &capabilities,
        &context->state,
        &resolved);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    context->state = resolved;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_transparency_context_get_window_requirements(
    const boxrig_x11_transparency_context *context,
    boxrig_x11_window_requirements *requirements_out)
{
    bool use_argb;

    if (context == NULL || requirements_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (context->state.config.request == BOXRIG_UI_TRANSPARENCY_REQUIRED &&
        context->state.effective == BOXRIG_UI_TRANSPARENCY_UNAVAILABLE) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    memset(requirements_out, 0, sizeof(*requirements_out));
    use_argb =
        context->state.config.request != BOXRIG_UI_TRANSPARENCY_OPAQUE &&
        context->support.render_extension && context->support.argb_visual;
    if (use_argb) {
        requirements_out->visual = context->argb_visual;
        requirements_out->depth = context->argb_depth;
        requirements_out->colormap = context->argb_colormap;
        requirements_out->attributes.background_pixel = 0;
        requirements_out->attributes.border_pixel = 0;
        requirements_out->premultiplied_argb32 = true;
    } else {
        requirements_out->visual =
            DefaultVisual(context->display, context->screen);
        requirements_out->depth =
            DefaultDepth(context->display, context->screen);
        requirements_out->colormap =
            DefaultColormap(context->display, context->screen);
        requirements_out->attributes.background_pixel =
            BlackPixel(context->display, context->screen);
        requirements_out->attributes.border_pixel =
            BlackPixel(context->display, context->screen);
    }
    requirements_out->attributes.colormap = requirements_out->colormap;
    requirements_out->attribute_mask =
        CWBackPixel | CWBorderPixel | CWColormap;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_transparency_context_bind_window(
    boxrig_x11_transparency_context *context,
    Window window)
{
    XWindowAttributes attributes;
    bool changed;

    if (context == NULL || window == None ||
        XGetWindowAttributes(context->display, window, &attributes) == 0 ||
        XScreenNumberOfScreen(attributes.screen) != context->screen) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    context->bound_window = window;
    context->support.window_bound = true;
    context->support.window_argb_compatible =
        context->support.render_extension &&
        visual_has_argb32(
            context->display,
            attributes.visual,
            attributes.depth);
    return update_resolved_state(context, &changed);
}

boxrig_ui_status boxrig_x11_transparency_context_refresh(
    boxrig_x11_transparency_context *context,
    bool *changed_out)
{
    if (context == NULL || changed_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    context->support.compositor_owner =
        XGetSelectionOwner(
            context->display,
            context->compositor_selection) != None;
    return update_resolved_state(context, changed_out);
}

int boxrig_x11_transparency_context_process_event(
    boxrig_x11_transparency_context *context,
    const XEvent *event,
    bool *changed_out)
{
    const XFixesSelectionNotifyEvent *selection_event;

    if (context == NULL || event == NULL || changed_out == NULL) {
        return -1;
    }
    *changed_out = false;
    if (!context->support.fixes_extension ||
        event->type != context->fixes_event_base + XFixesSelectionNotify) {
        return 0;
    }
    selection_event = (const XFixesSelectionNotifyEvent *)event;
    if (selection_event->window != context->tracker_window ||
        selection_event->selection != context->compositor_selection) {
        return 0;
    }
    if (boxrig_x11_transparency_context_refresh(
            context,
            changed_out) != BOXRIG_UI_OK) {
        return -1;
    }
    return 1;
}
