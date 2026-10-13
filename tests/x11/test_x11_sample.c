/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/sync.h>
#include <X11/keysym.h>

#include "boxrig/x11/scrollbar.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

#include "../x11_video.h"

typedef struct test_key {
    KeySym symbol;
    unsigned int state;
} test_key;

static bool read_notification_events(
    int descriptor,
    size_t *notification_count);

static bool wait_for_emitter_iteration(
    Display *display,
    Window window,
    int process_output,
    int timeout_milliseconds);

static pid_t start_xvfb(char *display_name, size_t display_name_capacity)
{
    int display_pipe[2];
    pid_t process;
    char number[32];
    size_t length = 0;

    if (pipe(display_pipe) == -1) {
        return -1;
    }
    process = fork();
    if (process == -1) {
        close(display_pipe[0]);
        close(display_pipe[1]);
        return -1;
    }
    if (process == 0) {
        char descriptor[16];

        close(display_pipe[0]);
        if (snprintf(descriptor, sizeof(descriptor), "%d", display_pipe[1]) < 0) {
            _exit(127);
        }
        execlp(
            "Xvfb",
            "Xvfb",
            "-displayfd",
            descriptor,
            "-screen",
            "0",
            "3840x2160x24",
            "-nolisten",
            "tcp",
            (char *)NULL);
        _exit(127);
    }
    close(display_pipe[1]);
    while (length + 1 < sizeof(number)) {
        ssize_t count = read(
            display_pipe[0],
            number + length,
            sizeof(number) - length - 1);

        if (count <= 0) {
            break;
        }
        length += (size_t)count;
        if (memchr(number, '\n', length) != NULL) {
            break;
        }
    }
    close(display_pipe[0]);
    if (length == 0) {
        kill(process, SIGTERM);
        waitpid(process, NULL, 0);
        errno = EIO;
        return -1;
    }
    number[strcspn(number, "\r\n")] = '\0';
    if (snprintf(display_name, display_name_capacity, ":%s", number) < 0) {
        kill(process, SIGTERM);
        waitpid(process, NULL, 0);
        errno = EIO;
        return -1;
    }
    return process;
}

static pid_t start_sample_with_renderer(
    const char *sample_path,
    char *const command_arguments[],
    const char *display_name,
    const char *term,
    bool allow_notifications,
    const char *renderer_argument,
    int *output_descriptor)
{
    int output_pipe[2];
    pid_t process;
    size_t argument_count = 0;

    while (command_arguments[argument_count] != NULL) {
        argument_count++;
    }
    if (argument_count == 0 || argument_count > 12) {
        errno = E2BIG;
        return -1;
    }

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
        char *sample_arguments[16];
        size_t index;

        close(output_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) == -1 ||
            dup2(output_pipe[1], STDERR_FILENO) == -1 ||
            setenv("DISPLAY", display_name, 1) == -1 ||
            setenv("BOXRIG_TERM", term, 1) == -1 ||
            (allow_notifications
                 ? setenv("BOXRIG_ALLOW_NOTIFICATIONS", "1", 1)
                 : unsetenv("BOXRIG_ALLOW_NOTIFICATIONS")) == -1) {
            _exit(127);
        }
        close(output_pipe[1]);
        sample_arguments[0] = (char *)sample_path;
        sample_arguments[1] = (char *)renderer_argument;
        sample_arguments[2] = "--";
        for (index = 0; index < argument_count; index++) {
            sample_arguments[index + 3] = command_arguments[index];
        }
        sample_arguments[argument_count + 3] = NULL;
        execv(sample_path, sample_arguments);
        _exit(127);
    }
    close(output_pipe[1]);
    *output_descriptor = output_pipe[0];
    return process;
}

static pid_t start_sample(
    const char *sample_path,
    char *const command_arguments[],
    const char *display_name,
    const char *term,
    bool allow_notifications,
    int *output_descriptor)
{
    /* This suite owns the X11 Cairo compositor and pixel contracts;
     * shared sample gates exercise the default auto renderer. */
    return start_sample_with_renderer(sample_path, command_arguments,
        display_name, term, allow_notifications, "--renderer=x11", output_descriptor);
}

static pid_t start_fake_notification_service(
    const char *service_path,
    int *output_descriptor)
{
    int output_pipe[2];
    pid_t process;
    struct pollfd descriptor;
    char ready = 0;

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
        if (dup2(output_pipe[1], STDOUT_FILENO) == -1) {
            _exit(127);
        }
        close(output_pipe[1]);
        execl(service_path, service_path, (char *)NULL);
        _exit(127);
    }
    close(output_pipe[1]);
    descriptor.fd = output_pipe[0];
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    if (poll(&descriptor, 1, 5000) != 1 ||
        (descriptor.revents & POLLIN) == 0 ||
        read(output_pipe[0], &ready, 1) != 1 || ready != 'R') {
        close(output_pipe[0]);
        kill(process, SIGTERM);
        waitpid(process, NULL, 0);
        errno = EIO;
        return -1;
    }
    *output_descriptor = output_pipe[0];
    return process;
}

static bool wait_for_window(
    Display *display,
    int process_output,
    Window *window_out)
{
    for (;;) {
        struct pollfd descriptors[2];
        int result;

        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == MapNotify) {
                *window_out = event.xmap.window;
                return true;
            }
        }
        descriptors[0].fd = ConnectionNumber(display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = process_output;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2, 5000);
        if (result <= 0 || (descriptors[1].revents & POLLHUP) != 0) {
            return false;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            char buffer[256];
            ssize_t count;

            count = read(process_output, buffer, sizeof(buffer));
            if (count < 0 && errno != EINTR) {
                return false;
            }
        }
    }
}

static bool send_key_transition(
    Display *display,
    Window window,
    test_key key,
    int type)
{
    XKeyEvent key_event;
    KeyCode keycode = XKeysymToKeycode(display, key.symbol);

    if (keycode == 0) {
        return false;
    }
    memset(&key_event, 0, sizeof(key_event));
    key_event.type = type;
    key_event.display = display;
    key_event.window = window;
    key_event.root = DefaultRootWindow(display);
    key_event.time = CurrentTime;
    key_event.x = 1;
    key_event.y = 1;
    key_event.x_root = 1;
    key_event.y_root = 1;
    key_event.same_screen = True;
    key_event.keycode = keycode;
    key_event.state = key.state;
    if (XSendEvent(
            display,
            window,
            True,
            type == KeyPress ? KeyPressMask : KeyReleaseMask,
            (XEvent *)&key_event) == 0) {
        return false;
    }
    XFlush(display);
    return true;
}

static bool send_key(
    Display *display,
    Window window,
    test_key key)
{
    return send_key_transition(display, window, key, KeyPress);
}

static bool window_has_no_children(Display *display, Window window)
{
    Window root;
    Window parent;
    Window *children = NULL;
    unsigned int child_count = 0;
    bool result;

    if (XQueryTree(
            display,
            window,
            &root,
            &parent,
            &children,
            &child_count) == 0) {
        return false;
    }
    result = child_count == 0;
    if (children != NULL) {
        XFree(children);
    }
    return result;
}

static bool set_wm_extended_sync_support(Display *display, bool supported)
{
    Window root = DefaultRootWindow(display);
    Atom net_supported =
        XInternAtom(display, "_NET_SUPPORTED", False);
    Atom frame_drawn =
        XInternAtom(display, "_NET_WM_FRAME_DRAWN", False);
    unsigned long value = frame_drawn;

    if (net_supported == None || frame_drawn == None) {
        return false;
    }
    if (supported) {
        XChangeProperty(
            display,
            root,
            net_supported,
            XA_ATOM,
            32,
            PropModeReplace,
            (unsigned char *)&value,
            1);
    } else {
        XDeleteProperty(display, root, net_supported);
    }
    XSync(display, False);
    return true;
}

static bool read_wm_sync_registration(
    Display *display,
    Window window,
    bool *protocol_registered_out,
    unsigned int *counter_count_out,
    XSyncCounter *basic_counter_out,
    XSyncCounter *extended_counter_out)
{
    Atom sync_request =
        XInternAtom(display, "_NET_WM_SYNC_REQUEST", False);
    Atom counter_property =
        XInternAtom(display, "_NET_WM_SYNC_REQUEST_COUNTER", False);
    Atom *protocols = NULL;
    int protocol_count = 0;
    unsigned char *property = NULL;
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    bool protocol_registered = false;
    int index;

    if (sync_request == None || counter_property == None ||
        protocol_registered_out == NULL || counter_count_out == NULL ||
        basic_counter_out == NULL || extended_counter_out == NULL ||
        XGetWMProtocols(display, window, &protocols, &protocol_count) == 0) {
        return false;
    }
    for (index = 0; index < protocol_count; index++) {
        if (protocols[index] == sync_request) {
            protocol_registered = true;
        }
    }
    XFree(protocols);
    if (XGetWindowProperty(
            display,
            window,
            counter_property,
            0,
            2,
            False,
            XA_CARDINAL,
            &actual_type,
            &actual_format,
            &item_count,
            &bytes_after,
            &property) != Success) {
        return false;
    }
    *basic_counter_out = None;
    *extended_counter_out = None;
    if (actual_type == XA_CARDINAL && actual_format == 32 &&
        item_count > 0 && item_count <= 2 && bytes_after == 0 &&
        property != NULL) {
        *basic_counter_out =
            (XSyncCounter)((unsigned long *)property)[0];
        if (item_count == 2) {
            *extended_counter_out =
                (XSyncCounter)((unsigned long *)property)[1];
        }
        if (*basic_counter_out == None ||
            (item_count == 2 && *extended_counter_out == None)) {
            XFree(property);
            return false;
        }
    } else if (actual_type != None || actual_format != 0 ||
               item_count != 0 || bytes_after != 0) {
        if (property != NULL) {
            XFree(property);
        }
        return false;
    }
    if (property != NULL) {
        XFree(property);
    }
    *protocol_registered_out = protocol_registered;
    *counter_count_out = (unsigned int)item_count;
    return true;
}

static bool send_wm_sync_request(
    Display *display,
    Window window,
    XSyncValue value,
    bool extended)
{
    XEvent event;
    Atom wm_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    Atom sync_request =
        XInternAtom(display, "_NET_WM_SYNC_REQUEST", False);

    if (wm_protocols == None || sync_request == None) {
        return false;
    }
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.display = display;
    event.xclient.window = window;
    event.xclient.message_type = wm_protocols;
    event.xclient.format = 32;
    event.xclient.data.l[0] = (long)sync_request;
    event.xclient.data.l[1] = CurrentTime;
    event.xclient.data.l[2] = (long)XSyncValueLow32(value);
    event.xclient.data.l[3] = (long)(uint32_t)XSyncValueHigh32(value);
    event.xclient.data.l[4] = extended ? 1 : 0;
    if (XSendEvent(display, window, False, NoEventMask, &event) == 0) {
        return false;
    }
    XFlush(display);
    return true;
}

static bool send_scrollbar_button(
    Display *display,
    Window window,
    int x,
    int y,
    unsigned int button,
    int type)
{
    XButtonEvent event;

    memset(&event, 0, sizeof(event));
    event.type = type;
    event.display = display;
    event.window = window;
    event.root = DefaultRootWindow(display);
    event.time = CurrentTime;
    event.x = x;
    event.y = y;
    event.x_root = x;
    event.y_root = y;
    event.state = type == ButtonRelease ? Button1Mask : 0;
    event.button = button;
    event.same_screen = True;
    if (XSendEvent(
            display,
            window,
            False,
            type == ButtonPress ? ButtonPressMask : ButtonReleaseMask,
            (XEvent *)&event) == 0) {
        return false;
    }
    XFlush(display);
    return true;
}

static bool send_scrollbar_motion(
    Display *display,
    Window window,
    int x,
    int y)
{
    XMotionEvent event;

    memset(&event, 0, sizeof(event));
    event.type = MotionNotify;
    event.display = display;
    event.window = window;
    event.root = DefaultRootWindow(display);
    event.time = CurrentTime;
    event.x = x;
    event.y = y;
    event.x_root = x;
    event.y_root = y;
    event.state = Button1Mask;
    event.same_screen = True;
    if (XSendEvent(
            display,
            window,
            False,
            PointerMotionMask,
            (XEvent *)&event) == 0) {
        return false;
    }
    XFlush(display);
    return true;
}

static bool send_terminal_button(
    Display *display,
    Window window,
    int x,
    int y,
    unsigned int button,
    int type,
    unsigned int state,
    Time time)
{
    XButtonEvent event;

    memset(&event, 0, sizeof(event));
    event.type = type;
    event.display = display;
    event.window = window;
    event.root = DefaultRootWindow(display);
    event.time = time;
    event.x = x;
    event.y = y;
    event.x_root = x;
    event.y_root = y;
    event.state = state;
    event.button = button;
    event.same_screen = True;
    if (XSendEvent(
            display,
            window,
            False,
            type == ButtonPress ? ButtonPressMask : ButtonReleaseMask,
            (XEvent *)&event) == 0) {
        return false;
    }
    XFlush(display);
    return true;
}

static bool send_terminal_motion(
    Display *display,
    Window window,
    int x,
    int y,
    unsigned int state,
    Time time)
{
    XMotionEvent event;

    memset(&event, 0, sizeof(event));
    event.type = MotionNotify;
    event.display = display;
    event.window = window;
    event.root = DefaultRootWindow(display);
    event.time = time;
    event.x = x;
    event.y = y;
    event.x_root = x;
    event.y_root = y;
    event.state = state;
    event.same_screen = True;
    if (XSendEvent(
            display,
            window,
            False,
            PointerMotionMask,
            (XEvent *)&event) == 0) {
        return false;
    }
    XFlush(display);
    return true;
}

static bool wait_for_selection_owner(
    Display *display,
    Atom selection,
    Window owner,
    int process_output)
{
    size_t attempt;

    for (attempt = 0; attempt < 100; attempt++) {
        struct pollfd descriptors[2];
        int result;

        XSync(display, False);
        if (XGetSelectionOwner(display, selection) == owner) {
            return true;
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == DestroyNotify &&
                event.xdestroywindow.window == owner) {
                return false;
            }
        }
        descriptors[0].fd = ConnectionNumber(display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = process_output;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2, 50);
        if (result < 0 && errno != EINTR) {
            return false;
        }
        if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
            return false;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            char buffer[256];

            if (read(process_output, buffer, sizeof(buffer)) <= 0) {
                return false;
            }
        }
    }
    return false;
}

static bool request_selection_text(
    Display *display,
    Window requestor,
    Atom selection,
    Atom utf8_string,
    Atom property,
    const char *expected)
{
    XEvent event;
    unsigned char *value = NULL;
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    size_t attempt;
    bool notified = false;
    bool matched = false;

    XConvertSelection(
        display,
        selection,
        utf8_string,
        property,
        requestor,
        CurrentTime);
    XFlush(display);
    for (attempt = 0; attempt < 100 && !notified; attempt++) {
        struct pollfd descriptor;
        int result;

        while (XPending(display) > 0) {
            XNextEvent(display, &event);
            if (event.type == SelectionNotify &&
                event.xselection.requestor == requestor &&
                event.xselection.selection == selection &&
                event.xselection.target == utf8_string) {
                notified = event.xselection.property == property;
                if (!notified) {
                    return false;
                }
                break;
            }
        }
        if (notified) {
            break;
        }
        descriptor.fd = ConnectionNumber(display);
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        result = poll(&descriptor, 1, 50);
        if (result < 0 && errno != EINTR) {
            return false;
        }
    }
    if (!notified ||
        XGetWindowProperty(
            display,
            requestor,
            property,
            0,
            1024,
            True,
            utf8_string,
            &actual_type,
            &actual_format,
            &item_count,
            &bytes_after,
            &value) != Success) {
        return false;
    }
    matched = actual_type == utf8_string && actual_format == 8 &&
              bytes_after == 0 && item_count == strlen(expected) &&
              value != NULL && memcmp(value, expected, item_count) == 0;
    if (value != NULL) {
        XFree(value);
    }
    return matched;
}

static bool wait_for_selection_text(
    Display *display,
    Window requestor,
    Atom selection,
    Atom utf8_string,
    Atom property,
    const char *expected)
{
    size_t attempt;

    for (attempt = 0; attempt < 8; attempt++) {
        if (request_selection_text(
                display,
                requestor,
                selection,
                utf8_string,
                property,
                expected)) {
            return true;
        }
    }
    return false;
}

static bool respond_to_selection_request(
    Display *display,
    const XSelectionRequestEvent *request,
    Atom targets,
    Atom utf8_string,
    const char *text)
{
    XEvent notify;
    Atom property = request->property == None
                        ? request->target
                        : request->property;

    memset(&notify, 0, sizeof(notify));
    notify.xselection.type = SelectionNotify;
    notify.xselection.display = display;
    notify.xselection.requestor = request->requestor;
    notify.xselection.selection = request->selection;
    notify.xselection.target = request->target;
    notify.xselection.property = property;
    notify.xselection.time = request->time;
    if (request->target == targets) {
        Atom offered[] = {targets, utf8_string};

        XChangeProperty(
            display,
            request->requestor,
            property,
            XA_ATOM,
            32,
            PropModeReplace,
            (const unsigned char *)offered,
            (int)(sizeof(offered) / sizeof(offered[0])));
    } else if (request->target == utf8_string) {
        XChangeProperty(
            display,
            request->requestor,
            property,
            utf8_string,
            8,
            PropModeReplace,
            (const unsigned char *)text,
            (int)strlen(text));
    } else {
        notify.xselection.property = None;
    }
    if (XSendEvent(
            display,
            request->requestor,
            False,
            0,
            &notify) == 0) {
        return false;
    }
    XFlush(display);
    return true;
}

static bool wait_for_title_serving_selection(
    Display *display,
    Window sample_window,
    int process_output,
    Atom targets,
    Atom utf8_string,
    const char *selection_text,
    const char *expected_title)
{
    size_t attempt;

    for (attempt = 0; attempt < 100; attempt++) {
        struct pollfd descriptors[2];
        char *title = NULL;
        bool title_matches = false;
        int result;

        XSync(display, False);
        if (XFetchName(display, sample_window, &title) != 0 &&
            title != NULL) {
            title_matches = strcmp(title, expected_title) == 0;
            XFree(title);
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == SelectionRequest) {
                if (!respond_to_selection_request(
                        display,
                        &event.xselectionrequest,
                        targets,
                        utf8_string,
                        selection_text)) {
                    return false;
                }
            } else if (event.type == DestroyNotify &&
                       event.xdestroywindow.window == sample_window) {
                return false;
            }
        }
        if (title_matches) {
            return true;
        }
        descriptors[0].fd = ConnectionNumber(display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = process_output;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2, 50);
        if (result < 0 && errno != EINTR) {
            return false;
        }
        if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
            return false;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            char buffer[256];

            if (read(process_output, buffer, sizeof(buffer)) <= 0) {
                return false;
            }
        }
    }
    return false;
}

static bool read_window_hash(
    Display *display,
    Window window,
    unsigned int width,
    unsigned int height,
    uint64_t *hash_out)
{
    XImage *image;
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned int x;
    unsigned int y;

    if (width == 0 || height == 0) {
        return false;
    }
    XSync(display, False);
    image = XGetImage(
        display,
        window,
        0,
        0,
        width,
        height,
        AllPlanes,
        ZPixmap);
    if (image == NULL) {
        return false;
    }
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            hash ^= (uint64_t)XGetPixel(image, (int)x, (int)y);
            hash *= UINT64_C(1099511628211);
        }
    }
    XDestroyImage(image);
    *hash_out = hash;
    return true;
}

