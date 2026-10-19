/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

#include "event_source_policy.h"

#define TEST_TIMEOUT_MILLISECONDS UINT64_C(5000)

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1 || now.tv_sec < 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static int remaining_timeout(uint64_t deadline)
{
    uint64_t now = monotonic_milliseconds();
    uint64_t remaining;

    if (now >= deadline) {
        return 0;
    }
    remaining = deadline - now;
    return remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
}

static bool drain_output(
    int descriptor,
    char *output,
    size_t capacity,
    size_t *length)
{
    char bytes[512];

    for (;;) {
        ssize_t count = read(descriptor, bytes, sizeof(bytes));

        if (count > 0) {
            size_t available = capacity - *length - 1;
            size_t copied = (size_t)count < available
                                ? (size_t)count
                                : available;

            memcpy(output + *length, bytes, copied);
            *length += copied;
            output[*length] = '\0';
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        return count == 0 || errno == EAGAIN || errno == EWOULDBLOCK;
    }
}

static pid_t start_sample(
    const char *sample_path,
    const char *helper_path,
    int *output_descriptor)
{
    int output_pipe[2];
    pid_t process;
    const char *test_im_module = getenv("BOXRIG_TEST_GTK4_IM_MODULE");

    if (pipe(output_pipe) == -1) {
        return -1;
    }
    process = fork();
    if (process == -1) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        return -1;
    }
    if (process == 0) {
        close(output_pipe[0]);
        if (setpgid(0, 0) == -1 ||
            dup2(output_pipe[1], STDOUT_FILENO) == -1 ||
            dup2(output_pipe[1], STDERR_FILENO) == -1 ||
            setenv("BOXRIG_TERM", "xterm-256color", 1) == -1 ||
            setenv("NO_AT_BRIDGE", "1", 1) == -1 ||
            setenv("GDK_BACKEND", "x11", 1) == -1 ||
            setenv("GSK_RENDERER", "cairo", 1) == -1 ||
            test_im_module == NULL || test_im_module[0] == '\0' ||
            setenv("GTK_IM_MODULE", test_im_module, 1) == -1 ||
            unsetenv("BOXRIG_BACKGROUND_OPACITY") == -1) {
            _exit(127);
        }
        close(output_pipe[1]);
        execl(
            sample_path,
            sample_path,
            "--",
            helper_path,
            (char *)NULL);
        _exit(127);
    }
    close(output_pipe[1]);
    if ((setpgid(process, process) == -1 && errno != EACCES &&
         errno != ESRCH) ||
        fcntl(output_pipe[0], F_SETFL, O_NONBLOCK) == -1) {
        close(output_pipe[0]);
        kill(-process, SIGTERM);
        waitpid(process, NULL, 0);
        return -1;
    }
    *output_descriptor = output_pipe[0];
    return process;
}

static bool apply_configure_request(
    Display *display,
    const XConfigureRequestEvent *request)
{
    XWindowChanges changes;

    memset(&changes, 0, sizeof(changes));
    changes.x = request->x;
    changes.y = request->y;
    changes.width = request->width;
    changes.height = request->height;
    changes.border_width = request->border_width;
    changes.sibling = request->above;
    changes.stack_mode = request->detail;
    XConfigureWindow(
        display,
        request->window,
        request->value_mask,
        &changes);
    XFlush(display);
    return true;
}

static bool wait_for_window(
    Display *display,
    Window root,
    int output_descriptor,
    Window *window_out,
    char *output,
    size_t output_capacity,
    size_t *output_length)
{
    uint64_t deadline =
        monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;
    bool process_closed = false;

    for (;;) {
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == ConfigureRequest &&
                event.xconfigurerequest.parent == root) {
                if (!apply_configure_request(
                        display,
                        &event.xconfigurerequest)) {
                    return false;
                }
            } else if (event.type == MapRequest &&
                event.xmaprequest.parent == root) {
                XMapWindow(display, event.xmaprequest.window);
                XSync(display, False);
                *window_out = event.xmaprequest.window;
                return true;
            }
        }
        if (process_closed) {
            return false;
        }
        {
            struct pollfd descriptors[2];
            int timeout = remaining_timeout(deadline);
            int result;

            if (timeout == 0) {
                return false;
            }
            descriptors[0].fd = ConnectionNumber(display);
            descriptors[0].events = POLLIN;
            descriptors[0].revents = 0;
            descriptors[1].fd = output_descriptor;
            descriptors[1].events = POLLIN;
            descriptors[1].revents = 0;
            result = poll(descriptors, 2, timeout);
            if (result == -1 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                return false;
            }
            if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) !=
                    0 &&
                !drain_output(
                    output_descriptor,
                    output,
                    output_capacity,
                    output_length)) {
                return false;
            }
            process_closed =
                (descriptors[1].revents & (POLLHUP | POLLERR)) != 0;
        }
    }
}

