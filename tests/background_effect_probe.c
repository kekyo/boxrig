/* boxrig - Under MIT. Exercise each sample's owned background provider. */
#if defined(BOXRIG_PROBE_GTK3)
#include "../examples/boxrig_term/gtk3/application_internal.h"
#define OWNER boxrig_term_gtk3_application
#define COMPONENT BoxrigGtk3TerminalWidget
#define ROUTER boxrig_gtk3_terminal_widget_set_key_router
#define VIEW(owner) boxrig_gtk3_terminal_widget_view((owner)->widget)
#elif defined(BOXRIG_PROBE_GTK4)
#include "../examples/boxrig_term/gtk4/application_internal.h"
#define OWNER boxrig_term_gtk4_application
#define COMPONENT BoxrigGtk4TerminalWidget
#define ROUTER boxrig_gtk4_terminal_widget_set_key_router
#define VIEW(owner) boxrig_gtk4_terminal_widget_view((owner)->widget)
#elif defined(BOXRIG_PROBE_WAYLAND)
#include "../examples/boxrig_term/wayland/application_internal.h"
#define OWNER boxrig_term_wayland_application
#define COMPONENT boxrig_wayland_terminal_component
#define ROUTER boxrig_wayland_terminal_component_set_key_router
#define VIEW(owner) boxrig_wayland_terminal_component_view((owner)->component)
#else
#include "../examples/boxrig_term/x11/application_internal.h"
#define OWNER boxrig_term_x11_application
#define COMPONENT boxrig_x11_terminal_component
#define ROUTER boxrig_x11_terminal_component_set_key_router
#define VIEW(owner) boxrig_x11_terminal_component_view((owner)->component)
#endif
#include <assert.h>
#include <string.h>

#define JOIN_(a, b) a##b
#define JOIN(a, b) JOIN_(a, b)
static boxrig_input_key_router original_router;
static bool initialized;
boxrig_ui_status JOIN(__real_, ROUTER)(COMPONENT *, boxrig_input_key_router, void *);

static void feed(OWNER *owner, const char *text)
{
    size_t consumed, length = strlen(text);
    assert(boxrig_terminal_feed(owner->terminal, (const uint8_t *)text, length,
        &consumed) == BOXRIG_STATUS_OK && consumed == length);
}
static void set_effect(OWNER *owner, bool enabled)
{
    boxrig_ui_background_effect_request request = enabled ?
        BOXRIG_UI_BACKGROUND_EFFECT_PREFER : BOXRIG_UI_BACKGROUND_EFFECT_NONE;
#if defined(BOXRIG_PROBE_GTK3)
    int x, y;
    GtkWidget *widget = GTK_WIDGET(owner->widget);
    assert(gtk_widget_translate_coordinates(widget, GTK_WIDGET(owner->window), 0, 0, &x, &y));
    boxrig_ui_background_effect_rectangle region = {x, y,
        gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget)};
    owner->background_request = request;
    assert(boxrig_gtk3_background_effect_set(owner->background_effect, request, &region, 1) == BOXRIG_UI_OK);
#elif defined(BOXRIG_PROBE_GTK4)
    graphene_rect_t bounds;
    assert(gtk_widget_compute_bounds(GTK_WIDGET(owner->widget), GTK_WIDGET(owner->window), &bounds));
    double dx, dy;
    gtk_native_get_surface_transform(GTK_NATIVE(owner->window), &dx, &dy);
    boxrig_ui_background_effect_rectangle region = {(int)(bounds.origin.x - dx), (int)(bounds.origin.y - dy),
        (int)bounds.size.width, (int)bounds.size.height};
    owner->background_request = request;
    assert(boxrig_gtk4_background_effect_set(owner->background_effect, request, &region, 1) == BOXRIG_UI_OK);
#elif defined(BOXRIG_PROBE_WAYLAND)
    owner->background_request = request;
    assert(boxrig_wayland_background_effect_set(owner->background_effect, request,
        &owner->background_region, 1) == BOXRIG_UI_OK);
#else
    XWindowAttributes attributes;
    assert(XGetWindowAttributes(owner->display, owner->window, &attributes));
    boxrig_ui_background_effect_rectangle region = {0, 0, attributes.width, attributes.height};
    assert(boxrig_x11_background_effect_set(owner->background_effect, request, &region, 1) == BOXRIG_UI_OK);
#endif
}
static boxrig_input_key_route route(const boxrig_key_event *event, void *context)
{
    OWNER *owner = context;
    if (event->key != BOXRIG_KEY_F8 && event->key != BOXRIG_KEY_F9)
        return original_router(event, context);
    if (event->action == BOXRIG_KEY_PRESS) {
        if (!initialized) {
#if defined(BOXRIG_PROBE_GTK3) || defined(BOXRIG_PROBE_GTK4)
            /* The key router exists before the PTY's initial screen reset.
             * Wait for the helper's OSC title before installing the scene. */
            const char *title = gtk_window_get_title(GTK_WINDOW(owner->window));
            if (title == NULL || strcmp(title, "plan27-background-ready") != 0)
                return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
#endif
            feed(owner, "\033[0m\033[2J\033[H\033[38;2;255;255;255mBOXRIG foreground\r\n"
                "Background effect\r\n\033_Gf=32,s=1,v=1,a=T,c=12,r=4;AP8A/w==\033\\"
                "\033[8;1H\033]12;#FFFFFF\007\033[1 q");
            /* Opaque pixels under text/cursor isolate antialiasing from the
             * changing desktop. The remaining image pixels are transparent. */
            uint8_t rgba[128 * 128 * 4] = {0};
            for (size_t y = 0; y < 128; y++)
                for (size_t x = 0; x < 128; x++)
                    if (y < 24 || x < 8) rgba[(y * 128 + x) * 4 + 3] = 255;
            boxrig_ui_background_image image = {.width = 128, .height = 128,
                .stride = 128 * 4, .length = sizeof(rgba), .rgba = rgba,
                .opacity = 255, .fit = BOXRIG_UI_BACKGROUND_STRETCH};
            assert(boxrig_ui_view_set_background_image(VIEW(owner), &image) == BOXRIG_UI_OK);
            initialized = true;
        }
        bool enabled = event->key == BOXRIG_KEY_F9;
        set_effect(owner, enabled);
        feed(owner, enabled ? "\033]0;plan27-effect-on\007" : "\033]0;plan27-effect-off\007");
        printf("plan27-effect-%s\n", enabled ? "on" : "off");
        fflush(stdout);
    }
    return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
}
boxrig_ui_status JOIN(__wrap_, ROUTER)(COMPONENT *component, boxrig_input_key_router router, void *context)
{
    assert(original_router == NULL && router != NULL && context != NULL);
    original_router = router;
    return JOIN(__real_, ROUTER)(component, route, context);
}