static bool color_channel_near(uint8_t actual, uint8_t expected)
{
    return actual > expected ? actual - expected <= 8
                             : expected - actual <= 8;
}

static bool wait_for_graphics_fixture(
    Display *display,
    Window window,
    int process_output)
{
    size_t attempt;

    for (attempt = 0; attempt < 100; attempt++) {
        XWindowAttributes attributes;
        XImage *image = NULL;
        struct pollfd descriptors[2];
        int result;
        bool matched = false;

        XSync(display, False);
        if (XGetWindowAttributes(display, window, &attributes) != 0 &&
            attributes.width >
                (int)boxrig_x11_scrollbar_pixel_width() &&
            attributes.height > 0) {
            unsigned int content_width =
                (unsigned int)attributes.width -
                boxrig_x11_scrollbar_pixel_width();
            unsigned int content_height = (unsigned int)attributes.height;
            unsigned int cell_width = content_width / 80;
            unsigned int cell_height = content_height / 24;

            if (cell_width >= 4 && cell_height >= 2 &&
                cell_width * 80 == content_width &&
                cell_height * 24 == content_height) {
                static const struct expected_color {
                    unsigned int column;
                    unsigned int row;
                    unsigned int columns;
                    unsigned int rows;
                    unsigned int source_x;
                    unsigned int source_y;
                    unsigned int source_width;
                    unsigned int source_height;
                    uint8_t red;
                    uint8_t green;
                    uint8_t blue;
                } expected[] = {
                    {0, 3, 4, 2, 4, 1, 16, 16, 255, 220, 64},
                    {0, 3, 4, 2, 10, 10, 16, 16, 224, 48, 48},
                    {0, 3, 4, 2, 7, 14, 16, 16, 40, 120, 240},
                    {6, 3, 2, 2, 2, 10, 8, 16, 224, 48, 48},
                    {0, 6, 4, 2, 10, 10, 16, 16, 24, 112, 48},
                    {6, 6, 4, 2, 10, 10, 16, 16, 144, 64, 224},
                    {0, 9, 4, 2, 10, 10, 16, 16, 224, 176, 40},
                    {6, 9, 4, 2, 10, 10, 16, 16, 0, 0, 0},
                    {0, 12, 4, 2, 10, 10, 16, 16, 240, 112, 32},
                    {0, 15, 4, 2, 10, 10, 16, 16, 32, 192, 208},
                    {1, 21, 4, 1, 7, 2, 16, 8, 240, 248, 255},
                };
                size_t index;

                image = XGetImage(
                    display,
                    window,
                    0,
                    0,
                    content_width,
                    content_height,
                    AllPlanes,
                    ZPixmap);
                matched = image != NULL;
                for (index = 0;
                     matched && index < sizeof(expected) / sizeof(expected[0]);
                     index++) {
                    uint64_t destination_width =
                        (uint64_t)expected[index].columns * cell_width;
                    uint64_t destination_height =
                        (uint64_t)expected[index].rows * cell_height;
                    int x = (int)(expected[index].column * cell_width +
                        ((uint64_t)expected[index].source_x * 2 + 1) *
                            destination_width /
                            ((uint64_t)expected[index].source_width * 2));
                    int y = (int)(expected[index].row * cell_height +
                        ((uint64_t)expected[index].source_y * 2 + 1) *
                            destination_height /
                            ((uint64_t)expected[index].source_height * 2));
                    XColor actual;

                    memset(&actual, 0, sizeof(actual));
                    actual.pixel = XGetPixel(image, x, y) &
                                   (attributes.visual->red_mask |
                                    attributes.visual->green_mask |
                                    attributes.visual->blue_mask);
                    if (XQueryColor(
                            display,
                            attributes.colormap,
                            &actual) == 0 ||
                        !color_channel_near(
                            (uint8_t)(actual.red / 257u),
                            expected[index].red) ||
                        !color_channel_near(
                            (uint8_t)(actual.green / 257u),
                            expected[index].green) ||
                        !color_channel_near(
                            (uint8_t)(actual.blue / 257u),
                            expected[index].blue)) {
                        if (attempt + 1 == 100) {
                            fprintf(
                                stderr,
                                "graphics pixel %u,%u expected %u,%u,%u "
                                "but got %u,%u,%u\n",
                                expected[index].column,
                                expected[index].row,
                                expected[index].red,
                                expected[index].green,
                                expected[index].blue,
                                actual.red / 257u,
                                actual.green / 257u,
                                actual.blue / 257u);
                        }
                        matched = false;
                    }
                }
            }
        }
        if (image != NULL) {
            XDestroyImage(image);
        }
        if (matched) {
            return true;
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == DestroyNotify &&
                event.xdestroywindow.window == window) {
                return false;
            }
        }
        descriptors[0].fd = ConnectionNumber(display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = process_output;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2, 50);
        if (result < 0 && errno != EINTR) {
            return false;
        }
        if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
            return false;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            char buffer[256];

            if (read(process_output, buffer, sizeof(buffer)) <= 0) {
                return false;
            }
        }
    }
    return false;
}

static bool wait_for_window_hash(
    Display *display,
    Window window,
    int process_output,
    unsigned int width,
    unsigned int height,
    uint64_t reference,
    bool equal,
    uint64_t *hash_out)
{
    size_t attempt;

    for (attempt = 0; attempt < 100; attempt++) {
        struct pollfd descriptors[2];
        uint64_t hash;
        int result;

        if (!read_window_hash(display, window, width, height, &hash)) {
            return false;
        }
        if ((hash == reference) == equal) {
            *hash_out = hash;
            return true;
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == DestroyNotify &&
                event.xdestroywindow.window == window) {
                return false;
            }
        }
        descriptors[0].fd = ConnectionNumber(display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = process_output;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2, 50);
        if (result < 0 && errno != EINTR) {
            return false;
        }
        if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
            return false;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            char buffer[256];

            if (read(process_output, buffer, sizeof(buffer)) <= 0) {
                return false;
            }
        }
    }
    return false;
}

static bool wait_for_zoom_geometry(
    Display *display,
    Window window,
    int process_output,
    int initial_width,
    int initial_height,
    bool enlarged)
{
    size_t attempt;

    for (attempt = 0; attempt < 100; attempt++) {
        struct pollfd descriptors[2];
        int result;

        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == ConfigureNotify &&
                event.xconfigure.window == window) {
                if ((enlarged &&
                     event.xconfigure.width > initial_width &&
                     event.xconfigure.height >= initial_height) ||
                    (!enlarged &&
                     event.xconfigure.width == initial_width &&
                     event.xconfigure.height == initial_height)) {
                    return true;
                }
            } else if (event.type == DestroyNotify &&
                       event.xdestroywindow.window == window) {
                return false;
            }
        }
        descriptors[0].fd = ConnectionNumber(display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = process_output;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2, 100);
        if (result < 0 && errno != EINTR) {
            return false;
        }
        if ((descriptors[1].revents & POLLHUP) != 0) {
            return false;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            char buffer[256];
            ssize_t count = read(process_output, buffer, sizeof(buffer));

            if (count <= 0 && errno != EINTR) {
                return false;
            }
        }
    }
    return false;
}

static bool window_has_rendered_content(
    Display *display,
    Window window,
    int process_output)
{
    int screen = DefaultScreen(display);
    XColor background;
    size_t attempt;

    memset(&background, 0, sizeof(background));
    background.red = 0x1010;
    background.green = 0x1010;
    background.blue = 0x1010;
    background.flags = DoRed | DoGreen | DoBlue;
    if (XAllocColor(
            display,
            DefaultColormap(display, screen),
            &background) == 0) {
        return false;
    }
    for (attempt = 0; attempt < 50; attempt++) {
        XWindowAttributes attributes;
        XImage *image;
        size_t background_pixels = 0;
        size_t foreground_pixels = 0;
        size_t total_pixels;
        int x;
        int y;
        struct pollfd descriptors[2];
        int result;

        XSync(display, False);
        if (XGetWindowAttributes(display, window, &attributes) == 0 ||
            attributes.width <= 0 || attributes.height <= 0) {
            return false;
        }
        image = XGetImage(
            display,
            window,
            0,
            0,
            (unsigned int)attributes.width,
            (unsigned int)attributes.height,
            AllPlanes,
            ZPixmap);
        if (image == NULL) {
            return false;
        }
        total_pixels = (size_t)attributes.width * (size_t)attributes.height;
        for (y = 0; y < attributes.height; y++) {
            for (x = 0; x < attributes.width; x++) {
                if ((XGetPixel(image, x, y) & 0x00ffffffUL) ==
                    (background.pixel & 0x00ffffffUL)) {
                    background_pixels++;
                } else {
                    foreground_pixels++;
                }
            }
        }
        XDestroyImage(image);
        if (background_pixels > total_pixels / 2 && foreground_pixels > 100) {
            return true;
        }
        descriptors[0].fd = ConnectionNumber(display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = process_output;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2, 100);
        if (result < 0 && errno != EINTR) {
            return false;
        }
        if ((descriptors[1].revents & POLLHUP) != 0) {
            return false;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            char buffer[256];
            ssize_t count = read(process_output, buffer, sizeof(buffer));

            if (count < 0 && errno != EINTR) {
                return false;
            }
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == DestroyNotify && event.xdestroywindow.window == window) {
                return false;
            }
        }
    }
    return false;
}

static bool wait_for_background_alpha(
    Display *display,
    Window window,
    int process_output,
    uint8_t expected_alpha)
{
    size_t attempt;

    for (attempt = 0; attempt < 50; attempt++) {
        XWindowAttributes attributes;
        XImage *image;
        size_t matching_pixels = 0;
        size_t total_pixels;
        int x;
        int y;
        struct pollfd descriptors[2];
        int result;

        XSync(display, False);
        if (XGetWindowAttributes(display, window, &attributes) == 0 ||
            attributes.depth != 32 || attributes.width <= 0 ||
            attributes.height <= 0) {
            return false;
        }
        image = XGetImage(
            display,
            window,
            0,
            0,
            (unsigned int)attributes.width,
            (unsigned int)attributes.height,
            AllPlanes,
            ZPixmap);
        if (image == NULL) {
            return false;
        }
        total_pixels = (size_t)attributes.width * (size_t)attributes.height;
        for (y = 0; y < attributes.height; y++) {
            for (x = 0; x < attributes.width; x++) {
                if (((XGetPixel(image, x, y) >> 24) & 0xffu) ==
                    expected_alpha) {
                    matching_pixels++;
                }
            }
        }
        XDestroyImage(image);
        if (matching_pixels > total_pixels / 2) {
            return true;
        }
        descriptors[0].fd = ConnectionNumber(display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = process_output;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2, 100);
        if (result < 0 && errno != EINTR) {
            return false;
        }
        if ((descriptors[1].revents & POLLHUP) != 0) {
            return false;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            char buffer[256];
            ssize_t count = read(process_output, buffer, sizeof(buffer));

            if (count < 0 && errno != EINTR) {
                return false;
            }
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == DestroyNotify &&
                event.xdestroywindow.window == window) {
                return false;
            }
        }
    }
    return false;
}

static bool wait_for_process_exit(int output_descriptor)
{
    for (;;) {
        struct pollfd descriptor;
        int result;

        descriptor.fd = output_descriptor;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        result = poll(&descriptor, 1, 5000);
        if (result <= 0) {
            return false;
        }
        if ((descriptor.revents & POLLHUP) != 0) {
            return true;
        }
        if ((descriptor.revents & POLLIN) != 0) {
            char buffer[256];
            ssize_t count = read(output_descriptor, buffer, sizeof(buffer));

            if (count == 0) {
                return true;
            }
        }
    }
}

static bool wait_for_sample_status_and_output(
    pid_t process,
    int output_descriptor,
    int *status_out,
    char *output,
    size_t output_capacity)
{
    size_t length = 0;
    size_t attempt;

    if (status_out == NULL || output == NULL || output_capacity == 0) {
        return false;
    }
    output[0] = '\0';
    for (attempt = 0; attempt < 100; attempt++) {
        struct pollfd descriptor;
        int poll_result;
        pid_t result = waitpid(process, status_out, WNOHANG);

        if (result == process) {
            for (;;) {
                ssize_t count = read(
                    output_descriptor,
                    output + length,
                    output_capacity - length - 1);

                if (count > 0) {
                    length += (size_t)count;
                    output[length] = '\0';
                    if (length + 1 == output_capacity) {
                        break;
                    }
                    continue;
                }
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
            return true;
        }
        if (result == -1) {
            return false;
        }
        descriptor.fd = output_descriptor;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        poll_result = poll(&descriptor, 1, 50);
        if (poll_result < 0 && errno == EINTR) {
            continue;
        }
        if (poll_result < 0) {
            return false;
        }
        if (poll_result > 0 &&
            (descriptor.revents & (POLLIN | POLLHUP)) != 0 &&
            length + 1 < output_capacity) {
            ssize_t count = read(
                output_descriptor,
                output + length,
                output_capacity - length - 1);

            if (count > 0) {
                length += (size_t)count;
                output[length] = '\0';
            }
        }
        if (poll_result > 0 &&
            (descriptor.revents & (POLLHUP | POLLERR)) != 0) {
            do {
                result = waitpid(process, status_out, 0);
            } while (result == -1 && errno == EINTR);
            return result == process;
        }
    }
    return false;
}

static bool rejects_invalid_terminal_type(
    const char *sample_path,
    const char *helper_path,
    const char *display_name)
{
    char *helper_arguments[2] = {(char *)helper_path, NULL};
    char output[256];
    pid_t process;
    int output_descriptor = -1;
    int status = 0;
    bool passed = false;

    process = start_sample(
        sample_path,
        helper_arguments,
        display_name,
        "boxrig",
        false,
        &output_descriptor);
    if (process == -1 ||
        !wait_for_sample_status_and_output(
            process,
            output_descriptor,
            &status,
            output,
            sizeof(output))) {
        goto cleanup;
    }
    process = -1;
    passed = WIFEXITED(status) && WEXITSTATUS(status) == 2 &&
             strstr(output, "BOXRIG_TERM must be xterm or xterm-256color") !=
                 NULL;

cleanup:
    if (output_descriptor != -1) {
        close(output_descriptor);
    }
    if (process > 0) {
        kill(process, SIGTERM);
        waitpid(process, NULL, 0);
    }
    return passed;
}

static bool run_background_opacity_policy(
    Display *display,
    const char *sample_path,
    const char *helper_path,
    const char *display_name)
{
    char *helper_arguments[2] = {(char *)helper_path, NULL};
    const test_key opacity_decrease = {XK_comma, ControlMask | ShiftMask};
    const test_key opacity_increase = {XK_period, ControlMask | ShiftMask};
    const test_key opacity_toggle = {XK_o, ControlMask | ShiftMask};
    const test_key quit_key = {XK_q, 0};
    pid_t process = -1;
    int output_descriptor = -1;
    Window window = None;
    Window compositor_owner = None;
    Atom compositor_selection;
    XWindowAttributes attributes;
    int status = 0;
    char output[512];
    const char *stage = "claim compositor selection";
    bool passed = false;

    memset(&attributes, 0, sizeof(attributes));
    output[0] = '\0';
    compositor_selection = XInternAtom(display, "_NET_WM_CM_S0", False);
    compositor_owner = XCreateSimpleWindow(
        display,
        DefaultRootWindow(display),
        0,
        0,
        1,
        1,
        0,
        0,
        0);
    if (compositor_selection == None || compositor_owner == None ||
        unsetenv("BOXRIG_BACKGROUND_OPACITY") == -1) {
        goto cleanup;
    }
    XSetSelectionOwner(
        display,
        compositor_selection,
        compositor_owner,
        CurrentTime);
    XSync(display, False);
    if (XGetSelectionOwner(display, compositor_selection) !=
        compositor_owner) {
        goto cleanup;
    }
    process = start_sample(
        sample_path,
        helper_arguments,
        display_name,
        "xterm-256color",
        false,
        &output_descriptor);
    stage = "start preferred sample at 100 percent";
    if (process == -1) {
        goto cleanup;
    }
    stage = "map preferred sample";
    if (!wait_for_window(display, output_descriptor, &window)) {
        goto cleanup;
    }
    stage = "inspect preferred Visual";
    if (XGetWindowAttributes(display, window, &attributes) == 0 ||
        attributes.depth != 32) {
        goto cleanup;
    }
    stage = "render preferred sample";
    if (!window_has_rendered_content(display, window, output_descriptor)) {
        goto cleanup;
    }
    stage = "observe initial opaque background";
    if (!wait_for_background_alpha(
            display,
            window,
            output_descriptor,
            255)) {
        goto cleanup;
    }
    stage = "toggle opacity to initial 80 percent";
    if (!send_key_transition(display, window, opacity_toggle, KeyPress) ||
        !send_key_transition(display, window, opacity_toggle, KeyRelease) ||
        !wait_for_background_alpha(
            display,
            window,
            output_descriptor,
            204)) {
        goto cleanup;
    }
    stage = "toggle opacity back to 100 percent";
    if (!send_key_transition(display, window, opacity_toggle, KeyPress) ||
        !send_key_transition(display, window, opacity_toggle, KeyRelease) ||
        !wait_for_background_alpha(
            display,
            window,
            output_descriptor,
            255)) {
        goto cleanup;
    }
    stage = "decrease opacity to 90 percent";
    if (!send_key_transition(display, window, opacity_decrease, KeyPress) ||
        !send_key_transition(display, window, opacity_decrease, KeyRelease) ||
        !wait_for_background_alpha(
            display,
            window,
            output_descriptor,
            230)) {
        goto cleanup;
    }
    stage = "increase opacity to 100 percent";
    if (!send_key_transition(display, window, opacity_increase, KeyPress) ||
        !send_key_transition(display, window, opacity_increase, KeyRelease) ||
        !wait_for_background_alpha(
            display,
            window,
            output_descriptor,
            255)) {
        goto cleanup;
    }
    stage = "keep running after opacity upper-bound no-op";
    if (!send_key_transition(display, window, opacity_increase, KeyPress) ||
        !send_key_transition(display, window, opacity_increase, KeyRelease) ||
        !wait_for_background_alpha(
            display,
            window,
            output_descriptor,
            255)) {
        goto cleanup;
    }
    stage = "toggle opacity to remembered 90 percent";
    if (!send_key_transition(display, window, opacity_toggle, KeyPress) ||
        !send_key_transition(display, window, opacity_toggle, KeyRelease) ||
        !wait_for_background_alpha(
            display,
            window,
            output_descriptor,
            230)) {
        goto cleanup;
    }
    stage = "toggle remembered opacity back to 100 percent";
    if (!send_key_transition(display, window, opacity_toggle, KeyPress) ||
        !send_key_transition(display, window, opacity_toggle, KeyRelease) ||
        !wait_for_background_alpha(
            display,
            window,
            output_descriptor,
            255)) {
        goto cleanup;
    }
    stage = "exit preferred sample";
    if (!send_key(display, window, quit_key) ||
        !wait_for_process_exit(output_descriptor) ||
        waitpid(process, &status, 0) != process || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        goto cleanup;
    }
    close(output_descriptor);
    output_descriptor = -1;
    process = -1;
    XSync(display, False);

    stage = "set invalid opacity";
    if (setenv("BOXRIG_BACKGROUND_OPACITY", "50x", 1) == -1) {
        goto cleanup;
    }
    process = start_sample(
        sample_path,
        helper_arguments,
        display_name,
        "xterm-256color",
        false,
        &output_descriptor);
    stage = "reject invalid opacity";
    if (unsetenv("BOXRIG_BACKGROUND_OPACITY") == -1 || process == -1 ||
        !wait_for_sample_status_and_output(
            process,
            output_descriptor,
            &status,
            output,
            sizeof(output)) ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 2 ||
        strstr(
            output,
            "BOXRIG_BACKGROUND_OPACITY must be an integer from 0 through 100") ==
            NULL) {
        goto cleanup;
    }
    process = -1;
    passed = true;

cleanup:
    (void)unsetenv("BOXRIG_BACKGROUND_OPACITY");
    if (output_descriptor != -1) {
        close(output_descriptor);
    }
    if (process > 0) {
        kill(process, SIGTERM);
        waitpid(process, NULL, 0);
    }
    if (compositor_owner != None) {
        XDestroyWindow(display, compositor_owner);
    }
    XSync(display, False);
    if (!passed) {
        fprintf(
            stderr,
            "background opacity failed at %s (depth=%d status=%d output=%s)\n",
            stage,
            window == None ? 0 : attributes.depth,
            status,
            output);
    }
    return passed;
}

static bool run_sample_command(
    Display *display,
    const char *sample_path,
    char *const command_arguments[],
    const char *display_name,
    const char *term,
    const test_key *keys,
    size_t key_count,
    bool require_rendered_content)
{
    pid_t sample_process = -1;
    int sample_output = -1;
    Window window = None;
    int sample_status = 0;
    size_t index;
    bool passed = false;

    sample_process = start_sample(
        sample_path,
        command_arguments,
        display_name,
        term,
        false,
        &sample_output);
    if (sample_process == -1) {
        return false;
    }
    if (!wait_for_window(display, sample_output, &window) ||
        !window_has_no_children(display, window) ||
        (require_rendered_content &&
         !window_has_rendered_content(display, window, sample_output))) {
        goto cleanup;
    }
    for (index = 0; index < key_count; index++) {
        if (!send_key(display, window, keys[index])) {
            goto cleanup;
        }
    }
    if (!wait_for_process_exit(sample_output) ||
        waitpid(sample_process, &sample_status, 0) != sample_process) {
        goto cleanup;
    }
    sample_process = -1;
    passed = WIFEXITED(sample_status) && WEXITSTATUS(sample_status) == 0;

cleanup:
    if (sample_output != -1) {
        close(sample_output);
    }
    if (sample_process > 0) {
        kill(sample_process, SIGTERM);
        waitpid(sample_process, NULL, 0);
    }
    XSync(display, False);
    if (!passed) {
        fprintf(
            stderr,
            "X11 emitter case failed: %s\n",
            command_arguments[2]);
    }
    return passed;
}

static bool wait_for_emitter_title(
    Display *display,
    Window window,
    int process_output)
{
    static const char expected[] = "boxrig emitter window";
    size_t attempt;

    for (attempt = 0; attempt < 100; attempt++) {
        struct pollfd descriptors[2];
        char *title = NULL;
        bool matches = false;
        int result;

        XSync(display, False);
        if (XFetchName(display, window, &title) != 0 && title != NULL) {
            matches = strcmp(title, expected) == 0;
            XFree(title);
        }
        if (matches) {
            return true;
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == DestroyNotify &&
                event.xdestroywindow.window == window) {
                return false;
            }
        }
        descriptors[0].fd = ConnectionNumber(display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = process_output;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2, 50);
        if (result < 0 && errno != EINTR) {
            return false;
        }
        if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
            return false;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            char buffer[256];

            if (read(process_output, buffer, sizeof(buffer)) <= 0) {
                return false;
            }
        }
    }
    return false;
}

