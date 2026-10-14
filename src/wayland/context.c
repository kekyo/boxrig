/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "context_private.h"
#include "ibus_private.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BOXRIG_WAYLAND_COMPOSITOR_VERSION 6U
#define BOXRIG_WAYLAND_SHM_VERSION 1U
#define BOXRIG_WAYLAND_SUBCOMPOSITOR_VERSION 1U
#define BOXRIG_WAYLAND_OUTPUT_VERSION 4U
#define BOXRIG_WAYLAND_VIEWPORTER_VERSION 1U
#define BOXRIG_WAYLAND_FRACTIONAL_SCALE_VERSION 1U
#define BOXRIG_WAYLAND_TEXT_INPUT_VERSION 2U
#define BOXRIG_WAYLAND_DATA_DEVICE_MANAGER_VERSION 3U
#define BOXRIG_WAYLAND_PRIMARY_SELECTION_VERSION 1U

static uint32_t supported_version(uint32_t advertised, uint32_t supported)
{
    return advertised < supported ? advertised : supported;
}

static void advance_generation(boxrig_wayland_context *context)
{
    if (context->global_generation != UINT64_MAX) {
        context->global_generation++;
    }
}

static void mark_fatal(
    boxrig_wayland_context *context,
    boxrig_wayland_failure_reason reason)
{
    if (context->state != BOXRIG_WAYLAND_CONTEXT_FATAL) {
        context->state = BOXRIG_WAYLAND_CONTEXT_FATAL;
        context->failure_reason = reason;
    }
}

static bool queue_work(
    boxrig_wayland_context *context,
    boxrig_wayland_work_type type,
    boxrig_wayland_terminal_component *component)
{
    size_t index;

    if (context->state == BOXRIG_WAYLAND_CONTEXT_FATAL ||
        context->state == BOXRIG_WAYLAND_CONTEXT_QUIESCED) {
        return false;
    }
    if (context->work_queue_depth == context->config.maximum_work_items) {
        mark_fatal(context, BOXRIG_WAYLAND_FAILURE_WORK_QUEUE_OVERFLOW);
        return false;
    }
    index = (context->work_queue_head + context->work_queue_depth) %
            context->config.maximum_work_items;
    context->work_items[index].type = type;
    context->work_items[index].component = component;
    context->work_queue_depth++;
    if (context->work_queue_depth > context->work_queue_high_water) {
        context->work_queue_high_water = context->work_queue_depth;
    }
    return true;
}

bool boxrig_wayland_context_queue_component_scale(
    boxrig_wayland_context *context,
    boxrig_wayland_terminal_component *component)
{
    if (context == NULL || component == NULL) {
        return false;
    }
    return queue_work(
        context,
        BOXRIG_WAYLAND_WORK_COMPONENT_SCALE,
        component);
}

void boxrig_wayland_context_queue_output_change(
    boxrig_wayland_context *context)
{
    if (context->component_count == 0 || context->output_work_queued) {
        return;
    }
    if (queue_work(
            context,
            BOXRIG_WAYLAND_WORK_OUTPUT_CHANGE,
            NULL)) {
        context->output_work_queued = true;
    }
}

void boxrig_wayland_context_remove_component_work(
    boxrig_wayland_context *context,
    boxrig_wayland_terminal_component *component)
{
    size_t count;
    size_t index;

    if (context == NULL || component == NULL) {
        return;
    }
    count = context->work_queue_depth;
    for (index = 0; index < count; index++) {
        boxrig_wayland_work_item item =
            context->work_items[context->work_queue_head];

        context->work_queue_head =
            (context->work_queue_head + 1) %
            context->config.maximum_work_items;
        context->work_queue_depth--;
        if (item.type != BOXRIG_WAYLAND_WORK_COMPONENT_SCALE ||
            item.component != component) {
            size_t tail =
                (context->work_queue_head + context->work_queue_depth) %
                context->config.maximum_work_items;

            context->work_items[tail] = item;
            context->work_queue_depth++;
        }
    }
}

