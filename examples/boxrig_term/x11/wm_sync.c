/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "wm_sync.h"

#include <X11/Xatom.h>

#include <limits.h>
#include <stdint.h>
#include <string.h>

static bool root_supports_atom(
    Display *display,
    int screen,
    Atom requested_atom)
{
    Window root = RootWindow(display, screen);
    Atom net_supported =
        XInternAtom(display, "_NET_SUPPORTED", False);
    long offset = 0;

    if (net_supported == None) {
        return false;
    }
    for (;;) {
        Atom actual_type = None;
        int actual_format = 0;
        unsigned long item_count = 0;
        unsigned long bytes_after = 0;
        unsigned char *property = NULL;
        unsigned long index;
        bool found = false;

        if (XGetWindowProperty(
                display,
                root,
                net_supported,
                offset,
                256,
                False,
                XA_ATOM,
                &actual_type,
                &actual_format,
                &item_count,
                &bytes_after,
                &property) != Success ||
            actual_type != XA_ATOM || actual_format != 32) {
            if (property != NULL) {
                XFree(property);
            }
            return false;
        }
        for (index = 0; index < item_count; index++) {
            if (((Atom *)property)[index] == requested_atom) {
                found = true;
                break;
            }
        }
        if (property != NULL) {
            XFree(property);
        }
        if (found) {
            return true;
        }
        if (bytes_after == 0) {
            return false;
        }
        if (item_count == 0 || item_count > (unsigned long)LONG_MAX ||
            offset > LONG_MAX - (long)item_count) {
            return false;
        }
        offset += (long)item_count;
    }
}

int boxrig_term_x11_wm_sync_initialize(
    boxrig_term_x11_wm_sync *sync,
    Display *display,
    int screen,
    Window window,
    Atom delete_window)
{
    Atom protocols[2];
    int protocol_count = 1;
    int event_base;
    int error_base;
    int major_version;
    int minor_version;
    unsigned long counter_property_values[2];
    XSyncValue initial_value;

    if (sync == NULL || display == NULL || screen < 0 || window == None ||
        delete_window == None) {
        return -1;
    }
    memset(sync, 0, sizeof(*sync));
    sync->display = display;
    sync->window = window;
    sync->wm_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    sync->sync_request =
        XInternAtom(display, "_NET_WM_SYNC_REQUEST", False);
    sync->counter_property =
        XInternAtom(display, "_NET_WM_SYNC_REQUEST_COUNTER", False);
    sync->frame_drawn =
        XInternAtom(display, "_NET_WM_FRAME_DRAWN", False);
    sync->frame_timings =
        XInternAtom(display, "_NET_WM_FRAME_TIMINGS", False);
    if (sync->wm_protocols == None || sync->sync_request == None ||
        sync->counter_property == None || sync->frame_drawn == None ||
        sync->frame_timings == None) {
        return -1;
    }
    protocols[0] = delete_window;
    /* Basic synchronization is advertised by the client protocol and counter
     * property. Only extended synchronization is gated by a root capability. */
    if (XSyncQueryExtension(display, &event_base, &error_base) != 0 &&
        XSyncInitialize(display, &major_version, &minor_version) != 0) {
        XSyncIntToValue(&initial_value, 0);
        sync->basic_counter =
            XSyncCreateCounter(display, initial_value);
        sync->extended_counter =
            XSyncCreateCounter(display, initial_value);
        if (sync->basic_counter != None &&
            sync->extended_counter != None) {
            counter_property_values[0] = sync->basic_counter;
            counter_property_values[1] = sync->extended_counter;
            XChangeProperty(
                display,
                window,
                sync->counter_property,
                XA_CARDINAL,
                32,
                PropModeReplace,
                (unsigned char *)counter_property_values,
                2);
            protocols[protocol_count++] = sync->sync_request;
            sync->enabled = true;
            sync->extended_enabled =
                root_supports_atom(display, screen, sync->frame_drawn);
            sync->extended_value = initial_value;
        } else {
            if (sync->basic_counter != None) {
                (void)XSyncDestroyCounter(
                    display,
                    sync->basic_counter);
            }
            if (sync->extended_counter != None) {
                (void)XSyncDestroyCounter(
                    display,
                    sync->extended_counter);
            }
            sync->basic_counter = None;
            sync->extended_counter = None;
        }
    }
    if (XSetWMProtocols(
            display,
            window,
            protocols,
            protocol_count) == 0) {
        if (sync->basic_counter != None ||
            sync->extended_counter != None) {
            XDeleteProperty(display, window, sync->counter_property);
        }
        if (sync->basic_counter != None) {
            (void)XSyncDestroyCounter(
                display,
                sync->basic_counter);
        }
        if (sync->extended_counter != None) {
            (void)XSyncDestroyCounter(
                display,
                sync->extended_counter);
        }
        memset(sync, 0, sizeof(*sync));
        return -1;
    }
    return 0;
}