static bool wait_for_synchronized_frame_title(
    Display *display,
    Window window,
    int process_output,
    const char *expected)
{
    size_t attempt;

    for (attempt = 0; attempt < 100; attempt++) {
        struct pollfd descriptors[2];
        char *title = NULL;
        bool matches = false;
        int result;

        XSync(display, False);
        if (XFetchName(display, window, &title) != 0 && title != NULL) {
            matches = strcmp(title, expected) == 0;
            XFree(title);
        }
        if (matches) {
            return true;
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == DestroyNotify &&
                event.xdestroywindow.window == window) {
                return false;
            }
        }
        descriptors[0].fd = ConnectionNumber(display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = process_output;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2, 50);
        if (result < 0 && errno != EINTR) {
            return false;
        }
        if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
            return false;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            char buffer[256];

            if (read(process_output, buffer, sizeof(buffer)) <= 0) {
                return false;
            }
        }
    }
    return false;
}

static bool query_cell_geometry(
    Display *display,
    Window window,
    int process_output,
    const XWindowAttributes *attributes,
    unsigned int scrollbar_width)
{
    const test_key query = {XK_p, 0};
    char expected[64];
    unsigned int content_width;
    int length;

    if (attributes->width <= (int)scrollbar_width ||
        attributes->height <= 0) {
        return false;
    }
    content_width = (unsigned int)attributes->width - scrollbar_width;
    if (content_width % 80 != 0 ||
        (unsigned int)attributes->height % 24 != 0) {
        return false;
    }
    length = snprintf(
        expected,
        sizeof(expected),
        "cell-%u-%u",
        (unsigned int)attributes->height / 24,
        content_width / 80);
    return length > 0 && (size_t)length < sizeof(expected) &&
           XStoreName(display, window, "awaiting-cell-geometry") != 0 &&
           send_key(display, window, query) &&
           wait_for_synchronized_frame_title(
               display,
               window,
               process_output,
               expected);
}

static bool wait_for_emitter_clipboard(
    Display *display,
    Window window,
    int process_output)
{
    Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
    Atom utf8_string = XInternAtom(display, "UTF8_STRING", False);
    Atom property = XInternAtom(
        display,
        "_BOXRIG_EMITTER_CLIPBOARD",
        False);
    Window requestor = XCreateSimpleWindow(
        display,
        DefaultRootWindow(display),
        0,
        0,
        1,
        1,
        0,
        0,
        0);
    size_t attempt;
    bool passed = false;

    if (clipboard == None || utf8_string == None || property == None ||
        requestor == None) {
        goto cleanup;
    }
    for (attempt = 0; attempt < 100; attempt++) {
        struct pollfd descriptors[2];
        int result;

        XSync(display, False);
        if (XGetSelectionOwner(display, clipboard) != None) {
            passed = wait_for_selection_text(
                display,
                requestor,
                clipboard,
                utf8_string,
                property,
                "boxrig clipboard fixture");
            goto cleanup;
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == DestroyNotify &&
                event.xdestroywindow.window == window) {
                goto cleanup;
            }
        }
        descriptors[0].fd = ConnectionNumber(display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = process_output;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2, 50);
        if (result < 0 && errno != EINTR) {
            goto cleanup;
        }
        if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
            goto cleanup;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            char buffer[256];

            if (read(process_output, buffer, sizeof(buffer)) <= 0) {
                goto cleanup;
            }
        }
    }

cleanup:
    if (requestor != None) {
        XDestroyWindow(display, requestor);
    }
    XSync(display, False);
    return passed;
}

static bool run_held_emitter_host_case(
    Display *display,
    const char *sample_path,
    const char *emitter_path,
    const char *case_name,
    const char *display_name,
    bool clipboard_case)
{
    char *command_arguments[6];
    pid_t sample_process = -1;
    int sample_output = -1;
    Window window = None;
    int sample_status = 0;
    bool passed = false;

    command_arguments[0] = (char *)emitter_path;
    command_arguments[1] = "-t";
    command_arguments[2] = (char *)case_name;
    command_arguments[3] = NULL;
    sample_process = start_sample(
        sample_path,
        command_arguments,
        display_name,
        "xterm-256color",
        false,
        &sample_output);
    if (sample_process == -1 ||
        !wait_for_window(display, sample_output, &window) ||
        (clipboard_case
             ? !wait_for_emitter_clipboard(display, window, sample_output)
             : !wait_for_emitter_title(display, window, sample_output)) ||
        !send_key(display, window, (test_key){XK_Return, 0}) ||
        !wait_for_process_exit(sample_output) ||
        waitpid(sample_process, &sample_status, 0) != sample_process) {
        goto cleanup;
    }
    sample_process = -1;
    passed = WIFEXITED(sample_status) && WEXITSTATUS(sample_status) == 0;

cleanup:
    if (sample_output != -1) {
        close(sample_output);
    }
    if (sample_process > 0) {
        kill(sample_process, SIGTERM);
        waitpid(sample_process, NULL, 0);
    }
    XSync(display, False);
    if (!passed) {
        fprintf(stderr, "X11 held emitter case failed: %s\n", case_name);
    }
    return passed;
}

static bool run_emitter_graphics_case(
    Display *display,
    const char *sample_path,
    const char *emitter_path,
    const char *display_name)
{
    char *command_arguments[4];
    pid_t sample_process = -1;
    int sample_output = -1;
    Window window = None;
    int sample_status = 0;
    bool passed = false;

    command_arguments[0] = (char *)emitter_path;
    command_arguments[1] = "-t";
    command_arguments[2] = "kitty-image-static";
    command_arguments[3] = NULL;
    sample_process = start_sample(
        sample_path,
        command_arguments,
        display_name,
        "xterm-256color",
        false,
        &sample_output);
    if (sample_process == -1 ||
        !wait_for_window(display, sample_output, &window) ||
        !wait_for_graphics_fixture(display, window, sample_output) ||
        !send_key(display, window, (test_key){XK_Return, 0}) ||
        !wait_for_process_exit(sample_output) ||
        waitpid(sample_process, &sample_status, 0) != sample_process) {
        goto cleanup;
    }
    sample_process = -1;
    passed = WIFEXITED(sample_status) && WEXITSTATUS(sample_status) == 0;

cleanup:
    if (sample_output != -1) {
        close(sample_output);
    }
    if (sample_process > 0) {
        kill(sample_process, SIGTERM);
        waitpid(sample_process, NULL, 0);
    }
    XSync(display, False);
    if (!passed) {
        fprintf(stderr, "X11 emitter graphics fixture failed\n");
    }
    return passed;
}

static bool read_emitter_image_color(
    Display *display,
    Window window,
    size_t column,
    size_t row,
    size_t columns,
    size_t rows,
    size_t source_x,
    size_t source_y,
    size_t source_width,
    size_t source_height,
    uint8_t *red_out,
    uint8_t *green_out,
    uint8_t *blue_out)
{
    XWindowAttributes attributes;
    XImage *image = NULL;
    XColor actual;
    unsigned int content_width;
    unsigned int content_height;
    unsigned int cell_width;
    unsigned int cell_height;
    int x;
    int y;

    XSync(display, False);
    if (XGetWindowAttributes(display, window, &attributes) == 0 ||
        attributes.width <= (int)boxrig_x11_scrollbar_pixel_width() ||
        attributes.height <= 0) {
        return false;
    }
    content_width = (unsigned int)attributes.width -
                    boxrig_x11_scrollbar_pixel_width();
    content_height = (unsigned int)attributes.height;
    cell_width = content_width / 80;
    cell_height = content_height / 24;
    if (cell_width < 4 || cell_height < 2 ||
        cell_width * 80 != content_width || cell_height * 24 != content_height ||
        column >= 80 || row >= 24 || columns == 0 || rows == 0 ||
        source_width == 0 || source_height == 0 ||
        source_x >= source_width || source_y >= source_height ||
        columns > 80 - column || rows > 24 - row) {
        return false;
    }
    x = (int)(column * cell_width +
              ((uint64_t)source_x * 2 + 1) * columns * cell_width /
                  ((uint64_t)source_width * 2));
    y = (int)(row * cell_height +
              ((uint64_t)source_y * 2 + 1) * rows * cell_height /
                  ((uint64_t)source_height * 2));
    image = XGetImage(
        display,
        window,
        x,
        y,
        1,
        1,
        AllPlanes,
        ZPixmap);
    if (image == NULL) {
        return false;
    }
    memset(&actual, 0, sizeof(actual));
    actual.pixel = XGetPixel(image, 0, 0) &
                   (attributes.visual->red_mask |
                    attributes.visual->green_mask |
                    attributes.visual->blue_mask);
    XDestroyImage(image);
    if (XQueryColor(
            display,
            attributes.colormap,
            &actual) == 0) {
        return false;
    }
    *red_out = (uint8_t)(actual.red / 257u);
    *green_out = (uint8_t)(actual.green / 257u);
    *blue_out = (uint8_t)(actual.blue / 257u);
    return true;
}

static bool wait_for_input_response_color(
    Display *display,
    Window window,
    int process_output,
    bool red_dominant)
{
    size_t attempt;

    for (attempt = 0; attempt < 100; attempt++) {
        uint8_t red;
        uint8_t green;
        uint8_t blue;

        if (read_emitter_image_color(
                display,
                window,
                79,
                23,
                1,
                1,
                0,
                0,
                1,
                1,
                &red,
                &green,
                &blue) &&
            (red_dominant ? red > green && red > blue
                          : green > red && green > blue)) {
            return true;
        }
        if (!wait_for_emitter_iteration(
                display,
                window,
                process_output,
                50)) {
            return false;
        }
    }
    return false;
}

static bool run_input_response_fixture(
    Display *display,
    const char *sample_path,
    const char *helper_path,
    const char *display_name)
{
    char *arguments[] = {
        (char *)helper_path,
        "--input-response",
        NULL};
    const test_key down = {XK_Down, 0};
    const test_key up = {XK_Up, 0};
    const test_key quit = {XK_q, 0};
    pid_t sample_process = -1;
    int sample_output = -1;
    Window window = None;
    int sample_status = 0;
    bool passed = false;

    sample_process = start_sample(
        sample_path,
        arguments,
        display_name,
        "xterm-256color",
        false,
        &sample_output);
    if (sample_process == -1 ||
        !wait_for_window(display, sample_output, &window) ||
        !wait_for_synchronized_frame_title(
            display,
            window,
            sample_output,
            "input-response-ready") ||
        !send_key(display, window, down) ||
        !wait_for_synchronized_frame_title(
            display,
            window,
            sample_output,
            "input-response-000001-D") ||
        !wait_for_input_response_color(
            display,
            window,
            sample_output,
            true) ||
        !send_key(display, window, up) ||
        !wait_for_synchronized_frame_title(
            display,
            window,
            sample_output,
            "input-response-000002-U") ||
        !wait_for_input_response_color(
            display,
            window,
            sample_output,
            false) ||
        !send_key(display, window, quit) ||
        !wait_for_process_exit(sample_output) ||
        waitpid(sample_process, &sample_status, 0) != sample_process) {
        goto cleanup;
    }
    sample_process = -1;
    passed = WIFEXITED(sample_status) && WEXITSTATUS(sample_status) == 0;

cleanup:
    if (sample_output != -1) {
        close(sample_output);
    }
    if (sample_process > 0) {
        kill(sample_process, SIGTERM);
        waitpid(sample_process, NULL, 0);
    }
    XSync(display, False);
    return passed;
}

static bool wait_for_emitter_iteration(
    Display *display,
    Window window,
    int process_output,
    int timeout_milliseconds)
{
    struct pollfd descriptors[2];
    int result;

    while (XPending(display) > 0) {
        XEvent event;

        XNextEvent(display, &event);
        if (event.type == DestroyNotify &&
            event.xdestroywindow.window == window) {
            return false;
        }
    }
    descriptors[0].fd = ConnectionNumber(display);
    descriptors[0].events = POLLIN;
    descriptors[0].revents = 0;
    descriptors[1].fd = process_output;
    descriptors[1].events = POLLIN;
    descriptors[1].revents = 0;
    result = poll(descriptors, 2, timeout_milliseconds);
    if (result < 0) {
        return errno == EINTR;
    }
    if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
        return false;
    }
    if ((descriptors[1].revents & POLLIN) != 0) {
        char buffer[256];

        if (read(process_output, buffer, sizeof(buffer)) <= 0) {
            return false;
        }
    }
    return true;
}

static bool wait_for_emitter_stability(
    Display *display,
    Window window,
    int process_output)
{
    uint64_t previous = 0;
    size_t stable_count = 0;
    size_t attempt;

    for (attempt = 0; attempt < 150; attempt++) {
        XWindowAttributes attributes;
        uint64_t current;

        if (XGetWindowAttributes(display, window, &attributes) != 0 &&
            attributes.width > 0 && attributes.height > 0 &&
            read_window_hash(
                display,
                window,
                (unsigned int)attributes.width,
                (unsigned int)attributes.height,
                &current)) {
            if (stable_count != 0 && current == previous) {
                stable_count++;
                if (stable_count == 3) {
                    return true;
                }
            } else {
                stable_count = 1;
                previous = current;
            }
        }
        if (!wait_for_emitter_iteration(
                display,
                window,
                process_output,
                20)) {
            return false;
        }
    }
    return false;
}

typedef struct fixed_animation_pixel {
    size_t source_x;
    size_t source_y;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    /* The direct frame split is filtered when the 16-pixel image is scaled. */
    bool red_dominant;
} fixed_animation_pixel;

typedef struct fixed_animation_fixture {
    const char *case_name;
    fixed_animation_pixel pixels[2];
} fixed_animation_fixture;

static bool run_fixed_emitter_animation_case(
    Display *display,
    const char *sample_path,
    const char *emitter_path,
    const char *display_name,
    const fixed_animation_fixture *fixture)
{
    char *command_arguments[] = {
        (char *)emitter_path, "-t", (char *)fixture->case_name, NULL,
    };
    pid_t sample_process = -1;
    int sample_output = -1;
    Window window = None;
    int sample_status = 0;
    bool pixels_match = false;
    bool passed = false;
    size_t attempt;

    sample_process = start_sample(
        sample_path,
        command_arguments,
        display_name,
        "xterm-256color",
        false,
        &sample_output);
    if (sample_process == -1 ||
        !wait_for_window(display, sample_output, &window)) {
        goto cleanup;
    }
    for (attempt = 0; attempt < 100 && !pixels_match; attempt++) {
        size_t index;

        pixels_match = true;
        for (index = 0;
             index < sizeof(fixture->pixels) / sizeof(fixture->pixels[0]);
             index++) {
            uint8_t red;
            uint8_t green;
            uint8_t blue;

            if (!read_emitter_image_color(
                    display,
                    window,
                    0,
                    2,
                    4,
                    2,
                    fixture->pixels[index].source_x,
                    fixture->pixels[index].source_y,
                    16,
                    16,
                    &red,
                    &green,
                    &blue) ||
                (fixture->pixels[index].red_dominant
                     ? (int)red - (int)green < 32 ||
                           (int)red - (int)blue < 32
                     : !color_channel_near(
                           red,
                           fixture->pixels[index].red) ||
                           !color_channel_near(
                               green,
                               fixture->pixels[index].green) ||
                           !color_channel_near(
                               blue,
                               fixture->pixels[index].blue))) {
                pixels_match = false;
                break;
            }
        }
        if (!pixels_match &&
            !wait_for_emitter_iteration(
                display,
                window,
                sample_output,
                20)) {
            goto cleanup;
        }
    }
    if (!pixels_match ||
        !send_key(display, window, (test_key){XK_Return, 0}) ||
        !wait_for_process_exit(sample_output) ||
        waitpid(sample_process, &sample_status, 0) != sample_process) {
        goto cleanup;
    }
    sample_process = -1;
    passed = WIFEXITED(sample_status) && WEXITSTATUS(sample_status) == 0;

cleanup:
    if (sample_output != -1) {
        close(sample_output);
    }
    if (sample_process > 0) {
        kill(sample_process, SIGTERM);
        waitpid(sample_process, NULL, 0);
    }
    XSync(display, False);
    if (!passed) {
        fprintf(
            stderr,
            "X11 fixed emitter animation failed (%s pixels=%d)\n",
            fixture->case_name,
            pixels_match);
    }
    return passed;
}

