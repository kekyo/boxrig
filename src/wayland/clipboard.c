/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/input_text.h"

#include "component_private.h"
#include "primary-selection-v1-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef enum boxrig_wayland_clipboard_offer_target {
    BOXRIG_WAYLAND_OFFER_CLIPBOARD = 0,
    BOXRIG_WAYLAND_OFFER_PRIMARY
} boxrig_wayland_clipboard_offer_target;

typedef enum boxrig_wayland_clipboard_transfer_kind {
    BOXRIG_WAYLAND_TRANSFER_OUTGOING = 0,
    BOXRIG_WAYLAND_TRANSFER_INCOMING
} boxrig_wayland_clipboard_transfer_kind;

typedef enum boxrig_wayland_paste_state {
    BOXRIG_WAYLAND_PASTE_READING = 0,
    BOXRIG_WAYLAND_PASTE_BEGIN,
    BOXRIG_WAYLAND_PASTE_CHUNK,
    BOXRIG_WAYLAND_PASTE_END
} boxrig_wayland_paste_state;

struct boxrig_wayland_clipboard_offer {
    boxrig_wayland_clipboard_offer *next;
    boxrig_wayland_seat *seat;
    boxrig_wayland_clipboard_offer_target target;
    union {
        struct wl_data_offer *clipboard;
        struct zwp_primary_selection_offer_v1 *primary;
    } proxy;
    unsigned int mime_preference;
};

struct boxrig_wayland_clipboard_source {
    boxrig_wayland_clipboard_source *next;
    boxrig_wayland_context *context;
    boxrig_wayland_seat *seat;
    boxrig_wayland_terminal_component *component;
    boxrig_wayland_clipboard_offer_target target;
    boxrig_text_snapshot *snapshot;
    size_t length;
    size_t reference_count;
    bool owned;
    union {
        struct wl_data_source *clipboard;
        struct zwp_primary_selection_source_v1 *primary;
    } proxy;
};

struct boxrig_wayland_clipboard_transfer {
    boxrig_wayland_clipboard_transfer *next;
    boxrig_wayland_context *context;
    boxrig_wayland_clipboard_source *source;
    boxrig_wayland_clipboard_offer *offer;
    boxrig_wayland_terminal_component *component;
    boxrig_wayland_seat *seat;
    boxrig_wayland_clipboard_transfer_kind kind;
    boxrig_wayland_clipboard_offer_target target;
    boxrig_wayland_clipboard_mime mime;
    boxrig_wayland_paste_state paste_state;
    uint64_t identifier;
    uint8_t *bytes;
    size_t length;
    size_t capacity;
    size_t offset;
    int descriptor;
    bool paste_blocked;
};

static unsigned int mime_preference(const char *mime_type)
{
    if (strcmp(mime_type, "text/plain;charset=utf-8") == 0) {
        return 1;
    }
    if (strcmp(mime_type, "UTF8_STRING") == 0) {
        return 2;
    }
    if (strcmp(mime_type, "text/plain") == 0) {
        return 3;
    }
    return 0;
}

static const char *mime_name(unsigned int preference)
{
    if (preference == 1) {
        return "text/plain;charset=utf-8";
    }
    if (preference == 2) {
        return "UTF8_STRING";
    }
    if (preference == 3) {
        return "text/plain";
    }
    return NULL;
}

static boxrig_wayland_clipboard_mime mime_value(unsigned int preference)
{
    if (preference == 1) {
        return BOXRIG_WAYLAND_CLIPBOARD_MIME_TEXT_UTF8;
    }
    if (preference == 2) {
        return BOXRIG_WAYLAND_CLIPBOARD_MIME_UTF8_STRING;
    }
    if (preference == 3) {
        return BOXRIG_WAYLAND_CLIPBOARD_MIME_TEXT;
    }
    return BOXRIG_WAYLAND_CLIPBOARD_MIME_NONE;
}

static uint32_t public_target(
    boxrig_wayland_clipboard_offer_target target)
{
    return target == BOXRIG_WAYLAND_OFFER_CLIPBOARD
               ? BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD
               : BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
}

static void note_operation(
    boxrig_wayland_context *context,
    const boxrig_wayland_seat *seat,
    boxrig_wayland_clipboard_offer_target target,
    boxrig_wayland_clipboard_mime mime,
    boxrig_wayland_clipboard_failure failure)
{
    context->clipboard_seat_global_name =
        seat == NULL ? 0 : seat->global_name;
    context->clipboard_target = public_target(target);
    context->clipboard_mime = mime;
    context->clipboard_failure = failure;
}

static void note_offer_mime(
    boxrig_wayland_clipboard_offer *offer,
    const char *mime_type)
{
    unsigned int preference = mime_preference(mime_type);

    if (preference != 0 &&
        (offer->mime_preference == 0 ||
         preference < offer->mime_preference)) {
        offer->mime_preference = preference;
    }
}

static void unlink_source(boxrig_wayland_clipboard_source *source)
{
    boxrig_wayland_clipboard_source **link =
        &source->context->clipboard_sources;

    while (*link != NULL && *link != source) {
        link = &(*link)->next;
    }
    if (*link == source) {
        *link = source->next;
    }
}

static void release_source(boxrig_wayland_clipboard_source *source)
{
    if (source == NULL) {
        return;
    }
    if (source->reference_count > 1) {
        source->reference_count--;
        return;
    }
    source->reference_count = 0;
    unlink_source(source);
    if (source->length > source->context->outgoing_clipboard_bytes) {
        source->context->outgoing_clipboard_bytes = 0;
    } else {
        source->context->outgoing_clipboard_bytes -= source->length;
    }
    boxrig_text_snapshot_destroy(source->snapshot);
    free(source);
}

static void cancel_source(boxrig_wayland_clipboard_source *source)
{
    if (source == NULL || !source->owned) {
        return;
    }
    source->owned = false;
    if (source->seat->clipboard_source == source) {
        source->seat->clipboard_source = NULL;
    }
    if (source->seat->primary_selection_source == source) {
        source->seat->primary_selection_source = NULL;
    }
    if (source->target == BOXRIG_WAYLAND_OFFER_CLIPBOARD) {
        if (source->proxy.clipboard != NULL) {
            wl_data_source_destroy(source->proxy.clipboard);
            source->proxy.clipboard = NULL;
        }
    } else {
        if (source->proxy.primary != NULL) {
            zwp_primary_selection_source_v1_destroy(source->proxy.primary);
            source->proxy.primary = NULL;
        }
    }
    release_source(source);
}

static bool configure_transfer_descriptor(int descriptor)
{
    int flags = fcntl(descriptor, F_GETFL);
    int descriptor_flags;

    if (flags == -1 ||
        fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == -1) {
        return false;
    }
    descriptor_flags = fcntl(descriptor, F_GETFD);
    return descriptor_flags != -1 &&
           fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) != -1;
}

static void unlink_transfer(boxrig_wayland_clipboard_transfer *transfer)
{
    boxrig_wayland_clipboard_transfer **link =
        &transfer->context->clipboard_transfers;

    while (*link != NULL && *link != transfer) {
        link = &(*link)->next;
    }
    if (*link == transfer) {
        *link = transfer->next;
    }
}

