/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/posix.h"
#include "boxrig/x11.h"

#include <X11/Xlib.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

typedef struct transfer_app {
    Display *display;
    Window parents[2];
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_x11_terminal_component *component;
    boxrig_posix_pty *pty;
    boxrig_session *session;
    unsigned int active_parent;
    bool transfer_requested;
    bool transfer_busy;
    bool verify;
    unsigned int transfers;
    unsigned int title_stage;
    bool first_sent;
    bool second_sent;
    bool peer_closed;
    bool close_requested;
    Atom delete_window;
} transfer_app;

static boxrig_input_key_route route_key(const boxrig_key_event *event, void *context)
{
    transfer_app *app = context;
    if (event->key != BOXRIG_KEY_F6) return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    if (event->action == BOXRIG_KEY_RELEASE) app->transfer_requested = true;
    return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
}

static bool service_layout(transfer_app *app)
{
    boxrig_ui_layout_action action;
    while (boxrig_x11_terminal_component_take_layout_action(app->component, &action)) {
        if (action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
            if (boxrig_session_resize(app->session, action.value.grid_resize.rows,
                    action.value.grid_resize.columns) != BOXRIG_SESSION_OK ||
                boxrig_x11_terminal_component_complete_grid_resize(app->component,
                    action.revision, true) != BOXRIG_UI_OK) return false;
        } else if (action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE) {
            boxrig_ui_layout_result layout;
            bool applied;
            if (boxrig_x11_terminal_component_complete_size_command(app->component,
                    action.revision, BOXRIG_UI_SIZE_ACCEPTED,
                    action.value.content_size.width,
                    action.value.content_size.height, &applied, &layout) != BOXRIG_UI_OK)
                return false;
        }
    }
    return true;
}

static bool transfer(transfer_app *app)
{
    unsigned int destination = 1u - app->active_parent;
    boxrig_ui_status status = boxrig_x11_terminal_component_reparent(
        app->component, app->parents[destination]);
    if (status == BOXRIG_UI_BUSY) {
        app->transfer_busy = true;
        return true;
    }
    if (status != BOXRIG_UI_OK) {
        fprintf(stderr, "reparent status: %d\n", (int)status);
        return false;
    }
    app->active_parent = destination;
    app->transfer_busy = false;
    app->transfer_requested = false;
    app->transfers++;
    (void)boxrig_x11_terminal_component_request_focus(app->component);
    XWindowAttributes attributes;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    if (!XGetWindowAttributes(app->display, app->parents[destination], &attributes))
        return false;
    allocation.width = (unsigned int)attributes.width;
    allocation.height = (unsigned int)attributes.height;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    if (boxrig_x11_terminal_component_allocate(app->component, &allocation,
            &layout) != BOXRIG_UI_OK) return false;
    return true;
}

static bool service(transfer_app *app, uint32_t ready)
{
    boxrig_session_dispatch_result dispatch;
    boxrig_ui_component_frame_result frame;
    boxrig_host_item item;
    if (boxrig_session_dispatch(app->session, ready, &dispatch) != BOXRIG_SESSION_OK ||
        boxrig_ui_model_synchronize(app->model) != BOXRIG_UI_OK ||
        !service_layout(app) ||
        boxrig_x11_terminal_component_frame(app->component, &frame) != BOXRIG_UI_OK)
        return false;
    if (dispatch.peer_closed) app->peer_closed = true;
    while (boxrig_terminal_host_item_peek(app->terminal, &item)) {
        if (item.type == BOXRIG_HOST_ITEM_TITLE) {
            if (app->title_stage == 0 && item.payload_length == 14 &&
                memcmp(item.payload, "transfer-ready", 14) == 0)
                app->title_stage = 1;
            else if (app->title_stage == 1 && item.payload_length == 12 &&
                memcmp(item.payload, "transfer-ack", 12) == 0)
                app->title_stage = 2;
            else if (app->title_stage == 2 && item.payload_length == 17 &&
                memcmp(item.payload, "transfer-verified", 17) == 0)
                app->title_stage = 3;
        }
        if (boxrig_terminal_host_item_consume(app->terminal) != BOXRIG_STATUS_OK)
            return false;
    }
    return true;
}

