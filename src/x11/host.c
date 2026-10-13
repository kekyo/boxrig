/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/x11/host.h"
#include "boxrig/posix.h"

#include <X11/Xatom.h>

#include "boxrig/notification/freedesktop.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

enum {
    CLIPBOARD_SLOT_COUNT = 12,
    TITLE_STACK_CAPACITY = 10,
    TITLE_PROPERTY_MAX_BYTES = 1024 * 1024
};

typedef struct title_value {
    uint8_t *bytes;
    size_t length;
} title_value;

typedef struct title_stack {
    title_value values[TITLE_STACK_CAPACITY];
    size_t count;
} title_stack;

typedef struct clipboard_payload {
    size_t reference_count;
    size_t length;
    uint8_t bytes[];
} clipboard_payload;

typedef struct clipboard_slot {
    Atom atom;
    bool cut_buffer;
    bool owned;
    clipboard_payload *payload;
} clipboard_slot;

struct boxrig_x11_host_adapter {
    Display *display;
    Window toplevel;
    Window clipboard_owner;
    Window view;
    Window root;
    boxrig_ui_host_item_consumer *consumer;
    size_t clipboard_capacity;
    boxrig_x11_host_policy policy;
    Atom utf8_string;
    Atom targets;
    Atom selection_property;
    Atom net_wm_name;
    Atom net_wm_icon_name;
    title_stack window_titles;
    title_stack icon_titles;
    clipboard_slot slots[CLIPBOARD_SLOT_COUNT];
    bool read_pending;
    uint64_t read_request_id;
    Atom read_selection;
    Atom read_target;
    bool window_focused;
    bool window_mapped;
    bool window_visible;
    boxrig_freedesktop_notifications *notifications;
};

static uint64_t monotonic_milliseconds(void *context)
{
    struct timespec now;
    uint64_t seconds;

    (void)context;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1 || now.tv_sec < 0) {
        return 0;
    }
    seconds = (uint64_t)now.tv_sec;
    if (seconds > UINT64_MAX / 1000) {
        return UINT64_MAX;
    }
    return seconds * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

size_t boxrig_x11_host_adapter_poll_descriptor_count(
    const boxrig_x11_host_adapter *adapter)
{
    if (adapter == NULL) return 0;
    const struct pollfd *fds;
    size_t count = 0;
    int timeout = -1;
    if (boxrig_freedesktop_notifications_prepare_poll(
            adapter->notifications, &fds, &count, &timeout) != 0) return 0;
    return count;
}

int boxrig_x11_host_adapter_prepare_poll(boxrig_x11_host_adapter *adapter,
    struct pollfd *descriptors, size_t capacity, size_t *count_out, int *timeout)
{
    if (adapter == NULL || count_out == NULL || (capacity != 0 && descriptors == NULL)) return -1;
    const struct pollfd *fds;
    size_t count;
    if (boxrig_freedesktop_notifications_prepare_poll(
            adapter->notifications, &fds, &count, timeout) != 0 || count > capacity) return -1;
    if (count != 0) memcpy(descriptors, fds, count * sizeof(*fds));
    *count_out = count;
    return 0;
}

int boxrig_x11_host_adapter_process_poll(boxrig_x11_host_adapter *adapter,
    const struct pollfd *descriptors, size_t count)
{
    if (adapter == NULL) return -1;
    boxrig_freedesktop_notifications_set_window_state(adapter->notifications,
        adapter->window_focused, adapter->window_visible);
    return boxrig_freedesktop_notifications_dispatch_poll(adapter->notifications, descriptors, count);
}

static clipboard_payload *clipboard_payload_create(
    const uint8_t *bytes,
    size_t length)
{
    clipboard_payload *payload;

    if (length > SIZE_MAX - sizeof(*payload)) {
        return NULL;
    }
    payload = malloc(sizeof(*payload) + length);
    if (payload == NULL) {
        return NULL;
    }
    payload->reference_count = 1;
    payload->length = length;
    if (length != 0) {
        memcpy(payload->bytes, bytes, length);
    }
    return payload;
}

static void clipboard_payload_retain(clipboard_payload *payload)
{
    payload->reference_count++;
}