static int add_to_value(
    XSyncValue value,
    int increment,
    XSyncValue *result_out)
{
    XSyncValue increment_value;
    int overflow = 0;

    XSyncIntToValue(&increment_value, increment);
    XSyncValueAdd(result_out, value, increment_value, &overflow);
    return overflow == 0 ? 0 : -1;
}

static bool value_is_odd(XSyncValue value)
{
    return (XSyncValueLow32(value) & 1u) != 0;
}

static int set_extended_counter(
    boxrig_term_x11_wm_sync *sync,
    XSyncValue value)
{
    if (XSyncSetCounter(
            sync->display,
            sync->extended_counter,
            value) == 0) {
        return -1;
    }
    sync->extended_value = value;
    return 0;
}

static int begin_extended_frame(
    boxrig_term_x11_wm_sync *sync,
    bool minimum_valid,
    XSyncValue minimum)
{
    XSyncValue base = sync->extended_value;
    XSyncValue next;
    XSyncValue completion;

    if (sync->extended_frame_active &&
        (!minimum_valid ||
         XSyncValueGreaterOrEqual(sync->extended_value, minimum))) {
        return 0;
    }
    if (minimum_valid && XSyncValueGreaterThan(minimum, base)) {
        base = minimum;
    }
    if (value_is_odd(base) &&
        XSyncValueGreaterThan(base, sync->extended_value)) {
        next = base;
    } else if (add_to_value(
                   base,
                   value_is_odd(base) ? 2 : 1,
                   &next) == -1) {
        return -1;
    }
    if (!value_is_odd(next) ||
        !XSyncValueGreaterThan(next, sync->extended_value) ||
        add_to_value(next, 1, &completion) == -1 ||
        value_is_odd(completion) ||
        set_extended_counter(sync, next) == -1) {
        return -1;
    }
    sync->extended_frame_active = true;
    return 0;
}

static void settle_extended_frame(
    boxrig_term_x11_wm_sync *sync)
{
    XSyncValue next;

    if (!sync->extended_frame_active ||
        add_to_value(sync->extended_value, 1, &next) == -1 ||
        value_is_odd(next) ||
        set_extended_counter(sync, next) == -1) {
        return;
    }
    sync->extended_frame_active = false;
    sync->extended_completion_pending = false;
    XFlush(sync->display);
}

void boxrig_term_x11_wm_sync_destroy(
    boxrig_term_x11_wm_sync *sync)
{
    if (sync == NULL) {
        return;
    }
    if (sync->display != NULL) {
        settle_extended_frame(sync);
        if (sync->window != None && sync->counter_property != None &&
            (sync->basic_counter != None ||
             sync->extended_counter != None)) {
            XDeleteProperty(
                sync->display,
                sync->window,
                sync->counter_property);
        }
        if (sync->basic_counter != None) {
            (void)XSyncDestroyCounter(
                sync->display,
                sync->basic_counter);
        }
        if (sync->extended_counter != None) {
            (void)XSyncDestroyCounter(
                sync->display,
                sync->extended_counter);
        }
    }
    memset(sync, 0, sizeof(*sync));
}

