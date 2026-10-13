/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "application_internal.h"

#include <errno.h>
#include <glib.h>
#include <limits.h>
#include <string.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static int frame_component(boxrig_term_x11_application *application)
{
    boxrig_ui_component_frame_result frame;
    size_t recognition_work;
    bool recognition_more;

    if (boxrig_term_x11_native_host_process_layout_action(application) == -1 ||
        boxrig_ui_model_synchronize(application->model) != BOXRIG_UI_OK ||
        boxrig_term_font_demo_apply(&application->font_demo) == -1 ||
        boxrig_term_scrollbar_demo_apply(&application->scrollbar_demo) == -1 ||
        boxrig_term_text_policy_resume(
            application->text_policy,
            BOXRIG_TERM_TEXT_POLICY_WORK_BUDGET_DEFAULT,
            &recognition_work,
            &recognition_more) == -1 ||
        boxrig_x11_terminal_component_frame(
            application->component,
            &frame) != BOXRIG_UI_OK ||
        (!frame.frame_pending &&
         boxrig_term_x11_wm_sync_complete_frame(
             &application->wm_sync) == -1)) {
        return -1;
    }
    int pointer_changed = boxrig_term_x11_update_pointer(application);
    if (pointer_changed < 0) return -1;
    return frame.additional_dispatch || recognition_more || pointer_changed != 0 ? 1 : 0;
}

static int apply_pending_resize(
    boxrig_term_x11_application *application,
    boxrig_x11_event_batch *batch)
{
    unsigned long serial;
    unsigned int width;
    unsigned int height;
    int result;

    if (!boxrig_x11_event_batch_take_resize(
            batch,
            &serial,
            &width,
            &height)) {
        return 0;
    }
    if (application->pending_size &&
        serial < application->pending_size_serial) {
        return boxrig_term_x11_native_host_allocate(
            application,
            serial,
            width,
            height);
    }
    if (boxrig_term_x11_wm_sync_begin_configure(
            &application->wm_sync) == -1) {
        return -1;
    }
    result = boxrig_term_x11_native_host_allocate(
        application,
        serial,
        width,
        height);
    return result;
}

static boxrig_ui_component_event_result dispatch_component_event(
    boxrig_term_x11_application *application,
    const XEvent *event)
{
    return boxrig_x11_terminal_component_dispatch_event(
        application->component,
        event);
}

static int retry_blocked_event(boxrig_term_x11_application *application)
{
    boxrig_ui_component_event_result result;

    if (!application->blocked_event_valid) {
        return 0;
    }
    result = boxrig_x11_terminal_component_dispatch_event(
        application->component,
        &application->blocked_event);
    if (result == BOXRIG_UI_COMPONENT_EVENT_BLOCKED) {
        return 1;
    }
    if (result == BOXRIG_UI_COMPONENT_EVENT_FAILURE) {
        return -1;
    }
    application->blocked_event_valid = false;
    return 0;
}