static bool send_key(
    Display *display,
    Window window,
    KeySym symbol,
    bool control_pressed,
    bool shift_pressed)
{
    int event_base;
    int error_base;
    int major_version;
    int minor_version;
    KeyCode control = XKeysymToKeycode(display, XK_Control_L);
    KeyCode shift = XKeysymToKeycode(display, XK_Shift_L);
    KeyCode key = XKeysymToKeycode(display, symbol);

    if (key == 0 || (control_pressed && control == 0) ||
        (shift_pressed && shift == 0) ||
        !XTestQueryExtension(
            display,
            &event_base,
            &error_base,
            &major_version,
            &minor_version)) {
        return false;
    }
    XSetInputFocus(display, window, RevertToParent, CurrentTime);
    if ((control_pressed &&
         XTestFakeKeyEvent(display, control, True, CurrentTime) == 0) ||
        (shift_pressed &&
         XTestFakeKeyEvent(display, shift, True, CurrentTime) == 0) ||
        XTestFakeKeyEvent(display, key, True, CurrentTime) == 0 ||
        XTestFakeKeyEvent(display, key, False, CurrentTime) == 0 ||
        (shift_pressed &&
         XTestFakeKeyEvent(display, shift, False, CurrentTime) == 0) ||
        (control_pressed &&
         XTestFakeKeyEvent(display, control, False, CurrentTime) == 0)) {
        return false;
    }
    XSync(display, False);
    return true;
}

static bool wait_for_resize_request(
    Display *display,
    Window window,
    int output_descriptor,
    int previous_width,
    int previous_height,
    bool enlarging,
    bool apply_request,
    XWindowAttributes *attributes_out,
    char *output,
    size_t output_capacity,
    size_t *output_length)
{
    uint64_t deadline =
        monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;

    for (;;) {
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == ConfigureRequest &&
                event.xconfigurerequest.window == window) {
                XConfigureRequestEvent *request = &event.xconfigurerequest;

                if ((request->value_mask & (CWWidth | CWHeight)) !=
                        (CWWidth | CWHeight) ||
                    (enlarging &&
                     (request->width <= previous_width ||
                      request->height <= previous_height)) ||
                    (!enlarging &&
                     (request->width != previous_width ||
                      request->height != previous_height))) {
                    return false;
                }
                if (!apply_request) {
                    return true;
                }
                if (attributes_out == NULL ||
                    !apply_configure_request(display, request)) {
                    return false;
                }
                XSync(display, False);
                return XGetWindowAttributes(
                           display,
                           window,
                           attributes_out) != 0 &&
                       attributes_out->width == request->width &&
                       attributes_out->height == request->height;
            }
            if (event.type == DestroyNotify &&
                event.xdestroywindow.window == window) {
                return false;
            }
        }
        {
            struct pollfd descriptors[2];
            int timeout = remaining_timeout(deadline);
            int result;

            if (timeout == 0) {
                return false;
            }
            descriptors[0].fd = ConnectionNumber(display);
            descriptors[0].events = POLLIN;
            descriptors[0].revents = 0;
            descriptors[1].fd = output_descriptor;
            descriptors[1].events = POLLIN;
            descriptors[1].revents = 0;
            result = poll(descriptors, 2, timeout);
            if (result == -1 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                return false;
            }
            if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) !=
                    0 &&
                !drain_output(
                    output_descriptor,
                    output,
                    output_capacity,
                    output_length)) {
                return false;
            }
            if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
                return false;
            }
        }
    }
}

