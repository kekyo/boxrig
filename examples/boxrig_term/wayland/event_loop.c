/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "application_internal.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static int monotonic_milliseconds(uint64_t *milliseconds_out)
{
    struct timespec now;
    uint64_t seconds;
    uint64_t milliseconds;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1 || now.tv_sec < 0 ||
        now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) {
        return -1;
    }
    seconds = (uint64_t)now.tv_sec;
    milliseconds = (uint64_t)now.tv_nsec / UINT64_C(1000000);
    if (seconds > (UINT64_MAX - milliseconds) / UINT64_C(1000)) {
        errno = EOVERFLOW;
        return -1;
    }
    *milliseconds_out = seconds * UINT64_C(1000) + milliseconds;
    return 0;
}

static void reduce_timeout(
    uint64_t now_milliseconds,
    uint64_t deadline_milliseconds,
    int *timeout_milliseconds)
{
    int deadline_timeout;

    if (deadline_milliseconds <= now_milliseconds) {
        deadline_timeout = 0;
    } else if (deadline_milliseconds - now_milliseconds > (uint64_t)INT_MAX) {
        deadline_timeout = INT_MAX;
    } else {
        deadline_timeout = (int)(deadline_milliseconds - now_milliseconds);
    }
    if (*timeout_milliseconds == -1 ||
        deadline_timeout < *timeout_milliseconds) {
        *timeout_milliseconds = deadline_timeout;
    }
}

static uint32_t session_readiness(short revents)
{
    uint32_t readiness = BOXRIG_SESSION_READY_NONE;

    if ((revents & (POLLIN | POLLHUP)) != 0) {
        readiness |= BOXRIG_SESSION_READY_READ;
    }
    if ((revents & POLLOUT) != 0) {
        readiness |= BOXRIG_SESSION_READY_WRITE;
    }
    return readiness;
}

static uint32_t transfer_readiness(short revents, uint32_t interests)
{
    uint32_t readiness = 0;

    if ((revents & POLLIN) != 0) {
        readiness |= BOXRIG_WAYLAND_WAIT_READABLE;
    }
    if ((revents & POLLOUT) != 0) {
        readiness |= BOXRIG_WAYLAND_WAIT_WRITABLE;
    }
    if ((revents & POLLHUP) != 0) {
        readiness |=
            (interests & BOXRIG_WAYLAND_WAIT_READABLE) != 0
                ? BOXRIG_WAYLAND_WAIT_READABLE
                : BOXRIG_WAYLAND_WAIT_ERROR;
    }
    if ((revents & (POLLERR | POLLNVAL)) != 0) {
        readiness |= BOXRIG_WAYLAND_WAIT_ERROR;
    }
    return readiness;
}

static int dispatch_session(
    boxrig_term_wayland_application *application,
    uint32_t readiness)
{
    boxrig_session_dispatch_result result;

    if (boxrig_session_dispatch(
            application->session,
            readiness,
            &result) != BOXRIG_SESSION_OK) {
        return -1;
    }
    application->peer_closed = application->peer_closed || result.peer_closed;
    return 0;
}

static int dispatch_transfers(
    boxrig_term_wayland_application *application,
    boxrig_wayland_wait_source *sources,
    size_t count)
{
    boxrig_wayland_transfer_dispatch_result result;

    if (count == 0) {
        return 0;
    }
    return boxrig_wayland_context_dispatch_wait_sources(
               application->context,
               sources,
               count,
               1,
               &result) == BOXRIG_UI_OK
               ? 0
               : -1;
}

static int dispatch_ready_work(
    boxrig_term_wayland_application *application,
    uint32_t session_ready,
    boxrig_wayland_wait_source *sources,
    size_t source_count)
{
    bool session_is_ready = session_ready != BOXRIG_SESSION_READY_NONE;
    bool clipboard_is_ready = false;
    size_t index;

    for (index = 0; index < source_count; index++) {
        clipboard_is_ready = clipboard_is_ready || sources[index].readiness != 0;
    }
    if (session_is_ready && clipboard_is_ready) {
        int first;
        int second;

        if (application->dispatch_clipboard_first) {
            first = dispatch_transfers(application, sources, source_count);
            second = dispatch_session(application, session_ready);
        } else {
            first = dispatch_session(application, session_ready);
            second = dispatch_transfers(application, sources, source_count);
        }
        application->dispatch_clipboard_first =
            !application->dispatch_clipboard_first;
        return first == -1 || second == -1 ? -1 : 0;
    }
    if (session_is_ready && dispatch_session(application, session_ready) == -1) {
        return -1;
    }
    if (clipboard_is_ready &&
        dispatch_transfers(application, sources, source_count) == -1) {
        return -1;
    }
    return 0;
}