static bool run(transfer_app *app)
{
    for (;;) {
        struct pollfd descriptors[2];
        uint32_t interests;
        uint32_t ready = BOXRIG_SESSION_READY_NONE;
        if (app->verify && app->title_stage == 1 && app->transfers == 0)
            app->transfer_requested = true;
        if (app->verify && app->title_stage == 2 && app->transfers == 1)
            app->transfer_requested = true;
        if (app->transfer_requested && !transfer(app)) return false;
        if (app->verify && app->transfers == 1 && !app->first_sent) {
            if (boxrig_terminal_send_text(app->terminal, "first\n", 6) != BOXRIG_STATUS_OK)
                return false;
            app->first_sent = true;
        }
        if (app->verify && app->transfers == 2 && !app->second_sent) {
            if (boxrig_terminal_send_text(app->terminal, "second\n", 7) != BOXRIG_STATUS_OK)
                return false;
            app->second_sent = true;
        }
        if (app->verify && app->transfers == 2 && app->title_stage == 3) return true;
        if (app->verify && app->peer_closed) return false;
        if (!app->verify && (app->peer_closed || app->close_requested)) return true;
        while (XPending(app->display) != 0) {
            XEvent event;
            XNextEvent(app->display, &event);
            if (event.type == ClientMessage &&
                (event.xclient.window == app->parents[0] ||
                 event.xclient.window == app->parents[1]) &&
                (Atom)event.xclient.data.l[0] == app->delete_window) {
                app->close_requested = true;
                continue;
            }
            if (event.type == DestroyNotify &&
                (event.xdestroywindow.window == app->parents[0] ||
                 event.xdestroywindow.window == app->parents[1])) return false;
            if (event.type == ConfigureNotify &&
                event.xconfigure.window == app->parents[app->active_parent] &&
                event.xconfigure.width > 0 && event.xconfigure.height > 0) {
                boxrig_ui_component_allocation allocation = {0};
                boxrig_ui_layout_result layout;
                allocation.width = (unsigned int)event.xconfigure.width;
                allocation.height = (unsigned int)event.xconfigure.height;
                allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
                if (boxrig_x11_terminal_component_allocate(app->component,
                        &allocation, &layout) != BOXRIG_UI_OK) return false;
            }
            boxrig_ui_component_event_result result =
                boxrig_x11_terminal_component_dispatch_event(app->component, &event);
            if (result == BOXRIG_UI_COMPONENT_EVENT_FAILURE) return false;
        }
        if (!app->verify && app->close_requested) return true;
        if (!service(app, BOXRIG_SESSION_READY_NONE)) return false;
        if (app->transfer_requested && !transfer(app)) return false;
        if (app->verify && !app->transfer_busy &&
            ((app->title_stage == 1 && app->transfers == 0) ||
             (app->title_stage == 2 && app->transfers == 1) ||
             (app->transfers == 1 && !app->first_sent) ||
             (app->transfers == 2 && !app->second_sent) ||
             (app->transfers == 2 && app->title_stage == 3))) continue;
        interests = boxrig_session_interests(app->session);
        if (interests & BOXRIG_SESSION_INTEREST_DISPATCH) continue;
        descriptors[0].fd = ConnectionNumber(app->display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = boxrig_posix_pty_descriptor(app->pty);
        descriptors[1].events = 0;
        if (interests & BOXRIG_SESSION_INTEREST_READ) descriptors[1].events |= POLLIN;
        if (interests & BOXRIG_SESSION_INTEREST_WRITE) descriptors[1].events |= POLLOUT;
        descriptors[1].revents = 0;
        if (poll(descriptors, 2, app->verify ? 60000 : -1) <= 0) {
            fprintf(stderr, "timeout: transfers=%u title_stage=%u interests=%u\n",
                app->transfers, app->title_stage, interests);
            return false;
        }
        if (descriptors[1].revents & (POLLIN | POLLHUP)) ready |= BOXRIG_SESSION_READY_READ;
        if (descriptors[1].revents & POLLOUT) ready |= BOXRIG_SESSION_READY_WRITE;
        if (descriptors[1].revents & (POLLERR | POLLNVAL)) return false;
        if (!service(app, ready)) return false;
    }
}

int main(int argc, char **argv)
{
    transfer_app app = {0};
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_session_config session_config = boxrig_session_config_default();
    boxrig_x11_terminal_component_config component_config =
        boxrig_x11_terminal_component_config_default();
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_font_effective_metrics metrics;
    char *verify_args[] = {"/bin/sh", "-c",
        "printf 'before-transfer\\r\\n\\033]0;transfer-ready\\007'; "
        "read value; [ \"$value\" = first ] || exit 2; "
        "printf 'after-first-transfer\\r\\n\\033]0;transfer-ack\\007'; "
        "read value; [ \"$value\" = second ] || exit 3; "
        "printf 'after-second-transfer\\r\\n\\033]0;transfer-verified\\007'; "
        "exec cat", NULL};
    char *interactive_args[] = {"/bin/sh", NULL};
    int result = 1;
    const char *stage = "display";
    app.verify = argc == 2 && strcmp(argv[1], "--verify") == 0;
    if (argc != 1 && !app.verify) {
        fprintf(stderr, "usage: %s [--verify]\n", argv[0]);
        return 2;
    }
    app.display = XOpenDisplay(NULL);
    if (app.display == NULL) goto done;
    stage = "windows";
    int screen = DefaultScreen(app.display);
    Window root = RootWindow(app.display, screen);
    app.delete_window = XInternAtom(app.display, "WM_DELETE_WINDOW", False);
    for (unsigned int i = 0; i < 2; i++) {
        app.parents[i] = XCreateSimpleWindow(app.display, root,
            40 + (int)i * 640, 40, 620, 420, 0,
            BlackPixel(app.display, screen), WhitePixel(app.display, screen));
        if (app.parents[i] == None) goto done;
        XStoreName(app.display, app.parents[i], i == 0 ? "boxrig dock" : "boxrig undock");
        XSelectInput(app.display, app.parents[i], StructureNotifyMask);
        XSetWMProtocols(app.display, app.parents[i], &app.delete_window, 1);
        XMapWindow(app.display, app.parents[i]);
    }
    terminal_config.rows = 24;
    terminal_config.columns = 80;
    stage = "terminal and session";
    if (boxrig_terminal_create(&terminal_config, &app.terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(app.terminal, &app.model) != BOXRIG_UI_OK ||
        boxrig_posix_pty_spawn(app.verify ? verify_args : interactive_args,
            "xterm-256color", 24, 80, &app.pty) != 0 ||
        boxrig_posix_pty_session_create(app.terminal, app.pty, &session_config,
            &app.session) != BOXRIG_SESSION_OK) goto done;
    component_config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    component_config.view.blink.text_enabled = false;
    component_config.view.blink.cursor_enabled = false;
    stage = "component";
    boxrig_ui_status create_status = boxrig_x11_terminal_component_create_child(
        app.display, app.parents[0], app.model, &component_config, &app.component);
    if (create_status != BOXRIG_UI_OK) {
        fprintf(stderr, "component create status: %d\n", (int)create_status);
        goto done;
    }
    if (boxrig_x11_terminal_component_get_effective_metrics(app.component,
            &metrics) != BOXRIG_FONT_CATALOG_OK) goto done;
    allocation.width = 80u * metrics.cell_width;
    allocation.height = 24u * metrics.cell_height;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    stage = "allocation";
    if (boxrig_x11_terminal_component_allocate(app.component, &allocation,
            &layout) != BOXRIG_UI_OK ||
        boxrig_x11_terminal_component_set_visible(app.component, true) != BOXRIG_UI_OK ||
        boxrig_x11_terminal_component_set_key_router(app.component,
            route_key, &app) != BOXRIG_UI_OK) goto done;
    XFlush(app.display);
    (void)boxrig_x11_terminal_component_request_focus(app.component);
    stage = "event loop";
    result = run(&app) ? 0 : 1;
done:
    if (result != 0) fprintf(stderr, "boxrig transfer failed at %s\n", stage);
    boxrig_x11_terminal_component_destroy(app.component);
    boxrig_session_destroy(app.session);
    boxrig_posix_pty_destroy(app.pty);
    boxrig_ui_model_destroy(app.model);
    boxrig_terminal_destroy(app.terminal);
    if (app.display != NULL) {
        for (unsigned int i = 0; i < 2; i++)
            if (app.parents[i] != None) XDestroyWindow(app.display, app.parents[i]);
        XCloseDisplay(app.display);
    }
    return result;
}
