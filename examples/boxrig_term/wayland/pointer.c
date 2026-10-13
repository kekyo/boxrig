/* boxrig - Under MIT. */
#include "application_internal.h"

#include <stdlib.h>
#include <wayland-cursor.h>

struct boxrig_term_wayland_pointer {
    boxrig_term_wayland_application *application;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_surface *surface;
    struct wl_cursor_theme *theme;
    uint32_t name;
    uint32_t serial;
    wl_fixed_t x;
    wl_fixed_t y;
    int scale;
    bool inside;
    bool hand;
    bool initialized;
};

static void pointer_enter(void *data, struct wl_pointer *pointer,
    uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
    boxrig_term_wayland_pointer *state = data;
    boxrig_term_wayland_application *application = state->application;
    (void)pointer;
    state->inside = application->component != NULL && surface ==
        boxrig_wayland_terminal_component_surface(application->component);
    state->serial = serial;
    state->x = x;
    state->y = y;
    state->initialized = false;
    if (state->inside) application->active_pointer = state;
    application->frame_requested = true;
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
    uint32_t serial, struct wl_surface *surface)
{
    boxrig_term_wayland_pointer *state = data;
    (void)pointer; (void)serial; (void)surface;
    state->inside = false;
    if (state->application->active_pointer == state)
        boxrig_term_link_gesture_cancel(state->application->link_gesture);
    state->initialized = false;
    state->application->frame_requested = true;
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
    uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    boxrig_term_wayland_pointer *state = data;
    (void)pointer; (void)time;
    state->x = x;
    state->y = y;
    if (state->inside) state->application->active_pointer = state;
    state->application->frame_requested = true;
}

/* This observer only owns cursor presentation. The component's independent
 * pointer retains all button, wheel, routing and clipboard serial handling. */
static void pointer_button(void *data, struct wl_pointer *pointer,
    uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    (void)data; (void)pointer; (void)serial; (void)time; (void)button; (void)state;
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
    uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis
};

static void drop_pointer(boxrig_term_wayland_pointer *state)
{
    if (state->pointer != NULL) {
        if (wl_pointer_get_version(state->pointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
            wl_pointer_release(state->pointer);
        else wl_pointer_destroy(state->pointer);
        state->pointer = NULL;
    }
    state->inside = false;
    if (state->application->active_pointer == state)
        boxrig_term_link_gesture_cancel(state->application->link_gesture);
    state->initialized = false;
    if (state->application->active_pointer == state)
        state->application->active_pointer = NULL;
    state->application->frame_requested = true;
}

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    boxrig_term_wayland_pointer *state = data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) == 0) {
        drop_pointer(state);
    } else if (state->pointer == NULL) {
        state->pointer = wl_seat_get_pointer(seat);
        if (state->pointer == NULL ||
            wl_pointer_add_listener(state->pointer, &pointer_listener, state) != 0) {
            state->application->pointer_failed = true;
            state->application->frame_requested = true;
        }
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name
};

int boxrig_term_wayland_pointer_add(boxrig_term_wayland_application *application,
    struct wl_registry *registry, uint32_t name, uint32_t version)
{
    size_t index = 0;
    while (index < BOXRIG_WAYLAND_SEATS_MAXIMUM && application->pointers[index] != NULL)
        index++;
    if (index == BOXRIG_WAYLAND_SEATS_MAXIMUM) return 0;
    boxrig_term_wayland_pointer *state = calloc(1, sizeof(*state));
    if (state == NULL) return -1;
    state->application = application;
    state->name = name;
    application->pointers[index] = state;
    /* Version 3 supplies geometry and pointer release. Wheel frame extensions
     * are unnecessary for this observer and stay with the input component. */
    state->seat = wl_registry_bind(registry, name, &wl_seat_interface,
        version < 3 ? version : 3);
    if (state->seat == NULL) return -1;
    wl_proxy_set_queue((struct wl_proxy *)state->seat, application->queue);
    return wl_seat_add_listener(state->seat, &seat_listener, state);
}

void boxrig_term_wayland_pointer_remove(boxrig_term_wayland_application *application,
    uint32_t name)
{
    for (size_t index = 0; index < BOXRIG_WAYLAND_SEATS_MAXIMUM; index++) {
        boxrig_term_wayland_pointer *state = application->pointers[index];
        if (state == NULL || state->name != name) continue;
        drop_pointer(state);
        if (state->surface != NULL) wl_surface_destroy(state->surface);
        if (state->theme != NULL) wl_cursor_theme_destroy(state->theme);
        if (state->seat != NULL) wl_seat_destroy(state->seat);
        free(state);
        application->pointers[index] = NULL;
        return;
    }
}