bool boxrig_term_x11_wm_sync_process_event(
    boxrig_term_x11_wm_sync *sync,
    const XEvent *event)
{
    XSyncValue requested_value;

    if (sync == NULL || event == NULL || !sync->enabled ||
        event->type != ClientMessage ||
        event->xclient.window != sync->window ||
        event->xclient.format != 32) {
        return false;
    }
    if (event->xclient.message_type == sync->frame_drawn ||
        event->xclient.message_type == sync->frame_timings) {
        return true;
    }
    if (event->xclient.message_type != sync->wm_protocols ||
        (Atom)event->xclient.data.l[0] != sync->sync_request) {
        return false;
    }
    XSyncIntsToValue(
        &requested_value,
        (unsigned int)(uint32_t)event->xclient.data.l[2],
        (int)(uint32_t)event->xclient.data.l[3]);
    if (!XSyncValueIsZero(requested_value)) {
        if (event->xclient.data.l[4] == 0) {
            sync->basic_requested_value = requested_value;
            sync->basic_request_pending = true;
        } else if (event->xclient.data.l[4] == 1) {
            sync->extended_requested_value = requested_value;
            sync->extended_request_pending = true;
        }
    }
    return true;
}

int boxrig_term_x11_wm_sync_begin_frame(
    boxrig_term_x11_wm_sync *sync)
{
    XSyncValue ignored;

    XSyncIntToValue(&ignored, 0);
    if (sync == NULL) {
        return -1;
    }
    if (!sync->enabled || !sync->extended_enabled) {
        return 0;
    }
    return begin_extended_frame(sync, false, ignored);
}

int boxrig_term_x11_wm_sync_begin_configure(
    boxrig_term_x11_wm_sync *sync)
{
    if (sync == NULL) {
        return -1;
    }
    if (!sync->enabled) {
        return 0;
    }
    if (sync->extended_request_pending &&
        begin_extended_frame(
            sync,
            true,
            sync->extended_requested_value) == -1) {
        return -1;
    }
    if (sync->extended_request_pending) {
        sync->extended_request_pending = false;
    }
    if (sync->extended_frame_active) {
        sync->extended_completion_pending = true;
    }
    if (sync->basic_request_pending) {
        sync->basic_frame_value = sync->basic_requested_value;
        sync->basic_frame_pending = true;
        sync->basic_request_pending = false;
    }
    return 0;
}

int boxrig_term_x11_wm_sync_complete_frame(
    boxrig_term_x11_wm_sync *sync)
{
    XSyncValue extended_frame_value;
    bool changed = false;

    if (sync == NULL) {
        return -1;
    }
    if (!sync->enabled ||
        (!sync->basic_frame_pending &&
         !sync->extended_completion_pending)) {
        return 0;
    }
    if (sync->extended_completion_pending &&
        (!sync->extended_frame_active ||
         !value_is_odd(sync->extended_value) ||
         add_to_value(
             sync->extended_value,
             1,
             &extended_frame_value) == -1)) {
        return -1;
    }
    if (sync->basic_frame_pending) {
        if (XSyncSetCounter(
                sync->display,
                sync->basic_counter,
                sync->basic_frame_value) == 0) {
            return -1;
        }
        sync->basic_frame_pending = false;
        changed = true;
    }
    if (sync->extended_completion_pending) {
        if (set_extended_counter(
                sync,
                extended_frame_value) == -1) {
            if (changed) {
                XFlush(sync->display);
            }
            return -1;
        }
        sync->extended_frame_active = false;
        sync->extended_completion_pending = false;
        changed = true;
    }
    /* The component's final copy is queued first on this connection. */
    if (changed) {
        XFlush(sync->display);
    }
    return 0;
}
