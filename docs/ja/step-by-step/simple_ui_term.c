#define _POSIX_C_SOURCE 200809L

#include "boxrig/terminal.h"
#include "boxrig/x11.h"
#include "boxrig/posix.h"

#include <X11/Xlib.h>

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static int process_host_items(
    Display *display,
    boxrig_ui_host_item_consumer *consumer)
{
    boxrig_host_item item;

    while (boxrig_ui_host_item_consumer_peek(consumer, &item)) {
        const uint64_t request_id = item.request_id;

        /* Handle bells and ignore unsupported host operations. */
        if (item.type == BOXRIG_HOST_ITEM_BELL) {
            (void)XBell(display, 0);
        }
        if (boxrig_ui_host_item_consumer_consume(consumer) != BOXRIG_UI_OK ||
            (request_id != 0 &&
             boxrig_ui_host_item_consumer_complete(
                 consumer, request_id, BOXRIG_HOST_REQUEST_UNAVAILABLE,
                 NULL, 0) != BOXRIG_UI_OK)) {
            return -1;
        }
    }
    return 0;
}

static int process_layout(
    boxrig_x11_terminal_component *component,
    boxrig_session *session,
    unsigned int width,
    unsigned int height)
{
    boxrig_ui_layout_action action;

    while (boxrig_x11_terminal_component_take_layout_action(component, &action)) {
        if (action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
            const bool succeeded = boxrig_session_resize(
                session, action.value.grid_resize.rows,
                action.value.grid_resize.columns) == BOXRIG_SESSION_OK;

            if (boxrig_x11_terminal_component_complete_grid_resize(
                    component, action.revision, succeeded) != BOXRIG_UI_OK ||
                !succeeded) {
                return -1;
            }
        } else {
            bool applied;
            boxrig_ui_layout_result layout;

            /* This host keeps its parent allocation for content-size requests. */
            if (boxrig_x11_terminal_component_complete_size_command(
                    component, action.revision, BOXRIG_UI_SIZE_REJECTED,
                    (boxrig_ui_fixed)width * BOXRIG_UI_FIXED_ONE,
                    (boxrig_ui_fixed)height * BOXRIG_UI_FIXED_ONE,
                    &applied, &layout) != BOXRIG_UI_OK) {
                return -1;
            }
        }
    }
    return 0;
}

static int allocate_component(
    boxrig_x11_terminal_component *component,
    unsigned int width,
    unsigned int height)
{
    const boxrig_ui_component_allocation allocation = {
        0, 0, width, height, BOXRIG_UI_WINDOW_EMBEDDED
    };
    boxrig_ui_layout_result layout;

    return boxrig_x11_terminal_component_allocate(
        component, &allocation, &layout) == BOXRIG_UI_OK ? 0 : -1;
}