static int process_x_events(boxrig_term_x11_application *application)
{
    boxrig_x11_event_batch batch;
    int retry_result = retry_blocked_event(application);

    if (retry_result != 0) {
        return retry_result < 0 ? -1 : 0;
    }
    boxrig_x11_event_batch_initialize(&batch);
    while (boxrig_ui_lifecycle_accepts_events(&application->lifecycle) &&
           boxrig_x11_event_batch_has_capacity(&batch) &&
           XPending(application->display) > 0) {
        XEvent event;
        bool resize;
        unsigned long resize_serial = 0;
        unsigned int resize_width = 0;
        unsigned int resize_height = 0;
        bool transparency_changed = false;
        int result;
        boxrig_ui_component_event_result component_result;

        XNextEvent(application->display, &event);
        (void)boxrig_x11_background_effect_process_event(application->background_effect, &event);
        if (event.xany.window == application->window) {
            if (event.type == EnterNotify) {
                application->pointer_inside = true;
                application->pointer_x = (boxrig_ui_fixed)event.xcrossing.x * BOXRIG_UI_FIXED_ONE;
                application->pointer_y = (boxrig_ui_fixed)event.xcrossing.y * BOXRIG_UI_FIXED_ONE;
            } else if (event.type == LeaveNotify || event.type == UnmapNotify) {
                application->pointer_inside = false;
                boxrig_term_link_gesture_cancel(application->link_gesture);
            }
        }
        if (event.type == FocusOut && event.xfocus.window == application->window)
            boxrig_term_link_gesture_cancel(application->link_gesture);
        resize = event.type == ConfigureNotify &&
                 event.xconfigure.window == application->window;
        if (event.type == PropertyNotify &&
            event.xproperty.window == application->window &&
            event.xproperty.atom == application->wm_state) {
            XWindowAttributes attributes;

            /* WM state can change without changing the native extent. Feed
             * that real allocation through the same bounded resize path. */
            if (XGetWindowAttributes(application->display, application->window,
                                     &attributes) == 0) {
                return -1;
            }
            resize = true;
            resize_serial = event.xproperty.serial;
            resize_width = (unsigned int)attributes.width;
            resize_height = (unsigned int)attributes.height;
        }
        if (boxrig_x11_event_batch_should_apply_resize(&batch, resize) &&
            apply_pending_resize(application, &batch) == -1) {
            return -1;
        }
        if (event.type == ConfigureNotify && resize) {
            resize_serial = event.xconfigure.serial;
            resize_width = event.xconfigure.width > 0
                               ? (unsigned int)event.xconfigure.width
                               : 1;
            resize_height = event.xconfigure.height > 0
                                ? (unsigned int)event.xconfigure.height
                                : 1;
        }
        if (boxrig_x11_event_batch_record(
                &batch,
                resize,
                resize_serial,
                resize_width,
                resize_height) == -1) {
            return -1;
        }
        if (resize) {
            continue;
        }
        if (event.type == ClientMessage &&
            event.xclient.window == application->window &&
            (Atom)event.xclient.data.l[0] == application->delete_window) {
            if (boxrig_ui_lifecycle_begin_shutdown(
                    &application->lifecycle) == -1) {
                return -1;
            }
            application->window_closed = true;
            break;
        }
        if (boxrig_term_x11_wm_sync_process_event(
                &application->wm_sync,
                &event)) {
            continue;
        }
        if (event.type == DestroyNotify &&
            event.xdestroywindow.window == application->window) {
            if (boxrig_ui_lifecycle_begin_shutdown(
                    &application->lifecycle) == -1) {
                return -1;
            }
            application->window_closed = true;
            application->window_destroyed = true;
            break;
        }
        result = boxrig_x11_transparency_context_process_event(
            application->transparency,
            &event,
            &transparency_changed);
        if (result == -1 ||
            (transparency_changed &&
             boxrig_term_x11_native_host_synchronize_transparency(
                 application) == -1)) {
            return -1;
        }
        if (result == 1) {
            continue;
        }
        result = boxrig_x11_host_adapter_process_event(
            application->host_adapter,
            &event);
        if (result == -1) {
            return -1;
        }
        if (result == 1) {
            continue;
        }
        component_result = dispatch_component_event(application, &event);
        if (component_result == BOXRIG_UI_COMPONENT_EVENT_FAILURE) {
            return -1;
        }
        if (component_result == BOXRIG_UI_COMPONENT_EVENT_BLOCKED) {
            application->blocked_event = event;
            application->blocked_event_valid = true;
            break;
        }
    }
    if (boxrig_ui_lifecycle_accepts_events(&application->lifecycle) &&
        apply_pending_resize(application, &batch) == -1) {
        return -1;
    }
    return 0;
}

