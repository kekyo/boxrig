/* boxrig - Under MIT. */
#include "boxrig/x11/background_effect.h"
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct boxrig_x11_background_effect {
    Display *display;
    Window window, root, tracker;
    Atom blur, selection, original_type;
    unsigned char *original;
    unsigned long original_count;
    long root_mask;
    int fixes_event;
    unsigned long region[1024];
    size_t count;
    bool changed;
    boxrig_ui_background_effect_state state;
    struct boxrig_x11_background_effect *next;
};
/* UI-thread registry coordinates property ownership and the shared root mask. */
static boxrig_x11_background_effect *owners;

static bool supported(boxrig_x11_background_effect *effect)
{
    if (XGetSelectionOwner(effect->display, effect->selection) == None) return false;
    Atom type; int format; unsigned long count, remaining; unsigned char *bytes = NULL;
    int result = XGetWindowProperty(effect->display, effect->root, effect->blur,
        0, 0, False, AnyPropertyType, &type, &format, &count, &remaining, &bytes);
    if (bytes != NULL) XFree(bytes);
    return result == Success && type != None;
}

boxrig_ui_status boxrig_x11_background_effect_create(Display *display, Window window,
    boxrig_x11_background_effect **out)
{
    if (out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *out = NULL;
    if (display == NULL || window == None) return BOXRIG_UI_INVALID_ARGUMENT;
    for (boxrig_x11_background_effect *e = owners; e != NULL; e = e->next)
        if (e->display == display && e->window == window) return BOXRIG_UI_BUSY;
    XWindowAttributes attributes;
    if (!XGetWindowAttributes(display, window, &attributes)) return BOXRIG_UI_UNAVAILABLE;
    boxrig_x11_background_effect *effect = calloc(1, sizeof(*effect));
    if (effect == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    effect->display = display; effect->window = window; effect->root = attributes.root;
    int error;
    if (!XFixesQueryExtension(display, &effect->fixes_event, &error)) { free(effect); return BOXRIG_UI_UNAVAILABLE; }
    effect->blur = XInternAtom(display, "_KDE_NET_WM_BLUR_BEHIND_REGION", False);
    char selection[40];
    snprintf(selection, sizeof(selection), "_NET_WM_CM_S%d", XScreenNumberOfScreen(attributes.screen));
    effect->selection = XInternAtom(display, selection, False);
    int format; unsigned long remaining;
    int result = XGetWindowProperty(display, window, effect->blur, 0, 1024, False, AnyPropertyType,
        &effect->original_type, &format, &effect->original_count, &remaining, &effect->original);
    if (result != Success || remaining != 0 || (effect->original_type != None &&
        (effect->original_type != XA_CARDINAL || format != 32))) {
        if (effect->original != NULL) XFree(effect->original);
        free(effect); return BOXRIG_UI_UNAVAILABLE;
    }
    XGetWindowAttributes(display, effect->root, &attributes);
    effect->root_mask = attributes.your_event_mask;
    for (boxrig_x11_background_effect *e = owners; e != NULL; e = e->next)
        if (e->display == display && e->root == effect->root) effect->root_mask = e->root_mask;
    XSelectInput(display, effect->root, attributes.your_event_mask | PropertyChangeMask);
    effect->tracker = XCreateSimpleWindow(display, effect->root, 0, 0, 1, 1, 0, 0, 0);
    XFixesSelectSelectionInput(display, effect->tracker, effect->selection,
        XFixesSetSelectionOwnerNotifyMask | XFixesSelectionWindowDestroyNotifyMask | XFixesSelectionClientCloseNotifyMask);
    effect->state.supported = supported(effect);
    effect->next = owners; owners = effect;
    *out = effect;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_background_effect_refresh(boxrig_x11_background_effect *effect)
{
    if (effect == NULL || effect->window == None) return BOXRIG_UI_INVALID_ARGUMENT;
    effect->state.supported = supported(effect);
    bool enabled = effect->state.request != BOXRIG_UI_BACKGROUND_EFFECT_NONE;
    effect->state.accepted = enabled && effect->state.supported;
    if (effect->state.accepted) {
        /* Xlib format 32 consumes native longs, including on LP64. */
        XChangeProperty(effect->display, effect->window, effect->blur, XA_CARDINAL, 32,
            PropModeReplace, (const unsigned char *)effect->region, (int)effect->count * 4);
        effect->changed = true;
    } else if (effect->changed || !enabled) {
        XDeleteProperty(effect->display, effect->window, effect->blur);
        effect->changed = true;
    }
    XFlush(effect->display);
    effect->state.reason = !enabled ? BOXRIG_UI_BACKGROUND_EFFECT_DISABLED : effect->state.accepted ?
        BOXRIG_UI_BACKGROUND_EFFECT_ACCEPTED : BOXRIG_UI_BACKGROUND_EFFECT_UNSUPPORTED;
    return enabled && !effect->state.supported && effect->state.request == BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE ?
        BOXRIG_UI_UNAVAILABLE : BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_background_effect_set(boxrig_x11_background_effect *effect,
    boxrig_ui_background_effect_request request,
    const boxrig_ui_background_effect_rectangle *rectangles, size_t count)
{
    if (effect == NULL || effect->window == None || request < BOXRIG_UI_BACKGROUND_EFFECT_NONE || request > BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE ||
        count > 256 || (count != 0 && rectangles == NULL)) return BOXRIG_UI_INVALID_ARGUMENT;
    for (size_t i = 0; i < count; i++)
        if (rectangles[i].width <= 0 || rectangles[i].height <= 0 ||
            (int64_t)rectangles[i].x + rectangles[i].width > INT32_MAX ||
            (int64_t)rectangles[i].y + rectangles[i].height > INT32_MAX) return BOXRIG_UI_INVALID_ARGUMENT;
    if (request == BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE && !supported(effect)) {
        if (effect->changed) (void)boxrig_x11_background_effect_refresh(effect);
        effect->state.supported = false;
        effect->state.reason = BOXRIG_UI_BACKGROUND_EFFECT_UNSUPPORTED;
        return BOXRIG_UI_UNAVAILABLE;
    }
    for (size_t i = 0; i < count; i++) {
        effect->region[4 * i] = (uint32_t)rectangles[i].x;
        effect->region[4 * i + 1] = (uint32_t)rectangles[i].y;
        effect->region[4 * i + 2] = (uint32_t)rectangles[i].width;
        effect->region[4 * i + 3] = (uint32_t)rectangles[i].height;
    }
    effect->count = count;
    effect->state.request = request;
    return boxrig_x11_background_effect_refresh(effect);
}

bool boxrig_x11_background_effect_process_event(boxrig_x11_background_effect *effect, const XEvent *event)
{
    if (effect == NULL || event == NULL || event->xany.display != effect->display) return false;
    if (event->type == DestroyNotify && event->xdestroywindow.window == effect->window) {
        effect->window = None;
        effect->state.accepted = false;
        effect->state.reason = BOXRIG_UI_BACKGROUND_EFFECT_UNSUPPORTED;
        return true;
    }
    bool changed = event->type == PropertyNotify && event->xproperty.window == effect->root && event->xproperty.atom == effect->blur;
    if (event->type == effect->fixes_event + XFixesSelectionNotify) {
        const XFixesSelectionNotifyEvent *selection = (const XFixesSelectionNotifyEvent *)event;
        changed = selection->window == effect->tracker && selection->selection == effect->selection;
    }
    if (changed) (void)boxrig_x11_background_effect_refresh(effect);
    return changed;
}

boxrig_ui_background_effect_state boxrig_x11_background_effect_get_state(const boxrig_x11_background_effect *effect)
{
    return effect == NULL ? (boxrig_ui_background_effect_state){0} : effect->state;
}

void boxrig_x11_background_effect_destroy(boxrig_x11_background_effect *effect)
{
    if (effect == NULL) return;
    if (effect->changed && effect->window != None) {
        if (effect->original_type == None) XDeleteProperty(effect->display, effect->window, effect->blur);
        else XChangeProperty(effect->display, effect->window, effect->blur, effect->original_type, 32,
            PropModeReplace, effect->original, (int)effect->original_count);
    }
    boxrig_x11_background_effect **link = &owners;
    while (*link != effect) link = &(*link)->next;
    *link = effect->next;
    bool shared = false;
    for (boxrig_x11_background_effect *e = owners; e != NULL; e = e->next)
        if (e->display == effect->display && e->root == effect->root) shared = true;
    if (!shared) {
        XWindowAttributes attributes;
        XGetWindowAttributes(effect->display, effect->root, &attributes);
        XSelectInput(effect->display, effect->root, (attributes.your_event_mask & ~PropertyChangeMask) |
            (effect->root_mask & PropertyChangeMask));
    }
    XDestroyWindow(effect->display, effect->tracker);
    XFlush(effect->display);
    if (effect->original != NULL) XFree(effect->original);
    free(effect);
}