static bool run_emitter_animation_case(
    Display *display,
    const char *sample_path,
    const char *emitter_path,
    const char *display_name)
{
    static const fixed_animation_fixture fixed[] = {
        {
            "kitty-animation-direct",
            {
                {7, 12, 224, 48, 48, true},
                {10, 10, 48, 200, 88, false},
            },
        },
        {
            "kitty-animation-client",
            {
                {4, 1, 255, 220, 64, false},
                {10, 10, 48, 96, 224, false},
            },
        },
        {
            "kitty-animation-compose",
            {
                {0, 0, 0, 0, 0, false},
                {10, 10, 48, 96, 224, false},
            },
        },
    };
    char *command_arguments[] = {
        (char *)emitter_path, "-t", "kitty-animation-loop", NULL,
    };
    pid_t sample_process = -1;
    int sample_output = -1;
    Window window = None;
    int sample_status = 0;
    bool found_red = false;
    bool found_green = false;
    bool passed = false;
    size_t attempt;
    size_t fixed_index;

    for (fixed_index = 0;
         fixed_index < sizeof(fixed) / sizeof(fixed[0]);
         fixed_index++) {
        if (!run_fixed_emitter_animation_case(
                display,
                sample_path,
                emitter_path,
                display_name,
                &fixed[fixed_index])) {
            return false;
        }
    }

    sample_process = start_sample(
        sample_path,
        command_arguments,
        display_name,
        "xterm-256color",
        false,
        &sample_output);
    if (sample_process == -1 ||
        !wait_for_window(display, sample_output, &window)) {
        goto cleanup;
    }
    for (attempt = 0; attempt < 150 && !(found_red && found_green); attempt++) {
        uint8_t red;
        uint8_t green;
        uint8_t blue;

        if (read_emitter_image_color(
                display,
                window,
                0,
                2,
                4,
                2,
                10,
                10,
                16,
                16,
                &red,
                &green,
                &blue)) {
            found_red = found_red ||
                        (color_channel_near(red, 224) &&
                         color_channel_near(green, 48) &&
                         color_channel_near(blue, 48));
            found_green = found_green ||
                          (color_channel_near(red, 48) &&
                           color_channel_near(green, 200) &&
                           color_channel_near(blue, 88));
        }
        if (!(found_red && found_green) &&
            !wait_for_emitter_iteration(
                display,
                window,
                sample_output,
                20)) {
            goto cleanup;
        }
    }
    if (!found_red || !found_green ||
        !send_key(display, window, (test_key){XK_Return, 0}) ||
        !wait_for_process_exit(sample_output) ||
        waitpid(sample_process, &sample_status, 0) != sample_process) {
        goto cleanup;
    }
    sample_process = -1;
    passed = WIFEXITED(sample_status) && WEXITSTATUS(sample_status) == 0;

cleanup:
    if (sample_output != -1) {
        close(sample_output);
    }
    if (sample_process > 0) {
        kill(sample_process, SIGTERM);
        waitpid(sample_process, NULL, 0);
    }
    XSync(display, False);
    if (!passed) {
        fprintf(
            stderr,
            "X11 emitter animation failed (red=%d green=%d)\n",
            found_red,
            found_green);
    }
    return passed;
}

static pid_t start_external_emitter_sample(
    const char *sample_path,
    char *const command_arguments[],
    const char *display_name,
    bool allow_external,
    int *output_descriptor)
{
    pid_t process;

    if ((allow_external
             ? setenv("BOXRIG_ALLOW_GRAPHICS_EXTERNAL", "1", 1)
             : unsetenv("BOXRIG_ALLOW_GRAPHICS_EXTERNAL")) == -1) {
        return -1;
    }
    process = start_sample(
        sample_path,
        command_arguments,
        display_name,
        "xterm-256color",
        false,
        output_descriptor);
    if (unsetenv("BOXRIG_ALLOW_GRAPHICS_EXTERNAL") == -1) {
        if (process > 0) {
            kill(process, SIGTERM);
            waitpid(process, NULL, 0);
        }
        return -1;
    }
    return process;
}

static bool external_emitter_pixels_match(
    Display *display,
    Window window)
{
    static const uint8_t expected[][3] = {
        {224, 48, 48},
        {48, 200, 88},
        {48, 96, 224},
    };
    size_t index;

    for (index = 0; index < sizeof(expected) / sizeof(expected[0]); index++) {
        uint8_t red;
        uint8_t green;
        uint8_t blue;

        if (!read_emitter_image_color(
                display,
                window,
                index * 6,
                3,
                4,
                2,
                10,
                10,
                16,
                16,
                &red,
                &green,
                &blue) ||
            !color_channel_near(red, expected[index][0]) ||
            !color_channel_near(green, expected[index][1]) ||
            !color_channel_near(blue, expected[index][2])) {
            return false;
        }
    }
    return true;
}

static void report_external_emitter_pixels(Display *display, Window window)
{
    static const uint8_t expected[][3] = {
        {224, 48, 48},
        {48, 200, 88},
        {48, 96, 224},
    };
    size_t expected_index;

    for (expected_index = 0;
         expected_index < sizeof(expected) / sizeof(expected[0]);
         expected_index++) {
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;
        size_t row;
        size_t column;

        (void)read_emitter_image_color(
            display,
            window,
            expected_index * 6,
            3,
            4,
            2,
            10,
            10,
            16,
            16,
            &red,
            &green,
            &blue);
        fprintf(
            stderr,
            "external image %lu,3 expected=%u,%u,%u actual=%u,%u,%u; "
            "matches:",
            (unsigned long)(expected_index * 6),
            expected[expected_index][0],
            expected[expected_index][1],
            expected[expected_index][2],
            red,
            green,
            blue);
        for (row = 0; row < 24; row++) {
            for (column = 0; column < 10; column++) {
                if (read_emitter_image_color(
                        display,
                        window,
                        column,
                        row,
                        4,
                        2,
                        10,
                        10,
                        16,
                        16,
                        &red,
                        &green,
                        &blue) &&
                    color_channel_near(red, expected[expected_index][0]) &&
                    color_channel_near(green, expected[expected_index][1]) &&
                    color_channel_near(blue, expected[expected_index][2])) {
                    fprintf(
                        stderr,
                        " %lu,%lu",
                        (unsigned long)column,
                        (unsigned long)row);
                }
            }
        }
        fputc('\n', stderr);
    }
}

static bool run_one_external_emitter_case(
    Display *display,
    const char *sample_path,
    const char *emitter_path,
    const char *display_name,
    bool allow_external)
{
    char *command_arguments[] = {
        (char *)emitter_path, "-t", "kitty-image-external", NULL,
    };
    pid_t sample_process = -1;
    int sample_output = -1;
    Window window = None;
    int sample_status = 0;
    bool pixels_match = false;
    bool passed = false;
    size_t attempt;

    sample_process = start_external_emitter_sample(
        sample_path,
        command_arguments,
        display_name,
        allow_external,
        &sample_output);
    if (sample_process == -1 ||
        !wait_for_window(display, sample_output, &window) ||
        !wait_for_emitter_stability(display, window, sample_output)) {
        goto cleanup;
    }
    for (attempt = 0; attempt < 100; attempt++) {
        pixels_match = external_emitter_pixels_match(display, window);
        if (pixels_match || !allow_external) {
            break;
        }
        if (!wait_for_emitter_iteration(
                display,
                window,
                sample_output,
                20)) {
            goto cleanup;
        }
    }
    if (pixels_match != allow_external) {
        if (allow_external) {
            report_external_emitter_pixels(display, window);
        }
        goto cleanup;
    }
    if (!send_key(display, window, (test_key){XK_Return, 0}) ||
        !wait_for_process_exit(sample_output) ||
        waitpid(sample_process, &sample_status, 0) != sample_process) {
        goto cleanup;
    }
    sample_process = -1;
    passed = WIFEXITED(sample_status) && WEXITSTATUS(sample_status) == 0;

cleanup:
    if (sample_output != -1) {
        close(sample_output);
    }
    if (sample_process > 0) {
        kill(sample_process, SIGTERM);
        waitpid(sample_process, NULL, 0);
    }
    XSync(display, False);
    if (!passed) {
        fprintf(
            stderr,
            "X11 external emitter policy failed (allow=%d pixels=%d)\n",
            allow_external,
            pixels_match);
    }
    return passed;
}

static bool run_emitter_external_policy(
    Display *display,
    const char *sample_path,
    const char *emitter_path,
    const char *display_name)
{
    return run_one_external_emitter_case(
               display,
               sample_path,
               emitter_path,
               display_name,
               false) &&
           run_one_external_emitter_case(
               display,
               sample_path,
               emitter_path,
               display_name,
               true);
}

static bool run_emitter_notification_policy(
    Display *display,
    const char *sample_path,
    const char *emitter_path,
    const char *display_name,
    int service_output)
{
    char *command_arguments[6];
    size_t notification_count = 0;
    size_t run_index;

    command_arguments[0] = (char *)emitter_path;
    command_arguments[1] = "-t";
    command_arguments[2] = "desktop-notification";
    command_arguments[3] = NULL;
    for (run_index = 0; run_index < 2; run_index++) {
        bool allow_notifications = run_index == 1;
        pid_t sample_process = -1;
        int sample_output = -1;
        Window window = None;
        int sample_status = 0;
        struct pollfd service_descriptor;
        bool run_passed = false;

        sample_process = start_sample(
            sample_path,
            command_arguments,
            display_name,
            "xterm-256color",
            allow_notifications,
            &sample_output);
        if (sample_process == -1 ||
            !wait_for_window(display, sample_output, &window) ||
            !window_has_rendered_content(display, window, sample_output) ||
            !send_key(display, window, (test_key){XK_Return, 0}) ||
            !wait_for_process_exit(sample_output) ||
            waitpid(sample_process, &sample_status, 0) != sample_process) {
            goto run_cleanup;
        }
        sample_process = -1;
        if (!WIFEXITED(sample_status) || WEXITSTATUS(sample_status) != 0) {
            goto run_cleanup;
        }
        service_descriptor.fd = service_output;
        service_descriptor.events = POLLIN;
        service_descriptor.revents = 0;
        if (allow_notifications) {
            if (poll(&service_descriptor, 1, 5000) != 1 ||
                (service_descriptor.revents & POLLIN) == 0 ||
                !read_notification_events(
                    service_output,
                    &notification_count)) {
                goto run_cleanup;
            }
        } else if (poll(&service_descriptor, 1, 0) != 0) {
            goto run_cleanup;
        }
        run_passed = true;

run_cleanup:
        if (sample_output != -1) {
            close(sample_output);
        }
        if (sample_process > 0) {
            kill(sample_process, SIGTERM);
            waitpid(sample_process, NULL, 0);
        }
        XSync(display, False);
        if (!run_passed) {
            return false;
        }
    }
    return notification_count == 1;
}

static bool run_emitter_sample_cases(
    Display *display,
    const char *sample_path,
    const char *emitter_path,
    const char *display_name,
    int service_output)
{
    char *text_arguments[] = {
        (char *)emitter_path, "-t", "text-cjk", NULL,
    };
    char *query_arguments[] = {
        (char *)emitter_path, "-t", "query-device-status", "--no-hold", NULL,
    };
    char *graphics_query_arguments[] = {
        (char *)emitter_path,
        "-t",
        "query-kitty-graphics",
        "--no-hold",
        NULL,
    };
    char *input_arguments[] = {
        (char *)emitter_path, "-t", "input-application-cursor", "--no-hold", NULL,
    };
    static const test_key enter[] = {{XK_Return, 0}};
    static const test_key up[] = {{XK_Up, 0}};

    return run_sample_command(
               display,
               sample_path,
               text_arguments,
               display_name,
               "xterm-256color",
               enter,
               sizeof(enter) / sizeof(enter[0]),
               true) &&
           run_sample_command(
               display,
               sample_path,
               query_arguments,
               display_name,
               "xterm-256color",
               NULL,
               0,
               false) &&
           run_sample_command(
               display,
               sample_path,
               graphics_query_arguments,
               display_name,
               "xterm-256color",
               NULL,
               0,
               false) &&
           run_emitter_graphics_case(
               display,
               sample_path,
               emitter_path,
               display_name) &&
           run_emitter_animation_case(
               display,
               sample_path,
               emitter_path,
               display_name) &&
           run_emitter_external_policy(
               display,
               sample_path,
               emitter_path,
               display_name) &&
           run_sample_command(
               display,
               sample_path,
               input_arguments,
               display_name,
               "xterm-256color",
               up,
               sizeof(up) / sizeof(up[0]),
               true) &&
           run_held_emitter_host_case(
               display,
               sample_path,
               emitter_path,
               "osc-titles",
               display_name,
               false) &&
           run_held_emitter_host_case(
               display,
               sample_path,
               emitter_path,
               "clipboard-write",
               display_name,
               true) &&
           run_emitter_notification_policy(
               display,
               sample_path,
               emitter_path,
               display_name,
               service_output);
}

static bool run_host_input_policy(
    Display *display,
    const char *sample_path,
    const char *helper_path,
    const char *display_name)
{
    char *helper_arguments[2] = {(char *)helper_path, NULL};
    const test_key copy = {XK_c, ControlMask | ShiftMask};
    const test_key paste = {XK_v, ControlMask | ShiftMask};
    const test_key fixture = {XK_x, 0};
    const test_key mouse_fixture = {XK_r, 0};
    const test_key quit = {XK_q, 0};
    static const test_key control_input[] = {
        {XK_2, ControlMask},
        {XK_3, ControlMask},
        {XK_slash, ControlMask},
        {XK_question, ControlMask | ShiftMask},
        {XK_c, ControlMask},
        {XK_z, ControlMask},
    };
    pid_t sample_process = -1;
    int sample_output = -1;
    Window sample_window = None;
    Window selection_window = None;
    XWindowAttributes attributes;
    Atom clipboard;
    Atom targets;
    Atom utf8_string;
    Atom property;
    unsigned int scrollbar_width = boxrig_x11_scrollbar_pixel_width();
    unsigned int content_width;
    int cell_width;
    int cell_height;
    int sample_status = 0;
    size_t index;
    const char *stage = "start sample";
    bool passed = false;

    sample_process = start_sample(
        sample_path,
        helper_arguments,
        display_name,
        "xterm-256color",
        false,
        &sample_output);
    if (sample_process == -1 ||
        !wait_for_window(display, sample_output, &sample_window) ||
        XGetWindowAttributes(display, sample_window, &attributes) == 0 ||
        attributes.width <= (int)scrollbar_width ||
        attributes.height <= 0) {
        goto cleanup;
    }
    content_width = (unsigned int)attributes.width - scrollbar_width;
    if (content_width % 80 != 0 ||
        (unsigned int)attributes.height % 24 != 0) {
        goto cleanup;
    }
    cell_width = (int)(content_width / 80);
    cell_height = attributes.height / 24;
    selection_window = XCreateSimpleWindow(
        display,
        DefaultRootWindow(display),
        0,
        0,
        1,
        1,
        0,
        0,
        0);
    clipboard = XInternAtom(display, "CLIPBOARD", False);
    targets = XInternAtom(display, "TARGETS", False);
    utf8_string = XInternAtom(display, "UTF8_STRING", False);
    property = XInternAtom(display, "_BOXRIG_SAMPLE_SELECTION", False);
    if (selection_window == None || clipboard == None || targets == None ||
        utf8_string == None || property == None) {
        goto cleanup;
    }

    stage = "render selection fixture";
    if (!send_key(display, sample_window, fixture) ||
        !wait_for_synchronized_frame_title(
            display,
            sample_window,
            sample_output,
            "host-input-ready") ||
        !window_has_rendered_content(
            display,
            sample_window,
            sample_output)) {
        goto cleanup;
    }
    stage = "select copy fixture";
    if (!send_terminal_button(
            display,
            sample_window,
            cell_width / 2,
            cell_height / 2,
            Button1,
            ButtonPress,
            0,
            100) ||
        !send_terminal_motion(
            display,
            sample_window,
            10 * cell_width + cell_width / 2,
            cell_height / 2,
            Button1Mask,
            110) ||
        !send_terminal_button(
            display,
            sample_window,
            10 * cell_width + cell_width / 2,
            cell_height / 2,
            Button1,
            ButtonRelease,
            Button1Mask,
            120)) {
        goto cleanup;
    }
    stage = "acquire primary selection";
    if (!wait_for_selection_owner(
            display,
            XA_PRIMARY,
            sample_window,
            sample_output)) {
        goto cleanup;
    }
    stage = "transfer primary selection";
    if (!wait_for_selection_text(
            display,
            selection_window,
            XA_PRIMARY,
            utf8_string,
            property,
            "copy-target")) {
        goto cleanup;
    }

    stage = "copy shortcut";
    if (!send_key_transition(
            display,
            sample_window,
            copy,
            KeyPress) ||
        !send_key_transition(
            display,
            sample_window,
            copy,
            KeyRelease) ||
        !wait_for_selection_owner(
            display,
            clipboard,
            sample_window,
            sample_output) ||
        !wait_for_selection_text(
            display,
            selection_window,
            clipboard,
            utf8_string,
            property,
            "copy-target")) {
        goto cleanup;
    }

    stage = "double click word selection";
    if (!send_terminal_button(
            display,
            sample_window,
            cell_width / 2,
            cell_height / 2,
            Button1,
            ButtonPress,
            0,
            200) ||
        !send_terminal_button(
            display,
            sample_window,
            cell_width / 2,
            cell_height / 2,
            Button1,
            ButtonRelease,
            Button1Mask,
            210) ||
        !wait_for_selection_text(
            display,
            selection_window,
            XA_PRIMARY,
            utf8_string,
            property,
            "copy")) {
        goto cleanup;
    }
    stage = "triple click logical line selection";
    if (!send_terminal_button(
            display,
            sample_window,
            cell_width / 2,
            cell_height / 2,
            Button1,
            ButtonPress,
            0,
            250) ||
        !send_terminal_button(
            display,
            sample_window,
            cell_width / 2,
            cell_height / 2,
            Button1,
            ButtonRelease,
            Button1Mask,
            260) ||
        !wait_for_selection_text(
            display,
            selection_window,
            XA_PRIMARY,
            utf8_string,
            property,
            "copy-target\n")) {
        goto cleanup;
    }

    stage = "clipboard paste shortcut";
    XSetSelectionOwner(display, clipboard, selection_window, CurrentTime);
    XSync(display, False);
    if (XGetSelectionOwner(display, clipboard) != selection_window ||
        !send_key_transition(
            display,
            sample_window,
            paste,
            KeyPress) ||
        !send_key_transition(
            display,
            sample_window,
            paste,
            KeyRelease) ||
        !wait_for_title_serving_selection(
            display,
            sample_window,
            sample_output,
            targets,
            utf8_string,
            "P",
            "clipboard-paste-received")) {
        goto cleanup;
    }

    stage = "primary middle paste";
    XSetSelectionOwner(display, XA_PRIMARY, selection_window, CurrentTime);
    XSync(display, False);
    if (XGetSelectionOwner(display, XA_PRIMARY) != selection_window ||
        !send_terminal_button(
            display,
            sample_window,
            cell_width / 2,
            cell_height / 2,
            Button2,
            ButtonPress,
            0,
            300) ||
        !send_terminal_button(
            display,
            sample_window,
            cell_width / 2,
            cell_height / 2,
            Button2,
            ButtonRelease,
            Button2Mask,
            310) ||
        !wait_for_title_serving_selection(
            display,
            sample_window,
            sample_output,
            targets,
            utf8_string,
            "M",
            "primary-paste-received")) {
        goto cleanup;
    }

    stage = "terminal mouse reporting";
    if (!send_key(display, sample_window, mouse_fixture) ||
        !wait_for_synchronized_frame_title(
            display,
            sample_window,
            sample_output,
            "mouse-reporting-ready") ||
        !send_terminal_button(
            display,
            sample_window,
            20 * cell_width + cell_width / 2,
            cell_height + cell_height / 2,
            Button1,
            ButtonPress,
            0,
            500) ||
        !send_terminal_button(
            display,
            sample_window,
            20 * cell_width + cell_width / 2,
            cell_height + cell_height / 2,
            Button1,
            ButtonRelease,
            Button1Mask,
            510) ||
        !wait_for_synchronized_frame_title(
            display,
            sample_window,
            sample_output,
            "mouse-terminal-received")) {
        goto cleanup;
    }

    stage = "shift mouse override";
    if (!send_terminal_button(
            display,
            sample_window,
            cell_width / 2,
            cell_height / 2,
            Button1,
            ButtonPress,
            ShiftMask,
            600) ||
        !send_terminal_motion(
            display,
            sample_window,
            4 * cell_width + cell_width / 2,
            cell_height / 2,
            Button1Mask,
            610) ||
        !send_terminal_button(
            display,
            sample_window,
            4 * cell_width + cell_width / 2,
            cell_height / 2,
            Button1,
            ButtonRelease,
            Button1Mask,
            620) ||
        !wait_for_selection_owner(
            display,
            XA_PRIMARY,
            sample_window,
            sample_output) ||
        !wait_for_selection_text(
            display,
            selection_window,
            XA_PRIMARY,
            utf8_string,
            property,
            "mouse")) {
        goto cleanup;
    }

    stage = "control rectangular mouse override";
    if (!send_terminal_button(
            display,
            sample_window,
            cell_width / 4,
            cell_height / 2,
            Button1,
            ButtonPress,
            ControlMask,
            700) ||
        !send_terminal_motion(
            display,
            sample_window,
            4 * cell_width + (3 * cell_width) / 4,
            cell_height + cell_height / 2,
            Button1Mask | ShiftMask,
            710) ||
        !send_terminal_button(
            display,
            sample_window,
            4 * cell_width + (3 * cell_width) / 4,
            cell_height + cell_height / 2,
            Button1,
            ButtonRelease,
            Button1Mask,
            720) ||
        !wait_for_selection_text(
            display,
            selection_window,
            XA_PRIMARY,
            utf8_string,
            property,
            "mouse\nblock")) {
        goto cleanup;
    }

    stage = "prepare raw control input";
    if (!send_key_transition(
            display,
            sample_window,
            (test_key){XK_k, 0},
            KeyPress) ||
        !send_key_transition(
            display,
            sample_window,
            (test_key){XK_k, 0},
            KeyRelease) ||
        !wait_for_synchronized_frame_title(
            display,
            sample_window,
            sample_output,
            "control-input-ready")) {
        goto cleanup;
    }
    stage = "send raw control input";
    for (index = 0;
         index < sizeof(control_input) / sizeof(control_input[0]);
         index++) {
        if (!send_key_transition(
                display,
                sample_window,
                control_input[index],
                KeyPress) ||
            !send_key_transition(
                display,
                sample_window,
                control_input[index],
                KeyRelease)) {
            goto cleanup;
        }
    }
    if (!wait_for_synchronized_frame_title(
            display,
            sample_window,
            sample_output,
            "control-input-received")) {
        goto cleanup;
    }

    stage = "unrouted quit key";
    if (!send_key(display, sample_window, quit) ||
        !wait_for_process_exit(sample_output) ||
        waitpid(sample_process, &sample_status, 0) != sample_process ||
        !WIFEXITED(sample_status) || WEXITSTATUS(sample_status) != 0) {
        goto cleanup;
    }
    sample_process = -1;
    passed = true;

cleanup:
    if (sample_output != -1) {
        close(sample_output);
    }
    if (sample_process > 0) {
        kill(sample_process, SIGTERM);
        waitpid(sample_process, NULL, 0);
    }
    if (selection_window != None) {
        XDestroyWindow(display, selection_window);
    }
    XSync(display, False);
    if (!passed) {
        fprintf(stderr, "X11 host input policy failed at %s\n", stage);
    }
    return passed;
}

