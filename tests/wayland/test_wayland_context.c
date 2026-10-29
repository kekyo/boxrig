/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "../editable_fixture.h"
#include "boxrig/wayland.h"
#include "boxrig/ui/editable.h"
#include "../composition_fixture.h"
#include "../frontend_component_fixture.h"
#include "../../examples/wayland/external_scrollbar.h"
#include "../external_scrollbar_example.h"
#include "../frontend_conformance_cases.h"
#include "fractional-scale-v1-server-protocol.h"
#include "primary-selection-v1-server-protocol.h"
#include "text-input-v3-server-protocol.h"
#include "viewporter-server-protocol.h"

#include <fontconfig/fontconfig.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#include <ibus.h>
#pragma GCC diagnostic pop
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

/* Protocol fixtures select the compositor route explicitly. Automatic
 * discovery is tested below against controlled, independent IME services. */
static boxrig_wayland_context_config protocol_context_config(void)
{
    boxrig_wayland_context_config config = boxrig_wayland_context_config_default();
    config.input_method = BOXRIG_WAYLAND_IME_WAYLAND;
    return config;
}

static boxrig_ui_status create_protocol_context(struct wl_display *display,
    struct wl_event_queue *queue, const boxrig_wayland_context_config *config,
    boxrig_wayland_context **context_out)
{
    boxrig_wayland_context_config effective = config == NULL ? protocol_context_config() : *config;
    return boxrig_wayland_context_create(display, queue, &effective, context_out);
}

/* These protocol and layout fixtures do not run an animation scheduler.
 * Keep their display steady; the animation fixture below supplies a runtime. */
static boxrig_ui_status create_steady_component(
    boxrig_wayland_context *context, struct wl_surface *parent,
    boxrig_ui_model *model, const boxrig_wayland_terminal_component_config *config,
    boxrig_wayland_terminal_component **component_out)
{
    boxrig_wayland_terminal_component_config effective = config == NULL
        ? boxrig_wayland_terminal_component_config_default() : *config;
    effective.view.blink.text_enabled = false;
    effective.view.blink.cursor_enabled = false;
    /* These geometry fixtures exercise the full terminal allocation. */
    effective.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    return boxrig_wayland_terminal_component_create(context, parent, model, &effective, component_out);
}

typedef struct fake_server fake_server;

typedef struct fake_seat {
    fake_server *server;
    size_t index;
} fake_seat;

typedef struct fake_offer {
    fake_server *server;
    size_t index;
    bool primary;
} fake_offer;

struct fake_server {
    struct wl_display *server_display;
    struct wl_client *server_client;
    struct wl_display *client_display;
    struct wl_event_queue *client_queue;
    struct wl_global *compositor;
    struct wl_global *shm;
    struct wl_global *subcompositor;
    struct wl_global *output;
    struct wl_global *viewporter;
    struct wl_global *fractional_scale_manager;
    struct wl_global *text_input_manager;
    struct wl_global *data_device_manager;
    struct wl_global *primary_selection_manager;
    struct wl_global *seats[2];
    fake_seat seat_data[2];
    struct wl_resource *parent_resource;
    struct wl_resource *child_resource;
    struct wl_resource *child_resources[16];
    struct wl_resource *output_resource;
    struct wl_resource *viewport_resource;
    struct wl_resource *fractional_scale_resource;
    struct wl_resource *seat_resources[2];
    struct wl_resource *keyboard_resources[2];
    struct wl_resource *pointer_resources[2];
    struct wl_resource *text_input_resources[2];
    struct wl_resource *data_device_resources[2];
    struct wl_resource *primary_selection_device_resources[2];
    struct wl_resource *data_source_resources[8];
    struct wl_resource *primary_selection_source_resources[8];
    struct wl_resource *data_offer_resources[8];
    struct wl_resource *primary_selection_offer_resources[8];
    fake_offer offers[16];
    int offer_receive_descriptors[16];
    char offer_receive_mimes[16][32];
    size_t seat_bind_count;
    size_t keyboard_create_count;
    size_t keyboard_destroy_count;
    size_t pointer_create_count;
    size_t pointer_destroy_count;
    size_t text_input_create_count;
    size_t text_input_destroy_count;
    size_t text_input_enable_count;
    size_t text_input_disable_count;
    size_t text_input_content_type_count;
    size_t text_input_cursor_rectangle_count;
    size_t text_input_commit_count;
    size_t surrounding_count;
    bool surrounding_pending[2];
    bool surrounding_available[2];
    bool text_pending_enabled[2];
    bool text_current_enabled[2];
    bool surrounding_feature[2];
    char surrounding_text[2][4001];
    int32_t surrounding_cursor[2];
    int32_t surrounding_anchor[2];
    uint32_t pending_change_cause[2];
    uint32_t change_cause[2];
    size_t data_device_create_count;
    size_t data_device_destroy_count;
    size_t primary_selection_device_create_count;
    size_t primary_selection_device_destroy_count;
    size_t data_source_create_count;
    size_t data_source_destroy_count;
    size_t primary_selection_source_create_count;
    size_t primary_selection_source_destroy_count;
    size_t data_source_offer_count;
    size_t primary_selection_source_offer_count;
    size_t data_offer_create_count;
    size_t primary_selection_offer_create_count;
    size_t offer_receive_count;
    size_t offer_destroy_count;
    size_t data_selection_count[2];
    size_t primary_selection_count[2];
    uint32_t data_selection_serial[2];
    uint32_t primary_selection_serial[2];
    struct wl_resource *data_selection_source[2];
    struct wl_resource *primary_selection_source[2];
    char data_source_mimes[3][32];
    char primary_selection_source_mimes[3][32];
    uint32_t text_input_commit_serials[2];
    uint32_t text_input_content_hints;
    struct wl_resource *text_input_v2_focus;
    bool text_input_v2_rectangle_pending;
    bool text_input_v2_rectangle_committed;
    int32_t text_input_v2_committed_x;
    int32_t text_input_v2_applied_x;
    size_t text_input_v2_apply_count;
    int32_t text_input_cursor_x;
    int32_t text_input_cursor_y;
    int32_t text_input_cursor_width;
    int32_t text_input_cursor_height;
    size_t surface_create_count;
    size_t child_surface_count;
    size_t surface_destroy_count;
    size_t subsurface_create_count;
    size_t subsurface_destroy_count;
    size_t subsurface_position_count;
    size_t subsurface_sync_count;
    size_t subsurface_desync_count;
    size_t parent_commit_count;
    size_t child_commit_count;
    size_t child_null_attach_count;
    size_t child_attach_count;
    size_t child_dirty_buffer_count;
    size_t frame_callback_count;
    size_t viewport_create_count;
    size_t fractional_scale_create_count;
    size_t region_create_count;
    size_t region_destroy_count;
    size_t region_add_count;
    size_t opaque_region_count;
    size_t input_region_count;
    size_t request_sequence;
    size_t subsurface_sync_order;
    size_t child_attach_order;
    size_t child_commit_order;
    size_t parent_commit_order;
    size_t subsurface_desync_order;
    size_t null_attach_order;
    size_t child_unmap_commit_order;
    size_t subsurface_destroy_order;
    size_t child_surface_destroy_order;
    size_t shm_pool_create_count;
    size_t shm_pool_destroy_count;
    size_t shm_buffer_create_count;
    size_t shm_buffer_destroy_count;
    int32_t shm_pool_size;
    int32_t shm_width;
    int32_t shm_height;
    int32_t shm_stride;
    uint32_t shm_format;
    int32_t buffer_scale;
    int32_t viewport_width;
    int32_t viewport_height;
    int32_t committed_viewport_width;
    int32_t committed_viewport_height;
    int32_t dirty_x;
    int32_t dirty_y;
    int32_t dirty_width;
    int32_t dirty_height;
    int32_t region_width;
    int32_t region_height;
    bool opaque_region_nonnull;
    bool input_region_nonnull;
    bool advertise_argb;
    size_t last_attached_buffer;
    void *shm_mapping;
    size_t shm_mapping_size;
    size_t shm_buffer_offsets[16];
    struct wl_resource *shm_buffers[16];
    bool shm_buffer_busy[16];
    struct wl_resource *frame_callbacks[32];
};

typedef struct host_registry_state {
    struct wl_event_queue *queue;
    struct wl_compositor *compositor;
} host_registry_state;

typedef struct wayland_fixture_host wayland_fixture_host;

typedef struct wayland_fixture_component {
    wayland_fixture_host *host;
    boxrig_wayland_terminal_component *native;
    struct wl_resource *surface_resource;
    boxrig_ui_component_attachment attachment;
    boxrig_ui_frame_request_callback frame_request_callback;
    void *frame_request_context;
    bool visible;
    bool allocated;
    size_t input_capacity;
} wayland_fixture_component;

struct wayland_fixture_host {
    fake_server server;
    host_registry_state registry_state;
    struct wl_registry *registry;
    struct wl_surface *parent;
    boxrig_wayland_context *context;
    uint32_t seat_global_name;
};

static void destroy_resource(
    struct wl_client *client,
    struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void surface_attach(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *buffer,
    int32_t x,
    int32_t y)
{
    fake_server *server = wl_resource_get_user_data(resource);

    (void)client;
    (void)x;
    (void)y;
    if (resource != server->parent_resource) {
        if (buffer == NULL) {
            server->child_null_attach_count++;
            server->null_attach_order = ++server->request_sequence;
        } else {
            size_t index;

            for (index = 0; index < server->shm_buffer_create_count; index++) {
                if (server->shm_buffers[index] == buffer) {
                    break;
                }
            }
            CHECK(index < server->shm_buffer_create_count);
            CHECK(!server->shm_buffer_busy[index]);
            server->shm_buffer_busy[index] = true;
            server->last_attached_buffer = index;
            server->child_attach_count++;
            server->child_attach_order = ++server->request_sequence;
        }
    }
}

static void surface_rectangle(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height)
{
    (void)client;
    fake_server *server = wl_resource_get_user_data(resource);

    (void)x;
    (void)y;
    (void)width;
    (void)height;
    if (resource != server->parent_resource) {
        server->child_dirty_buffer_count++;
        server->dirty_x = x;
        server->dirty_y = y;
        server->dirty_width = width;
        server->dirty_height = height;
    }
}

static void surface_frame(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t callback)
{
    fake_server *server = wl_resource_get_user_data(resource);
    struct wl_resource *callback_resource;

    CHECK(server->frame_callback_count <
          sizeof(server->frame_callbacks) /
              sizeof(server->frame_callbacks[0]));
    callback_resource = wl_resource_create(
        client,
        &wl_callback_interface,
        1,
        callback);
    CHECK(callback_resource != NULL);
    server->frame_callbacks[server->frame_callback_count++] =
        callback_resource;
}

static void surface_opaque_region(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *region)
{
    fake_server *server = wl_resource_get_user_data(resource);

    (void)client;
    if (resource != server->parent_resource) {
        server->opaque_region_count++;
        server->opaque_region_nonnull = region != NULL;
    }
}

static void surface_input_region(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *region)
{
    fake_server *server = wl_resource_get_user_data(resource);

    (void)client;
    if (resource != server->parent_resource) {
        server->input_region_count++;
        server->input_region_nonnull = region != NULL;
    }
}

static void surface_commit(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_server *server = wl_resource_get_user_data(resource);

    (void)client;
    if (resource == server->text_input_v2_focus && server->text_input_v2_rectangle_committed) {
        server->text_input_v2_applied_x = server->text_input_v2_committed_x;
        server->text_input_v2_apply_count++;
        server->text_input_v2_rectangle_committed = false;
    }
    if (resource == server->parent_resource) {
        server->parent_commit_count++;
        server->parent_commit_order = ++server->request_sequence;
    } else {
        server->child_commit_count++;
        server->child_commit_order = ++server->request_sequence;
        server->committed_viewport_width = server->viewport_width;
        server->committed_viewport_height = server->viewport_height;
        if (server->child_null_attach_count != 0) {
            server->child_unmap_commit_order =
                server->child_commit_order;
        }
    }
}

static void surface_integer(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t value)
{
    (void)client;
    (void)resource;
    (void)value;
}

static void surface_buffer_scale(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t value)
{
    fake_server *server = wl_resource_get_user_data(resource);

    (void)client;
    if (resource != server->parent_resource) {
        server->buffer_scale = value;
    }
}

static void surface_offset(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t x,
    int32_t y)
{
    (void)client;
    (void)resource;
    (void)x;
    (void)y;
}

static void surface_destroy(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_server *server = wl_resource_get_user_data(resource);

    (void)client;
    server->surface_destroy_count++;
    if (resource != server->parent_resource) {
        server->child_surface_destroy_order = ++server->request_sequence;
    }
    wl_resource_destroy(resource);
}

static const struct wl_surface_interface surface_implementation = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_rectangle,
    .frame = surface_frame,
    .set_opaque_region = surface_opaque_region,
    .set_input_region = surface_input_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_integer,
    .set_buffer_scale = surface_buffer_scale,
    .damage_buffer = surface_rectangle,
    .offset = surface_offset,
};

static void region_rectangle(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height)
{
    fake_server *server = wl_resource_get_user_data(resource);

    (void)client;
    (void)x;
    (void)y;
    server->region_add_count++;
    server->region_width = width;
    server->region_height = height;
}

static void region_destroy(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_server *server = wl_resource_get_user_data(resource);

    (void)client;
    server->region_destroy_count++;
    wl_resource_destroy(resource);
}

static const struct wl_region_interface region_implementation = {
    .destroy = region_destroy,
    .add = region_rectangle,
    .subtract = region_rectangle,
};

static void compositor_create_surface(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id)
{
    fake_server *server = wl_resource_get_user_data(resource);
    struct wl_resource *surface = wl_resource_create(
        client,
        &wl_surface_interface,
        wl_resource_get_version(resource),
        id);

    CHECK(surface != NULL);
    wl_resource_set_implementation(
        surface,
        &surface_implementation,
        server,
        NULL);
    server->surface_create_count++;
    if (server->parent_resource == NULL) {
        server->parent_resource = surface;
    } else {
        server->child_resource = surface;
        CHECK(server->child_surface_count < 16);
        server->child_resources[server->child_surface_count++] = surface;
    }
}

static void compositor_create_region(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id)
{
    struct wl_resource *region = wl_resource_create(
        client,
        &wl_region_interface,
        1,
        id);

    CHECK(region != NULL);
    wl_resource_set_implementation(
        region,
        &region_implementation,
        wl_resource_get_user_data(resource),
        NULL);
    ((fake_server *)wl_resource_get_user_data(resource))->region_create_count++;
}

static const struct wl_compositor_interface compositor_implementation = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void bind_compositor(
    struct wl_client *client,
    void *data,
    uint32_t version,
    uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(
        client,
        &wl_compositor_interface,
        (int)version,
        id);

    CHECK(resource != NULL);
    wl_resource_set_implementation(
        resource,
        &compositor_implementation,
        data,
        NULL);
}

static void subsurface_position(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t x,
    int32_t y)
{
    fake_server *server = wl_resource_get_user_data(resource);

    (void)client;
    (void)x;
    (void)y;
    server->subsurface_position_count++;
}

static void subsurface_sibling(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *sibling)
{
    (void)client;
    (void)resource;
    (void)sibling;
}

static void subsurface_sync(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_server *server = wl_resource_get_user_data(resource);

    (void)client;
    server->subsurface_sync_count++;
    server->subsurface_sync_order = ++server->request_sequence;
}

static void subsurface_desync(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_server *server = wl_resource_get_user_data(resource);

    (void)client;
    server->subsurface_desync_count++;
    server->subsurface_desync_order = ++server->request_sequence;
}

static void subsurface_destroy(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_server *server = wl_resource_get_user_data(resource);

    (void)client;
    server->subsurface_destroy_count++;
    server->subsurface_destroy_order = ++server->request_sequence;
    wl_resource_destroy(resource);
}

static const struct wl_subsurface_interface subsurface_implementation = {
    .destroy = subsurface_destroy,
    .set_position = subsurface_position,
    .place_above = subsurface_sibling,
    .place_below = subsurface_sibling,
    .set_sync = subsurface_sync,
    .set_desync = subsurface_desync,
};

static void subcompositor_get_subsurface(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id,
    struct wl_resource *surface,
    struct wl_resource *parent)
{
    fake_server *server = wl_resource_get_user_data(resource);
    struct wl_resource *subsurface;

    (void)surface;
    (void)parent;
    subsurface = wl_resource_create(
        client,
        &wl_subsurface_interface,
        1,
        id);
    CHECK(subsurface != NULL);
    wl_resource_set_implementation(
        subsurface,
        &subsurface_implementation,
        server,
        NULL);
    server->subsurface_create_count++;
}

static const struct wl_subcompositor_interface subcompositor_implementation = {
    .destroy = destroy_resource,
    .get_subsurface = subcompositor_get_subsurface,
};

static void bind_subcompositor(
    struct wl_client *client,
    void *data,
    uint32_t version,
    uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(
        client,
        &wl_subcompositor_interface,
        (int)version,
        id);

    CHECK(resource != NULL);
    wl_resource_set_implementation(
        resource,
        &subcompositor_implementation,
        data,
        NULL);
}

static void shm_buffer_destroy(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_server *server = wl_resource_get_user_data(resource);
    size_t index;

    (void)client;
    for (index = 0; index < server->shm_buffer_create_count; index++) {
        if (server->shm_buffers[index] == resource) {
            server->shm_buffers[index] = NULL;
            server->shm_buffer_busy[index] = false;
            break;
        }
    }
    server->shm_buffer_destroy_count++;
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface shm_buffer_implementation = {
    .destroy = shm_buffer_destroy,
};

static void shm_pool_create_buffer(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id,
    int32_t offset,
    int32_t width,
    int32_t height,
    int32_t stride,
    uint32_t format)
{
    fake_server *server = wl_resource_get_user_data(resource);
    struct wl_resource *buffer;

    CHECK(offset >= 0 && width > 0 && height > 0 && stride >= width * 4);
    buffer = wl_resource_create(client, &wl_buffer_interface, 1, id);
    CHECK(buffer != NULL);
    wl_resource_set_implementation(
        buffer,
        &shm_buffer_implementation,
        server,
        NULL);
    server->shm_buffer_create_count++;
    CHECK(server->shm_buffer_create_count <= 16);
    server->shm_buffers[server->shm_buffer_create_count - 1] = buffer;
    server->shm_buffer_offsets[server->shm_buffer_create_count - 1] =
        (size_t)offset;
    server->shm_width = width;
    server->shm_height = height;
    server->shm_stride = stride;
    server->shm_format = format;
}

static void complete_frame(fake_server *server, size_t index)
{
    CHECK(index < server->frame_callback_count);
    CHECK(server->frame_callbacks[index] != NULL);
    wl_callback_send_done(server->frame_callbacks[index], (uint32_t)index);
    wl_resource_destroy(server->frame_callbacks[index]);
    server->frame_callbacks[index] = NULL;
}

static void release_buffer(fake_server *server, size_t index)
{
    CHECK(index < server->shm_buffer_create_count);
    CHECK(server->shm_buffer_busy[index]);
    wl_buffer_send_release(server->shm_buffers[index]);
    server->shm_buffer_busy[index] = false;
}

static void shm_pool_destroy(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_server *server = wl_resource_get_user_data(resource);

    (void)client;
    server->shm_pool_destroy_count++;
    wl_resource_destroy(resource);
}

static void shm_pool_resize(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t size)
{
    (void)client;
    (void)resource;
    (void)size;
}

static const struct wl_shm_pool_interface shm_pool_implementation = {
    .create_buffer = shm_pool_create_buffer,
    .destroy = shm_pool_destroy,
    .resize = shm_pool_resize,
};

static void shm_create_pool(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id,
    int32_t fd,
    int32_t size)
{
    fake_server *server = wl_resource_get_user_data(resource);
    struct wl_resource *pool;

    CHECK(fd >= 0 && size > 0);
    if (server->shm_mapping != NULL) {
        CHECK(munmap(server->shm_mapping, server->shm_mapping_size) == 0);
    }
    server->shm_mapping = mmap(
        NULL,
        (size_t)size,
        PROT_READ,
        MAP_SHARED,
        fd,
        0);
    CHECK(server->shm_mapping != MAP_FAILED);
    server->shm_mapping_size = (size_t)size;
    close(fd);
    pool = wl_resource_create(client, &wl_shm_pool_interface, 1, id);
    CHECK(pool != NULL);
    wl_resource_set_implementation(
        pool,
        &shm_pool_implementation,
        server,
        NULL);
    server->shm_pool_create_count++;
    server->shm_pool_size = size;
}

static const struct wl_shm_interface shm_implementation = {
    .create_pool = shm_create_pool,
};

static void bind_shm(
    struct wl_client *client,
    void *data,
    uint32_t version,
    uint32_t id)
{
    struct wl_resource *resource;

    resource = wl_resource_create(client, &wl_shm_interface, (int)version, id);
    CHECK(resource != NULL);
    wl_resource_set_implementation(
        resource,
        &shm_implementation,
        data,
        NULL);
    if (((fake_server *)data)->advertise_argb) {
        wl_shm_send_format(resource, WL_SHM_FORMAT_ARGB8888);
    }
    wl_shm_send_format(resource, WL_SHM_FORMAT_XRGB8888);
}

static const struct wl_output_interface output_implementation = {
    .release = destroy_resource,
};

static void bind_output(
    struct wl_client *client,
    void *data,
    uint32_t version,
    uint32_t id)
{
    fake_server *server = data;
    struct wl_resource *resource = wl_resource_create(
        client,
        &wl_output_interface,
        (int)(version < 4 ? version : 4),
        id);

    CHECK(resource != NULL);
    wl_resource_set_implementation(
        resource,
        &output_implementation,
        server,
        NULL);
    server->output_resource = resource;
    wl_output_send_scale(resource, 2);
    if (wl_resource_get_version(resource) >= WL_OUTPUT_DONE_SINCE_VERSION) {
        wl_output_send_done(resource);
    }
}

static void viewport_set_source(
    struct wl_client *client,
    struct wl_resource *resource,
    wl_fixed_t x,
    wl_fixed_t y,
    wl_fixed_t width,
    wl_fixed_t height)
{
    (void)client;
    (void)resource;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void viewport_set_destination(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t width,
    int32_t height)
{
    fake_server *server = wl_resource_get_user_data(resource);

    (void)client;
    server->viewport_width = width;
    server->viewport_height = height;
}

static const struct wp_viewport_interface viewport_implementation = {
    .destroy = destroy_resource,
    .set_source = viewport_set_source,
    .set_destination = viewport_set_destination,
};

static void viewporter_get_viewport(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id,
    struct wl_resource *surface)
{
    fake_server *server = wl_resource_get_user_data(resource);
    struct wl_resource *viewport;

    (void)surface;
    viewport = wl_resource_create(
        client,
        &wp_viewport_interface,
        1,
        id);
    CHECK(viewport != NULL);
    wl_resource_set_implementation(
        viewport,
        &viewport_implementation,
        server,
        NULL);
    server->viewport_resource = viewport;
    server->viewport_create_count++;
}

static const struct wp_viewporter_interface viewporter_implementation = {
    .destroy = destroy_resource,
    .get_viewport = viewporter_get_viewport,
};

static void bind_viewporter(
    struct wl_client *client,
    void *data,
    uint32_t version,
    uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(
        client,
        &wp_viewporter_interface,
        (int)version,
        id);

    CHECK(resource != NULL);
    wl_resource_set_implementation(
        resource,
        &viewporter_implementation,
        data,
        NULL);
}

static const struct wp_fractional_scale_v1_interface
    fractional_scale_implementation = {
        .destroy = destroy_resource,
};

static void fractional_scale_manager_get(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id,
    struct wl_resource *surface)
{
    fake_server *server = wl_resource_get_user_data(resource);
    struct wl_resource *fractional_scale;

    (void)surface;
    fractional_scale = wl_resource_create(
        client,
        &wp_fractional_scale_v1_interface,
        1,
        id);
    CHECK(fractional_scale != NULL);
    wl_resource_set_implementation(
        fractional_scale,
        &fractional_scale_implementation,
        server,
        NULL);
    server->fractional_scale_resource = fractional_scale;
    server->fractional_scale_create_count++;
}

static const struct wp_fractional_scale_manager_v1_interface
    fractional_scale_manager_implementation = {
        .destroy = destroy_resource,
        .get_fractional_scale = fractional_scale_manager_get,
};

static void text_input_destroy(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_seat *seat = wl_resource_get_user_data(resource);

    (void)client;
    seat->server->text_input_destroy_count++;
    seat->server->text_input_resources[seat->index] = NULL;
    wl_resource_destroy(resource);
}

static void text_input_enable(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_seat *seat = wl_resource_get_user_data(resource);

    (void)client;
    seat->server->text_input_enable_count++;
    seat->server->text_pending_enabled[seat->index] = true;
    seat->server->surrounding_pending[seat->index] = false;
    if (wl_resource_get_version(resource) >= 2) {
        seat->server->text_input_v2_rectangle_pending = false;
        seat->server->text_input_v2_rectangle_committed = false;
    }
}

static void text_input_disable(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_seat *seat = wl_resource_get_user_data(resource);

    (void)client;
    seat->server->text_input_disable_count++;
    seat->server->text_pending_enabled[seat->index] = false;
    seat->server->surrounding_pending[seat->index] = false;
    if (wl_resource_get_version(resource) >= 2) {
        seat->server->text_input_v2_rectangle_pending = false;
        seat->server->text_input_v2_rectangle_committed = false;
    }
}

static void text_input_set_surrounding_text(
    struct wl_client *client,
    struct wl_resource *resource,
    const char *text,
    int32_t cursor,
    int32_t anchor)
{
    (void)client;
    fake_seat *seat = wl_resource_get_user_data(resource);
    CHECK(strlen(text) <= 4000);
    CHECK(cursor >= 0 && (size_t)cursor <= strlen(text));
    CHECK(anchor >= 0 && (size_t)anchor <= strlen(text));
    seat->server->surrounding_count++;
    seat->server->surrounding_pending[seat->index] = true;
    strcpy(seat->server->surrounding_text[seat->index], text);
    seat->server->surrounding_cursor[seat->index] = cursor;
    seat->server->surrounding_anchor[seat->index] = anchor;
}

static void text_input_set_text_change_cause(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t cause)
{
    (void)client;
    fake_seat *seat = wl_resource_get_user_data(resource);
    seat->server->pending_change_cause[seat->index] = cause;
}

static void text_input_set_content_type(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t hint,
    uint32_t purpose)
{
    fake_seat *seat = wl_resource_get_user_data(resource);

    (void)client;
    CHECK(hint == (ZWP_TEXT_INPUT_V3_CONTENT_HINT_MULTILINE |
        (wl_resource_get_version(resource) >= 2 ?
            ZWP_TEXT_INPUT_V3_CONTENT_HINT_PREEDIT_SHOWN : 0)));
    CHECK(purpose == ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL);
    seat->server->text_input_content_hints = hint;
    seat->server->text_input_content_type_count++;
}

static void text_input_set_cursor_rectangle(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height)
{
    fake_seat *seat = wl_resource_get_user_data(resource);

    (void)client;
    seat->server->text_input_cursor_rectangle_count++;
    seat->server->text_input_cursor_x = x;
    seat->server->text_input_cursor_y = y;
    seat->server->text_input_cursor_width = width;
    seat->server->text_input_cursor_height = height;
    if (wl_resource_get_version(resource) >= 2)
        seat->server->text_input_v2_rectangle_pending = true;
}

static void text_input_commit_state(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_seat *seat = wl_resource_get_user_data(resource);

    (void)client;
    seat->server->text_input_commit_count++;
    seat->server->text_input_commit_serials[seat->index]++;
    if (seat->server->text_input_v2_rectangle_pending) {
        seat->server->text_input_v2_committed_x = seat->server->text_input_cursor_x;
        seat->server->text_input_v2_rectangle_committed = true;
        seat->server->text_input_v2_rectangle_pending = false;
    }
    /* wlroots 0.17 latches optional features only across an enabled-state
     * transition. Re-enabling alone cannot add surrounding support there. */
    size_t index = seat->index;
    if (!seat->server->text_pending_enabled[index])
        seat->server->surrounding_feature[index] = false;
    else if (!seat->server->text_current_enabled[index])
        seat->server->surrounding_feature[index] = seat->server->surrounding_pending[index];
    seat->server->text_current_enabled[index] = seat->server->text_pending_enabled[index];
    seat->server->surrounding_available[index] = seat->server->surrounding_feature[index] &&
        seat->server->surrounding_pending[index];
    seat->server->change_cause[seat->index] = seat->server->pending_change_cause[seat->index];
    seat->server->pending_change_cause[seat->index] = ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD;
}

static const struct zwp_text_input_v3_interface text_input_implementation = {
    .destroy = text_input_destroy,
    .enable = text_input_enable,
    .disable = text_input_disable,
    .set_surrounding_text = text_input_set_surrounding_text,
    .set_text_change_cause = text_input_set_text_change_cause,
    .set_content_type = text_input_set_content_type,
    .set_cursor_rectangle = text_input_set_cursor_rectangle,
    .commit = text_input_commit_state,
};

static void text_input_manager_get(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id,
    struct wl_resource *seat_resource)
{
    fake_server *server = wl_resource_get_user_data(resource);
    fake_seat *seat = wl_resource_get_user_data(seat_resource);
    struct wl_resource *text_input = wl_resource_create(
        client,
        &zwp_text_input_v3_interface,
        wl_resource_get_version(resource),
        id);

    CHECK(text_input != NULL);
    CHECK(seat != NULL && seat->server == server);
    wl_resource_set_implementation(
        text_input,
        &text_input_implementation,
        seat,
        NULL);
    server->text_input_resources[seat->index] = text_input;
    server->text_input_create_count++;
}

static const struct zwp_text_input_manager_v3_interface
    text_input_manager_implementation = {
        .destroy = destroy_resource,
        .get_text_input = text_input_manager_get,
};

static void bind_text_input_manager(
    struct wl_client *client,
    void *data,
    uint32_t version,
    uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(
        client,
        &zwp_text_input_manager_v3_interface,
        (int)version,
        id);

    CHECK(resource != NULL);
    wl_resource_set_implementation(
        resource,
        &text_input_manager_implementation,
        data,
        NULL);
}

static void data_device_start_drag(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *source,
    struct wl_resource *origin,
    struct wl_resource *icon,
    uint32_t serial)
{
    (void)client;
    (void)resource;
    (void)source;
    (void)origin;
    (void)icon;
    (void)serial;
}

static void data_device_set_selection(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *source,
    uint32_t serial)
{
    fake_seat *seat = wl_resource_get_user_data(resource);

    (void)client;
    seat->server->data_selection_count[seat->index]++;
    seat->server->data_selection_source[seat->index] = source;
    seat->server->data_selection_serial[seat->index] = serial;
}

static void data_device_release(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_seat *seat = wl_resource_get_user_data(resource);

    (void)client;
    seat->server->data_device_destroy_count++;
    seat->server->data_device_resources[seat->index] = NULL;
    wl_resource_destroy(resource);
}

static const struct wl_data_device_interface data_device_implementation = {
    .start_drag = data_device_start_drag,
    .set_selection = data_device_set_selection,
    .release = data_device_release,
};

static void data_source_offer(
    struct wl_client *client,
    struct wl_resource *resource,
    const char *mime_type)
{
    fake_server *server = wl_resource_get_user_data(resource);
    size_t index = server->data_source_offer_count++;

    (void)client;
    CHECK(index < 3);
    CHECK(strlen(mime_type) < sizeof(server->data_source_mimes[index]));
    strcpy(server->data_source_mimes[index], mime_type);
}

static void data_source_destroy(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_server *server = wl_resource_get_user_data(resource);

    (void)client;
    server->data_source_destroy_count++;
    wl_resource_destroy(resource);
}

static void data_source_set_actions(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t actions)
{
    (void)client;
    (void)resource;
    (void)actions;
}

static const struct wl_data_source_interface data_source_implementation = {
    .offer = data_source_offer,
    .destroy = data_source_destroy,
    .set_actions = data_source_set_actions,
};

static void data_offer_accept(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t serial,
    const char *mime_type)
{
    (void)client;
    (void)resource;
    (void)serial;
    (void)mime_type;
}

static void offer_receive(
    struct wl_client *client,
    struct wl_resource *resource,
    const char *mime_type,
    int32_t descriptor)
{
    fake_offer *offer = wl_resource_get_user_data(resource);
    fake_server *server = offer->server;
    size_t index = server->offer_receive_count++;

    (void)client;
    CHECK(index < 16);
    CHECK(strlen(mime_type) < sizeof(server->offer_receive_mimes[index]));
    strcpy(server->offer_receive_mimes[index], mime_type);
    server->offer_receive_descriptors[index] = descriptor;
}

static void offer_destroy(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_offer *offer = wl_resource_get_user_data(resource);

    (void)client;
    offer->server->offer_destroy_count++;
    wl_resource_destroy(resource);
}

static void data_offer_finish(
    struct wl_client *client,
    struct wl_resource *resource)
{
    (void)client;
    (void)resource;
}

static void data_offer_set_actions(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t actions,
    uint32_t preferred_action)
{
    (void)client;
    (void)resource;
    (void)actions;
    (void)preferred_action;
}

static const struct wl_data_offer_interface data_offer_implementation = {
    .accept = data_offer_accept,
    .receive = offer_receive,
    .destroy = offer_destroy,
    .finish = data_offer_finish,
    .set_actions = data_offer_set_actions,
};

static void data_device_manager_create_source(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id)
{
    fake_server *server = wl_resource_get_user_data(resource);
    struct wl_resource *source = wl_resource_create(
        client,
        &wl_data_source_interface,
        wl_resource_get_version(resource),
        id);

    CHECK(source != NULL);
    CHECK(server->data_source_create_count < 8);
    wl_resource_set_implementation(
        source,
        &data_source_implementation,
        server,
        NULL);
    server->data_source_resources[server->data_source_create_count++] = source;
}

static void data_device_manager_get_device(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id,
    struct wl_resource *seat_resource)
{
    fake_server *server = wl_resource_get_user_data(resource);
    fake_seat *seat = wl_resource_get_user_data(seat_resource);
    struct wl_resource *device = wl_resource_create(
        client,
        &wl_data_device_interface,
        wl_resource_get_version(resource),
        id);

    CHECK(device != NULL);
    CHECK(seat != NULL && seat->server == server);
    wl_resource_set_implementation(
        device,
        &data_device_implementation,
        seat,
        NULL);
    server->data_device_resources[seat->index] = device;
    server->data_device_create_count++;
}

static const struct wl_data_device_manager_interface
    data_device_manager_implementation = {
        .create_data_source = data_device_manager_create_source,
        .get_data_device = data_device_manager_get_device,
};

static void bind_data_device_manager(
    struct wl_client *client,
    void *data,
    uint32_t version,
    uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(
        client,
        &wl_data_device_manager_interface,
        (int)version,
        id);

    CHECK(resource != NULL);
    wl_resource_set_implementation(
        resource,
        &data_device_manager_implementation,
        data,
        NULL);
}

static void primary_selection_device_set_selection(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *source,
    uint32_t serial)
{
    fake_seat *seat = wl_resource_get_user_data(resource);

    (void)client;
    seat->server->primary_selection_count[seat->index]++;
    seat->server->primary_selection_source[seat->index] = source;
    seat->server->primary_selection_serial[seat->index] = serial;
}

static void primary_selection_device_destroy(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_seat *seat = wl_resource_get_user_data(resource);

    (void)client;
    seat->server->primary_selection_device_destroy_count++;
    seat->server->primary_selection_device_resources[seat->index] = NULL;
    wl_resource_destroy(resource);
}

static const struct zwp_primary_selection_device_v1_interface
    primary_selection_device_implementation = {
        .set_selection = primary_selection_device_set_selection,
        .destroy = primary_selection_device_destroy,
};

static void primary_selection_source_offer(
    struct wl_client *client,
    struct wl_resource *resource,
    const char *mime_type)
{
    fake_server *server = wl_resource_get_user_data(resource);
    size_t index = server->primary_selection_source_offer_count++;

    (void)client;
    CHECK(index < 3);
    CHECK(strlen(mime_type) <
          sizeof(server->primary_selection_source_mimes[index]));
    strcpy(server->primary_selection_source_mimes[index], mime_type);
}

static void primary_selection_source_destroy(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_server *server = wl_resource_get_user_data(resource);

    (void)client;
    server->primary_selection_source_destroy_count++;
    wl_resource_destroy(resource);
}

static const struct zwp_primary_selection_source_v1_interface
    primary_selection_source_implementation = {
        .offer = primary_selection_source_offer,
        .destroy = primary_selection_source_destroy,
};

static const struct zwp_primary_selection_offer_v1_interface
    primary_selection_offer_implementation = {
        .receive = offer_receive,
        .destroy = offer_destroy,
};

static void primary_selection_manager_create_source(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id)
{
    fake_server *server = wl_resource_get_user_data(resource);
    struct wl_resource *source = wl_resource_create(
        client,
        &zwp_primary_selection_source_v1_interface,
        wl_resource_get_version(resource),
        id);

    CHECK(source != NULL);
    CHECK(server->primary_selection_source_create_count < 8);
    wl_resource_set_implementation(
        source,
        &primary_selection_source_implementation,
        server,
        NULL);
    server->primary_selection_source_resources[
        server->primary_selection_source_create_count++] = source;
}

static void primary_selection_manager_get_device(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id,
    struct wl_resource *seat_resource)
{
    fake_server *server = wl_resource_get_user_data(resource);
    fake_seat *seat = wl_resource_get_user_data(seat_resource);
    struct wl_resource *device = wl_resource_create(
        client,
        &zwp_primary_selection_device_v1_interface,
        wl_resource_get_version(resource),
        id);

    CHECK(device != NULL);
    CHECK(seat != NULL && seat->server == server);
    wl_resource_set_implementation(
        device,
        &primary_selection_device_implementation,
        seat,
        NULL);
    server->primary_selection_device_resources[seat->index] = device;
    server->primary_selection_device_create_count++;
}

static const struct zwp_primary_selection_device_manager_v1_interface
    primary_selection_manager_implementation = {
        .create_source = primary_selection_manager_create_source,
        .get_device = primary_selection_manager_get_device,
        .destroy = destroy_resource,
};

static void bind_primary_selection_manager(
    struct wl_client *client,
    void *data,
    uint32_t version,
    uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(
        client,
        &zwp_primary_selection_device_manager_v1_interface,
        (int)version,
        id);

    CHECK(resource != NULL);
    wl_resource_set_implementation(
        resource,
        &primary_selection_manager_implementation,
        data,
        NULL);
}

static void bind_fractional_scale_manager(
    struct wl_client *client,
    void *data,
    uint32_t version,
    uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(
        client,
        &wp_fractional_scale_manager_v1_interface,
        (int)version,
        id);

    CHECK(resource != NULL);
    wl_resource_set_implementation(
        resource,
        &fractional_scale_manager_implementation,
        data,
        NULL);
}

static void keyboard_release(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_seat *seat = wl_resource_get_user_data(resource);

    (void)client;
    seat->server->keyboard_destroy_count++;
    wl_resource_destroy(resource);
}

static const struct wl_keyboard_interface keyboard_implementation = {
    .release = keyboard_release,
};

static void pointer_release(
    struct wl_client *client,
    struct wl_resource *resource)
{
    fake_seat *seat = wl_resource_get_user_data(resource);

    (void)client;
    seat->server->pointer_destroy_count++;
    wl_resource_destroy(resource);
}

static const struct wl_pointer_interface pointer_implementation = {
    .set_cursor = NULL,
    .release = pointer_release,
};

static void seat_get_pointer(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id)
{
    fake_seat *seat = wl_resource_get_user_data(resource);
    struct wl_resource *pointer = wl_resource_create(
        client,
        &wl_pointer_interface,
        wl_resource_get_version(resource),
        id);

    CHECK(pointer != NULL);
    wl_resource_set_implementation(
        pointer,
        &pointer_implementation,
        seat,
        NULL);
    seat->server->pointer_resources[seat->index] = pointer;
    seat->server->pointer_create_count++;
}

static void seat_get_keyboard(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id)
{
    fake_seat *seat = wl_resource_get_user_data(resource);
    struct wl_resource *keyboard = wl_resource_create(
        client,
        &wl_keyboard_interface,
        wl_resource_get_version(resource),
        id);

    CHECK(keyboard != NULL);
    wl_resource_set_implementation(
        keyboard,
        &keyboard_implementation,
        seat,
        NULL);
    seat->server->keyboard_resources[seat->index] = keyboard;
    seat->server->keyboard_create_count++;
}

static void seat_get_touch(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id)
{
    struct wl_resource *touch = wl_resource_create(
        client,
        &wl_touch_interface,
        wl_resource_get_version(resource),
        id);

    CHECK(touch != NULL);
    wl_resource_set_implementation(touch, NULL, NULL, NULL);
}

static void seat_release(
    struct wl_client *client,
    struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_seat_interface seat_implementation = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

static void bind_seat(
    struct wl_client *client,
    void *data,
    uint32_t version,
    uint32_t id)
{
    fake_seat *seat = data;
    struct wl_resource *resource = wl_resource_create(
        client,
        &wl_seat_interface,
        (int)version,
        id);

    CHECK(resource != NULL);
    wl_resource_set_implementation(
        resource,
        &seat_implementation,
        seat,
        NULL);
    seat->server->seat_resources[seat->index] = resource;
    seat->server->seat_bind_count++;
    if (version >= WL_SEAT_NAME_SINCE_VERSION) {
        wl_seat_send_name(resource, seat->index == 0 ? "seat-a" : "seat-b");
    }
    wl_seat_send_capabilities(
        resource,
        WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER);
}

static void add_seat(fake_server *server, size_t index)
{
    CHECK(index < 2);
    server->seat_data[index].server = server;
    server->seat_data[index].index = index;
    server->seats[index] = wl_global_create(
        server->server_display,
        &wl_seat_interface,
        9,
        &server->seat_data[index],
        bind_seat);
    CHECK(server->seats[index] != NULL);
}

static void send_keymap(fake_server *server, size_t seat_index)
{
    static const char keymap[] =
        "xkb_keymap {\n"
        " xkb_keycodes \"minimal\" { minimum = 8; maximum = 255; <ESC> = 9; <AE01> = 10; <AD03> = 26; <RTRN> = 36; <UP> = 111; <LCTL> = 37; <AC01> = 38; <AC02> = 39; <LFSH> = 50; <COMP> = 66; <FK01> = 67; };\n"
        " xkb_types \"minimal\" { virtual_modifiers LevelThree; type \"ONE_LEVEL\" { modifiers = none; map[None] = Level1; }; type \"TWO_LEVEL\" { modifiers = Shift; map[None] = Level1; map[Shift] = Level2; }; };\n"
        " xkb_compatibility \"minimal\" { interpret Any+AnyOf(all) { action = NoAction(); }; };\n"
        " xkb_symbols \"minimal\" { key <ESC> { [ Escape ] }; key <RTRN> { [ Return ] }; key <UP> { [ Up ] }; key <AE01> { type=\"TWO_LEVEL\", [ 1, exclam ] }; key <AD03> { type=\"TWO_LEVEL\", [ e, E ] }; key <LCTL> { [ Control_L ] }; key <AC01> { type=\"TWO_LEVEL\", [ a, A ] }; key <AC02> { [ apostrophe ] }; key <LFSH> { [ Shift_L ] }; key <COMP> { [ Multi_key ] }; key <FK01> { [ F1 ] }; modifier_map Shift { <LFSH> }; modifier_map Control { <LCTL> }; };\n"
        "};\n";
    char path[] = "/tmp/boxrig-wayland-keymap-XXXXXX";
    int descriptor = mkstemp(path);
    size_t offset = 0;

    CHECK(descriptor >= 0);
    CHECK(unlink(path) == 0);
    while (offset < sizeof(keymap)) {
        ssize_t written = write(
            descriptor,
            keymap + offset,
            sizeof(keymap) - offset);

        CHECK(written > 0);
        offset += (size_t)written;
    }
    CHECK(lseek(descriptor, 0, SEEK_SET) == 0);
    wl_keyboard_send_keymap(
        server->keyboard_resources[seat_index],
        WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
        descriptor,
        (uint32_t)sizeof(keymap));
    close(descriptor);
}

static void send_oversized_keymap(fake_server *server, size_t seat_index)
{
    int descriptor = open("/dev/null", O_RDONLY | O_CLOEXEC);

    CHECK(descriptor >= 0);
    wl_keyboard_send_keymap(
        server->keyboard_resources[seat_index],
        WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
        descriptor,
        (uint32_t)(BOXRIG_WAYLAND_KEYMAP_BYTES_MAXIMUM + 1));
    close(descriptor);
}

static void create_server(
    fake_server *server,
    bool include_subcompositor,
    bool include_fractional_scale,
    bool advertise_argb)
{
    int sockets[2];

    *server = (fake_server){0};
    server->advertise_argb = advertise_argb;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    server->server_display = wl_display_create();
    CHECK(server->server_display != NULL);
    server->server_client = wl_client_create(server->server_display, sockets[1]);
    CHECK(server->server_client != NULL);
    server->client_display = wl_display_connect_to_fd(sockets[0]);
    CHECK(server->client_display != NULL);
    server->client_queue = wl_display_create_queue(server->client_display);
    CHECK(server->client_queue != NULL);
    server->compositor = wl_global_create(
        server->server_display,
        &wl_compositor_interface,
        6,
        server,
        bind_compositor);
    server->shm = wl_global_create(
        server->server_display,
        &wl_shm_interface,
        1,
        server,
        bind_shm);
    if (include_subcompositor) {
        server->subcompositor = wl_global_create(
            server->server_display,
            &wl_subcompositor_interface,
            1,
            server,
            bind_subcompositor);
    }
    server->output = wl_global_create(
        server->server_display,
        &wl_output_interface,
        4,
        server,
        bind_output);
    server->text_input_manager = wl_global_create(
        server->server_display,
        &zwp_text_input_manager_v3_interface,
        1,
        server,
        bind_text_input_manager);
    server->data_device_manager = wl_global_create(
        server->server_display,
        &wl_data_device_manager_interface,
        3,
        server,
        bind_data_device_manager);
    server->primary_selection_manager = wl_global_create(
        server->server_display,
        &zwp_primary_selection_device_manager_v1_interface,
        1,
        server,
        bind_primary_selection_manager);
    if (include_fractional_scale) {
        server->viewporter = wl_global_create(
            server->server_display,
            &wp_viewporter_interface,
            1,
            server,
            bind_viewporter);
        server->fractional_scale_manager = wl_global_create(
            server->server_display,
            &wp_fractional_scale_manager_v1_interface,
            1,
            server,
            bind_fractional_scale_manager);
    }
    CHECK(server->compositor != NULL && server->shm != NULL);
    CHECK(server->output != NULL && server->text_input_manager != NULL);
    CHECK(server->data_device_manager != NULL &&
          server->primary_selection_manager != NULL);
    CHECK(!include_fractional_scale ||
          (server->viewporter != NULL &&
           server->fractional_scale_manager != NULL));
    add_seat(server, 0);
}

static void pump(fake_server *server)
{
    struct pollfd descriptor;

    CHECK(wl_display_flush(server->client_display) >= 0);
    /* A surrounding context plus state changes may exceed one socket read.
     * Drain the requests already sent before inspecting committed state. */
    descriptor.fd = wl_client_get_fd(server->server_client);
    descriptor.events = POLLIN;
    do {
        CHECK(wl_event_loop_dispatch(
                  wl_display_get_event_loop(server->server_display),
                  0) == 0);
        descriptor.revents = 0;
        CHECK(poll(&descriptor, 1, 0) >= 0);
    } while ((descriptor.revents & POLLIN) != 0);
    wl_display_flush_clients(server->server_display);
    /* A hint batch can span multiple client socket reads. Finish dispatching
     * already available events, including done, before observing composition. */
    do {
        while (wl_display_prepare_read_queue(
                   server->client_display,
                   server->client_queue) != 0) {
            CHECK(wl_display_dispatch_queue_pending(
                      server->client_display,
                      server->client_queue) >= 0);
        }
        descriptor.fd = wl_display_get_fd(server->client_display);
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        if (poll(&descriptor, 1, 0) == 1 &&
            (descriptor.revents & POLLIN) != 0) {
            CHECK(wl_display_read_events(server->client_display) == 0);
        } else {
            wl_display_cancel_read(server->client_display);
        }
        CHECK(wl_display_dispatch_queue_pending(
                  server->client_display,
                  server->client_queue) >= 0);
    } while ((descriptor.revents & POLLIN) != 0);
}

static struct wl_resource *send_selection_offer(
    fake_server *server,
    size_t seat_index,
    bool primary,
    const char *const *mime_types,
    size_t mime_count)
{
    size_t offer_index = server->data_offer_create_count +
                         server->primary_selection_offer_create_count;
    fake_offer *offer;
    struct wl_resource *resource;
    size_t index;

    CHECK(offer_index < 16);
    offer = &server->offers[offer_index];
    offer->server = server;
    offer->index = offer_index;
    offer->primary = primary;
    if (primary) {
        resource = wl_resource_create(
            server->server_client,
            &zwp_primary_selection_offer_v1_interface,
            1,
            0);
        CHECK(resource != NULL);
        wl_resource_set_implementation(
            resource,
            &primary_selection_offer_implementation,
            offer,
            NULL);
        server->primary_selection_offer_resources
            [server->primary_selection_offer_create_count++] = resource;
        zwp_primary_selection_device_v1_send_data_offer(
            server->primary_selection_device_resources[seat_index],
            resource);
        for (index = 0; index < mime_count; index++) {
            zwp_primary_selection_offer_v1_send_offer(
                resource,
                mime_types[index]);
        }
        zwp_primary_selection_device_v1_send_selection(
            server->primary_selection_device_resources[seat_index],
            resource);
    } else {
        resource = wl_resource_create(
            server->server_client,
            &wl_data_offer_interface,
            3,
            0);
        CHECK(resource != NULL);
        wl_resource_set_implementation(
            resource,
            &data_offer_implementation,
            offer,
            NULL);
        server->data_offer_resources[server->data_offer_create_count++] =
            resource;
        wl_data_device_send_data_offer(
            server->data_device_resources[seat_index],
            resource);
        for (index = 0; index < mime_count; index++) {
            wl_data_offer_send_offer(resource, mime_types[index]);
        }
        wl_data_device_send_selection(
            server->data_device_resources[seat_index],
            resource);
    }
    wl_display_flush_clients(server->server_display);
    return resource;
}

static void destroy_server(fake_server *server)
{
    wl_event_queue_destroy(server->client_queue);
    wl_display_disconnect(server->client_display);
    wl_client_destroy(server->server_client);
    wl_display_destroy(server->server_display);
    if (server->shm_mapping != NULL) {
        CHECK(munmap(server->shm_mapping, server->shm_mapping_size) == 0);
        server->shm_mapping = NULL;
        server->shm_mapping_size = 0;
    }
}

static void host_registry_global(
    void *data,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version)
{
    host_registry_state *host = data;

    if (host->compositor == NULL &&
        strcmp(interface, wl_compositor_interface.name) == 0) {
        host->compositor = wl_registry_bind(
            registry,
            name,
            &wl_compositor_interface,
            version < 6 ? version : 6);
        CHECK(host->compositor != NULL);
        wl_proxy_set_queue(
            (struct wl_proxy *)host->compositor,
            host->queue);
    }
}

static void host_registry_global_remove(
    void *data,
    struct wl_registry *registry,
    uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener host_registry_listener = {
    .global = host_registry_global,
    .global_remove = host_registry_global_remove,
};

static struct wl_registry *create_host_registry(
    fake_server *server,
    host_registry_state *host)
{
    struct wl_registry *registry;
    void *wrapper = wl_proxy_create_wrapper(server->client_display);

    CHECK(wrapper != NULL);
    host->queue = server->client_queue;
    host->compositor = NULL;
    wl_proxy_set_queue(wrapper, server->client_queue);
    registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    CHECK(registry != NULL);
    wl_proxy_set_queue((struct wl_proxy *)registry, server->client_queue);
    CHECK(wl_registry_add_listener(
              registry,
              &host_registry_listener,
              host) == 0);
    return registry;
}

static void wayland_fixture_forward_frame_request(void *context)
{
    wayland_fixture_component *component = context;

    if (component->frame_request_callback != NULL) {
        component->frame_request_callback(component->frame_request_context);
    }
}

static boxrig_ui_status wayland_fixture_complete_host_action(
    wayland_fixture_component *component)
{
    boxrig_wayland_host_action action;

    while (boxrig_wayland_terminal_component_take_host_action(
        component->native,
        &action)) {
        bool applied;

        if (action.parent_commit_required) {
            wl_surface_commit(component->host->parent);
            pump(&component->host->server);
        }
        if (boxrig_wayland_terminal_component_complete_parent_commit(
                component->native,
                action.revision,
                &applied) != BOXRIG_UI_OK ||
            !applied) {
            return BOXRIG_UI_UNAVAILABLE;
        }
    }
    return BOXRIG_UI_OK;
}

static void wayland_fixture_release_presentation(
    wayland_fixture_host *host)
{
    fake_server *server = &host->server;
    size_t index;
    bool changed = false;

    for (index = 0; index < server->frame_callback_count; index++) {
        if (server->frame_callbacks[index] != NULL) {
            complete_frame(server, index);
            changed = true;
        }
    }
    for (index = 0; index < server->shm_buffer_create_count; index++) {
        if (server->shm_buffers[index] != NULL &&
            server->shm_buffer_busy[index]) {
            release_buffer(server, index);
            changed = true;
        }
    }
    if (changed) {
        pump(server);
    }
}

static boxrig_ui_status wayland_fixture_dispatch_component(
    wayland_fixture_component *component)
{
    size_t attempt;

    for (attempt = 0; attempt < 32; attempt++) {
        boxrig_wayland_dispatch_result result;
        boxrig_wayland_component_diagnostic diagnostic;
        boxrig_ui_status status = boxrig_wayland_context_dispatch(
            component->host->context,
            1,
            &result);

        if (status != BOXRIG_UI_OK) {
            return status;
        }
        status = boxrig_wayland_terminal_component_get_diagnostic(
            component->native,
            &diagnostic);
        if (status != BOXRIG_UI_OK) {
            return status;
        }
        if (diagnostic.input_blocked || !result.additional_dispatch) {
            return BOXRIG_UI_OK;
        }
    }
    return BOXRIG_UI_LIMIT_EXCEEDED;
}

static void *wayland_fixture_create_host(void)
{
    wayland_fixture_host *host = calloc(1, sizeof(*host));

    if (host == NULL) {
        return NULL;
    }
    create_server(&host->server, true, false, true);
    host->registry = create_host_registry(
        &host->server,
        &host->registry_state);
    CHECK(create_protocol_context(
              host->server.client_display,
              host->server.client_queue,
              NULL,
              &host->context) == BOXRIG_UI_OK);
    pump(&host->server);
    pump(&host->server);
    CHECK(host->registry_state.compositor != NULL);
    host->parent =
        wl_compositor_create_surface(host->registry_state.compositor);
    CHECK(host->parent != NULL);
    wl_proxy_set_queue(
        (struct wl_proxy *)host->parent,
        host->server.client_queue);
    pump(&host->server);
    host->seat_global_name =
        wl_global_get_name(host->server.seats[0], host->server.server_client);
    CHECK(host->seat_global_name != 0);
    return host;
}

static void wayland_fixture_destroy_host(void *opaque_host)
{
    wayland_fixture_host *host = opaque_host;

    if (host == NULL) {
        return;
    }
    wl_surface_destroy(host->parent);
    wl_compositor_destroy(host->registry_state.compositor);
    wl_registry_destroy(host->registry);
    pump(&host->server);
    boxrig_wayland_context_destroy(host->context);
    destroy_server(&host->server);
    free(host);
}

static boxrig_ui_status wayland_fixture_create_component(
    void *opaque_host,
    boxrig_ui_model *model,
    boxrig_ui_component_attachment attachment,
    void **component_out)
{
    wayland_fixture_host *host = opaque_host;
    wayland_fixture_component *component;
    boxrig_wayland_terminal_component_config config =
        boxrig_wayland_terminal_component_config_default();
    size_t surface_index = host->server.child_surface_count;
    boxrig_ui_status status;

    *component_out = NULL;
    component = calloc(1, sizeof(*component));
    if (component == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    component->host = host;
    component->attachment = attachment;
    component->input_capacity = config.maximum_input_events;
    config.attachment = attachment;
    config.paste_chunk_size = 4;
    config.frame_request_callback = wayland_fixture_forward_frame_request;
    config.frame_request_context = component;
    status = create_steady_component(
        host->context,
        host->parent,
        model,
        &config,
        &component->native);
    if (status != BOXRIG_UI_OK) {
        free(component);
        return status;
    }
    pump(&host->server);
    CHECK(host->server.child_surface_count == surface_index + 1);
    component->surface_resource = host->server.child_resources[surface_index];
    *component_out = component;
    return BOXRIG_UI_OK;
}

static void wayland_fixture_destroy_component(void *opaque_component)
{
    wayland_fixture_component *component = opaque_component;

    if (component == NULL) {
        return;
    }
    boxrig_wayland_terminal_component_destroy(component->native);
    pump(&component->host->server);
    free(component);
}

static boxrig_ui_status wayland_fixture_quiesce_component(
    void *opaque_component)
{
    wayland_fixture_component *component = opaque_component;
    boxrig_ui_status status = boxrig_wayland_terminal_component_quiesce(
        component->native);

    if (status == BOXRIG_UI_OK) {
        component->visible = false;
        pump(&component->host->server);
    }
    return status;
}

static boxrig_ui_view *wayland_fixture_component_view(void *opaque_component)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_view(component->native);
}

static bool wayland_fixture_get_effective_metrics(
    void *opaque_component,
    boxrig_font_effective_metrics *metrics_out)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_get_effective_metrics(
               component->native,
               metrics_out) == BOXRIG_FONT_CATALOG_OK;
}

static boxrig_ui_status wayland_fixture_allocate(
    void *opaque_component,
    const boxrig_ui_component_allocation *allocation,
    boxrig_ui_layout_result *layout_out)
{
    wayland_fixture_component *component = opaque_component;
    boxrig_ui_status status = boxrig_wayland_terminal_component_allocate(
        component->native,
        allocation,
        layout_out);

    if (status == BOXRIG_UI_OK) {
        component->allocated = true;
        status = wayland_fixture_complete_host_action(component);
    }
    return status;
}

static boxrig_ui_status wayland_fixture_set_preferred_grid(
    void *opaque_component,
    size_t rows,
    size_t columns)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_ui_view_set_preferred_grid(
        boxrig_wayland_terminal_component_view(component->native),
        rows,
        columns);
}

static bool wayland_fixture_get_preferred_grid(
    void *opaque_component,
    size_t *rows_out,
    size_t *columns_out)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_ui_view_get_preferred_grid(
               boxrig_wayland_terminal_component_view(component->native),
               rows_out,
               columns_out) == BOXRIG_UI_OK;
}

static boxrig_ui_size_command_state wayland_fixture_get_size_command_state(
    void *opaque_component)
{
    wayland_fixture_component *component = opaque_component;
    boxrig_ui_size_command_state state = BOXRIG_UI_SIZE_COMMAND_IDLE;

    (void)boxrig_ui_view_get_size_command_state(
        boxrig_wayland_terminal_component_view(component->native),
        &state);
    return state;
}

static bool wayland_fixture_take_layout_action(
    void *opaque_component,
    boxrig_ui_layout_action *action_out)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_take_layout_action(
        component->native, action_out);
}

static boxrig_ui_status wayland_fixture_complete_size_command(
    void *opaque_component,
    uint64_t revision,
    boxrig_ui_size_outcome outcome,
    boxrig_ui_fixed allocation_width,
    boxrig_ui_fixed allocation_height,
    bool *applied_out,
    boxrig_ui_layout_result *layout_out)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_complete_size_command(
        component->native,
        revision,
        outcome,
        allocation_width,
        allocation_height,
        applied_out,
        layout_out);
}

static boxrig_ui_status wayland_fixture_complete_grid_resize(
    void *opaque_component,
    uint64_t revision,
    bool succeeded)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_complete_grid_resize(
        component->native, revision, succeeded);
}

static boxrig_ui_status wayland_fixture_set_visible(
    void *opaque_component,
    bool visible)
{
    wayland_fixture_component *component = opaque_component;
    boxrig_ui_status status =
        boxrig_wayland_terminal_component_set_visible(
            component->native,
            visible);

    if (status == BOXRIG_UI_OK) {
        component->visible = visible;
        status = wayland_fixture_complete_host_action(component);
    }
    return status;
}

static boxrig_ui_status wayland_fixture_set_frame_request_callback(
    void *opaque_component,
    boxrig_ui_frame_request_callback callback,
    void *context)
{
    wayland_fixture_component *component = opaque_component;

    if ((callback == NULL) != (context == NULL)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    component->frame_request_callback = callback;
    component->frame_request_context = context;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status wayland_fixture_frame(
    void *opaque_component,
    boxrig_ui_component_frame_result *result_out)
{
    wayland_fixture_component *component = opaque_component;
    boxrig_wayland_component_diagnostic diagnostic;
    boxrig_ui_status status;

    wayland_fixture_release_presentation(component->host);
    status = wayland_fixture_dispatch_component(component);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    status = boxrig_wayland_terminal_component_frame(
        component->native,
        result_out);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    pump(&component->host->server);
    status = boxrig_wayland_terminal_component_get_diagnostic(
        component->native,
        &diagnostic);
    if (status == BOXRIG_UI_OK) {
        result_out->input_blocked =
            diagnostic.input_blocked || diagnostic.paste_blocked;
        result_out->additional_dispatch =
            diagnostic.paste_active || diagnostic.input_queue_depth != 0;
    }
    return status;
}

static boxrig_ui_status wayland_fixture_queue_paste(
    void *opaque_component,
    const uint8_t *bytes,
    size_t length)
{
    static const char *const mime_types[] = {
        "text/plain;charset=utf-8",
    };
    wayland_fixture_component *component = opaque_component;
    wayland_fixture_host *host = component->host;
    boxrig_ui_view_action action = {0};
    boxrig_wayland_wait_source sources[2];
    boxrig_wayland_transfer_dispatch_result transfer_result;
    size_t receive_index = host->server.offer_receive_count;
    size_t source_count;
    size_t offset = 0;
    size_t attempt;
    int descriptor;
    boxrig_ui_status status;

    send_selection_offer(&host->server, 0, false, mime_types, 1);
    pump(&host->server);
    action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    status = boxrig_wayland_terminal_component_apply_view_action(
        component->native,
        host->seat_global_name,
        NULL,
        &action);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    pump(&host->server);
    if (host->server.offer_receive_count != receive_index + 1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    descriptor = host->server.offer_receive_descriptors[receive_index];
    while (offset < length) {
        ssize_t written = write(descriptor, bytes + offset, length - offset);

        if (written > 0) {
            offset += (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            close(descriptor);
            return BOXRIG_UI_UNAVAILABLE;
        }
    }
    close(descriptor);
    for (attempt = 0; attempt < 32; attempt++) {
        size_t index;

        status = boxrig_wayland_context_prepare_wait_sources(
            host->context,
            sources,
            2,
            &source_count);
        if (status != BOXRIG_UI_OK || source_count == 0) {
            break;
        }
        for (index = 0; index < source_count; index++) {
            sources[index].readiness = sources[index].interests;
        }
        status = boxrig_wayland_context_dispatch_wait_sources(
            host->context,
            sources,
            source_count,
            source_count,
            &transfer_result);
        if (status != BOXRIG_UI_OK) {
            return status;
        }
    }
    if (status != BOXRIG_UI_OK || attempt == 32) {
        return status == BOXRIG_UI_OK ? BOXRIG_UI_LIMIT_EXCEEDED : status;
    }
    return wayland_fixture_dispatch_component(component);
}

static bool wayland_fixture_run_native_input_transcript(
    void *opaque_component,
    const boxrig_test_frontend_input_script *script,
    boxrig_test_frontend_input_observation *observation_out)
{
    wayland_fixture_component *component = opaque_component;
    fake_server *server;
    struct wl_array keys;
    boxrig_wayland_component_diagnostic diagnostic;
    int x;
    int y;
    bool keys_initialized = false;
    bool passed = false;

    if (component == NULL || script == NULL || observation_out == NULL ||
        script->x < 0 || script->y < 0 ||
        script->x % BOXRIG_UI_FIXED_ONE != 0 ||
        script->y % BOXRIG_UI_FIXED_ONE != 0 ||
        script->x / BOXRIG_UI_FIXED_ONE > INT_MAX ||
        script->y / BOXRIG_UI_FIXED_ONE > INT_MAX) {
        return false;
    }
    server = &component->host->server;
    x = (int)(script->x / BOXRIG_UI_FIXED_ONE);
    y = (int)(script->y / BOXRIG_UI_FIXED_ONE);
    if (server->keyboard_resources[0] == NULL ||
        server->pointer_resources[0] == NULL ||
        component->surface_resource == NULL ||
        boxrig_wayland_terminal_component_set_key_router(
            component->native,
            boxrig_test_frontend_capture_key,
            observation_out) != BOXRIG_UI_OK ||
        boxrig_wayland_terminal_component_set_mouse_router(
            component->native,
            boxrig_test_frontend_capture_mouse,
            observation_out) != BOXRIG_UI_OK) {
        goto cleanup;
    }
    send_keymap(server, 0);
    wl_keyboard_send_repeat_info(server->keyboard_resources[0], 25, 1);
    wl_array_init(&keys);
    keys_initialized = true;
    wl_keyboard_send_enter(
        server->keyboard_resources[0],
        101,
        component->surface_resource,
        &keys);
    pump(server);
    if (wayland_fixture_dispatch_component(component) != BOXRIG_UI_OK ||
        boxrig_wayland_terminal_component_get_diagnostic(
            component->native,
            &diagnostic) != BOXRIG_UI_OK) {
        goto cleanup;
    }
    observation_out->focused_after_enter =
        diagnostic.keyboard_focus_count != 0;

    if (boxrig_wayland_context_update_monotonic_time(
            component->host->context,
            1000) != BOXRIG_UI_OK) {
        goto cleanup;
    }
    wl_keyboard_send_key(
        server->keyboard_resources[0],
        102,
        1,
        59,
        WL_KEYBOARD_KEY_STATE_PRESSED);
    pump(server);
    if (wayland_fixture_dispatch_component(component) != BOXRIG_UI_OK ||
        boxrig_wayland_context_update_monotonic_time(
            component->host->context,
            1001) != BOXRIG_UI_OK ||
        wayland_fixture_dispatch_component(component) != BOXRIG_UI_OK) {
        goto cleanup;
    }
    wl_keyboard_send_key(
        server->keyboard_resources[0],
        103,
        2,
        59,
        WL_KEYBOARD_KEY_STATE_RELEASED);

    wl_pointer_send_enter(
        server->pointer_resources[0],
        104,
        component->surface_resource,
        wl_fixed_from_int(x),
        wl_fixed_from_int(y));
    wl_keyboard_send_modifiers(
        server->keyboard_resources[0],
        105,
        1u << 2,
        0,
        0,
        0);
    wl_pointer_send_button(
        server->pointer_resources[0],
        106,
        10,
        0x110,
        WL_POINTER_BUTTON_STATE_PRESSED);
    wl_keyboard_send_modifiers(
        server->keyboard_resources[0],
        107,
        1u << 0,
        0,
        0,
        0);
    wl_pointer_send_motion(
        server->pointer_resources[0],
        11,
        wl_fixed_from_int(x),
        wl_fixed_from_int(y));
    wl_keyboard_send_modifiers(
        server->keyboard_resources[0],
        108,
        0,
        0,
        0,
        0);
    wl_pointer_send_button(
        server->pointer_resources[0],
        109,
        12,
        0x110,
        WL_POINTER_BUTTON_STATE_RELEASED);
    pump(server);
    if (wayland_fixture_dispatch_component(component) != BOXRIG_UI_OK) {
        goto cleanup;
    }

    wl_keyboard_send_leave(
        server->keyboard_resources[0],
        110,
        component->surface_resource);
    wl_pointer_send_leave(
        server->pointer_resources[0],
        111,
        component->surface_resource);
    pump(server);
    if (wayland_fixture_dispatch_component(component) != BOXRIG_UI_OK ||
        boxrig_wayland_terminal_component_get_diagnostic(
            component->native,
            &diagnostic) != BOXRIG_UI_OK) {
        goto cleanup;
    }
    observation_out->focused_after_leave =
        diagnostic.keyboard_focus_count != 0 ||
        diagnostic.pointer_focus_count != 0;
    passed = true;

cleanup:
    if (keys_initialized) {
        wl_array_release(&keys);
    }
    if (boxrig_wayland_terminal_component_set_key_router(
            component->native,
            NULL,
            NULL) != BOXRIG_UI_OK ||
        boxrig_wayland_terminal_component_set_mouse_router(
            component->native,
            NULL,
            NULL) != BOXRIG_UI_OK) {
        passed = false;
    }
    return passed;
}

static boxrig_font_catalog_status wayland_fixture_register_font_file(
    void *opaque_component,
    const char *path,
    size_t path_length,
    boxrig_font_resource_id *resource_id_out)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_register_font_file(
        component->native,
        path,
        path_length,
        0,
        resource_id_out);
}

static boxrig_ui_status wayland_fixture_set_font_specification(
    void *opaque_component,
    const boxrig_font_specification *specification)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_set_font_specification(
        component->native,
        specification);
}

static boxrig_ui_status wayland_fixture_get_font_specification(
    void *opaque_component,
    boxrig_font_specification *specification_out)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_get_font_specification(
        component->native,
        specification_out);
}

static boxrig_font_resource_id wayland_fixture_primary_font_resource(
    void *opaque_component)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_primary_font_resource(
        component->native);
}

static uint64_t wayland_fixture_font_catalog_revision(void *opaque_component)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_font_catalog_revision(
        component->native);
}

static boxrig_ui_status wayland_fixture_set_line_height(
    void *opaque_component,
    float adjustment_dip)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_set_line_height_adjustment(
        component->native,
        adjustment_dip);
}

static boxrig_ui_status wayland_fixture_get_line_height(
    void *opaque_component,
    float *adjustment_dip_out)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_get_line_height_adjustment(
        component->native,
        adjustment_dip_out);
}

static boxrig_ui_status wayland_fixture_visual_zoom(
    void *opaque_component,
    boxrig_font_zoom_direction direction,
    boxrig_font_visual_step_policy policy,
    bool *changed_out)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_visual_zoom(
        component->native,
        direction,
        policy,
        changed_out);
}

static boxrig_ui_status wayland_fixture_visual_zoom_reset(
    void *opaque_component,
    bool *changed_out)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_visual_zoom_reset(
        component->native,
        changed_out);
}

static boxrig_ui_status wayland_fixture_set_text_direction(
    void *opaque_component,
    boxrig_text_direction_policy policy)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_set_text_direction(
        component->native,
        policy);
}

static boxrig_text_direction_policy wayland_fixture_text_direction(
    void *opaque_component)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_text_direction(
        component->native);
}

static bool wayland_fixture_copy_selection(
    void *opaque_host,
    void *opaque_component,
    const uint8_t *expected,
    size_t expected_length)
{
    wayland_fixture_host *host = opaque_host;
    wayland_fixture_component *component = opaque_component;
    struct wl_array keys;
    boxrig_wayland_serial serial;
    boxrig_ui_view_action action = {0};
    boxrig_wayland_wait_source sources[2];
    boxrig_wayland_transfer_dispatch_result transfer_result;
    uint8_t observed[64];
    size_t observed_length = 0;
    size_t source_count;
    size_t source_index;
    size_t attempt;
    int descriptors[2];
    bool matched = false;

    if (expected_length > sizeof(observed)) {
        return false;
    }
    wl_array_init(&keys);
    wl_keyboard_send_enter(
        host->server.keyboard_resources[0],
        701,
        component->surface_resource,
        &keys);
    pump(&host->server);
    if (boxrig_wayland_context_get_serial(
            host->context,
            host->seat_global_name,
            BOXRIG_WAYLAND_SERIAL_KEYBOARD_ENTER,
            &serial) != BOXRIG_UI_OK) {
        goto cleanup_keys;
    }
    action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    if (boxrig_wayland_terminal_component_apply_view_action(
            component->native,
            host->seat_global_name,
            &serial,
            &action) != BOXRIG_UI_OK) {
        goto cleanup_keys;
    }
    pump(&host->server);
    if (host->server.data_source_create_count == 0 ||
        socketpair(
            AF_UNIX,
            SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
            0,
            descriptors) != 0) {
        goto cleanup_keys;
    }
    source_index = host->server.data_source_create_count - 1;
    wl_data_source_send_send(
        host->server.data_source_resources[source_index],
        "text/plain;charset=utf-8",
        descriptors[1]);
    close(descriptors[1]);
    pump(&host->server);
    for (attempt = 0; attempt < 32; attempt++) {
        size_t index;

        if (boxrig_wayland_context_prepare_wait_sources(
                host->context,
                sources,
                2,
                &source_count) != BOXRIG_UI_OK) {
            goto cleanup_descriptor;
        }
        if (source_count == 0) {
            break;
        }
        for (index = 0; index < source_count; index++) {
            sources[index].readiness = sources[index].interests;
        }
        if (boxrig_wayland_context_dispatch_wait_sources(
                host->context,
                sources,
                source_count,
                source_count,
                &transfer_result) != BOXRIG_UI_OK) {
            goto cleanup_descriptor;
        }
    }
    if (attempt == 32) {
        goto cleanup_descriptor;
    }
    while (observed_length < sizeof(observed)) {
        ssize_t count = read(
            descriptors[0],
            observed + observed_length,
            sizeof(observed) - observed_length);

        if (count > 0) {
            observed_length += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count == 0 ||
                   (count < 0 &&
                    (errno == EAGAIN || errno == EWOULDBLOCK))) {
            break;
        } else {
            goto cleanup_descriptor;
        }
    }
    matched = observed_length == expected_length &&
              memcmp(observed, expected, expected_length) == 0;

cleanup_descriptor:
    close(descriptors[0]);
    wl_data_source_send_cancelled(
        host->server.data_source_resources[source_index]);
    pump(&host->server);
cleanup_keys:
    wl_array_release(&keys);
    return matched;
}

static boxrig_ui_view_action_capabilities
wayland_fixture_view_action_capabilities(void *opaque_component)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_view_action_capabilities(
        component->native,
        component->host->seat_global_name);
}

static boxrig_ui_status wayland_fixture_apply_view_action(
    void *opaque_component,
    const boxrig_ui_view_action *action)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_apply_view_action(
        component->native,
        component->host->seat_global_name,
        NULL,
        action);
}

static bool wayland_fixture_get_scrollbar_state(void *component, boxrig_scrollbar_state *state_out)
{
    wayland_fixture_component *fixture = component;
    example_external_scrollbar bar = {.component = fixture->native};
    return check_external_scrollbar_example(&bar, state_out);
}

static bool wayland_fixture_get_scrollbar_model(
    void *opaque_component, boxrig_scrollbar_model *model_out)
{
    wayland_fixture_component *component = opaque_component;
    return boxrig_wayland_terminal_component_get_scrollbar_model(component->native, model_out) == BOXRIG_UI_OK;
}

static boxrig_ui_status wayland_fixture_apply_scrollbar_action(
    void *opaque_component, boxrig_scrollbar_action action, size_t position, bool *changed_out)
{
    wayland_fixture_component *component = opaque_component;
    return boxrig_wayland_terminal_component_apply_scrollbar_action(component->native, action, position, changed_out);
}

static boxrig_ui_status wayland_fixture_set_transparency(
    void *opaque_component,
    const boxrig_ui_transparency_config *config)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_set_transparency(
        component->native,
        config);
}

static boxrig_ui_status wayland_fixture_get_transparency(
    void *opaque_component,
    boxrig_ui_transparency_state *state_out)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_get_transparency(
        component->native,
        state_out);
}

static uint32_t wayland_fixture_capabilities(void *opaque_component)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_capabilities(component->native);
}

static boxrig_ui_status wayland_fixture_get_diagnostic(
    void *opaque_component,
    boxrig_ui_component_diagnostic *diagnostic_out)
{
    wayland_fixture_component *component = opaque_component;
    boxrig_wayland_component_diagnostic native;
    boxrig_wayland_context_diagnostic context;
    boxrig_ui_transparency_state transparency;
    boxrig_ui_status status;

    if (diagnostic_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = boxrig_wayland_terminal_component_get_diagnostic(
        component->native,
        &native);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    status = boxrig_wayland_context_get_diagnostic(
        component->host->context,
        &context);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    status = boxrig_wayland_terminal_component_get_transparency(
        component->native,
        &transparency);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    memset(diagnostic_out, 0, sizeof(*diagnostic_out));
    diagnostic_out->running = native.running;
    diagnostic_out->visible = component->visible;
    diagnostic_out->focused = native.keyboard_focus_count != 0 ||
                              native.pointer_focus_count != 0;
    diagnostic_out->input_blocked =
        native.input_blocked || native.paste_blocked;
    diagnostic_out->layout_authority =
        component->attachment == BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY;
    diagnostic_out->layout_configured = component->allocated;
    diagnostic_out->metrics_revision = native.scale_revision;
    diagnostic_out->transparency_revision = transparency.revision;
    diagnostic_out->failed =
        native.failure_reason != BOXRIG_WAYLAND_FAILURE_NONE;
    diagnostic_out->failure_reason = diagnostic_out->failed
                                         ? BOXRIG_UI_COMPONENT_FAILURE_RENDERER
                                         : BOXRIG_UI_COMPONENT_FAILURE_NONE;
    diagnostic_out->native_input_queue_depth = native.input_queue_depth;
    diagnostic_out->native_input_queue_high_water =
        native.input_queue_high_water;
    diagnostic_out->native_input_queue_capacity = component->input_capacity;
    diagnostic_out->active_clipboard_transfers =
        context.active_clipboard_transfers;
    diagnostic_out->outgoing_clipboard_bytes =
        context.outgoing_clipboard_bytes;
    return BOXRIG_UI_OK;
}

static boxrig_composition *wayland_fixture_composition(void *opaque_component)
{
    wayland_fixture_component *component = opaque_component;

    return boxrig_wayland_terminal_component_composition(component->native);
}

static bool wayland_fixture_observe_presentation(
    void *opaque_component,
    const boxrig_test_frontend_presentation_probe *probe,
    boxrig_test_frontend_presentation_observation *observation_out)
{
    wayland_fixture_component *component = opaque_component;
    fake_server *server;
    boxrig_wayland_component_diagnostic diagnostic;
    const unsigned char *data;
    uint32_t background;
    uint32_t meaning;
    uint64_t stable_hash = UINT64_C(1469598103934665603);
    size_t coverage = 0;
    size_t buffer_offset;
    unsigned int width;
    unsigned int height;
    unsigned int x;
    unsigned int y;

    if (component == NULL || probe == NULL || observation_out == NULL ||
        boxrig_wayland_terminal_component_get_diagnostic(
            component->native,
            &diagnostic) != BOXRIG_UI_OK) {
        return false;
    }
    server = &component->host->server;
    width = (unsigned int)server->shm_width;
    height = (unsigned int)server->shm_height;
    if (server->shm_mapping == NULL || server->shm_stride < 0 ||
        server->last_attached_buffer >= server->shm_buffer_create_count ||
        width == 0 || height == 0 || probe->background_x < 0 ||
        probe->background_y < 0 || probe->meaning_x < 0 ||
        probe->meaning_y < 0 || probe->coverage_x < 0 ||
        probe->coverage_y < 0 || probe->stable_x < 0 ||
        probe->stable_y < 0 || (unsigned int)probe->background_x >= width ||
        (unsigned int)probe->background_y >= height ||
        (unsigned int)probe->meaning_x >= width ||
        (unsigned int)probe->meaning_y >= height ||
        (unsigned int)probe->coverage_x >= width ||
        (unsigned int)probe->coverage_y >= height ||
        (unsigned int)probe->stable_x >= width ||
        (unsigned int)probe->stable_y >= height ||
        probe->coverage_width == 0 || probe->coverage_height == 0 ||
        probe->stable_width == 0 || probe->stable_height == 0 ||
        probe->coverage_width > width - (unsigned int)probe->coverage_x ||
        probe->coverage_height > height - (unsigned int)probe->coverage_y ||
        probe->stable_width > width - (unsigned int)probe->stable_x ||
        probe->stable_height > height - (unsigned int)probe->stable_y) {
        return false;
    }
    buffer_offset =
        server->shm_buffer_offsets[server->last_attached_buffer];
    if (buffer_offset > server->shm_mapping_size ||
        (size_t)server->shm_stride >
            (server->shm_mapping_size - buffer_offset) / height) {
        return false;
    }
    data = (const unsigned char *)server->shm_mapping + buffer_offset;
    memcpy(
        &background,
        data + (size_t)probe->background_y * (size_t)server->shm_stride +
            (size_t)probe->background_x * sizeof(background),
        sizeof(background));
    memcpy(
        &meaning,
        data + (size_t)probe->meaning_y * (size_t)server->shm_stride +
            (size_t)probe->meaning_x * sizeof(meaning),
        sizeof(meaning));
    for (y = 0; y < probe->coverage_height; y++) {
        for (x = 0; x < probe->coverage_width; x++) {
            uint32_t pixel;

            memcpy(
                &pixel,
                data + (size_t)(probe->coverage_y + (int)y) *
                           (size_t)server->shm_stride +
                    (size_t)(probe->coverage_x + (int)x) * sizeof(pixel),
                sizeof(pixel));
            if (pixel != background) {
                coverage++;
            }
        }
    }
    for (y = 0; y < probe->stable_height; y++) {
        for (x = 0; x < probe->stable_width; x++) {
            uint32_t pixel;

            memcpy(
                &pixel,
                data + (size_t)(probe->stable_y + (int)y) *
                           (size_t)server->shm_stride +
                    (size_t)(probe->stable_x + (int)x) * sizeof(pixel),
                sizeof(pixel));
            stable_hash ^= pixel;
            stable_hash *= UINT64_C(1099511628211);
        }
    }
    if (server->shm_format == WL_SHM_FORMAT_XRGB8888) {
        background |= UINT32_C(0xff000000);
        meaning |= UINT32_C(0xff000000);
    }
    memset(observation_out, 0, sizeof(*observation_out));
    observation_out->generation = diagnostic.presentation_generation;
    observation_out->width = diagnostic.logical_width;
    observation_out->height = diagnostic.logical_height;
    observation_out->background_argb = background;
    observation_out->meaning_argb = meaning;
    observation_out->coverage_pixels = coverage;
    observation_out->stable_hash = stable_hash;
    observation_out->alpha_observable =
        server->shm_format == WL_SHM_FORMAT_ARGB8888;
    return true;
}

static void test_shared_frontend_component_fixture(const char *font_path)
{
    const boxrig_test_frontend_component_operations operations = {
        .frontend_name = "native Wayland",
        .lifecycle = {
            .create_host = wayland_fixture_create_host,
            .destroy_host = wayland_fixture_destroy_host,
            .create_component = wayland_fixture_create_component,
            .quiesce_component = wayland_fixture_quiesce_component,
            .destroy_component = wayland_fixture_destroy_component},
        .layout = {
            .component_view = wayland_fixture_component_view,
            .get_effective_metrics = wayland_fixture_get_effective_metrics,
            .allocate = wayland_fixture_allocate,
            .set_preferred_grid = wayland_fixture_set_preferred_grid,
            .get_preferred_grid = wayland_fixture_get_preferred_grid,
            .get_size_command_state =
                wayland_fixture_get_size_command_state,
            .take_layout_action = wayland_fixture_take_layout_action,
            .complete_size_command =
                wayland_fixture_complete_size_command,
            .complete_grid_resize =
                wayland_fixture_complete_grid_resize,
            .set_visible = wayland_fixture_set_visible},
        .frame = {
            .set_frame_request_callback =
                wayland_fixture_set_frame_request_callback,
            .frame = wayland_fixture_frame},
        .input = {
            .queue_paste = wayland_fixture_queue_paste,
            .run_native_transcript =
                wayland_fixture_run_native_input_transcript},
        .font = {
            .fixture_font_path = font_path,
            .register_font_file = wayland_fixture_register_font_file,
            .set_specification = wayland_fixture_set_font_specification,
            .get_specification = wayland_fixture_get_font_specification,
            .primary_resource = wayland_fixture_primary_font_resource,
            .catalog_revision = wayland_fixture_font_catalog_revision,
            .set_line_height = wayland_fixture_set_line_height,
            .get_line_height = wayland_fixture_get_line_height,
            .visual_zoom = wayland_fixture_visual_zoom,
            .visual_zoom_reset = wayland_fixture_visual_zoom_reset,
            .set_text_direction = wayland_fixture_set_text_direction,
            .text_direction = wayland_fixture_text_direction},
        .view_action = {
            .copy_selection = wayland_fixture_copy_selection,
            .capabilities = wayland_fixture_view_action_capabilities,
            .apply = wayland_fixture_apply_view_action,
            .get_scrollbar_model = wayland_fixture_get_scrollbar_model,
            .get_scrollbar_state = wayland_fixture_get_scrollbar_state,
            .apply_scrollbar_action =
                wayland_fixture_apply_scrollbar_action},
        .presentation = {
            .set_transparency = wayland_fixture_set_transparency,
            .get_transparency = wayland_fixture_get_transparency,
            .capabilities = wayland_fixture_capabilities,
            .get_diagnostic = wayland_fixture_get_diagnostic,
            .composition = wayland_fixture_composition,
            .observe = wayland_fixture_observe_presentation}};

    CHECK(boxrig_test_run_frontend_component_fixture(&operations));
}

static void test_ready_and_required_removal(void)
{
    fake_server server;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_context_diagnostic diagnostic;

    create_server(&server, true, false, true);
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              NULL,
              &context) == BOXRIG_UI_OK);
    CHECK(context != NULL);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.state == BOXRIG_WAYLAND_CONTEXT_INITIALIZING);
    pump(&server);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.state == BOXRIG_WAYLAND_CONTEXT_INITIALIZING);
    CHECK(diagnostic.pending_callbacks == 1);
    pump(&server);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.state == BOXRIG_WAYLAND_CONTEXT_READY);
    CHECK(diagnostic.compositor_version == 6);
    CHECK(diagnostic.shm_version == 1);
    CHECK(diagnostic.subcompositor_version == 1);
    CHECK(diagnostic.argb8888 && diagnostic.xrgb8888);
    CHECK(diagnostic.global_generation != 0);
    wl_global_remove(server.subcompositor);
    server.subcompositor = NULL;
    pump(&server);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.state == BOXRIG_WAYLAND_CONTEXT_FATAL);
    CHECK(diagnostic.failure_reason ==
          BOXRIG_WAYLAND_FAILURE_REQUIRED_GLOBAL_REMOVED);
    boxrig_wayland_context_destroy(context);
    destroy_server(&server);
}

static void test_missing_required_global(void)
{
    fake_server server;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_context_diagnostic diagnostic;

    create_server(&server, false, false, true);
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              NULL,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.state == BOXRIG_WAYLAND_CONTEXT_UNAVAILABLE);
    CHECK(diagnostic.failure_reason ==
          BOXRIG_WAYLAND_FAILURE_REQUIRED_GLOBAL_MISSING);
    boxrig_wayland_context_destroy(context);
    destroy_server(&server);
}

static void test_clipboard_managers_follow_seat_lifetime(void)
{
    fake_server server;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_context_diagnostic diagnostic;

    create_server(&server, true, false, true);
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              NULL,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.data_device_manager_version == 3);
    CHECK(diagnostic.primary_selection_manager_version == 1);
    CHECK(diagnostic.clipboard_capabilities ==
          (BOXRIG_WAYLAND_CLIPBOARD_CAPABILITY_CLIPBOARD |
           BOXRIG_WAYLAND_CLIPBOARD_CAPABILITY_PRIMARY));
    CHECK(server.data_device_create_count == 1);
    CHECK(server.primary_selection_device_create_count == 1);

    add_seat(&server, 1);
    pump(&server);
    pump(&server);
    CHECK(server.data_device_create_count == 2);
    CHECK(server.primary_selection_device_create_count == 2);

    wl_global_remove(server.primary_selection_manager);
    server.primary_selection_manager = NULL;
    pump(&server);
    pump(&server);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.primary_selection_manager_version == 0);
    CHECK(diagnostic.clipboard_capabilities ==
          BOXRIG_WAYLAND_CLIPBOARD_CAPABILITY_CLIPBOARD);
    CHECK(server.primary_selection_device_destroy_count == 2);

    wl_global_remove(server.data_device_manager);
    server.data_device_manager = NULL;
    pump(&server);
    pump(&server);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.data_device_manager_version == 0);
    CHECK(diagnostic.clipboard_capabilities == 0);
    CHECK(server.data_device_destroy_count == 2);

    boxrig_wayland_context_destroy(context);
    pump(&server);
    destroy_server(&server);
}

static void test_copy_uses_focused_seat_serial_and_immutable_sources(void)
{
    static const uint8_t screen_text[] = "copy-target\r\nblock-value";
    static const uint8_t copied_text[] = "copy\nbloc";
    fake_server server;
    host_registry_state host;
    struct wl_registry *host_registry;
    struct wl_surface *parent;
    struct wl_array keys;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_wayland_context_config context_config =
        protocol_context_config();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_wayland_context_diagnostic diagnostic;
    boxrig_wayland_serial keyboard_serial;
    boxrig_wayland_serial keyboard_key_serial;
    boxrig_wayland_serial pointer_serial;
    boxrig_wayland_serial invalid_serial;
    boxrig_ui_view_action_capabilities capabilities;
    boxrig_ui_view_action action;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_component_frame_result frame_result;
    boxrig_ui_layout_result layout;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_selection_options selection_options;
    boxrig_ui_hit_test_result anchor_hit;
    boxrig_ui_hit_test_result focus_hit;
    boxrig_wayland_wait_source sources[2];
    boxrig_wayland_transfer_dispatch_result transfer_result;
    uint8_t buffer[64];
    uint8_t received_text[sizeof(copied_text) - 1];
    size_t received;
    size_t source_count;
    size_t consumed;
    int blocked_pipe[2];
    int peer_pipe[2];
    int rejected_pipe[2];
    int unknown_pipe[2];
    uint32_t seat_name;

    create_server(&server, true, false, true);
    host_registry = create_host_registry(&server, &host);
    terminal_config.rows = 2;
    terminal_config.columns = 32;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              screen_text,
              sizeof(screen_text) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(screen_text) - 1);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    context_config.maximum_clipboard_transfers = 2;
    context_config.maximum_clipboard_bytes = 2 * (sizeof(copied_text) - 1);
    context_config.transfer_chunk_size = 3;
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              &context_config,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    CHECK(create_steady_component(
              context,
              parent,
              model,
              NULL,
              &component) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    CHECK(boxrig_wayland_terminal_component_get_effective_metrics(
              component,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    allocation.width = metrics.cell_width * terminal_config.columns;
    allocation.height = metrics.cell_height * terminal_config.rows;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_wayland_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(layout.rows == terminal_config.rows &&
          layout.columns == terminal_config.columns);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_hit_test(
              component,
              (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE / 4,
              (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE / 2,
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_hit_test(
              component,
              (boxrig_ui_fixed)metrics.cell_width * 3 *
                      BOXRIG_UI_FIXED_ONE +
                  (boxrig_ui_fixed)metrics.cell_width * 3 *
                      BOXRIG_UI_FIXED_ONE / 4,
              (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE +
                  (boxrig_ui_fixed)metrics.cell_height *
                      BOXRIG_UI_FIXED_ONE / 2,
              &focus_hit) == BOXRIG_UI_OK);
    selection_options.shape = BOXRIG_UI_SELECTION_RECTANGULAR;
    selection_options.kind = BOXRIG_UI_SELECTION_CHARACTER;
    CHECK(boxrig_ui_view_begin_selection_with_options(
              boxrig_wayland_terminal_component_view(component),
              &selection_options,
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(
              boxrig_wayland_terminal_component_view(component),
              &focus_hit) == BOXRIG_UI_OK);
    seat_name = wl_global_get_name(server.seats[0], server.server_client);
    capabilities =
        boxrig_wayland_terminal_component_view_action_capabilities(
            component,
            seat_name);
    CHECK(capabilities.copy_targets ==
          (BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION |
           BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD));
    CHECK(capabilities.paste_targets == capabilities.copy_targets);
    CHECK(!capabilities.reconversion);
    memset(&action, 0, sizeof(action));
    action.kind = BOXRIG_UI_VIEW_ACTION_RECONVERT;
    CHECK(!boxrig_ui_view_action_supported(&capabilities, &action));
    CHECK(boxrig_wayland_terminal_component_apply_view_action(
              component,
              seat_name,
              NULL,
              &action) == BOXRIG_UI_UNAVAILABLE);
    printf(
        "PASS %s through native Wayland public API\n",
        BOXRIG_TEST_CASE_EDITABLE_PROVIDER_ABSENT);

    send_keymap(&server, 0);
    wl_array_init(&keys);
    wl_keyboard_send_enter(
        server.keyboard_resources[0],
        101,
        server.child_resource,
        &keys);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        102,
        1,
        30,
        WL_KEYBOARD_KEY_STATE_PRESSED);
    pump(&server);
    CHECK(boxrig_wayland_context_get_serial(
              context,
              seat_name,
              BOXRIG_WAYLAND_SERIAL_KEYBOARD_ENTER,
              &keyboard_serial) == BOXRIG_UI_OK);
    CHECK(keyboard_serial.focus_generation != 0);
    CHECK(boxrig_wayland_context_get_serial(
              context,
              seat_name,
              BOXRIG_WAYLAND_SERIAL_KEYBOARD_KEY,
              &keyboard_key_serial) == BOXRIG_UI_OK);
    CHECK(keyboard_key_serial.value == 102);
    CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, false) == BOXRIG_UI_OK);
    capabilities = boxrig_wayland_terminal_component_view_action_capabilities(component, seat_name);
    CHECK(capabilities.copy_targets == (BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION | BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD));
    CHECK(capabilities.paste_targets == 0);
    action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    invalid_serial = keyboard_key_serial;
    invalid_serial.focus_generation++;
    CHECK(boxrig_wayland_terminal_component_apply_view_action(
              component,
              seat_name,
              &invalid_serial,
              &action) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_wayland_terminal_component_apply_view_action(
              component,
              seat_name,
              &keyboard_key_serial,
              &action) == BOXRIG_UI_OK);

    wl_pointer_send_enter(
        server.pointer_resources[0],
        202,
        server.child_resource,
        wl_fixed_from_int(1),
        wl_fixed_from_int(1));
    pump(&server);
    CHECK(boxrig_wayland_context_get_serial(
              context,
              seat_name,
              BOXRIG_WAYLAND_SERIAL_POINTER_ENTER,
              &pointer_serial) == BOXRIG_UI_OK);
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
    CHECK(boxrig_wayland_terminal_component_apply_view_action(
              component,
              seat_name + 1,
              &pointer_serial,
              &action) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_wayland_terminal_component_apply_view_action(
              component,
              seat_name,
              &pointer_serial,
              &action) == BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.data_source_create_count == 1);
    CHECK(server.primary_selection_source_create_count == 1);
    CHECK(server.data_source_offer_count == 3);
    CHECK(server.primary_selection_source_offer_count == 3);
    CHECK(strcmp(
              server.data_source_mimes[0],
              "text/plain;charset=utf-8") == 0);
    CHECK(strcmp(server.data_source_mimes[1], "UTF8_STRING") == 0);
    CHECK(strcmp(server.data_source_mimes[2], "text/plain") == 0);
    CHECK(strcmp(
              server.primary_selection_source_mimes[0],
              "text/plain;charset=utf-8") == 0);
    CHECK(strcmp(
              server.primary_selection_source_mimes[1],
              "UTF8_STRING") == 0);
    CHECK(strcmp(
              server.primary_selection_source_mimes[2],
              "text/plain") == 0);
    CHECK(server.data_selection_count[0] == 1);
    CHECK(server.primary_selection_count[0] == 1);
    CHECK(server.data_selection_serial[0] == 102);
    CHECK(server.primary_selection_serial[0] == 202);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.outgoing_clipboard_bytes ==
          2 * (sizeof(copied_text) - 1));
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    CHECK(boxrig_wayland_terminal_component_apply_view_action(
              component,
              seat_name,
              &keyboard_key_serial,
              &action) == BOXRIG_UI_LIMIT_EXCEEDED);

    /* Changing focus invalidates the input serial, not the published copy. */
    wl_keyboard_send_leave(server.keyboard_resources[0], 203,
        server.child_resource);
    wl_pointer_send_leave(server.pointer_resources[0], 204,
        server.child_resource);
    pump(&server);
    pump(&server);
    CHECK(server.data_source_destroy_count == 0);
    CHECK(server.primary_selection_source_destroy_count == 0);
    CHECK(boxrig_wayland_terminal_component_apply_view_action(component,
        seat_name, &keyboard_key_serial, &action) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) ==
        BOXRIG_UI_OK);
    CHECK(diagnostic.outgoing_clipboard_bytes == 2 * (sizeof(copied_text) - 1));

    CHECK(socketpair(
              AF_UNIX,
              SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
              0,
              unknown_pipe) == 0);
    wl_data_source_send_send(
        server.data_source_resources[0],
        "text/html",
        unknown_pipe[1]);
    close(unknown_pipe[1]);
    pump(&server);
    CHECK(read(unknown_pipe[0], buffer, sizeof(buffer)) == 0);
    close(unknown_pipe[0]);
    CHECK(boxrig_wayland_context_prepare_wait_sources(
              context,
              sources,
              2,
              &source_count) == BOXRIG_UI_OK);
    CHECK(source_count == 0);

    CHECK(socketpair(
              AF_UNIX,
              SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
              0,
              blocked_pipe) == 0);
    while (write(blocked_pipe[1], buffer, sizeof(buffer)) > 0) {
    }
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
    wl_data_source_send_send(
        server.data_source_resources[0],
        "UTF8_STRING",
        blocked_pipe[1]);
    close(blocked_pipe[1]);
    pump(&server);
    CHECK(boxrig_wayland_context_prepare_wait_sources(
              context,
              sources,
              2,
              &source_count) == BOXRIG_UI_OK);
    CHECK(source_count == 1);
    CHECK(sources[0].interests == BOXRIG_WAYLAND_WAIT_WRITABLE);
    CHECK((fcntl(sources[0].descriptor, F_GETFL) & O_NONBLOCK) != 0);
    CHECK((fcntl(sources[0].descriptor, F_GETFD) & FD_CLOEXEC) != 0);
    sources[0].readiness = BOXRIG_WAYLAND_WAIT_WRITABLE;
    CHECK(boxrig_wayland_context_dispatch_wait_sources(
              context,
              sources,
              source_count,
              1,
              &transfer_result) == BOXRIG_UI_OK);
    CHECK(transfer_result.processed_transfers == 1);
    CHECK(transfer_result.processed_bytes == 0);
    do {
        received = read(blocked_pipe[0], buffer, sizeof(buffer));
    } while (received != (size_t)-1 || errno == EINTR);
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);

    CHECK(socketpair(
              AF_UNIX,
              SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
              0,
              peer_pipe) == 0);
    wl_data_source_send_send(
        server.data_source_resources[0],
        "text/plain",
        peer_pipe[1]);
    close(peer_pipe[1]);
    CHECK(socketpair(
              AF_UNIX,
              SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
              0,
              rejected_pipe) == 0);
    wl_data_source_send_send(
        server.data_source_resources[0],
        "text/plain;charset=utf-8",
        rejected_pipe[1]);
    close(rejected_pipe[1]);
    pump(&server);
    CHECK(read(rejected_pipe[0], buffer, sizeof(buffer)) == 0);
    close(rejected_pipe[0]);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.active_clipboard_transfers == 2);
    CHECK(diagnostic.clipboard_transfer_high_water == 2);

    wl_data_source_send_cancelled(server.data_source_resources[0]);
    zwp_primary_selection_source_v1_send_cancelled(
        server.primary_selection_source_resources[0]);
    pump(&server);
    pump(&server);
    CHECK(server.data_source_destroy_count == 1);
    CHECK(server.primary_selection_source_destroy_count == 1);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.outgoing_clipboard_bytes == sizeof(copied_text) - 1);

    close(peer_pipe[0]);
    received = 0;
    while (boxrig_wayland_context_prepare_wait_sources(
               context,
               sources,
               2,
               &source_count) == BOXRIG_UI_OK &&
           source_count != 0) {
        size_t index;

        for (index = 0; index < source_count; index++) {
            sources[index].readiness = BOXRIG_WAYLAND_WAIT_WRITABLE;
        }
        CHECK(boxrig_wayland_context_dispatch_wait_sources(
                  context,
                  sources,
                  source_count,
                  1,
                  &transfer_result) == BOXRIG_UI_OK);
        while (received < sizeof(copied_text) - 1) {
            ssize_t read_count = read(
                blocked_pipe[0],
                received_text + received,
                sizeof(received_text) - received);

            if (read_count > 0) {
                received += (size_t)read_count;
            } else {
                CHECK(read_count == -1 &&
                      (errno == EAGAIN || errno == EWOULDBLOCK));
                break;
            }
        }
    }
    CHECK(received == sizeof(copied_text) - 1);
    CHECK(memcmp(received_text, copied_text, sizeof(copied_text) - 1) == 0);
    CHECK(read(blocked_pipe[0], buffer, sizeof(buffer)) == 0);
    close(blocked_pipe[0]);

    CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.outgoing_clipboard_bytes == 0);

    wl_array_release(&keys);
    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host_registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
}

static void test_paste_reads_validated_text_once_and_retries(void)
{
    static const char *const clipboard_mimes[] = {
        "text/plain",
        "text/html",
        "text/plain;charset=utf-8",
    };
    static const char *const plain_mime[] = {"text/plain"};
    static const char *const unsupported_mime[] = {"text/html"};
    static const uint8_t bracketed_mode[] = "\033[?2004h";
    static const uint8_t raw_text[] = {
        'a', '\r', '\n', 'b', '\r', 'c', '\t', 0x01, 'z', 0xe6, 0x97, 0xa5, 0xf0, 0x9f, 0x99, 0x82};
    static const uint8_t invalid_text[] = {0xff};
    static const uint8_t expected_text[] =
        "\033[200~a\nb\nc\tz日🙂\033[201~";
    fake_server server;
    host_registry_state host;
    struct wl_registry *host_registry;
    struct wl_surface *parent;
    struct wl_array keys;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_wayland_context_config context_config =
        protocol_context_config();
    boxrig_wayland_terminal_component_config component_config =
        boxrig_wayland_terminal_component_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_wayland_wait_source sources[2];
    boxrig_wayland_transfer_dispatch_result transfer_result;
    boxrig_wayland_dispatch_result dispatch_result;
    boxrig_wayland_context_diagnostic context_diagnostic;
    boxrig_wayland_component_diagnostic component_diagnostic;
    boxrig_ui_view_action action;
    const uint8_t *output;
    uint8_t observed[64];
    uint8_t oversized[25];
    size_t observed_length = 0;
    size_t source_count;
    size_t consumed;
    size_t attempts;
    uint32_t seat_name;

    memset(oversized, 'x', sizeof(oversized));
    create_server(&server, true, false, true);
    wl_array_init(&keys);
    host_registry = create_host_registry(&server, &host);
    terminal_config.rows = 1;
    terminal_config.columns = 32;
    terminal_config.output_capacity = 6;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              bracketed_mode,
              sizeof(bracketed_mode) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(bracketed_mode) - 1);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    context_config.transfer_chunk_size = 3;
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              &context_config,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    component_config.paste_capacity = sizeof(oversized) - 1;
    component_config.paste_chunk_size = 1;
    CHECK(create_steady_component(
              context,
              parent,
              model,
              &component_config,
              &component) == BOXRIG_UI_OK);
    boxrig_font_effective_metrics paste_metrics;
    boxrig_ui_component_allocation paste_allocation = {0};
    boxrig_ui_layout_result paste_layout;
    boxrig_wayland_host_action paste_parent_action;
    bool paste_parent_applied;
    CHECK(boxrig_wayland_terminal_component_get_effective_metrics(component, &paste_metrics) == BOXRIG_FONT_CATALOG_OK);
    paste_allocation.width = 32u * paste_metrics.cell_width;
    paste_allocation.height = paste_metrics.cell_height;
    paste_allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_wayland_terminal_component_allocate(component, &paste_allocation, &paste_layout) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_take_host_action(component, &paste_parent_action));
    wl_surface_commit(paste_parent_action.parent);
    CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component,
        paste_parent_action.revision, &paste_parent_applied) == BOXRIG_UI_OK && paste_parent_applied);
    pump(&server);
    pump(&server);
    send_keymap(&server, 0);
    pump(&server);
    seat_name = wl_global_get_name(server.seats[0], server.server_client);
    send_selection_offer(
        &server,
        0,
        false,
        clipboard_mimes,
        sizeof(clipboard_mimes) / sizeof(clipboard_mimes[0]));
    pump(&server);
    action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    CHECK(boxrig_wayland_terminal_component_apply_view_action(
              component,
              seat_name,
              NULL,
              &action) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_apply_view_action(
              component,
              seat_name,
              NULL,
              &action) == BOXRIG_UI_BUSY);
    uint64_t paste_move_revision;
    CHECK(boxrig_wayland_terminal_component_request_reparent(component, parent,
        &paste_move_revision) == BOXRIG_UI_BUSY);
    CHECK(server.subsurface_destroy_count == 0);
    CHECK(boxrig_wayland_context_prepare_wait_sources(
              context,
              sources,
              2,
              &source_count) == BOXRIG_UI_OK);
    CHECK(source_count == 1);
    CHECK(sources[0].interests == BOXRIG_WAYLAND_WAIT_READABLE);
    sources[0].readiness = BOXRIG_WAYLAND_WAIT_READABLE;
    CHECK(boxrig_wayland_context_dispatch_wait_sources(
              context,
              sources,
              source_count,
              1,
              &transfer_result) == BOXRIG_UI_OK);
    CHECK(transfer_result.processed_bytes == 0);
    pump(&server);
    CHECK(server.offer_receive_count == 1);
    CHECK(strcmp(
              server.offer_receive_mimes[0],
              "text/plain;charset=utf-8") == 0);
    CHECK(write(
              server.offer_receive_descriptors[0],
              raw_text,
              sizeof(raw_text)) == (ssize_t)sizeof(raw_text));
    close(server.offer_receive_descriptors[0]);
    for (attempts = 0; attempts < 16; attempts++) {
        size_t index;

        CHECK(boxrig_wayland_context_prepare_wait_sources(
                  context,
                  sources,
                  2,
                  &source_count) == BOXRIG_UI_OK);
        if (source_count == 0) {
            break;
        }
        for (index = 0; index < source_count; index++) {
            sources[index].readiness = BOXRIG_WAYLAND_WAIT_READABLE;
        }
        CHECK(boxrig_wayland_context_dispatch_wait_sources(
                  context,
                  sources,
                  source_count,
                  1,
                  &transfer_result) == BOXRIG_UI_OK);
        CHECK(transfer_result.processed_bytes <= 3);
    }
    CHECK(attempts < 16);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.paste_active);
    wl_keyboard_send_enter(
        server.keyboard_resources[0],
        302,
        server.child_resource,
        &keys);
    wl_keyboard_send_leave(
        server.keyboard_resources[0],
        303,
        server.child_resource);
    wl_keyboard_send_enter(
        server.keyboard_resources[0],
        304,
        server.child_resource,
        &keys);
    wl_keyboard_send_leave(
        server.keyboard_resources[0],
        305,
        server.child_resource);
    pump(&server);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.input_queue_depth == 4);
    CHECK(boxrig_wayland_context_dispatch(
              context,
              1,
              &dispatch_result) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_context_dispatch(
              context,
              1,
              &dispatch_result) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.input_queue_depth == 3);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 6);
    for (attempts = 0; attempts < 4; attempts++) {
        CHECK(boxrig_wayland_context_dispatch(
                  context,
                  1,
                  &dispatch_result) == BOXRIG_UI_OK);
        CHECK(boxrig_wayland_terminal_component_get_diagnostic(
                  component,
                  &component_diagnostic) == BOXRIG_UI_OK);
        if (component_diagnostic.paste_blocked) {
            break;
        }
    }
    CHECK(attempts < 4);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.paste_blocked);
    CHECK(boxrig_wayland_context_get_diagnostic(
              context,
              &context_diagnostic) == BOXRIG_UI_OK);
    CHECK(context_diagnostic.clipboard_blocked_reason ==
          BOXRIG_WAYLAND_CLIPBOARD_BLOCKED_TERMINAL);
    consumed = boxrig_terminal_output_peek(terminal, &output);
    CHECK(consumed <= sizeof(observed));
    memcpy(observed, output, consumed);
    observed_length = consumed;
    CHECK(boxrig_terminal_output_consume(terminal, consumed) ==
          BOXRIG_STATUS_OK);
    for (attempts = 0; attempts < 32; attempts++) {
        size_t length;

        CHECK(boxrig_wayland_context_dispatch(
                  context,
                  1,
                  &dispatch_result) == BOXRIG_UI_OK);
        length = boxrig_terminal_output_peek(terminal, &output);
        if (length != 0) {
            CHECK(length <= sizeof(observed) - observed_length);
            memcpy(observed + observed_length, output, length);
            observed_length += length;
            CHECK(boxrig_terminal_output_consume(terminal, length) ==
                  BOXRIG_STATUS_OK);
        }
        CHECK(boxrig_wayland_terminal_component_get_diagnostic(
                  component,
                  &component_diagnostic) == BOXRIG_UI_OK);
        if (!component_diagnostic.paste_active &&
            boxrig_terminal_output_peek(terminal, &output) == 0) {
            break;
        }
    }
    CHECK(attempts < 32);
    CHECK(observed_length == sizeof(expected_text) - 1);
    CHECK(memcmp(observed, expected_text, observed_length) == 0);
    CHECK(server.offer_receive_count == 1);
    CHECK(boxrig_wayland_context_get_diagnostic(
              context,
              &context_diagnostic) == BOXRIG_UI_OK);
    CHECK(context_diagnostic.incoming_clipboard_bytes == 0);
    CHECK(context_diagnostic.incoming_clipboard_bytes_high_water >=
          sizeof(raw_text));

    send_selection_offer(&server, 0, true, plain_mime, 1);
    pump(&server);
    action.clipboard_target =
        BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
    CHECK(boxrig_wayland_terminal_component_apply_view_action(
              component,
              seat_name,
              NULL,
              &action) == BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.offer_receive_count == 2);
    CHECK(write(
              server.offer_receive_descriptors[1],
              oversized,
              sizeof(oversized)) == (ssize_t)sizeof(oversized));
    close(server.offer_receive_descriptors[1]);
    for (attempts = 0; attempts < 16; attempts++) {
        CHECK(boxrig_wayland_context_prepare_wait_sources(
                  context,
                  sources,
                  2,
                  &source_count) == BOXRIG_UI_OK);
        if (source_count == 0) {
            break;
        }
        sources[0].readiness = BOXRIG_WAYLAND_WAIT_READABLE;
        CHECK(boxrig_wayland_context_dispatch_wait_sources(
                  context,
                  sources,
                  source_count,
                  1,
                  &transfer_result) == BOXRIG_UI_OK);
    }
    CHECK(attempts < 16);
    CHECK(boxrig_wayland_context_get_diagnostic(
              context,
              &context_diagnostic) == BOXRIG_UI_OK);
    CHECK(context_diagnostic.clipboard_failure ==
          BOXRIG_WAYLAND_CLIPBOARD_FAILURE_LIMIT_EXCEEDED);
    CHECK(context_diagnostic.active_clipboard_transfers == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    send_selection_offer(&server, 0, false, unsupported_mime, 1);
    pump(&server);
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    CHECK(boxrig_wayland_terminal_component_apply_view_action(
              component,
              seat_name,
              NULL,
              &action) == BOXRIG_UI_UNAVAILABLE);
    CHECK(server.offer_receive_count == 2);

    send_selection_offer(&server, 0, false, plain_mime, 1);
    pump(&server);
    CHECK(boxrig_wayland_terminal_component_apply_view_action(
              component,
              seat_name,
              NULL,
              &action) == BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.offer_receive_count == 3);
    CHECK(write(
              server.offer_receive_descriptors[2],
              invalid_text,
              sizeof(invalid_text)) == (ssize_t)sizeof(invalid_text));
    close(server.offer_receive_descriptors[2]);
    for (attempts = 0; attempts < 4; attempts++) {
        CHECK(boxrig_wayland_context_prepare_wait_sources(
                  context,
                  sources,
                  2,
                  &source_count) == BOXRIG_UI_OK);
        if (source_count == 0) {
            break;
        }
        sources[0].readiness = BOXRIG_WAYLAND_WAIT_READABLE;
        CHECK(boxrig_wayland_context_dispatch_wait_sources(
                  context,
                  sources,
                  source_count,
                  1,
                  &transfer_result) == BOXRIG_UI_OK);
    }
    CHECK(attempts < 4);
    CHECK(boxrig_wayland_context_get_diagnostic(
              context,
              &context_diagnostic) == BOXRIG_UI_OK);
    CHECK(context_diagnostic.clipboard_failure ==
          BOXRIG_WAYLAND_CLIPBOARD_FAILURE_INVALID_TEXT);

    send_selection_offer(&server, 0, false, plain_mime, 1);
    pump(&server);
    CHECK(boxrig_wayland_terminal_component_apply_view_action(
              component,
              seat_name,
              NULL,
              &action) == BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.offer_receive_count == 4);
    wl_data_device_send_selection(server.data_device_resources[0], NULL);
    wl_display_flush_clients(server.server_display);
    pump(&server);
    CHECK(boxrig_wayland_context_prepare_wait_sources(
              context,
              sources,
              2,
              &source_count) == BOXRIG_UI_OK);
    CHECK(source_count == 0);
    close(server.offer_receive_descriptors[3]);

    wl_array_release(&keys);
    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host_registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
}

static void test_owned_child_and_parent_handshake(void)
{
    fake_server server;
    host_registry_state host;
    struct wl_registry *host_registry;
    struct wl_surface *parent;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context_config context_config =
        protocol_context_config();
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_wayland_terminal_component *extra = NULL;
    boxrig_wayland_terminal_component_config component_config =
        boxrig_wayland_terminal_component_config_default();
    boxrig_wayland_host_action action;
    boxrig_ui_layout_action layout_action;
    boxrig_wayland_component_diagnostic diagnostic;
    boxrig_wayland_component_diagnostic before_failure;
    boxrig_wayland_context_diagnostic context_diagnostic;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_component_frame_result frame_result;
    boxrig_ui_layout_result layout;
    boxrig_ui_dirty dirty;
    boxrig_ui_hit_test_result annotation_hit;
    boxrig_ui_text_annotation annotation;
    size_t collapse_before;
    size_t consumed;
    uint64_t annotation_generation;
    bool applied;

    create_server(&server, true, false, true);
    host_registry = create_host_registry(&server, &host);
    terminal_config.rows = 2;
    terminal_config.columns = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    context_config.maximum_components = 1;
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              &context_config,
              &context) == BOXRIG_UI_OK);
    CHECK(create_steady_component(
              context,
              (struct wl_surface *)(uintptr_t)1,
              model,
              NULL,
              &component) == BOXRIG_UI_BUSY);
    CHECK(component == NULL);
    pump(&server);
    pump(&server);
    CHECK(host.compositor != NULL);
    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    component_config.maximum_dirty_rectangles = 1;
    CHECK(create_steady_component(
              context,
              parent,
              model,
              &component_config,
              &component) == BOXRIG_UI_OK);
    CHECK(component != NULL);
    allocation.width = 80;
    allocation.height = 40;
    allocation.x = 7;
    allocation.y = 9;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_wayland_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_surface(component) != NULL);
    CHECK(boxrig_wayland_terminal_component_view(component) != NULL);
    CHECK((boxrig_wayland_terminal_component_capabilities(component) &
           BOXRIG_UI_COMPONENT_CAPABILITY_OWNED_VISIBLE_CHILD) != 0);
    CHECK(create_steady_component(
              context,
              parent,
              model,
              NULL,
              &extra) == BOXRIG_UI_LIMIT_EXCEEDED);
    CHECK(extra == NULL);
    pump(&server);
    CHECK(server.surface_create_count == 2);
    CHECK(server.subsurface_create_count == 1);
    CHECK(server.subsurface_position_count == 2);
    CHECK(server.subsurface_desync_count == 1);
    CHECK(server.shm_pool_create_count == 1);
    CHECK(server.shm_buffer_create_count == 3);
    CHECK(server.shm_width == 80 && server.shm_height == 40);
    CHECK(server.shm_stride == 80 * 4);
    CHECK(server.shm_pool_size == 80 * 40 * 4 * 3);
    CHECK(server.shm_format == WL_SHM_FORMAT_XRGB8888);
    CHECK(boxrig_wayland_terminal_component_take_host_action(
        component,
        &action));
    CHECK(action.parent_commit_required && action.revision == 2);
    CHECK(action.x == 7 && action.y == 9);
    CHECK(boxrig_wayland_terminal_component_complete_parent_commit(
              component,
              action.revision + 1,
              &applied) == BOXRIG_UI_OK);
    CHECK(!applied);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.parent_commit_pending);
    CHECK(diagnostic.effective_parent_state_revision == 0);
    wl_surface_commit(parent);
    pump(&server);
    CHECK(boxrig_wayland_terminal_component_complete_parent_commit(
              component,
              action.revision,
              &applied) == BOXRIG_UI_OK);
    CHECK(applied);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    CHECK(!diagnostic.parent_commit_pending);
    CHECK(diagnostic.effective_parent_state_revision == action.revision);
    CHECK(diagnostic.effective_x == 7 && diagnostic.effective_y == 9);
    if (boxrig_wayland_terminal_component_take_layout_action(
            component, &layout_action)) {
        CHECK(layout_action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(boxrig_terminal_resize(
                  terminal,
                  layout_action.value.grid_resize.rows,
                  layout_action.value.grid_resize.columns) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_wayland_terminal_component_complete_grid_resize(
                  component,
                  layout_action.revision,
                  true) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    }
    CHECK(diagnostic.logical_width == 80 && diagnostic.logical_height == 40);
    CHECK(diagnostic.buffer_width == 80 && diagnostic.buffer_height == 40);
    CHECK(diagnostic.memory_bytes == (size_t)(80 * 40 * 4 * 4));
    CHECK(diagnostic.presentation_generation == 1);
    before_failure = diagnostic;
    allocation.width = UINT32_MAX;
    allocation.height = UINT32_MAX;
    CHECK(boxrig_wayland_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_LIMIT_EXCEEDED);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.logical_width == before_failure.logical_width);
    CHECK(diagnostic.logical_height == before_failure.logical_height);
    CHECK(diagnostic.memory_bytes == before_failure.memory_bytes);
    CHECK(diagnostic.presentation_generation ==
          before_failure.presentation_generation);
    CHECK(boxrig_wayland_terminal_component_set_visible(component, true) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(!frame_result.frame_pending);
    pump(&server);
    CHECK(server.child_attach_count == 1);
    CHECK(server.last_attached_buffer == 0);
    CHECK(server.child_dirty_buffer_count == 1);
    CHECK(server.frame_callback_count == 1);

    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"A",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 1);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(frame_result.frame_pending);
    CHECK(!frame_result.additional_dispatch);
    pump(&server);
    CHECK(server.child_attach_count == 1);

    complete_frame(&server, 0);
    pump(&server);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(!frame_result.frame_pending);
    pump(&server);
    CHECK(server.child_attach_count == 2);
    CHECK(server.last_attached_buffer == 1);

    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"B",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 1);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(frame_result.frame_pending);
    CHECK(!frame_result.additional_dispatch);
    complete_frame(&server, 1);
    pump(&server);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.child_attach_count == 3);
    CHECK(server.last_attached_buffer == 2);

    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"C",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 1);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(frame_result.frame_pending);
    complete_frame(&server, 2);
    pump(&server);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(frame_result.frame_pending);
    CHECK(!frame_result.additional_dispatch);
    pump(&server);
    CHECK(server.child_attach_count == 3);

    release_buffer(&server, 1);
    pump(&server);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(!frame_result.frame_pending);
    pump(&server);
    CHECK(server.child_attach_count == 4);
    CHECK(server.last_attached_buffer == 1);
    CHECK(server.child_dirty_buffer_count == 4);
    CHECK(server.dirty_x == 0 && server.dirty_width == 80);
    CHECK(server.dirty_y >= 0 && server.dirty_height > 0);
    CHECK(server.dirty_y + server.dirty_height <= 40);
    CHECK(server.dirty_height < 40);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.dirty_rectangle_high_water == 1);
    collapse_before = diagnostic.dirty_collapse_count;
    annotation_generation = diagnostic.presentation_generation;

    complete_frame(&server, 3);
    pump(&server);
    CHECK(boxrig_wayland_terminal_component_hit_test(
              component,
              BOXRIG_UI_FIXED_ONE,
              BOXRIG_UI_FIXED_ONE,
              &annotation_hit) == BOXRIG_UI_OK);
    CHECK(annotation_hit.inside_grid && annotation_hit.text_position_valid);
    memset(&annotation, 0, sizeof(annotation));
    annotation.source_id = 73;
    annotation.item_id = 1;
    annotation.range.start = annotation_hit.leading_position;
    annotation.range.end = annotation_hit.trailing_position;
    annotation.style.field_mask = BOXRIG_UI_ANNOTATION_BACKGROUND;
    annotation.style.background.kind = BOXRIG_COLOR_RGB;
    annotation.style.background.red = 0x12;
    annotation.style.background.green = 0x34;
    annotation.style.background.blue = 0x56;
    CHECK(boxrig_ui_view_replace_text_annotations(
              boxrig_wayland_terminal_component_view(component),
              annotation.source_id,
              annotation_hit.leading_position.screen,
              annotation_hit.leading_position.line_id,
              annotation_hit.leading_position.line_revision,
              &annotation,
              1) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(frame_result.frame_pending);
    CHECK(boxrig_ui_view_peek_dirty(
        boxrig_wayland_terminal_component_view(component),
        BOXRIG_UI_DIRTY_OVERLAY,
        &dirty));
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.presentation_generation == annotation_generation);
    dirty.row = 1;
    dirty.row_count = 1;
    dirty.column = 0;
    dirty.column_count = 1;
    CHECK(boxrig_ui_view_invalidate(
              boxrig_wayland_terminal_component_view(component),
              BOXRIG_UI_DIRTY_OVERLAY,
              dirty) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(frame_result.frame_pending);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.dirty_rectangle_high_water == 1);
    CHECK(diagnostic.dirty_collapse_count > collapse_before);
    release_buffer(&server, 1);
    pump(&server);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.child_attach_count == 5);
    CHECK(server.last_attached_buffer == 1);
    CHECK(server.child_dirty_buffer_count == 5);
    CHECK(server.dirty_x == 0 && server.dirty_y == 0);
    CHECK(server.dirty_width == 80 && server.dirty_height == 40);
    CHECK(!boxrig_ui_view_peek_dirty(
        boxrig_wayland_terminal_component_view(component),
        BOXRIG_UI_DIRTY_OVERLAY,
        &dirty));
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.presentation_generation > annotation_generation);
    boxrig_wayland_terminal_component_destroy(component);
    pump(&server);
    CHECK(server.child_null_attach_count == 1);
    CHECK(server.subsurface_destroy_count == 1);
    CHECK(server.surface_destroy_count == 1);
    CHECK(server.shm_buffer_destroy_count == 3);
    CHECK(server.shm_pool_destroy_count == 1);
    CHECK(server.null_attach_order < server.child_unmap_commit_order);
    CHECK(server.child_unmap_commit_order < server.subsurface_destroy_order);
    CHECK(server.subsurface_destroy_order < server.child_surface_destroy_order);
    CHECK(boxrig_wayland_context_get_diagnostic(
              context,
              &context_diagnostic) == BOXRIG_UI_OK);
    CHECK(context_diagnostic.component_count == 0);
    wl_surface_commit(parent);
    pump(&server);
    CHECK(server.parent_commit_count == 2);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host_registry);
    pump(&server);
    CHECK(server.surface_destroy_count == 2);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
}

static void test_visible_resize_commits_child_with_parent(void)
{
    fake_server server;
    host_registry_state host;
    struct wl_registry *host_registry;
    struct wl_surface *parent;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame_result;
    boxrig_wayland_host_action action;
    boxrig_ui_layout_action layout_action;
    bool applied;

    create_server(&server, true, false, true);
    host_registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              NULL,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    CHECK(host.compositor != NULL);
    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    CHECK(create_steady_component(
              context,
              parent,
              model,
              NULL,
              &component) == BOXRIG_UI_OK);
    allocation.width = 80;
    allocation.height = 40;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_wayland_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_take_layout_action(
        component, &layout_action));
    CHECK(layout_action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
    CHECK(boxrig_terminal_resize(
              terminal,
              layout_action.value.grid_resize.rows,
              layout_action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
    CHECK(boxrig_wayland_terminal_component_complete_grid_resize(
              component,
              layout_action.revision,
              true) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_take_host_action(
        component,
        &action));
    wl_surface_commit(parent);
    CHECK(boxrig_wayland_terminal_component_complete_parent_commit(
              component,
              action.revision,
              &applied) == BOXRIG_UI_OK);
    CHECK(applied);
    CHECK(boxrig_wayland_terminal_component_set_visible(component, true) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.child_attach_count == 1);
    CHECK(server.child_commit_count == 1);
    CHECK(server.frame_callback_count == 1);

    allocation.width = 96;
    allocation.height = 48;
    CHECK(boxrig_wayland_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_take_layout_action(
        component, &layout_action));
    CHECK(layout_action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
    CHECK(boxrig_terminal_resize(
              terminal,
              layout_action.value.grid_resize.rows,
              layout_action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
    CHECK(boxrig_wayland_terminal_component_complete_grid_resize(
              component,
              layout_action.revision,
              true) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_take_host_action(
        component,
        &action));
    wl_surface_commit(parent);
    CHECK(boxrig_wayland_terminal_component_complete_parent_commit(
              component,
              action.revision,
              &applied) == BOXRIG_UI_OK);
    CHECK(applied);
    pump(&server);
    CHECK(server.subsurface_sync_count == 1);
    CHECK(server.child_attach_count == 2);
    CHECK(server.child_commit_count == 2);
    CHECK(server.parent_commit_count == 2);
    CHECK(server.subsurface_desync_count == 2);
    CHECK(server.subsurface_sync_order < server.child_attach_order);
    CHECK(server.child_attach_order < server.child_commit_order);
    CHECK(server.child_commit_order < server.parent_commit_order);
    CHECK(server.parent_commit_order < server.subsurface_desync_order);

    /* Buffers not used by the resize must get a complete image on their first
     * presentation, even if only a small area is invalidated afterwards. The
     * compositor still owns the first buffer throughout these two frames. */
    size_t image_bytes = (size_t)server.shm_stride * (size_t)server.shm_height;
    size_t resize_buffer = server.last_attached_buffer;
    uint8_t *resize_pixels = (uint8_t *)server.shm_mapping + server.shm_buffer_offsets[resize_buffer];
    uint8_t *expected = malloc(image_bytes);
    CHECK(expected != NULL);
    memcpy(expected, resize_pixels, image_bytes);
    for (size_t index = 0; index < 2; index++) {
        for (size_t frame = 0; frame < server.frame_callback_count; frame++) {
            if (server.frame_callbacks[frame] != NULL) complete_frame(&server, frame);
        }
        pump(&server);
        boxrig_ui_dirty dirty = {.row_count = 1, .column_count = 1};
        CHECK(boxrig_ui_view_invalidate(boxrig_wayland_terminal_component_view(component),
            BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
        CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) == BOXRIG_UI_OK);
        pump(&server);
        CHECK(server.last_attached_buffer == resize_buffer + index + 1);
        const uint8_t *pixels = (const uint8_t *)server.shm_mapping +
            server.shm_buffer_offsets[server.last_attached_buffer];
        CHECK(memcmp(pixels, expected, image_bytes) == 0);
        CHECK(memcmp(resize_pixels, expected, image_bytes) == 0);
    }
    free(expected);
    puts("PASS Wayland resized buffers initialize on presentation and preserve busy buffers");

    /* An output change must not replace a child buffer staged for an
     * outstanding parent commit. The scale belongs to the following frame. */
    allocation.width = 112;
    allocation.height = 56;
    CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_take_layout_action(component, &layout_action));
    CHECK(boxrig_terminal_resize(terminal, layout_action.value.grid_resize.rows,
        layout_action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
    CHECK(boxrig_wayland_terminal_component_complete_grid_resize(component,
        layout_action.revision, true) == BOXRIG_UI_OK);
    pump(&server);
    wl_surface_send_enter(server.child_resource, server.output_resource);
    pump(&server);
    boxrig_wayland_dispatch_result scale_dispatch;
    CHECK(boxrig_wayland_context_dispatch(context, BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT,
        &scale_dispatch) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) == BOXRIG_UI_OK);
    boxrig_wayland_component_diagnostic scale_diagnostic;
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &scale_diagnostic) == BOXRIG_UI_OK);
    CHECK(scale_diagnostic.scale_numerator == 120);
    CHECK(scale_diagnostic.buffer_width == 112 && scale_diagnostic.buffer_height == 56);
    CHECK(scale_diagnostic.parent_commit_pending);
    CHECK(boxrig_wayland_terminal_component_take_host_action(component, &action));
    wl_surface_commit(action.parent);
    CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component,
        action.revision, &applied) == BOXRIG_UI_OK && applied);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &scale_diagnostic) == BOXRIG_UI_OK);
    CHECK(scale_diagnostic.scale_numerator == 240);
    CHECK(scale_diagnostic.buffer_width == 224 && scale_diagnostic.buffer_height == 112);

    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host_registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
}

static void test_integer_output_scale_and_hit_test(void)
{
    fake_server server;
    host_registry_state host;
    struct wl_registry *host_registry;
    struct wl_surface *parent;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame_result;
    boxrig_wayland_component_diagnostic diagnostic;
    boxrig_wayland_context_diagnostic context_diagnostic;
    boxrig_wayland_dispatch_result dispatch_result;
    boxrig_ui_hit_test_result transformed_hit;
    boxrig_ui_hit_test_result direct_hit;
    size_t attached;

    create_server(&server, true, false, true);
    host_registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              NULL,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    CHECK(create_steady_component(
              context,
              parent,
              model,
              NULL,
              &component) == BOXRIG_UI_OK);
    allocation.width = 80;
    allocation.height = 40;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_wayland_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_set_visible(component, true) ==
          BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.child_resource != NULL && server.output_resource != NULL);
    wl_surface_send_enter(server.child_resource, server.output_resource);
    pump(&server);
    CHECK(boxrig_wayland_context_get_diagnostic(
              context,
              &context_diagnostic) == BOXRIG_UI_OK);
    CHECK(context_diagnostic.work_queue_depth == 1);
    CHECK(context_diagnostic.work_queue_high_water == 1);
    CHECK(boxrig_wayland_context_dispatch(context, 1, &dispatch_result) ==
          BOXRIG_UI_OK);
    CHECK(dispatch_result.processed_items == 1);
    CHECK(!dispatch_result.additional_dispatch);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.buffer_scale == 2);
    CHECK(server.shm_width == 160 && server.shm_height == 80);
    CHECK(server.dirty_width == 160 && server.dirty_height == 80);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.scale_numerator == 240);
    CHECK(diagnostic.buffer_scale == 2);
    CHECK(!diagnostic.fractional_scale_active);
    CHECK(diagnostic.entered_output_count == 1);
    CHECK(diagnostic.scale_revision == 2);
    CHECK(boxrig_wayland_terminal_component_hit_test(
              component,
              4 * BOXRIG_UI_FIXED_ONE,
              4 * BOXRIG_UI_FIXED_ONE,
              &transformed_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              boxrig_wayland_terminal_component_view(component),
              8 * BOXRIG_UI_FIXED_ONE,
              8 * BOXRIG_UI_FIXED_ONE,
              &direct_hit) == BOXRIG_UI_OK);
    CHECK(transformed_hit.inside_grid == direct_hit.inside_grid);
    CHECK(transformed_hit.row == direct_hit.row);
    CHECK(transformed_hit.visual_column == direct_hit.visual_column);

    attached = server.last_attached_buffer;
    complete_frame(&server, 0);
    release_buffer(&server, attached);
    pump(&server);
    wl_surface_send_leave(server.child_resource, server.output_resource);
    pump(&server);
    CHECK(boxrig_wayland_context_dispatch(context, 1, &dispatch_result) ==
          BOXRIG_UI_OK);
    CHECK(dispatch_result.processed_items == 1);
    CHECK(!dispatch_result.additional_dispatch);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.buffer_scale == 1);
    CHECK(server.shm_width == 80 && server.shm_height == 40);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.scale_numerator == 120);
    CHECK(diagnostic.entered_output_count == 0);
    CHECK(diagnostic.scale_revision == 3);

    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host_registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
}

static void test_fractional_scale_viewport_and_hit_test(void)
{
    fake_server server;
    host_registry_state host;
    struct wl_registry *host_registry;
    struct wl_surface *parent;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame_result;
    boxrig_wayland_component_diagnostic diagnostic;
    boxrig_wayland_context_diagnostic context_diagnostic;
    boxrig_wayland_dispatch_result dispatch_result;
    boxrig_ui_hit_test_result transformed_hit;
    boxrig_ui_hit_test_result direct_hit;

    create_server(&server, true, true, true);
    host_registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              NULL,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    CHECK(create_steady_component(
              context,
              parent,
              model,
              NULL,
              &component) == BOXRIG_UI_OK);
    allocation.width = 80;
    allocation.height = 40;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_wayland_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_set_visible(component, true) ==
          BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.viewport_create_count == 1);
    CHECK(server.fractional_scale_create_count == 1);
    CHECK(server.fractional_scale_resource != NULL);
    wp_fractional_scale_v1_send_preferred_scale(
        server.fractional_scale_resource,
        150);
    pump(&server);
    CHECK(boxrig_wayland_context_get_diagnostic(
              context,
              &context_diagnostic) == BOXRIG_UI_OK);
    CHECK(context_diagnostic.work_queue_depth == 1);
    CHECK(context_diagnostic.work_queue_high_water == 1);
    CHECK(boxrig_wayland_context_dispatch(context, 1, &dispatch_result) ==
          BOXRIG_UI_OK);
    CHECK(dispatch_result.processed_items == 1);
    CHECK(!dispatch_result.additional_dispatch);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.buffer_scale == 1);
    CHECK(server.viewport_width == 80 && server.viewport_height == 40);
    CHECK(server.shm_width == 100 && server.shm_height == 50);
    CHECK(server.dirty_x == 0 && server.dirty_y == 0);
    CHECK(server.dirty_width == 100 && server.dirty_height == 50);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.scale_numerator == 150);
    CHECK(diagnostic.buffer_scale == 1);
    CHECK(diagnostic.fractional_scale_active);
    CHECK(diagnostic.viewport_width == 80 && diagnostic.viewport_height == 40);
    CHECK(diagnostic.scale_revision == 2);
    CHECK(boxrig_wayland_terminal_component_hit_test(
              component,
              4 * BOXRIG_UI_FIXED_ONE,
              4 * BOXRIG_UI_FIXED_ONE,
              &transformed_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              boxrig_wayland_terminal_component_view(component),
              5 * BOXRIG_UI_FIXED_ONE,
              5 * BOXRIG_UI_FIXED_ONE,
              &direct_hit) == BOXRIG_UI_OK);
    CHECK(transformed_hit.inside_grid == direct_hit.inside_grid);
    CHECK(transformed_hit.row == direct_hit.row);
    CHECK(transformed_hit.visual_column == direct_hit.visual_column);

    /* The compositor must display each resized buffer at the new logical
     * extent, including when the output scale has not changed. */
    const unsigned int sizes[][2] = {{123, 67}, {61, 31}, {80, 40}};
    for (size_t index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
        boxrig_wayland_host_action host_action;
        boxrig_ui_layout_action grid_action;
        boxrig_font_effective_metrics metrics;
        bool applied;
        size_t rows;
        size_t columns;

        for (size_t frame = 0; frame < server.frame_callback_count; frame++) {
            if (server.frame_callbacks[frame] != NULL) {
                complete_frame(&server, frame);
            }
        }
        pump(&server);
        allocation.width = sizes[index][0];
        allocation.height = sizes[index][1];
        CHECK(boxrig_wayland_terminal_component_allocate(
                  component, &allocation, &layout) == BOXRIG_UI_OK);
        if (boxrig_wayland_terminal_component_take_layout_action(
                component, &grid_action)) {
            CHECK(grid_action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
            CHECK(boxrig_terminal_resize(terminal,
                      grid_action.value.grid_resize.rows,
                      grid_action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
            CHECK(boxrig_wayland_terminal_component_complete_grid_resize(
                      component, grid_action.revision, true) == BOXRIG_UI_OK);
        }
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(boxrig_wayland_terminal_component_take_host_action(
            component, &host_action));
        wl_surface_commit(parent);
        CHECK(boxrig_wayland_terminal_component_complete_parent_commit(
                  component, host_action.revision, &applied) == BOXRIG_UI_OK);
        CHECK(applied);
        pump(&server);
        CHECK(server.committed_viewport_width == (int32_t)allocation.width);
        CHECK(server.committed_viewport_height == (int32_t)allocation.height);
        CHECK(server.shm_width == (int32_t)((allocation.width * 150 + 119) / 120));
        CHECK(server.shm_height == (int32_t)((allocation.height * 150 + 119) / 120));
        CHECK(boxrig_wayland_terminal_component_get_effective_metrics(
                  component, &metrics) == BOXRIG_FONT_CATALOG_OK);
        boxrig_terminal_size(terminal, &rows, &columns);
        CHECK(columns == (size_t)server.shm_width / metrics.cell_width);
        CHECK(rows == (size_t)server.shm_height / metrics.cell_height);
        CHECK(boxrig_wayland_terminal_component_hit_test(component,
                  ((boxrig_ui_fixed)allocation.width - 1) * BOXRIG_UI_FIXED_ONE,
                  ((boxrig_ui_fixed)allocation.height - 1) * BOXRIG_UI_FIXED_ONE,
                  &transformed_hit) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_hit_test(
                  boxrig_wayland_terminal_component_view(component),
                  ((boxrig_ui_fixed)allocation.width - 1) * BOXRIG_UI_FIXED_ONE * 150 / 120,
                  ((boxrig_ui_fixed)allocation.height - 1) * BOXRIG_UI_FIXED_ONE * 150 / 120,
                  &direct_hit) == BOXRIG_UI_OK);
        CHECK(transformed_hit.inside_grid == direct_hit.inside_grid);
        CHECK(transformed_hit.row == direct_hit.row);
        CHECK(transformed_hit.visual_column == direct_hit.visual_column);
    }
    puts("PASS fractional Wayland resize updates committed extent, terminal grid, and hit testing");

    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host_registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
}

static void test_work_queue_overflow_is_sticky(void)
{
    fake_server server;
    host_registry_state host;
    struct wl_registry *host_registry;
    struct wl_surface *parent;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context_config context_config =
        protocol_context_config();
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *first = NULL;
    boxrig_wayland_terminal_component *second = NULL;
    boxrig_wayland_terminal_component_config component_config =
        boxrig_wayland_terminal_component_config_default();
    boxrig_wayland_context_diagnostic context_diagnostic;
    boxrig_wayland_component_diagnostic component_diagnostic;
    boxrig_wayland_dispatch_result dispatch_result;

    create_server(&server, true, false, true);
    host_registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    context_config.maximum_components = 2;
    context_config.maximum_work_items = 1;
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              &context_config,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    CHECK(create_steady_component(
              context,
              parent,
              model,
              &component_config,
              &first) == BOXRIG_UI_OK);
    component_config.attachment = BOXRIG_UI_COMPONENT_PRESENTATION_ONLY;
    CHECK(create_steady_component(
              context,
              parent,
              model,
              &component_config,
              &second) == BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.child_surface_count == 2);
    wl_surface_send_enter(server.child_resources[0], server.output_resource);
    wl_surface_send_enter(server.child_resources[1], server.output_resource);
    pump(&server);
    CHECK(boxrig_wayland_context_get_diagnostic(
              context,
              &context_diagnostic) == BOXRIG_UI_OK);
    CHECK(context_diagnostic.state == BOXRIG_WAYLAND_CONTEXT_FATAL);
    CHECK(context_diagnostic.failure_reason ==
          BOXRIG_WAYLAND_FAILURE_WORK_QUEUE_OVERFLOW);
    CHECK(context_diagnostic.work_queue_depth == 1);
    CHECK(context_diagnostic.work_queue_high_water == 1);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              second,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.failure_reason ==
          BOXRIG_WAYLAND_FAILURE_WORK_QUEUE_OVERFLOW);
    CHECK(boxrig_wayland_context_dispatch(context, 1, &dispatch_result) ==
          BOXRIG_UI_UNAVAILABLE);
    CHECK(dispatch_result.processed_items == 0);

    boxrig_wayland_terminal_component_destroy(second);
    boxrig_wayland_terminal_component_destroy(first);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host_registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
}

static void test_transparency_regions_and_fallback(void)
{
    fake_server server;
    host_registry_state host;
    struct wl_registry *host_registry;
    struct wl_surface *parent;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame_result;
    boxrig_ui_transparency_config transparency;
    boxrig_ui_transparency_state state;
    boxrig_wayland_component_diagnostic before_failure;
    boxrig_wayland_component_diagnostic after_failure;
    size_t initial_attach_count;
    size_t attached;

    create_server(&server, true, false, true);
    host_registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              NULL,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    CHECK(create_steady_component(
              context,
              parent,
              model,
              NULL,
              &component) == BOXRIG_UI_OK);
    allocation.width = 80;
    allocation.height = 40;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_wayland_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_set_visible(component, true) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.shm_format == WL_SHM_FORMAT_XRGB8888);
    CHECK(server.opaque_region_nonnull);
    CHECK(server.input_region_nonnull);
    CHECK(server.region_add_count == 2);
    CHECK(server.region_width == 80 && server.region_height == 40);
    CHECK(server.region_create_count == server.region_destroy_count);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &before_failure) == BOXRIG_UI_OK);
    CHECK(before_failure.frame_callback_pending);
    initial_attach_count = server.child_attach_count;

    transparency = boxrig_ui_transparency_config_default();
    transparency.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
    transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    transparency.background_opacity = 128;
    CHECK(boxrig_wayland_terminal_component_set_transparency(
              component,
              &transparency) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_transparency(
              component,
              &state) == BOXRIG_UI_OK);
    CHECK(state.effective == BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA);
    CHECK(state.effective_background_opacity == 128);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.child_attach_count == initial_attach_count + 1);
    CHECK(server.shm_format == WL_SHM_FORMAT_ARGB8888);
    CHECK(!server.opaque_region_nonnull);
    CHECK(server.input_region_nonnull);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &before_failure) == BOXRIG_UI_OK);
    CHECK(before_failure.shm_format == WL_SHM_FORMAT_ARGB8888);

    transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
    CHECK(boxrig_wayland_terminal_component_set_transparency(
              component,
              &transparency) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_wayland_terminal_component_get_transparency(
              component,
              &state) == BOXRIG_UI_OK);
    CHECK(state.config.request == BOXRIG_UI_TRANSPARENCY_PREFERRED);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &after_failure) == BOXRIG_UI_OK);
    CHECK(after_failure.shm_format == before_failure.shm_format);
    CHECK(after_failure.presentation_generation ==
          before_failure.presentation_generation);

    attached = server.last_attached_buffer;
    complete_frame(&server, 0);
    release_buffer(&server, attached);
    pump(&server);
    transparency.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
    CHECK(boxrig_wayland_terminal_component_set_transparency(
              component,
              &transparency) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_transparency(
              component,
              &state) == BOXRIG_UI_OK);
    CHECK(state.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
    CHECK(state.reason == BOXRIG_UI_TRANSPARENCY_REASON_SCOPE_UNSUPPORTED);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.shm_format == WL_SHM_FORMAT_XRGB8888);
    CHECK(server.opaque_region_nonnull);

    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host_registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
}

static void test_visual_zoom_issues_one_application_size_command(void)
{
    fake_server server;
    host_registry_state host;
    struct wl_registry *host_registry;
    struct wl_surface *parent;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_layout_result completed_layout;
    boxrig_ui_size_command_state command_state;
    boxrig_ui_layout_action action;
    boxrig_font_effective_metrics initial_metrics;
    boxrig_font_effective_metrics zoomed_metrics;
    boxrig_font_effective_metrics reset_metrics;
    bool changed;
    bool applied;

    create_server(&server, true, false, true);
    host_registry = create_host_registry(&server, &host);
    terminal_config.rows = 4;
    terminal_config.columns = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              NULL,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    CHECK(create_steady_component(
              context,
              parent,
              model,
              NULL,
              &component) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_effective_metrics(
              component,
              &initial_metrics) == BOXRIG_FONT_CATALOG_OK);
    allocation.width =
        terminal_config.columns * initial_metrics.cell_width;
    allocation.height = terminal_config.rows * initial_metrics.cell_height;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_wayland_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);

    CHECK(boxrig_wayland_terminal_component_visual_zoom(
              component,
              BOXRIG_FONT_ZOOM_INCREASE,
              boxrig_font_visual_step_policy_default(),
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(boxrig_wayland_terminal_component_get_effective_metrics(
              component,
              &zoomed_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(zoomed_metrics.requested_size_dip >
          initial_metrics.requested_size_dip);
    CHECK(boxrig_ui_view_get_size_command_state(
              boxrig_wayland_terminal_component_view(component),
              &command_state) == BOXRIG_UI_OK);
    CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_READY);
    CHECK(boxrig_wayland_terminal_component_take_layout_action(
        component, &action));
    CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE);
    CHECK(action.value.content_size.cause ==
          BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS);
    CHECK(action.value.content_size.width ==
          (boxrig_ui_fixed)(terminal_config.columns *
                            zoomed_metrics.cell_width) *
              BOXRIG_UI_FIXED_ONE);
    CHECK(action.value.content_size.height ==
          (boxrig_ui_fixed)(terminal_config.rows *
                            zoomed_metrics.cell_height) *
              BOXRIG_UI_FIXED_ONE);
    CHECK(!boxrig_wayland_terminal_component_take_layout_action(
        component, &action));
    CHECK(boxrig_wayland_terminal_component_visual_zoom(
              component,
              BOXRIG_FONT_ZOOM_INCREASE,
              boxrig_font_visual_step_policy_default(),
              &changed) == BOXRIG_UI_BUSY);
    CHECK(!changed);
    CHECK(boxrig_wayland_terminal_component_get_effective_metrics(
              component,
              &reset_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(reset_metrics.requested_size_dip ==
          zoomed_metrics.requested_size_dip);
    CHECK(boxrig_ui_view_get_size_command_state(
              boxrig_wayland_terminal_component_view(component),
              &command_state) == BOXRIG_UI_OK);
    CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_DISPATCHED);
    allocation.width = (unsigned int)(
        action.value.content_size.width / BOXRIG_UI_FIXED_ONE);
    allocation.height = (unsigned int)(
        action.value.content_size.height / BOXRIG_UI_FIXED_ONE);
    CHECK(boxrig_wayland_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_complete_size_command(
              component,
              action.revision,
              BOXRIG_UI_SIZE_ACCEPTED,
              action.value.content_size.width,
              action.value.content_size.height,
              &applied,
              &completed_layout) == BOXRIG_UI_OK);
    CHECK(applied);

    CHECK(boxrig_wayland_terminal_component_visual_zoom_reset(
              component,
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(boxrig_wayland_terminal_component_get_effective_metrics(
              component,
              &reset_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(reset_metrics.requested_size_dip ==
          initial_metrics.requested_size_dip);
    CHECK(boxrig_ui_view_get_size_command_state(
              boxrig_wayland_terminal_component_view(component),
              &command_state) == BOXRIG_UI_OK);
    CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_READY);

    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host_registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
}

static void test_constrained_visual_zoom_dispatches_one_grid_action(void)
{
    fake_server server;
    host_registry_state host;
    struct wl_registry *host_registry;
    struct wl_surface *parent;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_layout_action action;
    boxrig_ui_component_frame_result frame_result;
    boxrig_wayland_component_diagnostic before;
    boxrig_wayland_component_diagnostic after;
    boxrig_font_effective_metrics initial_metrics;
    boxrig_font_effective_metrics zoomed_metrics;
    size_t rows;
    size_t columns;
    bool changed;

    create_server(&server, true, false, true);
    host_registry = create_host_registry(&server, &host);
    terminal_config.rows = 4;
    terminal_config.columns = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              NULL,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    CHECK(create_steady_component(
              context,
              parent,
              model,
              NULL,
              &component) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_effective_metrics(
              component,
              &initial_metrics) == BOXRIG_FONT_CATALOG_OK);
    allocation.width =
        terminal_config.columns * initial_metrics.cell_width;
    allocation.height = terminal_config.rows * initial_metrics.cell_height;
    allocation.window_state = BOXRIG_UI_WINDOW_MAXIMIZED;
    CHECK(boxrig_wayland_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(!boxrig_wayland_terminal_component_take_layout_action(
        component, &action));
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &before) == BOXRIG_UI_OK);

    CHECK(boxrig_wayland_terminal_component_visual_zoom(
              component,
              BOXRIG_FONT_ZOOM_DECREASE,
              boxrig_font_visual_step_policy_default(),
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(boxrig_wayland_terminal_component_get_effective_metrics(
              component,
              &zoomed_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(zoomed_metrics.requested_size_dip <
          initial_metrics.requested_size_dip);
    CHECK(boxrig_wayland_terminal_component_frame(
              component, &frame_result) == BOXRIG_UI_OK);
    CHECK(frame_result.frame_pending && frame_result.additional_dispatch);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &after) == BOXRIG_UI_OK);
    CHECK(after.presentation_generation == before.presentation_generation);
    CHECK(boxrig_wayland_terminal_component_take_layout_action(
        component, &action));
    CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
    CHECK(action.value.grid_resize.rows ==
          allocation.height / zoomed_metrics.cell_height);
    CHECK(action.value.grid_resize.columns ==
          allocation.width / zoomed_metrics.cell_width);
    CHECK(action.value.grid_resize.rows != terminal_config.rows ||
          action.value.grid_resize.columns != terminal_config.columns);
    CHECK(!boxrig_wayland_terminal_component_take_layout_action(
        component, &action));
    CHECK(boxrig_wayland_terminal_component_frame(
              component, &frame_result) == BOXRIG_UI_OK);
    CHECK(frame_result.frame_pending && !frame_result.additional_dispatch);
    CHECK(boxrig_wayland_terminal_component_complete_grid_resize(
              component, action.revision + 1, true) == BOXRIG_UI_STALE);
    CHECK(boxrig_wayland_terminal_component_complete_grid_resize(
              component, action.revision, true) ==
          BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_resize(
              terminal,
              action.value.grid_resize.rows,
              action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
    CHECK(boxrig_wayland_terminal_component_complete_grid_resize(
              component, action.revision, true) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_complete_grid_resize(
              component, action.revision, true) == BOXRIG_UI_STALE);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_frame(
              component, &frame_result) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &after) == BOXRIG_UI_OK);
    CHECK(after.presentation_generation > before.presentation_generation);
    boxrig_terminal_size(terminal, &rows, &columns);
    CHECK(rows == action.value.grid_resize.rows);
    CHECK(columns == action.value.grid_resize.columns);
    CHECK(boxrig_wayland_terminal_component_get_layout(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(layout.rows == rows && layout.columns == columns);

    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host_registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
}

static void test_missing_argb_policy(void)
{
    fake_server server;
    host_registry_state host;
    struct wl_registry *host_registry;
    struct wl_surface *parent;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_wayland_terminal_component_config config =
        boxrig_wayland_terminal_component_config_default();
    boxrig_ui_transparency_state state;

    create_server(&server, true, false, false);
    host_registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              NULL,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    config.transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    config.transparency.requested_scopes =
        BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    config.transparency.background_opacity = 128;
    CHECK(create_steady_component(
              context,
              parent,
              model,
              &config,
              &component) == BOXRIG_UI_UNAVAILABLE);
    CHECK(component == NULL);
    config.transparency.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
    CHECK(create_steady_component(
              context,
              parent,
              model,
              &config,
              &component) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_transparency(
              component,
              &state) == BOXRIG_UI_OK);
    CHECK(state.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
    CHECK(state.reason ==
          BOXRIG_UI_TRANSPARENCY_REASON_ARGB32_TARGET_UNAVAILABLE);

    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host_registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
}

typedef struct key_route_capture {
    boxrig_key_event events[32];
    size_t count;
    boxrig_wayland_terminal_component *inhibition_probe;
} key_route_capture;

typedef struct mouse_route_capture {
    boxrig_mouse_event events[16];
    boxrig_ui_hit_test_result hits[16];
    size_t count;
} mouse_route_capture;

static boxrig_input_key_route capture_key_route(
    const boxrig_key_event *event,
    void *context)
{
    key_route_capture *capture = context;

    CHECK(capture->count < 32);
    capture->events[capture->count++] = *event;
    if (capture->inhibition_probe != NULL) {
        boxrig_ui_input_state before, after;
        CHECK(boxrig_wayland_terminal_component_get_input_state(capture->inhibition_probe, &before) == BOXRIG_UI_OK);
        CHECK(boxrig_wayland_terminal_component_set_input_enabled(capture->inhibition_probe, false) == BOXRIG_UI_BUSY);
        CHECK(boxrig_wayland_terminal_component_get_input_state(capture->inhibition_probe, &after) == BOXRIG_UI_OK);
        CHECK(before.phase == after.phase && before.generation == after.generation);
    }
    return event->codepoint == '1' ? BOXRIG_INPUT_KEY_ROUTE_APPLICATION
                                  : BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
}

static boxrig_input_mouse_route capture_mouse_route(
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    void *context)
{
    mouse_route_capture *capture = context;

    CHECK(hit != NULL && capture->count < 16);
    capture->events[capture->count] = *event;
    capture->hits[capture->count++] = *hit;
    return event->action == BOXRIG_MOUSE_PRESS &&
                   event->button == BOXRIG_MOUSE_BUTTON_LEFT
               ? BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION
               : BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
}

static void dispatch_context(boxrig_wayland_context *context)
{
    boxrig_wayland_dispatch_result result;

    CHECK(boxrig_wayland_context_dispatch(
              context,
              BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT,
              &result) == BOXRIG_UI_OK);
}

static void test_seat_keyboard_routing_and_removal(void)
{
    fake_server server;
    host_registry_state host;
    struct wl_registry *host_registry;
    struct wl_surface *parent;
    struct wl_array keys;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_wayland_context_diagnostic context_diagnostic;
    boxrig_wayland_component_diagnostic component_diagnostic;
    boxrig_wayland_dispatch_result dispatch_result;
    boxrig_wayland_serial serial;
    key_route_capture capture = {0};
    uint32_t seat_name;
    const uint8_t *output;
    size_t output_length;

    create_server(&server, true, false, true);
    host_registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              NULL,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    pump(&server);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &context_diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(context_diagnostic.seat_count == 1);
    CHECK(context_diagnostic.xkb_available);
    CHECK(server.seat_bind_count == 1);
    CHECK(server.keyboard_create_count == 1);
    CHECK(server.pointer_create_count == 1);
    seat_name = wl_global_get_name(server.seats[0], server.server_client);
    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    CHECK(create_steady_component(
              context,
              parent,
              model,
              NULL,
              &component) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_set_key_router(
              component,
              capture_key_route,
              &capture) == BOXRIG_UI_OK);
    boxrig_input_config replacement = boxrig_input_config_default();
    CHECK(boxrig_wayland_terminal_component_set_input_config(component, &replacement) == BOXRIG_UI_OK);
    pump(&server);
    send_keymap(&server, 0);
    wl_array_init(&keys);
    wl_keyboard_send_enter(
        server.keyboard_resources[0],
        41,
        server.parent_resource,
        &keys);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        42,
        10,
        30,
        WL_KEYBOARD_KEY_STATE_PRESSED);
    pump(&server);
    dispatch_context(context);
    CHECK(capture.count == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    wl_keyboard_send_enter(
        server.keyboard_resources[0],
        43,
        server.child_resource,
        &keys);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        44,
        11,
        30,
        WL_KEYBOARD_KEY_STATE_PRESSED);
    pump(&server);
    CHECK(boxrig_wayland_terminal_component_set_input_config(component, &replacement) == BOXRIG_UI_BUSY);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        45,
        12,
        30,
        WL_KEYBOARD_KEY_STATE_RELEASED);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        46,
        13,
        2,
        WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        47,
        14,
        2,
        WL_KEYBOARD_KEY_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    CHECK(capture.count == 4);
    CHECK(boxrig_wayland_terminal_component_set_input_config(component, &replacement) == BOXRIG_UI_OK);
    CHECK(capture.events[0].action == BOXRIG_KEY_PRESS);
    CHECK(capture.events[0].codepoint == 'a');
    CHECK(capture.events[0].shifted_codepoint == 'A');
    CHECK(capture.events[0].base_layout_codepoint == 'a');
    CHECK(capture.events[1].action == BOXRIG_KEY_RELEASE);
    CHECK(capture.events[2].codepoint == '1');
    CHECK(capture.events[3].action == BOXRIG_KEY_RELEASE);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 1 && output[0] == 'a');
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.keyboard_focus_count == 1);
    CHECK(boxrig_ui_view_focused(
        boxrig_wayland_terminal_component_view(component)));
    CHECK(component_diagnostic.input_queue_high_water >= 4);
    CHECK(!component_diagnostic.input_blocked);
    CHECK(boxrig_wayland_context_get_serial(
              context,
              seat_name,
              BOXRIG_WAYLAND_SERIAL_KEYBOARD_ENTER,
              &serial) == BOXRIG_UI_OK);
    CHECK(serial.seat_global_name == seat_name);
    CHECK(serial.kind == BOXRIG_WAYLAND_SERIAL_KEYBOARD_ENTER);
    CHECK(serial.value == 43);

    CHECK(boxrig_wayland_context_update_monotonic_time(context, 1000) ==
          BOXRIG_UI_OK);
    wl_keyboard_send_repeat_info(server.keyboard_resources[0], 25, 400);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        48,
        UINT32_MAX - 5,
        30,
        WL_KEYBOARD_KEY_STATE_PRESSED);
    pump(&server);
    CHECK(boxrig_wayland_context_dispatch(
              context,
              BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT,
              &dispatch_result) == BOXRIG_UI_OK);
    CHECK(dispatch_result.has_repeat_deadline);
    CHECK(dispatch_result.repeat_deadline_milliseconds == 1400);
    CHECK(boxrig_wayland_terminal_component_set_input_config(component, &replacement) == BOXRIG_UI_BUSY);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &context_diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(context_diagnostic.active_repeat_count == 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 1 && output[0] == 'a');
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    CHECK(boxrig_wayland_context_update_monotonic_time(context, 1400) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_wayland_context_dispatch(
              context,
              BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT,
              &dispatch_result) == BOXRIG_UI_OK);
    CHECK(dispatch_result.has_repeat_deadline);
    CHECK(dispatch_result.repeat_deadline_milliseconds == 1440);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 1 && output[0] == 'a');
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    CHECK(boxrig_wayland_context_update_monotonic_time(context, 2000) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_wayland_context_dispatch(context, 2, &dispatch_result) ==
          BOXRIG_UI_OK);
    CHECK(dispatch_result.processed_items == 2);
    CHECK(dispatch_result.additional_dispatch);
    CHECK(dispatch_result.repeat_deadline_milliseconds == 1520);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 2 && memcmp(output, "aa", 2) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    wl_keyboard_send_key(
        server.keyboard_resources[0],
        49,
        4,
        30,
        WL_KEYBOARD_KEY_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_wayland_context_dispatch(
              context,
              BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT,
              &dispatch_result) == BOXRIG_UI_OK);
    CHECK(!dispatch_result.has_repeat_deadline);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_wayland_context_update_monotonic_time(context, 1999) ==
          BOXRIG_UI_INVALID_ARGUMENT);

    send_oversized_keymap(&server, 0);
    pump(&server);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &context_diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(context_diagnostic.keymap_failure_count == 1);
    CHECK((context_diagnostic.input_capabilities &
           BOXRIG_WAYLAND_INPUT_CAPABILITY_KEYBOARD) == 0);

    wl_global_remove(server.seats[0]);
    server.seats[0] = NULL;
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &context_diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(context_diagnostic.seat_count == 0);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.keyboard_focus_count == 0);
    CHECK(!boxrig_ui_view_focused(
        boxrig_wayland_terminal_component_view(component)));
    CHECK(component_diagnostic.failure_reason == BOXRIG_WAYLAND_FAILURE_NONE);

    wl_array_release(&keys);
    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host_registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
}

typedef struct reparent_callback_state {
    boxrig_wayland_terminal_component *component;
    struct wl_surface *parent;
    size_t calls;
} reparent_callback_state;

static bool fail_reparent_sync;
void *__real_wl_proxy_create_wrapper(void *proxy);
void *__wrap_wl_proxy_create_wrapper(void *proxy)
{
    if (fail_reparent_sync) {
        fail_reparent_sync = false;
        return NULL;
    }
    return __real_wl_proxy_create_wrapper(proxy);
}

static void reparent_frame_requested(void *data)
{
    reparent_callback_state *state = data;
    if (state->component != NULL) {
        uint64_t revision;
        CHECK(boxrig_wayland_terminal_component_request_reparent(state->component,
            state->parent, &revision) == BOXRIG_UI_BUSY);
        state->calls++;
    }
}

static void test_reparent_input_generation_and_pending_focus(void)
{
    fake_server server;
    host_registry_state host;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_wayland_terminal_component_config config = boxrig_wayland_terminal_component_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_layout_action layout_action;
    boxrig_wayland_host_action action;
    boxrig_ui_component_frame_result frame;
    boxrig_wayland_component_diagnostic diagnostic;
    reparent_callback_state callback = {0};
    struct wl_array keys;
    const uint8_t *output;
    uint64_t revision, rejected_revision;
    bool applied;
    size_t consumed;
    create_server(&server, true, false, true);
    struct wl_registry *registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[?1004h", 8, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(server.client_display, server.client_queue, NULL, &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    struct wl_surface *parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    config.keyboard_focus_fallback = true;
    config.frame_request_callback = reparent_frame_requested;
    config.frame_request_context = &callback;
    CHECK(create_steady_component(context, parent, model, &config, &component) == BOXRIG_UI_OK);
    allocation.width = 80;
    allocation.height = 40;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_take_layout_action(component, &layout_action));
    CHECK(boxrig_terminal_resize(terminal, layout_action.value.grid_resize.rows,
        layout_action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
    CHECK(boxrig_wayland_terminal_component_complete_grid_resize(component,
        layout_action.revision, true) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_take_host_action(component, &action));
    wl_surface_commit(action.parent);
    CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component, action.revision, &applied) == BOXRIG_UI_OK && applied);
    pump(&server);
    struct wl_resource *child_resource = server.child_resource;
    struct wl_surface *destination = wl_compositor_create_surface(host.compositor);
    CHECK(destination != NULL);
    pump(&server);
    struct wl_resource *destination_resource = server.child_resource;
    CHECK(child_resource != destination_resource);
    send_keymap(&server, 0);
    wl_array_init(&keys);
    wl_keyboard_send_enter(server.keyboard_resources[0], 10, server.parent_resource, &keys);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 3 && memcmp(output, "\033[I", 3) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 3) == BOXRIG_STATUS_OK);
    wl_keyboard_send_key(server.keyboard_resources[0], 11, 1, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_request_reparent(component, destination, &revision) == BOXRIG_UI_BUSY);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1 && output[0] == 'a');
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    wl_keyboard_send_key(server.keyboard_resources[0], 12, 2, KEY_A, WL_KEYBOARD_KEY_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    wl_pointer_send_enter(server.pointer_resources[0], 21, child_resource,
        wl_fixed_from_int(2), wl_fixed_from_int(2));
    wl_pointer_send_button(server.pointer_resources[0], 22, 9, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_request_reparent(component, destination, &revision) == BOXRIG_UI_BUSY);
    wl_pointer_send_button(server.pointer_resources[0], 23, 10, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    boxrig_composition *composition = boxrig_wayland_terminal_component_composition(component);
    boxrig_composition_event preedit = {.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE,
        .text = "x", .text_length = 1, .caret_offset = 1, .caret_visible = true};
    CHECK(boxrig_composition_apply_event(composition, &preedit) == BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_wayland_terminal_component_request_reparent(component, destination, &revision) == BOXRIG_UI_BUSY);
    preedit = (boxrig_composition_event){.type = BOXRIG_COMPOSITION_CANCEL};
    CHECK(boxrig_composition_apply_event(composition, &preedit) == BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
    boxrig_ui_input_state input_state;
    CHECK(boxrig_wayland_terminal_component_get_input_state(component, &input_state) == BOXRIG_UI_OK);
    uint64_t input_generation = input_state.generation;
    callback.component = component;
    callback.parent = destination;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[>3u", 5, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_wayland_terminal_component_request_reparent(component, destination, &revision) == BOXRIG_UI_OK);
    CHECK(callback.calls > 0);
    CHECK(boxrig_wayland_terminal_component_request_reparent(component, parent, &rejected_revision) == BOXRIG_UI_BUSY);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 3 && memcmp(output, "\033[O", 3) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 3) == BOXRIG_STATUS_OK);
    /* Old focus and keys cannot enter the new input generation. */
    wl_keyboard_send_enter(server.keyboard_resources[0], 13, server.parent_resource, &keys);
    wl_keyboard_send_key(server.keyboard_resources[0], 14, 3, KEY_E, WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_keyboard_send_key(server.keyboard_resources[0], 15, 4, KEY_E, WL_KEYBOARD_KEY_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    /* Acceptance already detached the old parent, before the new commit. */
    wl_surface_destroy(parent);
    /* The destination may receive focus before the server acknowledges the
     * move. Its keys are suspended, then input resumes without a second enter. */
    wl_keyboard_send_enter(server.keyboard_resources[0], 16, destination_resource, &keys);
    zwp_text_input_v3_send_enter(server.text_input_resources[0], destination_resource);
    wl_keyboard_send_key(server.keyboard_resources[0], 17, 5, KEY_E, WL_KEYBOARD_KEY_STATE_PRESSED);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_take_host_action(component, &action));
    CHECK(action.parent == destination);
    wl_surface_commit(action.parent);
    fail_reparent_sync = true;
    CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component,
        action.revision, &applied) == BOXRIG_UI_OUT_OF_MEMORY && !applied);
    CHECK(!fail_reparent_sync);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.reparent_pending && diagnostic.effective_reparent_revision < revision);
    CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component, action.revision, &applied) == BOXRIG_UI_OK && applied);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(!diagnostic.reparent_pending && diagnostic.effective_reparent_revision == revision);
    CHECK(boxrig_wayland_terminal_component_get_input_state(component, &input_state) == BOXRIG_UI_OK);
    CHECK(input_state.generation > input_generation && diagnostic.keyboard_focus_count == 1);
    CHECK(diagnostic.text_input_focus_count == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 3 && memcmp(output, "\033[I", 3) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 3) == BOXRIG_STATUS_OK);
    /* A press discarded during the move must not produce a release report
     * after acknowledgement, even when Kitty release reporting is enabled. */
    CHECK(boxrig_wayland_terminal_component_request_reparent(component, destination, &rejected_revision) == BOXRIG_UI_BUSY);
    wl_keyboard_send_key(server.keyboard_resources[0], 18, 6, KEY_E, WL_KEYBOARD_KEY_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[<u", 4, &consumed) == BOXRIG_STATUS_OK);
    pump(&server);
    uint32_t destination_text_serial = server.text_input_commit_serials[0];
    CHECK(destination_text_serial != 0);
    zwp_text_input_v3_send_commit_string(server.text_input_resources[0], "stale");
    zwp_text_input_v3_send_done(server.text_input_resources[0], destination_text_serial - 1);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    zwp_text_input_v3_send_commit_string(server.text_input_resources[0], "fresh");
    zwp_text_input_v3_send_done(server.text_input_resources[0], destination_text_serial);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 5 && memcmp(output, "fresh", 5) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 5) == BOXRIG_STATUS_OK);
    wl_keyboard_send_key(server.keyboard_resources[0], 19, 7, KEY_E, WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_keyboard_send_key(server.keyboard_resources[0], 20, 8, KEY_E, WL_KEYBOARD_KEY_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    size_t resumed_length = boxrig_terminal_output_peek(terminal, &output);
    if (resumed_length != 1 || output[0] != 'e') {
        fprintf(stderr, "Resumed input length=%zu bytes=", resumed_length);
        for (size_t index = 0; index < resumed_length; index++) fprintf(stderr, "%02x ", output[index]);
        fputc('\n', stderr);
    }
    CHECK(resumed_length == 1 && output[0] == 'e');
    callback.component = NULL;
    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(destination);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    wl_array_release(&keys);
    destroy_server(&server);
    puts("PASS Wayland reparent rejects old input and resumes destination focus");
}

static void test_host_keyboard_focus_routes_to_fallback_component(void)
{
    fake_server server;
    host_registry_state host;
    struct wl_registry *host_registry;
    struct wl_surface *parent;
    struct wl_surface *decoration;
    struct wl_resource *decoration_resource;
    struct wl_array keys;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_wayland_terminal_component_config component_config =
        boxrig_wayland_terminal_component_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_wayland_component_diagnostic component_diagnostic;
    const uint8_t *output;
    size_t output_length;

    create_server(&server, true, false, true);
    host_registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              NULL,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    pump(&server);
    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    component_config.keyboard_focus_fallback = true;
    CHECK(create_steady_component(
              context,
              parent,
              model,
              &component_config,
              &component) == BOXRIG_UI_OK);
    pump(&server);
    decoration = wl_compositor_create_surface(host.compositor);
    CHECK(decoration != NULL);
    wl_proxy_set_queue((struct wl_proxy *)decoration, server.client_queue);
    pump(&server);
    decoration_resource = server.child_resource;
    CHECK(decoration_resource != NULL &&
          decoration_resource != server.parent_resource);
    send_keymap(&server, 0);
    wl_array_init(&keys);
    wl_keyboard_send_enter(
        server.keyboard_resources[0],
        91,
        decoration_resource,
        &keys);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        92,
        20,
        30,
        WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        93,
        21,
        30,
        WL_KEYBOARD_KEY_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 1 && output[0] == 'a');
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    zwp_text_input_v3_send_enter(
        server.text_input_resources[0],
        decoration_resource);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.keyboard_focus_count == 1);
    CHECK(boxrig_ui_view_focused(
        boxrig_wayland_terminal_component_view(component)));
    CHECK(component_diagnostic.text_input_focus_count == 1);

    wl_keyboard_send_leave(
        server.keyboard_resources[0],
        94,
        decoration_resource);
    zwp_text_input_v3_send_leave(
        server.text_input_resources[0],
        decoration_resource);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.keyboard_focus_count == 0);
    CHECK(!boxrig_ui_view_focused(
        boxrig_wayland_terminal_component_view(component)));
    CHECK(component_diagnostic.text_input_focus_count == 0);

    wl_array_release(&keys);
    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(decoration);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host_registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
}

static void test_pointer_scale_batch_and_click_wrap(void)
{
    fake_server server;
    host_registry_state host;
    struct wl_registry *host_registry;
    struct wl_surface *parent;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame_result;
    boxrig_ui_hit_test_result hit;
    boxrig_wayland_component_diagnostic diagnostic;
    boxrig_wayland_serial serial;
    mouse_route_capture capture = {0};
    uint32_t seat_name;
    const uint8_t *output;

    create_server(&server, true, true, true);
    host_registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              NULL,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    pump(&server);
    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    CHECK(create_steady_component(
              context,
              parent,
              model,
              NULL,
              &component) == BOXRIG_UI_OK);
    allocation.width = 80;
    allocation.height = 40;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_wayland_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_set_mouse_router(
              component,
              capture_mouse_route,
              &capture) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, false) == BOXRIG_UI_OK);
    pump(&server);
    send_keymap(&server, 0);
    wl_keyboard_send_modifiers(
        server.keyboard_resources[0],
        50,
        1u << 2,
        0,
        0,
        0);
    pump(&server);
    wp_fractional_scale_v1_send_preferred_scale(
        server.fractional_scale_resource,
        150);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    pump(&server);
    CHECK(boxrig_wayland_terminal_component_hit_test(
              component,
              4 * BOXRIG_UI_FIXED_ONE,
              6 * BOXRIG_UI_FIXED_ONE,
              &hit) == BOXRIG_UI_OK);

    wl_pointer_send_enter(
        server.pointer_resources[0],
        51,
        server.child_resource,
        wl_fixed_from_int(4),
        wl_fixed_from_int(6));
    wl_pointer_send_motion(
        server.pointer_resources[0],
        UINT32_MAX - 5,
        wl_fixed_from_int(4),
        wl_fixed_from_int(6));
    wl_pointer_send_button(
        server.pointer_resources[0],
        52,
        UINT32_MAX - 5,
        0x110,
        WL_POINTER_BUTTON_STATE_PRESSED);
    wl_keyboard_send_modifiers(
        server.keyboard_resources[0],
        51,
        1u << 0,
        0,
        0,
        0);
    wl_pointer_send_button(
        server.pointer_resources[0],
        53,
        2,
        0x110,
        WL_POINTER_BUTTON_STATE_RELEASED);
    wl_keyboard_send_modifiers(
        server.keyboard_resources[0],
        52,
        0,
        0,
        0,
        0);
    wl_pointer_send_button(
        server.pointer_resources[0],
        54,
        4,
        0x110,
        WL_POINTER_BUTTON_STATE_PRESSED);
    wl_pointer_send_button(
        server.pointer_resources[0],
        55,
        5,
        0x110,
        WL_POINTER_BUTTON_STATE_RELEASED);
    wl_pointer_send_axis(
        server.pointer_resources[0],
        6,
        WL_POINTER_AXIS_VERTICAL_SCROLL,
        wl_fixed_from_int(2));
    wl_pointer_send_axis_value120(
        server.pointer_resources[0],
        WL_POINTER_AXIS_VERTICAL_SCROLL,
        240);
    wl_pointer_send_axis(
        server.pointer_resources[0],
        6,
        WL_POINTER_AXIS_HORIZONTAL_SCROLL,
        wl_fixed_from_int(-1));
    wl_pointer_send_axis_discrete(
        server.pointer_resources[0],
        WL_POINTER_AXIS_HORIZONTAL_SCROLL,
        -1);
    pump(&server);
    dispatch_context(context);
    CHECK(capture.count == 5);
    CHECK(capture.events[0].action == BOXRIG_MOUSE_MOTION);
    CHECK(capture.events[0].x == 5 && capture.events[0].y == 7);
    CHECK(capture.events[0].row == hit.row);
    CHECK(capture.events[0].column == hit.visual_column);
    CHECK(capture.hits[0].x == 5 * BOXRIG_UI_FIXED_ONE);
    CHECK(capture.hits[0].y ==
          7 * BOXRIG_UI_FIXED_ONE + BOXRIG_UI_FIXED_ONE / 2);
    CHECK(capture.hits[0].row == hit.row &&
          capture.hits[0].visual_column == hit.visual_column);
    CHECK(capture.hits[0].layout_generation == hit.layout_generation &&
          capture.hits[0].annotation_generation ==
              hit.annotation_generation);
    CHECK(capture.events[1].button == BOXRIG_MOUSE_BUTTON_LEFT);
    CHECK(capture.events[1].click_count == 1);
    CHECK(capture.events[1].modifiers == BOXRIG_MODIFIER_CONTROL);
    CHECK(capture.events[2].action == BOXRIG_MOUSE_RELEASE);
    CHECK(capture.events[2].modifiers == BOXRIG_MODIFIER_SHIFT);
    CHECK(capture.events[3].click_count == 2);
    CHECK(capture.events[4].action == BOXRIG_MOUSE_RELEASE);

    wl_pointer_send_frame(server.pointer_resources[0]);
    pump(&server);
    dispatch_context(context);
    CHECK(capture.count == 11);
    CHECK(capture.events[5].button == BOXRIG_MOUSE_BUTTON_WHEEL_DOWN);
    CHECK(capture.events[6].action == BOXRIG_MOUSE_RELEASE);
    CHECK(capture.events[7].button == BOXRIG_MOUSE_BUTTON_WHEEL_DOWN);
    CHECK(capture.events[8].action == BOXRIG_MOUSE_RELEASE);
    CHECK(capture.events[9].button == BOXRIG_MOUSE_BUTTON_WHEEL_LEFT);
    CHECK(capture.events[10].action == BOXRIG_MOUSE_RELEASE);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.pointer_focus_count == 1);
    CHECK(diagnostic.pressed_button_count == 0);
    seat_name = wl_global_get_name(server.seats[0], server.server_client);
    CHECK(boxrig_wayland_context_get_serial(
              context,
              seat_name,
              BOXRIG_WAYLAND_SERIAL_POINTER_BUTTON,
              &serial) == BOXRIG_UI_OK);
    CHECK(serial.value == 55);

    wl_pointer_send_motion(server.pointer_resources[0], 7,
        wl_fixed_from_double(-0.125), wl_fixed_from_double(-1.25));
    pump(&server);
    dispatch_context(context);
    CHECK(capture.count == 12);
    CHECK(capture.events[11].x == -1 && capture.events[11].y == -2);
    CHECK(!capture.hits[11].inside_grid && capture.hits[11].x < 0);
    CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, true) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_set_mouse_router(component, NULL, NULL) == BOXRIG_UI_OK);
    const char pixel_mode[] = "\033[?1003;1016h";
    size_t consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)pixel_mode,
        sizeof(pixel_mode) - 1, &consumed) == BOXRIG_STATUS_OK);
    wl_pointer_send_motion(server.pointer_resources[0], 8,
        wl_fixed_from_double(-0.125), wl_fixed_from_double(-1.25));
    wl_pointer_send_motion(server.pointer_resources[0], 9,
        wl_fixed_from_double(4.25), wl_fixed_from_double(6.5));
    pump(&server);
    dispatch_context(context);
    const char pixels[] = "\033[<35;0;-1M\033[<35;6;9M";
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(pixels) - 1);
    CHECK(memcmp(output, pixels, sizeof(pixels) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(pixels) - 1) == BOXRIG_STATUS_OK);

    /* Neither an unfinished native frame nor its fractional wheel remainder
     * may become terminal input in the next enabled generation. */
    for (unsigned int boundary = 0; boundary < 2; boundary++) {
        if (boundary != 0)
            CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, false) == BOXRIG_UI_OK);
        wl_pointer_send_axis_value120(server.pointer_resources[0], WL_POINTER_AXIS_VERTICAL_SCROLL, 120);
        pump(&server);
        if (boundary == 0)
            CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, false) == BOXRIG_UI_OK);
        CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, true) == BOXRIG_UI_OK);
        wl_pointer_send_frame(server.pointer_resources[0]);
        pump(&server);
        dispatch_context(context);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    }
    wl_pointer_send_axis_value120(server.pointer_resources[0], WL_POINTER_AXIS_VERTICAL_SCROLL, 60);
    wl_pointer_send_frame(server.pointer_resources[0]);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, false) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, true) == BOXRIG_UI_OK);
    wl_pointer_send_axis_value120(server.pointer_resources[0], WL_POINTER_AXIS_VERTICAL_SCROLL, 60);
    wl_pointer_send_frame(server.pointer_resources[0]);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    wl_pointer_send_axis_value120(server.pointer_resources[0], WL_POINTER_AXIS_VERTICAL_SCROLL, 60);
    wl_pointer_send_frame(server.pointer_resources[0]);
    pump(&server);
    dispatch_context(context);
    const char wheel[] = "\033[<65;6;9M";
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(wheel) - 1);
    CHECK(memcmp(output, wheel, sizeof(wheel) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(wheel) - 1) == BOXRIG_STATUS_OK);

    wl_pointer_send_leave(
        server.pointer_resources[0],
        56,
        server.child_resource);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.pointer_focus_count == 0);

    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host_registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
}

typedef struct editable_owner {
    char text[10000];
    uint64_t revision;
    unsigned int calls, accepted;
    boxrig_ui_status disposition;
} editable_owner;

static bool fail_edit_preparation, fail_preedit_preparation;
boxrig_ui_status __real_boxrig_ui_editable_document_apply_edit(
    boxrig_ui_editable_document *, const boxrig_ui_editable_edit *);
/** Simulates exhausted document preparation storage before owner acceptance. */
boxrig_ui_status __wrap_boxrig_ui_editable_document_apply_edit(
    boxrig_ui_editable_document *document, const boxrig_ui_editable_edit *edit)
{
    if (fail_edit_preparation) {
        fail_edit_preparation = false;
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    return __real_boxrig_ui_editable_document_apply_edit(document, edit);
}

boxrig_composition_status __real_boxrig_composition_apply_event(
    boxrig_composition *, const boxrig_composition_event *);
/** Simulates failure to prepare the next preedit before accepting the batch. */
boxrig_composition_status __wrap_boxrig_composition_apply_event(
    boxrig_composition *composition, const boxrig_composition_event *event)
{
    if (fail_preedit_preparation && event->type == BOXRIG_COMPOSITION_PREEDIT_UPDATE) {
        fail_preedit_preparation = false;
        return BOXRIG_COMPOSITION_OUT_OF_MEMORY;
    }
    return __real_boxrig_composition_apply_event(composition, event);
}

static boxrig_ui_status apply_editable(const boxrig_ui_editable_replacement_request *request,
    const boxrig_ui_editable_document_snapshot *proposed, void *data)
{
    editable_owner *owner = data;
    owner->calls++;
    CHECK(request->document_identity == 7 && request->base_document_revision == owner->revision);
    if (owner->disposition != BOXRIG_UI_OK) return owner->disposition;
    size_t first = (size_t)(request->range.offset - 100);
    size_t length = strlen(owner->text);
    CHECK(first + request->range.length <= length);
    CHECK(length - request->range.length + request->replacement_length < sizeof(owner->text));
    memmove(owner->text + first + request->replacement_length,
        owner->text + first + request->range.length, length - first - request->range.length + 1);
    if (request->replacement_length != 0)
        memcpy(owner->text + first, request->replacement, request->replacement_length);
    CHECK(strlen(owner->text) == proposed->context_length);
    CHECK(memcmp(owner->text, proposed->context, proposed->context_length) == 0);
    owner->revision = proposed->document_revision;
    owner->accepted++;
    return BOXRIG_UI_OK;
}

static void publish_editable(boxrig_ui_editable_document *endpoint, editable_owner *owner,
    const char *text, size_t cursor, size_t anchor)
{
    if (text != owner->text) strcpy(owner->text, text);
    owner->revision++;
    boxrig_ui_editable_context_input input = {7, owner->revision, owner->text, strlen(owner->text),
        100, 100 + cursor, 100 + anchor};
    CHECK(boxrig_ui_editable_document_publish_context(endpoint, &input) == BOXRIG_UI_OK);
}

static void test_text_input_editable_document(void)
{
    fake_server server;
    host_registry_state host;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_allocation allocation = {.width = 160, .height = 80,
        .window_state = BOXRIG_UI_WINDOW_EMBEDDED};
    create_server(&server, true, false, true);
    struct wl_registry *registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(server.client_display, server.client_queue, NULL,
        &context) == BOXRIG_UI_OK);
    pump(&server); pump(&server); pump(&server);
    struct wl_surface *parent = wl_compositor_create_surface(host.compositor);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    CHECK(create_steady_component(context, parent, model, NULL, &component) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    pump(&server);
    send_keymap(&server, 0);
    struct wl_array keys;
    wl_array_init(&keys);
    wl_keyboard_send_enter(server.keyboard_resources[0], 10, server.child_resource, &keys);
    struct wl_resource *im = server.text_input_resources[0];
    zwp_text_input_v3_send_enter(im, server.child_resource);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(server.surrounding_count == 0 && !server.surrounding_available[0]);
    zwp_text_input_v3_send_preedit_string(im, "old", 3, 3);
    zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
    pump(&server);

    boxrig_ui_editable_document *endpoint = boxrig_ui_view_editable_document(
        boxrig_wayland_terminal_component_view(component));
    editable_owner owner = {.disposition = BOXRIG_UI_OK};
    CHECK(boxrig_ui_editable_document_set_apply_handler(endpoint, apply_editable, &owner) == BOXRIG_UI_OK);
    publish_editable(endpoint, &owner, "前かな後", 9, 9);
    dispatch_context(context); pump(&server);
    CHECK(server.surrounding_available[0]);
    CHECK(strcmp(server.surrounding_text[0], "前かな後") == 0);
    CHECK(server.surrounding_cursor[0] == 9 && server.surrounding_anchor[0] == 9);
    CHECK(server.change_cause[0] == ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_OTHER);
    boxrig_composition_view initial_preedit;
    CHECK(boxrig_composition_get_view(boxrig_wayland_terminal_component_composition(component),
        &initial_preedit) == BOXRIG_COMPOSITION_OK && !initial_preedit.active);

    uint32_t serial = server.text_input_commit_serials[0];
    zwp_text_input_v3_send_delete_surrounding_text(im, 3, 0);
    zwp_text_input_v3_send_commit_string(im, "語");
    zwp_text_input_v3_send_preedit_string(im, "あ", 3, 3);
    pump(&server); dispatch_context(context);
    CHECK(owner.calls == 0 && strcmp(owner.text, "前かな後") == 0);
    zwp_text_input_v3_send_done(im, serial);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(owner.accepted == 1 && strcmp(owner.text, "前か語後") == 0);
    CHECK(strcmp(server.surrounding_text[0], owner.text) == 0);
    CHECK(server.surrounding_cursor[0] == 9 && server.surrounding_anchor[0] == 9);
    CHECK(server.change_cause[0] == ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD);
    const uint8_t *output;
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    boxrig_composition_view preedit;
    CHECK(boxrig_composition_get_view(boxrig_wayland_terminal_component_composition(component),
        &preedit) == BOXRIG_COMPOSITION_OK);
    CHECK(preedit.active && preedit.text_length == 3 && memcmp(preedit.text, "あ", 3) == 0);

    owner.disposition = BOXRIG_UI_UNAVAILABLE;
    zwp_text_input_v3_send_delete_surrounding_text(im, 3, 0);
    zwp_text_input_v3_send_commit_string(im, "拒");
    zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(owner.calls == 2 && owner.accepted == 1 && strcmp(owner.text, "前か語後") == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    owner.disposition = BOXRIG_UI_BUSY;
    zwp_text_input_v3_send_commit_string(im, "追");
    zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
    pump(&server); dispatch_context(context);
    CHECK(owner.accepted == 1 && strcmp(owner.text, "前か語後") == 0);
    owner.disposition = BOXRIG_UI_OK;
    dispatch_context(context); pump(&server);
    CHECK(owner.accepted == 2 && strcmp(owner.text, "前か語追後") == 0);
    dispatch_context(context); pump(&server);
    CHECK(owner.accepted == 2 && boxrig_terminal_output_peek(terminal, &output) == 0);

    publish_editable(endpoint, &owner, "", 0, 0);
    dispatch_context(context); pump(&server);
    CHECK(server.surrounding_available[0] && server.surrounding_text[0][0] == '\0');
    zwp_text_input_v3_send_commit_string(im, "空");
    zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(owner.accepted == 3 && strcmp(owner.text, "空") == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    static const struct {
        const char *text;
        size_t cursor, anchor;
        uint32_t before, after;
        const char *commit, *preedit, *expected;
        int32_t cursor_after, anchor_after;
    } edits[] = {
        {"Ae\xcc\x81Z", 4, 4, 2, 0, NULL, NULL, "AeZ", 2, 2},
        {"前😀\n後", 7, 7, 4, 1, "語", NULL, "前語後", 6, 6},
        {"前abc後", 3, 6, 3, 3, "語", NULL, "語", 3, 3},
        {"前abc後", 6, 3, 3, 3, NULL, NULL, "abc", 3, 0},
        {"前選択後", 9, 3, 0, 0, NULL, "候", "前後", 3, 3},
    };
    for (size_t index = 0; index < sizeof(edits) / sizeof(edits[0]); index++) {
        publish_editable(endpoint, &owner, edits[index].text, edits[index].cursor, edits[index].anchor);
        dispatch_context(context); pump(&server);
        unsigned int accepted = owner.accepted;
        zwp_text_input_v3_send_delete_surrounding_text(im, edits[index].before, edits[index].after);
        if (edits[index].commit != NULL) zwp_text_input_v3_send_commit_string(im, edits[index].commit);
        if (edits[index].preedit != NULL) zwp_text_input_v3_send_preedit_string(im, edits[index].preedit, 3, 3);
        zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
        pump(&server); dispatch_context(context); pump(&server);
        CHECK(owner.accepted == accepted + 1 && strcmp(owner.text, edits[index].expected) == 0);
        CHECK(strcmp(server.surrounding_text[0], owner.text) == 0);
        CHECK(server.surrounding_cursor[0] == edits[index].cursor_after);
        CHECK(server.surrounding_anchor[0] == edits[index].anchor_after);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    }

    /* Consecutive done events need not have distinct commit serials. */
    publish_editable(endpoint, &owner, "ABC", 1, 1);
    dispatch_context(context); pump(&server);
    serial = server.text_input_commit_serials[0];
    unsigned int accepted = owner.accepted;
    zwp_text_input_v3_send_commit_string(im, "X");
    zwp_text_input_v3_send_done(im, serial);
    zwp_text_input_v3_send_commit_string(im, "Y");
    zwp_text_input_v3_send_done(im, serial);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(owner.accepted == accepted + 2 && strcmp(owner.text, "AXYBC") == 0);

    /* Both a queued edit and a later callback from the prior document expire. */
    serial = server.text_input_commit_serials[0];
    zwp_text_input_v3_send_commit_string(im, "old");
    zwp_text_input_v3_send_done(im, serial);
    pump(&server);
    publish_editable(endpoint, &owner, "NEW", 1, 1);
    dispatch_context(context); pump(&server);
    CHECK(strcmp(owner.text, "NEW") == 0);
    zwp_text_input_v3_send_commit_string(im, "late");
    zwp_text_input_v3_send_done(im, serial);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(strcmp(owner.text, "NEW") == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    /* A valid old serial caused only by geometry still applies the edit,
     * and its new surrounding state waits for the current serial. */
    serial = server.text_input_commit_serials[0];
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[2;2H", 6, &consumed) == BOXRIG_STATUS_OK);
    dispatch_context(context); pump(&server);
    CHECK(server.text_input_commit_serials[0] != serial);
    uint32_t geometry_serial = server.text_input_commit_serials[0];
    zwp_text_input_v3_send_commit_string(im, "G");
    zwp_text_input_v3_send_done(im, serial);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(strcmp(owner.text, "NGEW") == 0);
    CHECK(server.text_input_commit_serials[0] == geometry_serial);
    CHECK(strcmp(server.surrounding_text[0], "NEW") == 0);
    zwp_text_input_v3_send_done(im, geometry_serial);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(strcmp(server.surrounding_text[0], "NGEW") == 0);

    /* A malformed batch invalidates following events from that IME state. */
    accepted = owner.accepted;
    serial = server.text_input_commit_serials[0];
    zwp_text_input_v3_send_delete_surrounding_text(im, 1, 0);
    zwp_text_input_v3_send_preedit_string(im, "\xc0", 0, 0);
    zwp_text_input_v3_send_commit_string(im, "bad");
    zwp_text_input_v3_send_done(im, serial);
    zwp_text_input_v3_send_commit_string(im, "late");
    zwp_text_input_v3_send_done(im, serial);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(owner.accepted == accepted && strcmp(owner.text, "NGEW") == 0);

    for (int fault = 0; fault < 2; fault++) {
        unsigned int calls = owner.calls;
        zwp_text_input_v3_send_delete_surrounding_text(im, 1, 0);
        zwp_text_input_v3_send_commit_string(im, "allocated");
        zwp_text_input_v3_send_preedit_string(im, "候", 3, 3);
        zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
        pump(&server);
        if (fault == 0) fail_edit_preparation = true;
        else fail_preedit_preparation = true;
        dispatch_context(context); pump(&server);
        CHECK(!fail_edit_preparation && !fail_preedit_preparation);
        CHECK(owner.calls == calls && strcmp(owner.text, "NGEW") == 0);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
        CHECK(boxrig_composition_get_view(boxrig_wayland_terminal_component_composition(component),
            &preedit) == BOXRIG_COMPOSITION_OK && !preedit.active);
    }

    /* A cursor-only publication invalidates a BUSY batch without changing
     * the body revision. Native focus loss also discards queued edits. */
    owner.disposition = BOXRIG_UI_BUSY;
    zwp_text_input_v3_send_commit_string(im, "busy");
    zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
    pump(&server); dispatch_context(context);
    boxrig_ui_editable_context_input moved = {7, owner.revision, owner.text, strlen(owner.text),
        100, 101, 101};
    CHECK(boxrig_ui_editable_document_publish_context(endpoint, &moved) == BOXRIG_UI_OK);
    owner.disposition = BOXRIG_UI_OK;
    dispatch_context(context); pump(&server);
    CHECK(strcmp(owner.text, "NGEW") == 0 && server.surrounding_cursor[0] == 1);
    accepted = owner.accepted;
    zwp_text_input_v3_send_commit_string(im, "unfocused");
    zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
    pump(&server);
    wl_keyboard_send_leave(server.keyboard_resources[0], 11, server.child_resource);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(owner.accepted == accepted && strcmp(owner.text, "NGEW") == 0);
    zwp_text_input_v3_send_leave(im, server.child_resource);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(boxrig_composition_get_view(boxrig_wayland_terminal_component_composition(component),
        &preedit) == BOXRIG_COMPOSITION_OK && !preedit.active);
    wl_keyboard_send_enter(server.keyboard_resources[0], 12, server.child_resource, &keys);
    zwp_text_input_v3_send_enter(im, server.child_resource);
    pump(&server); dispatch_context(context); pump(&server);

    for (size_t index = 0; index < 2000; index++) memcpy(owner.text + index * 3, "あ", 3);
    owner.text[6000] = '\0';
    publish_editable(endpoint, &owner, owner.text, 3000, 3003);
    dispatch_context(context); pump(&server);
    CHECK(server.surrounding_available[0]);
    size_t bounded_length = strlen(server.surrounding_text[0]);
    CHECK(bounded_length <= 4000 && bounded_length % 3 == 0);
    CHECK(server.surrounding_cursor[0] % 3 == 0 && server.surrounding_anchor[0] % 3 == 0);
    CHECK(server.surrounding_anchor[0] - server.surrounding_cursor[0] == 3);
    for (size_t index = 0; index < bounded_length; index += 3)
        CHECK(memcmp(server.surrounding_text[0] + index, "あ", 3) == 0);
    accepted = owner.accepted;
    const uint32_t invalid_lengths[] = {1, UINT32_MAX};
    for (size_t index = 0; index < 2; index++) {
        zwp_text_input_v3_send_delete_surrounding_text(im, invalid_lengths[index], 0);
        zwp_text_input_v3_send_commit_string(im, "invalid");
        zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
        pump(&server); dispatch_context(context); pump(&server);
        CHECK(owner.accepted == accepted && strlen(owner.text) == 6000);
    }
    publish_editable(endpoint, &owner, owner.text, 0, 4002);
    dispatch_context(context); pump(&server);
    CHECK(!server.surrounding_available[0]);

    publish_editable(endpoint, &owner, "readonly", 8, 8);
    CHECK(boxrig_ui_editable_document_set_apply_handler(endpoint, NULL, NULL) == BOXRIG_UI_OK);
    dispatch_context(context); pump(&server);
    CHECK(server.surrounding_available[0]);
    unsigned int calls = owner.calls;
    zwp_text_input_v3_send_delete_surrounding_text(im, 1, 0);
    zwp_text_input_v3_send_commit_string(im, "!");
    zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(owner.calls == calls && strcmp(owner.text, "readonly") == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_ui_editable_document_set_apply_handler(endpoint, apply_editable, &owner) == BOXRIG_UI_OK);

    /* Protocol strings cannot carry an embedded NUL. Do not advertise a
     * truncated success or turn its trailing bytes into terminal input. */
    const char binary_text[] = {'A', '\0', 'B'};
    boxrig_ui_editable_context_input binary = {7, ++owner.revision, binary_text, 3, 100, 103, 103};
    CHECK(boxrig_ui_editable_document_publish_context(endpoint, &binary) == BOXRIG_UI_OK);
    dispatch_context(context); pump(&server);
    CHECK(!server.surrounding_available[0]);
    CHECK(boxrig_ui_editable_document_clear(endpoint) == BOXRIG_UI_OK);
    dispatch_context(context); pump(&server);
    CHECK(!server.surrounding_available[0]);
    zwp_text_input_v3_send_commit_string(im, "端");
    zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
    pump(&server); dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 3 && memcmp(output, "端", 3) == 0);

    CHECK(boxrig_terminal_output_consume(terminal, 3) == BOXRIG_STATUS_OK);
    for (size_t index = 0; index < sizeof(boxrig_editable_cases) / sizeof(boxrig_editable_cases[0]); index++) {
        CHECK(boxrig_ui_editable_document_clear(endpoint) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_editable_document_set_apply_handler(endpoint,
            boxrig_editable_cases[index].synchronous_edit_available ? apply_editable : NULL,
            boxrig_editable_cases[index].synchronous_edit_available ? &owner : NULL) == BOXRIG_UI_OK);
        strcpy(owner.text, boxrig_editable_fixture.text);
        if (boxrig_editable_cases[index].context_available)
            publish_editable(endpoint, &owner, boxrig_editable_fixture.text,
                boxrig_editable_fixture.cursor, boxrig_editable_fixture.anchor);
        dispatch_context(context); pump(&server);
        boxrig_ui_editable_capabilities caps = boxrig_ui_editable_document_capabilities(endpoint);
        CHECK(caps.context_available == boxrig_editable_cases[index].context_available);
        CHECK(caps.synchronous_edit_available == boxrig_editable_cases[index].synchronous_edit_available);
        CHECK(!caps.reconversion_available && !caps.busy);
        CHECK(server.surrounding_available[0] == caps.context_available);
        if (caps.context_available) {
            CHECK(strcmp(server.surrounding_text[0], boxrig_editable_fixture.text) == 0);
            CHECK(server.surrounding_cursor[0] == 9 && server.surrounding_anchor[0] == 9);
            zwp_text_input_v3_send_delete_surrounding_text(im, 3, 0);
        }
        zwp_text_input_v3_send_commit_string(im, boxrig_editable_fixture.commit);
        zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
        pump(&server); dispatch_context(context); pump(&server);
        CHECK(strcmp(owner.text, boxrig_editable_cases[index].after_synchronous_edit) == 0);
        size_t length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(length == boxrig_editable_cases[index].terminal_bytes);
        if (length != 0) {
            CHECK(memcmp(output, boxrig_editable_fixture.commit, length) == 0);
            CHECK(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
        }
        printf("PASS Wayland shared editable state=%s\n", boxrig_editable_cases[index].name);
    }

    /* Queue overflow and quiescence cannot partially accept document input. */
    publish_editable(endpoint, &owner, "full", 4, 4);
    dispatch_context(context); pump(&server);
    accepted = owner.accepted;
    for (size_t index = 0; index <= BOXRIG_WAYLAND_INPUT_EVENTS_DEFAULT; index++) {
        zwp_text_input_v3_send_commit_string(im, "X");
        zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
        pump(&server);
    }
    pump(&server); dispatch_context(context);
    boxrig_wayland_component_diagnostic diagnostic;
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.failure_reason == BOXRIG_WAYLAND_FAILURE_INPUT_QUEUE_OVERFLOW);
    CHECK(owner.accepted == accepted && strcmp(owner.text, "full") == 0);
    CHECK(boxrig_wayland_terminal_component_quiesce(component) == BOXRIG_UI_OK);
    zwp_text_input_v3_send_commit_string(im, "closed");
    zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
    pump(&server); dispatch_context(context);
    CHECK(owner.accepted == accepted && boxrig_terminal_output_peek(terminal, &output) == 0);

    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
    puts("PASS native Wayland surrounding context and atomic owner edits");
}

static void test_text_input_batch_and_compose_fallback(void)
{
    fake_server server;
    host_registry_state host;
    struct wl_registry *host_registry;
    struct wl_surface *parent;
    struct wl_array keys;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_composition_view composition_view;
    boxrig_wayland_context_diagnostic context_diagnostic;
    boxrig_wayland_component_diagnostic component_diagnostic;
    const uint8_t *output;
    size_t output_length;
    uint32_t serial;

    create_server(&server, true, false, true);
    host_registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              NULL,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    pump(&server);
    CHECK(server.text_input_create_count == 1);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &context_diagnostic) ==
          BOXRIG_UI_OK);
    CHECK((context_diagnostic.input_capabilities &
           BOXRIG_WAYLAND_INPUT_CAPABILITY_TEXT_INPUT_V3) != 0);
    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    CHECK(create_steady_component(
              context,
              parent,
              model,
              NULL,
              &component) == BOXRIG_UI_OK);
    allocation.width = 80;
    allocation.height = 40;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_wayland_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    pump(&server);
    send_keymap(&server, 0);
    wl_array_init(&keys);
    wl_keyboard_send_enter(
        server.keyboard_resources[0],
        61,
        server.child_resource,
        &keys);
    zwp_text_input_v3_send_enter(
        server.text_input_resources[0],
        server.child_resource);
    pump(&server);
    dispatch_context(context);
    pump(&server);
    CHECK(server.text_input_enable_count == 1);
    CHECK(server.text_input_content_type_count == 1);
    CHECK(server.text_input_cursor_rectangle_count >= 1);
    CHECK(server.text_input_cursor_width > 0);
    CHECK(server.text_input_cursor_height > 0);
    CHECK(server.text_input_commit_count == 1);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &context_diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(context_diagnostic.active_text_input_count == 1);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.text_input_focus_count == 1);

    serial = server.text_input_commit_serials[0];
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        62,
        10,
        30,
        WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        63,
        11,
        30,
        WL_KEYBOARD_KEY_STATE_RELEASED);
    /* A key forwarded by the compositor was not consumed by its input method. */
    pump(&server);
    dispatch_context(context);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 1 && output[0] == 'a');
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    /* Input consumed by the IME arrives separately through text-input-v3. */
    zwp_text_input_v3_send_preedit_string(
        server.text_input_resources[0],
        boxrig_composition_fixture.utf8,
        (int32_t)(boxrig_composition_fixture.selection_offset +
                  boxrig_composition_fixture.selection_length),
        (int32_t)boxrig_composition_fixture.caret_offset);
    zwp_text_input_v3_send_done(server.text_input_resources[0], serial);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_composition_get_view(
              boxrig_wayland_terminal_component_composition(component),
              &composition_view) == BOXRIG_COMPOSITION_OK);
    CHECK(composition_view.active);
    CHECK(composition_view.text_length ==
          boxrig_composition_fixture.utf8_length);
    CHECK(memcmp(
              composition_view.text,
              boxrig_composition_fixture.utf8,
              boxrig_composition_fixture.utf8_length) == 0);
    CHECK(composition_view.selection_offset ==
          boxrig_composition_fixture.selection_offset);
    CHECK(composition_view.selection_length ==
          boxrig_composition_fixture.selection_length);
    CHECK(composition_view.caret_offset ==
          boxrig_composition_fixture.caret_offset);
    CHECK(composition_view.caret_visible);
    /* Version 1 exposes one preedit span and no clause feedback. */
    CHECK(composition_view.segment_count == 1);
    CHECK(composition_view.segments[0].text_offset == 0);
    CHECK(composition_view.segments[0].text_length ==
          boxrig_composition_fixture.utf8_length);
    CHECK(composition_view.segments[0].feedback ==
          BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.ime_active);
    CHECK(!component_diagnostic.ime_commit_pending);

    zwp_text_input_v3_send_commit_string(
        server.text_input_resources[0],
        boxrig_composition_fixture.commit_utf8);
    zwp_text_input_v3_send_delete_surrounding_text(
        server.text_input_resources[0],
        1,
        1);
    zwp_text_input_v3_send_preedit_string(
        server.text_input_resources[0],
        "い",
        0,
        3);
    zwp_text_input_v3_send_done(server.text_input_resources[0], serial);
    pump(&server);
    dispatch_context(context);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == boxrig_composition_fixture.commit_utf8_length);
    CHECK(memcmp(
              output,
              boxrig_composition_fixture.commit_utf8,
              boxrig_composition_fixture.commit_utf8_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_composition_get_view(
              boxrig_wayland_terminal_component_composition(component),
              &composition_view) == BOXRIG_COMPOSITION_OK);
    CHECK(composition_view.active);
    CHECK(composition_view.text_length == 3);
    CHECK(memcmp(composition_view.text, "い", 3) == 0);
    CHECK(composition_view.selection_offset == 0);
    CHECK(composition_view.selection_length == 3);

    zwp_text_input_v3_send_preedit_string(
        server.text_input_resources[0],
        "古",
        0,
        0);
    zwp_text_input_v3_send_done(
        server.text_input_resources[0],
        serial + 99);
    zwp_text_input_v3_send_preedit_string(
        server.text_input_resources[0],
        "あ",
        1,
        1);
    zwp_text_input_v3_send_done(server.text_input_resources[0], serial);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_composition_get_view(
              boxrig_wayland_terminal_component_composition(component),
              &composition_view) == BOXRIG_COMPOSITION_OK);
    CHECK(composition_view.text_length == 3);
    CHECK(memcmp(composition_view.text, "い", 3) == 0);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.input_error_count >= 2);

    /* GNOME can hide preedit while retaining the previous anchor. Without
     * any text there is no caret to place; the old composition must end. */
    const struct {
        const char *text;
        int32_t cursor;
        int32_t anchor;
        const char *cleared;
    } preedits[] = {
        {"ls", 2, 2, NULL},
        {"ls", 2, 2, ""},
        {"ｌｓ", 6, 2, NULL},
        {"あ", 3, 1, ""},
    };
    for (size_t index = 0;
         index < sizeof(preedits) / sizeof(preedits[0]);
         index++) {
        zwp_text_input_v3_send_preedit_string(
            server.text_input_resources[0], preedits[index].text,
            preedits[index].cursor, preedits[index].anchor);
        zwp_text_input_v3_send_done(server.text_input_resources[0], serial);
        pump(&server);
        dispatch_context(context);
        CHECK(boxrig_composition_get_view(
                  boxrig_wayland_terminal_component_composition(component),
                  &composition_view) == BOXRIG_COMPOSITION_OK);
        CHECK(composition_view.active);
        CHECK(composition_view.text_length == strlen(preedits[index].text));
        CHECK(memcmp(composition_view.text, preedits[index].text,
                  composition_view.text_length) == 0);
        CHECK(composition_view.caret_offset == (size_t)preedits[index].cursor);
        CHECK(composition_view.selection_length == 0);

        zwp_text_input_v3_send_preedit_string(
            server.text_input_resources[0], preedits[index].cleared,
            0, preedits[index].anchor);
        zwp_text_input_v3_send_done(server.text_input_resources[0], serial);
        pump(&server);
        dispatch_context(context);
        CHECK(boxrig_composition_get_view(
                  boxrig_wayland_terminal_component_composition(component),
                  &composition_view) == BOXRIG_COMPOSITION_OK);
        CHECK(!composition_view.active);
        CHECK(composition_view.text_length == 0);
        CHECK(composition_view.cluster_count == 0);
        CHECK(!composition_view.caret_visible);
        CHECK(!composition_view.candidate.visible);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

        wl_keyboard_send_key(server.keyboard_resources[0], 80, 30,
            30, WL_KEYBOARD_KEY_STATE_PRESSED);
        wl_keyboard_send_key(server.keyboard_resources[0], 81, 31,
            30, WL_KEYBOARD_KEY_STATE_RELEASED);
        wl_keyboard_send_key(server.keyboard_resources[0], 82, 32,
            28, WL_KEYBOARD_KEY_STATE_PRESSED);
        wl_keyboard_send_key(server.keyboard_resources[0], 83, 33,
            28, WL_KEYBOARD_KEY_STATE_RELEASED);
        pump(&server);
        dispatch_context(context);
        output_length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(output_length == 2 && memcmp(output, "a\r", 2) == 0);
        CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_composition_get_view(
                  boxrig_wayland_terminal_component_composition(component),
                  &composition_view) == BOXRIG_COMPOSITION_OK);
        CHECK(!composition_view.active);
    }
    puts("PASS Wayland empty preedit clears stale caret state before direct typing");

    /* A malformed selection endpoint must not hide valid preedit text or
     * discard committed text from the same completed native batch. */
    zwp_text_input_v3_send_preedit_string(
        server.text_input_resources[0], "ｌｓ", 6, 2);
    zwp_text_input_v3_send_done(server.text_input_resources[0], serial);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_composition_get_view(
              boxrig_wayland_terminal_component_composition(component),
              &composition_view) == BOXRIG_COMPOSITION_OK);
    CHECK(composition_view.active && composition_view.text_length == 6);
    CHECK(composition_view.caret_offset == 6);
    zwp_text_input_v3_send_commit_string(server.text_input_resources[0], "ｌｓ");
    zwp_text_input_v3_send_preedit_string(server.text_input_resources[0], NULL, 0, 2);
    zwp_text_input_v3_send_done(server.text_input_resources[0], serial);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_composition_get_view(
              boxrig_wayland_terminal_component_composition(component),
              &composition_view) == BOXRIG_COMPOSITION_OK);
    CHECK(!composition_view.active);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 6 && memcmp(output, "ｌｓ", 6) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    puts("PASS Wayland preedit with an invalid selection endpoint preserves text and commit");

    zwp_text_input_v3_send_leave(
        server.text_input_resources[0],
        server.child_resource);
    pump(&server);
    dispatch_context(context);
    pump(&server);
    CHECK(server.text_input_disable_count == 1);
    CHECK(server.text_input_commit_count == 2);
    CHECK(boxrig_composition_get_view(
              boxrig_wayland_terminal_component_composition(component),
              &composition_view) == BOXRIG_COMPOSITION_OK);
    CHECK(!composition_view.active);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.text_input_focus_count == 0);
    CHECK(!component_diagnostic.ime_active);

    wl_global_remove(server.text_input_manager);
    server.text_input_manager = NULL;
    pump(&server);
    pump(&server);
    CHECK(server.text_input_destroy_count == 1);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &context_diagnostic) ==
          BOXRIG_UI_OK);
    CHECK((context_diagnostic.input_capabilities &
           BOXRIG_WAYLAND_INPUT_CAPABILITY_TEXT_INPUT_V3) == 0);

    wl_keyboard_send_key(
        server.keyboard_resources[0],
        64,
        20,
        58,
        WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        65,
        21,
        58,
        WL_KEYBOARD_KEY_STATE_RELEASED);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        66,
        22,
        31,
        WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        67,
        23,
        31,
        WL_KEYBOARD_KEY_STATE_RELEASED);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        68,
        24,
        18,
        WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        69,
        25,
        18,
        WL_KEYBOARD_KEY_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 2 && memcmp(output, "é", 2) == 0);

    wl_array_release(&keys);
    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host_registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
}

static void test_text_input_candidate_stays_at_preedit_caret(void)
{
    fake_server server;
    host_registry_state host;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame;
    boxrig_font_effective_metrics metrics;
    size_t consumed;
    const uint8_t *output;

    create_server(&server, true, false, true);
    struct wl_registry *registry = create_host_registry(&server, &host);
    config.rows = 4;
    config.columns = 20;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(server.client_display, server.client_queue,
        NULL, &context) == BOXRIG_UI_OK);
    pump(&server); pump(&server); pump(&server);
    struct wl_surface *parent = wl_compositor_create_surface(host.compositor);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    CHECK(create_steady_component(context, parent, model, NULL, &component) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_effective_metrics(component, &metrics) ==
        BOXRIG_FONT_CATALOG_OK);
    boxrig_ui_component_allocation allocation = {
        .width = config.columns * metrics.cell_width,
        .height = config.rows * metrics.cell_height,
        .window_state = BOXRIG_UI_WINDOW_EMBEDDED,
    };
    CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[2;3H", 6, &consumed) == BOXRIG_STATUS_OK);
    pump(&server);
    send_keymap(&server, 0);
    struct wl_array keys;
    wl_array_init(&keys);
    wl_keyboard_send_enter(server.keyboard_resources[0], 10, server.child_resource, &keys);
    struct wl_resource *im = server.text_input_resources[0];
    zwp_text_input_v3_send_enter(im, server.child_resource);
    pump(&server); dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.text_input_cursor_x == (int32_t)(2 * metrics.cell_width));
    CHECK(server.text_input_cursor_y == (int32_t)metrics.cell_height);

    const struct {
        const char *text;
        int32_t cursor;
        int32_t anchor;
        size_t caret_columns;
    } preedits[] = {
        {"ｌｓ", 6, 2, 4},
        {"ls", 2, 2, 2},
        {"ls", 0, 0, 0},
        {"ls", 1, 1, 1},
        {"ls", -1, -1, 0},
    };
    for (size_t index = 0; index < sizeof(preedits) / sizeof(preedits[0]); index++) {
        size_t rectangles = server.text_input_cursor_rectangle_count;
        zwp_text_input_v3_send_preedit_string(im, preedits[index].text,
            preedits[index].cursor, preedits[index].anchor);
        zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
        pump(&server); dispatch_context(context); pump(&server);
        CHECK(server.text_input_cursor_rectangle_count == rectangles);
        CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        pump(&server);
        CHECK(server.text_input_cursor_x ==
            (int32_t)((2 + preedits[index].caret_columns) * metrics.cell_width));
        CHECK(server.text_input_cursor_y == (int32_t)metrics.cell_height);
        rectangles = server.text_input_cursor_rectangle_count;
        size_t commits = server.text_input_commit_count;

        /* A compositor can repeat its current preedit in response to the
         * cursor update. No input or layout change justifies moving the
         * candidate, including between dispatch and the next frame. */
        for (size_t repeat = 0; repeat < 32; repeat++) {
            zwp_text_input_v3_send_preedit_string(im, preedits[index].text,
                preedits[index].cursor, preedits[index].anchor);
            zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
            pump(&server); dispatch_context(context); pump(&server);
            CHECK(server.text_input_cursor_rectangle_count == rectangles);
            CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
            pump(&server);
            CHECK(server.text_input_cursor_rectangle_count == rectangles);
            CHECK(server.text_input_commit_count == commits);
        }
    }
    zwp_text_input_v3_send_preedit_string(im, NULL, 0, 2);
    zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
    pump(&server); dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.text_input_cursor_x == (int32_t)(2 * metrics.cell_width));
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[3;4H", 6, &consumed) == BOXRIG_STATUS_OK);
    dispatch_context(context); pump(&server);
    CHECK(server.text_input_cursor_x == (int32_t)(3 * metrics.cell_width));
    CHECK(server.text_input_cursor_y == (int32_t)(2 * metrics.cell_height));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    wl_array_release(&keys);
    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
    puts("PASS Wayland repeated preedit keeps the candidate at the displayed caret");
}

static void test_text_input_v2_clause_hints(void)
{
    fake_server server;
    host_registry_state host;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_wayland_context_diagnostic diagnostic;
    boxrig_composition_view view;
    const uint8_t *output;
    const char *text = "記者の汽車が記者で記者した";
    size_t length = strlen(text);

    create_server(&server, true, false, true);
    wl_global_destroy(server.text_input_manager);
    server.text_input_manager = wl_global_create(server.server_display,
        &zwp_text_input_manager_v3_interface, 2, &server, bind_text_input_manager);
    CHECK(server.text_input_manager != NULL);
    struct wl_registry *registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(server.client_display, server.client_queue,
        NULL, &context) == BOXRIG_UI_OK);
    pump(&server); pump(&server); pump(&server);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.text_input_version == 2);
    struct wl_surface *parent = wl_compositor_create_surface(host.compositor);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    CHECK(create_steady_component(context, parent, model, NULL, &component) == BOXRIG_UI_OK);
    pump(&server);
    struct wl_resource *im = server.text_input_resources[0];
    send_keymap(&server, 0);
    struct wl_array keys;
    wl_array_init(&keys);
    wl_keyboard_send_enter(server.keyboard_resources[0], 10, server.child_resource, &keys);
    zwp_text_input_v3_send_enter(im, server.child_resource);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK((server.text_input_content_hints & ZWP_TEXT_INPUT_V3_CONTENT_HINT_PREEDIT_SHOWN) != 0);
    /* Language notifications do not inject text. No submit action is advertised. */
    zwp_text_input_v3_send_language(im, "ja");
    zwp_text_input_v3_send_action(im, ZWP_TEXT_INPUT_V3_ACTION_NONE, 1);

    const uint32_t clauses[][2] = {{0, 9}, {9, 18}, {18, 27}, {27, 39},
        {18, 27}, {9, 18}, {9, 15}, {9, 21}, {0, 9}};
    for (size_t clause = 0; clause < sizeof(clauses) / sizeof(clauses[0]); clause++) {
        uint32_t start = clauses[clause][0], end = clauses[clause][1];
        /* Hints can precede the text and overlap, including duplicates. */
        zwp_text_input_v3_send_preedit_hint(im, 0, (uint32_t)length,
            ZWP_TEXT_INPUT_V3_PREEDIT_HINT_WHOLE);
        zwp_text_input_v3_send_preedit_hint(im, start, end,
            ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SELECTION);
        zwp_text_input_v3_send_preedit_hint(im, start, end,
            ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SELECTION);
        zwp_text_input_v3_send_preedit_string(im, text, (int32_t)start, (int32_t)start);
        zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
        pump(&server); dispatch_context(context); pump(&server);
        CHECK(boxrig_composition_get_view(
            boxrig_wayland_terminal_component_composition(component), &view) == BOXRIG_COMPOSITION_OK);
        CHECK(view.active && view.text_length == length && memcmp(view.text, text, length) == 0);
        CHECK(view.caret_visible && view.caret_offset == start);
        CHECK(view.selection_offset == start && view.selection_length == end - start);
        for (size_t index = 0; index < view.cluster_count; index++) {
            boxrig_composition_cluster_view cluster;
            CHECK(boxrig_composition_get_cluster(
                boxrig_wayland_terminal_component_composition(component), index, &cluster) == BOXRIG_COMPOSITION_OK);
            bool selected = cluster.text_offset >= start && cluster.text_offset < end;
            CHECK(cluster.selected == selected);
            CHECK(((cluster.feedback & BOXRIG_COMPOSITION_FEEDBACK_TARGET) != 0) == selected);
            CHECK((cluster.feedback & BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE) != 0);
        }
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    }

    /* UTF-8 byte ranges include ASCII, Japanese, and a four-byte scalar. */
    text = "A記者の🙂汽車がZ";
    zwp_text_input_v3_send_preedit_string(im, text, -1, -1);
    zwp_text_input_v3_send_preedit_hint(im, 0, 14, ZWP_TEXT_INPUT_V3_PREEDIT_HINT_PREFIX);
    zwp_text_input_v3_send_preedit_hint(im, 14, 23, ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SELECTION);
    zwp_text_input_v3_send_preedit_hint(im, 23, 24, ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SUFFIX);
    zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(boxrig_composition_get_view(
        boxrig_wayland_terminal_component_composition(component), &view) == BOXRIG_COMPOSITION_OK);
    CHECK(view.active && !view.caret_visible && view.text_length == strlen(text));
    CHECK(view.selection_offset == 14 && view.selection_length == 9);
    for (size_t index = 0; index < view.cluster_count; index++) {
        boxrig_composition_cluster_view cluster;
        CHECK(boxrig_composition_get_cluster(
            boxrig_wayland_terminal_component_composition(component), index, &cluster) == BOXRIG_COMPOSITION_OK);
        CHECK(cluster.selected == (cluster.text_offset >= 14 && cluster.text_offset < 23));
    }

    /* An unhinted next batch must not retain any previous clause decoration. */
    zwp_text_input_v3_send_preedit_string(im, text, 14, 14);
    zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(boxrig_composition_get_view(
        boxrig_wayland_terminal_component_composition(component), &view) == BOXRIG_COMPOSITION_OK);
    CHECK(view.selection_length == 0 && view.caret_visible && view.caret_offset == 14);
    for (size_t index = 0; index < view.cluster_count; index++) {
        boxrig_composition_cluster_view cluster;
        CHECK(boxrig_composition_get_cluster(
            boxrig_wayland_terminal_component_composition(component), index, &cluster) == BOXRIG_COMPOSITION_OK);
        CHECK(!cluster.selected);
        CHECK(cluster.feedback == BOXRIG_COMPOSITION_FEEDBACK_NONE);
    }

    /* Bad hints must not discard valid preedit or the accompanying commit. */
    zwp_text_input_v3_send_commit_string(im, "確定");
    zwp_text_input_v3_send_preedit_string(im, text, 14, 14);
    zwp_text_input_v3_send_preedit_hint(im, 2, 7, ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SELECTION);
    zwp_text_input_v3_send_preedit_hint(im, 23, 14, ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SELECTION);
    zwp_text_input_v3_send_preedit_hint(im, 0, UINT32_MAX, ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SELECTION);
    zwp_text_input_v3_send_preedit_hint(im, 0, 14, UINT32_MAX);
    zwp_text_input_v3_send_preedit_hint(im, 14, 23, ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SELECTION);
    zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(boxrig_composition_get_view(
        boxrig_wayland_terminal_component_composition(component), &view) == BOXRIG_COMPOSITION_OK);
    CHECK(view.active && view.selection_offset == 14 && view.selection_length == 9);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == strlen("確定"));
    CHECK(memcmp(output, "確定", strlen("確定")) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, strlen("確定")) == BOXRIG_STATUS_OK);

    /* Noncontiguous selections must never invert the gap between them. */
    zwp_text_input_v3_send_preedit_string(im, "abcdefg", 3, 3);
    const uint32_t hints[] = {ZWP_TEXT_INPUT_V3_PREEDIT_HINT_WHOLE,
        ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SELECTION, ZWP_TEXT_INPUT_V3_PREEDIT_HINT_PREDICTION,
        ZWP_TEXT_INPUT_V3_PREEDIT_HINT_PREFIX, ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SUFFIX,
        ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SPELLING_ERROR, ZWP_TEXT_INPUT_V3_PREEDIT_HINT_COMPOSE_ERROR};
    for (uint32_t index = 0; index < 7; index++)
        zwp_text_input_v3_send_preedit_hint(im, index, index + 1, hints[index]);
    zwp_text_input_v3_send_preedit_hint(im, 6, 7, ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SELECTION);
    zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(boxrig_composition_get_view(
        boxrig_wayland_terminal_component_composition(component), &view) == BOXRIG_COMPOSITION_OK);
    CHECK(view.cluster_count == 7 && view.selection_length == 0 && view.caret_offset == 3);
    for (size_t index = 0; index < 7; index++) {
        boxrig_composition_cluster_view cluster;
        CHECK(boxrig_composition_get_cluster(
            boxrig_wayland_terminal_component_composition(component), index, &cluster) == BOXRIG_COMPOSITION_OK);
        CHECK(cluster.feedback != BOXRIG_COMPOSITION_FEEDBACK_NONE);
        CHECK(((cluster.feedback & BOXRIG_COMPOSITION_FEEDBACK_TARGET) != 0) == (index == 1 || index == 6));
        CHECK(((cluster.feedback & BOXRIG_COMPOSITION_FEEDBACK_ERROR) != 0) == (index >= 5));
    }

    /* Excess decoration cannot suppress valid text or inject an old selection. */
    char many[1025];
    memset(many, 'a', sizeof(many) - 1); many[sizeof(many) - 1] = '\0';
    zwp_text_input_v3_send_preedit_string(im, many, 0, 0);
    for (uint32_t index = 0; index < sizeof(many) - 1; index++)
        zwp_text_input_v3_send_preedit_hint(im, index, index + 1, ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SELECTION);
    zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(boxrig_composition_get_view(
        boxrig_wayland_terminal_component_composition(component), &view) == BOXRIG_COMPOSITION_OK);
    CHECK(view.text_length == sizeof(many) - 1 && view.selection_length == 0);
    CHECK(view.segment_count == 0);

    /* A new focus epoch cannot inherit hints from an unfinished old batch. */
    zwp_text_input_v3_send_preedit_hint(im, 0, 3, ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SELECTION);
    zwp_text_input_v3_send_leave(im, server.child_resource);
    zwp_text_input_v3_send_enter(im, server.child_resource);
    pump(&server); dispatch_context(context); pump(&server);
    zwp_text_input_v3_send_preedit_string(im, "abc", 0, 0);
    zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(boxrig_composition_get_view(
        boxrig_wayland_terminal_component_composition(component), &view) == BOXRIG_COMPOSITION_OK);
    CHECK(view.text_length == 3 && view.selection_length == 0 && view.segment_count == 0);

    zwp_text_input_v3_send_preedit_string(im, NULL, 0, 0);
    zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
    pump(&server); dispatch_context(context);
    CHECK(boxrig_composition_get_view(
        boxrig_wayland_terminal_component_composition(component), &view) == BOXRIG_COMPOSITION_OK);
    CHECK(!view.active && view.cluster_count == 0 && view.selection_length == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    wl_array_release(&keys);
    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
    puts("PASS Wayland v2 preedit hints select the actual clause across navigation and resizing");
}

static void test_text_input_v2_cursor_surface_commit(void)
{
    for (size_t focus_parent = 0; focus_parent < 2; focus_parent++) {
        fake_server server;
        host_registry_state host;
        boxrig_terminal *terminal;
        boxrig_ui_model *model;
        boxrig_wayland_context *context;
        boxrig_wayland_terminal_component *component;
        boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
        boxrig_wayland_terminal_component_config config = boxrig_wayland_terminal_component_config_default();
        boxrig_font_effective_metrics metrics;
        boxrig_ui_layout_result layout;
        boxrig_ui_component_frame_result frame;
        boxrig_wayland_host_action action;
        bool applied;
        create_server(&server, true, false, true);
        wl_global_destroy(server.text_input_manager);
        server.text_input_manager = wl_global_create(server.server_display,
            &zwp_text_input_manager_v3_interface, 2, &server, bind_text_input_manager);
        CHECK(server.text_input_manager != NULL);
        struct wl_registry *registry = create_host_registry(&server, &host);
        terminal_config.rows = 4;
        terminal_config.columns = 40;
        CHECK(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
        CHECK(create_protocol_context(server.client_display, server.client_queue, NULL, &context) == BOXRIG_UI_OK);
        pump(&server); pump(&server); pump(&server);
        struct wl_surface *parent = wl_compositor_create_surface(host.compositor);
        wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
        config.keyboard_focus_fallback = focus_parent != 0;
        CHECK(create_steady_component(context, parent, model, &config, &component) == BOXRIG_UI_OK);
        CHECK(boxrig_wayland_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
        boxrig_ui_component_allocation allocation = {.width = 40 * metrics.cell_width,
            .height = 4 * metrics.cell_height, .x = 7, .y = 9,
            .window_state = BOXRIG_UI_WINDOW_EMBEDDED};
        CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
        CHECK(boxrig_wayland_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
        if (boxrig_wayland_terminal_component_take_host_action(component, &action)) {
            wl_surface_commit(parent);
            CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component, action.revision, &applied) == BOXRIG_UI_OK && applied);
        }
        pump(&server);
        server.text_input_v2_focus = focus_parent ? server.parent_resource : server.child_resource;
        send_keymap(&server, 0);
        struct wl_array keys;
        wl_array_init(&keys);
        wl_keyboard_send_enter(server.keyboard_resources[0], 10, server.text_input_v2_focus, &keys);
        struct wl_resource *im = server.text_input_resources[0];
        zwp_text_input_v3_send_enter(im, server.text_input_v2_focus);
        pump(&server); dispatch_context(context);
        for (int cursor = 0; cursor <= 3; cursor++) {
            for (size_t index = 0; index < server.frame_callback_count; index++)
                if (server.frame_callbacks[index] != NULL) complete_frame(&server, index);
            for (size_t index = 0; index < server.shm_buffer_create_count; index++)
                if (server.shm_buffer_busy[index]) release_buffer(&server, index);
            pump(&server);
            zwp_text_input_v3_send_preedit_string(im, "abc", cursor, cursor);
            zwp_text_input_v3_send_done(im, server.text_input_commit_serials[0]);
            pump(&server); dispatch_context(context); pump(&server);
            size_t before = server.text_input_v2_apply_count;
            CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
            pump(&server);
            if (focus_parent) {
                CHECK(server.text_input_v2_apply_count == before);
                CHECK(boxrig_wayland_terminal_component_take_host_action(component, &action));
                wl_surface_commit(parent);
                CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component, action.revision, &applied) == BOXRIG_UI_OK && applied);
                pump(&server);
            }
            CHECK(server.text_input_v2_apply_count == before + 1);
            CHECK(server.text_input_v2_applied_x == cursor * (int32_t)metrics.cell_width + (focus_parent ? 7 : 0));
        }
        /* Repositioning a settled child does not repaint it, but a parent-
         * focused IME still needs its candidate coordinates moved with it. */
        boxrig_wayland_component_diagnostic diagnostic;
        CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
        uint64_t generation = diagnostic.presentation_generation;
        allocation.x += 13;
        allocation.y += 17;
        CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
        CHECK(boxrig_wayland_terminal_component_take_host_action(component, &action));
        wl_surface_commit(parent);
        CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component, action.revision, &applied) == BOXRIG_UI_OK && applied);
        pump(&server);
        CHECK(server.text_input_v2_applied_x == 3 * (int32_t)metrics.cell_width + (focus_parent ? allocation.x : 0));
        CHECK(server.text_input_cursor_y == (focus_parent ? allocation.y : 0));
        CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
        CHECK(diagnostic.presentation_generation == generation);
        wl_array_release(&keys);
        boxrig_wayland_terminal_component_destroy(component);
        wl_surface_destroy(parent);
        wl_compositor_destroy(host.compositor);
        wl_registry_destroy(registry);
        pump(&server);
        boxrig_wayland_context_destroy(context);
        boxrig_ui_model_destroy(model);
        boxrig_terminal_destroy(terminal);
        destroy_server(&server);
    }
    puts("PASS Wayland v2 applies candidate positions on the focused child or caller-owned parent commit");
}

static void test_multiple_seats_and_components_are_independent(void)
{
    fake_server server;
    host_registry_state host;
    struct wl_registry *host_registry;
    struct wl_surface *parent_a;
    struct wl_surface *parent_b;
    struct wl_resource *child_a;
    struct wl_resource *child_b;
    struct wl_array keys;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal_a = NULL;
    boxrig_terminal *terminal_b = NULL;
    boxrig_ui_model *model_a = NULL;
    boxrig_ui_model *model_b = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component_a = NULL;
    boxrig_wayland_terminal_component *component_b = NULL;
    boxrig_wayland_context_diagnostic diagnostic;
    boxrig_wayland_component_diagnostic component_diagnostic;
    const uint8_t *output;
    size_t output_length;

    create_server(&server, true, false, true);
    host_registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal_a) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal_b) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal_a, &model_a) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_model_create(terminal_b, &model_b) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              NULL,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    pump(&server);
    parent_a = wl_compositor_create_surface(host.compositor);
    CHECK(parent_a != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent_a, server.client_queue);
    CHECK(create_steady_component(
              context,
              parent_a,
              model_a,
              NULL,
              &component_a) == BOXRIG_UI_OK);
    pump(&server);
    child_a = server.child_resource;
    parent_b = wl_compositor_create_surface(host.compositor);
    CHECK(parent_b != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent_b, server.client_queue);
    CHECK(create_steady_component(
              context,
              parent_b,
              model_b,
              NULL,
              &component_b) == BOXRIG_UI_OK);
    pump(&server);
    child_b = server.child_resource;

    add_seat(&server, 1);
    pump(&server);
    pump(&server);
    pump(&server);
    CHECK(server.seat_bind_count == 2);
    CHECK(server.keyboard_create_count == 2);
    CHECK(server.text_input_create_count == 2);
    send_keymap(&server, 0);
    send_keymap(&server, 1);
    wl_array_init(&keys);
    wl_keyboard_send_enter(server.keyboard_resources[0], 71, child_a, &keys);
    wl_keyboard_send_enter(server.keyboard_resources[1], 72, child_b, &keys);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        73,
        30,
        30,
        WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        74,
        31,
        30,
        WL_KEYBOARD_KEY_STATE_RELEASED);
    wl_keyboard_send_key(
        server.keyboard_resources[1],
        75,
        32,
        18,
        WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_keyboard_send_key(
        server.keyboard_resources[1],
        76,
        33,
        18,
        WL_KEYBOARD_KEY_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    output_length = boxrig_terminal_output_peek(terminal_a, &output);
    CHECK(output_length == 1 && output[0] == 'a');
    output_length = boxrig_terminal_output_peek(terminal_b, &output);
    CHECK(output_length == 1 && output[0] == 'e');
    CHECK(boxrig_terminal_output_consume(terminal_a, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal_b, 1) == BOXRIG_STATUS_OK);

    CHECK(boxrig_ui_view_focused(
        boxrig_wayland_terminal_component_view(component_a)));
    CHECK(boxrig_ui_view_focused(
        boxrig_wayland_terminal_component_view(component_b)));
    /* A view remains focused while any keyboard seat still targets it. */
    wl_keyboard_send_leave(server.keyboard_resources[1], 83, child_b);
    wl_keyboard_send_enter(server.keyboard_resources[1], 84, child_a, &keys);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_ui_view_focused(
        boxrig_wayland_terminal_component_view(component_a)));
    CHECK(!boxrig_ui_view_focused(
        boxrig_wayland_terminal_component_view(component_b)));
    wl_keyboard_send_leave(server.keyboard_resources[0], 85, child_a);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_ui_view_focused(
        boxrig_wayland_terminal_component_view(component_a)));
    wl_keyboard_send_enter(server.keyboard_resources[0], 86, child_a, &keys);
    wl_keyboard_send_leave(server.keyboard_resources[1], 87, child_a);
    wl_keyboard_send_enter(server.keyboard_resources[1], 88, child_b, &keys);
    pump(&server);
    dispatch_context(context);

    wl_pointer_send_enter(
        server.pointer_resources[0],
        77,
        child_a,
        wl_fixed_from_int(1),
        wl_fixed_from_int(1));
    wl_pointer_send_enter(
        server.pointer_resources[1],
        78,
        child_a,
        wl_fixed_from_int(1),
        wl_fixed_from_int(1));
    wl_pointer_send_button(
        server.pointer_resources[0],
        79,
        34,
        0x110,
        WL_POINTER_BUTTON_STATE_PRESSED);
    wl_pointer_send_button(
        server.pointer_resources[1],
        80,
        35,
        0x110,
        WL_POINTER_BUTTON_STATE_PRESSED);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component_a,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.pointer_focus_count == 2);
    CHECK(component_diagnostic.pressed_button_count == 2);

    /* Each seat edits only its focused document; a BUSY owner does not stop
     * another view, and leaving drops that seat's queued replacement. */
    editable_owner owners[2] = {{.disposition = BOXRIG_UI_BUSY}, {.disposition = BOXRIG_UI_OK}};
    boxrig_wayland_terminal_component *components[2] = {component_a, component_b};
    struct wl_resource *children[2] = {child_a, child_b};
    boxrig_ui_editable_document *endpoints[2];
    for (size_t index = 0; index < 2; index++) {
        endpoints[index] = boxrig_ui_view_editable_document(boxrig_wayland_terminal_component_view(components[index]));
        CHECK(boxrig_ui_editable_document_set_apply_handler(endpoints[index], apply_editable,
            &owners[index]) == BOXRIG_UI_OK);
        publish_editable(endpoints[index], &owners[index], index == 0 ? "seat-a" : "seat-b", 6, 6);
        zwp_text_input_v3_send_enter(server.text_input_resources[index], children[index]);
    }
    pump(&server); dispatch_context(context); pump(&server);
    for (size_t index = 0; index < 2; index++) {
        CHECK(strcmp(server.surrounding_text[index], owners[index].text) == 0);
        zwp_text_input_v3_send_commit_string(server.text_input_resources[index], "!");
        zwp_text_input_v3_send_done(server.text_input_resources[index], server.text_input_commit_serials[index]);
    }
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(owners[0].accepted == 0 && owners[1].accepted == 1);
    CHECK(strcmp(owners[0].text, "seat-a") == 0 && strcmp(owners[1].text, "seat-b!") == 0);
    owners[0].disposition = BOXRIG_UI_OK;
    dispatch_context(context); pump(&server);
    CHECK(owners[0].accepted == 1 && strcmp(owners[0].text, "seat-a!") == 0);
    zwp_text_input_v3_send_commit_string(server.text_input_resources[0], "late");
    zwp_text_input_v3_send_done(server.text_input_resources[0], server.text_input_commit_serials[0]);
    pump(&server);
    for (size_t index = 0; index < 2; index++)
        zwp_text_input_v3_send_leave(server.text_input_resources[index], children[index]);
    pump(&server); dispatch_context(context); pump(&server);
    CHECK(owners[0].accepted == 1 && owners[1].accepted == 1);
    for (size_t index = 0; index < 2; index++)
        CHECK(boxrig_ui_editable_document_clear(endpoints[index]) == BOXRIG_UI_OK);

    CHECK(boxrig_wayland_context_update_monotonic_time(context, 3000) ==
          BOXRIG_UI_OK);
    wl_keyboard_send_repeat_info(server.keyboard_resources[0], 20, 300);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        77,
        34,
        30,
        WL_KEYBOARD_KEY_STATE_PRESSED);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_consume(terminal_a, 1) == BOXRIG_STATUS_OK);
    wl_global_remove(server.seats[0]);
    server.seats[0] = NULL;
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.seat_count == 1);
    CHECK(diagnostic.active_repeat_count == 0);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component_a,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.pointer_focus_count == 1);
    CHECK(component_diagnostic.pressed_button_count == 1);

    wl_pointer_send_button(
        server.pointer_resources[1],
        81,
        36,
        0x110,
        WL_POINTER_BUTTON_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component_a,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.pressed_button_count == 0);

    wl_keyboard_send_key(
        server.keyboard_resources[1],
        78,
        35,
        18,
        WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_keyboard_send_key(
        server.keyboard_resources[1],
        79,
        36,
        18,
        WL_KEYBOARD_KEY_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    output_length = boxrig_terminal_output_peek(terminal_b, &output);
    CHECK(output_length == 1 && output[0] == 'e');

    CHECK(boxrig_wayland_terminal_component_quiesce(component_b) ==
          BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_focused(
        boxrig_wayland_terminal_component_view(component_b)));

    wl_array_release(&keys);
    boxrig_wayland_terminal_component_destroy(component_b);
    boxrig_wayland_terminal_component_destroy(component_a);
    wl_surface_destroy(parent_b);
    wl_surface_destroy(parent_a);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host_registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model_b);
    boxrig_ui_model_destroy(model_a);
    boxrig_terminal_destroy(terminal_b);
    boxrig_terminal_destroy(terminal_a);
    destroy_server(&server);
}

static void test_input_backpressure_and_resource_limits(void)
{
    fake_server server;
    host_registry_state host;
    struct wl_registry *host_registry;
    struct wl_surface *parent;
    struct wl_array keys;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_wayland_context_config context_config =
        protocol_context_config();
    boxrig_wayland_terminal_component_config component_config =
        boxrig_wayland_terminal_component_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_wayland_dispatch_result dispatch_result;
    boxrig_wayland_context_diagnostic context_diagnostic;
    boxrig_wayland_component_diagnostic component_diagnostic;
    boxrig_composition_view composition_view;
    const uint8_t *output;
    size_t output_length;
    size_t index;

    terminal_config.output_capacity = 1;
    context_config.maximum_seats = 1;
    component_config.maximum_input_events = 4;
    component_config.maximum_ime_bytes = 2;
    create_server(&server, true, false, true);
    host_registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(
              server.client_display,
              server.client_queue,
              &context_config,
              &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    pump(&server);
    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    CHECK(create_steady_component(
              context,
              parent,
              model,
              &component_config,
              &component) == BOXRIG_UI_OK);
    pump(&server);
    send_keymap(&server, 0);
    wl_array_init(&keys);
    wl_keyboard_send_enter(
        server.keyboard_resources[0],
        81,
        server.child_resource,
        &keys);
    pump(&server);
    dispatch_context(context);

    wl_keyboard_send_key(
        server.keyboard_resources[0],
        82,
        40,
        30,
        WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        83,
        41,
        30,
        WL_KEYBOARD_KEY_STATE_RELEASED);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        84,
        42,
        18,
        WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_keyboard_send_key(
        server.keyboard_resources[0],
        85,
        43,
        18,
        WL_KEYBOARD_KEY_STATE_RELEASED);
    pump(&server);
    CHECK(boxrig_wayland_context_dispatch(
              context,
              BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT,
              &dispatch_result) == BOXRIG_UI_OK);
    CHECK(!dispatch_result.additional_dispatch);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.input_blocked);
    CHECK(component_diagnostic.input_queue_depth == 2);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 1 && output[0] == 'a');
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(!component_diagnostic.input_blocked);
    CHECK(component_diagnostic.input_queue_depth == 0);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 1 && output[0] == 'e');
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    zwp_text_input_v3_send_enter(
        server.text_input_resources[0],
        server.child_resource);
    pump(&server);
    pump(&server);
    zwp_text_input_v3_send_commit_string(
        server.text_input_resources[0],
        "a");
    zwp_text_input_v3_send_done(
        server.text_input_resources[0],
        server.text_input_commit_serials[0]);
    zwp_text_input_v3_send_commit_string(
        server.text_input_resources[0],
        "e");
    zwp_text_input_v3_send_done(
        server.text_input_resources[0],
        server.text_input_commit_serials[0]);
    pump(&server);
    CHECK(boxrig_wayland_context_dispatch(
              context,
              BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT,
              &dispatch_result) == BOXRIG_UI_OK);
    CHECK(!dispatch_result.additional_dispatch);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.input_blocked);
    CHECK(component_diagnostic.input_queue_depth == 0);
    CHECK(component_diagnostic.ime_commit_pending);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 1 && output[0] == 'a');
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(!component_diagnostic.input_blocked);
    CHECK(!component_diagnostic.ime_commit_pending);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 1 && output[0] == 'e');
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    zwp_text_input_v3_send_preedit_string(
        server.text_input_resources[0],
        "あ",
        0,
        0);
    zwp_text_input_v3_send_done(
        server.text_input_resources[0],
        server.text_input_commit_serials[0]);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_composition_get_view(
              boxrig_wayland_terminal_component_composition(component),
              &composition_view) == BOXRIG_COMPOSITION_OK);
    CHECK(!composition_view.active);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.input_error_count >= 1);
    zwp_text_input_v3_send_leave(
        server.text_input_resources[0],
        server.child_resource);
    pump(&server);
    dispatch_context(context);

    for (index = 0; index < 5; index++) {
        wl_keyboard_send_key(
            server.keyboard_resources[0],
            (uint32_t)(90 + index),
            (uint32_t)(50 + index),
            index % 2 == 0 ? 30 : 18,
            WL_KEYBOARD_KEY_STATE_PRESSED);
    }
    pump(&server);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &component_diagnostic) == BOXRIG_UI_OK);
    CHECK(component_diagnostic.failure_reason ==
          BOXRIG_WAYLAND_FAILURE_INPUT_QUEUE_OVERFLOW);
    CHECK(component_diagnostic.input_queue_depth == 4);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    add_seat(&server, 1);
    pump(&server);
    pump(&server);
    CHECK(boxrig_wayland_context_get_diagnostic(context, &context_diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(context_diagnostic.seat_count == 1);
    CHECK(context_diagnostic.unavailable_seat_count == 1);

    wl_array_release(&keys);
    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host_registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
}

static uint64_t blink_now, blink_generation;
static bool blink_reserved;
static bool blink_reject;
static boxrig_ui_status read_blink_time(void *context, uint64_t *time_out)
{
    (void)context;
    *time_out = blink_now;
    return BOXRIG_UI_OK;
}
static boxrig_ui_status reserve_blink(void *context, uint64_t deadline, uint64_t generation)
{
    (void)context;
    CHECK(deadline > blink_now);
    if (blink_reject) return BOXRIG_UI_UNAVAILABLE;
    blink_generation = generation;
    blink_reserved = true;
    return BOXRIG_UI_OK;
}
static void cancel_blink(void *context)
{
    (void)context;
    blink_reserved = false;
}
static void test_keyboard_resets_cursor_blink(void)
{
    fake_server server;
    host_registry_state host;
    struct wl_registry *registry;
    struct wl_surface *parent;
    struct wl_array keys;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_wayland_terminal_component_config config = boxrig_wayland_terminal_component_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_ui_animation_runtime *runtime = NULL;
    boxrig_ui_animation_scheduler scheduler = {NULL, read_blink_time, reserve_blink, cancel_blink};
    CHECK(boxrig_ui_animation_runtime_create(&scheduler, &runtime) == BOXRIG_UI_OK);
    config.animation_runtime = runtime;
    create_server(&server, true, false, true);
    registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(server.client_display, server.client_queue, NULL, &context) == BOXRIG_UI_OK);
    pump(&server); pump(&server); pump(&server);
    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    boxrig_wayland_terminal_component_config missing = config;
    missing.animation_runtime = NULL;
    CHECK(boxrig_wayland_terminal_component_create(context, parent, model, &missing, &component) == BOXRIG_UI_UNAVAILABLE);
    CHECK(component == NULL);
    CHECK(boxrig_wayland_terminal_component_create(context, parent, model, &config, &component) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    pump(&server);
    send_keymap(&server, 0);
    wl_array_init(&keys);
    wl_keyboard_send_enter(server.keyboard_resources[0], 41, server.child_resource, &keys);
    pump(&server);
    dispatch_context(context);
    boxrig_ui_view *view = boxrig_wayland_terminal_component_view(component);
    CHECK(blink_reserved && boxrig_ui_view_cursor_blink_visible(view));
    blink_now = 500;
    CHECK(boxrig_ui_animation_runtime_dispatch(runtime, blink_generation) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_cursor_blink_visible(view));
    wl_keyboard_send_key(server.keyboard_resources[0], 42, 10, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
    pump(&server);
    dispatch_context(context);
    const uint8_t *output;
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1 && output[0] == 'a');
    CHECK(boxrig_ui_view_cursor_blink_visible(view));
    blink_now = 1000;
    CHECK(boxrig_ui_animation_runtime_dispatch(runtime, blink_generation) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_cursor_blink_visible(view));
    wl_keyboard_send_key(server.keyboard_resources[0], 43, 11, 30, WL_KEYBOARD_KEY_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    CHECK(!boxrig_ui_view_cursor_blink_visible(view));
    zwp_text_input_v3_send_enter(server.text_input_resources[0], server.child_resource);
    pump(&server); pump(&server);
    zwp_text_input_v3_send_commit_string(server.text_input_resources[0], "b");
    zwp_text_input_v3_send_done(server.text_input_resources[0], server.text_input_commit_serials[0]);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 2 && memcmp(output, "ab", 2) == 0);
    CHECK(boxrig_ui_view_cursor_blink_visible(view));
    blink_now = 1500;
    CHECK(boxrig_ui_animation_runtime_dispatch(runtime, blink_generation) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_cursor_blink_visible(view));
    static const char *const mime[] = {"text/plain;charset=utf-8"};
    send_selection_offer(&server, 0, false, mime, 1);
    pump(&server);
    boxrig_ui_view_action action = {0};
    action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    uint32_t seat_name = wl_global_get_name(server.seats[0], server.server_client);
    CHECK(boxrig_wayland_terminal_component_apply_view_action(component, seat_name, NULL, &action) == BOXRIG_UI_OK);
    pump(&server);
    CHECK(server.offer_receive_count == 1);
    CHECK(write(server.offer_receive_descriptors[0], "c", 1) == 1);
    close(server.offer_receive_descriptors[0]);
    for (unsigned attempt = 0; ; attempt++) {
        boxrig_wayland_wait_source sources[2];
        size_t count;
        CHECK(boxrig_wayland_context_prepare_wait_sources(context, sources, 2, &count) == BOXRIG_UI_OK);
        if (count == 0) break;
        CHECK(attempt < 4);
        for (size_t i = 0; i < count; i++) sources[i].readiness = BOXRIG_WAYLAND_WAIT_READABLE;
        boxrig_wayland_transfer_dispatch_result transfer;
        CHECK(boxrig_wayland_context_dispatch_wait_sources(context, sources, count, 1, &transfer) == BOXRIG_UI_OK);
    }
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 3 && memcmp(output, "abc", 3) == 0);
    CHECK(boxrig_ui_view_cursor_blink_visible(view));
    wl_keyboard_send_leave(server.keyboard_resources[0], 44, server.child_resource);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_ui_view_cursor_blink_visible(view));
    CHECK(!blink_reserved);
    wl_keyboard_send_enter(server.keyboard_resources[0], 45, server.child_resource, &keys);
    pump(&server);
    dispatch_context(context);
    blink_now = 2000;
    blink_reject = true;
    CHECK(boxrig_ui_animation_runtime_dispatch(runtime, blink_generation) == BOXRIG_UI_UNAVAILABLE);
    boxrig_wayland_component_diagnostic diagnostic;
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(!diagnostic.running && diagnostic.failure_reason == BOXRIG_WAYLAND_FAILURE_ANIMATION);

    wl_array_release(&keys);
    boxrig_wayland_terminal_component_destroy(component);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 2000) == BOXRIG_STATUS_OK);
    boxrig_ui_animation_runtime_destroy(runtime);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
}


static void test_input_inhibition(void)
{
    fake_server server;
    host_registry_state host;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.output_capacity = 6;
    boxrig_wayland_terminal_component_config component_config = boxrig_wayland_terminal_component_config_default();
    component_config.paste_chunk_size = 1;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    create_server(&server, true, false, true);
    struct wl_registry *registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(server.client_display, server.client_queue, NULL, &context) == BOXRIG_UI_OK);
    pump(&server); pump(&server); pump(&server);
    struct wl_surface *parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    CHECK(create_steady_component(context, parent, model, &component_config, &component) == BOXRIG_UI_OK);
    pump(&server);
    send_keymap(&server, 0);
    struct wl_array keys;
    wl_array_init(&keys);
    wl_keyboard_send_enter(server.keyboard_resources[0], 61, server.child_resource, &keys);
    zwp_text_input_v3_send_enter(server.text_input_resources[0], server.child_resource);
    pump(&server);
    dispatch_context(context);
    pump(&server);
    boxrig_ui_input_state state;
    CHECK(boxrig_wayland_terminal_component_get_input_state(component, &state) == BOXRIG_UI_OK);
    CHECK(state.phase == BOXRIG_UI_INPUT_ENABLED && state.generation != 0);
    uint64_t generation = state.generation;
    uint32_t old_serial = server.text_input_commit_serials[0];
    const uint8_t *output;

    CHECK(boxrig_terminal_send_text(terminal, "filled", 6) == BOXRIG_STATUS_OK);
    wl_keyboard_send_key(server.keyboard_resources[0], 62, 10, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED);
    zwp_text_input_v3_send_commit_string(server.text_input_resources[0], "old");
    zwp_text_input_v3_send_done(server.text_input_resources[0], old_serial);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, false) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_input_state(component, &state) == BOXRIG_UI_OK);
    CHECK(state.phase == BOXRIG_UI_INPUT_DISABLED && state.generation > generation);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 6 && memcmp(output, "filled", 6) == 0);
    CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, true) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);
    zwp_text_input_v3_send_commit_string(server.text_input_resources[0], "late");
    zwp_text_input_v3_send_done(server.text_input_resources[0], old_serial);
    wl_keyboard_send_key(server.keyboard_resources[0], 63, 11, KEY_A, WL_KEYBOARD_KEY_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    wl_keyboard_send_key(server.keyboard_resources[0], 64, 12, KEY_E, WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_keyboard_send_key(server.keyboard_resources[0], 65, 13, KEY_E, WL_KEYBOARD_KEY_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1 && output[0] == 'e');
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    pump(&server);
    zwp_text_input_v3_send_commit_string(server.text_input_resources[0], "z");
    zwp_text_input_v3_send_done(server.text_input_resources[0], server.text_input_commit_serials[0]);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1 && output[0] == 'z');
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    key_route_capture routed = {.inhibition_probe = component};
    CHECK(boxrig_wayland_terminal_component_set_key_router(component, capture_key_route, &routed) == BOXRIG_UI_OK);
    /* Native receipt and FIFO dispatch are separate boundaries. A held key
     * remains inhibited whether its press arrived before or during the stop. */
    for (unsigned int boundary = 0; boundary < 2; boundary++) {
        if (boundary != 0)
            CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, false) == BOXRIG_UI_OK);
        wl_keyboard_send_key(server.keyboard_resources[0], 66, 14, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED);
        pump(&server);
        if (boundary == 0)
            CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, false) == BOXRIG_UI_OK);
        else dispatch_context(context);
        CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, true) == BOXRIG_UI_OK);
        wl_keyboard_send_key(server.keyboard_resources[0], 67, 15, KEY_A, WL_KEYBOARD_KEY_STATE_RELEASED);
        pump(&server);
        dispatch_context(context);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
        wl_keyboard_send_key(server.keyboard_resources[0], 68, 16, KEY_E, WL_KEYBOARD_KEY_STATE_PRESSED);
        wl_keyboard_send_key(server.keyboard_resources[0], 69, 17, KEY_E, WL_KEYBOARD_KEY_STATE_RELEASED);
        pump(&server);
        dispatch_context(context);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 1 && output[0] == 'e');
        CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
        CHECK(boxrig_wayland_terminal_component_set_input_config(component, &config.input) == BOXRIG_UI_OK);
    }
    CHECK(routed.count != 0);
    CHECK(boxrig_wayland_terminal_component_set_key_router(component, NULL, NULL) == BOXRIG_UI_OK);

    const char *mimes[] = {"text/plain;charset=utf-8"};
    send_selection_offer(&server, 0, false, mimes, 1);
    pump(&server);
    boxrig_ui_view_action action = {BOXRIG_UI_VIEW_ACTION_PASTE, BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD};
    uint32_t seat = wl_global_get_name(server.seats[0], server.server_client);
    CHECK(boxrig_wayland_terminal_component_apply_view_action(component, seat, NULL, &action) == BOXRIG_UI_OK);
    pump(&server);
    boxrig_wayland_wait_source sources[2];
    size_t count;
    CHECK(boxrig_wayland_context_prepare_wait_sources(context, sources, 2, &count) == BOXRIG_UI_OK && count == 1);
    CHECK(write(server.offer_receive_descriptors[0], "old", 3) == 3);
    close(server.offer_receive_descriptors[0]);
    CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, false) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_apply_view_action(component, seat, NULL, &action) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, true) == BOXRIG_UI_OK);
    sources[0].readiness = BOXRIG_WAYLAND_WAIT_READABLE;
    boxrig_wayland_transfer_dispatch_result transfer;
    CHECK(boxrig_wayland_context_dispatch_wait_sources(context, sources, count, 1, &transfer) == BOXRIG_UI_OK);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[?2004h", 8, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_wayland_terminal_component_apply_view_action(component, seat, NULL, &action) == BOXRIG_UI_OK);
    pump(&server);
    CHECK(write(server.offer_receive_descriptors[1], "discard", 7) == 7);
    close(server.offer_receive_descriptors[1]);
    for (unsigned attempt = 0; attempt < 16; attempt++) {
        CHECK(boxrig_wayland_context_prepare_wait_sources(context, sources, 2, &count) == BOXRIG_UI_OK);
        for (size_t index = 0; index < count; index++) sources[index].readiness = BOXRIG_WAYLAND_WAIT_READABLE;
        CHECK(boxrig_wayland_context_dispatch_wait_sources(context, sources, count, 1, &transfer) == BOXRIG_UI_OK);
        dispatch_context(context);
        if (boxrig_terminal_output_peek(terminal, &output) == 6) break;
    }
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 6 && memcmp(output, "\033[200~", 6) == 0);
    CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, false) == BOXRIG_UI_BUSY);
    CHECK(boxrig_wayland_terminal_component_get_input_state(component, &state) == BOXRIG_UI_OK && state.phase == BOXRIG_UI_INPUT_STOPPING);
    CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, true) == BOXRIG_UI_BUSY);
    CHECK(boxrig_wayland_terminal_component_set_input_config(component, &config.input) == BOXRIG_UI_BUSY);
    routed.count = 0;
    CHECK(boxrig_wayland_terminal_component_set_key_router(component, capture_key_route, &routed) == BOXRIG_UI_OK);
    wl_keyboard_send_key(server.keyboard_resources[0], 70, 18, KEY_E, WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_keyboard_send_key(server.keyboard_resources[0], 71, 19, KEY_E, WL_KEYBOARD_KEY_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    CHECK(routed.count == 2);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 6 && memcmp(output, "\033[200~", 6) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[5n", 4, &consumed) == BOXRIG_STATUS_OK);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 4 && memcmp(output, "\033[0n", 4) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 4) == BOXRIG_STATUS_OK);
    dispatch_context(context);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 6 && memcmp(output, "\033[201~", 6) == 0);
    CHECK(boxrig_wayland_terminal_component_get_input_state(component, &state) == BOXRIG_UI_OK && state.phase == BOXRIG_UI_INPUT_DISABLED);
    CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, true) == BOXRIG_UI_OK);

    wl_array_release(&keys);
    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
    puts("PASS Wayland input inhibition across native generations");
}

static void test_terminal_io_configuration(void)
{
    for (unsigned encoding = 0; encoding < 2; encoding++) {
        fake_server server;
        host_registry_state host;
        const uint8_t mapped[] = {0x1b, 'U', 0};
        const boxrig_key_mapping mapping = {BOXRIG_KEY_UP, 0, mapped, sizeof(mapped)};
        boxrig_terminal_config config = boxrig_terminal_config_default();
        config.encoding = (boxrig_encoding)encoding;
        config.input.enter_newline = BOXRIG_NEWLINE_LF;
        config.paste_newline = BOXRIG_NEWLINE_CRLF;
        config.input.key_mappings = &mapping;
        config.input.key_mapping_count = 1;
        boxrig_terminal *terminal = NULL;
        boxrig_ui_model *model = NULL;
        boxrig_wayland_context *context = NULL;
        boxrig_wayland_terminal_component *component = NULL;
        create_server(&server, true, false, true);
        struct wl_registry *registry = create_host_registry(&server, &host);
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
        CHECK(create_protocol_context(server.client_display, server.client_queue,
                  NULL, &context) == BOXRIG_UI_OK);
        pump(&server); pump(&server); pump(&server);
        struct wl_surface *parent = wl_compositor_create_surface(host.compositor);
        CHECK(parent != NULL);
        wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
        CHECK(create_steady_component(context, parent, model, NULL, &component) == BOXRIG_UI_OK);
        boxrig_ui_component_allocation allocation = {0};
        allocation.width = 80;
        allocation.height = 40;
        allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
        boxrig_ui_layout_result layout;
        CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
        pump(&server);
        send_keymap(&server, 0);
        struct wl_array keys;
        wl_array_init(&keys);
        wl_keyboard_send_enter(server.keyboard_resources[0], 61, server.child_resource, &keys);
        zwp_text_input_v3_send_enter(server.text_input_resources[0], server.child_resource);
        pump(&server);
        dispatch_context(context);
        pump(&server);
        CHECK(server.text_input_commit_count == 1);
        zwp_text_input_v3_send_commit_string(server.text_input_resources[0], "日本");
        zwp_text_input_v3_send_done(server.text_input_resources[0], server.text_input_commit_serials[0]);
        pump(&server);
        dispatch_context(context);
        const uint32_t keycodes[] = {103, 28};
        for (unsigned index = 0; index < 2; index++) {
            wl_keyboard_send_key(server.keyboard_resources[0], 62 + index * 2, 10 + index * 2,
                keycodes[index], WL_KEYBOARD_KEY_STATE_PRESSED);
            wl_keyboard_send_key(server.keyboard_resources[0], 63 + index * 2, 11 + index * 2,
                keycodes[index], WL_KEYBOARD_KEY_STATE_RELEASED);
        }
        pump(&server);
        dispatch_context(context);
        const char *mimes[] = {"text/plain;charset=utf-8"};
        send_selection_offer(&server, 0, false, mimes, 1);
        pump(&server);
        boxrig_ui_view_action action = {0};
        action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
        action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
        uint32_t seat = wl_global_get_name(server.seats[0], server.server_client);
        CHECK(boxrig_wayland_terminal_component_apply_view_action(component, seat, NULL, &action) == BOXRIG_UI_OK);
        pump(&server);
        CHECK(server.offer_receive_count == 1);
        CHECK(write(server.offer_receive_descriptors[0], "日本\n", 7) == 7);
        close(server.offer_receive_descriptors[0]);
        bool complete = false;
        for (unsigned attempt = 0; attempt < 16; attempt++) {
            boxrig_wayland_wait_source sources[2];
            size_t count;
            CHECK(boxrig_wayland_context_prepare_wait_sources(context, sources, 2, &count) == BOXRIG_UI_OK);
            for (size_t index = 0; index < count; index++) sources[index].readiness = BOXRIG_WAYLAND_WAIT_READABLE;
            boxrig_wayland_transfer_dispatch_result result;
            CHECK(boxrig_wayland_context_dispatch_wait_sources(context, sources, count, 1, &result) == BOXRIG_UI_OK);
            dispatch_context(context);
            boxrig_wayland_component_diagnostic diagnostic;
            CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
            CHECK(boxrig_wayland_context_prepare_wait_sources(context, sources, 2, &count) == BOXRIG_UI_OK);
            if (count == 0 && !diagnostic.paste_active && diagnostic.input_queue_depth == 0) { complete = true; break; }
        }
        CHECK(complete);
        const uint8_t utf8_expected[] = "日本\033U\0\n日本\r\n";
        const uint8_t sjis_expected[] = "\x93\xfa\x96\x7b\033U\0\n\x93\xfa\x96\x7b\r\n";
        const uint8_t *expected = encoding ? sjis_expected : utf8_expected;
        size_t length = encoding ? sizeof(sjis_expected) - 1 : sizeof(utf8_expected) - 1;
        const uint8_t *output;
        CHECK(boxrig_terminal_output_peek(terminal, &output) == length);
        CHECK(memcmp(output, expected, length) == 0);
        wl_array_release(&keys);
        boxrig_wayland_terminal_component_destroy(component);
        wl_surface_destroy(parent);
        wl_compositor_destroy(host.compositor);
        wl_registry_destroy(registry);
        pump(&server);
        boxrig_wayland_context_destroy(context);
        boxrig_ui_model_destroy(model);
        boxrig_terminal_destroy(terminal);
        destroy_server(&server);
    }
    puts("PASS native Wayland UTF-8/Shift_JIS IME, key and paste settings");
}

static void test_standard_scrollbar_visibility(void)
{
    fake_server server;
    host_registry_state host;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    terminal_config.rows = 4;
    terminal_config.columns = 20;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    create_server(&server, true, true, true);
    struct wl_registry *registry = create_host_registry(&server, &host);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(server.client_display, server.client_queue,
        NULL, &context) == BOXRIG_UI_OK);
    pump(&server);
    pump(&server);
    pump(&server);
    struct wl_surface *parent = wl_compositor_create_surface(host.compositor);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    boxrig_wayland_terminal_component_config config =
        boxrig_wayland_terminal_component_config_default();
    CHECK(config.scrollbar_presentation == BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED);
    config.view.blink.text_enabled = false;
    config.view.blink.cursor_enabled = false;
    CHECK(boxrig_wayland_terminal_component_create(context, parent, model,
        &config, &component) == BOXRIG_UI_OK);
    boxrig_ui_scrollbar_visibility_state visibility;
    CHECK(boxrig_wayland_terminal_component_get_scrollbar_visibility(component,
        &visibility) == BOXRIG_UI_OK);
    CHECK(visibility.requested == BOXRIG_UI_SCROLLBAR_ALWAYS && visibility.visible &&
        !visibility.pending && visibility.reserved_width > 0);
    unsigned int bar_width = visibility.reserved_width;
    boxrig_font_effective_metrics metrics;
    CHECK(boxrig_wayland_terminal_component_get_effective_metrics(component,
        &metrics) == BOXRIG_FONT_CATALOG_OK);
    boxrig_ui_component_allocation allocation = {0};
    allocation.width = metrics.cell_width * terminal_config.columns + bar_width;
    allocation.height = metrics.cell_height * terminal_config.rows;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    boxrig_ui_layout_result layout;
    boxrig_ui_layout_action action;
    boxrig_ui_component_frame_result frame;
    CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation,
        &layout) == BOXRIG_UI_OK);
    CHECK(layout.columns == terminal_config.columns && layout.rows == terminal_config.rows);
    CHECK(!boxrig_wayland_terminal_component_take_layout_action(component, &action));
    pump(&server);
    CHECK(server.shm_mapping != NULL);
    const uint32_t *pixels = server.shm_mapping;
    for (unsigned int x = allocation.width - bar_width; x < allocation.width; x++)
        CHECK((pixels[x] & UINT32_C(0xffffff)) == UINT32_C(0x484848));
    CHECK((pixels[allocation.width - bar_width - 1] & UINT32_C(0xffffff)) != UINT32_C(0x484848));
    size_t consumed;
    for (size_t row = 0; row < 30; row++) {
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"row\r\n", 5, &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == 5);
    }
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    boxrig_scrollbar_state before;
    CHECK(boxrig_wayland_terminal_component_get_scrollbar_state(component, &before) == BOXRIG_UI_OK);
    CHECK(before.model.enabled);
    mouse_route_capture capture = {0};
    CHECK(boxrig_wayland_terminal_component_set_mouse_router(component, capture_mouse_route,
        &capture) == BOXRIG_UI_OK);
    pump(&server);
    wl_pointer_send_enter(server.pointer_resources[0], 71, server.child_resource,
        wl_fixed_from_int(allocation.width - 1), wl_fixed_from_int(0));
    wl_pointer_send_button(server.pointer_resources[0], 72, 10, 0x110, WL_POINTER_BUTTON_STATE_PRESSED);
    wl_pointer_send_button(server.pointer_resources[0], 73, 11, 0x110, WL_POINTER_BUTTON_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    boxrig_scrollbar_state scrolled;
    CHECK(boxrig_wayland_terminal_component_get_scrollbar_state(component, &scrolled) == BOXRIG_UI_OK);
    CHECK(scrolled.model.position < before.model.position && capture.count == 0);
    size_t track_position = scrolled.model.position;
    wl_pointer_send_axis(server.pointer_resources[0], 11, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(1));
    wl_pointer_send_axis_value120(server.pointer_resources[0], WL_POINTER_AXIS_VERTICAL_SCROLL, 120);
    wl_pointer_send_frame(server.pointer_resources[0]);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_get_scrollbar_state(component, &scrolled) == BOXRIG_UI_OK);
    CHECK(scrolled.model.position == track_position + 1 && capture.count == 0);
    boxrig_scrollbar_thumb thumb;
    boxrig_scrollbar_calculate_thumb(&scrolled.model, allocation.height, bar_width, &thumb);
    wl_pointer_send_motion(server.pointer_resources[0], 12,
        wl_fixed_from_int(allocation.width - 1), wl_fixed_from_int(thumb.start + 1));
    wl_pointer_send_button(server.pointer_resources[0], 74, 13, 0x110, WL_POINTER_BUTTON_STATE_PRESSED);
    wl_pointer_send_motion(server.pointer_resources[0], 14,
        wl_fixed_from_int(allocation.width - 1), wl_fixed_from_int(1));
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_get_scrollbar_state(component, &scrolled) == BOXRIG_UI_OK);
    CHECK(scrolled.model.position == 0 && capture.count == 0);
    CHECK(boxrig_wayland_terminal_component_set_scrollbar_visibility(component,
        BOXRIG_UI_SCROLLBAR_NEVER) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_scrollbar_visibility(component,
        &visibility) == BOXRIG_UI_OK);
    CHECK(visibility.requested == BOXRIG_UI_SCROLLBAR_NEVER && visibility.visible &&
        visibility.pending && visibility.reserved_width == bar_width);
    CHECK(boxrig_wayland_terminal_component_set_scrollbar_visibility(component,
        BOXRIG_UI_SCROLLBAR_AUTO) == BOXRIG_UI_BUSY);
    CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation,
        &layout) == BOXRIG_UI_BUSY);
    boxrig_font_specification font = boxrig_font_specification_default();
    CHECK(boxrig_wayland_terminal_component_set_font_specification(component, &font) == BOXRIG_UI_BUSY);
    bool zoom_changed = true;
    CHECK(boxrig_wayland_terminal_component_visual_zoom(component, BOXRIG_FONT_ZOOM_INCREASE,
        boxrig_font_visual_step_policy_default(), &zoom_changed) == BOXRIG_UI_BUSY);
    CHECK(!zoom_changed);
    wp_fractional_scale_v1_send_preferred_scale(server.fractional_scale_resource, 150);
    pump(&server);
    dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    CHECK(frame.frame_pending && frame.additional_dispatch);
    boxrig_wayland_component_diagnostic diagnostic;
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.scale_numerator == 120);
    CHECK(boxrig_wayland_terminal_component_take_layout_action(component, &action));
    CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
    CHECK(action.value.grid_resize.columns == allocation.width / metrics.cell_width);
    CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows,
        action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
    CHECK(boxrig_wayland_terminal_component_complete_grid_resize(component,
        action.revision, true) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_scrollbar_visibility(component,
        &visibility) == BOXRIG_UI_OK);
    CHECK(!visibility.visible && !visibility.pending && visibility.reserved_width == 0);
    /* Hiding cancels a drag, but its release must not reach the terminal. */
    wl_pointer_send_button(server.pointer_resources[0], 75, 15, 0x110, WL_POINTER_BUTTON_STATE_RELEASED);
    pump(&server);
    dispatch_context(context);
    CHECK(capture.count == 0);
    pixels = server.shm_mapping;
    CHECK((pixels[allocation.width - 1] & UINT32_C(0xffffff)) ==
        (pixels[allocation.width - bar_width - 1] & UINT32_C(0xffffff)));
    CHECK(!boxrig_wayland_terminal_component_take_layout_action(component, &action));
    CHECK(boxrig_wayland_terminal_component_get_layout(component, &allocation, &layout) == BOXRIG_UI_OK);
    CHECK(layout.columns == allocation.width / metrics.cell_width);
    boxrig_scrollbar_state after;
    CHECK(boxrig_wayland_terminal_component_get_scrollbar_state(component, &after) == BOXRIG_UI_OK);
    CHECK(after.model.history_rows == before.model.history_rows);

    CHECK(boxrig_wayland_terminal_component_set_scrollbar_visibility(component,
        BOXRIG_UI_SCROLLBAR_AUTO) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_take_layout_action(component, &action));
    CHECK(action.value.grid_resize.columns == terminal_config.columns);
    CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows,
        action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
    CHECK(boxrig_wayland_terminal_component_complete_grid_resize(component,
        action.revision, true) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_scrollbar_visibility(component,
        &visibility) == BOXRIG_UI_OK);
    CHECK(visibility.visible && !visibility.pending && visibility.reserved_width == bar_width);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_wayland_terminal_component_take_layout_action(component, &action));
    unsigned int buffer_width = (allocation.width * 150 + 119) / 120;
    unsigned int buffer_bar_width = (bar_width * 150 + 119) / 120;
    CHECK(action.value.grid_resize.columns == (buffer_width - buffer_bar_width) / metrics.cell_width);
    CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows,
        action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
    CHECK(boxrig_wayland_terminal_component_complete_grid_resize(component,
        action.revision, true) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_scrollbar_visibility(component,
        &visibility) == BOXRIG_UI_OK && visibility.reserved_width == bar_width && !visibility.pending);
    CHECK(boxrig_wayland_terminal_component_set_visible(component, false) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_scrollbar_visibility(component,
        &visibility) == BOXRIG_UI_OK && visibility.visible);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[?1049h", 8, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_take_layout_action(component, &action));
    CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows,
        action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
    CHECK(boxrig_wayland_terminal_component_complete_grid_resize(component,
        action.revision, true) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_scrollbar_visibility(component,
        &visibility) == BOXRIG_UI_OK && !visibility.visible && !visibility.pending);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[?1049l", 8, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_take_layout_action(component, &action));
    CHECK(boxrig_wayland_terminal_component_complete_grid_resize(component,
        action.revision, false) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_scrollbar_visibility(component,
        &visibility) == BOXRIG_UI_OK && !visibility.visible && !visibility.pending);
    boxrig_wayland_terminal_component_destroy(component);
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NATIVE_CHILD;
    CHECK(boxrig_wayland_terminal_component_create(context, parent, model,
        &config, &component) == BOXRIG_UI_INVALID_ARGUMENT);
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    CHECK(boxrig_wayland_terminal_component_create(context, parent, model,
        &config, &component) == BOXRIG_UI_OK);
    for (int policy = BOXRIG_UI_SCROLLBAR_ALWAYS; policy <= BOXRIG_UI_SCROLLBAR_NEVER; policy++) {
        CHECK(boxrig_wayland_terminal_component_set_scrollbar_visibility(component, policy) == BOXRIG_UI_OK);
        CHECK(boxrig_wayland_terminal_component_get_scrollbar_visibility(component, &visibility) == BOXRIG_UI_OK);
        CHECK(visibility.requested == (boxrig_ui_scrollbar_visibility)policy &&
            !visibility.visible && !visibility.pending && visibility.reserved_width == 0);
    }
    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
    puts("PASS native Wayland standard scrollbar visibility and resize transaction");
}

/* These cases are appended to test_wayland_context.c and use its real client
 * and protocol fixture. The runner owns a separate IBus daemon and engine. */

static void send_ibus_keymap(fake_server *server)
{
    struct xkb_context *xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = {.layout = "us"};
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(xkb, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    CHECK(keymap != NULL);
    char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    CHECK(text != NULL);
    char path[] = "/tmp/boxrig-ime-keymap-XXXXXX";
    int descriptor = mkstemp(path);
    CHECK(descriptor >= 0 && unlink(path) == 0);
    size_t length = strlen(text) + 1;
    for (size_t offset = 0; offset < length;) {
        ssize_t count = write(descriptor, text + offset, length - offset);
        CHECK(count > 0);
        offset += (size_t)count;
    }
    wl_keyboard_send_keymap(server->keyboard_resources[0], WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
        descriptor, (uint32_t)length);
    close(descriptor);
    free(text);
    xkb_keymap_unref(keymap);
    xkb_context_unref(xkb);
}

static void ibus_test_poll(boxrig_wayland_context *context)
{
    const struct pollfd *sources;
    size_t count;
    int timeout = 60000;
    CHECK(boxrig_wayland_context_prepare_ime_poll(context, &sources, &count, &timeout) == BOXRIG_UI_OK);
    CHECK(count != 0 || timeout == 0);
    struct pollfd *descriptors = calloc(count == 0 ? 1 : count, sizeof(*descriptors));
    CHECK(descriptors != NULL);
    if (count != 0) memcpy(descriptors, sources, count * sizeof(*descriptors));
    int ready;
    do { ready = poll(descriptors, (nfds_t)count, timeout); } while (ready < 0 && errno == EINTR);
    if (getenv("BOXRIG_TEST_TRACE_IBUS") != NULL) fprintf(stderr, "IBus poll: count=%zu timeout=%d ready=%d\n", count, timeout, ready);
    CHECK(ready >= 0);
    CHECK(ready != 0 || timeout != 60000);
    CHECK(boxrig_wayland_context_dispatch_ime_poll(context, descriptors, count) == BOXRIG_UI_OK);
    free(descriptors);
}

static void ibus_test_drain(fake_server *server, boxrig_wayland_context *context,
    boxrig_wayland_terminal_component *component)
{
    for (;;) {
        pump(server);
        dispatch_context(context);
        boxrig_wayland_context_diagnostic bus;
        boxrig_wayland_component_diagnostic input;
        CHECK(boxrig_wayland_context_get_diagnostic(context, &bus) == BOXRIG_UI_OK);
        CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &input) == BOXRIG_UI_OK);
        CHECK(input.failure_reason == BOXRIG_WAYLAND_FAILURE_NONE);
        if (getenv("BOXRIG_TEST_TRACE_IBUS") != NULL) fprintf(stderr, "IBus drain: state=%d contexts=%zu pending=%zu depth=%zu waiting=%d\n",
            bus.direct.state, bus.direct.contexts, bus.direct.pending, input.input_queue_depth, input.ime_waiting);
        CHECK(bus.direct.state == BOXRIG_IME_CONNECTING || bus.direct.state == BOXRIG_IME_READY);
        if (bus.direct.state == BOXRIG_IME_READY && bus.direct.contexts == 1 &&
            bus.direct.pending == 0 && input.input_queue_depth == 0) break;
        ibus_test_poll(context);
    }
}

static void ibus_test_key(fake_server *server, uint32_t key)
{
    static uint32_t serial = 100;
    wl_keyboard_send_key(server->keyboard_resources[0], serial++, 0, key, WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_keyboard_send_key(server->keyboard_resources[0], serial++, 0, key, WL_KEYBOARD_KEY_STATE_RELEASED);
}

static void check_ibus_preedit(boxrig_wayland_terminal_component *component,
    const char *text, size_t start, size_t end, bool underline)
{
    boxrig_composition *composition = boxrig_wayland_terminal_component_composition(component);
    boxrig_composition_view view;
    CHECK(boxrig_composition_get_view(composition, &view) == BOXRIG_COMPOSITION_OK);
    CHECK(view.active && view.text_length == strlen(text) && memcmp(view.text, text, strlen(text)) == 0);
    CHECK(view.selection_offset == start && view.selection_length == end - start);
    for (size_t index = 0; index < view.cluster_count; index++) {
        boxrig_composition_cluster_view cluster;
        CHECK(boxrig_composition_get_cluster(composition, index, &cluster) == BOXRIG_COMPOSITION_OK);
        bool selected = cluster.text_offset >= start && cluster.text_offset < end;
        CHECK(cluster.selected == selected);
        CHECK(((cluster.feedback & BOXRIG_COMPOSITION_FEEDBACK_TARGET) != 0) == selected);
        CHECK(((cluster.feedback & BOXRIG_COMPOSITION_FEEDBACK_REVERSE) != 0) == selected);
        CHECK((cluster.feedback & BOXRIG_COMPOSITION_FEEDBACK_HIGHLIGHT) == 0);
        CHECK(((cluster.feedback & BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE) != 0) == underline);
    }
}

static void check_ibus_inactive(boxrig_wayland_terminal_component *component)
{
    boxrig_composition_view view;
    CHECK(boxrig_composition_get_view(boxrig_wayland_terminal_component_composition(component), &view)
        == BOXRIG_COMPOSITION_OK && !view.active);
}

static void check_ibus_output(boxrig_terminal *terminal, const char *expected)
{
    const uint8_t *output;
    size_t length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(length == strlen(expected));
    CHECK(length == 0 || memcmp(output, expected, length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
}

static GVariant *ibus_test_state(GDBusConnection *connection)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(connection, "org.boxrig.IMEProbe",
        "/org/boxrig/IMEProbe", "org.boxrig.IMEProbe", "GetState", NULL,
        G_VARIANT_TYPE("(suuuuuiiiiu)"), G_DBUS_CALL_FLAGS_NONE, 60000, NULL, &error);
    if (error != NULL) fprintf(stderr, "IBus probe: %s\n", error->message);
    CHECK(reply != NULL);
    return reply;
}

static void test_ibus_route_selection(void)
{
    const struct { boxrig_wayland_input_method policy; uint32_t version;
        bool ibus; const char *modifiers; bool unavailable; } cases[] = {
        {BOXRIG_WAYLAND_IME_WAYLAND, 1, false, "@im=ibus", false},
        {BOXRIG_WAYLAND_IME_AUTO, 2, false, "@im=ibus", false},
        {BOXRIG_WAYLAND_IME_AUTO, 1, true, "@im=ibus", false},
        {BOXRIG_WAYLAND_IME_AUTO, 1, true, "", false},
        {BOXRIG_WAYLAND_IME_AUTO, 1, false, "@im=fcitx", true},
        {BOXRIG_WAYLAND_IME_AUTO, 1, true, NULL, false},
        {BOXRIG_WAYLAND_IME_IBUS, 2, true, "gtk-im-context-simple", false},
        {BOXRIG_WAYLAND_IME_IBUS, 1, false, "@im=ibus", true},
    };
    GError *error = NULL;
    GDBusConnection *probe = g_dbus_connection_new_for_address_sync(getenv("IBUS_ADDRESS"),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);
    CHECK(probe != NULL && error == NULL);
    char *address = strdup(getenv("IBUS_ADDRESS"));
    char *module = strdup(getenv("GTK_IM_MODULE"));
    char *modifiers = strdup(getenv("XMODIFIERS"));
    CHECK(address != NULL && module != NULL && modifiers != NULL);
    for (int candidate_policy = BOXRIG_UI_CANDIDATE_AUTO; candidate_policy <= BOXRIG_UI_CANDIDATE_CLIENT; candidate_policy++)
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        bool selected = cases[index].ibus || (cases[index].policy == BOXRIG_WAYLAND_IME_AUTO &&
            cases[index].version >= 2 && candidate_policy == BOXRIG_UI_CANDIDATE_CLIENT &&
            !cases[index].unavailable);
        if (getenv("BOXRIG_TEST_TRACE_IBUS") != NULL) fprintf(stderr, "IBus route case %zu\n", index);
        CHECK(setenv("IBUS_ADDRESS", cases[index].unavailable ? "unix:path=/dev/null" : address, 1) == 0);
        CHECK(cases[index].modifiers == NULL ? unsetenv("XMODIFIERS") == 0 :
            setenv("XMODIFIERS", cases[index].modifiers, 1) == 0);
        /* Native Wayland must ignore GTK-only module configuration. */
        CHECK(setenv("GTK_IM_MODULE", "fcitx", 1) == 0);
        fake_server server;
        host_registry_state host;
        create_server(&server, true, false, true);
        wl_global_destroy(server.text_input_manager);
        server.text_input_manager = wl_global_create(server.server_display,
            &zwp_text_input_manager_v3_interface, cases[index].version, &server, bind_text_input_manager);
        struct wl_registry *registry = create_host_registry(&server, &host);
        boxrig_wayland_context_config config = boxrig_wayland_context_config_default();
        CHECK(config.input_method == BOXRIG_WAYLAND_IME_AUTO);
        config.input_method = cases[index].policy;
        CHECK(config.candidate_policy == BOXRIG_UI_CANDIDATE_AUTO);
        config.candidate_policy = (boxrig_ui_candidate_policy)candidate_policy;
        boxrig_wayland_context *context = NULL;
        CHECK(boxrig_wayland_context_create(server.client_display, server.client_queue, &config, &context) == BOXRIG_UI_OK);
        pump(&server); pump(&server); pump(&server);
        boxrig_terminal *terminal = NULL;
        boxrig_ui_model *model = NULL;
        boxrig_wayland_terminal_component *component = NULL;
        boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
        CHECK(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
        struct wl_surface *parent = wl_compositor_create_surface(host.compositor);
        wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
        CHECK(create_steady_component(context, parent, model, NULL, &component) == BOXRIG_UI_OK);
        pump(&server);
        send_ibus_keymap(&server);
        struct wl_array keys;
        wl_array_init(&keys);
        wl_keyboard_send_enter(server.keyboard_resources[0], 10, server.child_resource, &keys);
        zwp_text_input_v3_send_enter(server.text_input_resources[0], server.child_resource);
        if (cases[index].unavailable) {
            for (;;) {
                pump(&server); dispatch_context(context);
                boxrig_wayland_context_diagnostic state;
                CHECK(boxrig_wayland_context_get_diagnostic(context, &state) == BOXRIG_UI_OK);
                if (state.direct.state == BOXRIG_IME_UNAVAILABLE && state.direct.pending == 0) break;
                ibus_test_poll(context);
            }
            ibus_test_key(&server, KEY_A);
            pump(&server); dispatch_context(context); pump(&server);
            check_ibus_output(terminal, "a");
        } else if (selected) {
            ibus_test_key(&server, KEY_F1);
            ibus_test_drain(&server, context, component);
            check_ibus_preedit(component, "記者の汽車が記者で記者した", 0, 9, true);
            check_ibus_output(terminal, "");
        } else {
            pump(&server); dispatch_context(context); pump(&server);
        }
        boxrig_wayland_context_diagnostic diagnostic;
        CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) == BOXRIG_UI_OK);
        CHECK(diagnostic.input_method == (selected ? BOXRIG_WAYLAND_IME_IBUS : BOXRIG_WAYLAND_IME_WAYLAND));
        CHECK(diagnostic.direct.state == (cases[index].unavailable ? BOXRIG_IME_UNAVAILABLE :
            selected ? BOXRIG_IME_READY : BOXRIG_IME_DISABLED));
        CHECK(((diagnostic.input_capabilities & BOXRIG_WAYLAND_INPUT_CAPABILITY_IBUS) != 0) == selected);
        CHECK(server.text_current_enabled[0] == !selected);
        boxrig_wayland_component_diagnostic candidate_state;
        CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &candidate_state) == BOXRIG_UI_OK);
        CHECK(candidate_state.candidate_policy == (boxrig_ui_candidate_policy)candidate_policy);
        bool client = selected && candidate_policy != BOXRIG_UI_CANDIDATE_SYSTEM;
        CHECK(candidate_state.candidate_owner == (client ? BOXRIG_UI_CANDIDATE_OWNER_CLIENT : BOXRIG_UI_CANDIDATE_OWNER_SYSTEM));
        CHECK(candidate_state.candidate_reason == (candidate_policy == BOXRIG_UI_CANDIDATE_SYSTEM ? BOXRIG_UI_CANDIDATE_SYSTEM_REQUEST :
            cases[index].unavailable ? BOXRIG_UI_CANDIDATE_IME_UNAVAILABLE :
            client ? BOXRIG_UI_CANDIDATE_READY : BOXRIG_UI_CANDIDATE_STANDARD_INPUT));
        CHECK(candidate_state.candidate_count == 0 && !candidate_state.candidate_visible);
        if (selected) {
            GVariant *reply = g_dbus_connection_call_sync(probe, "org.boxrig.IMEProbe",
                "/org/boxrig/IMEProbe", "org.boxrig.IMEProbe", "GetCandidates", NULL,
                G_VARIANT_TYPE("(uuu)"), G_DBUS_CALL_FLAGS_NONE, 60000, NULL, &error);
            CHECK(reply != NULL && error == NULL);
            guint caps, panel_lookup, panel_auxiliary;
            g_variant_get(reply, "(uuu)", &caps, &panel_lookup, &panel_auxiliary);
            g_variant_unref(reply);
            CHECK((caps & 6U) == (client ? 6U : 0U));
        }
        wl_array_release(&keys);
        boxrig_wayland_terminal_component_destroy(component);
        wl_surface_destroy(parent);
        wl_compositor_destroy(host.compositor);
        wl_registry_destroy(registry);
        pump(&server);
        boxrig_wayland_context_destroy(context);
        boxrig_ui_model_destroy(model);
        boxrig_terminal_destroy(terminal);
        destroy_server(&server);
    }
    CHECK(setenv("IBUS_ADDRESS", address, 1) == 0 && setenv("GTK_IM_MODULE", module, 1) == 0);
    CHECK(setenv("XMODIFIERS", modifiers, 1) == 0);
    free(address); free(module); free(modifiers);
    g_object_unref(probe);
    puts("PASS native Wayland explicit and automatic IME route selection with unavailable fallback");
}

static void test_ibus_native_preedit(void)
{
    CHECK(getenv("IBUS_ADDRESS") != NULL && getenv("BOXRIG_TEST_IBUS_ENGINE_PID") != NULL);
    GError *error = NULL;
    GDBusConnection *probe = g_dbus_connection_new_for_address_sync(getenv("IBUS_ADDRESS"),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);
    CHECK(probe != NULL);
    fake_server server;
    host_registry_state host;
    create_server(&server, true, true, true);
    struct wl_registry *registry = create_host_registry(&server, &host);
    boxrig_wayland_context_config config = boxrig_wayland_context_config_default();
    config.input_method = BOXRIG_WAYLAND_IME_IBUS;
    boxrig_wayland_context *context = NULL;
    CHECK(boxrig_wayland_context_create(server.client_display, server.client_queue, &config, &context) == BOXRIG_UI_OK);
    pump(&server); pump(&server); pump(&server);
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    terminal_config.rows = 4; terminal_config.columns = 40;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    struct wl_surface *parent = wl_compositor_create_surface(host.compositor);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    boxrig_wayland_terminal_component *component = NULL;
    CHECK(create_steady_component(context, parent, model, NULL, &component) == BOXRIG_UI_OK);
    pump(&server);
    send_ibus_keymap(&server);
    struct wl_array keys;
    wl_array_init(&keys);
    wl_keyboard_send_enter(server.keyboard_resources[0], 10, server.child_resource, &keys);
    zwp_text_input_v3_send_enter(server.text_input_resources[0], server.child_resource);
    /* Queue keys during connection setup: consumed IME keys must not escape
     * as raw terminal text, and replies must preserve their original order. */
    ibus_test_key(&server, KEY_F1);
    ibus_test_drain(&server, context, component);
    check_ibus_preedit(component, "記者の汽車が記者で記者した", 0, 9, true);
    check_ibus_output(terminal, "");
    CHECK(!server.text_current_enabled[0]);
    GVariant *candidate_reply = g_dbus_connection_call_sync(probe, "org.boxrig.IMEProbe",
        "/org/boxrig/IMEProbe", "org.boxrig.IMEProbe", "GetCandidates", NULL,
        G_VARIANT_TYPE("(uuu)"), G_DBUS_CALL_FLAGS_NONE, 60000, NULL, &error);
    CHECK(candidate_reply != NULL && error == NULL);
    guint candidate_caps, panel_lookup, panel_auxiliary;
    g_variant_get(candidate_reply, "(uuu)", &candidate_caps, &panel_lookup, &panel_auxiliary);
    g_variant_unref(candidate_reply);
    CHECK((candidate_caps & 6U) == 6U);
    for (guint action = 1; action <= 3; action++) {
        candidate_reply = g_dbus_connection_call_sync(probe, "org.boxrig.IMEProbe",
            "/org/boxrig/IMEProbe", "org.boxrig.IMEProbe", "Candidates", g_variant_new("(u)", action),
            G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, 60000, NULL, &error);
        CHECK(candidate_reply != NULL && error == NULL);
        g_variant_unref(candidate_reply);
        boxrig_ui_candidate_view candidates;
        for (;;) {
            pump(&server); dispatch_context(context);
            CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component),
                &candidates) == BOXRIG_UI_OK);
            if (candidates.snapshot.count == 5 && candidates.snapshot.visible == (action != 2) &&
                candidates.snapshot.auxiliary_visible == (action != 2)) break;
            ibus_test_poll(context);
        }
        CHECK(candidates.snapshot.page_size == 3 && candidates.snapshot.cursor == 1);
        CHECK(strcmp(candidates.snapshot.items[1], "汽車") == 0);
        CHECK(strcmp(candidates.snapshot.labels[1], "s") == 0);
        CHECK(strcmp(candidates.snapshot.auxiliary, "説明🙂\n次の行") == 0);
        check_ibus_output(terminal, "");
    }
    const uint32_t movements[] = {KEY_RIGHT, KEY_RIGHT, KEY_RIGHT, KEY_LEFT, KEY_LEFT};
    const size_t starts[] = {9, 18, 27, 18, 9}, ends[] = {18, 27, 39, 27, 18};
    for (size_t index = 0; index < 5; index++) {
        ibus_test_key(&server, movements[index]);
        ibus_test_drain(&server, context, component);
        check_ibus_preedit(component, "記者の汽車が記者で記者した", starts[index], ends[index], true);
        check_ibus_output(terminal, "");
    }
    wl_keyboard_send_modifiers(server.keyboard_resources[0], 20, 1, 0, 0, 0);
    ibus_test_key(&server, KEY_LEFT);
    ibus_test_drain(&server, context, component);
    check_ibus_preedit(component, "記者の汽車が記者で記者した", 9, 15, true);
    ibus_test_key(&server, KEY_RIGHT); ibus_test_key(&server, KEY_RIGHT);
    ibus_test_drain(&server, context, component);
    check_ibus_preedit(component, "記者の汽車が記者で記者した", 9, 21, true);
    wl_keyboard_send_modifiers(server.keyboard_resources[0], 21, 0, 0, 0, 0);
    ibus_test_key(&server, KEY_F2);
    ibus_test_drain(&server, context, component);
    check_ibus_preedit(component, "A記者の🙂汽車がZ", 14, 23, true);
    ibus_test_key(&server, KEY_F4);
    ibus_test_drain(&server, context, component);
    check_ibus_preedit(component, "A語B", 1, 4, true);
    ibus_test_key(&server, KEY_F3);
    ibus_test_drain(&server, context, component);
    check_ibus_preedit(component, "abc", 0, 0, false);
    ibus_test_key(&server, KEY_F5);
    ibus_test_drain(&server, context, component);
    check_ibus_inactive(component);
    ibus_test_key(&server, KEY_F6);
    ibus_test_drain(&server, context, component);
    check_ibus_preedit(component, "abc", 0, 0, false);
    ibus_test_key(&server, KEY_F7);
    ibus_test_drain(&server, context, component);
    check_ibus_inactive(component);
    const uint32_t cancels[] = {KEY_BACKSPACE, KEY_ESC};
    for (size_t index = 0; index < 2; index++) {
        ibus_test_key(&server, KEY_F1); ibus_test_key(&server, cancels[index]);
        ibus_test_key(&server, KEY_A);
        ibus_test_drain(&server, context, component);
        check_ibus_inactive(component);
        check_ibus_output(terminal, "a");
    }
    ibus_test_key(&server, KEY_F1); ibus_test_key(&server, KEY_ENTER); ibus_test_key(&server, KEY_A);
    ibus_test_drain(&server, context, component);
    check_ibus_inactive(component);
    check_ibus_output(terminal, "記者の汽車が記者で記者したa");
    ibus_test_key(&server, KEY_F8); ibus_test_key(&server, KEY_F12);
    ibus_test_drain(&server, context, component);
    check_ibus_output(terminal, "a一二");
    key_route_capture routed = {0};
    CHECK(boxrig_wayland_terminal_component_set_key_router(component, capture_key_route, &routed) == BOXRIG_UI_OK);
    ibus_test_key(&server, KEY_1);
    ibus_test_drain(&server, context, component);
    CHECK(routed.count == 2 && routed.events[0].action == BOXRIG_KEY_PRESS && routed.events[1].action == BOXRIG_KEY_RELEASE);
    check_ibus_output(terminal, "");
    CHECK(boxrig_wayland_terminal_component_set_key_router(component, NULL, NULL) == BOXRIG_UI_OK);
    /* Unhandled keys still participate in XKB Compose, in their original
     * order even when the entire sequence arrives before the first reply. */
    send_keymap(&server, 0);
    ibus_test_key(&server, 58); /* Multi_key in the small fixture keymap. */
    ibus_test_key(&server, 31); /* Apostrophe. */
    ibus_test_key(&server, 18); /* e. */
    ibus_test_drain(&server, context, component);
    check_ibus_output(terminal, "é");
    send_ibus_keymap(&server);
    /* Repeats use the same asynchronous IME route as a physical press. */
    CHECK(boxrig_wayland_context_update_monotonic_time(context, 0) == BOXRIG_UI_OK);
    wl_keyboard_send_repeat_info(server.keyboard_resources[0], 10, 1);
    ibus_test_key(&server, KEY_F1);
    wl_keyboard_send_key(server.keyboard_resources[0], 22, 0, KEY_RIGHT, WL_KEYBOARD_KEY_STATE_PRESSED);
    ibus_test_drain(&server, context, component);
    check_ibus_preedit(component, "記者の汽車が記者で記者した", 9, 18, true);
    CHECK(boxrig_wayland_context_update_monotonic_time(context, 1) == BOXRIG_UI_OK);
    ibus_test_drain(&server, context, component);
    check_ibus_preedit(component, "記者の汽車が記者で記者した", 18, 27, true);
    wl_keyboard_send_key(server.keyboard_resources[0], 23, 0, KEY_RIGHT, WL_KEYBOARD_KEY_STATE_RELEASED);
    wl_keyboard_send_repeat_info(server.keyboard_resources[0], 0, 0);
    ibus_test_drain(&server, context, component);
    check_ibus_output(terminal, "");

    /* Selection comes from attributes; the caret remains independently at
     * the start of the selected clause after the composition is laid out. */
    boxrig_font_effective_metrics metrics;
    CHECK(boxrig_wayland_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_wayland_terminal_component_set_ime_window_offset(component, 11, 27) == BOXRIG_UI_OK);
    boxrig_ui_component_allocation allocation = {.width = 40 * metrics.cell_width,
        .height = 4 * metrics.cell_height, .x = 7, .y = 9, .window_state = BOXRIG_UI_WINDOW_EMBEDDED};
    boxrig_ui_layout_result layout;
    CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    boxrig_ui_component_frame_result frame;
    boxrig_wayland_host_action action;
    bool applied;
    if (boxrig_wayland_terminal_component_take_host_action(component, &action)) {
        wl_surface_commit(parent);
        CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component,
            action.revision, &applied) == BOXRIG_UI_OK && applied);
    }
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    pump(&server);
    ibus_test_key(&server, KEY_F1); ibus_test_key(&server, KEY_RIGHT);
    ibus_test_drain(&server, context, component);
    for (size_t index = 0; index < server.frame_callback_count; index++)
        if (server.frame_callbacks[index] != NULL) complete_frame(&server, index);
    for (size_t index = 0; index < server.shm_buffer_create_count; index++)
        if (server.shm_buffer_busy[index]) release_buffer(&server, index);
    pump(&server);
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    pump(&server);
    /* A following ProcessKeyEvent is an IPC barrier for the rectangle. */
    ibus_test_key(&server, KEY_F3);
    ibus_test_drain(&server, context, component);
    GVariant *state = ibus_test_state(probe);
    const gchar *surrounding;
    guint cursor, anchor, caps, presses, releases, rectangles;
    gint x, y, width, height;
    g_variant_get(state, "(&suuuuuiiiiu)", &surrounding, &cursor, &anchor, &caps,
        &presses, &releases, &x, &y, &width, &height, &rectangles);
    CHECK(x == (gint)(6 * metrics.cell_width + 7 + 11) && y == 9 + 27);
    CHECK(width > 0 && height == (gint)metrics.cell_height && rectangles > 0);
    CHECK((caps & IBUS_CAP_PREEDIT_TEXT) != 0 && (caps & IBUS_CAP_SURROUNDING_TEXT) == 0);
    CHECK(presses != 0 && presses == releases + 1);
    CHECK(strcmp(surrounding, "") == 0);
    g_variant_unref(state);
    ibus_test_key(&server, KEY_ESC);
    ibus_test_drain(&server, context, component);

    boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(
        boxrig_wayland_terminal_component_view(component));
    editable_owner owner = {.disposition = BOXRIG_UI_OK};
    CHECK(boxrig_ui_editable_document_set_apply_handler(document, apply_editable, &owner) == BOXRIG_UI_OK);
    publish_editable(document, &owner, "前かな後", 9, 9);
    ibus_test_key(&server, KEY_F10);
    ibus_test_drain(&server, context, component);
    CHECK(owner.accepted == 1 && strcmp(owner.text, "前か語後") == 0);
    check_ibus_output(terminal, "");
    ibus_test_key(&server, KEY_F11);
    ibus_test_drain(&server, context, component);
    CHECK(owner.accepted == 1 && strcmp(owner.text, "前か語後") == 0);
    check_ibus_output(terminal, "");
    owner.disposition = BOXRIG_UI_UNAVAILABLE;
    ibus_test_key(&server, KEY_F10);
    ibus_test_drain(&server, context, component);
    CHECK(owner.accepted == 1 && strcmp(owner.text, "前か語後") == 0);
    owner.disposition = BOXRIG_UI_BUSY;
    unsigned int calls = owner.calls;
    ibus_test_key(&server, KEY_F12);
    for (;;) {
        pump(&server); dispatch_context(context);
        if (owner.calls != calls) break;
        ibus_test_poll(context);
    }
    CHECK(owner.accepted == 1 && strcmp(owner.text, "前か語後") == 0);
    owner.disposition = BOXRIG_UI_OK;
    ibus_test_drain(&server, context, component);
    CHECK(strcmp(owner.text, "前か語一二後") == 0);
    check_ibus_output(terminal, "");
    CHECK(boxrig_ui_editable_document_set_apply_handler(document, NULL, NULL) == BOXRIG_UI_OK);
    publish_editable(document, &owner, "readonly", 8, 8);
    ibus_test_key(&server, KEY_F10);
    ibus_test_drain(&server, context, component);
    CHECK(strcmp(owner.text, "readonly") == 0);
    check_ibus_output(terminal, "");
    CHECK(boxrig_ui_editable_document_set_apply_handler(document, apply_editable, &owner) == BOXRIG_UI_OK);
    /* An empty COMMIT-mode preedit is not a request to delete the document's
     * selection when focus leaves. Only actual engine commits can do that. */
    publish_editable(document, &owner, "前選択後", 9, 3);
    unsigned int accepted_before_focus = owner.accepted;
    wl_keyboard_send_modifiers(server.keyboard_resources[0], 24, 1, 0, 0, 0);
    ibus_test_key(&server, KEY_F9);
    wl_keyboard_send_modifiers(server.keyboard_resources[0], 25, 0, 0, 0, 0);
    ibus_test_drain(&server, context, component);
    check_ibus_inactive(component);
    candidate_reply = g_dbus_connection_call_sync(probe, "org.boxrig.IMEProbe",
        "/org/boxrig/IMEProbe", "org.boxrig.IMEProbe", "Candidates", g_variant_new("(u)", 1U),
        G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, 60000, NULL, &error);
    CHECK(candidate_reply != NULL && error == NULL);
    g_variant_unref(candidate_reply);
    boxrig_ui_candidate_view focused_candidates;
    for (;;) {
        pump(&server); dispatch_context(context);
        CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component),
            &focused_candidates) == BOXRIG_UI_OK);
        if (focused_candidates.snapshot.visible && focused_candidates.snapshot.count == 5) break;
        ibus_test_poll(context);
    }
    wl_keyboard_send_leave(server.keyboard_resources[0], 26, server.child_resource);
    pump(&server); dispatch_context(context);
    CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component),
        &focused_candidates) == BOXRIG_UI_OK);
    CHECK(focused_candidates.snapshot.count == 0 && !focused_candidates.snapshot.visible);
    CHECK(strcmp(owner.text, "前選択後") == 0 && owner.accepted == accepted_before_focus);
    check_ibus_output(terminal, "");
    wl_keyboard_send_enter(server.keyboard_resources[0], 27, server.child_resource, &keys);
    ibus_test_drain(&server, context, component);

    CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component),
        &focused_candidates) == BOXRIG_UI_OK);
    CHECK(focused_candidates.snapshot.count == 0 && !focused_candidates.snapshot.visible);

    /* Losing focus while another key is in flight still commits the already
     * published COMMIT-mode preedit once to its original document. */
    publish_editable(document, &owner, "前後", 3, 3);
    accepted_before_focus = owner.accepted;
    ibus_test_key(&server, KEY_F9);
    ibus_test_drain(&server, context, component);
    pid_t stopped_engine = (pid_t)strtol(getenv("BOXRIG_TEST_IBUS_ENGINE_PID"), NULL, 10);
    CHECK(stopped_engine > 1 && kill(stopped_engine, SIGSTOP) == 0);
    ibus_test_key(&server, KEY_F2);
    pump(&server); dispatch_context(context);
    wl_keyboard_send_leave(server.keyboard_resources[0], 28, server.child_resource);
    wl_keyboard_send_enter(server.keyboard_resources[0], 29, server.child_resource, &keys);
    pump(&server); dispatch_context(context);
    CHECK(kill(stopped_engine, SIGCONT) == 0);
    ibus_test_drain(&server, context, component);
    CHECK(strcmp(owner.text, "前確定後") == 0 && owner.accepted == accepted_before_focus + 1);
    check_ibus_output(terminal, "");
    check_ibus_inactive(component);

    publish_editable(document, &owner, "前後", 3, 3);
    ibus_test_key(&server, KEY_F9);
    ibus_test_drain(&server, context, component);
    wl_keyboard_send_leave(server.keyboard_resources[0], 28, server.child_resource);
    pump(&server); dispatch_context(context);
    CHECK(strcmp(owner.text, "前確定後") == 0);
    check_ibus_output(terminal, "");
    check_ibus_inactive(component);
    wl_keyboard_send_enter(server.keyboard_resources[0], 29, server.child_resource, &keys);
    ibus_test_drain(&server, context, component);
    CHECK(boxrig_ui_editable_document_clear(document) == BOXRIG_UI_OK);
    ibus_test_key(&server, KEY_F9);
    ibus_test_drain(&server, context, component);
    wl_keyboard_send_leave(server.keyboard_resources[0], 30, server.child_resource);
    pump(&server); dispatch_context(context);
    check_ibus_inactive(component);
    check_ibus_output(terminal, "確定");
    wl_keyboard_send_enter(server.keyboard_resources[0], 31, server.child_resource, &keys);
    ibus_test_key(&server, KEY_A);
    ibus_test_drain(&server, context, component);
    check_ibus_output(terminal, "a");

    /* Drop both already received and still undispatched engine commits when
     * input is stopped; the next context must accept its own fresh preedit. */
    for (unsigned int boundary = 0; boundary < 2; boundary++) {
        GVariant *commit_reply = g_dbus_connection_call_sync(probe, "org.boxrig.IMEProbe",
            "/org/boxrig/IMEProbe", "org.boxrig.IMEProbe", "CommitText", g_variant_new("(s)", "old"),
            G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, 60000, NULL, &error);
        CHECK(commit_reply != NULL && error == NULL);
        g_variant_unref(commit_reply);
        if (boundary == 0) {
            boxrig_wayland_component_diagnostic input;
            do {
                ibus_test_poll(context);
                CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &input) == BOXRIG_UI_OK);
            } while (input.input_queue_depth == 0);
        }
        CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, false) == BOXRIG_UI_OK);
        check_ibus_inactive(component);
        CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, true) == BOXRIG_UI_OK);
        ibus_test_key(&server, KEY_F2);
        ibus_test_drain(&server, context, component);
        check_ibus_preedit(component, "A記者の🙂汽車がZ", 14, 23, true);
        check_ibus_output(terminal, "");
    }
    ibus_test_key(&server, KEY_F9);
    ibus_test_drain(&server, context, component);
    CHECK(kill(stopped_engine, SIGSTOP) == 0);
    ibus_test_key(&server, KEY_ENTER);
    pump(&server); dispatch_context(context);
    CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, false) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, true) == BOXRIG_UI_OK);
    ibus_test_key(&server, KEY_F2);
    pump(&server); dispatch_context(context);
    CHECK(kill(stopped_engine, SIGCONT) == 0);
    ibus_test_drain(&server, context, component);
    check_ibus_preedit(component, "A記者の🙂汽車がZ", 14, 23, true);
    check_ibus_output(terminal, "");

    pid_t daemon_pid = (pid_t)strtol(getenv("BOXRIG_TEST_IBUS_DAEMON_PID"), NULL, 10);
    pid_t engine_pid = (pid_t)strtol(getenv("BOXRIG_TEST_IBUS_ENGINE_PID"), NULL, 10);
    CHECK(daemon_pid > 1 && kill(daemon_pid, SIGTERM) == 0);
    for (;;) {
        boxrig_wayland_context_diagnostic diagnostic;
        pump(&server); dispatch_context(context);
        CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) == BOXRIG_UI_OK);
        if (diagnostic.direct.state == BOXRIG_IME_UNAVAILABLE) break;
        ibus_test_poll(context);
    }
    ibus_test_key(&server, KEY_A);
    pump(&server); dispatch_context(context); pump(&server);
    check_ibus_output(terminal, "a");
    CHECK(server.text_current_enabled[0]);
    CHECK(write(3, "restart\n", 8) == 8);
    char response[96];
    size_t offset = 0;
    while (offset < sizeof(response) - 1) {
        struct pollfd ready = {.fd = 4, .events = POLLIN};
        CHECK(poll(&ready, 1, 60000) == 1);
        CHECK(read(4, response + offset, 1) == 1);
        if (response[offset++] == '\n') break;
    }
    response[offset] = '\0';
    int new_engine, new_daemon;
    CHECK(sscanf(response, "ready %d %d", &new_engine, &new_daemon) == 2);
    engine_pid = (pid_t)new_engine;
    wl_keyboard_send_leave(server.keyboard_resources[0], 32, server.child_resource);
    wl_keyboard_send_enter(server.keyboard_resources[0], 33, server.child_resource, &keys);
    ibus_test_key(&server, KEY_F2);
    ibus_test_drain(&server, context, component);
    CHECK(!server.text_current_enabled[0]);
    check_ibus_preedit(component, "A記者の🙂汽車がZ", 14, 23, true);
    check_ibus_output(terminal, "");

    /* A delayed old-focus response must not replace the new composition. */
    CHECK(engine_pid > 1 && kill(engine_pid, SIGSTOP) == 0);
    ibus_test_key(&server, KEY_F1);
    pump(&server); dispatch_context(context);
    wl_keyboard_send_leave(server.keyboard_resources[0], 34, server.child_resource);
    wl_keyboard_send_enter(server.keyboard_resources[0], 35, server.child_resource, &keys);
    ibus_test_key(&server, KEY_F2);
    pump(&server); dispatch_context(context);
    CHECK(kill(engine_pid, SIGCONT) == 0);
    ibus_test_drain(&server, context, component);
    check_ibus_preedit(component, "A記者の🙂汽車がZ", 14, 23, true);
    check_ibus_output(terminal, "");

    /* Destroy with an outstanding key reply. No callback may retain a freed
     * component, inject the consumed key, or wait for the engine to resume. */
    CHECK(engine_pid > 1 && kill(engine_pid, SIGSTOP) == 0);
    ibus_test_key(&server, KEY_F1);
    pump(&server); dispatch_context(context);
    boxrig_wayland_component_diagnostic waiting;
    boxrig_wayland_dispatch_result dispatch;
    CHECK(boxrig_wayland_context_dispatch(context, BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT, &dispatch) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &waiting) == BOXRIG_UI_OK);
    CHECK(waiting.ime_waiting && !dispatch.additional_dispatch);
    boxrig_wayland_terminal_component_destroy(component);
    boxrig_wayland_context_destroy(context);
    context = NULL;
    CHECK(kill(engine_pid, SIGCONT) == 0);
    check_ibus_output(terminal, "");
    wl_array_release(&keys);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(registry);
    pump(&server);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
    g_object_unref(probe);
    puts("PASS native Wayland IBus clause attributes, cancellation, ordered keys and editable context");
}

static void test_native_candidate_pointer(void)
{
    fake_server server;
    host_registry_state host;
    create_server(&server, true, true, true);
    struct wl_registry *registry = create_host_registry(&server, &host);
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 16; config.columns = 40;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(server.client_display, server.client_queue,
        NULL, &context) == BOXRIG_UI_OK);
    pump(&server); pump(&server); pump(&server);
    struct wl_surface *parent = wl_compositor_create_surface(host.compositor);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    CHECK(create_steady_component(context, parent, model, NULL, &component) == BOXRIG_UI_OK);
    boxrig_font_effective_metrics metrics;
    CHECK(boxrig_wayland_terminal_component_get_effective_metrics(component,
        &metrics) == BOXRIG_FONT_CATALOG_OK);
    boxrig_ui_component_allocation allocation = {.width = metrics.cell_width * config.columns,
        .height = metrics.cell_height * config.rows, .window_state = BOXRIG_UI_WINDOW_EMBEDDED};
    boxrig_ui_layout_result layout;
    CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    pump(&server);
    size_t pixel_bytes = (size_t)server.shm_stride * server.shm_height;
    uint8_t *baseline = malloc(pixel_bytes);
    CHECK(baseline != NULL);
    memcpy(baseline, (uint8_t *)server.shm_mapping +
        server.shm_buffer_offsets[server.last_attached_buffer], pixel_bytes);
    boxrig_ui_view *view = boxrig_wayland_terminal_component_view(component);
    const char *items[] = {"記者", "汽車", "帰社"}, *labels[] = {"1", "2", "3"};
    boxrig_ui_candidate_snapshot snapshot = {.count = 3, .page_size = 3, .cursor = 1,
        .visible = true, .cursor_visible = true, .items = items, .labels = labels};
    boxrig_ui_candidate_view candidates;
    for (size_t stage = 0; stage < 3; stage++) {
        for (size_t index = 0; index < server.frame_callback_count; index++)
            if (server.frame_callbacks[index] != NULL) complete_frame(&server, index);
        for (size_t index = 0; index < server.shm_buffer_create_count; index++)
            if (server.shm_buffers[index] != NULL && server.shm_buffer_busy[index]) release_buffer(&server, index);
        pump(&server);
        if (stage == 1) boxrig_ui_view_clear_candidates(view);
        else CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_OK);
        boxrig_ui_component_frame_result frame;
        CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        pump(&server);
        CHECK(boxrig_ui_view_get_candidates(view, &candidates) == BOXRIG_UI_OK);
        const uint8_t *pixels = (uint8_t *)server.shm_mapping +
            server.shm_buffer_offsets[server.last_attached_buffer];
        CHECK((memcmp(baseline, pixels, pixel_bytes) == 0) == (stage == 1));
        CHECK((candidates.layout.bounds.width == 0) == (stage == 1));
        if (stage != 1) CHECK(candidates.layout.count == 3 && candidates.layout.revision == candidates.revision);
    }
    free(baseline);
    mouse_route_capture capture = {0};
    CHECK(boxrig_wayland_terminal_component_set_mouse_router(component,
        capture_mouse_route, &capture) == BOXRIG_UI_OK);
    size_t consumed;
    const char mouse_mode[] = "\x1b[?1000h\x1b[?1006h";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)mouse_mode, sizeof(mouse_mode) - 1,
        &consumed) == BOXRIG_STATUS_OK);
    int x = candidates.layout.bounds.x + 2, y = candidates.layout.bounds.y + 2;
    wl_pointer_send_enter(server.pointer_resources[0], 71, server.child_resource,
        wl_fixed_from_int(x), wl_fixed_from_int(y));
    wl_pointer_send_button(server.pointer_resources[0], 72, 10, BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
    wl_pointer_send_motion(server.pointer_resources[0], 11,
        wl_fixed_from_int(allocation.width - 30), wl_fixed_from_int(allocation.height - 10));
    wl_pointer_send_axis(server.pointer_resources[0], 12, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(1));
    wl_pointer_send_axis_value120(server.pointer_resources[0], WL_POINTER_AXIS_VERTICAL_SCROLL, 120);
    wl_pointer_send_frame(server.pointer_resources[0]);
    pump(&server); dispatch_context(context);
    CHECK(capture.count == 0);
    check_ibus_output(terminal, "");
    boxrig_ui_view_clear_candidates(view);
    wl_pointer_send_button(server.pointer_resources[0], 73, 13, BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED);
    pump(&server); dispatch_context(context);
    CHECK(capture.count == 0);
    check_ibus_output(terminal, "");
    /* A fresh terminal press/release must work after candidate capture ends. */
    wl_pointer_send_button(server.pointer_resources[0], 74, 14, BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
    wl_pointer_send_button(server.pointer_resources[0], 75, 15, BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED);
    pump(&server); dispatch_context(context);
    CHECK(capture.count == 2 && capture.events[0].action == BOXRIG_MOUSE_PRESS &&
        capture.events[1].action == BOXRIG_MOUSE_RELEASE);
    const uint8_t *output;
    size_t length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(length > 0 && output[0] == 0x1b);
    CHECK(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
    for (size_t index = 0; index < server.frame_callback_count; index++)
        if (server.frame_callbacks[index] != NULL) complete_frame(&server, index);
    for (size_t index = 0; index < server.shm_buffer_create_count; index++)
        if (server.shm_buffers[index] != NULL && server.shm_buffer_busy[index]) release_buffer(&server, index);
    pump(&server);
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_OK);
    boxrig_ui_component_frame_result frame;
    CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    pump(&server);
    /* A terminal-owned press still receives its release over the panel. */
    capture.count = 0;
    wl_pointer_send_button(server.pointer_resources[0], 76, 16, BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
    wl_pointer_send_motion(server.pointer_resources[0], 17, wl_fixed_from_int(x), wl_fixed_from_int(y));
    wl_pointer_send_button(server.pointer_resources[0], 77, 18, BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED);
    pump(&server); dispatch_context(context);
    CHECK(capture.count == 2 && capture.events[0].action == BOXRIG_MOUSE_PRESS &&
        capture.events[1].action == BOXRIG_MOUSE_RELEASE);
    length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(length > 0 && output[length - 1] == 'm');
    CHECK(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
    /* A lost pointer focus cancels capture even if its release goes elsewhere. */
    capture.count = 0;
    wl_pointer_send_button(server.pointer_resources[0], 78, 19, BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
    wl_pointer_send_leave(server.pointer_resources[0], 79, server.child_resource);
    wl_pointer_send_enter(server.pointer_resources[0], 80, server.child_resource,
        wl_fixed_from_int(allocation.width - 30), wl_fixed_from_int(allocation.height - 10));
    wl_pointer_send_button(server.pointer_resources[0], 81, 20, BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
    wl_pointer_send_button(server.pointer_resources[0], 82, 21, BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED);
    pump(&server); dispatch_context(context);
    CHECK(capture.count == 2 && capture.events[0].action == BOXRIG_MOUSE_PRESS &&
        capture.events[1].action == BOXRIG_MOUSE_RELEASE);
    length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(length > 0 && output[length - 1] == 'm');
    CHECK(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
    boxrig_wayland_component_diagnostic diagnostic;
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.pressed_button_count == 0 && diagnostic.input_error_count == 0);
    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
    puts("PASS native Wayland candidate pixels, erasure and complete pointer capture");
}

static void test_native_candidate_scale(void)
{
    fake_server server;
    host_registry_state host;
    create_server(&server, true, true, true);
    struct wl_registry *registry = create_host_registry(&server, &host);
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 24; config.columns = 78;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_context *context = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(create_protocol_context(server.client_display, server.client_queue,
        NULL, &context) == BOXRIG_UI_OK);
    pump(&server); pump(&server); pump(&server);
    struct wl_surface *parent = wl_compositor_create_surface(host.compositor);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    boxrig_wayland_terminal_component_config component_config = boxrig_wayland_terminal_component_config_default();
    component_config.view.blink.text_enabled = false;
    component_config.view.blink.cursor_enabled = false;
    CHECK(boxrig_wayland_terminal_component_create(context, parent, model, &component_config, &component) == BOXRIG_UI_OK);
    boxrig_ui_component_allocation allocation = {.width = 574, .height = 336,
        .window_state = BOXRIG_UI_WINDOW_NORMAL};
    boxrig_ui_layout_result layout;
    CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    pump(&server);
    boxrig_composition *composition = boxrig_wayland_terminal_component_composition(component);
    CHECK(boxrig_composition_focus_changed(composition, true) == BOXRIG_COMPOSITION_OK);
    boxrig_composition_event preedit = {.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE,
        .text = "汽車", .text_length = 6, .selection_length = 6, .caret_visible = true};
    CHECK(boxrig_composition_apply_event(composition, &preedit) == BOXRIG_COMPOSITION_OK);
    boxrig_ui_view *view = boxrig_wayland_terminal_component_view(component);
    const char *items[] = {"記者", "汽車", "帰社"}, *labels[] = {"1", "2", "3"};
    boxrig_ui_candidate_snapshot snapshot = {.count = 3, .page_size = 3, .cursor = 1,
        .visible = true, .cursor_visible = true, .items = items, .labels = labels};
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_OK);
    const uint32_t scales[] = {210, 120, 210};
    for (size_t scale = 0; scale < sizeof(scales) / sizeof(scales[0]); scale++) {
        wp_fractional_scale_v1_send_preferred_scale(server.fractional_scale_resource, scales[scale]);
        pump(&server); dispatch_context(context);
        for (size_t step = 0; step < 3; step++) {
            for (size_t index = 0; index < server.frame_callback_count; index++)
                if (server.frame_callbacks[index] != NULL) complete_frame(&server, index);
            for (size_t index = 0; index < server.shm_buffer_create_count; index++)
                if (server.shm_buffers[index] != NULL && server.shm_buffer_busy[index]) release_buffer(&server, index);
            pump(&server);
            boxrig_ui_layout_action action;
            if (boxrig_wayland_terminal_component_take_layout_action(component, &action)) {
                CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
                CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows,
                    action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
                CHECK(boxrig_wayland_terminal_component_complete_grid_resize(component,
                    action.revision, true) == BOXRIG_UI_OK);
            }
            CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
            boxrig_ui_component_frame_result frame;
            CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
            boxrig_ui_hit_test_result hit;
            CHECK(boxrig_wayland_terminal_component_hit_test(component,
                ((boxrig_ui_fixed)allocation.width - 20) * BOXRIG_UI_FIXED_ONE,
                ((boxrig_ui_fixed)allocation.height - 20) * BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
            pump(&server);
        }
        boxrig_ui_candidate_view candidates;
        boxrig_wayland_component_diagnostic diagnostic;
        CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
        CHECK(diagnostic.scale_numerator == scales[scale]);
        CHECK(boxrig_ui_view_get_candidates(view, &candidates) == BOXRIG_UI_OK);
        CHECK(candidates.snapshot.visible && candidates.snapshot.cursor == 1 && candidates.layout.count == 3);
        CHECK(candidates.layout.bounds.width > 0 && candidates.layout.bounds.height > 0);
        CHECK(candidates.layout.bounds.x >= 0 && candidates.layout.bounds.y >= 0);
        CHECK((int64_t)candidates.layout.bounds.x + candidates.layout.bounds.width <= diagnostic.buffer_width);
        CHECK((int64_t)candidates.layout.bounds.y + candidates.layout.bounds.height <= diagnostic.buffer_height);
    }
    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
    puts("PASS native Wayland keeps composition and candidates through fractional scale changes");
}

static void test_fcitx5_native_input(void)
{
    fake_server server;
    host_registry_state host;
    create_server(&server, true, true, true);
    struct wl_registry *registry = create_host_registry(&server, &host);
    boxrig_wayland_context_config config = boxrig_wayland_context_config_default();
    config.input_method = getenv("BOXRIG_TEST_IME_AUTO") != NULL ? BOXRIG_WAYLAND_IME_AUTO : BOXRIG_WAYLAND_IME_FCITX5;
    bool client_candidates = getenv("BOXRIG_TEST_FCITX_CANDIDATES") != NULL;
    config.candidate_policy = client_candidates ? BOXRIG_UI_CANDIDATE_CLIENT : BOXRIG_UI_CANDIDATE_SYSTEM;
    boxrig_wayland_context *context = NULL;
    CHECK(boxrig_wayland_context_create(server.client_display, server.client_queue, &config, &context) == BOXRIG_UI_OK);
    pump(&server); pump(&server); pump(&server);
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    CHECK(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    struct wl_surface *parent = wl_compositor_create_surface(host.compositor);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    boxrig_wayland_terminal_component *component;
    CHECK(create_steady_component(context, parent, model, NULL, &component) == BOXRIG_UI_OK);
    pump(&server);
    send_ibus_keymap(&server);
    struct wl_array keys;
    wl_array_init(&keys);
    wl_keyboard_send_enter(server.keyboard_resources[0], 10, server.child_resource, &keys);
    zwp_text_input_v3_send_enter(server.text_input_resources[0], server.child_resource);
    ibus_test_drain(&server, context, component);
    boxrig_wayland_context_diagnostic diagnostic;
    CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.input_method == BOXRIG_WAYLAND_IME_FCITX5);
    CHECK((diagnostic.input_capabilities & BOXRIG_WAYLAND_INPUT_CAPABILITY_FCITX5) != 0);
    CHECK((diagnostic.input_capabilities & BOXRIG_WAYLAND_INPUT_CAPABILITY_IBUS) == 0);
    boxrig_wayland_component_diagnostic component_state;
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &component_state) == BOXRIG_UI_OK);
    CHECK((component_state.input_capabilities & BOXRIG_WAYLAND_INPUT_CAPABILITY_FCITX5) != 0);
    CHECK((component_state.input_capabilities & BOXRIG_WAYLAND_INPUT_CAPABILITY_IBUS) == 0);
    CHECK(!server.text_current_enabled[0]);
    if (getenv("BOXRIG_TEST_FCITX_REAL") != NULL) {
        GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        CHECK(bus != NULL);
        GError *error = NULL;
        GVariant *reply = g_dbus_connection_call_sync(bus, "org.fcitx.Fcitx5", "/controller",
            "org.fcitx.Fcitx.Controller1", "SetCurrentIM", g_variant_new("(s)", "mozc"),
            G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NO_AUTO_START, 60000, NULL, &error);
        if (error != NULL) fprintf(stderr, "fcitx5 SetCurrentIM: %s\n", error->message);
        CHECK(reply != NULL);
        g_variant_unref(reply);
        const uint32_t word[] = {KEY_K, KEY_I, KEY_S, KEY_H, KEY_A};
        for (size_t index = 0; index < G_N_ELEMENTS(word); index++) ibus_test_key(&server, word[index]);
        ibus_test_drain(&server, context, component);
        boxrig_composition_view text;
        CHECK(boxrig_composition_get_view(boxrig_wayland_terminal_component_composition(component), &text) == BOXRIG_COMPOSITION_OK);
        CHECK(text.active && text.text_length == strlen("きしゃ") && memcmp(text.text, "きしゃ", text.text_length) == 0);
        check_ibus_output(terminal, "");
        ibus_test_key(&server, KEY_SPACE);
        ibus_test_drain(&server, context, component);
        if (client_candidates) {
            ibus_test_key(&server, KEY_SPACE);
            ibus_test_drain(&server, context, component);
            boxrig_ui_candidate_view panel;
            CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component), &panel) == BOXRIG_UI_OK);
            CHECK(panel.snapshot.format == BOXRIG_UI_CANDIDATE_PAGE && panel.snapshot.count > 0 && panel.snapshot.has_next_page);
            ibus_test_key(&server, KEY_PAGEDOWN);
            ibus_test_drain(&server, context, component);
            CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component), &panel) == BOXRIG_UI_OK);
            CHECK(panel.snapshot.count > 0 && panel.snapshot.has_previous_page);
        }
        CHECK(boxrig_composition_get_view(boxrig_wayland_terminal_component_composition(component), &text) == BOXRIG_COMPOSITION_OK);
        CHECK(text.active && text.text_length != 0);
        CHECK(text.text_length != strlen("きしゃ") || memcmp(text.text, "きしゃ", text.text_length) != 0);
        char *converted = g_strndup(text.text, text.text_length);
        ibus_test_key(&server, KEY_ENTER);
        ibus_test_drain(&server, context, component);
        check_ibus_inactive(component);
        check_ibus_output(terminal, converted);
        g_free(converted);
        for (size_t index = 0; index < G_N_ELEMENTS(word); index++) ibus_test_key(&server, word[index]);
        ibus_test_key(&server, KEY_ESC);
        ibus_test_drain(&server, context, component);
        check_ibus_inactive(component);
        check_ibus_output(terminal, "");
        g_object_unref(bus);
    } else {
        GDBusConnection *probe = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        CHECK(probe != NULL);
        boxrig_ui_component_allocation allocation = {.width = 600, .height = 300,
            .window_state = BOXRIG_UI_WINDOW_NORMAL};
        boxrig_ui_layout_result layout;
        CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
        const uint32_t scales[] = {150, 240, 120};
        for (size_t index = 0; index < G_N_ELEMENTS(scales); index++) {
            wp_fractional_scale_v1_send_preferred_scale(server.fractional_scale_resource, scales[index]);
            pump(&server); dispatch_context(context);
            for (size_t buffer = 0; buffer < server.shm_buffer_create_count; buffer++)
                if (server.shm_buffers[buffer] != NULL && server.shm_buffer_busy[buffer]) release_buffer(&server, buffer);
            for (size_t callback = 0; callback < server.frame_callback_count; callback++)
                if (server.frame_callbacks[callback] != NULL) complete_frame(&server, callback);
            pump(&server);
            boxrig_ui_component_frame_result frame;
            for (;;) {
                CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
                CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
                boxrig_ui_layout_action resize;
                if (!boxrig_wayland_terminal_component_take_layout_action(component, &resize)) break;
                CHECK(resize.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
                CHECK(boxrig_terminal_resize(terminal, resize.value.grid_resize.rows,
                    resize.value.grid_resize.columns) == BOXRIG_STATUS_OK);
                CHECK(boxrig_wayland_terminal_component_complete_grid_resize(component, resize.revision, true) == BOXRIG_UI_OK);
            }
            int origin = 32 + 8 * (int)index;
            CHECK(boxrig_wayland_terminal_component_set_ime_window_offset(component, origin, 48) == BOXRIG_UI_OK);
            ibus_test_key(&server, KEY_ESC);
            ibus_test_drain(&server, context, component);
            GVariant *position = g_dbus_connection_call_sync(probe, "org.fcitx.Fcitx5", "/org/boxrig/FcitxProbe",
                "org.boxrig.FcitxProbe", "Cursor", NULL, G_VARIANT_TYPE("(iiiidu)"),
                G_DBUS_CALL_FLAGS_NO_AUTO_START, 60000, NULL, NULL);
            CHECK(position != NULL);
            gint x, y, width, height;
            gdouble scale;
            guint updates;
            g_variant_get(position, "(iiiidu)", &x, &y, &width, &height, &scale, &updates);
            if (updates == 0 || scale != scales[index] / 120.0)
                fprintf(stderr, "Fcitx cursor scale case %zu: updates=%u scale=%g rectangle=%d,%d,%d,%d\n",
                    index, updates, scale, x, y, width, height);
            CHECK(updates > 0 && scale == scales[index] / 120.0);
            CHECK(x == origin * scale && y == 48 * scale);
            boxrig_font_effective_metrics metrics;
            CHECK(boxrig_wayland_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
            CHECK(width >= (gint)metrics.cell_width && width < (gint)metrics.cell_width + 4);
            CHECK(height >= (gint)metrics.cell_height && height < (gint)metrics.cell_height + 4);
            g_variant_unref(position);
        }
        g_object_unref(probe);
        if (client_candidates) {
            CHECK(component_state.candidate_owner == BOXRIG_UI_CANDIDATE_OWNER_NONE);
            ibus_test_key(&server, KEY_V);
            ibus_test_drain(&server, context, component);
            boxrig_ui_candidate_view panel;
            CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component), &panel) == BOXRIG_UI_OK);
            CHECK(panel.snapshot.format == BOXRIG_UI_CANDIDATE_PAGE && panel.snapshot.count == 2 && panel.snapshot.cursor == 1);
            CHECK(strcmp(panel.snapshot.auxiliary_above, "上よみ") == 0 && strcmp(panel.snapshot.auxiliary, "下") == 0);
            CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &component_state) == BOXRIG_UI_OK);
            CHECK(component_state.candidate_owner == BOXRIG_UI_CANDIDATE_OWNER_CLIENT);
            ibus_test_key(&server, KEY_H);
            ibus_test_drain(&server, context, component);
            CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component), &panel) == BOXRIG_UI_OK);
            CHECK(panel.snapshot.count == 0 && !panel.snapshot.auxiliary_visible && !panel.snapshot.auxiliary_above_visible);
        }
        ibus_test_key(&server, KEY_A);
        ibus_test_drain(&server, context, component);
        boxrig_composition_view text;
        CHECK(boxrig_composition_get_view(boxrig_wayland_terminal_component_composition(component), &text) == BOXRIG_COMPOSITION_OK);
        CHECK(text.active && text.text_length == strlen("きしゃ") && memcmp(text.text, "きしゃ", text.text_length) == 0);
        CHECK(text.caret_offset == 3);
        check_ibus_output(terminal, "");
        ibus_test_key(&server, KEY_ESC);
        ibus_test_drain(&server, context, component);
        check_ibus_inactive(component);
        ibus_test_key(&server, KEY_A);
        ibus_test_key(&server, KEY_B);
        ibus_test_key(&server, KEY_U);
        ibus_test_drain(&server, context, component);
        check_ibus_output(terminal, "汽車🙂u");
        ibus_test_key(&server, KEY_F);
        ibus_test_drain(&server, context, component);
        check_ibus_output(terminal, "x");
        boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(
            boxrig_wayland_terminal_component_view(component));
        editable_owner owner = {.disposition = BOXRIG_UI_OK};
        CHECK(boxrig_ui_editable_document_set_apply_handler(document, apply_editable, &owner) == BOXRIG_UI_OK);
        publish_editable(document, &owner, "前かな後", 9, 9);
        ibus_test_key(&server, KEY_D);
        ibus_test_drain(&server, context, component);
        CHECK(owner.accepted == 1 && strcmp(owner.text, "前記者後") == 0);
        check_ibus_output(terminal, "");
        CHECK(boxrig_ui_editable_document_clear(document) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_editable_document_set_apply_handler(document, NULL, NULL) == BOXRIG_UI_OK);
    }
    ibus_test_key(&server, KEY_A);
    ibus_test_drain(&server, context, component);
    wl_keyboard_send_leave(server.keyboard_resources[0], 50, server.child_resource);
    pump(&server); dispatch_context(context);
    check_ibus_output(terminal, getenv("BOXRIG_TEST_FCITX_REAL") != NULL ? "あ" : "き");
    check_ibus_inactive(component);
    wl_array_release(&keys);
    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
    puts("PASS native Wayland fcitx5 preedit, conversion, commit, cancellation and focus commit");
}

static void test_fcitx4_native_input(void)
{
    fake_server server;
    host_registry_state host;
    create_server(&server, true, true, true);
    struct wl_registry *registry = create_host_registry(&server, &host);
    boxrig_wayland_context_config config = boxrig_wayland_context_config_default();
    config.input_method = getenv("BOXRIG_TEST_IME_AUTO") != NULL ? BOXRIG_WAYLAND_IME_AUTO : BOXRIG_WAYLAND_IME_FCITX4;
    bool panels = getenv("BOXRIG_TEST_FCITX4_PANELS") != NULL;
    config.candidate_policy = panels ? BOXRIG_UI_CANDIDATE_AUTO : BOXRIG_UI_CANDIDATE_SYSTEM;
    boxrig_wayland_context *context = NULL;
    CHECK(boxrig_wayland_context_create(server.client_display, server.client_queue, &config, &context) == BOXRIG_UI_OK);
    pump(&server); pump(&server); pump(&server);
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    CHECK(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    struct wl_surface *parent = wl_compositor_create_surface(host.compositor);
    wl_proxy_set_queue((struct wl_proxy *)parent, server.client_queue);
    boxrig_wayland_terminal_component *component;
    CHECK(create_steady_component(context, parent, model, NULL, &component) == BOXRIG_UI_OK);
    pump(&server);
    send_ibus_keymap(&server);
    struct wl_array keys;
    wl_array_init(&keys);
    wl_keyboard_send_enter(server.keyboard_resources[0], 10, server.child_resource, &keys);
    zwp_text_input_v3_send_enter(server.text_input_resources[0], server.child_resource);
    ibus_test_drain(&server, context, component);
    boxrig_wayland_context_diagnostic diagnostic;
    CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.input_method == BOXRIG_WAYLAND_IME_FCITX4 && diagnostic.direct.backend == BOXRIG_IME_BACKEND_FCITX4);
    CHECK((diagnostic.input_capabilities & BOXRIG_WAYLAND_INPUT_CAPABILITY_FCITX4) != 0);
    CHECK((diagnostic.input_capabilities & BOXRIG_WAYLAND_INPUT_CAPABILITY_IBUS) == 0);
    boxrig_wayland_component_diagnostic component_state;
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &component_state) == BOXRIG_UI_OK);
    CHECK((component_state.input_capabilities & BOXRIG_WAYLAND_INPUT_CAPABILITY_FCITX4) != 0);
    CHECK((component_state.input_capabilities & BOXRIG_WAYLAND_INPUT_CAPABILITY_IBUS) == 0);
    CHECK(!server.text_current_enabled[0]);
    if (getenv("BOXRIG_TEST_FCITX_REAL") != NULL) {
        CHECK(panels);
        const char *display = getenv("DISPLAY"), *address = getenv("BOXRIG_TEST_FCITX4_REAL_ADDRESS");
        CHECK(display != NULL && strrchr(display, ':') != NULL && address != NULL);
        char *service = g_strdup_printf("org.fcitx.Fcitx-%d", atoi(strrchr(display, ':') + 1));
        GDBusConnection *bus = g_dbus_connection_new_for_address_sync(address,
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
            NULL, NULL, NULL);
        CHECK(bus != NULL);
        GVariant *reply = g_dbus_connection_call_sync(bus, service, "/inputmethod", "org.fcitx.Fcitx.InputMethod",
            "SetCurrentIM", g_variant_new("(s)", "mozc"), G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NO_AUTO_START, 60000, NULL, NULL);
        CHECK(reply != NULL);
        g_variant_unref(reply);
        reply = g_dbus_connection_call_sync(bus, service, "/inputmethod", "org.fcitx.Fcitx.InputMethod",
            "ActivateIM", NULL, G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NO_AUTO_START, 60000, NULL, NULL);
        CHECK(reply != NULL);
        g_variant_unref(reply);
        const uint32_t word[] = {KEY_K, KEY_I, KEY_S, KEY_H, KEY_A};
        for (size_t index = 0; index < G_N_ELEMENTS(word); index++) ibus_test_key(&server, word[index]);
        ibus_test_drain(&server, context, component);
        boxrig_ui_candidate_view panel;
        CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component), &panel) == BOXRIG_UI_OK);
        CHECK(panel.snapshot.format == BOXRIG_UI_CANDIDATE_STRING_PANEL && panel.snapshot.visible);
        CHECK(panel.snapshot.panel_preedit != NULL && g_str_has_prefix(panel.snapshot.panel_preedit, "きしゃ"));
        CHECK(panel.snapshot.panel_cursor == strlen("きしゃ"));
        check_ibus_inactive(component);
        check_ibus_output(terminal, "");
        ibus_test_key(&server, KEY_SPACE);
        ibus_test_key(&server, KEY_SPACE);
        ibus_test_drain(&server, context, component);
        CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component), &panel) == BOXRIG_UI_OK);
        CHECK(panel.snapshot.panel_text != NULL && panel.snapshot.panel_text[0] != 0);
        char *first = g_strdup(panel.snapshot.panel_text);
        ibus_test_key(&server, KEY_PAGEDOWN);
        ibus_test_drain(&server, context, component);
        CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component), &panel) == BOXRIG_UI_OK);
        CHECK(panel.snapshot.panel_text != NULL && panel.snapshot.panel_text[0] != 0);
        CHECK(strcmp(panel.snapshot.panel_text, first) != 0);
        g_free(first);
        boxrig_font_effective_metrics metrics;
        CHECK(boxrig_wayland_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
        boxrig_ui_component_allocation allocation = {.width = tc.columns * metrics.cell_width,
            .height = tc.rows * metrics.cell_height, .window_state = BOXRIG_UI_WINDOW_EMBEDDED};
        boxrig_ui_layout_result layout;
        CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
        CHECK(boxrig_wayland_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
        boxrig_ui_component_frame_result frame;
        CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component), &panel) == BOXRIG_UI_OK);
        CHECK(panel.layout.panel_text.height > 0 && panel.layout.panel_preedit.height > 0 && panel.layout.panel_caret.height > 0);
        CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &component_state) == BOXRIG_UI_OK);
        CHECK(component_state.candidate_owner == BOXRIG_UI_CANDIDATE_OWNER_CLIENT);
        ibus_test_key(&server, KEY_F7);
        ibus_test_drain(&server, context, component);
        CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component), &panel) == BOXRIG_UI_OK);
        CHECK(panel.snapshot.panel_preedit != NULL && g_str_has_prefix(panel.snapshot.panel_preedit, "キシャ"));
        ibus_test_key(&server, KEY_ENTER);
        ibus_test_drain(&server, context, component);
        check_ibus_output(terminal, "キシャ");
        CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component), &panel) == BOXRIG_UI_OK);
        CHECK(!panel.snapshot.visible);
        for (size_t index = 0; index < G_N_ELEMENTS(word); index++) ibus_test_key(&server, word[index]);
        ibus_test_key(&server, KEY_ESC);
        ibus_test_drain(&server, context, component);
        CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component), &panel) == BOXRIG_UI_OK);
        CHECK(!panel.snapshot.visible);
        check_ibus_output(terminal, "");
        ibus_test_key(&server, KEY_A);
        ibus_test_drain(&server, context, component);
        wl_keyboard_send_leave(server.keyboard_resources[0], 73, server.child_resource);
        for (;;) {
            pump(&server); dispatch_context(context);
            CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) == BOXRIG_UI_OK);
            CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &component_state) == BOXRIG_UI_OK);
            if (diagnostic.direct.pending == 0 && component_state.input_queue_depth == 0 && !component_state.ime_waiting) break;
            ibus_test_poll(context);
        }
        check_ibus_output(terminal, "あ");
        CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component), &panel) == BOXRIG_UI_OK);
        CHECK(panel.snapshot.panel_text == NULL);
        CHECK(g_dbus_connection_close_sync(bus, NULL, NULL));
        g_object_unref(bus);
        g_free(service);
        puts("PASS native Wayland real fcitx4/Mozc string panel, keyboard paging, cancellation and focus commit");
        goto fcitx4_cleanup;
    }
    ibus_test_key(&server, KEY_A);
    ibus_test_drain(&server, context, component);
    boxrig_composition_view text;
    CHECK(boxrig_composition_get_view(boxrig_wayland_terminal_component_composition(component), &text) == BOXRIG_COMPOSITION_OK);
    if (panels) {
        CHECK(!text.active && component_state.candidate_owner == BOXRIG_UI_CANDIDATE_OWNER_NONE);
        boxrig_font_effective_metrics metrics;
        CHECK(boxrig_wayland_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
        boxrig_ui_component_allocation allocation = {.width = tc.columns * metrics.cell_width,
            .height = tc.rows * metrics.cell_height, .window_state = BOXRIG_UI_WINDOW_EMBEDDED};
        boxrig_ui_layout_result layout;
        CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
        CHECK(boxrig_wayland_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
        boxrig_ui_component_frame_result frame;
        CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        boxrig_ui_candidate_view panel;
        CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component), &panel) == BOXRIG_UI_OK);
        CHECK(panel.snapshot.format == BOXRIG_UI_CANDIDATE_STRING_PANEL && panel.snapshot.panel_preedit != NULL);
        CHECK(strcmp(panel.snapshot.panel_preedit, "きしゃ🙂") == 0 && panel.snapshot.panel_cursor == 6);
        CHECK(panel.layout.panel_preedit.height > 0 && panel.layout.panel_caret.height > 0);
        ibus_test_key(&server, KEY_P);
        ibus_test_drain(&server, context, component);
        for (size_t index = 0; index < server.frame_callback_count; index++)
            if (server.frame_callbacks[index] != NULL) complete_frame(&server, index);
        for (size_t index = 0; index < server.shm_buffer_create_count; index++)
            if (server.shm_buffers[index] != NULL && server.shm_buffer_busy[index]) release_buffer(&server, index);
        pump(&server);
        CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component), &panel) == BOXRIG_UI_OK);
        CHECK(strcmp(panel.snapshot.panel_text, "1. 汽車 2号 2. 記者 🙂") == 0);
        CHECK(panel.layout.panel_text.height > 0 && panel.snapshot.count == 0);
        ibus_test_key(&server, KEY_N);
        ibus_test_drain(&server, context, component);
        CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component), &panel) == BOXRIG_UI_OK);
        CHECK(strcmp(panel.snapshot.panel_text, "1. 次ページ 2. 候補") == 0);
        CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &component_state) == BOXRIG_UI_OK);
        CHECK(component_state.candidate_owner == BOXRIG_UI_CANDIDATE_OWNER_CLIENT);
        CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, false) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component), &panel) == BOXRIG_UI_OK);
        CHECK(panel.snapshot.panel_text == NULL);
        CHECK(boxrig_wayland_terminal_component_set_input_enabled(component, true) == BOXRIG_UI_OK);
        ibus_test_drain(&server, context, component);
        ibus_test_key(&server, KEY_A);
        ibus_test_drain(&server, context, component);
    } else {
        CHECK(text.active && text.text_length == strlen("きしゃ") && memcmp(text.text, "きしゃ", text.text_length) == 0);
        CHECK(text.caret_offset == 3 && text.segment_count == 2);
    }
    check_ibus_output(terminal, "");
    ibus_test_key(&server, KEY_ESC);
    ibus_test_drain(&server, context, component);
    check_ibus_inactive(component);
    ibus_test_key(&server, KEY_A);
    ibus_test_key(&server, KEY_B);
    ibus_test_key(&server, KEY_U);
    ibus_test_drain(&server, context, component);
    check_ibus_output(terminal, "汽車🙂u");
    editable_owner owner = {.disposition = BOXRIG_UI_OK};
    boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(
        boxrig_wayland_terminal_component_view(component));
    for (unsigned int destination = 0; destination < 2; destination++) {
        if (destination != 0) {
            CHECK(boxrig_ui_editable_document_set_apply_handler(document, apply_editable, &owner) == BOXRIG_UI_OK);
            publish_editable(document, &owner, "前かな後", 9, 9);
            wl_keyboard_send_enter(server.keyboard_resources[0], 51, server.child_resource, &keys);
            ibus_test_drain(&server, context, component);
        }
        ibus_test_key(&server, KEY_A);
        ibus_test_drain(&server, context, component);
        wl_keyboard_send_leave(server.keyboard_resources[0], 52 + destination, server.child_resource);
        for (;;) {
            pump(&server); dispatch_context(context);
            CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) == BOXRIG_UI_OK);
            CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &component_state) == BOXRIG_UI_OK);
            if (diagnostic.direct.pending == 0 && component_state.input_queue_depth == 0 && !component_state.ime_waiting) break;
            ibus_test_poll(context);
        }
        if (destination != 0) CHECK(owner.accepted == 1 && strcmp(owner.text, "前かなserver-commit後") == 0);
        check_ibus_output(terminal, destination == 0 ? "server-commit" : "");
        check_ibus_inactive(component);
    }
    if (panels) {
        boxrig_ui_candidate_view panel;
        CHECK(boxrig_ui_view_get_candidates(boxrig_wayland_terminal_component_view(component), &panel) == BOXRIG_UI_OK);
        CHECK(panel.snapshot.panel_text == NULL && !panel.snapshot.visible);
    }
fcitx4_cleanup:
    wl_array_release(&keys);
    boxrig_wayland_terminal_component_destroy(component);
    wl_surface_destroy(parent);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(registry);
    pump(&server);
    boxrig_wayland_context_destroy(context);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_server(&server);
    puts("PASS native Wayland fcitx4 selection, preedit, cancellation, input and server focus commit");
}

int main(int argc, char **argv)
{
    boxrig_wayland_context_config context_config =
        boxrig_wayland_context_config_default();
    boxrig_wayland_terminal_component_config component_config =
        boxrig_wayland_terminal_component_config_default();

    if (argc != 2) {
        fprintf(stderr, "usage: test_wayland_context FONT_FILE\n");
        return 2;
    }
    if (getenv("BOXRIG_TEST_FCITX4") != NULL) {
        test_fcitx4_native_input();
        FcFini();
        return 0;
    }
    if (getenv("BOXRIG_TEST_FCITX5") != NULL) {
        test_fcitx5_native_input();
        FcFini();
        return 0;
    }
    CHECK(context_config.input_method == BOXRIG_WAYLAND_IME_AUTO);
    CHECK(context_config.maximum_seats == BOXRIG_WAYLAND_SEATS_DEFAULT);
    CHECK(context_config.maximum_keymap_bytes ==
          BOXRIG_WAYLAND_KEYMAP_BYTES_DEFAULT);
    CHECK(context_config.maximum_clipboard_transfers ==
          BOXRIG_WAYLAND_CLIPBOARD_TRANSFERS_DEFAULT);
    CHECK(context_config.maximum_clipboard_bytes ==
          BOXRIG_WAYLAND_CLIPBOARD_BYTES_DEFAULT);
    CHECK(context_config.transfer_chunk_size ==
          BOXRIG_WAYLAND_TRANSFER_CHUNK_DEFAULT);
    CHECK(component_config.buffer_count == 3);
    CHECK(component_config.maximum_input_events ==
          BOXRIG_WAYLAND_INPUT_EVENTS_DEFAULT);
    CHECK(component_config.maximum_ime_bytes ==
          BOXRIG_WAYLAND_IME_BYTES_DEFAULT);
    CHECK(component_config.paste_capacity ==
          BOXRIG_WAYLAND_PASTE_BYTES_DEFAULT);
    CHECK(component_config.paste_chunk_size ==
          BOXRIG_WAYLAND_PASTE_CHUNK_DEFAULT);
    CHECK(!component_config.keyboard_focus_fallback);
    test_ibus_route_selection();
    test_ibus_native_preedit();
    test_native_candidate_pointer();
    test_native_candidate_scale();
    test_standard_scrollbar_visibility();
    test_input_inhibition();
    test_terminal_io_configuration();
    test_ready_and_required_removal();
    test_missing_required_global();
    test_clipboard_managers_follow_seat_lifetime();
    test_copy_uses_focused_seat_serial_and_immutable_sources();
    test_paste_reads_validated_text_once_and_retries();
    test_owned_child_and_parent_handshake();
    test_visible_resize_commits_child_with_parent();
    test_integer_output_scale_and_hit_test();
    test_fractional_scale_viewport_and_hit_test();
    test_work_queue_overflow_is_sticky();
    test_transparency_regions_and_fallback();
    test_visual_zoom_issues_one_application_size_command();
    test_constrained_visual_zoom_dispatches_one_grid_action();
    test_missing_argb_policy();
    test_seat_keyboard_routing_and_removal();
    test_host_keyboard_focus_routes_to_fallback_component();
    test_reparent_input_generation_and_pending_focus();
    test_pointer_scale_batch_and_click_wrap();
    test_text_input_editable_document();
    test_text_input_batch_and_compose_fallback();
    test_text_input_candidate_stays_at_preedit_caret();
    test_text_input_v2_clause_hints();
    test_text_input_v2_cursor_surface_commit();
    test_keyboard_resets_cursor_blink();
    test_multiple_seats_and_components_are_independent();
    test_input_backpressure_and_resource_limits();
    test_shared_frontend_component_fixture(argv[1]);
    FcFini();
    printf("PASS native Wayland context and owned child lifecycle\n");
    return 0;
}
