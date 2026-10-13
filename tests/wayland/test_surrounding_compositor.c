/* boxrig - Under MIT. */
#define _GNU_SOURCE
#include "boxrig/wayland.h"
#include "boxrig/ui/editable.h"
#include "input-method-v2-client-protocol.h"
#include "text-input-v3-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <errno.h>
#include <fontconfig/fontconfig.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)

typedef struct driver {
    struct wl_display *display;
    struct wl_event_queue *queue;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct xdg_wm_base *wm;
    struct zwp_input_method_manager_v2 *manager;
    struct zwp_input_method_v2 *im;
    boxrig_wayland_context *context;
    boxrig_wayland_terminal_component *component;
    boxrig_ui_editable_document *endpoint;
    boxrig_terminal *terminal;
    const char *mode;
    char document[128], surrounding[4001];
    uint64_t revision;
    uint32_t serial, cursor, anchor;
    unsigned int step, calls, accepted, activations;
    bool configured, active, context_seen, attached, engine_ready, geometry_changed;
} driver;

static void global(void *data, struct wl_registry *registry, uint32_t name,
    const char *interface, uint32_t version)
{
    driver *d = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        d->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 6 ? version : 6);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        d->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, wl_seat_interface.name) == 0 && d->seat == NULL)
        d->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
        d->wm = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    else if (strcmp(interface, zwp_input_method_manager_v2_interface.name) == 0)
        d->manager = wl_registry_bind(registry, name, &zwp_input_method_manager_v2_interface, 1);
}

static void global_removed(void *data, struct wl_registry *registry, uint32_t name)
{ (void)data; (void)registry; (void)name; }
static const struct wl_registry_listener registry_listener = {global, global_removed};