static bool read_window_background_alpha(
    Display *display,
    Window window,
    const XWindowAttributes *attributes,
    unsigned int *alpha_out)
{
    XRenderPictFormat *format;
    XImage *image;
    unsigned long pixel;

    if (display == NULL || window == None || attributes == NULL ||
        attributes->width < 2 || attributes->height < 2 ||
        alpha_out == NULL) {
        return false;
    }
    format = XRenderFindVisualFormat(display, attributes->visual);
    if (format == NULL || format->type != PictTypeDirect ||
        format->direct.alphaMask == 0 || format->direct.alpha < 0) {
        return false;
    }
    image = XGetImage(
        display,
        window,
        attributes->width / 2,
        attributes->height / 2,
        1,
        1,
        AllPlanes,
        ZPixmap);
    if (image == NULL) {
        return false;
    }
    pixel = XGetPixel(image, 0, 0);
    *alpha_out = (unsigned int)(
        (pixel >> format->direct.alpha) & format->direct.alphaMask);
    XDestroyImage(image);
    return true;
}

static bool wait_for_background_alpha(
    Display *display,
    Window window,
    Damage dirty,
    int dirty_event_base,
    int output_descriptor,
    unsigned int expected_alpha,
    char *output,
    size_t output_capacity,
    size_t *output_length)
{
    uint64_t deadline =
        monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;

    for (;;) {
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == dirty_event_base + XDamageNotify) {
                XDamageNotifyEvent *notification =
                    (XDamageNotifyEvent *)&event;
                XWindowAttributes attributes;
                unsigned int alpha;

                if (notification->drawable == window &&
                    notification->damage == dirty) {
                    XDamageSubtract(display, dirty, None, None);
                    if (XGetWindowAttributes(
                            display,
                            window,
                            &attributes) != 0 &&
                        read_window_background_alpha(
                            display,
                            window,
                            &attributes,
                            &alpha) &&
                        alpha == expected_alpha) {
                        return true;
                    }
                }
            }
            if (event.type == DestroyNotify &&
                event.xdestroywindow.window == window) {
                return false;
            }
        }
        {
            struct pollfd descriptors[2];
            int timeout = remaining_timeout(deadline);
            int result;

            if (timeout == 0) {
                return false;
            }
            descriptors[0].fd = ConnectionNumber(display);
            descriptors[0].events = POLLIN;
            descriptors[0].revents = 0;
            descriptors[1].fd = output_descriptor;
            descriptors[1].events = POLLIN;
            descriptors[1].revents = 0;
            result = poll(descriptors, 2, timeout);
            if (result == -1 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                return false;
            }
            if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) !=
                    0 &&
                !drain_output(
                    output_descriptor,
                    output,
                    output_capacity,
                    output_length)) {
                return false;
            }
            if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
                return false;
            }
        }
    }
}

static bool query_grid_without_resize_request(
    Display *display,
    Window window,
    int output_descriptor,
    const char *sentinel,
    unsigned int expected_rows,
    unsigned int expected_columns,
    bool accept_initial_resize,
    char *output,
    size_t output_capacity,
    size_t *output_length)
{
    char expected[64];
    int expected_length;
    uint64_t deadline =
        monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;

    expected_length = snprintf(
        expected,
        sizeof(expected),
        "grid-%u-%u",
        expected_rows,
        expected_columns);
    if (expected_length <= 0 ||
        (size_t)expected_length >= sizeof(expected) ||
        XStoreName(display, window, sentinel) == 0 ||
        !send_key(display, window, XK_g, false, false)) {
        return false;
    }
    for (;;) {
        char *title = NULL;
        bool matches = false;
        bool retry = false;
        bool resized = false;

        XSync(display, False);
        if (XFetchName(display, window, &title) != 0 && title != NULL) {
            matches = strcmp(title, expected) == 0;
            retry = !matches && strncmp(title, "grid-", 5) == 0;
            XFree(title);
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == ConfigureRequest &&
                event.xconfigurerequest.window == window) {
                if (!accept_initial_resize ||
                    !apply_configure_request(
                        display,
                        &event.xconfigurerequest)) {
                    return false;
                }
                resized = true;
            }
            if (event.type == DestroyNotify &&
                event.xdestroywindow.window == window) {
                return false;
            }
        }
        if (resized || retry) {
            if (XStoreName(display, window, sentinel) == 0 ||
                !send_key(display, window, XK_g, false, false)) {
                return false;
            }
            matches = false;
        }
        if (matches) {
            return true;
        }
        {
            struct pollfd descriptors[2];
            int timeout = remaining_timeout(deadline);
            int result;

            if (timeout == 0) {
                return false;
            }
            descriptors[0].fd = ConnectionNumber(display);
            descriptors[0].events = POLLIN;
            descriptors[0].revents = 0;
            descriptors[1].fd = output_descriptor;
            descriptors[1].events = POLLIN;
            descriptors[1].revents = 0;
            result = poll(descriptors, 2, timeout);
            if (result == -1 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                return false;
            }
            if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) !=
                    0 &&
                !drain_output(
                    output_descriptor,
                    output,
                    output_capacity,
                    output_length)) {
                return false;
            }
            if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
                return false;
            }
        }
    }
}

