/* boxrig - Under MIT. */
#include "boxrig/gtk3/background_effect.h"
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

static void property(Display *display, Window window, Atom atom,
    const unsigned long *expected, size_t count)
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
static void wait_accepted(boxrig_gtk3_background_effect *effect, bool accepted)
{
    while (boxrig_gtk3_background_effect_get_state(effect).accepted != accepted)
        g_main_context_iteration(NULL, TRUE);
}
static void scale_changed(GObject *window, GParamSpec *property, gpointer data)
{
    (void)window; (void)property;
    *(bool *)data = true;
}
int main(void)
{
    alarm(60); /* Deadlock watchdog; capability notifications drive success. */
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    gtk_init(NULL, NULL);
    GdkDisplay *gdk = gdk_display_get_default();
    Display *display = gdk_x11_display_get_xdisplay(gdk);
    Window root = DefaultRootWindow(display);
    Window compositor = XCreateSimpleWindow(display, root, 0, 0, 1, 1, 0, 0, 0);
    Atom blur = XInternAtom(display, "_KDE_NET_WM_BLUR_BEHIND_REGION", False);
    char selection_name[40];
    snprintf(selection_name, sizeof(selection_name), "_NET_WM_CM_S%d", DefaultScreen(display));
    Atom selection = XInternAtom(display, selection_name, False);
    XSetSelectionOwner(display, selection, compositor, CurrentTime);
    XChangeProperty(display, root, blur, XA_CARDINAL, 32, PropModeReplace, NULL, 0);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWidget *fixed = gtk_fixed_new();
    GtkWidget *terminal = gtk_drawing_area_new();
    gtk_widget_set_size_request(terminal, 100, 50);
    gtk_container_add(GTK_CONTAINER(window), fixed);
    gtk_fixed_put(GTK_FIXED(fixed), terminal, 20, 30);
    gtk_widget_realize(window);
    Window xid = gdk_x11_window_get_xid(gtk_widget_get_window(window));
    unsigned long original[] = {1, 2, 30, 40};
    XChangeProperty(display, xid, blur, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)original, 4);
    boxrig_gtk3_background_effect *effect;
    assert(boxrig_gtk3_background_effect_create(GTK_WINDOW(window), NULL, NULL, &effect) == BOXRIG_UI_OK);
    boxrig_ui_background_effect_rectangle region = {20, 30, 100, 50};
    assert(boxrig_gtk3_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_PREFER, &region, 1) == BOXRIG_UI_OK);
    gtk_widget_show_all(window);
    assert(boxrig_gtk3_background_effect_get_state(effect).accepted);
    int x, y;
    assert(gtk_widget_translate_coordinates(terminal, window, 0, 0, &x, &y));
    assert(x == region.x && y == region.y);
    int scale = gtk_widget_get_scale_factor(window);
    unsigned long expected[] = {20 * scale, 30 * scale, 100 * scale, 50 * scale};
    property(display, xid, blur, expected, 4);

    int next_scale = scale == 1 ? 2 : 1;
    bool scale_notified = false;
    g_signal_connect_after(window, "notify::scale-factor", G_CALLBACK(scale_changed), &scale_notified);
    gdk_x11_display_set_window_scale(gdk, next_scale);
    while (!scale_notified) g_main_context_iteration(NULL, TRUE);
    assert(gtk_widget_get_scale_factor(window) == next_scale);
    unsigned long resized[] = {20 * next_scale, 30 * next_scale, 100 * next_scale, 50 * next_scale};
    property(display, xid, blur, resized, 4);
    if (next_scale == 2) {
        boxrig_ui_background_effect_rectangle huge = {INT32_MAX - 100, 0, 50, 20};
        assert(boxrig_gtk3_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_PREFER, &huge, 1) == BOXRIG_UI_INVALID_ARGUMENT);
        property(display, xid, blur, resized, 4);
    }
    /* An empty GTK region must not become KDE's whole-window marker. */
    assert(boxrig_gtk3_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_PREFER, NULL, 0) == BOXRIG_UI_OK);
    property(display, xid, blur, NULL, 0);
    assert(boxrig_gtk3_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE, &region, 1) == BOXRIG_UI_OK);
    gtk_widget_hide(window);
    property(display, xid, blur, original, 4);
    gtk_widget_show_all(window);
    property(display, xid, blur, resized, 4);

    XDeleteProperty(display, root, blur);
    XFlush(display);
    wait_accepted(effect, false);
    property(display, xid, blur, NULL, 0);
    assert(boxrig_gtk3_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE, &region, 1) == BOXRIG_UI_UNAVAILABLE);
    assert(boxrig_gtk3_background_effect_get_state(effect).request == BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE);
    XChangeProperty(display, root, blur, XA_CARDINAL, 32, PropModeReplace, NULL, 0);
    XFlush(display);
    wait_accepted(effect, true);
    property(display, xid, blur, resized, 4);
    XSetSelectionOwner(display, selection, None, CurrentTime);
    XFlush(display);
    wait_accepted(effect, false);
    property(display, xid, blur, NULL, 0);
    XSetSelectionOwner(display, selection, compositor, CurrentTime);
    XFlush(display);
    wait_accepted(effect, true);
    property(display, xid, blur, resized, 4);
    assert(boxrig_gtk3_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_NONE, NULL, 0) == BOXRIG_UI_OK);
    property(display, xid, blur, NULL, 0);
    boxrig_gtk3_background_effect_destroy(effect);
    property(display, xid, blur, original, 4);

    /* GTK can dispose its native window before the retained adapter is freed. */
    assert(boxrig_gtk3_background_effect_create(GTK_WINDOW(window), NULL, NULL, &effect) == BOXRIG_UI_OK);
    assert(boxrig_gtk3_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_PREFER, &region, 1) == BOXRIG_UI_OK);
    gtk_widget_destroy(window);
    boxrig_gtk3_background_effect_destroy(effect);
    XDeleteProperty(display, root, blur);
    XDestroyWindow(display, compositor);
    gdk_display_sync(gdk);
    while (g_main_context_iteration(NULL, FALSE)) {}
    alarm(0);
    puts("PASS GTK3 X11 region, scale changes, empty region, remap, compositor restart and restoration");
    return 0;
}