static int dispatch_writable_session_now(
    boxrig_term_x11_application *application,
    int pty_descriptor)
{
    boxrig_session_dispatch_result session_result;
    struct pollfd descriptor;
    int poll_result;

    if ((boxrig_session_interests(application->session) &
         BOXRIG_SESSION_INTEREST_WRITE) == 0) {
        return 0;
    }
    descriptor.fd = pty_descriptor;
    descriptor.events = POLLOUT;
    descriptor.revents = 0;
    do {
        poll_result = poll(&descriptor, 1u, 0);
    } while (poll_result == -1 && errno == EINTR);
    if (poll_result == -1 ||
        (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
        return -1;
    }
    if ((descriptor.revents & POLLOUT) == 0) {
        return 0;
    }
    if (boxrig_session_dispatch(
            application->session,
            BOXRIG_SESSION_READY_WRITE,
            &session_result) != BOXRIG_SESSION_OK) {
        return -1;
    }
    application->peer_closed =
        application->peer_closed || session_result.peer_closed;
    return 0;
}

int boxrig_term_x11_run_event_loop(
    boxrig_term_x11_application *application)
{
    int pty_descriptor = boxrig_posix_pty_descriptor(application->pty);
    struct pollfd *descriptors = NULL;
    size_t descriptor_capacity = 0;
    size_t prepared_host_descriptor_count = 0;
    size_t prepared_uri_descriptor_count = 0;
    bool poll_pending = false;
    int result = -1;

    while (boxrig_ui_lifecycle_accepts_events(&application->lifecycle)) {
        boxrig_session_dispatch_result session_result;
        int frame_result;
        uint32_t session_interests;
        uint32_t session_ready = BOXRIG_SESSION_READY_NONE;
        size_t host_descriptor_capacity;
        size_t host_descriptor_count;
        const struct pollfd *uri_descriptors;
        size_t uri_descriptor_count;
        size_t required_descriptor_capacity;
        int timeout_milliseconds = -1;

        if (boxrig_ui_animation_poll_dispatch(application->animation_loop) != BOXRIG_UI_OK) {
            goto cleanup;
        }
        if (poll_pending) {
            if (boxrig_term_uri_launcher_process_poll(application->uri_launcher,
                    descriptors + 2 + prepared_host_descriptor_count,
                    prepared_uri_descriptor_count) == -1) goto cleanup;
            if (boxrig_x11_host_adapter_process_poll(
                    application->host_adapter,
                    descriptors + 2,
                    prepared_host_descriptor_count) == -1 ||
                (descriptors[1].revents & (POLLERR | POLLNVAL)) != 0) {
                goto cleanup;
            }
            if ((descriptors[1].revents & (POLLIN | POLLHUP)) != 0) {
                session_ready |= BOXRIG_SESSION_READY_READ;
            }
            if ((descriptors[1].revents & POLLOUT) != 0) {
                session_ready |= BOXRIG_SESSION_READY_WRITE;
            }
            poll_pending = false;
        }
        if (boxrig_session_dispatch(
                application->session,
                session_ready,
                &session_result) != BOXRIG_SESSION_OK ||
            boxrig_x11_host_adapter_process_items(
                application->host_adapter) == -1) {
            goto cleanup;
        }
        application->peer_closed =
            application->peer_closed || session_result.peer_closed;
        if (process_x_events(application) == -1 ||
            dispatch_writable_session_now(
                application,
                pty_descriptor) == -1) {
            goto cleanup;
        }
        frame_result = frame_component(application);
        if (frame_result == -1) {
            goto cleanup;
        }
        if (!boxrig_ui_lifecycle_accepts_events(&application->lifecycle)) {
            break;
        }
        if (application->peer_closed) {
            if (boxrig_ui_lifecycle_begin_shutdown(
                    &application->lifecycle) == -1) {
                goto cleanup;
            }
            break;
        }
        if (frame_result == 1) {
            /* Keep component progress immediate without skipping readiness
             * collection for PTY and X11 traffic. */
            timeout_milliseconds = 0;
        }
        session_interests = boxrig_session_interests(application->session);
        if (XPending(application->display) > 0 ||
            ((session_interests & BOXRIG_SESSION_INTEREST_DISPATCH) != 0 &&
             (session_interests & BOXRIG_SESSION_INTEREST_WRITE) == 0)) {
            continue;
        }
        /* Process queued native transitions before deciding that a click is
         * single. A delayed double-click event still owns the selection. */
        uint64_t now = (uint64_t)(g_get_monotonic_time() / 1000);
        if (boxrig_term_link_gesture_dispatch(application->link_gesture, now) == -1)
            fprintf(stderr, "boxrig_term: could not confirm URL click.\n");
        uint64_t deadline;
        if (boxrig_term_link_gesture_deadline(application->link_gesture, &deadline)) {
            int delay = deadline <= now ? 0 : deadline - now > INT_MAX ? INT_MAX : (int)(deadline - now);
            if (timeout_milliseconds < 0 || delay < timeout_milliseconds) timeout_milliseconds = delay;
        }
        if (boxrig_term_uri_launcher_prepare_poll(application->uri_launcher,
                &uri_descriptors, &uri_descriptor_count, &timeout_milliseconds) == -1) goto cleanup;
        host_descriptor_capacity =
            boxrig_x11_host_adapter_poll_descriptor_count(
                application->host_adapter);
        if (uri_descriptor_count > SIZE_MAX / sizeof(*descriptors) - 2 ||
            host_descriptor_capacity > SIZE_MAX / sizeof(*descriptors) - 2 - uri_descriptor_count) {
            goto cleanup;
        }
        required_descriptor_capacity = 2 + host_descriptor_capacity + uri_descriptor_count;
        if (required_descriptor_capacity > descriptor_capacity) {
            struct pollfd *new_descriptors = realloc(
                descriptors,
                required_descriptor_capacity * sizeof(*descriptors));

            if (new_descriptors == NULL) {
                goto cleanup;
            }
            descriptors = new_descriptors;
            descriptor_capacity = required_descriptor_capacity;
        }
        descriptors[0].fd = ConnectionNumber(application->display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = pty_descriptor;
        descriptors[1].events = 0;
        if ((session_interests & BOXRIG_SESSION_INTEREST_READ) != 0) {
            descriptors[1].events |= POLLIN;
        }
        if ((session_interests & BOXRIG_SESSION_INTEREST_WRITE) != 0) {
            descriptors[1].events |= POLLOUT;
        }
        descriptors[1].revents = 0;
        if (boxrig_ui_animation_poll_prepare_wait(
                application->animation_loop, timeout_milliseconds,
                &timeout_milliseconds) != BOXRIG_UI_OK ||
            boxrig_x11_host_adapter_prepare_poll(
                application->host_adapter,
                descriptors + 2,
                descriptor_capacity - 2 - uri_descriptor_count,
                &host_descriptor_count,
                &timeout_milliseconds) == -1) {
            goto cleanup;
        }
        memcpy(descriptors + 2 + host_descriptor_count, uri_descriptors,
            uri_descriptor_count * sizeof(*descriptors));
        if (boxrig_x11_wait_for_activity(
                application->display,
                descriptors,
                (nfds_t)(2 + host_descriptor_count + uri_descriptor_count),
                timeout_milliseconds) == -1) {
            goto cleanup;
        }
        prepared_host_descriptor_count = host_descriptor_count;
        prepared_uri_descriptor_count = uri_descriptor_count;
        poll_pending = true;
    }
    result = 0;

cleanup:
    free(descriptors);
    return result;
}