static bool run_scrollbar_viewport(
    Display *display,
    const char *sample_path,
    const char *helper_path,
    const char *display_name)
{
    const test_key history = {XK_h, 0};
    const test_key query_grid = {XK_g, 0};
    const test_key quit = {XK_q, 0};
    static const char expected_grid_title[] = "grid-24-80";
    char *helper_arguments[2];
    pid_t sample_process = -1;
    int sample_output = -1;
    Window window = None;
    XWindowAttributes window_attributes;
    unsigned int scrollbar_width = boxrig_x11_scrollbar_pixel_width();
    unsigned int content_width;
    uint64_t initial_hash;
    uint64_t live_hash;
    uint64_t top_hash;
    uint64_t page_hash;
    uint64_t restored_hash;
    int sample_status = 0;
    int middle_x;
    bool passed = false;

    helper_arguments[0] = (char *)helper_path;
    helper_arguments[1] = NULL;
    sample_process = start_sample(
        sample_path,
        helper_arguments,
        display_name,
        "xterm-256color",
        false,
        &sample_output);
    if (sample_process == -1 ||
        !wait_for_window(display, sample_output, &window) ||
        XGetWindowAttributes(display, window, &window_attributes) == 0 ||
        !window_has_no_children(display, window) ||
        window_attributes.width <= (int)scrollbar_width) {
        fprintf(stderr, "X11 composited scrollbar geometry was unavailable\n");
        goto cleanup;
    }
    content_width = (unsigned int)window_attributes.width - scrollbar_width;
    if (content_width % 80 != 0 ||
        (unsigned int)window_attributes.height % 24 != 0 ||
        !read_window_hash(
            display,
            window,
            content_width,
            (unsigned int)window_attributes.height,
            &initial_hash) ||
        !send_key(display, window, history) ||
        !wait_for_synchronized_frame_title(
            display,
            window,
            sample_output,
            "history-ready") ||
        !wait_for_window_hash(
            display,
            window,
            sample_output,
            content_width,
            (unsigned int)window_attributes.height,
            initial_hash,
            false,
            &live_hash)) {
        fprintf(stderr, "X11 scrollbar history fixture was not rendered\n");
        goto cleanup;
    }

    middle_x = window_attributes.width - (int)scrollbar_width / 2;
    if (!send_scrollbar_button(
            display,
            window,
            middle_x,
            window_attributes.height - 2,
            Button1,
            ButtonPress) ||
        !send_scrollbar_motion(display, window, middle_x, 1) ||
        !send_scrollbar_button(
            display,
            window,
            middle_x,
            1,
            Button1,
            ButtonRelease) ||
        !wait_for_window_hash(
            display,
            window,
            sample_output,
            content_width,
            (unsigned int)window_attributes.height,
            live_hash,
            false,
            &top_hash)) {
        fprintf(stderr, "X11 scrollbar thumb did not drag to oldest history\n");
        goto cleanup;
    }
    if (!send_scrollbar_button(
            display,
            window,
            middle_x,
            1,
            Button1,
            ButtonPress) ||
        !send_scrollbar_motion(
            display,
            window,
            middle_x,
            window_attributes.height - 2) ||
        !send_scrollbar_button(
            display,
            window,
            middle_x,
            window_attributes.height - 2,
            Button1,
            ButtonRelease) ||
        !wait_for_window_hash(
            display,
            window,
            sample_output,
            content_width,
            (unsigned int)window_attributes.height,
            live_hash,
            true,
            &restored_hash)) {
        fprintf(stderr, "X11 scrollbar thumb did not return to live output\n");
        goto cleanup;
    }

    if (!send_scrollbar_button(
            display,
            window,
            middle_x,
            0,
            Button1,
            ButtonPress) ||
        !send_scrollbar_button(
            display,
            window,
            middle_x,
            0,
            Button1,
            ButtonRelease) ||
        !wait_for_window_hash(
            display,
            window,
            sample_output,
            content_width,
            (unsigned int)window_attributes.height,
            live_hash,
            false,
            &page_hash) ||
        page_hash == top_hash) {
        fprintf(stderr, "X11 scrollbar track did not move one page upward\n");
        goto cleanup;
    }
    if (!send_scrollbar_button(
            display,
            window,
            middle_x,
            window_attributes.height - 1,
            Button1,
            ButtonPress) ||
        !send_scrollbar_button(
            display,
            window,
            middle_x,
            window_attributes.height - 1,
            Button1,
            ButtonRelease) ||
        !wait_for_window_hash(
            display,
            window,
            sample_output,
            content_width,
            (unsigned int)window_attributes.height,
            live_hash,
            true,
            &restored_hash) ||
        restored_hash != live_hash ||
        XStoreName(display, window, "awaiting-scrollbar-grid") == 0 ||
        !send_key(display, window, query_grid) ||
        !wait_for_synchronized_frame_title(
            display,
            window,
            sample_output,
            expected_grid_title) ||
        !send_key(display, window, quit) ||
        !wait_for_process_exit(sample_output) ||
        waitpid(sample_process, &sample_status, 0) != sample_process) {
        fprintf(stderr, "X11 scrollbar did not restore live output and grid\n");
        goto cleanup;
    }
    sample_process = -1;
    passed = WIFEXITED(sample_status) && WEXITSTATUS(sample_status) == 0;

cleanup:
    if (sample_output != -1) {
        close(sample_output);
    }
    if (sample_process > 0) {
        kill(sample_process, SIGTERM);
        waitpid(sample_process, NULL, 0);
    }
    XSync(display, False);
    return passed;
}

static bool run_zoom_shortcuts(
    Display *display,
    const char *sample_path,
    char *const command_arguments[],
    const char *display_name,
    bool wheel)
{
    const test_key zoom_in = {XK_plus, ControlMask | ShiftMask};
    const test_key zoom_reset = {XK_0, ControlMask};
    const test_key query_grid = {XK_g, 0};
    const test_key quit = {XK_q, 0};
    static const char expected_grid_title[] = "grid-24-80";
    pid_t sample_process = -1;
    int sample_output = -1;
    Window window = None;
    XWindowAttributes initial;
    XWindowAttributes zoomed;
    XWindowAttributes final;
    unsigned int scrollbar_width = boxrig_x11_scrollbar_pixel_width();
    int sample_status = 0;
    bool passed = false;

    sample_process = start_sample(
        sample_path,
        command_arguments,
        display_name,
        "xterm-256color",
        false,
        &sample_output);
    if (sample_process == -1) {
        return false;
    }
    if (!wait_for_window(display, sample_output, &window)) {
        fprintf(stderr, "zoom sample did not map its window\n");
        goto cleanup;
    }
    if (XGetWindowAttributes(display, window, &initial) == 0 ||
        !window_has_no_children(display, window) ||
        !window_has_rendered_content(display, window, sample_output) ||
        !query_cell_geometry(
            display,
            window,
            sample_output,
            &initial,
            scrollbar_width)) {
        fprintf(stderr, "zoom sample window geometry was unavailable\n");
        goto cleanup;
    }
    while (XPending(display) > 0) {
        XEvent ignored;

        XNextEvent(display, &ignored);
    }
    bool zoom_sent = wheel
        ? send_terminal_button(display, window, 20, 20, Button4, ButtonPress,
              ControlMask, CurrentTime)
        : send_key_transition(display, window, zoom_in, KeyPress) &&
          send_key_transition(display, window, zoom_in, KeyRelease);
    if (!zoom_sent) {
        fprintf(stderr, "zoom-in shortcut key series could not be sent\n");
        goto cleanup;
    }
    if (!wait_for_zoom_geometry(
            display,
            window,
            sample_output,
            initial.width,
            initial.height,
            true) ||
        !window_has_rendered_content(display, window, sample_output) ||
        XGetWindowAttributes(display, window, &zoomed) == 0 ||
        !query_cell_geometry(
            display,
            window,
            sample_output,
            &zoomed,
            scrollbar_width) ||
        XStoreName(display, window, "awaiting-zoom-grid") == 0 ||
        !send_key(display, window, query_grid) ||
        !wait_for_synchronized_frame_title(
            display,
            window,
            sample_output,
            expected_grid_title)) {
        fprintf(stderr, "zoom-in did not preserve the PTY grid and drawing\n");
        goto cleanup;
    }
    if (!send_key_transition(display, window, zoom_reset, KeyPress) ||
        !send_key_transition(display, window, zoom_reset, KeyRelease)) {
        fprintf(stderr, "zoom-reset shortcut key series could not be sent\n");
        goto cleanup;
    }
    if (!wait_for_zoom_geometry(
            display,
            window,
            sample_output,
            initial.width,
            initial.height,
            false) ||
        !window_has_rendered_content(display, window, sample_output) ||
        XGetWindowAttributes(display, window, &final) == 0 ||
        !query_cell_geometry(
            display,
            window,
            sample_output,
            &final,
            scrollbar_width) ||
        XStoreName(display, window, "awaiting-reset-grid") == 0 ||
        !send_key(display, window, query_grid) ||
        !wait_for_synchronized_frame_title(
            display,
            window,
            sample_output,
            expected_grid_title)) {
        fprintf(stderr, "zoom reset did not preserve the PTY grid and drawing\n");
        goto cleanup;
    }
    if (final.width != initial.width || final.height != initial.height ||
        !send_key(display, window, quit)) {
        fprintf(stderr, "zoom sample did not reset or accept terminal input\n");
        goto cleanup;
    }
    if (!wait_for_process_exit(sample_output) ||
        waitpid(sample_process, &sample_status, 0) != sample_process) {
        fprintf(stderr, "zoom sample did not exit after terminal input\n");
        goto cleanup;
    }
    sample_process = -1;
    passed = WIFEXITED(sample_status) && WEXITSTATUS(sample_status) == 0;

cleanup:
    if (sample_output != -1) {
        close(sample_output);
    }
    if (sample_process > 0) {
        kill(sample_process, SIGTERM);
        waitpid(sample_process, NULL, 0);
    }
    XSync(display, False);
    return passed;
}

static bool run_alternate_resize(
    Display *display, const char *sample_path, const char *helper_path,
    const char *display_name)
{
    static const char *renderers[] = {"--renderer=x11", "--renderer=opengl"};
    for (size_t renderer = 0; renderer < 2; renderer++) {
        char *helper_arguments[] = {(char *)helper_path, "--resize-alternate", NULL};
        int output = -1, status = 0;
        pid_t process = start_sample_with_renderer(sample_path, helper_arguments,
            display_name, "xterm-256color", false, renderers[renderer], &output);
        Window window = None;
        XWindowAttributes initial;
        XImage *actual = NULL;
        bool passed = false;
        unsigned int scrollbar = boxrig_x11_scrollbar_pixel_width();
        char grid_title[64];
        if (process <= 0 || !wait_for_window(display, output, &window) ||
            !wait_for_synchronized_frame_title(display, window, output, "resize-primary") ||
            XGetWindowAttributes(display, window, &initial) == 0) goto cleanup;
        boxrig_test_video_hold();
        if (!send_key(display, window, (test_key){XK_a, 0}) ||
            !wait_for_synchronized_frame_title(display, window, output, "resize-alternate"))
            goto cleanup;
        boxrig_test_video_hold();
        XMoveResizeWindow(display, window, 0, 0, 3840, 2160);
        XSync(display, False);
        snprintf(grid_title, sizeof(grid_title), "grid-%u-%u",
            2160u / ((unsigned int)initial.height / 24u),
            (3840u - scrollbar) / (((unsigned int)initial.width - scrollbar) / 80u));
        if (!send_key(display, window, (test_key){XK_g, 0}) ||
            !wait_for_synchronized_frame_title(display, window, output, grid_title)) goto cleanup;
        boxrig_test_video_hold();
        if (!send_key(display, window, (test_key){XK_e, 0}) ||
            !wait_for_synchronized_frame_title(display, window, output, "resize-restored"))
            goto cleanup;
        boxrig_test_video_hold();
        XResizeWindow(display, window, (unsigned int)initial.width, (unsigned int)initial.height);
        XSync(display, False);
        if (!send_key(display, window, (test_key){XK_g, 0}) ||
            !wait_for_synchronized_frame_title(display, window, output, "grid-24-80") ||
            !send_key(display, window, (test_key){XK_c, 0}) ||
            !wait_for_synchronized_frame_title(display, window, output, "cursor-24-7")) goto cleanup;
        boxrig_test_video_hold();
        actual = XGetImage(display, window, 0, 0, (unsigned int)initial.width - scrollbar,
            (unsigned int)initial.height, AllPlanes, ZPixmap);
        if (actual == NULL || !send_key(display, window, (test_key){XK_r, 0}) ||
            !wait_for_synchronized_frame_title(display, window, output, "resize-reference"))
            goto cleanup;
        /* Compare the restored viewport with its independently written rows,
         * excluding the scrollbar because the reference has no history. */
        for (size_t attempt = 0; attempt < 100; attempt++) {
            XImage *expected = XGetImage(display, window, 0, 0,
                (unsigned int)actual->width, (unsigned int)actual->height, AllPlanes, ZPixmap);
            bool matches = expected != NULL;
            if (expected != NULL) {
                for (int y = 0; matches && y < actual->height; y++) {
                    for (int x = 0; x < actual->width; x++) {
                        if (XGetPixel(actual, x, y) != XGetPixel(expected, x, y)) {
                            matches = false;
                            break;
                        }
                    }
                }
                XDestroyImage(expected);
            }
            if (matches) { passed = true; break; }
            if (!wait_for_emitter_iteration(display, window, output, 50)) break;
        }
        boxrig_test_video_hold();
        if (!passed || !send_key(display, window, (test_key){XK_q, 0}) ||
            !wait_for_process_exit(output) || waitpid(process, &status, 0) != process) {
            passed = false;
            goto cleanup;
        }
        process = -1;
        passed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
cleanup:
        if (actual != NULL) XDestroyImage(actual);
        if (output != -1) close(output);
        if (process > 0) { kill(process, SIGTERM); waitpid(process, NULL, 0); }
        XSync(display, False);
        if (!passed) {
            fprintf(stderr, "alternate 4K resize restoration failed: %s\n", renderers[renderer]);
            return false;
        }
        printf("PASS alternate 4K resize pixels and cursor: %s\n", renderers[renderer]);
    }
    return true;
}