static void destroy_transfer(boxrig_wayland_clipboard_transfer *transfer)
{
    if (transfer == NULL) {
        return;
    }
    unlink_transfer(transfer);
    if (transfer->descriptor >= 0) {
        close(transfer->descriptor);
    }
    if (transfer->context->active_clipboard_transfers != 0) {
        transfer->context->active_clipboard_transfers--;
    }
    if (transfer->kind == BOXRIG_WAYLAND_TRANSFER_OUTGOING) {
        release_source(transfer->source);
    } else {
        if (transfer->length > transfer->context->incoming_clipboard_bytes) {
            transfer->context->incoming_clipboard_bytes = 0;
        } else {
            transfer->context->incoming_clipboard_bytes -= transfer->length;
        }
        free(transfer->bytes);
    }
    free(transfer);
}

static void create_outgoing_transfer(
    boxrig_wayland_clipboard_source *source,
    const char *mime_type,
    int descriptor)
{
    boxrig_wayland_context *context = source->context;
    boxrig_wayland_clipboard_transfer *transfer;
    unsigned int preference = mime_preference(mime_type);

    if (preference == 0 || !source->owned ||
        context->state == BOXRIG_WAYLAND_CONTEXT_FATAL ||
        context->state == BOXRIG_WAYLAND_CONTEXT_QUIESCED ||
        context->active_clipboard_transfers ==
            context->config.maximum_clipboard_transfers ||
        !configure_transfer_descriptor(descriptor)) {
        note_operation(
            context,
            source->seat,
            source->target,
            mime_value(preference),
            preference == 0
                ? BOXRIG_WAYLAND_CLIPBOARD_FAILURE_MIME_UNSUPPORTED
                : context->active_clipboard_transfers ==
                          context->config.maximum_clipboard_transfers
                      ? BOXRIG_WAYLAND_CLIPBOARD_FAILURE_BUSY
                      : BOXRIG_WAYLAND_CLIPBOARD_FAILURE_IO);
        close(descriptor);
        return;
    }
    transfer = calloc(1, sizeof(*transfer));
    if (transfer == NULL) {
        close(descriptor);
        return;
    }
    context->next_clipboard_transfer_identifier++;
    if (context->next_clipboard_transfer_identifier == 0) {
        context->next_clipboard_transfer_identifier++;
    }
    transfer->context = context;
    transfer->source = source;
    transfer->seat = source->seat;
    transfer->component = source->component;
    transfer->kind = BOXRIG_WAYLAND_TRANSFER_OUTGOING;
    transfer->target = source->target;
    transfer->mime = mime_value(preference);
    transfer->identifier = context->next_clipboard_transfer_identifier;
    transfer->descriptor = descriptor;
    transfer->next = context->clipboard_transfers;
    context->clipboard_transfers = transfer;
    source->reference_count++;
    context->active_clipboard_transfers++;
    if (context->active_clipboard_transfers >
        context->clipboard_transfer_high_water) {
        context->clipboard_transfer_high_water =
            context->active_clipboard_transfers;
    }
    note_operation(
        context,
        source->seat,
        source->target,
        transfer->mime,
        BOXRIG_WAYLAND_CLIPBOARD_FAILURE_NONE);
}

static void data_source_target(
    void *data,
    struct wl_data_source *proxy,
    const char *mime_type)
{
    (void)data;
    (void)proxy;
    (void)mime_type;
}

static void data_source_send(
    void *data,
    struct wl_data_source *proxy,
    const char *mime_type,
    int32_t descriptor)
{
    boxrig_wayland_clipboard_source *source = data;

    (void)proxy;
    create_outgoing_transfer(source, mime_type, descriptor);
}

static void data_source_cancelled(
    void *data,
    struct wl_data_source *proxy)
{
    (void)proxy;
    cancel_source(data);
}

static void data_source_dnd_drop_performed(
    void *data,
    struct wl_data_source *proxy)
{
    (void)data;
    (void)proxy;
}

static void data_source_dnd_finished(
    void *data,
    struct wl_data_source *proxy)
{
    (void)data;
    (void)proxy;
}

static void data_source_action(
    void *data,
    struct wl_data_source *proxy,
    uint32_t action)
{
    (void)data;
    (void)proxy;
    (void)action;
}

static const struct wl_data_source_listener data_source_listener = {
    .target = data_source_target,
    .send = data_source_send,
    .cancelled = data_source_cancelled,
    .dnd_drop_performed = data_source_dnd_drop_performed,
    .dnd_finished = data_source_dnd_finished,
    .action = data_source_action,
};

static void primary_source_send(
    void *data,
    struct zwp_primary_selection_source_v1 *proxy,
    const char *mime_type,
    int32_t descriptor)
{
    boxrig_wayland_clipboard_source *source = data;

    (void)proxy;
    create_outgoing_transfer(source, mime_type, descriptor);
}

static void primary_source_cancelled(
    void *data,
    struct zwp_primary_selection_source_v1 *proxy)
{
    (void)proxy;
    cancel_source(data);
}

static const struct zwp_primary_selection_source_v1_listener
    primary_source_listener = {
        .send = primary_source_send,
        .cancelled = primary_source_cancelled,
};

static boxrig_wayland_seat *find_seat(
    boxrig_wayland_context *context,
    uint32_t global_name)
{
    boxrig_wayland_seat *seat;

    for (seat = context->seats; seat != NULL; seat = seat->next) {
        if (seat->global_name == global_name) {
            return seat;
        }
    }
    return NULL;
}

static bool serial_matches(
    const boxrig_wayland_seat *seat,
    const boxrig_wayland_terminal_component *component,
    const boxrig_wayland_serial *serial)
{
    boxrig_wayland_serial_kind kind;
    const boxrig_wayland_serial *retained;

    if (serial == NULL || serial->seat_global_name != seat->global_name ||
        serial->kind < BOXRIG_WAYLAND_SERIAL_KEYBOARD_ENTER ||
        serial->kind > BOXRIG_WAYLAND_SERIAL_KEYBOARD_KEY) {
        return false;
    }
    kind = serial->kind;
    retained = &seat->serials[kind];
    if (!seat->serial_valid[kind] ||
        seat->serial_components[kind] != component ||
        retained->seat_global_name != serial->seat_global_name ||
        retained->kind != serial->kind || retained->value != serial->value ||
        retained->focus_generation != serial->focus_generation ||
        serial->focus_generation != component->input_generation) {
        return false;
    }
    if (kind == BOXRIG_WAYLAND_SERIAL_KEYBOARD_ENTER ||
        kind == BOXRIG_WAYLAND_SERIAL_KEYBOARD_KEY) {
        return seat->keyboard_component == component;
    }
    return seat->pointer_component == component;
}