static void clipboard_payload_release(clipboard_payload *payload)
{
    if (payload == NULL) {
        return;
    }
    payload->reference_count--;
    if (payload->reference_count == 0) {
        free(payload);
    }
}

static void set_slot_payload(
    clipboard_slot *slot,
    clipboard_payload *payload,
    bool owned)
{
    clipboard_payload_release(slot->payload);
    slot->payload = payload;
    slot->owned = owned;
    clipboard_payload_retain(payload);
}

static clipboard_slot *find_slot(
    boxrig_x11_host_adapter *adapter,
    Atom atom)
{
    size_t index;

    for (index = 0; index < CLIPBOARD_SLOT_COUNT; index++) {
        if (adapter->slots[index].atom == atom) {
            return &adapter->slots[index];
        }
    }
    return NULL;
}

static void title_value_release(title_value *value)
{
    free(value->bytes);
    value->bytes = NULL;
    value->length = 0;
}

static int title_value_copy(
    title_value *value,
    const uint8_t *bytes,
    size_t length)
{
    uint8_t *copy = NULL;

    if (length != 0) {
        copy = malloc(length);
        if (copy == NULL) {
            return -1;
        }
        memcpy(copy, bytes, length);
    }
    value->bytes = copy;
    value->length = length;
    return 0;
}

static int read_title(
    boxrig_x11_host_adapter *adapter,
    bool icon,
    title_value *value)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *property = NULL;
    char *legacy = NULL;
    int status = XGetWindowProperty(
        adapter->display,
        adapter->toplevel,
        icon ? adapter->net_wm_icon_name : adapter->net_wm_name,
        0,
        (long)(TITLE_PROPERTY_MAX_BYTES / 4),
        False,
        AnyPropertyType,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &property);
    int result;

    if (status == Success && actual_type != None && actual_format == 8 &&
        bytes_after == 0 && item_count <= TITLE_PROPERTY_MAX_BYTES) {
        result = title_value_copy(
            value,
            property,
            (size_t)item_count);
        if (property != NULL) {
            XFree(property);
        }
        return result;
    }
    if (property != NULL) {
        XFree(property);
    }
    status = icon ? XGetIconName(adapter->display, adapter->toplevel, &legacy)
                  : XFetchName(adapter->display, adapter->toplevel, &legacy);
    if (status == 0 || legacy == NULL) {
        return title_value_copy(value, NULL, 0);
    }
    result = title_value_copy(
        value,
        (const uint8_t *)legacy,
        strlen(legacy));
    XFree(legacy);
    return result;
}

static int set_title(
    boxrig_x11_host_adapter *adapter,
    uint32_t flags,
    const uint8_t *bytes,
    size_t length)
{
    char *legacy_title;

    if (length > INT_MAX || length == SIZE_MAX) {
        return -1;
    }
    legacy_title = malloc(length + 1);
    if (legacy_title == NULL) {
        return -1;
    }
    if (length != 0) {
        memcpy(legacy_title, bytes, length);
    }
    legacy_title[length] = '\0';
    if ((flags & BOXRIG_TITLE_WINDOW) != 0) {
        (void)XChangeProperty(
            adapter->display,
            adapter->toplevel,
            adapter->net_wm_name,
            adapter->utf8_string,
            8,
            PropModeReplace,
            bytes,
            (int)length);
        (void)XStoreName(adapter->display, adapter->toplevel, legacy_title);
    }
    if ((flags & BOXRIG_TITLE_ICON) != 0) {
        (void)XChangeProperty(
            adapter->display,
            adapter->toplevel,
            adapter->net_wm_icon_name,
            adapter->utf8_string,
            8,
            PropModeReplace,
            bytes,
            (int)length);
        (void)XSetIconName(adapter->display, adapter->toplevel, legacy_title);
    }
    free(legacy_title);
    XFlush(adapter->display);
    return 0;
}

static int title_stack_push(
    boxrig_x11_host_adapter *adapter,
    title_stack *stack,
    bool icon)
{
    title_value value = {NULL, 0};

    if (read_title(adapter, icon, &value) == -1) {
        return -1;
    }
    if (stack->count == TITLE_STACK_CAPACITY) {
        title_value_release(&stack->values[0]);
        memmove(
            stack->values,
            stack->values + 1,
            (TITLE_STACK_CAPACITY - 1) * sizeof(stack->values[0]));
        stack->count--;
    }
    stack->values[stack->count] = value;
    stack->count++;
    return 0;
}