static bool query_cell_geometry_without_resize_request(
    Display *display,
    Window window,
    int output_descriptor,
    unsigned int *height_out,
    unsigned int *width_out,
    char *output,
    size_t output_capacity,
    size_t *output_length)
{
    uint64_t deadline =
        monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;

    if (XStoreName(display, window, "awaiting-cell-geometry") == 0 ||
        !send_key(display, window, XK_p, false, false)) {
        return false;
    }
    for (;;) {
        char *title = NULL;
        unsigned int height = 0;
        unsigned int width = 0;
        int parsed_length = 0;
        bool matches = false;

        XSync(display, False);
        if (XFetchName(display, window, &title) != 0 && title != NULL) {
            matches = sscanf(
                          title,
                          "cell-%u-%u%n",
                          &height,
                          &width,
                          &parsed_length) == 2 &&
                      parsed_length > 0 &&
                      title[parsed_length] == '\0' &&
                      height > 0 && width > 0;
            XFree(title);
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if ((event.type == ConfigureRequest &&
                 event.xconfigurerequest.window == window) ||
                (event.type == DestroyNotify &&
                 event.xdestroywindow.window == window)) {
                return false;
            }
        }
        if (matches) {
            *height_out = height;
            *width_out = width;
            return true;
        }
        {
            struct pollfd descriptors[2];
            int timeout = remaining_timeout(deadline);
            int result;

            if (timeout == 0) {
                return false;
            }
            descriptors[0].fd = ConnectionNumber(display);
            descriptors[0].events = POLLIN;
            descriptors[0].revents = 0;
            descriptors[1].fd = output_descriptor;
            descriptors[1].events = POLLIN;
            descriptors[1].revents = 0;
            result = poll(descriptors, 2, timeout);
            if (result == -1 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                return false;
            }
            if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) !=
                    0 &&
                !drain_output(
                    output_descriptor,
                    output,
                    output_capacity,
                    output_length)) {
                return false;
            }
            if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
                return false;
            }
        }
    }
}

static bool wait_for_title(
    Display *display,
    Window window,
    int output_descriptor,
    const char *expected,
    char *output,
    size_t output_capacity,
    size_t *output_length)
{
    uint64_t deadline =
        monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;

    for (;;) {
        char *title = NULL;
        bool matches = false;

        XSync(display, False);
        if (XFetchName(display, window, &title) != 0 && title != NULL) {
            matches = strcmp(title, expected) == 0;
            XFree(title);
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == ConfigureRequest &&
                event.xconfigurerequest.window == window) {
                if (!apply_configure_request(
                        display,
                        &event.xconfigurerequest)) {
                    return false;
                }
            } else if (event.type == DestroyNotify &&
                       event.xdestroywindow.window == window) {
                return false;
            }
        }
        if (matches) {
            return true;
        }
        {
            struct pollfd descriptors[2];
            int timeout = remaining_timeout(deadline);
            int result;

            if (timeout == 0) {
                return false;
            }
            descriptors[0].fd = ConnectionNumber(display);
            descriptors[0].events = POLLIN;
            descriptors[0].revents = 0;
            descriptors[1].fd = output_descriptor;
            descriptors[1].events = POLLIN;
            descriptors[1].revents = 0;
            result = poll(descriptors, 2, timeout);
            if (result == -1 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                return false;
            }
            if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) !=
                    0 &&
                !drain_output(
                    output_descriptor,
                    output,
                    output_capacity,
                    output_length)) {
                return false;
            }
            if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
                return false;
            }
        }
    }
}