static void shm_format(
    void *data,
    struct wl_shm *shm,
    uint32_t format)
{
    boxrig_wayland_context *context = data;

    (void)shm;
    if (format == WL_SHM_FORMAT_ARGB8888) {
        context->argb8888 = true;
    } else if (format == WL_SHM_FORMAT_XRGB8888) {
        context->xrgb8888 = true;
    }
}

static const struct wl_shm_listener shm_listener = {
    .format = shm_format,
};

static void output_geometry(
    void *data,
    struct wl_output *output,
    int32_t x,
    int32_t y,
    int32_t physical_width,
    int32_t physical_height,
    int32_t subpixel,
    const char *make,
    const char *model,
    int32_t transform)
{
    (void)data;
    (void)output;
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)make;
    (void)model;
    (void)transform;
}

static void output_mode(
    void *data,
    struct wl_output *output,
    uint32_t flags,
    int32_t width,
    int32_t height,
    int32_t refresh)
{
    (void)data;
    (void)output;
    (void)flags;
    (void)width;
    (void)height;
    (void)refresh;
}

static void output_done(void *data, struct wl_output *output)
{
    (void)data;
    (void)output;
}

static void output_scale(
    void *data,
    struct wl_output *output,
    int32_t factor)
{
    boxrig_wayland_output *entry = data;

    (void)output;
    if (factor > 0) {
        entry->scale = factor;
        boxrig_wayland_terminal_components_note_output(
            entry->context,
            entry->global_name);
    }
}

static void output_name(
    void *data,
    struct wl_output *output,
    const char *name)
{
    (void)data;
    (void)output;
    (void)name;
}