static int title_stack_pop(
    boxrig_x11_host_adapter *adapter,
    title_stack *stack,
    uint32_t target)
{
    title_value *value;
    int result;

    if (stack->count == 0) {
        return 0;
    }
    value = &stack->values[stack->count - 1];
    result = set_title(adapter, target, value->bytes, value->length);
    title_value_release(value);
    stack->count--;
    return result;
}

static int apply_title(
    boxrig_x11_host_adapter *adapter,
    const boxrig_host_item *item)
{
    uint32_t operation = item->flags &
                         (BOXRIG_TITLE_STACK_PUSH | BOXRIG_TITLE_STACK_POP);

    if (!adapter->policy.allow_title_change) {
        return 0;
    }
    if (operation == (BOXRIG_TITLE_STACK_PUSH | BOXRIG_TITLE_STACK_POP)) {
        return -1;
    }
    if (operation == BOXRIG_TITLE_STACK_PUSH) {
        if ((item->flags & BOXRIG_TITLE_WINDOW) != 0 &&
            title_stack_push(adapter, &adapter->window_titles, false) == -1) {
            return -1;
        }
        if ((item->flags & BOXRIG_TITLE_ICON) != 0 &&
            title_stack_push(adapter, &adapter->icon_titles, true) == -1) {
            return -1;
        }
        return 0;
    }
    if (operation == BOXRIG_TITLE_STACK_POP) {
        if ((item->flags & BOXRIG_TITLE_WINDOW) != 0 &&
            title_stack_pop(
                adapter,
                &adapter->window_titles,
                BOXRIG_TITLE_WINDOW) == -1) {
            return -1;
        }
        if ((item->flags & BOXRIG_TITLE_ICON) != 0 &&
            title_stack_pop(
                adapter,
                &adapter->icon_titles,
                BOXRIG_TITLE_ICON) == -1) {
            return -1;
        }
        return 0;
    }
    return set_title(
        adapter,
        item->flags,
        item->payload,
        item->payload_length);
}