static bool run_synchronized_frame_resize(
    Display *display,
    const char *sample_path,
    const char *helper_path,
    const char *display_name)
{
    enum {
        INITIAL_COLUMNS = 80,
        INITIAL_ROWS = 24,
        FINAL_COLUMNS = 120,
        FINAL_ROWS = 40,
        RESIZE_COUNT = 96
    };
    char *helper_arguments[2];
    const test_key frame = {XK_b, 0};
    const test_key query_grid = {XK_g, 0};
    const test_key quit = {XK_q, 0};
    pid_t sample_process = -1;
    int sample_output = -1;
    Window window = None;
    XWindowAttributes initial;
    XWindowAttributes final;
    unsigned int cell_width;
    unsigned int cell_height;
    unsigned int scrollbar_width = boxrig_x11_scrollbar_pixel_width();
    unsigned int final_width;
    unsigned int final_height;
    char expected_grid_title[64];
    int sample_status = 0;
    size_t index;
    XSyncCounter basic_counter = None;
    XSyncCounter extended_counter = None;
    XSyncValue basic_value;
    XSyncValue extended_value;
    unsigned int sync_counter_count = 0;
    bool sync_protocol_registered = false;
    bool passed = false;

    helper_arguments[0] = (char *)helper_path;
    helper_arguments[1] = NULL;
    if (!set_wm_extended_sync_support(display, false)) {
        return false;
    }
    sample_process = start_sample(
        sample_path,
        helper_arguments,
        display_name,
        "xterm-256color",
        false,
        &sample_output);
    if (sample_process == -1 ||
        !wait_for_window(display, sample_output, &window) ||
        XGetWindowAttributes(display, window, &initial) == 0 ||
        !window_has_no_children(display, window) ||
        !read_wm_sync_registration(
            display,
            window,
            &sync_protocol_registered,
            &sync_counter_count,
            &basic_counter,
            &extended_counter) ||
        !sync_protocol_registered || sync_counter_count != 2 ||
        basic_counter == extended_counter ||
        XSyncQueryCounter(display, basic_counter, &basic_value) == 0 ||
        XSyncQueryCounter(display, extended_counter, &extended_value) == 0 ||
        !XSyncValueIsZero(basic_value) ||
        !XSyncValueIsZero(extended_value) ||
        initial.width <= 0 || initial.height <= 0 ||
        initial.width <= (int)scrollbar_width ||
        ((unsigned int)initial.width - scrollbar_width) % INITIAL_COLUMNS != 0 ||
        initial.height % INITIAL_ROWS != 0) {
        fprintf(stderr, "synchronized frame setup was invalid\n");
        goto cleanup;
    }
    cell_width = ((unsigned int)initial.width - scrollbar_width) /
                 INITIAL_COLUMNS;
    cell_height = (unsigned int)initial.height / INITIAL_ROWS;
    final_width = cell_width * FINAL_COLUMNS + scrollbar_width;
    final_height = cell_height * FINAL_ROWS;
    XResizeWindow(
        display,
        window,
        (unsigned int)initial.width + cell_width,
        (unsigned int)initial.height);
    XSync(display, False);
    if (!wait_for_zoom_geometry(
            display,
            window,
            sample_output,
            initial.width,
            initial.height,
            true)) {
        fprintf(stderr, "X11 frame did not follow a width-only resize\n");
        goto cleanup;
    }
    if (snprintf(
            expected_grid_title,
            sizeof(expected_grid_title),
            "grid-%u-%u",
            FINAL_ROWS,
            FINAL_COLUMNS) < 0 ||
        !send_key(display, window, frame)) {
        goto cleanup;
    }
    for (index = 0; index < RESIZE_COUNT; index++) {
        unsigned int columns = 81 + (unsigned int)(index % 40);
        unsigned int rows = 25 + (unsigned int)(index % 15);

        if (index + 1 == RESIZE_COUNT) {
            columns = FINAL_COLUMNS;
            rows = FINAL_ROWS;
        }
        XResizeWindow(
            display,
            window,
            cell_width * columns + scrollbar_width,
            cell_height * rows);
    }
    XSync(display, False);
    if (!wait_for_synchronized_frame_title(
            display,
            window,
            sample_output,
            "synchronized-frame-complete")) {
        fprintf(stderr, "synchronized terminal update did not complete\n");
        goto cleanup;
    }
    if (!window_has_rendered_content(display, window, sample_output)) {
        fprintf(stderr, "synchronized frame did not contain terminal pixels\n");
        goto cleanup;
    }
    if (!send_key(display, window, query_grid) ||
        !wait_for_synchronized_frame_title(
            display,
            window,
            sample_output,
            expected_grid_title)) {
        fprintf(stderr, "synchronized frame did not report its final grid\n");
        goto cleanup;
    }
    if (XGetWindowAttributes(display, window, &final) == 0 ||
        final.width != (int)final_width ||
        final.height != (int)final_height) {
        fprintf(stderr, "synchronized frame geometry was not final\n");
        goto cleanup;
    }
    if (!send_key(display, window, quit) ||
        !wait_for_process_exit(sample_output) ||
        waitpid(sample_process, &sample_status, 0) != sample_process) {
        fprintf(stderr, "synchronized frame sample did not exit\n");
        goto cleanup;
    }
    sample_process = -1;
    passed = WIFEXITED(sample_status) && WEXITSTATUS(sample_status) == 0;

cleanup:
    if (sample_output != -1) {
        close(sample_output);
    }
    if (sample_process > 0) {
        kill(sample_process, SIGTERM);
        waitpid(sample_process, NULL, 0);
    }
    XSync(display, False);
    return passed;
}

static bool wait_for_wm_sync_alarm(
    Display *display,
    Window window,
    int process_output,
    int sync_event_base,
    XSyncAlarm alarm,
    XSyncValue requested_value)
{
    size_t attempt;

    for (attempt = 0; attempt < 100; attempt++) {
        struct pollfd descriptors[2];
        int result;

        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == sync_event_base + XSyncAlarmNotify) {
                XSyncAlarmNotifyEvent *alarm_event =
                    (XSyncAlarmNotifyEvent *)&event;

                if (alarm_event->alarm == alarm &&
                    XSyncValueEqual(
                        alarm_event->counter_value,
                        requested_value)) {
                    return true;
                }
            }
            if (event.type == DestroyNotify &&
                event.xdestroywindow.window == window) {
                return false;
            }
        }
        descriptors[0].fd = ConnectionNumber(display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = process_output;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2, 50);
        if (result < 0 && errno != EINTR) {
            return false;
        }
        if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
            return false;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            char buffer[256];

            if (read(process_output, buffer, sizeof(buffer)) <= 0) {
                return false;
            }
        }
    }
    return false;
}

static bool run_wm_sync_resize(
    Display *display,
    const char *sample_path,
    const char *helper_path,
    const char *display_name)
{
    enum {
        INITIAL_COLUMNS = 80,
        INITIAL_ROWS = 24,
        FINAL_COLUMNS = 100,
        FINAL_ROWS = 30
    };
    const test_key query_grid = {XK_g, 0};
    const test_key quit = {XK_q, 0};
    char *helper_arguments[2];
    pid_t sample_process = -1;
    int sample_output = -1;
    Window window = None;
    XWindowAttributes initial;
    XWindowAttributes final;
    unsigned int scrollbar_width = boxrig_x11_scrollbar_pixel_width();
    unsigned int cell_width;
    unsigned int cell_height;
    unsigned int final_width;
    unsigned int final_height;
    XSyncCounter basic_counter = None;
    XSyncCounter extended_counter = None;
    XSyncAlarm alarm = None;
    XSyncAlarmAttributes alarm_attributes;
    XSyncValue initial_basic_value;
    XSyncValue initial_extended_value;
    XSyncValue current_basic_value;
    XSyncValue current_extended_value;
    XSyncValue requested_value;
    int sync_event_base = 0;
    int sync_error_base = 0;
    int sync_major = 0;
    int sync_minor = 0;
    int sample_status = 0;
    unsigned int counter_count = 0;
    bool protocol_registered = false;
    bool passed = false;

    helper_arguments[0] = (char *)helper_path;
    helper_arguments[1] = NULL;
    if (XSyncQueryExtension(
            display,
            &sync_event_base,
            &sync_error_base) == 0 ||
        XSyncInitialize(display, &sync_major, &sync_minor) == 0 ||
        !set_wm_extended_sync_support(display, false)) {
        fprintf(stderr, "X11 Sync extension was unavailable\n");
        return false;
    }
    sample_process = start_sample(
        sample_path,
        helper_arguments,
        display_name,
        "xterm-256color",
        false,
        &sample_output);
    if (sample_process == -1 ||
        !wait_for_window(display, sample_output, &window) ||
        !window_has_no_children(display, window) ||
        XGetWindowAttributes(display, window, &initial) == 0 ||
        initial.width <= (int)scrollbar_width || initial.height <= 0 ||
        ((unsigned int)initial.width - scrollbar_width) %
                INITIAL_COLUMNS !=
            0 ||
        (unsigned int)initial.height % INITIAL_ROWS != 0 ||
        !window_has_rendered_content(display, window, sample_output) ||
        !read_wm_sync_registration(
            display,
            window,
            &protocol_registered,
            &counter_count,
            &basic_counter,
            &extended_counter) ||
        !protocol_registered || counter_count != 2 ||
        basic_counter == extended_counter ||
        XSyncQueryCounter(
            display,
            basic_counter,
            &initial_basic_value) == 0 ||
        XSyncQueryCounter(
            display,
            extended_counter,
            &initial_extended_value) == 0) {
        fprintf(stderr, "X11 sample did not register WM sync support\n");
        goto cleanup;
    }
    XSyncIntsToValue(&requested_value, UINT32_C(47), 1);
    if (!XSyncValueLessThan(initial_basic_value, requested_value) ||
        XStoreName(display, window, "awaiting-wm-sync-request") == 0 ||
        !send_wm_sync_request(display, window, requested_value, false) ||
        !send_key(display, window, query_grid) ||
        !wait_for_synchronized_frame_title(
            display,
            window,
            sample_output,
            "grid-24-80") ||
        XSyncQueryCounter(
            display,
            basic_counter,
            &current_basic_value) == 0 ||
        XSyncQueryCounter(
            display,
            extended_counter,
            &current_extended_value) == 0 ||
        !XSyncValueEqual(current_basic_value, initial_basic_value) ||
        !XSyncValueEqual(current_extended_value, initial_extended_value)) {
        fprintf(stderr, "WM sync counter advanced before ConfigureNotify\n");
        goto cleanup;
    }
    memset(&alarm_attributes, 0, sizeof(alarm_attributes));
    alarm_attributes.trigger.counter = basic_counter;
    alarm_attributes.trigger.value_type = XSyncAbsolute;
    alarm_attributes.trigger.wait_value = requested_value;
    alarm_attributes.trigger.test_type = XSyncPositiveComparison;
    XSyncIntToValue(&alarm_attributes.delta, 0);
    alarm_attributes.events = True;
    alarm = XSyncCreateAlarm(
        display,
        XSyncCACounter | XSyncCAValueType | XSyncCAValue |
            XSyncCATestType | XSyncCADelta | XSyncCAEvents,
        &alarm_attributes);
    if (alarm == None) {
        fprintf(stderr, "WM sync alarm could not be created\n");
        goto cleanup;
    }
    cell_width = ((unsigned int)initial.width - scrollbar_width) /
                 INITIAL_COLUMNS;
    cell_height = (unsigned int)initial.height / INITIAL_ROWS;
    final_width = cell_width * FINAL_COLUMNS + scrollbar_width;
    final_height = cell_height * FINAL_ROWS;
    XResizeWindow(display, window, final_width, final_height);
    XFlush(display);
    if (!wait_for_wm_sync_alarm(
            display,
            window,
            sample_output,
            sync_event_base,
            alarm,
            requested_value) ||
        XSyncQueryCounter(
            display,
            basic_counter,
            &current_basic_value) == 0 ||
        XSyncQueryCounter(
            display,
            extended_counter,
            &current_extended_value) == 0 ||
        !XSyncValueEqual(current_basic_value, requested_value) ||
        !XSyncValueEqual(current_extended_value, initial_extended_value) ||
        XGetWindowAttributes(display, window, &final) == 0 ||
        final.width != (int)final_width || final.height != (int)final_height ||
        !window_has_rendered_content(display, window, sample_output) ||
        XStoreName(display, window, "awaiting-wm-sync-grid") == 0 ||
        !send_key(display, window, query_grid) ||
        !wait_for_synchronized_frame_title(
            display,
            window,
            sample_output,
            "grid-30-100") ||
        !send_key(display, window, quit) ||
        !wait_for_process_exit(sample_output) ||
        waitpid(sample_process, &sample_status, 0) != sample_process) {
        fprintf(stderr, "WM sync counter did not follow completed frame\n");
        goto cleanup;
    }
    sample_process = -1;
    passed = WIFEXITED(sample_status) && WEXITSTATUS(sample_status) == 0;

cleanup:
    if (alarm != None) {
        (void)XSyncDestroyAlarm(display, alarm);
    }
    if (sample_output != -1) {
        close(sample_output);
    }
    if (sample_process > 0) {
        kill(sample_process, SIGTERM);
        waitpid(sample_process, NULL, 0);
    }
    (void)set_wm_extended_sync_support(display, false);
    XSync(display, False);
    return passed;
}

static bool wait_for_managed_window(
    Display *display,
    int process_output,
    Window *window_out)
{
    Window managed_window = None;
    size_t attempt;

    for (attempt = 0; attempt < 100; attempt++) {
        struct pollfd descriptors[2];
        int result;

        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == ConfigureRequest) {
                XConfigureRequestEvent *request =
                    &event.xconfigurerequest;
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
                    (unsigned int)request->value_mask,
                    &changes);
                XFlush(display);
            } else if (event.type == MapRequest) {
                if (managed_window != None &&
                    managed_window != event.xmaprequest.window) {
                    return false;
                }
                managed_window = event.xmaprequest.window;
                XMapWindow(display, managed_window);
                XFlush(display);
            } else if (event.type == MapNotify &&
                       managed_window != None &&
                       event.xmap.window == managed_window) {
                *window_out = managed_window;
                return true;
            } else if (event.type == DestroyNotify &&
                       managed_window != None &&
                       event.xdestroywindow.window == managed_window) {
                return false;
            }
        }
        descriptors[0].fd = ConnectionNumber(display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = process_output;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2, 50);
        if (result < 0 && errno != EINTR) {
            return false;
        }
        if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
            return false;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            char buffer[256];

            if (read(process_output, buffer, sizeof(buffer)) <= 0) {
                return false;
            }
        }
    }
    return false;
}

static bool completed_frame_has_terminal_and_scrollbar(
    Display *display,
    Window window)
{
    XWindowAttributes attributes;
    XColor background;
    XImage *image;
    unsigned int scrollbar_width = boxrig_x11_scrollbar_pixel_width();
    size_t content_background_pixels = 0;
    size_t content_foreground_pixels = 0;
    size_t scrollbar_foreground_pixels = 0;
    size_t content_pixels;
    size_t scrollbar_pixels;
    unsigned long color_mask;
    int x;
    int y;

    memset(&background, 0, sizeof(background));
    background.red = 0x1010;
    background.green = 0x1010;
    background.blue = 0x1010;
    background.flags = DoRed | DoGreen | DoBlue;
    if (XGetWindowAttributes(display, window, &attributes) == 0 ||
        attributes.width <= (int)scrollbar_width || attributes.height <= 0 ||
        XAllocColor(
            display,
            attributes.colormap,
            &background) == 0) {
        return false;
    }
    image = XGetImage(
        display,
        window,
        0,
        0,
        (unsigned int)attributes.width,
        (unsigned int)attributes.height,
        AllPlanes,
        ZPixmap);
    if (image == NULL) {
        return false;
    }
    color_mask = attributes.visual->red_mask |
                 attributes.visual->green_mask |
                 attributes.visual->blue_mask;
    for (y = 0; y < attributes.height; y++) {
        for (x = 0; x < attributes.width; x++) {
            unsigned long pixel =
                XGetPixel(image, x, y) & color_mask;

            if (x < attributes.width - (int)scrollbar_width) {
                if (pixel == (background.pixel & color_mask)) {
                    content_background_pixels++;
                } else {
                    content_foreground_pixels++;
                }
            } else if (pixel != (background.pixel & color_mask)) {
                scrollbar_foreground_pixels++;
            }
        }
    }
    XDestroyImage(image);
    content_pixels =
        (size_t)(attributes.width - (int)scrollbar_width) *
        (size_t)attributes.height;
    scrollbar_pixels =
        (size_t)scrollbar_width * (size_t)attributes.height;
    return content_background_pixels > content_pixels / 2 &&
           content_foreground_pixels > 100 &&
           scrollbar_foreground_pixels > scrollbar_pixels / 2;
}

