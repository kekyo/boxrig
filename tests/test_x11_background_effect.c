/* boxrig - Under MIT. */
#include "boxrig/x11/background_effect.h"
#include <X11/Xatom.h>
#include <assert.h>
#include <stdio.h>

static void property(Display *display, Window window, Atom atom, const unsigned long *expected, size_t count)
{
    Atom type; int format; unsigned long length, remaining; unsigned char *bytes;
    assert(XGetWindowProperty(display, window, atom, 0, 1024, False, AnyPropertyType,
        &type, &format, &length, &remaining, &bytes) == Success);
    if (expected == NULL) assert(type == None);
    else {
        assert(type == XA_CARDINAL && format == 32 && length == count && remaining == 0);
        for (size_t i = 0; i < count; i++) assert(((unsigned long *)bytes)[i] == expected[i]);
    }
    if (bytes != NULL) XFree(bytes);
}
static void dispatch(Display *display, boxrig_x11_background_effect *effect)
{
    XSync(display, False);
    bool observed = false;
    while (XPending(display)) {
        XEvent event;
        XNextEvent(display, &event);
        if (boxrig_x11_background_effect_process_event(effect, &event)) observed = true;
    }
    assert(observed);
}
int main(void)
{
    Display *display = XOpenDisplay(NULL);
    assert(display != NULL);
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    Window window = XCreateSimpleWindow(display, root, 0, 0, 200, 100, 0, 0, 0);
    Window compositor = XCreateSimpleWindow(display, root, 0, 0, 1, 1, 0, 0, 0);
    Atom blur = XInternAtom(display, "_KDE_NET_WM_BLUR_BEHIND_REGION", False);
    char selection_name[40];
    snprintf(selection_name, sizeof(selection_name), "_NET_WM_CM_S%d", screen);
    Atom selection = XInternAtom(display, selection_name, False);
    unsigned long old[] = {1, 2, 30, 40};
    XChangeProperty(display, window, blur, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)old, 4);
    boxrig_x11_background_effect *effect, *duplicate;
    assert(boxrig_x11_background_effect_create(display, window, &effect) == BOXRIG_UI_OK);
    assert(boxrig_x11_background_effect_create(display, window, &duplicate) == BOXRIG_UI_BUSY);
    boxrig_ui_background_effect_rectangle rectangles[] = {{10, 20, 100, 50}, {120, 20, 30, 50}};
    assert(boxrig_x11_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_PREFER, rectangles, 2) == BOXRIG_UI_OK);
    assert(!boxrig_x11_background_effect_get_state(effect).supported);
    property(display, window, blur, old, 4);
    XSetSelectionOwner(display, selection, compositor, CurrentTime);
    assert(boxrig_x11_background_effect_refresh(effect) == BOXRIG_UI_OK);
    assert(!boxrig_x11_background_effect_get_state(effect).supported);
    XChangeProperty(display, root, blur, XA_CARDINAL, 32, PropModeReplace, NULL, 0);
    dispatch(display, effect);
    assert(boxrig_x11_background_effect_get_state(effect).accepted);
    unsigned long expected[] = {10, 20, 100, 50, 120, 20, 30, 50};
    property(display, window, blur, expected, 8);
    rectangles[0] = (boxrig_ui_background_effect_rectangle){20, 40, 200, 100};
    assert(boxrig_x11_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE, rectangles, 1) == BOXRIG_UI_OK);
    unsigned long doubled[] = {20, 40, 200, 100};
    property(display, window, blur, doubled, 4);
    XSetSelectionOwner(display, selection, None, CurrentTime);
    dispatch(display, effect);
    property(display, window, blur, NULL, 0);
    assert(!boxrig_x11_background_effect_get_state(effect).accepted);
    XSetSelectionOwner(display, selection, compositor, CurrentTime);
    dispatch(display, effect);
    property(display, window, blur, doubled, 4);
    assert(boxrig_x11_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_NONE, NULL, 0) == BOXRIG_UI_OK);
    property(display, window, blur, NULL, 0);
    boxrig_x11_background_effect_destroy(effect);
    property(display, window, blur, old, 4);
    XDeleteProperty(display, root, blur);
    XDestroyWindow(display, window); XDestroyWindow(display, compositor); XCloseDisplay(display);
    puts("PASS X11 background region, device coordinates, capability loss, restart, removal and restore");
    return 0;
}