static boxrig_host_request_result write_clipboard(
    boxrig_x11_host_adapter *adapter,
    const boxrig_host_item *item)
{
    static const uint8_t empty = 0;
    clipboard_payload *payload;
    size_t index;
    bool published = false;

    if (!adapter->policy.allow_clipboard_write) {
        return BOXRIG_HOST_REQUEST_DENIED;
    }
    if (item->payload_length > INT_MAX) {
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    payload = clipboard_payload_create(item->payload, item->payload_length);
    if (payload == NULL) {
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    for (index = 0; index < CLIPBOARD_SLOT_COUNT; index++) {
        clipboard_slot *slot = &adapter->slots[index];

        if ((item->flags & (1u << index)) == 0) {
            continue;
        }
        if (slot->cut_buffer) {
            (void)XChangeProperty(
                adapter->display,
                adapter->root,
                slot->atom,
                adapter->utf8_string,
                8,
                PropModeReplace,
                item->payload_length == 0 ? &empty : item->payload,
                (int)item->payload_length);
            set_slot_payload(slot, payload, true);
            published = true;
            continue;
        }
        (void)XSetSelectionOwner(
            adapter->display,
            slot->atom,
            adapter->clipboard_owner,
            CurrentTime);
        if (XGetSelectionOwner(adapter->display, slot->atom) ==
            adapter->clipboard_owner) {
            set_slot_payload(slot, payload, true);
            published = true;
        }
    }
    clipboard_payload_release(payload);
    XFlush(adapter->display);
    return published ? BOXRIG_HOST_REQUEST_SUCCESS
                     : BOXRIG_HOST_REQUEST_UNAVAILABLE;
}

static int consume_and_complete(
    boxrig_x11_host_adapter *adapter,
    uint64_t request_id,
    boxrig_host_request_result result,
    const uint8_t *payload,
    size_t payload_length)
{
    if (boxrig_ui_host_item_consumer_consume(adapter->consumer) !=
            BOXRIG_UI_OK ||
        boxrig_ui_host_item_consumer_complete(
            adapter->consumer,
            request_id,
            result,
            payload,
            payload_length) != BOXRIG_UI_OK) {
        return -1;
    }
    return 0;
}

static int get_property(
    boxrig_x11_host_adapter *adapter,
    Window window,
    Atom property,
    bool delete_property,
    unsigned char **value_out,
    size_t *length_out)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *value = NULL;
    size_t long_length;
    int status;

    *value_out = NULL;
    *length_out = 0;
    long_length = adapter->clipboard_capacity / 4;
    if (adapter->clipboard_capacity % 4 != 0) {
        long_length++;
    }
    if (long_length > LONG_MAX) {
        long_length = LONG_MAX;
    }
    status = XGetWindowProperty(
        adapter->display,
        window,
        property,
        0,
        (long)long_length,
        delete_property ? True : False,
        AnyPropertyType,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &value);
    if (status != Success) {
        return -1;
    }
    if (actual_type == None) {
        if (value != NULL) {
            XFree(value);
        }
        return 0;
    }
    if (actual_format != 8 || bytes_after != 0 ||
        item_count > adapter->clipboard_capacity) {
        if (value != NULL) {
            XFree(value);
        }
        return -1;
    }
    *value_out = value;
    *length_out = (size_t)item_count;
    return 1;
}

static int begin_clipboard_read(
    boxrig_x11_host_adapter *adapter,
    const boxrig_host_item *item)
{
    size_t index;

    if (!adapter->policy.allow_clipboard_read) {
        return consume_and_complete(
            adapter,
            item->request_id,
            BOXRIG_HOST_REQUEST_DENIED,
            NULL,
            0);
    }
    for (index = 0; index < CLIPBOARD_SLOT_COUNT; index++) {
        clipboard_slot *slot = &adapter->slots[index];

        if ((item->flags & (1u << index)) == 0) {
            continue;
        }
        if (slot->cut_buffer) {
            unsigned char *value = NULL;
            size_t length = 0;
            int property_status = get_property(
                adapter,
                adapter->root,
                slot->atom,
                false,
                &value,
                &length);
            int result;

            if (property_status == 0) {
                continue;
            }
            result = consume_and_complete(
                adapter,
                item->request_id,
                property_status == 1 ? BOXRIG_HOST_REQUEST_SUCCESS
                                     : BOXRIG_HOST_REQUEST_UNAVAILABLE,
                property_status == 1 ? value : NULL,
                property_status == 1 ? length : 0);
            if (value != NULL) {
                XFree(value);
            }
            return result;
        }
        if (XGetSelectionOwner(adapter->display, slot->atom) != None) {
            uint64_t request_id = item->request_id;

            if (boxrig_ui_host_item_consumer_consume(adapter->consumer) !=
                BOXRIG_UI_OK) {
                return -1;
            }
            adapter->read_pending = true;
            adapter->read_request_id = request_id;
            adapter->read_selection = slot->atom;
            adapter->read_target = adapter->utf8_string;
            (void)XConvertSelection(
                adapter->display,
                adapter->read_selection,
                adapter->read_target,
                adapter->selection_property,
                adapter->clipboard_owner,
                CurrentTime);
            XFlush(adapter->display);
            return 0;
        }
    }
    return consume_and_complete(
        adapter,
        item->request_id,
        BOXRIG_HOST_REQUEST_UNAVAILABLE,
        NULL,
        0);
}

boxrig_x11_host_policy boxrig_x11_host_policy_default(void)
{
    boxrig_x11_host_policy policy;

    policy.allow_title_change = true;
    policy.allow_clipboard_write = true;
    policy.allow_clipboard_read = false;
    policy.allow_graphics_external = false;
    policy.notification = boxrig_notification_policy_config_default(
        monotonic_milliseconds,
        NULL);
    return policy;
}

int boxrig_x11_host_adapter_create(
    Display *display,
    const boxrig_x11_host_targets *targets,
    boxrig_ui_host_item_consumer *consumer,
    size_t clipboard_capacity,
    const boxrig_x11_host_policy *policy,
    boxrig_x11_host_adapter **adapter_out)
{
    static const Atom cut_buffers[] = {
        XA_CUT_BUFFER0,
        XA_CUT_BUFFER1,
        XA_CUT_BUFFER2,
        XA_CUT_BUFFER3,
        XA_CUT_BUFFER4,
        XA_CUT_BUFFER5,
        XA_CUT_BUFFER6,
        XA_CUT_BUFFER7,
    };
    boxrig_x11_host_adapter *adapter;
    XWindowAttributes attributes;
    Window focused_window = None;
    int focus_revert = RevertToNone;
    size_t index;

    if (display == NULL || targets == NULL || targets->toplevel == None ||
        targets->clipboard_owner == None || targets->view == None ||
        boxrig_ui_host_item_consumer_terminal(consumer) == NULL ||
        policy == NULL || adapter_out == NULL) {
        return -1;
    }
    *adapter_out = NULL;
    adapter = calloc(1, sizeof(*adapter));
    if (adapter == NULL) {
        return -1;
    }
    adapter->display = display;
    adapter->toplevel = targets->toplevel;
    adapter->clipboard_owner = targets->clipboard_owner;
    adapter->view = targets->view;
    adapter->root = RootWindow(display, DefaultScreen(display));
    adapter->consumer = consumer;
    adapter->clipboard_capacity = clipboard_capacity;
    adapter->policy = *policy;
    if (boxrig_freedesktop_notifications_create(
            consumer, &policy->notification,
            &adapter->notifications) == -1) {
        free(adapter);
        return -1;
    }
    if (XGetWindowAttributes(display, targets->view, &attributes) == 0) {
        boxrig_freedesktop_notifications_destroy(adapter->notifications);
        free(adapter);
        return -1;
    }
    XGetInputFocus(display, &focused_window, &focus_revert);
    adapter->window_focused = focused_window == targets->view;
    adapter->window_mapped = attributes.map_state != IsUnmapped;
    adapter->window_visible = adapter->window_mapped;
    adapter->utf8_string = XInternAtom(display, "UTF8_STRING", False);
    adapter->targets = XInternAtom(display, "TARGETS", False);
    adapter->selection_property = XInternAtom(
        display,
        "_BOXRIG_CLIPBOARD",
        False);
    adapter->net_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    adapter->net_wm_icon_name = XInternAtom(
        display,
        "_NET_WM_ICON_NAME",
        False);
    adapter->slots[0].atom = XInternAtom(display, "CLIPBOARD", False);
    adapter->slots[1].atom = XA_PRIMARY;
    adapter->slots[2].atom = XA_SECONDARY;
    adapter->slots[3].atom = XInternAtom(display, "SELECT", False);
    for (index = 0; index < sizeof(cut_buffers) / sizeof(cut_buffers[0]);
         index++) {
        adapter->slots[index + 4].atom = cut_buffers[index];
        adapter->slots[index + 4].cut_buffer = true;
    }
    if (adapter->utf8_string == None || adapter->targets == None ||
        adapter->selection_property == None || adapter->net_wm_name == None ||
        adapter->net_wm_icon_name == None || adapter->slots[0].atom == None ||
        adapter->slots[3].atom == None) {
        boxrig_freedesktop_notifications_destroy(adapter->notifications);
        free(adapter);
        return -1;
    }
    *adapter_out = adapter;
    return 0;
}

int boxrig_x11_host_adapter_process_items(boxrig_x11_host_adapter *adapter)
{
    boxrig_host_item item;

    if (adapter == NULL) {
        return -1;
    }
    while (boxrig_ui_host_item_consumer_peek(adapter->consumer, &item)) {
        if (item.type == BOXRIG_HOST_ITEM_BELL) {
            (void)XBell(adapter->display, 0);
        } else if (item.type == BOXRIG_HOST_ITEM_TITLE) {
            if (apply_title(adapter, &item) == -1) {
                return -1;
            }
        } else if (item.type == BOXRIG_HOST_ITEM_WORKING_DIRECTORY) {
            /* The untrusted URI remains metadata and causes no host action. */
        } else if (item.type == BOXRIG_HOST_ITEM_CLIPBOARD_WRITE) {
            boxrig_host_request_result result = write_clipboard(adapter, &item);

            if (consume_and_complete(
                    adapter,
                    item.request_id,
                    result,
                    NULL,
                    0) == -1) {
                return -1;
            }
            continue;
        } else if (item.type == BOXRIG_HOST_ITEM_CLIPBOARD_READ) {
            if (adapter->read_pending) {
                return 0;
            }
            if (begin_clipboard_read(adapter, &item) == -1) {
                return -1;
            }
            continue;
        } else if (item.type == BOXRIG_HOST_ITEM_NOTIFICATION) {
            if (boxrig_freedesktop_notifications_process(adapter->notifications,
                    adapter->window_focused, adapter->window_visible) != 0) return -1;
            continue;
        } else if (item.type == BOXRIG_HOST_ITEM_GRAPHICS_DATA) {
            uint8_t *bytes = NULL;
            size_t length = 0;
            boxrig_host_request_result result = boxrig_posix_read_graphics_transport(
                item.graphics_transport,
                adapter->policy.allow_graphics_external,
                &bytes,
                &length);
            int completion = consume_and_complete(
                adapter,
                item.request_id,
                result,
                result == BOXRIG_HOST_REQUEST_SUCCESS ? bytes : NULL,
                result == BOXRIG_HOST_REQUEST_SUCCESS ? length : 0);

            free(bytes);
            if (completion == -1) {
                return -1;
            }
            continue;
        } else {
            return -1;
        }
        if (boxrig_ui_host_item_consumer_consume(adapter->consumer) !=
            BOXRIG_UI_OK) {
            return -1;
        }
    }
    XFlush(adapter->display);
    return 0;
}

static int process_selection_request(
    boxrig_x11_host_adapter *adapter,
    const XSelectionRequestEvent *request)
{
    XEvent response;
    clipboard_slot *slot = find_slot(adapter, request->selection);
    Atom property = request->property == None
                        ? request->target
                        : request->property;
    bool success = false;

    if (request->owner != adapter->clipboard_owner) {
        return 0;
    }

    if (slot != NULL && !slot->cut_buffer && slot->owned &&
        slot->payload != NULL &&
        XGetSelectionOwner(adapter->display, request->selection) ==
            adapter->clipboard_owner) {
        if (request->target == adapter->targets) {
            Atom supported[] = {
                adapter->targets,
                adapter->utf8_string,
                XA_STRING,
            };

            (void)XChangeProperty(
                adapter->display,
                request->requestor,
                property,
                XA_ATOM,
                32,
                PropModeReplace,
                (const unsigned char *)supported,
                (int)(sizeof(supported) / sizeof(supported[0])));
            success = true;
        } else if (request->target == adapter->utf8_string ||
                   request->target == XA_STRING) {
            (void)XChangeProperty(
                adapter->display,
                request->requestor,
                property,
                request->target,
                8,
                PropModeReplace,
                slot->payload->bytes,
                (int)slot->payload->length);
            success = true;
        }
    }
    memset(&response, 0, sizeof(response));
    response.xselection.type = SelectionNotify;
    response.xselection.display = request->display;
    response.xselection.requestor = request->requestor;
    response.xselection.selection = request->selection;
    response.xselection.target = request->target;
    response.xselection.property = success ? property : None;
    response.xselection.time = request->time;
    if (XSendEvent(
            adapter->display,
            request->requestor,
            False,
            0,
            &response) == 0) {
        return -1;
    }
    XFlush(adapter->display);
    return 1;
}

static int finish_pending_read(
    boxrig_x11_host_adapter *adapter,
    boxrig_host_request_result result,
    const uint8_t *payload,
    size_t payload_length)
{
    boxrig_ui_status status = boxrig_ui_host_item_consumer_complete(
        adapter->consumer,
        adapter->read_request_id,
        result,
        payload,
        payload_length);

    if (status != BOXRIG_UI_OK) {
        return -1;
    }
    adapter->read_pending = false;
    adapter->read_request_id = 0;
    adapter->read_selection = None;
    adapter->read_target = None;
    return 1;
}

static int process_selection_notify(
    boxrig_x11_host_adapter *adapter,
    const XSelectionEvent *event)
{
    unsigned char *value = NULL;
    size_t length = 0;
    int property_status;
    int result;

    if (!adapter->read_pending ||
        event->requestor != adapter->clipboard_owner ||
        event->selection != adapter->read_selection ||
        event->target != adapter->read_target) {
        return 0;
    }
    if (event->property == None) {
        if (adapter->read_target == adapter->utf8_string) {
            adapter->read_target = XA_STRING;
            (void)XConvertSelection(
                adapter->display,
                adapter->read_selection,
                adapter->read_target,
                adapter->selection_property,
                adapter->clipboard_owner,
                CurrentTime);
            XFlush(adapter->display);
            return 1;
        }
        return finish_pending_read(
            adapter,
            BOXRIG_HOST_REQUEST_UNAVAILABLE,
            NULL,
            0);
    }
    if (event->property != adapter->selection_property) {
        return finish_pending_read(
            adapter,
            BOXRIG_HOST_REQUEST_UNAVAILABLE,
            NULL,
            0);
    }
    property_status = get_property(
        adapter,
        adapter->clipboard_owner,
        event->property,
        true,
        &value,
        &length);
    result = finish_pending_read(
        adapter,
        property_status == 1 ? BOXRIG_HOST_REQUEST_SUCCESS
                             : BOXRIG_HOST_REQUEST_UNAVAILABLE,
        property_status == 1 ? value : NULL,
        property_status == 1 ? length : 0);
    if (value != NULL) {
        XFree(value);
    }
    return result;
}

int boxrig_x11_host_adapter_process_event(
    boxrig_x11_host_adapter *adapter,
    const XEvent *event)
{
    if (adapter == NULL || event == NULL) {
        return -1;
    }
    if (event->type == FocusIn && event->xfocus.window == adapter->view) {
        adapter->window_focused = true;
    } else if (event->type == FocusOut &&
               event->xfocus.window == adapter->view) {
        adapter->window_focused = false;
    } else if (event->type == MapNotify &&
               event->xmap.window == adapter->view) {
        adapter->window_mapped = true;
        adapter->window_visible = true;
    } else if (event->type == UnmapNotify &&
               event->xunmap.window == adapter->view) {
        adapter->window_mapped = false;
        adapter->window_visible = false;
    } else if (event->type == VisibilityNotify &&
               event->xvisibility.window == adapter->view) {
        adapter->window_visible = adapter->window_mapped &&
                                  event->xvisibility.state !=
                                      VisibilityFullyObscured;
    }
    if (event->type == SelectionRequest) {
        return process_selection_request(adapter, &event->xselectionrequest);
    }
    if (event->type == SelectionNotify) {
        return process_selection_notify(adapter, &event->xselection);
    }
    if (event->type == SelectionClear) {
        clipboard_slot *slot = find_slot(
            adapter,
            event->xselectionclear.selection);

        if (event->xselectionclear.window != adapter->clipboard_owner ||
            slot == NULL || slot->cut_buffer) {
            return 0;
        }
        if (XGetSelectionOwner(adapter->display, slot->atom) ==
            adapter->clipboard_owner) {
            return 1;
        }
        slot->owned = false;
        clipboard_payload_release(slot->payload);
        slot->payload = NULL;
        return 1;
    }
    return 0;
}

void boxrig_x11_host_adapter_destroy(boxrig_x11_host_adapter *adapter)
{
    size_t index;

    if (adapter == NULL) {
        return;
    }
    if (adapter->read_pending) {
        (void)boxrig_ui_host_item_consumer_complete(
            adapter->consumer,
            adapter->read_request_id,
            BOXRIG_HOST_REQUEST_UNAVAILABLE,
            NULL,
            0);
    }
    boxrig_freedesktop_notifications_destroy(adapter->notifications);
    for (index = 0; index < CLIPBOARD_SLOT_COUNT; index++) {
        clipboard_slot *slot = &adapter->slots[index];

        if (!slot->cut_buffer && slot->owned &&
            XGetSelectionOwner(adapter->display, slot->atom) ==
                adapter->clipboard_owner) {
            (void)XSetSelectionOwner(
                adapter->display,
                slot->atom,
                None,
                CurrentTime);
        }
        clipboard_payload_release(slot->payload);
    }
    for (index = 0; index < adapter->window_titles.count; index++) {
        title_value_release(&adapter->window_titles.values[index]);
    }
    for (index = 0; index < adapter->icon_titles.count; index++) {
        title_value_release(&adapter->icon_titles.values[index]);
    }
    XFlush(adapter->display);
    free(adapter);
}