static bool send_wm_delete(Display *display, Window window)
{
    XEvent event;
    Atom wm_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    Atom delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);

    if (wm_protocols == None || delete_window == None) {
        return false;
    }
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.display = display;
    event.xclient.window = window;
    event.xclient.message_type = wm_protocols;
    event.xclient.format = 32;
    event.xclient.data.l[0] = (long)delete_window;
    event.xclient.data.l[1] = CurrentTime;
    if (XSendEvent(display, window, False, NoEventMask, &event) == 0) {
        return false;
    }
    XFlush(display);
    return true;
}

static bool wait_for_exit(
    pid_t process,
    int output_descriptor,
    int *status_out,
    char *output,
    size_t output_capacity,
    size_t *output_length)
{
    uint64_t deadline =
        monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;

    for (;;) {
        struct pollfd descriptor;
        pid_t result = waitpid(process, status_out, WNOHANG);
        int timeout;
        int poll_result;

        if (result == process) {
            (void)drain_output(
                output_descriptor,
                output,
                output_capacity,
                output_length);
            return true;
        }
        if (result == -1) {
            return false;
        }
        timeout = remaining_timeout(deadline);
        if (timeout == 0) {
            return false;
        }
        descriptor.fd = output_descriptor;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        poll_result = poll(&descriptor, 1, timeout);
        if (poll_result == -1 && errno == EINTR) {
            continue;
        }
        if (poll_result <= 0 ||
            !drain_output(
                output_descriptor,
                output,
                output_capacity,
                output_length)) {
            return false;
        }
    }
}