static bool wait_for_test_wm_frame(
    Display *display,
    Window window,
    int process_output,
    int sync_event_base,
    XSyncAlarm alarm,
    XSyncCounter extended_counter,
    int old_width,
    int old_height,
    uint64_t old_hash,
    bool send_extended_request,
    XSyncValue *request_value_out,
    XSyncValue *completed_value_out,
    XWindowAttributes *completed_attributes_out)
{
    bool initial_odd_seen = false;
    bool requested_odd_seen = false;
    bool resize_applied = false;
    bool configure_seen = false;
    unsigned int requested_width = 0;
    unsigned int requested_height = 0;
    XSyncValue request_value;
    size_t attempt;

    XSyncIntToValue(&request_value, 0);
    for (attempt = 0; attempt < 200; attempt++) {
        struct pollfd descriptors[2];
        int result;

        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == sync_event_base + XSyncAlarmNotify) {
                XSyncAlarmNotifyEvent *alarm_event =
                    (XSyncAlarmNotifyEvent *)&event;
                bool odd;

                if (alarm_event->alarm != alarm) {
                    continue;
                }
                odd = (XSyncValueLow32(
                           alarm_event->counter_value) & 1u) != 0;
                if (!initial_odd_seen) {
                    XWindowAttributes frozen_attributes;
                    XSyncValue current_value;
                    uint64_t frozen_hash;

                    if (!odd || resize_applied ||
                        XSyncQueryCounter(
                            display,
                            extended_counter,
                            &current_value) == 0 ||
                        !XSyncValueEqual(
                            current_value,
                            alarm_event->counter_value) ||
                        XGetWindowAttributes(
                            display,
                            window,
                            &frozen_attributes) == 0 ||
                        frozen_attributes.width != old_width ||
                        frozen_attributes.height != old_height ||
                        !read_window_hash(
                            display,
                            window,
                            (unsigned int)old_width,
                            (unsigned int)old_height,
                            &frozen_hash) ||
                        frozen_hash != old_hash) {
                        return false;
                    }
                    initial_odd_seen = true;
                } else if (odd) {
                    XSyncAlarmAttributes completion_attributes;
                    XSyncValue one;
                    int overflow = 0;

                    if (!send_extended_request || !resize_applied ||
                        !XSyncValueGreaterOrEqual(
                            alarm_event->counter_value,
                            request_value)) {
                        return false;
                    }
                    memset(
                        &completion_attributes,
                        0,
                        sizeof(completion_attributes));
                    XSyncIntToValue(&one, 1);
                    XSyncValueAdd(
                        &completion_attributes.trigger.wait_value,
                        alarm_event->counter_value,
                        one,
                        &overflow);
                    completion_attributes.trigger.test_type =
                        XSyncPositiveComparison;
                    XSyncIntToValue(&completion_attributes.delta, 0);
                    completion_attributes.events = True;
                    if (overflow != 0 ||
                        XSyncChangeAlarm(
                            display,
                            alarm,
                            XSyncCAValue | XSyncCATestType |
                                XSyncCADelta | XSyncCAEvents,
                            &completion_attributes) == 0) {
                        return false;
                    }
                    XFlush(display);
                    requested_odd_seen = true;
                } else {
                    XSyncValue current_value;

                    if (!resize_applied || !configure_seen ||
                        (send_extended_request &&
                         (!requested_odd_seen ||
                          !XSyncValueGreaterThan(
                              alarm_event->counter_value,
                              request_value))) ||
                        XSyncQueryCounter(
                            display,
                            extended_counter,
                            &current_value) == 0 ||
                        !XSyncValueEqual(
                            current_value,
                            alarm_event->counter_value) ||
                        XGetWindowAttributes(
                            display,
                            window,
                            completed_attributes_out) == 0 ||
                        completed_attributes_out->width !=
                            (int)requested_width ||
                        completed_attributes_out->height !=
                            (int)requested_height ||
                        !completed_frame_has_terminal_and_scrollbar(
                            display,
                            window)) {
                        return false;
                    }
                    *request_value_out = request_value;
                    *completed_value_out = alarm_event->counter_value;
                    return true;
                }
            } else if (event.type == ConfigureRequest &&
                       event.xconfigurerequest.window == window) {
                XConfigureRequestEvent *request =
                    &event.xconfigurerequest;
                XWindowAttributes frozen_attributes;
                XSyncValue current_value;
                uint64_t frozen_hash;

                if (!initial_odd_seen || resize_applied ||
                    XSyncQueryCounter(
                        display,
                        extended_counter,
                        &current_value) == 0 ||
                    (XSyncValueLow32(current_value) & 1u) == 0 ||
                    XGetWindowAttributes(
                        display,
                        window,
                        &frozen_attributes) == 0 ||
                    frozen_attributes.width != old_width ||
                    frozen_attributes.height != old_height ||
                    !read_window_hash(
                        display,
                        window,
                        (unsigned int)old_width,
                        (unsigned int)old_height,
                        &frozen_hash) ||
                    frozen_hash != old_hash) {
                    return false;
                }
                if (send_extended_request) {
                    XSyncValue offset;
                    int overflow = 0;

                    XSyncIntToValue(&offset, 240);
                    XSyncValueAdd(
                        &request_value,
                        current_value,
                        offset,
                        &overflow);
                    if (overflow != 0 ||
                        !send_wm_sync_request(
                            display,
                            window,
                            request_value,
                            true)) {
                        return false;
                    }
                }
                requested_width = (unsigned int)request->width;
                requested_height = (unsigned int)request->height;
                XResizeWindow(
                    display,
                    window,
                    requested_width,
                    requested_height);
                XFlush(display);
                resize_applied = true;
            } else if (event.type == ConfigureNotify &&
                       event.xconfigure.window == window &&
                       resize_applied &&
                       event.xconfigure.width == (int)requested_width &&
                       event.xconfigure.height == (int)requested_height) {
                configure_seen = true;
            } else if (event.type == DestroyNotify &&
                       event.xdestroywindow.window == window) {
                return false;
            }
        }
        descriptors[0].fd = ConnectionNumber(display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = process_output;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2, 50);
        if (result < 0 && errno != EINTR) {
            return false;
        }
        if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
            return false;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            char buffer[256];

            if (read(process_output, buffer, sizeof(buffer)) <= 0) {
                return false;
            }
        }
    }
    return false;
}

static bool send_wm_frame_notifications(
    Display *display,
    Window window,
    XSyncValue completed_value)
{
    XEvent event;
    Atom frame_drawn =
        XInternAtom(display, "_NET_WM_FRAME_DRAWN", False);
    Atom frame_timings =
        XInternAtom(display, "_NET_WM_FRAME_TIMINGS", False);

    if (frame_drawn == None || frame_timings == None) {
        return false;
    }
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.display = display;
    event.xclient.window = window;
    event.xclient.format = 32;
    event.xclient.message_type = frame_drawn;
    event.xclient.data.l[0] =
        (long)XSyncValueLow32(completed_value);
    event.xclient.data.l[1] =
        (long)(uint32_t)XSyncValueHigh32(completed_value);
    if (XSendEvent(
            display,
            window,
            False,
            NoEventMask,
            &event) == 0) {
        return false;
    }
    event.xclient.message_type = frame_timings;
    event.xclient.data.l[2] = 0;
    event.xclient.data.l[3] = 16667;
    event.xclient.data.l[4] = 0;
    if (XSendEvent(
            display,
            window,
            False,
            NoEventMask,
            &event) == 0) {
        return false;
    }
    XFlush(display);
    return true;
}

static bool send_wm_delete(Display *display, Window window)
{
    XEvent event;
    Atom wm_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    Atom delete_window =
        XInternAtom(display, "WM_DELETE_WINDOW", False);

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
    if (XSendEvent(
            display,
            window,
            False,
            NoEventMask,
            &event) == 0) {
        return false;
    }
    XFlush(display);
    return true;
}

static bool read_cell_resize_hints(
    Display *display,
    Window window,
    unsigned int scrollbar_width,
    XSizeHints *hints_out)
{
    const long required_flags = PBaseSize | PResizeInc | PMinSize;
    XSizeHints hints;
    long supplied = 0;

    memset(&hints, 0, sizeof(hints));
    if (hints_out == NULL ||
        XGetWMNormalHints(display, window, &hints, &supplied) == 0 ||
        hints.flags != required_flags ||
        hints.base_width != (int)scrollbar_width ||
        hints.base_height != 0 || hints.width_inc <= 0 ||
        hints.height_inc <= 0 ||
        hints.min_width != hints.base_width + hints.width_inc ||
        hints.min_height != hints.base_height + hints.height_inc) {
        return false;
    }
    *hints_out = hints;
    return true;
}

static bool size_follows_cell_resize_hints(
    const XSizeHints *hints,
    int width,
    int height)
{
    return hints != NULL && width >= hints->min_width &&
           height >= hints->min_height &&
           (width - hints->base_width) % hints->width_inc == 0 &&
           (height - hints->base_height) % hints->height_inc == 0;
}

static bool wait_for_grid_without_resize_request(
    Display *display,
    Window window,
    int process_output,
    unsigned int rows,
    unsigned int columns)
{
    const test_key query_grid = {XK_g, 0};
    char expected[64];
    size_t attempt;

    if (snprintf(
            expected,
            sizeof(expected),
            "grid-%u-%u",
            rows,
            columns) < 0 ||
        XStoreName(display, window, "awaiting-resize-grid") == 0 ||
        !send_key(display, window, query_grid)) {
        return false;
    }
    for (attempt = 0; attempt < 100; attempt++) {
        struct pollfd descriptors[2];
        char *title = NULL;
        bool title_matches = false;
        int result;

        XSync(display, False);
        if (XFetchName(display, window, &title) != 0 && title != NULL) {
            title_matches = strcmp(title, expected) == 0;
            XFree(title);
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == ConfigureRequest &&
                event.xconfigurerequest.window == window) {
                return false;
            }
            if (event.type == DestroyNotify &&
                event.xdestroywindow.window == window) {
                return false;
            }
        }
        if (title_matches) {
            return true;
        }
        descriptors[0].fd = ConnectionNumber(display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = process_output;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2, 50);
        if (result < 0 && errno != EINTR) {
            return false;
        }
        if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
            return false;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            char buffer[256];

            if (read(process_output, buffer, sizeof(buffer)) <= 0) {
                return false;
            }
        }
    }
    return false;
}

static bool wait_for_application_cell_resize(
    Display *display,
    Window window,
    int process_output,
    unsigned int scrollbar_width,
    unsigned int rows,
    unsigned int columns,
    const XSizeHints *previous_hints,
    XSizeHints *updated_hints_out)
{
    size_t attempt;

    for (attempt = 0; attempt < 100; attempt++) {
        struct pollfd descriptors[2];
        int result;

        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == ConfigureRequest &&
                event.xconfigurerequest.window == window) {
                XConfigureRequestEvent *request =
                    &event.xconfigurerequest;
                XSizeHints updated_hints;
                long expected_width;
                long expected_height;

                if ((request->value_mask & (CWWidth | CWHeight)) !=
                        (CWWidth | CWHeight) ||
                    !read_cell_resize_hints(
                        display,
                        window,
                        scrollbar_width,
                        &updated_hints) ||
                    (updated_hints.width_inc ==
                         previous_hints->width_inc &&
                     updated_hints.height_inc ==
                         previous_hints->height_inc)) {
                    return false;
                }
                expected_width =
                    (long)updated_hints.base_width +
                    (long)columns * updated_hints.width_inc;
                expected_height =
                    (long)updated_hints.base_height +
                    (long)rows * updated_hints.height_inc;
                if (expected_width > INT_MAX ||
                    expected_height > INT_MAX ||
                    request->width != expected_width ||
                    request->height != expected_height) {
                    return false;
                }
                XResizeWindow(
                    display,
                    window,
                    (unsigned int)request->width,
                    (unsigned int)request->height);
                XFlush(display);
                *updated_hints_out = updated_hints;
                return true;
            }
            if (event.type == DestroyNotify &&
                event.xdestroywindow.window == window) {
                return false;
            }
        }
        descriptors[0].fd = ConnectionNumber(display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = process_output;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2, 50);
        if (result < 0 && errno != EINTR) {
            return false;
        }
        if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
            return false;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            char buffer[256];

            if (read(process_output, buffer, sizeof(buffer)) <= 0) {
                return false;
            }
        }
    }
    return false;
}

static bool run_resize_increment_contract(
    Display *display,
    const char *sample_path,
    const char *helper_path,
    const char *display_name)
{
    enum {
        INITIAL_COLUMNS = 80,
        INITIAL_ROWS = 24,
        EXACT_COLUMNS = 83,
        EXACT_ROWS = 27,
        CONSTRAINED_COLUMNS = 84,
        CONSTRAINED_ROWS = 28
    };
    const test_key zoom_in = {XK_plus, ControlMask | ShiftMask};
    const test_key zoom_reset = {XK_0, ControlMask};
    const test_key quit = {XK_q, 0};
    char *helper_arguments[2];
    pid_t sample_process = -1;
    int sample_output = -1;
    Window window = None;
    XWindowAttributes initial;
    XSizeHints initial_hints;
    XSizeHints zoomed_hints;
    XSizeHints reset_hints;
    unsigned int scrollbar_width = boxrig_x11_scrollbar_pixel_width();
    unsigned int exact_width;
    unsigned int exact_height;
    unsigned int constrained_width;
    unsigned int constrained_height;
    int sample_status = 0;
    const char *stage = "enable test window manager";
    bool root_redirected = false;
    bool passed = false;

    helper_arguments[0] = (char *)helper_path;
    helper_arguments[1] = NULL;
    if (!set_wm_extended_sync_support(display, false)) {
        return false;
    }
    XSelectInput(
        display,
        DefaultRootWindow(display),
        SubstructureRedirectMask | SubstructureNotifyMask);
    XSync(display, False);
    root_redirected = true;
    stage = "start managed sample";
    sample_process = start_sample(
        sample_path,
        helper_arguments,
        display_name,
        "xterm-256color",
        false,
        &sample_output);
    if (sample_process == -1 ||
        !wait_for_managed_window(display, sample_output, &window) ||
        !window_has_no_children(display, window)) {
        goto cleanup;
    }
    stage = "read initial WM_NORMAL_HINTS";
    if (XGetWindowAttributes(display, window, &initial) == 0 ||
        !read_cell_resize_hints(
            display,
            window,
            scrollbar_width,
            &initial_hints) ||
        !size_follows_cell_resize_hints(
            &initial_hints,
            initial.width,
            initial.height) ||
        (initial.width - initial_hints.base_width) /
                initial_hints.width_inc !=
            INITIAL_COLUMNS ||
        (initial.height - initial_hints.base_height) /
                initial_hints.height_inc !=
            INITIAL_ROWS) {
        goto cleanup;
    }
    exact_width = (unsigned int)(
        initial_hints.base_width +
        EXACT_COLUMNS * initial_hints.width_inc);
    exact_height = (unsigned int)(
        initial_hints.base_height +
        EXACT_ROWS * initial_hints.height_inc);
    stage = "apply hint-respecting interactive size";
    XResizeWindow(display, window, exact_width, exact_height);
    XFlush(display);
    if (!wait_for_grid_without_resize_request(
            display,
            window,
            sample_output,
            EXACT_ROWS,
            EXACT_COLUMNS)) {
        goto cleanup;
    }
    constrained_width = (unsigned int)(
        initial_hints.base_width +
        CONSTRAINED_COLUMNS * initial_hints.width_inc +
        initial_hints.width_inc / 2);
    constrained_height = (unsigned int)(
        initial_hints.base_height +
        CONSTRAINED_ROWS * initial_hints.height_inc +
        initial_hints.height_inc / 2);
    stage = "apply ignored-hint interactive size";
    XResizeWindow(
        display,
        window,
        constrained_width,
        constrained_height);
    XFlush(display);
    if (!wait_for_grid_without_resize_request(
            display,
            window,
            sample_output,
            CONSTRAINED_ROWS,
            CONSTRAINED_COLUMNS)) {
        goto cleanup;
    }
    stage = "observe zoom hints before ConfigureRequest";
    if (!send_key_transition(display, window, zoom_in, KeyPress) ||
        !send_key_transition(display, window, zoom_in, KeyRelease) ||
        !wait_for_application_cell_resize(
            display,
            window,
            sample_output,
            scrollbar_width,
            CONSTRAINED_ROWS,
            CONSTRAINED_COLUMNS,
            &initial_hints,
            &zoomed_hints) ||
        !wait_for_grid_without_resize_request(
            display,
            window,
            sample_output,
            CONSTRAINED_ROWS,
            CONSTRAINED_COLUMNS)) {
        goto cleanup;
    }
    stage = "observe reset hints before ConfigureRequest";
    if (!send_key_transition(display, window, zoom_reset, KeyPress) ||
        !send_key_transition(display, window, zoom_reset, KeyRelease) ||
        !wait_for_application_cell_resize(
            display,
            window,
            sample_output,
            scrollbar_width,
            CONSTRAINED_ROWS,
            CONSTRAINED_COLUMNS,
            &zoomed_hints,
            &reset_hints) ||
        reset_hints.width_inc != initial_hints.width_inc ||
        reset_hints.height_inc != initial_hints.height_inc ||
        !wait_for_grid_without_resize_request(
            display,
            window,
            sample_output,
            CONSTRAINED_ROWS,
            CONSTRAINED_COLUMNS)) {
        goto cleanup;
    }
    stage = "exit managed sample";
    if (!send_key(display, window, quit) ||
        !wait_for_process_exit(sample_output) ||
        waitpid(sample_process, &sample_status, 0) != sample_process) {
        goto cleanup;
    }
    sample_process = -1;
    passed = WIFEXITED(sample_status) && WEXITSTATUS(sample_status) == 0;

cleanup:
    if (sample_output != -1) {
        close(sample_output);
    }
    if (sample_process > 0) {
        kill(sample_process, SIGTERM);
        waitpid(sample_process, NULL, 0);
    }
    if (root_redirected) {
        XSelectInput(
            display,
            DefaultRootWindow(display),
            SubstructureNotifyMask);
    }
    (void)set_wm_extended_sync_support(display, false);
    XSync(display, False);
    if (!passed) {
        fprintf(stderr, "X11 resize increments failed at %s\n", stage);
    }
    return passed;
}

static bool run_extended_wm_sync_resize(
    Display *display,
    const char *sample_path,
    const char *helper_path,
    const char *display_name)
{
    const test_key zoom_in = {XK_plus, ControlMask | ShiftMask};
    const test_key zoom_reset = {XK_0, ControlMask};
    char *helper_arguments[2];
    pid_t sample_process = -1;
    int sample_output = -1;
    Window window = None;
    XWindowAttributes initial;
    XWindowAttributes zoomed;
    XWindowAttributes reset;
    XSyncCounter basic_counter = None;
    XSyncCounter extended_counter = None;
    XSyncAlarm alarm = None;
    XSyncAlarmAttributes alarm_attributes;
    XSyncValue initial_basic_value;
    XSyncValue initial_extended_value;
    XSyncValue current_basic_value;
    XSyncValue alarm_value;
    XSyncValue alarm_offset;
    XSyncValue first_request_value;
    XSyncValue first_completed_value;
    XSyncValue second_request_value;
    XSyncValue second_completed_value;
    uint64_t initial_hash;
    uint64_t zoomed_hash;
    int sync_event_base = 0;
    int sync_error_base = 0;
    int sample_status = 0;
    int overflow = 0;
    unsigned int counter_count = 0;
    bool protocol_registered = false;
    bool root_redirected = false;
    bool passed = false;

    helper_arguments[0] = (char *)helper_path;
    helper_arguments[1] = NULL;
    if (XSyncQueryExtension(
            display,
            &sync_event_base,
            &sync_error_base) == 0 ||
        !set_wm_extended_sync_support(display, true)) {
        return false;
    }
    XSelectInput(
        display,
        DefaultRootWindow(display),
        SubstructureRedirectMask | SubstructureNotifyMask);
    XSync(display, False);
    root_redirected = true;
    sample_process = start_sample(
        sample_path,
        helper_arguments,
        display_name,
        "xterm-256color",
        false,
        &sample_output);
    if (sample_process == -1 ||
        !wait_for_managed_window(display, sample_output, &window) ||
        !window_has_no_children(display, window) ||
        !window_has_rendered_content(display, window, sample_output) ||
        XGetWindowAttributes(display, window, &initial) == 0 ||
        !read_wm_sync_registration(
            display,
            window,
            &protocol_registered,
            &counter_count,
            &basic_counter,
            &extended_counter) ||
        !protocol_registered || counter_count != 2 ||
        basic_counter == extended_counter ||
        XSyncQueryCounter(
            display,
            basic_counter,
            &initial_basic_value) == 0 ||
        XSyncQueryCounter(
            display,
            extended_counter,
            &initial_extended_value) == 0 ||
        (XSyncValueLow32(initial_extended_value) & 1u) != 0 ||
        !read_window_hash(
            display,
            window,
            (unsigned int)initial.width,
            (unsigned int)initial.height,
            &initial_hash)) {
        fprintf(stderr, "extended WM sync registration was unavailable\n");
        goto cleanup;
    }
    XSyncIntToValue(&alarm_offset, 1);
    XSyncValueAdd(
        &alarm_value,
        initial_extended_value,
        alarm_offset,
        &overflow);
    memset(&alarm_attributes, 0, sizeof(alarm_attributes));
    alarm_attributes.trigger.counter = extended_counter;
    alarm_attributes.trigger.value_type = XSyncAbsolute;
    alarm_attributes.trigger.wait_value = alarm_value;
    alarm_attributes.trigger.test_type = XSyncPositiveTransition;
    XSyncIntToValue(&alarm_attributes.delta, 1);
    alarm_attributes.events = True;
    alarm = overflow == 0
                ? XSyncCreateAlarm(
                      display,
                      XSyncCACounter | XSyncCAValueType | XSyncCAValue |
                          XSyncCATestType | XSyncCADelta | XSyncCAEvents,
                      &alarm_attributes)
                : None;
    while (XPending(display) > 0) {
        XEvent ignored;

        XNextEvent(display, &ignored);
    }
    if (alarm == None ||
        !send_key_transition(display, window, zoom_in, KeyPress) ||
        !send_key_transition(display, window, zoom_in, KeyRelease) ||
        !wait_for_test_wm_frame(
            display,
            window,
            sample_output,
            sync_event_base,
            alarm,
            extended_counter,
            initial.width,
            initial.height,
            initial_hash,
            false,
            &first_request_value,
            &first_completed_value,
            &zoomed) ||
        zoomed.width <= initial.width || zoomed.height < initial.height ||
        !XSyncValueIsZero(first_request_value) ||
        (XSyncValueLow32(first_completed_value) & 1u) != 0 ||
        XSyncQueryCounter(
            display,
            basic_counter,
            &current_basic_value) == 0 ||
        !XSyncValueEqual(current_basic_value, initial_basic_value) ||
        !read_window_hash(
            display,
            window,
            (unsigned int)zoomed.width,
            (unsigned int)zoomed.height,
            &zoomed_hash)) {
        fprintf(stderr, "application frame was not synchronized before resize\n");
        goto cleanup;
    }
    if (!send_key_transition(display, window, zoom_reset, KeyPress) ||
        !send_key_transition(display, window, zoom_reset, KeyRelease)) {
        fprintf(stderr, "zoom reset could not be sent to the managed window\n");
        goto cleanup;
    }
    if (!wait_for_test_wm_frame(
            display,
            window,
            sample_output,
            sync_event_base,
            alarm,
            extended_counter,
            zoomed.width,
            zoomed.height,
            zoomed_hash,
            true,
            &second_request_value,
            &second_completed_value,
            &reset)) {
        XSyncValue failed_value;

        if (XSyncQueryCounter(
                display,
                extended_counter,
                &failed_value) != 0) {
            fprintf(
                stderr,
                "extended WM resize frame stopped at counter %u\n",
                XSyncValueLow32(failed_value));
        } else {
            fprintf(stderr, "extended WM resize counter was unavailable\n");
        }
        goto cleanup;
    }
    if (reset.width != initial.width || reset.height != initial.height ||
        !XSyncValueGreaterThan(
            second_completed_value,
            second_request_value) ||
        (XSyncValueLow32(second_completed_value) & 1u) != 0 ||
        XSyncQueryCounter(
            display,
            basic_counter,
            &current_basic_value) == 0 ||
        !XSyncValueEqual(current_basic_value, initial_basic_value)) {
        fprintf(stderr, "extended WM counters or reset geometry were invalid\n");
        goto cleanup;
    }
    if (!send_wm_frame_notifications(
            display,
            window,
            second_completed_value) ||
        !send_wm_delete(display, window)) {
        fprintf(stderr, "frame acknowledgement or WM delete could not be sent\n");
        goto cleanup;
    }
    if (!wait_for_process_exit(sample_output) ||
        waitpid(sample_process, &sample_status, 0) != sample_process) {
        fprintf(stderr, "sample did not exit after synchronized WM delete\n");
        goto cleanup;
    }
    sample_process = -1;
    passed = WIFEXITED(sample_status) && WEXITSTATUS(sample_status) == 0;

cleanup:
    if (alarm != None) {
        (void)XSyncDestroyAlarm(display, alarm);
    }
    if (sample_output != -1) {
        close(sample_output);
    }
    if (sample_process > 0) {
        kill(sample_process, SIGTERM);
        waitpid(sample_process, NULL, 0);
    }
    if (root_redirected) {
        XSelectInput(
            display,
            DefaultRootWindow(display),
            SubstructureNotifyMask);
    }
    (void)set_wm_extended_sync_support(display, false);
    XSync(display, False);
    return passed;
}