static boxrig_ui_status create_selection_source(
    boxrig_wayland_terminal_component *component,
    boxrig_wayland_seat *seat,
    boxrig_wayland_clipboard_offer_target target,
    const boxrig_wayland_serial *serial)
{
    boxrig_wayland_context *context = component->context;
    boxrig_wayland_clipboard_source *source;
    boxrig_text_snapshot *snapshot = NULL;
    size_t length;
    boxrig_ui_status status = boxrig_ui_view_create_selection_snapshot(
        component->view,
        &snapshot);

    if (status != BOXRIG_UI_OK) {
        return status;
    }
    (void)boxrig_text_snapshot_data(snapshot, &length);
    if (length > context->config.maximum_clipboard_bytes -
                     context->outgoing_clipboard_bytes) {
        boxrig_text_snapshot_destroy(snapshot);
        note_operation(
            context,
            seat,
            target,
            BOXRIG_WAYLAND_CLIPBOARD_MIME_TEXT_UTF8,
            BOXRIG_WAYLAND_CLIPBOARD_FAILURE_LIMIT_EXCEEDED);
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    source = calloc(1, sizeof(*source));
    if (source == NULL) {
        boxrig_text_snapshot_destroy(snapshot);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    source->context = context;
    source->seat = seat;
    source->component = component;
    source->target = target;
    source->snapshot = snapshot;
    source->length = length;
    source->reference_count = 1;
    source->owned = true;
    if (target == BOXRIG_WAYLAND_OFFER_CLIPBOARD) {
        source->proxy.clipboard =
            wl_data_device_manager_create_data_source(
                context->data_device_manager);
        if (source->proxy.clipboard == NULL) {
            free(source);
            boxrig_text_snapshot_destroy(snapshot);
            return BOXRIG_UI_OUT_OF_MEMORY;
        }
        wl_proxy_set_queue(
            (struct wl_proxy *)source->proxy.clipboard,
            context->queue);
        if (wl_data_source_add_listener(
                source->proxy.clipboard,
                &data_source_listener,
                source) != 0) {
            wl_data_source_destroy(source->proxy.clipboard);
            free(source);
            boxrig_text_snapshot_destroy(snapshot);
            return BOXRIG_UI_UNAVAILABLE;
        }
        wl_data_source_offer(
            source->proxy.clipboard,
            "text/plain;charset=utf-8");
        wl_data_source_offer(source->proxy.clipboard, "UTF8_STRING");
        wl_data_source_offer(source->proxy.clipboard, "text/plain");
    } else {
        source->proxy.primary =
            zwp_primary_selection_device_manager_v1_create_source(
                context->primary_selection_manager);
        if (source->proxy.primary == NULL) {
            free(source);
            boxrig_text_snapshot_destroy(snapshot);
            return BOXRIG_UI_OUT_OF_MEMORY;
        }
        wl_proxy_set_queue(
            (struct wl_proxy *)source->proxy.primary,
            context->queue);
        if (zwp_primary_selection_source_v1_add_listener(
                source->proxy.primary,
                &primary_source_listener,
                source) != 0) {
            zwp_primary_selection_source_v1_destroy(source->proxy.primary);
            free(source);
            boxrig_text_snapshot_destroy(snapshot);
            return BOXRIG_UI_UNAVAILABLE;
        }
        zwp_primary_selection_source_v1_offer(
            source->proxy.primary,
            "text/plain;charset=utf-8");
        zwp_primary_selection_source_v1_offer(
            source->proxy.primary,
            "UTF8_STRING");
        zwp_primary_selection_source_v1_offer(
            source->proxy.primary,
            "text/plain");
    }
    source->next = context->clipboard_sources;
    context->clipboard_sources = source;
    context->outgoing_clipboard_bytes += length;
    if (context->outgoing_clipboard_bytes >
        context->outgoing_clipboard_bytes_high_water) {
        context->outgoing_clipboard_bytes_high_water =
            context->outgoing_clipboard_bytes;
    }
    if (target == BOXRIG_WAYLAND_OFFER_CLIPBOARD) {
        wl_data_device_set_selection(
            seat->data_device,
            source->proxy.clipboard,
            serial->value);
        seat->clipboard_source = source;
    } else {
        zwp_primary_selection_device_v1_set_selection(
            seat->primary_selection_device,
            source->proxy.primary,
            serial->value);
        seat->primary_selection_source = source;
    }
    context->flush_required = true;
    note_operation(
        context,
        seat,
        target,
        BOXRIG_WAYLAND_CLIPBOARD_MIME_TEXT_UTF8,
        BOXRIG_WAYLAND_CLIPBOARD_FAILURE_NONE);
    return BOXRIG_UI_OK;
}

static bool component_has_incoming_transfer(
    const boxrig_wayland_context *context,
    const boxrig_wayland_terminal_component *component)
{
    const boxrig_wayland_clipboard_transfer *transfer;

    for (transfer = context->clipboard_transfers;
         transfer != NULL;
         transfer = transfer->next) {
        if (transfer->kind == BOXRIG_WAYLAND_TRANSFER_INCOMING &&
            transfer->component == component) {
            return true;
        }
    }
    return false;
}

static boxrig_ui_status create_incoming_transfer(
    boxrig_wayland_terminal_component *component,
    boxrig_wayland_seat *seat,
    boxrig_wayland_clipboard_offer_target target)
{
    boxrig_wayland_context *context = component->context;
    boxrig_wayland_clipboard_offer *offer =
        target == BOXRIG_WAYLAND_OFFER_CLIPBOARD
            ? seat->clipboard_offer
            : seat->primary_selection_offer;
    boxrig_wayland_clipboard_transfer *transfer;
    const char *selected_mime;
    int descriptors[2] = {-1, -1};

    if (component_has_incoming_transfer(context, component)) {
        note_operation(
            context,
            seat,
            target,
            offer == NULL
                ? BOXRIG_WAYLAND_CLIPBOARD_MIME_NONE
                : mime_value(offer->mime_preference),
            BOXRIG_WAYLAND_CLIPBOARD_FAILURE_BUSY);
        return BOXRIG_UI_BUSY;
    }
    if (offer == NULL) {
        note_operation(
            context,
            seat,
            target,
            BOXRIG_WAYLAND_CLIPBOARD_MIME_NONE,
            BOXRIG_WAYLAND_CLIPBOARD_FAILURE_OFFER_UNAVAILABLE);
        return BOXRIG_UI_UNAVAILABLE;
    }
    selected_mime = mime_name(offer->mime_preference);
    if (selected_mime == NULL) {
        note_operation(
            context,
            seat,
            target,
            BOXRIG_WAYLAND_CLIPBOARD_MIME_NONE,
            BOXRIG_WAYLAND_CLIPBOARD_FAILURE_MIME_UNSUPPORTED);
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (context->active_clipboard_transfers ==
        context->config.maximum_clipboard_transfers) {
        note_operation(
            context,
            seat,
            target,
            mime_value(offer->mime_preference),
            BOXRIG_WAYLAND_CLIPBOARD_FAILURE_BUSY);
        return BOXRIG_UI_BUSY;
    }
    if (pipe(descriptors) == -1 ||
        !configure_transfer_descriptor(descriptors[0]) ||
        !configure_transfer_descriptor(descriptors[1])) {
        if (descriptors[0] >= 0) {
            close(descriptors[0]);
        }
        if (descriptors[1] >= 0) {
            close(descriptors[1]);
        }
        note_operation(
            context,
            seat,
            target,
            mime_value(offer->mime_preference),
            BOXRIG_WAYLAND_CLIPBOARD_FAILURE_IO);
        return BOXRIG_UI_UNAVAILABLE;
    }
    transfer = calloc(1, sizeof(*transfer));
    if (transfer == NULL) {
        close(descriptors[0]);
        close(descriptors[1]);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    context->next_clipboard_transfer_identifier++;
    if (context->next_clipboard_transfer_identifier == 0) {
        context->next_clipboard_transfer_identifier++;
    }
    transfer->context = context;
    transfer->offer = offer;
    transfer->component = component;
    transfer->seat = seat;
    transfer->kind = BOXRIG_WAYLAND_TRANSFER_INCOMING;
    transfer->target = target;
    transfer->mime = mime_value(offer->mime_preference);
    transfer->paste_state = BOXRIG_WAYLAND_PASTE_READING;
    transfer->identifier = context->next_clipboard_transfer_identifier;
    transfer->descriptor = descriptors[0];
    transfer->next = context->clipboard_transfers;
    context->clipboard_transfers = transfer;
    context->active_clipboard_transfers++;
    if (context->active_clipboard_transfers >
        context->clipboard_transfer_high_water) {
        context->clipboard_transfer_high_water =
            context->active_clipboard_transfers;
    }
    if (target == BOXRIG_WAYLAND_OFFER_CLIPBOARD) {
        wl_data_offer_receive(
            offer->proxy.clipboard,
            selected_mime,
            descriptors[1]);
    } else {
        zwp_primary_selection_offer_v1_receive(
            offer->proxy.primary,
            selected_mime,
            descriptors[1]);
    }
    close(descriptors[1]);
    context->flush_required = true;
    note_operation(
        context,
        seat,
        target,
        transfer->mime,
        BOXRIG_WAYLAND_CLIPBOARD_FAILURE_NONE);
    return BOXRIG_UI_OK;
}

boxrig_ui_view_action_capabilities
boxrig_wayland_terminal_component_view_action_capabilities(
    const boxrig_wayland_terminal_component *component,
    uint32_t seat_global_name)
{
    boxrig_ui_view_action_capabilities capabilities = {0};
    boxrig_wayland_seat *seat;

    if (component == NULL || !component->running ||
        component->context->state != BOXRIG_WAYLAND_CONTEXT_READY) {
        return capabilities;
    }
    seat = find_seat(component->context, seat_global_name);
    if (seat == NULL) {
        return capabilities;
    }
    if (seat->data_device != NULL) {
        capabilities.copy_targets |=
            BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
        capabilities.paste_targets |=
            BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    }
    if (seat->primary_selection_device != NULL) {
        capabilities.copy_targets |=
            BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
        capabilities.paste_targets |=
            BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
    }
    if (component->input_control.state.phase != BOXRIG_UI_INPUT_ENABLED)
        capabilities.paste_targets = 0;
    return capabilities;
}

boxrig_ui_status boxrig_wayland_terminal_component_apply_view_action(
    boxrig_wayland_terminal_component *component,
    uint32_t seat_global_name,
    const boxrig_wayland_serial *serial,
    const boxrig_ui_view_action *action)
{
    boxrig_ui_view_action_capabilities capabilities;
    boxrig_wayland_seat *seat;

    if (component == NULL || action == NULL ||
        (action->kind != BOXRIG_UI_VIEW_ACTION_COPY &&
         action->kind != BOXRIG_UI_VIEW_ACTION_PASTE &&
         action->kind != BOXRIG_UI_VIEW_ACTION_RECONVERT) ||
        (action->kind == BOXRIG_UI_VIEW_ACTION_RECONVERT
             ? action->clipboard_target != 0
             : (action->clipboard_target !=
                    BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION &&
                action->clipboard_target !=
                    BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD))) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    capabilities =
        boxrig_wayland_terminal_component_view_action_capabilities(
            component,
            seat_global_name);
    if (!boxrig_ui_view_action_supported(&capabilities, action)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    seat = find_seat(component->context, seat_global_name);
    if (action->kind == BOXRIG_UI_VIEW_ACTION_PASTE) {
        return create_incoming_transfer(
            component,
            seat,
            action->clipboard_target == BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD
                ? BOXRIG_WAYLAND_OFFER_CLIPBOARD
                : BOXRIG_WAYLAND_OFFER_PRIMARY);
    }
    if (!serial_matches(seat, component, serial)) {
        note_operation(
            component->context,
            seat,
            action->clipboard_target == BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD
                ? BOXRIG_WAYLAND_OFFER_CLIPBOARD
                : BOXRIG_WAYLAND_OFFER_PRIMARY,
            BOXRIG_WAYLAND_CLIPBOARD_MIME_NONE,
            BOXRIG_WAYLAND_CLIPBOARD_FAILURE_SERIAL_UNAVAILABLE);
        return BOXRIG_UI_UNAVAILABLE;
    }
    return create_selection_source(
        component,
        seat,
        action->clipboard_target == BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD
            ? BOXRIG_WAYLAND_OFFER_CLIPBOARD
            : BOXRIG_WAYLAND_OFFER_PRIMARY,
        serial);
}

static void data_offer_offer(
    void *data,
    struct wl_data_offer *proxy,
    const char *mime_type)
{
    (void)proxy;
    note_offer_mime(data, mime_type);
}

static void data_offer_source_actions(
    void *data,
    struct wl_data_offer *proxy,
    uint32_t source_actions)
{
    (void)data;
    (void)proxy;
    (void)source_actions;
}

static void data_offer_action(
    void *data,
    struct wl_data_offer *proxy,
    uint32_t action)
{
    (void)data;
    (void)proxy;
    (void)action;
}

static const struct wl_data_offer_listener data_offer_listener = {
    .offer = data_offer_offer,
    .source_actions = data_offer_source_actions,
    .action = data_offer_action,
};

static void primary_offer_offer(
    void *data,
    struct zwp_primary_selection_offer_v1 *proxy,
    const char *mime_type)
{
    (void)proxy;
    note_offer_mime(data, mime_type);
}

static const struct zwp_primary_selection_offer_v1_listener
    primary_offer_listener = {
        .offer = primary_offer_offer,
};

static boxrig_wayland_clipboard_offer *find_offer(
    boxrig_wayland_seat *seat,
    boxrig_wayland_clipboard_offer_target target,
    const void *proxy)
{
    boxrig_wayland_clipboard_offer *offer;

    for (offer = seat->clipboard_offers;
         offer != NULL;
         offer = offer->next) {
        if (offer->target == target &&
            ((target == BOXRIG_WAYLAND_OFFER_CLIPBOARD &&
              (const void *)offer->proxy.clipboard == proxy) ||
             (target == BOXRIG_WAYLAND_OFFER_PRIMARY &&
              (const void *)offer->proxy.primary == proxy))) {
            return offer;
        }
    }
    return NULL;
}

static void destroy_incoming_transfers(
    boxrig_wayland_context *context,
    const boxrig_wayland_seat *seat,
    const boxrig_wayland_terminal_component *component,
    const boxrig_wayland_clipboard_offer *offer)
{
    boxrig_wayland_clipboard_transfer *transfer =
        context->clipboard_transfers;

    while (transfer != NULL) {
        boxrig_wayland_clipboard_transfer *next = transfer->next;

        if (transfer->kind == BOXRIG_WAYLAND_TRANSFER_INCOMING &&
            (seat == NULL || transfer->seat == seat) &&
            (component == NULL || transfer->component == component) &&
            (offer == NULL || transfer->offer == offer)) {
            destroy_transfer(transfer);
        }
        transfer = next;
    }
}

static void destroy_offer(boxrig_wayland_clipboard_offer *offer)
{
    boxrig_wayland_clipboard_offer **link;

    if (offer == NULL) {
        return;
    }
    destroy_incoming_transfers(
        offer->seat->context,
        NULL,
        NULL,
        offer);
    link = &offer->seat->clipboard_offers;
    while (*link != NULL && *link != offer) {
        link = &(*link)->next;
    }
    if (*link == offer) {
        *link = offer->next;
    }
    if (offer->seat->clipboard_offer == offer) {
        offer->seat->clipboard_offer = NULL;
    }
    if (offer->seat->primary_selection_offer == offer) {
        offer->seat->primary_selection_offer = NULL;
    }
    if (offer->target == BOXRIG_WAYLAND_OFFER_CLIPBOARD) {
        wl_data_offer_destroy(offer->proxy.clipboard);
    } else {
        zwp_primary_selection_offer_v1_destroy(offer->proxy.primary);
    }
    free(offer);
}

static void destroy_offers_for_target(
    boxrig_wayland_seat *seat,
    boxrig_wayland_clipboard_offer_target target)
{
    boxrig_wayland_clipboard_offer *offer = seat->clipboard_offers;

    while (offer != NULL) {
        boxrig_wayland_clipboard_offer *next = offer->next;

        if (offer->target == target) {
            destroy_offer(offer);
        }
        offer = next;
    }
}

static boxrig_wayland_clipboard_offer *create_offer(
    boxrig_wayland_seat *seat,
    boxrig_wayland_clipboard_offer_target target,
    void *proxy)
{
    boxrig_wayland_clipboard_offer *offer = calloc(1, sizeof(*offer));

    if (offer == NULL) {
        return NULL;
    }
    offer->seat = seat;
    offer->target = target;
    if (target == BOXRIG_WAYLAND_OFFER_CLIPBOARD) {
        offer->proxy.clipboard = proxy;
        wl_proxy_set_queue((struct wl_proxy *)proxy, seat->context->queue);
        if (wl_data_offer_add_listener(
                offer->proxy.clipboard,
                &data_offer_listener,
                offer) != 0) {
            free(offer);
            return NULL;
        }
    } else {
        offer->proxy.primary = proxy;
        wl_proxy_set_queue((struct wl_proxy *)proxy, seat->context->queue);
        if (zwp_primary_selection_offer_v1_add_listener(
                offer->proxy.primary,
                &primary_offer_listener,
                offer) != 0) {
            free(offer);
            return NULL;
        }
    }
    offer->next = seat->clipboard_offers;
    seat->clipboard_offers = offer;
    return offer;
}

static void data_device_data_offer(
    void *data,
    struct wl_data_device *device,
    struct wl_data_offer *proxy)
{
    boxrig_wayland_seat *seat = data;

    (void)device;
    if (create_offer(seat, BOXRIG_WAYLAND_OFFER_CLIPBOARD, proxy) == NULL) {
        wl_data_offer_destroy(proxy);
    }
}

static void data_device_enter(
    void *data,
    struct wl_data_device *device,
    uint32_t serial,
    struct wl_surface *surface,
    wl_fixed_t x,
    wl_fixed_t y,
    struct wl_data_offer *proxy)
{
    boxrig_wayland_seat *seat = data;
    boxrig_wayland_clipboard_offer *offer = find_offer(
        seat,
        BOXRIG_WAYLAND_OFFER_CLIPBOARD,
        proxy);

    (void)device;
    (void)serial;
    (void)surface;
    (void)x;
    (void)y;
    if (offer != seat->clipboard_offer) {
        destroy_offer(offer);
    }
}

static void data_device_leave(void *data, struct wl_data_device *device)
{
    (void)data;
    (void)device;
}

static void data_device_motion(
    void *data,
    struct wl_data_device *device,
    uint32_t time,
    wl_fixed_t x,
    wl_fixed_t y)
{
    (void)data;
    (void)device;
    (void)time;
    (void)x;
    (void)y;
}

static void data_device_drop(void *data, struct wl_data_device *device)
{
    (void)data;
    (void)device;
}

static void data_device_selection(
    void *data,
    struct wl_data_device *device,
    struct wl_data_offer *proxy)
{
    boxrig_wayland_seat *seat = data;
    boxrig_wayland_clipboard_offer *offer = proxy == NULL
        ? NULL
        : find_offer(seat, BOXRIG_WAYLAND_OFFER_CLIPBOARD, proxy);

    (void)device;
    if (seat->clipboard_offer != offer) {
        destroy_offer(seat->clipboard_offer);
        seat->clipboard_offer = offer;
    }
}

static const struct wl_data_device_listener data_device_listener = {
    .data_offer = data_device_data_offer,
    .enter = data_device_enter,
    .leave = data_device_leave,
    .motion = data_device_motion,
    .drop = data_device_drop,
    .selection = data_device_selection,
};

static void primary_device_data_offer(
    void *data,
    struct zwp_primary_selection_device_v1 *device,
    struct zwp_primary_selection_offer_v1 *proxy)
{
    boxrig_wayland_seat *seat = data;

    (void)device;
    if (create_offer(seat, BOXRIG_WAYLAND_OFFER_PRIMARY, proxy) == NULL) {
        zwp_primary_selection_offer_v1_destroy(proxy);
    }
}

static void primary_device_selection(
    void *data,
    struct zwp_primary_selection_device_v1 *device,
    struct zwp_primary_selection_offer_v1 *proxy)
{
    boxrig_wayland_seat *seat = data;
    boxrig_wayland_clipboard_offer *offer = proxy == NULL
        ? NULL
        : find_offer(seat, BOXRIG_WAYLAND_OFFER_PRIMARY, proxy);

    (void)device;
    if (seat->primary_selection_offer != offer) {
        destroy_offer(seat->primary_selection_offer);
        seat->primary_selection_offer = offer;
    }
}

static const struct zwp_primary_selection_device_v1_listener
    primary_device_listener = {
        .data_offer = primary_device_data_offer,
        .selection = primary_device_selection,
};

static bool create_data_device(boxrig_wayland_seat *seat)
{
    if (seat->context->data_device_manager == NULL ||
        seat->data_device != NULL) {
        return true;
    }
    seat->data_device = wl_data_device_manager_get_data_device(
        seat->context->data_device_manager,
        seat->proxy);
    if (seat->data_device == NULL) {
        return false;
    }
    wl_proxy_set_queue(
        (struct wl_proxy *)seat->data_device,
        seat->context->queue);
    if (wl_data_device_add_listener(
            seat->data_device,
            &data_device_listener,
            seat) != 0) {
        wl_data_device_destroy(seat->data_device);
        seat->data_device = NULL;
        return false;
    }
    seat->context->flush_required = true;
    return true;
}

static bool create_primary_device(boxrig_wayland_seat *seat)
{
    if (seat->context->primary_selection_manager == NULL ||
        seat->primary_selection_device != NULL) {
        return true;
    }
    seat->primary_selection_device =
        zwp_primary_selection_device_manager_v1_get_device(
            seat->context->primary_selection_manager,
            seat->proxy);
    if (seat->primary_selection_device == NULL) {
        return false;
    }
    wl_proxy_set_queue(
        (struct wl_proxy *)seat->primary_selection_device,
        seat->context->queue);
    if (zwp_primary_selection_device_v1_add_listener(
            seat->primary_selection_device,
            &primary_device_listener,
            seat) != 0) {
        zwp_primary_selection_device_v1_destroy(
            seat->primary_selection_device);
        seat->primary_selection_device = NULL;
        return false;
    }
    seat->context->flush_required = true;
    return true;
}

bool boxrig_wayland_clipboard_create_seat_devices(
    boxrig_wayland_seat *seat)
{
    return seat != NULL && create_data_device(seat) &&
           create_primary_device(seat);
}

static void destroy_sources(
    boxrig_wayland_context *context,
    boxrig_wayland_seat *seat,
    boxrig_wayland_terminal_component *component,
    boxrig_wayland_clipboard_offer_target *target)
{
    boxrig_wayland_clipboard_source *source = context->clipboard_sources;

    while (source != NULL) {
        boxrig_wayland_clipboard_source *next = source->next;

        if ((seat == NULL || source->seat == seat) &&
            (component == NULL || source->component == component) &&
            (target == NULL || source->target == *target)) {
            cancel_source(source);
        }
        source = next;
    }
}

void boxrig_wayland_clipboard_quiesce_component(
    boxrig_wayland_terminal_component *component)
{
    if (component != NULL && component->context != NULL) {
        destroy_sources(
            component->context,
            NULL,
            component,
            NULL);
        destroy_incoming_transfers(
            component->context,
            NULL,
            component,
            NULL);
    }
}

bool boxrig_wayland_clipboard_paste_started(
    const boxrig_wayland_terminal_component *component)
{
    for (const boxrig_wayland_clipboard_transfer *transfer = component->context->clipboard_transfers;
         transfer != NULL; transfer = transfer->next) {
        if (transfer->kind == BOXRIG_WAYLAND_TRANSFER_INCOMING && transfer->component == component &&
            (transfer->paste_state == BOXRIG_WAYLAND_PASTE_CHUNK ||
             transfer->paste_state == BOXRIG_WAYLAND_PASTE_END)) return true;
    }
    return false;
}

void boxrig_wayland_clipboard_cancel_input(boxrig_wayland_terminal_component *component)
{
    destroy_incoming_transfers(component->context, NULL, component, NULL);
}

static void destroy_data_device(boxrig_wayland_seat *seat)
{
    boxrig_wayland_clipboard_offer_target target =
        BOXRIG_WAYLAND_OFFER_CLIPBOARD;

    destroy_sources(seat->context, seat, NULL, &target);
    destroy_offers_for_target(seat, BOXRIG_WAYLAND_OFFER_CLIPBOARD);
    if (seat->data_device != NULL) {
        if (wl_proxy_get_version((struct wl_proxy *)seat->data_device) >=
            WL_DATA_DEVICE_RELEASE_SINCE_VERSION) {
            wl_data_device_release(seat->data_device);
        } else {
            wl_data_device_destroy(seat->data_device);
        }
        seat->data_device = NULL;
        seat->context->flush_required = true;
    }
}

static void destroy_primary_device(boxrig_wayland_seat *seat)
{
    boxrig_wayland_clipboard_offer_target target =
        BOXRIG_WAYLAND_OFFER_PRIMARY;

    destroy_sources(seat->context, seat, NULL, &target);
    destroy_offers_for_target(seat, BOXRIG_WAYLAND_OFFER_PRIMARY);
    if (seat->primary_selection_device != NULL) {
        zwp_primary_selection_device_v1_destroy(
            seat->primary_selection_device);
        seat->primary_selection_device = NULL;
        seat->context->flush_required = true;
    }
}

void boxrig_wayland_clipboard_destroy_seat_devices(
    boxrig_wayland_seat *seat)
{
    if (seat != NULL) {
        destroy_incoming_transfers(
            seat->context,
            seat,
            NULL,
            NULL);
        destroy_data_device(seat);
        destroy_primary_device(seat);
    }
}

bool boxrig_wayland_clipboard_bind_data_manager(
    boxrig_wayland_context *context,
    struct wl_registry *registry,
    uint32_t name,
    uint32_t version)
{
    boxrig_wayland_seat *seat;

    if (context == NULL || registry == NULL || version == 0 ||
        context->data_device_manager != NULL) {
        return false;
    }
    context->data_device_manager = wl_registry_bind(
        registry,
        name,
        &wl_data_device_manager_interface,
        version);
    if (context->data_device_manager == NULL) {
        return false;
    }
    context->data_device_manager_name = name;
    wl_proxy_set_queue(
        (struct wl_proxy *)context->data_device_manager,
        context->queue);
    for (seat = context->seats; seat != NULL; seat = seat->next) {
        if (!create_data_device(seat)) {
            return false;
        }
    }
    context->flush_required = true;
    return true;
}

bool boxrig_wayland_clipboard_bind_primary_manager(
    boxrig_wayland_context *context,
    struct wl_registry *registry,
    uint32_t name,
    uint32_t version)
{
    boxrig_wayland_seat *seat;

    if (context == NULL || registry == NULL || version == 0 ||
        context->primary_selection_manager != NULL) {
        return false;
    }
    context->primary_selection_manager = wl_registry_bind(
        registry,
        name,
        &zwp_primary_selection_device_manager_v1_interface,
        version);
    if (context->primary_selection_manager == NULL) {
        return false;
    }
    context->primary_selection_manager_name = name;
    wl_proxy_set_queue(
        (struct wl_proxy *)context->primary_selection_manager,
        context->queue);
    for (seat = context->seats; seat != NULL; seat = seat->next) {
        if (!create_primary_device(seat)) {
            return false;
        }
    }
    context->flush_required = true;
    return true;
}

bool boxrig_wayland_clipboard_remove_global(
    boxrig_wayland_context *context,
    uint32_t name)
{
    boxrig_wayland_seat *seat;

    if (context == NULL) {
        return false;
    }
    if (name == context->data_device_manager_name) {
        for (seat = context->seats; seat != NULL; seat = seat->next) {
            destroy_data_device(seat);
        }
        wl_data_device_manager_destroy(context->data_device_manager);
        context->data_device_manager = NULL;
        context->data_device_manager_name = 0;
        return true;
    }
    if (name == context->primary_selection_manager_name) {
        for (seat = context->seats; seat != NULL; seat = seat->next) {
            destroy_primary_device(seat);
        }
        zwp_primary_selection_device_manager_v1_destroy(
            context->primary_selection_manager);
        context->primary_selection_manager = NULL;
        context->primary_selection_manager_name = 0;
        return true;
    }
    return false;
}

void boxrig_wayland_clipboard_destroy_context(
    boxrig_wayland_context *context)
{
    boxrig_wayland_clipboard_transfer *transfer;
    boxrig_wayland_clipboard_source *source;

    if (context == NULL) {
        return;
    }
    while ((transfer = context->clipboard_transfers) != NULL) {
        destroy_transfer(transfer);
    }
    while ((source = context->clipboard_sources) != NULL) {
        cancel_source(source);
    }
    if (context->primary_selection_manager != NULL) {
        zwp_primary_selection_device_manager_v1_destroy(
            context->primary_selection_manager);
        context->primary_selection_manager = NULL;
    }
    if (context->data_device_manager != NULL) {
        wl_data_device_manager_destroy(context->data_device_manager);
        context->data_device_manager = NULL;
    }
}

boxrig_ui_status boxrig_wayland_context_prepare_wait_sources(
    const boxrig_wayland_context *context,
    boxrig_wayland_wait_source *sources,
    size_t capacity,
    size_t *count_out)
{
    const boxrig_wayland_clipboard_transfer *transfer;
    size_t count = 0;

    if (context == NULL || count_out == NULL ||
        (sources == NULL && capacity != 0)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    for (transfer = context->clipboard_transfers;
         transfer != NULL;
         transfer = transfer->next) {
        if (transfer->descriptor >= 0) {
            count++;
        }
    }
    *count_out = count;
    if (count > capacity) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    count = 0;
    for (transfer = context->clipboard_transfers;
         transfer != NULL;
         transfer = transfer->next) {
        if (transfer->descriptor < 0) {
            continue;
        }
        sources[count].identifier = transfer->identifier;
        sources[count].descriptor = transfer->descriptor;
        sources[count].interests =
            transfer->kind == BOXRIG_WAYLAND_TRANSFER_INCOMING
                ? BOXRIG_WAYLAND_WAIT_READABLE
                : BOXRIG_WAYLAND_WAIT_WRITABLE;
        sources[count].readiness = 0;
        count++;
    }
    return BOXRIG_UI_OK;
}

static boxrig_wayland_clipboard_transfer *find_transfer(
    boxrig_wayland_context *context,
    const boxrig_wayland_wait_source *source)
{
    boxrig_wayland_clipboard_transfer *transfer;

    for (transfer = context->clipboard_transfers;
         transfer != NULL;
         transfer = transfer->next) {
        if (transfer->identifier == source->identifier &&
            transfer->descriptor == source->descriptor) {
            return transfer;
        }
    }
    return NULL;
}

static size_t dispatch_outgoing_transfer(
    boxrig_wayland_clipboard_transfer *transfer)
{
    const uint8_t *bytes;
    size_t length;
    size_t remaining;
    size_t chunk;
    ssize_t written;

    bytes = boxrig_text_snapshot_data(transfer->source->snapshot, &length);
    if (transfer->offset >= length) {
        destroy_transfer(transfer);
        return 0;
    }
    remaining = length - transfer->offset;
    chunk = remaining < transfer->context->config.transfer_chunk_size
                ? remaining
                : transfer->context->config.transfer_chunk_size;
    do {
        written = send(
            transfer->descriptor,
            bytes + transfer->offset,
            chunk,
            MSG_NOSIGNAL);
        if (written == -1 && errno == ENOTSOCK) {
            written = write(
                transfer->descriptor,
                bytes + transfer->offset,
                chunk);
        }
    } while (written == -1 && errno == EINTR);
    if (written > 0) {
        size_t count = (size_t)written;

        transfer->offset += count;
        if (transfer->offset == length) {
            destroy_transfer(transfer);
        }
        return count;
    }
    if (written == -1 &&
        (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    destroy_transfer(transfer);
    return 0;
}

static bool reserve_incoming_bytes(
    boxrig_wayland_clipboard_transfer *transfer,
    size_t required)
{
    size_t limit = transfer->component->config.paste_capacity + 1;
    size_t capacity = transfer->capacity;
    uint8_t *bytes;

    if (required > limit) {
        return false;
    }
    if (required <= capacity) {
        return true;
    }
    if (capacity == 0) {
        capacity = transfer->context->config.transfer_chunk_size;
        if (capacity > limit) {
            capacity = limit;
        }
    }
    while (capacity < required) {
        if (capacity > limit / 2) {
            capacity = limit;
        } else {
            capacity *= 2;
        }
    }
    bytes = realloc(transfer->bytes, capacity);
    if (bytes == NULL) {
        return false;
    }
    transfer->bytes = bytes;
    transfer->capacity = capacity;
    return true;
}

static size_t dispatch_incoming_transfer(
    boxrig_wayland_clipboard_transfer *transfer)
{
    boxrig_wayland_context *context = transfer->context;
    size_t source_limit = transfer->component->config.paste_capacity;
    size_t available = source_limit + 1 - transfer->length;
    size_t chunk = available < context->config.transfer_chunk_size
                       ? available
                       : context->config.transfer_chunk_size;
    ssize_t received;

    if (!reserve_incoming_bytes(transfer, transfer->length + chunk)) {
        note_operation(
            context,
            transfer->seat,
            transfer->target,
            transfer->mime,
            BOXRIG_WAYLAND_CLIPBOARD_FAILURE_LIMIT_EXCEEDED);
        destroy_transfer(transfer);
        return 0;
    }
    do {
        received = read(
            transfer->descriptor,
            transfer->bytes + transfer->length,
            chunk);
    } while (received == -1 && errno == EINTR);
    if (received > 0) {
        size_t count = (size_t)received;

        transfer->length += count;
        context->incoming_clipboard_bytes += count;
        if (context->incoming_clipboard_bytes >
            context->incoming_clipboard_bytes_high_water) {
            context->incoming_clipboard_bytes_high_water =
                context->incoming_clipboard_bytes;
        }
        if (transfer->length > source_limit) {
            note_operation(
                context,
                transfer->seat,
                transfer->target,
                transfer->mime,
                BOXRIG_WAYLAND_CLIPBOARD_FAILURE_LIMIT_EXCEEDED);
            destroy_transfer(transfer);
        }
        return count;
    }
    if (received == -1 &&
        (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    if (received == 0) {
        char *normalized = NULL;
        size_t normalized_length = 0;
        size_t source_length = transfer->length;

        close(transfer->descriptor);
        transfer->descriptor = -1;
        transfer->offer = NULL;
        if (boxrig_input_text_normalize_utf8(
                transfer->bytes,
                transfer->length,
                source_limit,
                &normalized,
                &normalized_length) == -1) {
            note_operation(
                context,
                transfer->seat,
                transfer->target,
                transfer->mime,
                BOXRIG_WAYLAND_CLIPBOARD_FAILURE_INVALID_TEXT);
            destroy_transfer(transfer);
            return 0;
        }
        if (boxrig_ui_view_notify_local_input(transfer->component->view) != BOXRIG_UI_OK) {
            transfer->component->failure_reason = BOXRIG_WAYLAND_FAILURE_INPUT;
            free(normalized);
            destroy_transfer(transfer);
            return 0;
        }
        free(transfer->bytes);
        transfer->bytes = (uint8_t *)normalized;
        transfer->length = normalized_length;
        transfer->capacity = normalized_length;
        if (source_length > context->incoming_clipboard_bytes) {
            context->incoming_clipboard_bytes = 0;
        } else {
            context->incoming_clipboard_bytes -= source_length;
        }
        context->incoming_clipboard_bytes += normalized_length;
        transfer->paste_state = BOXRIG_WAYLAND_PASTE_BEGIN;
        return 0;
    }
    note_operation(
        context,
        transfer->seat,
        transfer->target,
        transfer->mime,
        BOXRIG_WAYLAND_CLIPBOARD_FAILURE_IO);
    destroy_transfer(transfer);
    return 0;
}

boxrig_ui_status boxrig_wayland_context_dispatch_wait_sources(
    boxrig_wayland_context *context,
    const boxrig_wayland_wait_source *sources,
    size_t count,
    size_t maximum_transfers,
    boxrig_wayland_transfer_dispatch_result *result_out)
{
    size_t index;

    if (context == NULL || result_out == NULL || maximum_transfers == 0 ||
        maximum_transfers > context->config.maximum_clipboard_transfers ||
        (sources == NULL && count != 0)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    memset(result_out, 0, sizeof(*result_out));
    if (context->state == BOXRIG_WAYLAND_CONTEXT_FATAL ||
        context->state == BOXRIG_WAYLAND_CONTEXT_QUIESCED) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    for (index = 0; index < count; index++) {
        boxrig_wayland_clipboard_transfer *transfer;

        transfer = find_transfer(context, &sources[index]);
        if (transfer == NULL) {
            continue;
        }
        if ((sources[index].readiness &
             ((transfer->kind == BOXRIG_WAYLAND_TRANSFER_INCOMING
                   ? BOXRIG_WAYLAND_WAIT_READABLE
                   : BOXRIG_WAYLAND_WAIT_WRITABLE) |
              BOXRIG_WAYLAND_WAIT_ERROR)) == 0) {
            continue;
        }
        if (result_out->processed_transfers == maximum_transfers) {
            result_out->additional_dispatch = true;
            continue;
        }
        result_out->processed_transfers++;
        if ((sources[index].readiness & BOXRIG_WAYLAND_WAIT_ERROR) != 0) {
            note_operation(
                context,
                transfer->seat,
                transfer->target,
                transfer->mime,
                BOXRIG_WAYLAND_CLIPBOARD_FAILURE_IO);
            destroy_transfer(transfer);
        } else if (transfer->kind == BOXRIG_WAYLAND_TRANSFER_INCOMING) {
            result_out->processed_bytes +=
                dispatch_incoming_transfer(transfer);
        } else {
            result_out->processed_bytes +=
                dispatch_outgoing_transfer(transfer);
        }
    }
    return BOXRIG_UI_OK;
}

static bool dispatch_paste_transfer(
    boxrig_wayland_clipboard_transfer *transfer)
{
    boxrig_paste_event event;
    boxrig_status status;

    memset(&event, 0, sizeof(event));
    if (transfer->paste_state == BOXRIG_WAYLAND_PASTE_BEGIN) {
        event.phase = BOXRIG_PASTE_BEGIN;
    } else if (transfer->paste_state == BOXRIG_WAYLAND_PASTE_CHUNK) {
        size_t remaining = transfer->length - transfer->offset;

        if (remaining == 0) {
            transfer->paste_state = BOXRIG_WAYLAND_PASTE_END;
            return true;
        }
        event.phase = BOXRIG_PASTE_CHUNK;
        event.text = (const char *)transfer->bytes + transfer->offset;
        event.text_length = boxrig_input_text_chunk_length(
                event.text, remaining, transfer->component->config.paste_chunk_size);
    } else {
        event.phase = BOXRIG_PASTE_END;
    }
    status = boxrig_terminal_send_paste(transfer->component->terminal, &event);
    if (status == BOXRIG_STATUS_WOULD_BLOCK) {
        transfer->paste_blocked = true;
        return false;
    }
    transfer->paste_blocked = false;
    if (status != BOXRIG_STATUS_OK) {
        note_operation(
            transfer->context,
            transfer->seat,
            transfer->target,
            transfer->mime,
            BOXRIG_WAYLAND_CLIPBOARD_FAILURE_IO);
        destroy_transfer(transfer);
        return false;
    }
    if (transfer->paste_state == BOXRIG_WAYLAND_PASTE_BEGIN) {
        transfer->paste_state = BOXRIG_WAYLAND_PASTE_CHUNK;
    } else if (transfer->paste_state == BOXRIG_WAYLAND_PASTE_CHUNK) {
        transfer->offset += event.text_length;
    } else {
        destroy_transfer(transfer);
    }
    return true;
}

size_t boxrig_wayland_clipboard_dispatch(
    boxrig_wayland_context *context,
    size_t maximum_items,
    bool *additional_out)
{
    boxrig_wayland_clipboard_transfer *transfer;
    size_t processed = 0;

    *additional_out = false;
    for (transfer = context->clipboard_transfers;
         transfer != NULL && processed < maximum_items;) {
        boxrig_wayland_clipboard_transfer *next = transfer->next;

        if (transfer->kind == BOXRIG_WAYLAND_TRANSFER_INCOMING &&
            transfer->paste_state != BOXRIG_WAYLAND_PASTE_READING) {
            uint64_t identifier = transfer->identifier;
            int descriptor = transfer->descriptor;
            bool progressed = dispatch_paste_transfer(transfer);

            processed++;
            if (progressed &&
                find_transfer(context, &(boxrig_wayland_wait_source){
                    .identifier = identifier,
                    .descriptor = descriptor,
                }) != NULL) {
                *additional_out = true;
            }
        }
        transfer = next;
    }
    if (processed == maximum_items) {
        for (; transfer != NULL; transfer = transfer->next) {
            if (transfer->kind == BOXRIG_WAYLAND_TRANSFER_INCOMING &&
                transfer->paste_state != BOXRIG_WAYLAND_PASTE_READING &&
                !transfer->paste_blocked) {
                *additional_out = true;
                break;
            }
        }
    }
    return processed;
}

void boxrig_wayland_clipboard_get_context_diagnostic(
    const boxrig_wayland_context *context,
    boxrig_wayland_context_diagnostic *diagnostic)
{
    const boxrig_wayland_clipboard_transfer *transfer;

    diagnostic->clipboard_failure = context->clipboard_failure;
    diagnostic->clipboard_seat_global_name =
        context->clipboard_seat_global_name;
    diagnostic->clipboard_target = context->clipboard_target;
    diagnostic->clipboard_mime = context->clipboard_mime;
    diagnostic->incoming_clipboard_bytes = context->incoming_clipboard_bytes;
    diagnostic->incoming_clipboard_bytes_high_water =
        context->incoming_clipboard_bytes_high_water;
    for (transfer = context->clipboard_transfers;
         transfer != NULL;
         transfer = transfer->next) {
        if (transfer->kind == BOXRIG_WAYLAND_TRANSFER_INCOMING) {
            if (transfer->paste_blocked) {
                diagnostic->clipboard_blocked_reason =
                    BOXRIG_WAYLAND_CLIPBOARD_BLOCKED_TERMINAL;
                return;
            }
            if (transfer->descriptor >= 0) {
                diagnostic->clipboard_blocked_reason =
                    BOXRIG_WAYLAND_CLIPBOARD_BLOCKED_READ;
            }
        } else if (transfer->descriptor >= 0 &&
                   diagnostic->clipboard_blocked_reason ==
                       BOXRIG_WAYLAND_CLIPBOARD_NOT_BLOCKED) {
            diagnostic->clipboard_blocked_reason =
                BOXRIG_WAYLAND_CLIPBOARD_BLOCKED_WRITE;
        }
    }
}

void boxrig_wayland_clipboard_get_component_diagnostic(
    const boxrig_wayland_terminal_component *component,
    boxrig_wayland_component_diagnostic *diagnostic)
{
    const boxrig_wayland_clipboard_transfer *transfer;

    for (transfer = component->context->clipboard_transfers;
         transfer != NULL;
         transfer = transfer->next) {
        if (transfer->kind == BOXRIG_WAYLAND_TRANSFER_INCOMING &&
            transfer->component == component &&
            transfer->paste_state != BOXRIG_WAYLAND_PASTE_READING) {
            diagnostic->paste_active = true;
            diagnostic->paste_blocked = transfer->paste_blocked;
            diagnostic->paste_bytes = transfer->length;
            return;
        }
    }
}

bool boxrig_wayland_clipboard_input_pending(
    const boxrig_wayland_terminal_component *component)
{
    for (const boxrig_wayland_clipboard_transfer *transfer =
             component->context->clipboard_transfers;
         transfer != NULL; transfer = transfer->next) {
        if (transfer->kind == BOXRIG_WAYLAND_TRANSFER_INCOMING &&
            transfer->component == component) {
            return true;
        }
    }
    return false;
}