static int prepare_display_read(
    boxrig_term_wayland_application *application)
{
    boxrig_wayland_dispatch_result dispatch_result;

    while (wl_display_prepare_read_queue(
               application->display,
               application->queue) != 0) {
        if (boxrig_term_wayland_native_host_dispatch_pending(
                application,
                BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT,
                &dispatch_result) == -1) {
            return -1;
        }
    }
    return 0;
}

int boxrig_term_wayland_run_event_loop(
    boxrig_term_wayland_application *application)
{
    struct pollfd *descriptors = NULL;
    size_t descriptor_capacity = 0;
    size_t uri_count = 0, ime_count = 0, notification_count = 0;
    int result = -1;
    boxrig_wayland_wait_source
        transfer_sources[BOXRIG_WAYLAND_CLIPBOARD_TRANSFERS_MAXIMUM];
    boxrig_wayland_dispatch_result context_result;
    size_t transfer_count = 0;
    uint32_t session_ready = BOXRIG_SESSION_READY_NONE;
    bool poll_result_pending = false;
    bool display_read_prepared = false;
    int pty_descriptor = boxrig_posix_pty_descriptor(application->pty);

    while (!application->window_closed && !application->peer_closed) {
        boxrig_session_dispatch_result retained_session_result;
        uint32_t session_interests;
        uint64_t now_milliseconds;
        bool frame_additional;
        bool host_items_pending;
        bool immediate;
        size_t index;
        int timeout_milliseconds = -1;
        int poll_result;

        if (poll_result_pending) {
            if (boxrig_freedesktop_notifications_dispatch_poll(
                    application->notifications,
                    descriptors + 2 + transfer_count + uri_count + ime_count,
                    notification_count) == -1) goto cleanup;
            if (boxrig_wayland_context_dispatch_ime_poll(application->context,
                    descriptors + 2 + transfer_count + uri_count, ime_count) != BOXRIG_UI_OK) goto cleanup;
            if (boxrig_term_uri_launcher_process_poll(application->uri_launcher,
                    descriptors + 2 + transfer_count, uri_count) == -1) goto cleanup;
            if (dispatch_ready_work(
                    application,
                    session_ready,
                    transfer_sources,
                    transfer_count) == -1) {
                goto cleanup;
            }
            poll_result_pending = false;
            session_ready = BOXRIG_SESSION_READY_NONE;
            transfer_count = 0;
        }
        if (monotonic_milliseconds(&now_milliseconds) == -1 ||
            boxrig_ui_animation_poll_dispatch(
                application->animation_loop) != BOXRIG_UI_OK ||
            boxrig_wayland_context_update_monotonic_time(
                application->context,
                now_milliseconds) != BOXRIG_UI_OK ||
            boxrig_term_wayland_native_host_dispatch_pending(
                application,
                BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT,
                &context_result) == -1 ||
            boxrig_session_dispatch(
                application->session,
                BOXRIG_SESSION_READY_NONE,
                &retained_session_result) != BOXRIG_SESSION_OK ||
            boxrig_term_wayland_native_host_process_items(
                application,
                &host_items_pending) == -1 ||
            boxrig_term_wayland_native_host_process_layout_action(application) ==
                -1 ||
            boxrig_term_wayland_native_host_apply_background_opacity(
                application) == -1 ||
            boxrig_term_wayland_native_host_frame(
                application,
                &frame_additional) == -1 ||
            boxrig_term_wayland_native_host_flush(application) == -1) {
            goto cleanup;
        }
        application->peer_closed =
            application->peer_closed || retained_session_result.peer_closed;
        if (application->window_closed || application->peer_closed) {
            break;
        }
        session_interests = boxrig_session_interests(application->session);
        immediate = context_result.additional_dispatch || frame_additional ||
                    host_items_pending ||
                    ((session_interests & BOXRIG_SESSION_INTEREST_DISPATCH) != 0 &&
                     (session_interests & BOXRIG_SESSION_INTEREST_WRITE) == 0);
        if (immediate) {
            continue;
        }
        if (monotonic_milliseconds(&now_milliseconds) == -1) goto cleanup;
        if (boxrig_term_link_gesture_dispatch(application->link_gesture, now_milliseconds) == -1)
            fprintf(stderr, "boxrig_term: could not confirm URL click.\n");
        uint64_t link_deadline;
        if (boxrig_term_link_gesture_deadline(application->link_gesture, &link_deadline))
            reduce_timeout(now_milliseconds, link_deadline, &timeout_milliseconds);
        if (boxrig_wayland_context_prepare_wait_sources(application->context, transfer_sources,
                BOXRIG_WAYLAND_CLIPBOARD_TRANSFERS_MAXIMUM, &transfer_count) != BOXRIG_UI_OK ||
            prepare_display_read(application) == -1) goto cleanup;
        display_read_prepared = true;
        if (boxrig_term_wayland_native_host_flush(application) == -1) goto cleanup;
        const struct pollfd *uri_descriptors;
        if (boxrig_term_uri_launcher_prepare_poll(application->uri_launcher,
                &uri_descriptors, &uri_count, &timeout_milliseconds) == -1) goto cleanup;
        const struct pollfd *ime_descriptors;
        if (boxrig_wayland_context_prepare_ime_poll(application->context,
                &ime_descriptors, &ime_count, &timeout_milliseconds) != BOXRIG_UI_OK) goto cleanup;
        const struct pollfd *notification_descriptors;
        if (boxrig_freedesktop_notifications_prepare_poll(application->notifications,
                &notification_descriptors, &notification_count, &timeout_milliseconds) == -1) goto cleanup;
        if (uri_count > SIZE_MAX / sizeof(*descriptors) - 2 - BOXRIG_WAYLAND_CLIPBOARD_TRANSFERS_MAXIMUM)
            goto cleanup;
        size_t required = 2 + BOXRIG_WAYLAND_CLIPBOARD_TRANSFERS_MAXIMUM + uri_count;
        if (ime_count > SIZE_MAX / sizeof(*descriptors) - required) goto cleanup;
        required += ime_count;
        if (notification_count > SIZE_MAX / sizeof(*descriptors) - required) goto cleanup;
        required += notification_count;
        if (required > descriptor_capacity) {
            struct pollfd *grown = realloc(descriptors, required * sizeof(*descriptors));
            if (grown == NULL) goto cleanup;
            descriptors = grown;
            descriptor_capacity = required;
        }
        if (context_result.has_repeat_deadline) {
            reduce_timeout(
                now_milliseconds,
                context_result.repeat_deadline_milliseconds,
                &timeout_milliseconds);
        }
        if (boxrig_ui_animation_poll_prepare_wait(
                application->animation_loop, timeout_milliseconds,
                &timeout_milliseconds) != BOXRIG_UI_OK) {
            goto cleanup;
        }
        memset(descriptors, 0, descriptor_capacity * sizeof(*descriptors));
        descriptors[0].fd = wl_display_get_fd(application->display);
        descriptors[0].events = POLLIN;
        if (application->display_flush_blocked) {
            descriptors[0].events |= POLLOUT;
        }
        descriptors[1].fd = pty_descriptor;
        if ((session_interests & BOXRIG_SESSION_INTEREST_READ) != 0) {
            descriptors[1].events |= POLLIN;
        }
        if ((session_interests & BOXRIG_SESSION_INTEREST_WRITE) != 0) {
            descriptors[1].events |= POLLOUT;
        }
        for (index = 0; index < transfer_count; index++) {
            descriptors[index + 2].fd = transfer_sources[index].descriptor;
            if ((transfer_sources[index].interests &
                 BOXRIG_WAYLAND_WAIT_READABLE) != 0) {
                descriptors[index + 2].events |= POLLIN;
            }
            if ((transfer_sources[index].interests &
                 BOXRIG_WAYLAND_WAIT_WRITABLE) != 0) {
                descriptors[index + 2].events |= POLLOUT;
            }
        }
        memcpy(descriptors + 2 + transfer_count, uri_descriptors, uri_count * sizeof(*descriptors));
        if (ime_count != 0) memcpy(descriptors + 2 + transfer_count + uri_count,
            ime_descriptors, ime_count * sizeof(*descriptors));
        if (notification_count != 0) memcpy(descriptors + 2 + transfer_count + uri_count + ime_count,
            notification_descriptors, notification_count * sizeof(*descriptors));
        poll_result = poll(
            descriptors,
            (nfds_t)(transfer_count + 2 + uri_count + ime_count + notification_count),
            timeout_milliseconds);
        display_read_prepared = false;
        if (poll_result < 0) {
            wl_display_cancel_read(application->display);
            if (errno == EINTR) {
                transfer_count = 0;
                continue;
            }
            goto cleanup;
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            if (wl_display_read_events(application->display) < 0) {
                goto cleanup;
            }
        } else {
            wl_display_cancel_read(application->display);
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
            (descriptors[1].revents & (POLLERR | POLLNVAL)) != 0) {
            goto cleanup;
        }
        session_ready = session_readiness(descriptors[1].revents);
        for (index = 0; index < transfer_count; index++) {
            transfer_sources[index].readiness =
                transfer_readiness(
                    descriptors[index + 2].revents,
                    transfer_sources[index].interests);
        }
        poll_result_pending = true;
    }
    result = 0;
cleanup:
    if (display_read_prepared) wl_display_cancel_read(application->display);
    free(descriptors);
    return result;
}