static bool read_notification_events(
    int descriptor,
    size_t *notification_count)
{
    char buffer[32];
    ssize_t length = read(descriptor, buffer, sizeof(buffer));
    ssize_t index;

    if (length <= 0) {
        return false;
    }
    for (index = 0; index < length; index++) {
        if (buffer[index] != 'N') {
            return false;
        }
        (*notification_count)++;
    }
    return true;
}

static bool wait_for_notification_processing(
    Display *display,
    Window window,
    int process_output,
    int service_output,
    size_t expected_notification_count,
    size_t *notification_count)
{
    static const char expected_title[] = "notification-processed";
    size_t attempt;

    for (attempt = 0; attempt < 100; attempt++) {
        struct pollfd descriptors[3];
        char *title = NULL;
        bool title_matches = false;
        int result;

        XSync(display, False);
        if (XFetchName(display, window, &title) != 0 && title != NULL) {
            title_matches = strcmp(title, expected_title) == 0;
            XFree(title);
        }
        if (title_matches &&
            *notification_count == expected_notification_count) {
            descriptors[0].fd = service_output;
            descriptors[0].events = POLLIN;
            descriptors[0].revents = 0;
            result = poll(descriptors, 1, 0);
            if (result == 0) {
                return true;
            }
            if (result == 1 &&
                (descriptors[0].revents & POLLIN) != 0 &&
                read_notification_events(
                    service_output,
                    notification_count)) {
                return *notification_count == expected_notification_count;
            }
            return false;
        }
        if (*notification_count > expected_notification_count) {
            return false;
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == DestroyNotify &&
                event.xdestroywindow.window == window) {
                return false;
            }
        }
        descriptors[0].fd = ConnectionNumber(display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = process_output;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        descriptors[2].fd = service_output;
        descriptors[2].events = POLLIN;
        descriptors[2].revents = 0;
        result = poll(descriptors, 3, 100);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if ((descriptors[1].revents & (POLLHUP | POLLERR)) != 0 ||
            (descriptors[2].revents & (POLLHUP | POLLERR)) != 0) {
            return false;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            char buffer[256];

            if (read(process_output, buffer, sizeof(buffer)) <= 0) {
                return false;
            }
        }
        if ((descriptors[2].revents & POLLIN) != 0 &&
            !read_notification_events(
                service_output,
                notification_count)) {
            return false;
        }
    }
    return false;
}

static bool run_notification_policy(
    Display *display,
    const char *sample_path,
    const char *helper_path,
    const char *display_name,
    int service_output)
{
    char *helper_arguments[2];
    size_t notification_count = 0;
    size_t run_index;

    helper_arguments[0] = (char *)helper_path;
    helper_arguments[1] = NULL;
    for (run_index = 0; run_index < 2; run_index++) {
        bool allow_notifications = run_index == 1;
        pid_t sample_process = -1;
        int sample_output = -1;
        Window window = None;
        int sample_status = 0;
        bool run_passed = false;

        sample_process = start_sample(
            sample_path,
            helper_arguments,
            display_name,
            "xterm-256color",
            allow_notifications,
            &sample_output);
        if (sample_process == -1 ||
            !wait_for_window(display, sample_output, &window) ||
            !send_key(display, window, (test_key){XK_n, 0}) ||
            !wait_for_notification_processing(
                display,
                window,
                sample_output,
                service_output,
                allow_notifications ? 1 : 0,
                &notification_count) ||
            !send_key(display, window, (test_key){XK_q, 0}) ||
            !wait_for_process_exit(sample_output) ||
            waitpid(sample_process, &sample_status, 0) != sample_process) {
            goto run_cleanup;
        }
        sample_process = -1;
        run_passed = WIFEXITED(sample_status) &&
                     WEXITSTATUS(sample_status) == 0;

run_cleanup:
        if (sample_output != -1) {
            close(sample_output);
        }
        if (sample_process > 0) {
            kill(sample_process, SIGTERM);
            waitpid(sample_process, NULL, 0);
        }
        XSync(display, False);
        if (!run_passed) {
            return false;
        }
    }
    return notification_count == 1;
}

static bool create_fixture(char *path, size_t path_capacity)
{
    int descriptor;
    size_t line;

    if (snprintf(path, path_capacity, "/tmp/boxrig-cui-XXXXXX") < 0) {
        return false;
    }
    descriptor = mkstemp(path);
    if (descriptor == -1) {
        return false;
    }
    for (line = 1; line <= 80; line++) {
        if (dprintf(
                descriptor,
                "boxrig interactive terminal line %zu\n",
                line) < 0) {
            close(descriptor);
            unlink(path);
            return false;
        }
    }
    if (close(descriptor) == -1) {
        unlink(path);
        return false;
    }
    return true;
}

static bool run_color_filter_demo(
    Display *display, const char *sample_path, const char *helper_path,
    const char *display_name)
{
    char *command[] = {(char *)helper_path, NULL};
    const test_key toggle = {XK_F12, ControlMask | ShiftMask};
    const test_key quit = {XK_q, 0};
    Window window = None;
    int output = -1;
    int status = 0;
    bool passed = false;
    const char *stage = "initial frame";
    if (setenv("BOXRIG_COLOR_FILTER_DEMO", "1", 1) != 0) return false;
    pid_t process = start_sample(sample_path, command, display_name,
        "xterm-256color", false, &output);
    unsetenv("BOXRIG_COLOR_FILTER_DEMO");
    if (process < 0) return false;
    stage = "mapped window";
    if (!wait_for_window(display, output, &window)) goto cleanup;
    XWindowAttributes attributes;
    if (!XGetWindowAttributes(display, window, &attributes)) goto cleanup;
    stage = "grid query";
    /* Querying the grid also verifies that terminal input still works. */
    if (!send_key(display, window, (test_key){XK_g, 0}) ||
        !wait_for_synchronized_frame_title(display, window, output, "grid-24-80"))
        goto cleanup;
    XImage *pixel = XGetImage(display, window, 0, attributes.height - 1,
        1, 1, AllPlanes, ZPixmap);
    if (pixel == NULL) goto cleanup;
    bool corrected = (XGetPixel(pixel, 0, 0) & 0xffffffu) == 0x404040;
    XDestroyImage(pixel);
    if (!corrected) {
        fprintf(stderr, "sample color demo did not correct blank cell background\n");
        goto cleanup;
    }
    uint64_t filtered, plain, observed;
    stage = "disable filter";
    if (!read_window_hash(display, window, (unsigned int)attributes.width,
            (unsigned int)attributes.height, &filtered) ||
        !send_key_transition(display, window, toggle, KeyPress) ||
        !send_key_transition(display, window, toggle, KeyRelease) ||
        !wait_for_window_hash(display, window, output,
            (unsigned int)attributes.width, (unsigned int)attributes.height,
            filtered, false, &plain)) goto cleanup;
    stage = "identity filter";
    if (!send_key_transition(display, window, toggle, KeyPress) ||
        !send_key_transition(display, window, toggle, KeyRelease) ||
        XStoreName(display, window, "awaiting-color-grid") == 0 ||
        !send_key(display, window, (test_key){XK_g, 0}) ||
        !wait_for_synchronized_frame_title(display, window, output, "grid-24-80") ||
        !wait_for_window_hash(display, window, output,
            (unsigned int)attributes.width, (unsigned int)attributes.height,
            plain, true, &observed)) goto cleanup;
    stage = "restore filter";
    if (!send_key_transition(display, window, toggle, KeyPress) ||
        !send_key_transition(display, window, toggle, KeyRelease) ||
        !wait_for_window_hash(display, window, output,
            (unsigned int)attributes.width, (unsigned int)attributes.height,
            filtered, true, &observed) || !send_key(display, window, quit) ||
        !wait_for_process_exit(output) || waitpid(process, &status, 0) != process)
        goto cleanup;
    process = -1;
    passed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
cleanup:
    if (!passed) fprintf(stderr, "color demo failed at %s\n", stage);
    close(output);
    if (process > 0) { kill(process, SIGTERM); waitpid(process, NULL, 0); }
    XSync(display, False);
    return passed;
}

int main(int argument_count, char **arguments)
{
    char display_name[40];
    char fixture_path[64];
    pid_t xvfb_process;
    pid_t notification_service_process = -1;
    int notification_service_output = -1;
    Display *display = NULL;
    bool passed = false;
    char *helper_arguments[2];
    char *less_arguments[4];
    char *vim_arguments[7];
    const test_key quit_key[] = {{XK_q, 0}};
    const test_key vim_quit_keys[] = {
        {XK_Z, ShiftMask},
        {XK_Q, ShiftMask},
    };
    static const char *terms[] = {"xterm", "xterm-256color"};
    size_t term_index;
    boxrig_test_video video = {0};

    if (argument_count != 5 &&
        !(argument_count == 6 && (strcmp(arguments[5], "--alternate-resize") == 0 ||
            strcmp(arguments[5], "--color-demo") == 0 ||
            strcmp(arguments[5], "--wheel-zoom") == 0))) {
        fprintf(
            stderr,
            "usage: %s SAMPLE PTY_HELPER FAKE_NOTIFICATION_SERVICE EMITTER\n",
            arguments[0]);
        return 2;
    }
    notification_service_process = start_fake_notification_service(
        arguments[3],
        &notification_service_output);
    if (notification_service_process == -1) {
        perror("start fake notification service");
        return 1;
    }
    if (!create_fixture(fixture_path, sizeof(fixture_path))) {
        perror("create fixture");
        close(notification_service_output);
        kill(notification_service_process, SIGTERM);
        waitpid(notification_service_process, NULL, 0);
        return 1;
    }
    xvfb_process = start_xvfb(display_name, sizeof(display_name));
    if (xvfb_process == -1) {
        perror("start Xvfb");
        unlink(fixture_path);
        close(notification_service_output);
        kill(notification_service_process, SIGTERM);
        waitpid(notification_service_process, NULL, 0);
        return 1;
    }
    display = XOpenDisplay(display_name);
    if (display == NULL) {
        fprintf(stderr, "could not open Xvfb display %s\n", display_name);
        goto cleanup;
    }
    XSelectInput(display, DefaultRootWindow(display), SubstructureNotifyMask);
    XSync(display, False);
    if (!boxrig_test_video_start(&video, display_name, "3840x2160")) goto cleanup;
    if (argument_count == 6 && strcmp(arguments[5], "--wheel-zoom") == 0) {
        helper_arguments[0] = arguments[2];
        helper_arguments[1] = NULL;
        passed = run_zoom_shortcuts(display, arguments[1], helper_arguments,
            display_name, true);
        goto cleanup;
    }
    if (argument_count == 6) {
        passed = strcmp(arguments[5], "--color-demo") == 0
            ? run_color_filter_demo(display, arguments[1], arguments[2], display_name)
            : run_alternate_resize(display, arguments[1], arguments[2], display_name);
        goto cleanup;
    }
    if (unsetenv("BOXRIG_BACKGROUND_OPACITY") == -1) {
        perror("clear background opacity");
        goto cleanup;
    }
    helper_arguments[0] = arguments[2];
    helper_arguments[1] = NULL;
    less_arguments[0] = "/usr/bin/less";
    less_arguments[1] = "-R";
    less_arguments[2] = fixture_path;
    less_arguments[3] = NULL;
    vim_arguments[0] = "/usr/bin/vi";
    vim_arguments[1] = "-Nu";
    vim_arguments[2] = "NONE";
    vim_arguments[3] = "-n";
    vim_arguments[4] = fixture_path;
    vim_arguments[5] = NULL;
    if (!rejects_invalid_terminal_type(
            arguments[1],
            arguments[2],
            display_name)) {
        fprintf(stderr, "X11 sample accepted an invalid BOXRIG_TERM value\n");
        goto cleanup;
    }
    if (!run_background_opacity_policy(
            display,
            arguments[1],
            arguments[2],
            display_name)) {
        fprintf(stderr, "X11 sample background opacity policy failed\n");
        goto cleanup;
    }
    if (!run_host_input_policy(
            display,
            arguments[1],
            arguments[2],
            display_name)) {
        fprintf(stderr, "X11 sample host input policy failed\n");
        goto cleanup;
    }
    if (!run_input_response_fixture(
            display,
            arguments[1],
            arguments[2],
            display_name)) {
        fprintf(stderr, "input response sequence was not visible in order\n");
        goto cleanup;
    }
    if (!run_scrollbar_viewport(
            display,
            arguments[1],
            arguments[2],
            display_name)) {
        fprintf(stderr, "X11 scrollbar did not navigate terminal history\n");
        goto cleanup;
    }
    if (!run_zoom_shortcuts(
            display,
            arguments[1],
            helper_arguments,
            display_name, false)) {
        fprintf(stderr, "font zoom shortcuts did not resize and reset X11\n");
        goto cleanup;
    }
    if (!run_zoom_shortcuts(display, arguments[1], helper_arguments,
            display_name, true)) {
        fprintf(stderr, "Ctrl+wheel did not use the existing X11 zoom and reset path\n");
        goto cleanup;
    }
    if (!run_color_filter_demo(display, arguments[1], arguments[2], display_name)) goto cleanup;
    if (!run_alternate_resize(display, arguments[1], arguments[2], display_name)) goto cleanup;
    if (!run_synchronized_frame_resize(
            display,
            arguments[1],
            arguments[2],
            display_name)) {
        fprintf(
            stderr,
            "synchronized frame did not survive the X11 resize burst\n");
        goto cleanup;
    }
    if (!run_wm_sync_resize(
            display,
            arguments[1],
            arguments[2],
            display_name)) {
        fprintf(stderr, "X11 WM sync resize protocol failed\n");
        goto cleanup;
    }
    if (!run_resize_increment_contract(
            display,
            arguments[1],
            arguments[2],
            display_name)) {
        fprintf(stderr, "X11 resize increment contract failed\n");
        goto cleanup;
    }
    if (!run_extended_wm_sync_resize(
            display,
            arguments[1],
            arguments[2],
            display_name)) {
        fprintf(stderr, "X11 extended WM frame protocol failed\n");
        goto cleanup;
    }
    if (!run_notification_policy(
            display,
            arguments[1],
            arguments[2],
            display_name,
            notification_service_output)) {
        fprintf(stderr, "notification opt-in policy did not reach fake D-Bus\n");
        goto cleanup;
    }
    if (!run_emitter_sample_cases(
            display,
            arguments[1],
            arguments[4],
            display_name,
            notification_service_output)) {
        fprintf(stderr, "boxrig_emitter did not complete X11 sample cases\n");
        goto cleanup;
    }
    for (term_index = 0;
         term_index < sizeof(terms) / sizeof(terms[0]);
         term_index++) {
        if (!run_sample_command(
                display,
                arguments[1],
                helper_arguments,
                display_name,
                terms[term_index],
                quit_key,
                sizeof(quit_key) / sizeof(quit_key[0]),
                false)) {
            fprintf(
                stderr,
                "PTY helper did not complete with TERM=%s\n",
                terms[term_index]);
            goto cleanup;
        }
        if (!run_sample_command(
                display,
                arguments[1],
                less_arguments,
                display_name,
                terms[term_index],
                quit_key,
                sizeof(quit_key) / sizeof(quit_key[0]),
                true)) {
            fprintf(
                stderr,
                "less did not render and exit with TERM=%s\n",
                terms[term_index]);
            goto cleanup;
        }
        if (!run_sample_command(
                display,
                arguments[1],
                vim_arguments,
                display_name,
                terms[term_index],
                vim_quit_keys,
                sizeof(vim_quit_keys) / sizeof(vim_quit_keys[0]),
                true)) {
            fprintf(
                stderr,
                "Vim did not render and exit with TERM=%s\n",
                terms[term_index]);
            goto cleanup;
        }
    }
    passed = true;

cleanup:
    unlink(fixture_path);
    if (!boxrig_test_video_stop(&video)) passed = false;
    if (display != NULL) {
        XCloseDisplay(display);
    }
    kill(xvfb_process, SIGTERM);
    waitpid(xvfb_process, NULL, 0);
    close(notification_service_output);
    kill(notification_service_process, SIGTERM);
    waitpid(notification_service_process, NULL, 0);
    if (!passed) {
        return 1;
    }
    printf(
        "PASS X11 sample emitter notifications zoom and terminal programs\n");
    return 0;
}