static void output_description(
    void *data,
    struct wl_output *output,
    const char *description)
{
    (void)data;
    (void)output;
    (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static bool add_output(
    boxrig_wayland_context *context,
    uint32_t name,
    uint32_t version)
{
    boxrig_wayland_output **outputs;
    boxrig_wayland_output *entry;
    size_t capacity;

    if (context->output_count == context->output_capacity) {
        capacity = context->output_capacity == 0 ? 4 : context->output_capacity * 2;
        if (capacity > context->config.maximum_work_items) {
            capacity = context->config.maximum_work_items;
        }
        if (capacity <= context->output_count ||
            capacity > SIZE_MAX / sizeof(*outputs)) {
            return false;
        }
        outputs = realloc(context->outputs, capacity * sizeof(*outputs));
        if (outputs == NULL) {
            return false;
        }
        context->outputs = outputs;
        context->output_capacity = capacity;
    }
    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return false;
    }
    entry->context = context;
    entry->global_name = name;
    entry->version = supported_version(version, BOXRIG_WAYLAND_OUTPUT_VERSION);
    entry->scale = 1;
    entry->proxy = wl_registry_bind(
        context->registry,
        name,
        &wl_output_interface,
        entry->version);
    if (entry->proxy == NULL) {
        free(entry);
        return false;
    }
    wl_proxy_set_queue((struct wl_proxy *)entry->proxy, context->queue);
    if (wl_output_add_listener(entry->proxy, &output_listener, entry) != 0) {
        wl_output_destroy(entry->proxy);
        free(entry);
        return false;
    }
    context->outputs[context->output_count] = entry;
    context->output_count++;
    context->flush_required = true;
    return true;
}

static void registry_global(
    void *data,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version)
{
    boxrig_wayland_context *context = data;

    advance_generation(context);
    if (strcmp(interface, wl_compositor_interface.name) == 0 &&
        context->compositor == NULL) {
        context->compositor = wl_registry_bind(
            registry,
            name,
            &wl_compositor_interface,
            supported_version(version, BOXRIG_WAYLAND_COMPOSITOR_VERSION));
        if (context->compositor != NULL) {
            context->compositor_name = name;
            wl_proxy_set_queue(
                (struct wl_proxy *)context->compositor,
                context->queue);
        }
    } else if (strcmp(interface, wl_shm_interface.name) == 0 &&
               context->shm == NULL) {
        context->shm = wl_registry_bind(
            registry,
            name,
            &wl_shm_interface,
            supported_version(version, BOXRIG_WAYLAND_SHM_VERSION));
        if (context->shm != NULL) {
            context->shm_name = name;
            wl_proxy_set_queue((struct wl_proxy *)context->shm, context->queue);
            if (wl_shm_add_listener(context->shm, &shm_listener, context) != 0) {
                mark_fatal(context, BOXRIG_WAYLAND_FAILURE_RESOURCE);
            }
        }
    } else if (strcmp(interface, wl_subcompositor_interface.name) == 0 &&
               context->subcompositor == NULL) {
        context->subcompositor = wl_registry_bind(
            registry,
            name,
            &wl_subcompositor_interface,
            supported_version(version, BOXRIG_WAYLAND_SUBCOMPOSITOR_VERSION));
        if (context->subcompositor != NULL) {
            context->subcompositor_name = name;
            wl_proxy_set_queue(
                (struct wl_proxy *)context->subcompositor,
                context->queue);
        }
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (!add_output(context, name, version)) {
            mark_fatal(context, BOXRIG_WAYLAND_FAILURE_RESOURCE);
        }
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0 &&
               context->viewporter == NULL) {
        context->viewporter = wl_registry_bind(
            registry,
            name,
            &wp_viewporter_interface,
            supported_version(version, BOXRIG_WAYLAND_VIEWPORTER_VERSION));
        if (context->viewporter != NULL) {
            context->viewporter_name = name;
            wl_proxy_set_queue(
                (struct wl_proxy *)context->viewporter,
                context->queue);
        }
    } else if (strcmp(
                   interface,
                   wp_fractional_scale_manager_v1_interface.name) == 0 &&
               context->fractional_scale_manager == NULL) {
        context->fractional_scale_manager = wl_registry_bind(
            registry,
            name,
            &wp_fractional_scale_manager_v1_interface,
            supported_version(
                version,
                BOXRIG_WAYLAND_FRACTIONAL_SCALE_VERSION));
        if (context->fractional_scale_manager != NULL) {
            context->fractional_scale_name = name;
            wl_proxy_set_queue(
                (struct wl_proxy *)context->fractional_scale_manager,
                context->queue);
        }
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        if (!boxrig_wayland_input_add_seat(context, name, version)) {
            context->unavailable_seat_count++;
        }
    } else if (strcmp(
                   interface,
                   zwp_text_input_manager_v3_interface.name) == 0 &&
               context->text_input_manager == NULL) {
        if (!boxrig_wayland_input_bind_text_manager(
                context,
                registry,
                name,
                supported_version(
                    version,
                    BOXRIG_WAYLAND_TEXT_INPUT_VERSION))) {
            context->text_input_manager = NULL;
            context->text_input_name = 0;
        }
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0 &&
               context->data_device_manager == NULL) {
        if (!boxrig_wayland_clipboard_bind_data_manager(
                context,
                registry,
                name,
                supported_version(
                    version,
                    BOXRIG_WAYLAND_DATA_DEVICE_MANAGER_VERSION))) {
            context->data_device_manager = NULL;
            context->data_device_manager_name = 0;
        }
    } else if (strcmp(
                   interface,
                   zwp_primary_selection_device_manager_v1_interface.name) ==
                   0 &&
               context->primary_selection_manager == NULL) {
        if (!boxrig_wayland_clipboard_bind_primary_manager(
                context,
                registry,
                name,
                supported_version(
                    version,
                    BOXRIG_WAYLAND_PRIMARY_SELECTION_VERSION))) {
            context->primary_selection_manager = NULL;
            context->primary_selection_manager_name = 0;
        }
    }
    context->flush_required = true;
}

static void remove_output(boxrig_wayland_context *context, uint32_t name)
{
    size_t index;

    for (index = 0; index < context->output_count; index++) {
        if (context->outputs[index]->global_name == name) {
            wl_output_destroy(context->outputs[index]->proxy);
            free(context->outputs[index]);
            context->outputs[index] =
                context->outputs[context->output_count - 1];
            context->output_count--;
            boxrig_wayland_terminal_components_note_output(context, name);
            context->flush_required = true;
            return;
        }
    }
}

static void registry_global_remove(
    void *data,
    struct wl_registry *registry,
    uint32_t name)
{
    boxrig_wayland_context *context = data;

    (void)registry;
    advance_generation(context);
    if (boxrig_wayland_clipboard_remove_global(context, name) ||
        boxrig_wayland_input_remove_global(context, name)) {
        context->flush_required = true;
        return;
    }
    if (name == context->compositor_name || name == context->shm_name ||
        name == context->subcompositor_name) {
        if (context->state == BOXRIG_WAYLAND_CONTEXT_INITIALIZING) {
            context->failure_reason =
                BOXRIG_WAYLAND_FAILURE_REQUIRED_GLOBAL_MISSING;
        } else {
            mark_fatal(
                context,
                BOXRIG_WAYLAND_FAILURE_REQUIRED_GLOBAL_REMOVED);
        }
        return;
    }
    if (name == context->viewporter_name) {
        wp_viewporter_destroy(context->viewporter);
        context->viewporter = NULL;
        context->viewporter_name = 0;
        context->flush_required = true;
    } else if (name == context->fractional_scale_name) {
        wp_fractional_scale_manager_v1_destroy(
            context->fractional_scale_manager);
        context->fractional_scale_manager = NULL;
        context->fractional_scale_name = 0;
        context->flush_required = true;
    } else {
        remove_output(context, name);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void initial_sync_done(
    void *data,
    struct wl_callback *callback,
    uint32_t callback_data);

static const struct wl_callback_listener initial_sync_listener = {
    .done = initial_sync_done,
};

static bool request_initial_sync(boxrig_wayland_context *context)
{
    struct wl_callback *callback;
    void *wrapper = wl_proxy_create_wrapper(context->display);

    if (wrapper == NULL) {
        return false;
    }
    wl_proxy_set_queue(wrapper, context->queue);
    callback = wl_display_sync(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    if (callback == NULL) {
        return false;
    }
    wl_proxy_set_queue((struct wl_proxy *)callback, context->queue);
    if (wl_callback_add_listener(
            callback,
            &initial_sync_listener,
            context) != 0) {
        wl_callback_destroy(callback);
        return false;
    }
    context->initial_sync = callback;
    context->flush_required = true;
    return true;
}

static void initial_sync_done(
    void *data,
    struct wl_callback *callback,
    uint32_t callback_data)
{
    boxrig_wayland_context *context = data;

    (void)callback_data;
    wl_callback_destroy(callback);
    context->initial_sync = NULL;
    if (context->state == BOXRIG_WAYLAND_CONTEXT_FATAL) {
        return;
    }
    if (context->compositor == NULL || context->shm == NULL ||
        context->subcompositor == NULL) {
        context->state = BOXRIG_WAYLAND_CONTEXT_UNAVAILABLE;
        context->failure_reason =
            BOXRIG_WAYLAND_FAILURE_REQUIRED_GLOBAL_MISSING;
        return;
    }
    if (!context->registry_globals_synchronized) {
        /* The first sync completes registry enumeration. A second sync is
         * required for initial events from objects bound by global handlers,
         * including wl_shm format capabilities. */
        context->registry_globals_synchronized = true;
        if (!request_initial_sync(context)) {
            mark_fatal(context, BOXRIG_WAYLAND_FAILURE_RESOURCE);
        }
        return;
    }
    context->state = BOXRIG_WAYLAND_CONTEXT_READY;
    context->failure_reason = BOXRIG_WAYLAND_FAILURE_NONE;
    if (!boxrig_wayland_ibus_initialize(context)) mark_fatal(context, BOXRIG_WAYLAND_FAILURE_RESOURCE);
}

boxrig_wayland_context_config boxrig_wayland_context_config_default(void)
{
    boxrig_wayland_context_config config;

    config.input_method = BOXRIG_WAYLAND_IME_WAYLAND;
    config.candidate_policy = BOXRIG_UI_CANDIDATE_AUTO;
    config.maximum_components = BOXRIG_WAYLAND_COMPONENTS_DEFAULT;
    config.maximum_work_items = BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT;
    config.maximum_seats = BOXRIG_WAYLAND_SEATS_DEFAULT;
    config.maximum_keymap_bytes = BOXRIG_WAYLAND_KEYMAP_BYTES_DEFAULT;
    config.maximum_clipboard_transfers =
        BOXRIG_WAYLAND_CLIPBOARD_TRANSFERS_DEFAULT;
    config.maximum_clipboard_bytes = BOXRIG_WAYLAND_CLIPBOARD_BYTES_DEFAULT;
    config.transfer_chunk_size = BOXRIG_WAYLAND_TRANSFER_CHUNK_DEFAULT;
    return config;
}

boxrig_ui_status boxrig_wayland_context_create(
    struct wl_display *display,
    struct wl_event_queue *queue,
    const boxrig_wayland_context_config *config,
    boxrig_wayland_context **context_out)
{
    boxrig_wayland_context_config effective;
    boxrig_wayland_context *context;
    void *wrapper;

    if (display == NULL || queue == NULL || context_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *context_out = NULL;
    effective = config == NULL ? boxrig_wayland_context_config_default()
                               : *config;
    if (effective.input_method < BOXRIG_WAYLAND_IME_WAYLAND ||
        effective.input_method > BOXRIG_WAYLAND_IME_FCITX5 ||
        effective.candidate_policy < BOXRIG_UI_CANDIDATE_AUTO ||
        effective.candidate_policy > BOXRIG_UI_CANDIDATE_CLIENT ||
        effective.maximum_components == 0 ||
        effective.maximum_components > BOXRIG_WAYLAND_COMPONENTS_MAXIMUM ||
        effective.maximum_work_items == 0 ||
        effective.maximum_work_items > BOXRIG_WAYLAND_WORK_ITEMS_MAXIMUM ||
        effective.maximum_seats == 0 ||
        effective.maximum_seats > BOXRIG_WAYLAND_SEATS_MAXIMUM ||
        effective.maximum_keymap_bytes == 0 ||
        effective.maximum_keymap_bytes > BOXRIG_WAYLAND_KEYMAP_BYTES_MAXIMUM ||
        effective.maximum_clipboard_transfers == 0 ||
        effective.maximum_clipboard_transfers >
            BOXRIG_WAYLAND_CLIPBOARD_TRANSFERS_MAXIMUM ||
        effective.maximum_clipboard_bytes == 0 ||
        effective.maximum_clipboard_bytes >
            BOXRIG_WAYLAND_CLIPBOARD_BYTES_MAXIMUM ||
        effective.transfer_chunk_size == 0 ||
        effective.transfer_chunk_size >
            BOXRIG_WAYLAND_TRANSFER_CHUNK_MAXIMUM) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    context->display = display;
    context->queue = queue;
    context->config = effective;
    context->state = BOXRIG_WAYLAND_CONTEXT_INITIALIZING;
    context->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (context->xkb_context != NULL) {
        const char *locale = setlocale(LC_CTYPE, NULL);

        if (locale == NULL || locale[0] == '\0') {
            locale = "C";
        }
        context->compose_table = xkb_compose_table_new_from_locale(
            context->xkb_context,
            locale,
            XKB_COMPOSE_COMPILE_NO_FLAGS);
    }
    context->work_items = calloc(
        effective.maximum_work_items,
        sizeof(*context->work_items));
    if (context->work_items == NULL) {
        boxrig_wayland_context_destroy(context);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    wrapper = wl_proxy_create_wrapper(display);
    if (wrapper == NULL) {
        boxrig_wayland_context_destroy(context);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    wl_proxy_set_queue(wrapper, queue);
    context->registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    if (context->registry == NULL) {
        boxrig_wayland_context_destroy(context);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    wl_proxy_set_queue((struct wl_proxy *)context->registry, queue);
    if (wl_registry_add_listener(
            context->registry,
            &registry_listener,
            context) != 0 ||
        !request_initial_sync(context)) {
        boxrig_wayland_context_destroy(context);
        return BOXRIG_UI_UNAVAILABLE;
    }
    *context_out = context;
    return BOXRIG_UI_OK;
}

void boxrig_wayland_context_destroy(boxrig_wayland_context *context)
{
    size_t index;

    if (context == NULL) {
        return;
    }
    if (context->component_count != 0) {
        return;
    }
    boxrig_wayland_input_destroy_context(context);
    boxrig_wayland_ibus_destroy(context);
    boxrig_wayland_clipboard_destroy_context(context);
    if (context->initial_sync != NULL) {
        wl_callback_destroy(context->initial_sync);
    }
    for (index = context->output_count; index != 0; index--) {
        wl_output_destroy(context->outputs[index - 1]->proxy);
        free(context->outputs[index - 1]);
    }
    free(context->outputs);
    if (context->fractional_scale_manager != NULL) {
        wp_fractional_scale_manager_v1_destroy(
            context->fractional_scale_manager);
    }
    if (context->viewporter != NULL) {
        wp_viewporter_destroy(context->viewporter);
    }
    if (context->subcompositor != NULL) {
        wl_subcompositor_destroy(context->subcompositor);
    }
    if (context->shm != NULL) {
        wl_shm_destroy(context->shm);
    }
    if (context->compositor != NULL) {
        wl_compositor_destroy(context->compositor);
    }
    if (context->registry != NULL) {
        wl_registry_destroy(context->registry);
    }
    free(context->work_items);
    if (context->compose_table != NULL) {
        xkb_compose_table_unref(context->compose_table);
    }
    if (context->xkb_context != NULL) {
        xkb_context_unref(context->xkb_context);
    }
    free(context);
}

boxrig_ui_status boxrig_wayland_context_dispatch(
    boxrig_wayland_context *context,
    size_t maximum_items,
    boxrig_wayland_dispatch_result *result_out)
{
    size_t processed = 0;
    bool input_additional = false;
    bool clipboard_additional = false;

    if (context == NULL || result_out == NULL || maximum_items == 0 ||
        maximum_items > context->config.maximum_work_items) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (wl_display_get_error(context->display) != 0) {
        mark_fatal(context, BOXRIG_WAYLAND_FAILURE_DISPLAY);
    }
    memset(result_out, 0, sizeof(*result_out));
    if (context->state == BOXRIG_WAYLAND_CONTEXT_FATAL) {
        result_out->additional_dispatch = false;
        result_out->flush_required = context->flush_required;
        return BOXRIG_UI_UNAVAILABLE;
    }
    while (processed < maximum_items && context->work_queue_depth != 0) {
        boxrig_wayland_work_item item =
            context->work_items[context->work_queue_head];

        context->work_queue_head =
            (context->work_queue_head + 1) %
            context->config.maximum_work_items;
        context->work_queue_depth--;
        if (item.type == BOXRIG_WAYLAND_WORK_OUTPUT_CHANGE) {
            context->output_work_queued = false;
            boxrig_wayland_terminal_components_process_output_changes(context);
        } else {
            boxrig_wayland_terminal_component_process_scale(item.component);
        }
        processed++;
    }
    {
        size_t idle_subsystems = 0;

        while (processed < maximum_items && idle_subsystems < 2) {
            bool additional = false;
            size_t count;

            if (context->dispatch_clipboard_first) {
                count = boxrig_wayland_clipboard_dispatch(
                    context,
                    1,
                    &additional);
                clipboard_additional = additional;
            } else {
                count = boxrig_wayland_input_dispatch(
                    context,
                    1,
                    &additional);
                input_additional = additional;
            }
            context->dispatch_clipboard_first =
                !context->dispatch_clipboard_first;
            if (count == 0) {
                idle_subsystems++;
            } else {
                processed += count;
                idle_subsystems = 0;
            }
        }
    }
    result_out->processed_items = processed;
    result_out->additional_dispatch =
        context->work_queue_depth != 0 || input_additional ||
        clipboard_additional;
    result_out->flush_required = context->flush_required;
    result_out->has_repeat_deadline = boxrig_wayland_input_repeat_deadline(
        context,
        &result_out->repeat_deadline_milliseconds);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_wayland_context_update_monotonic_time(
    boxrig_wayland_context *context,
    uint64_t milliseconds)
{
    if (context == NULL ||
        (context->monotonic_time_set &&
         milliseconds < context->monotonic_milliseconds)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    context->monotonic_milliseconds = milliseconds;
    context->monotonic_time_set = true;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_wayland_context_get_serial(
    const boxrig_wayland_context *context,
    uint32_t seat_global_name,
    boxrig_wayland_serial_kind kind,
    boxrig_wayland_serial *serial_out)
{
    const boxrig_wayland_seat *seat;

    if (context == NULL || serial_out == NULL ||
        kind < BOXRIG_WAYLAND_SERIAL_KEYBOARD_ENTER ||
        kind > BOXRIG_WAYLAND_SERIAL_KEYBOARD_KEY) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    for (seat = context->seats; seat != NULL; seat = seat->next) {
        if (seat->global_name == seat_global_name) {
            if (!seat->serial_valid[kind]) {
                return BOXRIG_UI_UNAVAILABLE;
            }
            *serial_out = seat->serials[kind];
            return BOXRIG_UI_OK;
        }
    }
    return BOXRIG_UI_UNAVAILABLE;
}

void boxrig_wayland_context_note_flushed(boxrig_wayland_context *context)
{
    if (context != NULL) {
        context->flush_required = false;
    }
}

boxrig_ui_status boxrig_wayland_context_get_diagnostic(
    const boxrig_wayland_context *context,
    boxrig_wayland_context_diagnostic *diagnostic_out)
{
    if (context == NULL || diagnostic_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    memset(diagnostic_out, 0, sizeof(*diagnostic_out));
    diagnostic_out->state = context->state;
    diagnostic_out->failure_reason = context->failure_reason;
    diagnostic_out->compositor_version = context->compositor == NULL
                                              ? 0
                                              : wl_proxy_get_version(
                                                    (struct wl_proxy *)
                                                        context->compositor);
    diagnostic_out->shm_version = context->shm == NULL
                                       ? 0
                                       : wl_proxy_get_version(
                                             (struct wl_proxy *)context->shm);
    diagnostic_out->subcompositor_version =
        context->subcompositor == NULL
            ? 0
            : wl_proxy_get_version(
                  (struct wl_proxy *)context->subcompositor);
    diagnostic_out->viewporter_version = context->viewporter == NULL
                                              ? 0
                                              : wl_proxy_get_version(
                                                    (struct wl_proxy *)
                                                        context->viewporter);
    diagnostic_out->fractional_scale_version =
        context->fractional_scale_manager == NULL
            ? 0
            : wl_proxy_get_version(
                  (struct wl_proxy *)context->fractional_scale_manager);
    diagnostic_out->text_input_version = context->text_input_manager == NULL
                                             ? 0
                                             : wl_proxy_get_version(
                                                   (struct wl_proxy *)
                                                       context->text_input_manager);
    diagnostic_out->data_device_manager_version =
        context->data_device_manager == NULL
            ? 0
            : wl_proxy_get_version(
                  (struct wl_proxy *)context->data_device_manager);
    diagnostic_out->primary_selection_manager_version =
        context->primary_selection_manager == NULL
            ? 0
            : wl_proxy_get_version(
                  (struct wl_proxy *)context->primary_selection_manager);
    diagnostic_out->xkb_available = context->xkb_context != NULL;
    if (context->compose_table != NULL) {
        diagnostic_out->input_capabilities |=
            BOXRIG_WAYLAND_INPUT_CAPABILITY_XKB_COMPOSE;
    }
    if (context->text_input_manager != NULL) {
        diagnostic_out->input_capabilities |=
            BOXRIG_WAYLAND_INPUT_CAPABILITY_TEXT_INPUT_V3;
    }
    if (context->data_device_manager != NULL) {
        diagnostic_out->clipboard_capabilities |=
            BOXRIG_WAYLAND_CLIPBOARD_CAPABILITY_CLIPBOARD;
    }
    if (context->primary_selection_manager != NULL) {
        diagnostic_out->clipboard_capabilities |=
            BOXRIG_WAYLAND_CLIPBOARD_CAPABILITY_PRIMARY;
    }
    {
        const boxrig_wayland_seat *seat;

        for (seat = context->seats; seat != NULL; seat = seat->next) {
            if (seat->keymap_available) {
                diagnostic_out->input_capabilities |=
                    BOXRIG_WAYLAND_INPUT_CAPABILITY_KEYBOARD;
            }
            if (seat->pointer != NULL) {
                diagnostic_out->input_capabilities |=
                    BOXRIG_WAYLAND_INPUT_CAPABILITY_POINTER;
            }
            if (seat->repeat_active) {
                diagnostic_out->active_repeat_count++;
            }
            if (seat->text_enabled && seat->text_component != NULL) {
                diagnostic_out->active_text_input_count++;
            }
        }
    }
    diagnostic_out->argb8888 = context->argb8888;
    diagnostic_out->xrgb8888 = context->xrgb8888;
    diagnostic_out->global_generation = context->global_generation;
    diagnostic_out->component_count = context->component_count;
    diagnostic_out->seat_count = context->seat_count;
    diagnostic_out->seat_high_water = context->seat_high_water;
    diagnostic_out->unavailable_seat_count = context->unavailable_seat_count;
    diagnostic_out->keymap_failure_count = context->keymap_failure_count;
    diagnostic_out->pending_callbacks = context->initial_sync == NULL ? 0 : 1;
    diagnostic_out->work_queue_depth = context->work_queue_depth;
    diagnostic_out->work_queue_high_water = context->work_queue_high_water;
    diagnostic_out->active_clipboard_transfers =
        context->active_clipboard_transfers;
    diagnostic_out->clipboard_transfer_high_water =
        context->clipboard_transfer_high_water;
    diagnostic_out->outgoing_clipboard_bytes =
        context->outgoing_clipboard_bytes;
    diagnostic_out->outgoing_clipboard_bytes_high_water =
        context->outgoing_clipboard_bytes_high_water;
    boxrig_wayland_clipboard_get_context_diagnostic(
        context,
        diagnostic_out);
    boxrig_wayland_ibus_diagnostic(context, diagnostic_out);
    return BOXRIG_UI_OK;
}

static boxrig_ui_status read_animation_time(void *context, uint64_t *time_out)
{
    (void)context;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1 || now.tv_sec < 0 ||
        now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    uint64_t seconds = (uint64_t)now.tv_sec;
    uint64_t milliseconds = (uint64_t)now.tv_nsec / UINT64_C(1000000);
    if (seconds > (UINT64_MAX - milliseconds) / UINT64_C(1000)) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    *time_out = seconds * UINT64_C(1000) + milliseconds;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_wayland_animation_poll_create(
    boxrig_ui_animation_poll **adapter_out)
{
    return boxrig_ui_animation_poll_create(read_animation_time, NULL, adapter_out);
}