void boxrig_term_wayland_pointer_destroy(boxrig_term_wayland_application *application)
{
    for (size_t index = 0; index < BOXRIG_WAYLAND_SEATS_MAXIMUM; index++) {
        if (application->pointers[index] != NULL)
            boxrig_term_wayland_pointer_remove(application, application->pointers[index]->name);
    }
}

static int set_pointer(boxrig_term_wayland_pointer *state, bool hand, int scale)
{
    if (state->initialized && state->hand == hand && state->scale == scale) return 0;
    if (state->surface == NULL) {
        state->surface = wl_compositor_create_surface(state->application->compositor);
        if (state->surface == NULL) return -1;
    }
    if (state->theme == NULL || state->scale != scale) {
        const char *size_text = getenv("XCURSOR_SIZE");
        char *end = NULL;
        long size = size_text == NULL ? 24 : strtol(size_text, &end, 10);
        if (size < 8 || size > 256 || (end != NULL && *end != '\0')) size = 24;
        struct wl_cursor_theme *theme = wl_cursor_theme_load(
            getenv("XCURSOR_THEME"), (int)size * scale, state->application->shm);
        if (theme == NULL) return -1;
        if (state->theme != NULL) wl_cursor_theme_destroy(state->theme);
        state->theme = theme;
        state->scale = scale;
    }
    struct wl_cursor *cursor = wl_cursor_theme_get_cursor(state->theme, hand ? "pointer" : "text");
    if (cursor == NULL) cursor = wl_cursor_theme_get_cursor(state->theme, hand ? "hand2" : "xterm");
    if (cursor == NULL || cursor->image_count == 0) return -1;
    struct wl_cursor_image *image = cursor->images[0];
    struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);
    if (buffer == NULL) return -1;
    wl_pointer_set_cursor(state->pointer, state->serial, state->surface,
        (int32_t)(image->hotspot_x / (uint32_t)scale),
        (int32_t)(image->hotspot_y / (uint32_t)scale));
    if (state->application->compositor_version >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION)
        wl_surface_set_buffer_scale(state->surface, scale);
    wl_surface_attach(state->surface, buffer, 0, 0);
    wl_surface_damage(state->surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(state->surface);
    state->hand = hand;
    state->initialized = true;
    return 0;
}

int boxrig_term_wayland_pointer_update(boxrig_term_wayland_application *application)
{
    if (application->pointer_failed) return -1;
    if (application->component == NULL || application->text_policy == NULL) return 0;
    boxrig_wayland_component_diagnostic diagnostic;
    if (boxrig_wayland_terminal_component_get_diagnostic(application->component,
            &diagnostic) != BOXRIG_UI_OK) return -1;
    bool focused = boxrig_ui_view_focused(
        boxrig_wayland_terminal_component_view(application->component));
    boxrig_term_wayland_pointer *active = application->active_pointer;
    if (active == NULL || !active->inside || !focused)
        boxrig_term_link_gesture_cancel(application->link_gesture);
    bool hand = false;
    int changed = boxrig_term_text_policy_refresh_pointer(application->text_policy,
        active != NULL && active->inside && focused,
        active == NULL ? 0 : (boxrig_ui_fixed)active->x * diagnostic.scale_numerator / 480,
        active == NULL ? 0 : (boxrig_ui_fixed)active->y * diagnostic.scale_numerator / 480,
        &hand);
    if (changed < 0) return -1;
    int scale = (int)((diagnostic.scale_numerator + 119) / 120);
    if (scale < 1 || scale > 16 || application->compositor_version <
            WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION) scale = 1;
    for (size_t index = 0; index < BOXRIG_WAYLAND_SEATS_MAXIMUM; index++) {
        boxrig_term_wayland_pointer *state = application->pointers[index];
        if (state == NULL || !state->inside) continue;
        bool seat_hand = state == active && hand;
        if (state != active && focused) {
            /* Hover belongs to the most recently moved pointer. Other seats
             * still resolve their own current link without changing hover. */
            boxrig_ui_hit_test_result hit;
            boxrig_term_link_target target = {0};
            if (boxrig_wayland_terminal_component_hit_test(application->component,
                    (boxrig_ui_fixed)state->x / 4, (boxrig_ui_fixed)state->y / 4,
                    &hit) != BOXRIG_UI_OK || boxrig_term_text_policy_query_link(
                    application->text_policy, &hit, &target) < 0) return -1;
            seat_hand = target.kind != BOXRIG_TERM_TEXT_POINTER_NONE;
            boxrig_term_link_target_clear(&target);
        }
        if (set_pointer(state, seat_hand, scale) < 0) return -1;
    }
    return changed;
}