int main(int argument_count, char **arguments)
{
    Display *display = NULL;
    Window root;
    Window window = None;
    XWindowAttributes attributes;
    XWindowAttributes resized_attributes;
    XWindowAttributes zoomed_attributes;
    XWindowAttributes reset_attributes;
    Damage dirty = None;
    pid_t process = -1;
    int output_descriptor = -1;
    int dirty_event_base;
    int dirty_error_base;
    unsigned int cell_height = 0;
    unsigned int cell_width = 0;
    int status = 0;
    char output[4096] = "";
    size_t output_length = 0;
    bool passed = false;

    if (argument_count != 3) {
        fprintf(stderr, "usage: %s GTK4_SAMPLE PTY_HELPER\n", arguments[0]);
        return 2;
    }
    if (boxrig_term_gtk4_dispatch_needs_immediate_wake(
            false,
            false,
            false,
            false) ||
        boxrig_term_gtk4_dispatch_needs_immediate_wake(
            false,
            false,
            true,
            false) ||
        !boxrig_term_gtk4_dispatch_needs_immediate_wake(
            false,
            true,
            true,
            false) ||
        !boxrig_term_gtk4_dispatch_needs_immediate_wake(
            false,
            false,
            false,
            true) ||
        !boxrig_term_gtk4_dispatch_needs_immediate_wake(
            true,
            false,
            false,
            false)) {
        fprintf(
            stderr,
            "unmapped GTK4 frame request caused an immediate wakeup\n");
        return 1;
    }
    if (boxrig_term_gtk4_descriptor_events_need_update(5u, 5u) ||
        !boxrig_term_gtk4_descriptor_events_need_update(5u, 7u)) {
        fprintf(stderr, "unchanged GTK4 descriptor events caused a wakeup\n");
        return 1;
    }
    display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "could not open the X11 display\n");
        goto cleanup;
    }
    root = DefaultRootWindow(display);
    XSelectInput(
        display,
        root,
        SubstructureNotifyMask | SubstructureRedirectMask);
    XSync(display, False);
    process = start_sample(arguments[1], arguments[2], &output_descriptor);
    if (process == -1) {
        perror("start GTK4 sample");
        goto cleanup;
    }
    if (!wait_for_window(
            display,
            root,
            output_descriptor,
            &window,
            output,
            sizeof(output),
            &output_length)) {
        fprintf(stderr, "GTK4 sample did not map a window\n%s", output);
        goto cleanup;
    }
    if (XGetWindowAttributes(display, window, &attributes) == 0 ||
        attributes.map_state != IsViewable || attributes.width <= 1 ||
        attributes.height <= 1) {
        fprintf(stderr, "GTK4 sample mapped without a visible allocation\n");
        goto cleanup;
    }
    if (!send_wm_delete(display, window) ||
        !wait_for_exit(
            process,
            output_descriptor,
            &status,
            output,
            sizeof(output),
            &output_length)) {
        fprintf(stderr, "GTK4 sample did not close after WM_DELETE\n%s", output);
        goto cleanup;
    }
    process = -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "GTK4 sample exited abnormally\n%s", output);
        goto cleanup;
    }
    XSync(display, False);
    while (XPending(display) > 0) {
        XEvent event;

        XNextEvent(display, &event);
    }
    close(output_descriptor);
    output_descriptor = -1;
    window = None;
    status = 0;
    output[0] = '\0';
    output_length = 0;

    process = start_sample(arguments[1], arguments[2], &output_descriptor);
    if (process == -1) {
        perror("restart GTK4 sample");
        goto cleanup;
    }
    if (!wait_for_window(
            display,
            root,
            output_descriptor,
            &window,
            output,
            sizeof(output),
            &output_length)) {
        fprintf(stderr, "GTK4 sample did not remap a window\n%s", output);
        goto cleanup;
    }
    XSelectInput(
        display,
        window,
        PropertyChangeMask | StructureNotifyMask);
    XSync(display, False);
    if (XGetWindowAttributes(display, window, &attributes) == 0 ||
        attributes.width <= 1 || attributes.height <= 1 ||
        !query_grid_without_resize_request(
            display,
            window,
            output_descriptor,
            "awaiting-initial-grid",
            24,
            80,
            true,
            output,
            sizeof(output),
            &output_length) ||
        XGetWindowAttributes(display, window, &attributes) == 0) {
        fprintf(
            stderr,
            "GTK4 sample did not start with a 24x80 grid\n%s",
            output);
        goto cleanup;
    }
    if (!XDamageQueryExtension(
            display,
            &dirty_event_base,
            &dirty_error_base) ||
        (dirty = XDamageCreate(
             display,
             window,
             XDamageReportNonEmpty)) == None) {
        fprintf(stderr, "could not observe GTK4 sample dirty\n");
        goto cleanup;
    }
    XDamageSubtract(display, dirty, None, None);
    XSync(display, False);
    if (!send_key(display, window, XK_o, true, true) ||
        !wait_for_background_alpha(
            display,
            window,
            dirty,
            dirty_event_base,
            output_descriptor,
            204,
            output,
            sizeof(output),
            &output_length)) {
        fprintf(
            stderr,
            "GTK4 sample did not preserve 80%% background alpha\n%s",
            output);
        goto cleanup;
    }
    XDamageDestroy(display, dirty);
    XSync(display, False);
    dirty = None;
    if (!query_cell_geometry_without_resize_request(
            display,
            window,
            output_descriptor,
            &cell_height,
            &cell_width,
            output,
            sizeof(output),
            &output_length) ||
        cell_width > (unsigned int)(INT_MAX - 1) / 4u ||
        cell_height > (unsigned int)(INT_MAX - 1) / 2u ||
        attributes.width > INT_MAX - (int)(cell_width * 4u) - 1 ||
        attributes.height > INT_MAX - (int)(cell_height * 2u) - 1) {
        fprintf(
            stderr,
            "GTK4 sample did not report its cell geometry\n%s",
            output);
        goto cleanup;
    }
    /* A pointer-owned allocation remains authoritative even when its content
     * extent includes a sub-cell remainder. */
    XResizeWindow(
        display,
        window,
        (unsigned int)attributes.width + cell_width * 2u + 1u,
        (unsigned int)attributes.height + cell_height + 1u);
    XSync(display, False);
    if (!query_grid_without_resize_request(
            display,
            window,
            output_descriptor,
            "awaiting-first-user-resized-grid",
            25,
            82,
            false,
            output,
            sizeof(output),
            &output_length) ||
        XGetWindowAttributes(display, window, &resized_attributes) == 0 ||
        resized_attributes.width !=
            attributes.width + (int)(cell_width * 2u) + 1 ||
        resized_attributes.height !=
            attributes.height + (int)cell_height + 1) {
        fprintf(
            stderr,
            "GTK4 sample corrected the first user allocation\n%s",
            output);
        goto cleanup;
    }
    XResizeWindow(
        display,
        window,
        (unsigned int)attributes.width + cell_width * 4u + 1u,
        (unsigned int)attributes.height + cell_height * 2u + 1u);
    XSync(display, False);
    if (!query_grid_without_resize_request(
            display,
            window,
            output_descriptor,
            "awaiting-user-resized-grid",
            26,
            84,
            false,
            output,
            sizeof(output),
            &output_length) ||
        XGetWindowAttributes(display, window, &resized_attributes) == 0 ||
        resized_attributes.width !=
            attributes.width + (int)(cell_width * 4u) + 1 ||
        resized_attributes.height !=
            attributes.height + (int)(cell_height * 2u) + 1) {
        fprintf(
            stderr,
            "GTK4 sample did not preserve the user-resized 26x84 grid\n%s",
            output);
        goto cleanup;
    }
    if (!send_key(display, window, XK_equal, true, true) ||
        !wait_for_resize_request(
            display,
            window,
            output_descriptor,
            resized_attributes.width,
            resized_attributes.height,
            true,
            true,
            &zoomed_attributes,
            output,
            sizeof(output),
            &output_length) ||
        !query_grid_without_resize_request(
            display,
            window,
            output_descriptor,
            "awaiting-zoomed-grid",
            26,
            84,
            false,
            output,
            sizeof(output),
            &output_length)) {
        fprintf(
            stderr,
            "GTK4 Ctrl+Plus did not resize once while preserving 26x84\n%s",
            output);
        goto cleanup;
    }
    if (!send_key(display, window, XK_0, true, false) ||
        !wait_for_resize_request(
            display,
            window,
            output_descriptor,
            attributes.width + (int)(cell_width * 4u),
            attributes.height + (int)(cell_height * 2u),
            false,
            true,
            &reset_attributes,
            output,
            sizeof(output),
            &output_length) ||
        reset_attributes.width !=
            attributes.width + (int)(cell_width * 4u) ||
        reset_attributes.height !=
            attributes.height + (int)(cell_height * 2u) ||
        !query_grid_without_resize_request(
            display,
            window,
            output_descriptor,
            "awaiting-reset-grid",
            26,
            84,
            false,
            output,
            sizeof(output),
            &output_length)) {
        fprintf(
            stderr,
            "GTK4 Ctrl+0 did not restore once while preserving 26x84\n%s",
            output);
        goto cleanup;
    }
    if (!send_key(display, window, XK_k, false, false) ||
        !wait_for_title(
            display,
            window,
            output_descriptor,
            "control-input-ready",
            output,
            sizeof(output),
            &output_length) ||
        !send_key(display, window, XK_2, true, false) ||
        !send_key(display, window, XK_3, true, false) ||
        !send_key(display, window, XK_slash, true, false) ||
        !send_key(display, window, XK_question, true, true) ||
        !send_key(display, window, XK_c, true, false) ||
        !send_key(display, window, XK_z, true, false) ||
        !wait_for_title(
            display,
            window,
            output_descriptor,
            "control-input-received",
            output,
            sizeof(output),
            &output_length)) {
        fprintf(
            stderr,
            "GTK4 control input did not reach the raw PTY fixture\n%s",
            output);
        goto cleanup;
    }
    if (!send_key(display, window, XK_q, false, false) ||
        !wait_for_exit(
            process,
            output_descriptor,
            &status,
            output,
            sizeof(output),
            &output_length)) {
        fprintf(
            stderr,
            "GTK4 sample did not exit after zoom and reset\n%s",
            output);
        goto cleanup;
    }
    process = -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "GTK4 sample exited abnormally\n%s", output);
        goto cleanup;
    }
    passed = true;

cleanup:
    if (dirty != None && display != NULL) {
        XDamageDestroy(display, dirty);
        XSync(display, False);
    }
    if (process > 0) {
        kill(-process, SIGTERM);
        waitpid(process, NULL, 0);
    }
    if (output_descriptor != -1) {
        close(output_descriptor);
    }
    if (display != NULL) {
        XCloseDisplay(display);
    }
    if (!passed) {
        return 1;
    }
    printf(
        "PASS GTK4 sample preserves user resize, alpha, and zoom grid\n");
    return 0;
}