int main(int argument_count, char **arguments)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_x11_terminal_component_config component_config =
        boxrig_x11_terminal_component_config_default();
    boxrig_session_config session_config = boxrig_session_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_animation_poll *animations = NULL;
    boxrig_ui_host_item_consumer *consumer = NULL;
    boxrig_x11_terminal_component *component = NULL;
    boxrig_posix_pty *pty = NULL;
    boxrig_session *session = NULL;
    Display *display = NULL;
    Window window = None;
    Atom delete_window = None;
    Atom protocols = None;
    boxrig_font_effective_metrics metrics;
    unsigned int width;
    unsigned int height;
    bool running = true;
    bool blocked = false;
    XEvent blocked_event;
    uint32_t ready = BOXRIG_SESSION_READY_NONE;
    int result = 1;

    /* XIM uses the locale selected by the application. */
    if (setlocale(LC_ALL, "") == NULL || XSetLocaleModifiers("") == NULL) {
        fprintf(stderr, "simple-ui-term: could not initialize locale\n");
        goto cleanup;
    }
    display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "simple-ui-term: could not open X display\n");
        goto cleanup;
    }
    window = XCreateSimpleWindow(
        display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
    if (window == None) {
        goto cleanup;
    }
    XSelectInput(display, window, StructureNotifyMask | FocusChangeMask);
    delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    if (delete_window == None || protocols == None ||
        XSetWMProtocols(display, window, &delete_window, 1) == 0) {
        goto cleanup;
    }
    XStoreName(display, window, "boxrig UI guide");

    if (boxrig_x11_animation_poll_create(&animations) != BOXRIG_UI_OK) {
        goto cleanup;
    }
    component_config.animation_runtime = boxrig_ui_animation_poll_runtime(animations);
    config.rows = 25;
    config.columns = 80;
    component_config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(terminal, &model) != BOXRIG_UI_OK ||
        boxrig_ui_host_item_consumer_create(model, &consumer) != BOXRIG_UI_OK ||
        boxrig_x11_terminal_component_create_child(
            display, window, model, &component_config, &component) != BOXRIG_UI_OK ||
        boxrig_x11_terminal_component_get_effective_metrics(
            component, &metrics) != BOXRIG_FONT_CATALOG_OK ||
        metrics.cell_width == 0 || metrics.cell_height == 0 ||
        metrics.cell_width > UINT_MAX / config.columns ||
        metrics.cell_height > UINT_MAX / config.rows) {
        fprintf(stderr, "simple-ui-term: could not create terminal component\n");
        goto cleanup;
    }
    width = (unsigned int)config.columns * metrics.cell_width;
    height = (unsigned int)config.rows * metrics.cell_height;
    XResizeWindow(display, window, width, height);

    {
        char *shell_arguments[] = { "/bin/sh", "-i", NULL };
        char **child_arguments = argument_count > 1 ? arguments + 1 : shell_arguments;

        if (boxrig_posix_pty_spawn(
                child_arguments, "xterm", config.rows, config.columns, &pty) == -1 ||
            boxrig_posix_pty_session_create(
                terminal, pty, &session_config, &session) != BOXRIG_SESSION_OK) {
            fprintf(stderr, "simple-ui-term: could not connect child process\n");
            goto cleanup;
        }
    }
    if (allocate_component(component, width, height) == -1 ||
        process_layout(component, session, width, height) == -1 ||
        boxrig_x11_terminal_component_set_visible(component, true) != BOXRIG_UI_OK) {
        goto cleanup;
    }
    XMapWindow(display, window);

    while (running) {
        boxrig_session_dispatch_result dispatched;
        boxrig_ui_component_frame_result frame;
        boxrig_host_item item;
        bool had_host_items;
        uint32_t interests;
        bool retry_session;
        bool waiting_for_output;
        int timeout;
        struct pollfd descriptors[2] = {
            { ConnectionNumber(display), POLLIN, 0 },
            { boxrig_posix_pty_descriptor(pty), 0, 0 }
        };

        if (boxrig_ui_animation_poll_dispatch(animations) != BOXRIG_UI_OK ||
            boxrig_session_dispatch(session, ready, &dispatched) != BOXRIG_SESSION_OK) {
            goto cleanup;
        }
        ready = BOXRIG_SESSION_READY_NONE;
        had_host_items = boxrig_ui_host_item_consumer_peek(consumer, &item);
        if (process_host_items(display, consumer) == -1) {
            goto cleanup;
        }

        /* Handle at most one native event before servicing the PTY and frame. */
        if (blocked || XPending(display) > 0) {
            XEvent event;
            boxrig_ui_component_event_result handled;

            if (blocked) {
                event = blocked_event;
            } else {
                XNextEvent(display, &event);
            }
            handled = boxrig_x11_terminal_component_dispatch_event(component, &event);
            if (handled == BOXRIG_UI_COMPONENT_EVENT_FAILURE) {
                goto cleanup;
            }
            blocked = handled == BOXRIG_UI_COMPONENT_EVENT_BLOCKED;
            if (blocked) {
                blocked_event = event;
            } else if (handled == BOXRIG_UI_COMPONENT_EVENT_UNRELATED) {
                if (event.type == ConfigureNotify &&
                    event.xconfigure.window == window &&
                    event.xconfigure.width > 0 && event.xconfigure.height > 0) {
                    width = (unsigned int)event.xconfigure.width;
                    height = (unsigned int)event.xconfigure.height;
                    if (allocate_component(component, width, height) == -1) {
                        goto cleanup;
                    }
                } else if (event.type == FocusIn && event.xfocus.window == window) {
                    if (boxrig_x11_terminal_component_request_focus(component) !=
                        BOXRIG_UI_OK) {
                        goto cleanup;
                    }
                } else if (event.type == ClientMessage &&
                           event.xclient.window == window &&
                           event.xclient.message_type == protocols &&
                           event.xclient.format == 32 &&
                           (Atom)event.xclient.data.l[0] == delete_window) {
                    running = false;
                }
            }
        }
        if (!running) {
            break;
        }
        if (process_layout(component, session, width, height) == -1 ||
            boxrig_ui_model_synchronize(model) != BOXRIG_UI_OK ||
            boxrig_x11_terminal_component_frame(component, &frame) != BOXRIG_UI_OK ||
            process_layout(component, session, width, height) == -1) {
            goto cleanup;
        }
        if (dispatched.peer_closed) {
            break;
        }

        interests = boxrig_session_interests(session);
        /* Later native events cannot advance a retained blocked event. */
        descriptors[0].events = blocked ? 0 : POLLIN;
        if ((interests & BOXRIG_SESSION_INTEREST_READ) != 0) {
            descriptors[1].events |= POLLIN;
        }
        if ((interests & BOXRIG_SESSION_INTEREST_WRITE) != 0) {
            descriptors[1].events |= POLLOUT;
        }
        retry_session = (interests & BOXRIG_SESSION_INTEREST_DISPATCH) != 0 &&
            (!dispatched.core_blocked || had_host_items ||
             dispatched.transport_bytes_written != 0);
        waiting_for_output = frame.input_blocked &&
            (interests & BOXRIG_SESSION_INTEREST_WRITE) != 0 &&
            dispatched.transport_bytes_written == 0;
        timeout = (frame.additional_dispatch && !waiting_for_output) || retry_session ||
            (blocked && dispatched.transport_bytes_written != 0) ||
            (!blocked && XPending(display) > 0) ? 0 : -1;
        if (boxrig_ui_animation_poll_prepare_wait(animations, timeout, &timeout) != BOXRIG_UI_OK) {
            goto cleanup;
        }
        XFlush(display);
        /* poll with timeout zero still collects PTY readiness during UI work. */
        if (poll(descriptors, 2, timeout) == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("simple-ui-term: poll");
            goto cleanup;
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
            (descriptors[1].revents & POLLNVAL) != 0) {
            goto cleanup;
        }
        /* Drain unread bytes on hangup before accepting transport EOF. */
        if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            ready |= BOXRIG_SESSION_READY_READ;
        }
        if ((descriptors[1].revents & (POLLOUT | POLLHUP | POLLERR)) != 0) {
            ready |= BOXRIG_SESSION_READY_WRITE;
        }
    }
    result = 0;

cleanup:
    if (result != 0) {
        fprintf(stderr, "simple-ui-term: terminal operation failed\n");
    }
    /* Stop native delivery before releasing the borrowed model and Display. */
    if (component != NULL) {
        (void)boxrig_x11_terminal_component_quiesce(component);
    }
    boxrig_x11_terminal_component_destroy(component);
    boxrig_ui_animation_poll_destroy(animations);
    boxrig_session_destroy(session);
    boxrig_ui_host_item_consumer_destroy(consumer);
    if (pty != NULL) {
        int child_status;

        /* This minimal host forcibly stops its direct child on window close. */
        (void)kill(boxrig_posix_pty_child(pty), SIGKILL);
        if (boxrig_posix_pty_wait(pty, &child_status) == -1) {
            perror("simple-ui-term: could not reap child process");
            result = 1;
        }
        boxrig_posix_pty_destroy(pty);
    }
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    if (display != NULL) {
        if (window != None) {
            XDestroyWindow(display, window);
        }
        XCloseDisplay(display);
    }
    return result;
}