static void ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{ (void)data; xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener wm_listener = {ping};
static void configured(void *data, struct xdg_surface *surface, uint32_t serial)
{ driver *d = data; xdg_surface_ack_configure(surface, serial); d->configured = true; }
static const struct xdg_surface_listener surface_listener = {configured};
static void size_changed(void *data, struct xdg_toplevel *toplevel, int32_t width,
    int32_t height, struct wl_array *states)
{ (void)data; (void)toplevel; (void)width; (void)height; (void)states; }
static void closed(void *data, struct xdg_toplevel *toplevel)
{ (void)data; (void)toplevel; CHECK(false); }
static const struct xdg_toplevel_listener toplevel_listener = {.configure = size_changed, .close = closed};

static void activated(void *data, struct zwp_input_method_v2 *im)
{
    (void)im;
    driver *d = data;
    d->active = true;
    d->context_seen = false;
    d->activations++;
}
static void deactivated(void *data, struct zwp_input_method_v2 *im)
{ (void)im; ((driver *)data)->active = false; }
static void surrounding(void *data, struct zwp_input_method_v2 *im, const char *text,
    uint32_t cursor, uint32_t anchor)
{
    (void)im;
    driver *d = data;
    CHECK(strlen(text) <= 4000 && cursor <= strlen(text) && anchor <= strlen(text));
    strcpy(d->surrounding, text);
    d->cursor = cursor;
    d->anchor = anchor;
    d->context_seen = true;
    printf("engine surrounding=%s cursor=%u anchor=%u\n", text, cursor, anchor);
}
static void change_cause(void *data, struct zwp_input_method_v2 *im, uint32_t cause)
{ (void)data; (void)im; (void)cause; }
static void content_type(void *data, struct zwp_input_method_v2 *im, uint32_t hint, uint32_t purpose)
{
    (void)data; (void)im;
    CHECK(hint == ZWP_TEXT_INPUT_V3_CONTENT_HINT_MULTILINE && purpose == ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL);
}
static void done(void *data, struct zwp_input_method_v2 *im)
{
    (void)im;
    driver *d = data;
    d->serial++;
    if (d->active) d->engine_ready = true;
    printf("engine done serial=%u step=%u active=%d\n", d->serial, d->step, d->active);
}
static void unavailable(void *data, struct zwp_input_method_v2 *im)
{ (void)data; (void)im; CHECK(false); }
static const struct zwp_input_method_v2_listener im_listener = {
    activated, deactivated, surrounding, change_cause, content_type, done, unavailable
};

static boxrig_ui_status apply(const boxrig_ui_editable_replacement_request *request,
    const boxrig_ui_editable_document_snapshot *proposed, void *data)
{
    driver *d = data;
    CHECK(request->document_identity == 7 && request->base_document_revision == d->revision);
    d->calls++;
    if (strcmp(d->mode, "reject") == 0) return BOXRIG_UI_UNAVAILABLE;
    if (strcmp(d->mode, "busy") == 0 && d->calls == 1) return BOXRIG_UI_BUSY;
    size_t length = strlen(d->document), first = (size_t)request->range.offset;
    CHECK(first + request->range.length <= length);
    CHECK(length - request->range.length + request->replacement_length < sizeof(d->document));
    memmove(d->document + first + request->replacement_length,
        d->document + first + request->range.length, length - first - request->range.length + 1);
    if (request->replacement_length != 0)
        memcpy(d->document + first, request->replacement, request->replacement_length);
    d->revision = proposed->document_revision;
    d->accepted++;
    printf("owner accepted=%u document=%s\n", d->accepted, d->document);
    return BOXRIG_UI_OK;
}

static void attach(driver *d)
{
    d->attached = true;
    d->revision++;
    if (strcmp(d->mode, "readonly") != 0)
        CHECK(boxrig_ui_editable_document_set_apply_handler(d->endpoint, apply, d) == BOXRIG_UI_OK);
    size_t cursor = strcmp(d->mode, "empty") == 0 ? 0 : 9;
    boxrig_ui_editable_context_input snapshot = {7, d->revision, d->document, strlen(d->document),
        0, cursor, cursor};
    CHECK(boxrig_ui_editable_document_publish_context(d->endpoint, &snapshot) == BOXRIG_UI_OK);
}

static bool advance(driver *d)
{
    if (!d->engine_ready) return false;
    boxrig_composition_view view;
    CHECK(boxrig_composition_get_view(boxrig_wayland_terminal_component_composition(d->component),
        &view) == BOXRIG_COMPOSITION_OK);
    if (d->step == 0) {
        if (strcmp(d->mode, "attach") == 0 && !d->attached) {
            CHECK(!d->context_seen);
            d->engine_ready = false;
            attach(d);
            return false;
        }
        /* Earlier geometry commits may still deliver done after attachment.
         * Wait for the new activation and its surrounding state explicitly. */
        if (strcmp(d->mode, "attach") == 0 &&
            (d->activations < 2 || !d->context_seen)) return false;
        CHECK(d->context_seen == d->attached);
        if (d->attached) {
            CHECK(strcmp(d->surrounding, d->document) == 0);
            CHECK(d->cursor == (strcmp(d->mode, "empty") == 0 ? 0u : 9u) && d->anchor == d->cursor);
        }
        zwp_input_method_v2_set_preedit_string(d->im, "日本", 6, 6);
        printf("engine preedit serial=%u\n", d->serial);
        zwp_input_method_v2_commit(d->im, d->serial);
        d->step++;
    } else if (d->step == 1 && view.active && view.text_length == 6 && memcmp(view.text, "日本", 6) == 0) {
        if (d->attached && strcmp(d->mode, "empty") != 0)
            zwp_input_method_v2_delete_surrounding_text(d->im, 3, 0);
        zwp_input_method_v2_commit_string(d->im, "語");
        zwp_input_method_v2_set_preedit_string(d->im, "次", 3, 3);
        printf("engine replacement serial=%u\n", d->serial);
        zwp_input_method_v2_commit(d->im, d->serial);
        d->step++;
    } else if (d->step == 2) {
        if (strcmp(d->mode, "geometry") == 0 && !d->geometry_changed) return false;
        bool refused = strcmp(d->mode, "readonly") == 0 || strcmp(d->mode, "reject") == 0;
        const uint8_t *output;
        size_t bytes = boxrig_terminal_output_peek(d->terminal, &output);
        if ((!d->attached && bytes != 3) || (d->attached && !refused && d->accepted != 1) ||
            (refused && d->activations < 2)) return false;
        const char *expected = !d->attached || refused ? "前かな後" :
            strcmp(d->mode, "empty") == 0 ? "語" : "前か語後";
        CHECK(strcmp(d->document, expected) == 0);
        CHECK(bytes == (d->attached ? 0u : 3u));
        if (bytes != 0) CHECK(memcmp(output, "語", 3) == 0);
        if (d->attached && strcmp(d->surrounding, expected) != 0) return false;
        CHECK(d->accepted == (d->attached && !refused ? 1u : 0u));
        if (strcmp(d->mode, "busy") == 0) CHECK(d->calls == 2);
        zwp_input_method_v2_set_preedit_string(d->im, "", 0, 0);
        printf("engine cancel serial=%u\n", d->serial);
        zwp_input_method_v2_commit(d->im, d->serial);
        d->step++;
    } else if (d->step == 3 && !view.active) {
        if (strcmp(d->mode, "geometry") == 0) CHECK(d->geometry_changed);
        printf("PASS real compositor mode=%s activations=%u accepted=%u document=%s\n",
            d->mode, d->activations, d->accepted, d->document);
        return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    driver d = {.mode = argv[1]};
    setvbuf(stdout, NULL, _IONBF, 0);
    strcpy(d.document, strcmp(d.mode, "empty") == 0 ? "" : "前かな後");
    d.display = wl_display_connect(NULL);
    CHECK(d.display != NULL);
    d.queue = wl_display_create_queue(d.display);
    CHECK(d.queue != NULL);
    void *wrapper = wl_proxy_create_wrapper(d.display);
    CHECK(wrapper != NULL);
    wl_proxy_set_queue(wrapper, d.queue);
    d.registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    CHECK(wl_registry_add_listener(d.registry, &registry_listener, &d) == 0);
    CHECK(wl_display_roundtrip_queue(d.display, d.queue) >= 0);
    CHECK(d.compositor && d.shm && d.wm && d.seat && d.manager);
    CHECK(xdg_wm_base_add_listener(d.wm, &wm_listener, &d) == 0);
    d.im = zwp_input_method_manager_v2_get_input_method(d.manager, d.seat);
    CHECK(zwp_input_method_v2_add_listener(d.im, &im_listener, &d) == 0);
    CHECK(boxrig_wayland_context_create(d.display, d.queue, NULL, &d.context) == BOXRIG_UI_OK);
    boxrig_wayland_context_diagnostic diagnostic;
    do {
        CHECK(wl_display_roundtrip_queue(d.display, d.queue) >= 0);
        CHECK(boxrig_wayland_context_get_diagnostic(d.context, &diagnostic) == BOXRIG_UI_OK);
    } while (diagnostic.state == BOXRIG_WAYLAND_CONTEXT_INITIALIZING);
    CHECK(diagnostic.text_input_version == 1);
    struct wl_surface *parent = wl_compositor_create_surface(d.compositor);
    struct xdg_surface *surface = xdg_wm_base_get_xdg_surface(d.wm, parent);
    CHECK(xdg_surface_add_listener(surface, &surface_listener, &d) == 0);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(surface);
    CHECK(xdg_toplevel_add_listener(toplevel, &toplevel_listener, &d) == 0);
    xdg_toplevel_set_app_id(toplevel, "boxrig-step25");
    xdg_toplevel_set_title(toplevel, "boxrig surrounding context");
    wl_surface_commit(parent);
    while (!d.configured) CHECK(wl_display_dispatch_queue(d.display, d.queue) >= 0);
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_model *model = NULL;
    CHECK(boxrig_terminal_create(&terminal_config, &d.terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(d.terminal, &model) == BOXRIG_UI_OK);
    boxrig_wayland_terminal_component_config config = boxrig_wayland_terminal_component_config_default();
    config.view.blink.text_enabled = config.view.blink.cursor_enabled = false;
    config.keyboard_focus_fallback = true;
    CHECK(boxrig_wayland_terminal_component_create(d.context, parent, model, &config, &d.component) == BOXRIG_UI_OK);
    d.endpoint = boxrig_ui_view_editable_document(boxrig_wayland_terminal_component_view(d.component));
    if (strcmp(d.mode, "none") != 0 && strcmp(d.mode, "attach") != 0) attach(&d);
    boxrig_ui_component_allocation allocation = {.width = 320, .height = 180,
        .window_state = BOXRIG_UI_WINDOW_EMBEDDED};
    boxrig_ui_layout_result layout;
    CHECK(boxrig_wayland_terminal_component_allocate(d.component, &allocation, &layout) == BOXRIG_UI_OK);
    int fd = memfd_create("boxrig-surrounding-parent", MFD_CLOEXEC);
    size_t size = 320 * 180 * 4;
    CHECK(fd >= 0 && ftruncate(fd, (off_t)size) == 0);
    void *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(mapping != MAP_FAILED);
    memset(mapping, 0, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(d.shm, fd, (int32_t)size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, 320, 180, 320 * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_surface_attach(parent, buffer, 0, 0);
    wl_surface_damage_buffer(parent, 0, 0, 320, 180);
    xdg_surface_set_window_geometry(surface, 0, 0, 320, 180);
    wl_surface_commit(parent);
    CHECK(boxrig_wayland_terminal_component_set_visible(d.component, true) == BOXRIG_UI_OK);
    bool passed = false;
    while (!passed) {
        CHECK(wl_display_dispatch_queue_pending(d.display, d.queue) >= 0);
        boxrig_wayland_dispatch_result result;
        CHECK(boxrig_wayland_context_dispatch(d.context, BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT, &result) == BOXRIG_UI_OK);
        if (strcmp(d.mode, "geometry") == 0 && d.step == 2 && d.accepted == 1 &&
            strcmp(d.surrounding, d.document) == 0 && !d.geometry_changed) {
            /* Publish a changed candidate rectangle immediately before the
             * engine cancels, independent of compositor frame timing. */
            CHECK(boxrig_wayland_terminal_component_set_line_height_adjustment(d.component,
                2.0f) == BOXRIG_UI_OK);
            CHECK(boxrig_wayland_terminal_component_allocate(d.component, &allocation,
                &layout) == BOXRIG_UI_OK);
            d.geometry_changed = true;
        }
        boxrig_ui_component_frame_result frame;
        CHECK(boxrig_wayland_terminal_component_frame(d.component, &frame) == BOXRIG_UI_OK);
        boxrig_ui_layout_action layout_action;
        while (boxrig_wayland_terminal_component_take_layout_action(d.component, &layout_action)) {
            CHECK(layout_action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
            CHECK(boxrig_terminal_resize(d.terminal, layout_action.value.grid_resize.rows,
                layout_action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
            CHECK(boxrig_wayland_terminal_component_complete_grid_resize(d.component,
                layout_action.revision, true) == BOXRIG_UI_OK);
        }
        boxrig_wayland_host_action action;
        while (boxrig_wayland_terminal_component_take_host_action(d.component, &action)) {
            bool applied;
            wl_surface_commit(parent);
            CHECK(boxrig_wayland_terminal_component_complete_parent_commit(d.component, action.revision,
                &applied) == BOXRIG_UI_OK && applied);
        }
        /* An explicit sync wakes the loop after queued requests, including a
         * one-time BUSY retry, without treating elapsed time as success.
         * Host and engine share this connection: consume done for all host
         * updates before the engine commits with its latest serial. */
        CHECK(wl_display_roundtrip_queue(d.display, d.queue) >= 0);
        passed = advance(&d);
    }
    CHECK(boxrig_wayland_terminal_component_quiesce(d.component) == BOXRIG_UI_OK);
    boxrig_wayland_terminal_component_destroy(d.component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(d.terminal);
    zwp_input_method_v2_destroy(d.im);
    boxrig_wayland_context_destroy(d.context);
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(surface);
    wl_surface_destroy(parent);
    wl_buffer_destroy(buffer);
    munmap(mapping, size);
    zwp_input_method_manager_v2_destroy(d.manager);
    wl_seat_destroy(d.seat);
    xdg_wm_base_destroy(d.wm);
    wl_shm_destroy(d.shm);
    wl_compositor_destroy(d.compositor);
    wl_registry_destroy(d.registry);
    wl_event_queue_destroy(d.queue);
    wl_display_disconnect(d.display);
    FcFini();
    return 0;
}
