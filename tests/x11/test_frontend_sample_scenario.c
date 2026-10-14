/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/sync.h>
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

#include "../frontend_sample_scenario.h"
#include "../x11_video.h"

#define TEST_TIMEOUT_MILLISECONDS UINT64_C(15000)

typedef struct sample_process {
    pid_t identifier;
    int output;
    char diagnostics[8192];
    size_t diagnostics_length;
} sample_process;

static int last_x11_error;
static Window held_clipboard_owner;
static XSelectionRequestEvent held_clipboard_request;
static bool held_clipboard_requested;
static bool held_clipboard_released;
static bool held_clipboard_may_cancel;

static int record_x11_error(Display *display, XErrorEvent *event)
{
    (void)display;
    last_x11_error = event->error_code;
    return 0;
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1 || now.tv_sec < 0) {
        return 0u;
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

static bool drain_output(sample_process *process)
{
    char bytes[512];

    for (;;) {
        ssize_t count = read(process->output, bytes, sizeof(bytes));

        if (count > 0) {
            size_t available = sizeof(process->diagnostics) -
                               process->diagnostics_length - 1u;
            size_t copied = (size_t)count < available
                                ? (size_t)count
                                : available;

            memcpy(
                process->diagnostics + process->diagnostics_length,
                bytes,
                copied);
            process->diagnostics_length += copied;
            process->diagnostics[process->diagnostics_length] = '\0';
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        return count == 0 || errno == EAGAIN || errno == EWOULDBLOCK;
    }
}

static bool poll_sample(
    Display *display,
    sample_process *process,
    int timeout)
{
    struct pollfd descriptors[2];
    int result;

    /* A synchronous property query may have read the acknowledgement event
     * into Xlib already. Do not wait for another socket byte in that case,
     * but still drain the child's pipe to advance teardown handshakes. */
    if (XPending(display) > 0) {
        timeout = 0;
    }
    descriptors[0].fd = ConnectionNumber(display);
    descriptors[0].events = POLLIN;
    descriptors[0].revents = 0;
    descriptors[1].fd = process->output;
    descriptors[1].events = POLLIN;
    descriptors[1].revents = 0;
    do {
        result = poll(descriptors, 2, timeout);
    } while (result == -1 && errno == EINTR);
    if (result < 0) {
        return false;
    }
    if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0 &&
        !drain_output(process)) {
        return false;
    }
    return (descriptors[1].revents & (POLLHUP | POLLERR)) == 0;
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

static bool reply_to_held_clipboard_request(
    Display *display, const XSelectionRequestEvent *request, bool release_text)
{
    Atom targets = XInternAtom(display, "TARGETS", False);
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    Atom plain_utf8 = XInternAtom(display, "text/plain;charset=utf-8", False);
    Atom plain = XInternAtom(display, "text/plain", False);
    Atom property = request->property != None ? request->property : request->target;
    XEvent reply;
    int previous_error = last_x11_error;
    bool sent;

    last_x11_error = 0;
    memset(&reply, 0, sizeof(reply));
    reply.xselection.type = SelectionNotify;
    reply.xselection.display = display;
    reply.xselection.requestor = request->requestor;
    reply.xselection.selection = request->selection;
    reply.xselection.target = request->target;
    reply.xselection.time = request->time;
    if (request->target == targets) {
        Atom formats[] = {targets, utf8, plain_utf8, plain};
        XChangeProperty(display, request->requestor, property, XA_ATOM, 32,
                        PropModeReplace, (const unsigned char *)formats, 4);
        reply.xselection.property = property;
    } else if (request->target == utf8 || request->target == plain_utf8 ||
               request->target == plain) {
        if (!release_text) {
            if (held_clipboard_requested) {
                last_x11_error = previous_error;
                return false;
            }
            held_clipboard_request = *request;
            held_clipboard_requested = true;
            last_x11_error = previous_error;
            return true;
        }
        XChangeProperty(display, request->requestor, property, request->target, 8,
                        PropModeReplace, (const unsigned char *)"late-paste", 10);
        reply.xselection.property = property;
    }
    sent = XSendEvent(display, request->requestor, False, NoEventMask, &reply) != 0;
    XSync(display, False);
    if (release_text && held_clipboard_may_cancel) {
        fprintf(stderr, "GTK4 selection reply diagnostic: requestor=%lu target=%lu sent=%d error=%d\n",
                request->requestor, request->target, sent, last_x11_error);
    }
    /* GTK 4 may finish cancellation and close its requestor after the audit
     * checkpoint but before this reply. That is not successful delivery. */
    sent = sent && (last_x11_error == 0 ||
        (release_text && held_clipboard_may_cancel && last_x11_error == BadWindow));
    last_x11_error = previous_error;
    return sent;
}

static bool process_window_events(
    Display *display,
    Window root,
    Window window,
    Window *mapped_out)
{
    while (XPending(display) > 0) {
        XEvent event;

        XNextEvent(display, &event);
        if (event.type == SelectionRequest && held_clipboard_owner != None &&
            event.xselectionrequest.owner == held_clipboard_owner) {
            /* GDK X11 can try another target after a cancelled read finishes.
             * Keep the external owner serving only after teardown released it. */
            if (!reply_to_held_clipboard_request(display, &event.xselectionrequest,
                    held_clipboard_may_cancel && held_clipboard_released)) {
                return false;
            }
        } else if (event.type == ConfigureRequest &&
            event.xconfigurerequest.parent == root) {
            if (!apply_configure_request(display, &event.xconfigurerequest)) {
                return false;
            }
        } else if (event.type == MapRequest &&
                   event.xmaprequest.parent == root) {
            XMapWindow(display, event.xmaprequest.window);
            XSync(display, False);
            if (mapped_out != NULL && *mapped_out == None) {
                *mapped_out = event.xmaprequest.window;
            }
        } else if (window != None && event.type == DestroyNotify &&
                   event.xdestroywindow.window == window) {
            return false;
        }
    }
    return true;
}

static bool start_sample(
    const char *sample_path,
    const char *helper_path,
    const char *pending_clipboard,
    const char *helper_mode,
    sample_process *process_out)
{
    int output_pipe[2];
    pid_t identifier;
    const char *test_im_module = getenv("BOXRIG_TEST_GTK4_IM_MODULE");
    const char *renderer = getenv("BOXRIG_TEST_SAMPLE_RENDERER");

    memset(process_out, 0, sizeof(*process_out));
    process_out->identifier = -1;
    process_out->output = -1;
    if (pipe(output_pipe) == -1) {
        return false;
    }
    identifier = fork();
    if (identifier == -1) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        return false;
    }
    if (identifier == 0) {
        close(output_pipe[0]);
        if (setpgid(0, 0) == -1 ||
            dup2(output_pipe[1], STDOUT_FILENO) == -1 ||
            dup2(output_pipe[1], STDERR_FILENO) == -1 ||
            setenv("BOXRIG_TERM", "xterm-256color", 1) == -1 ||
            setenv("GDK_BACKEND", "x11", 1) == -1 ||
            setenv("GSK_RENDERER", "cairo", 1) == -1 ||
            (test_im_module != NULL &&
             setenv("GTK_IM_MODULE", test_im_module, 1) == -1) ||
            setenv("BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD", pending_clipboard, 1) == -1 ||
            setenv("NO_AT_BRIDGE", "1", 1) == -1 ||
            unsetenv("BOXRIG_BACKGROUND_OPACITY") == -1) {
            _exit(127);
        }
        close(output_pipe[1]);
        if (renderer != NULL && renderer[0] != '\0') {
            execl(sample_path, sample_path, renderer, "--", helper_path,
                  helper_mode, (char *)NULL);
            _exit(127);
        }
        execl(
            sample_path,
            sample_path,
            "--",
            helper_path,
            helper_mode,
            (char *)NULL);
        _exit(127);
    }
    close(output_pipe[1]);
    if ((setpgid(identifier, identifier) == -1 && errno != EACCES &&
         errno != ESRCH) ||
        fcntl(output_pipe[0], F_SETFL, O_NONBLOCK) == -1) {
        close(output_pipe[0]);
        kill(-identifier, SIGTERM);
        waitpid(identifier, NULL, 0);
        return false;
    }
    process_out->identifier = identifier;
    process_out->output = output_pipe[0];
    return true;
}

static bool wait_for_window(
    Display *display,
    Window root,
    sample_process *process,
    Window *window_out)
{
    /* Instrumented GTK startup can include expensive native icon/font loading. */
    uint64_t deadline = monotonic_milliseconds() + UINT64_C(120000);

    *window_out = None;
    while (*window_out == None) {
        int timeout = remaining_timeout(deadline);

        if (timeout == 0 ||
            !process_window_events(
                display,
                root,
                None,
                window_out) ||
            (*window_out == None &&
             !poll_sample(display, process, timeout))) {
            return false;
        }
    }
    XSelectInput(
        display,
        *window_out,
        PropertyChangeMask | StructureNotifyMask | ExposureMask);
    XSync(display, False);
    return true;
}

static bool read_title(Window window, Display *display, char *title, size_t size)
{
    char *native_title = NULL;
    size_t length;
    int fetched;

    last_x11_error = Success;
    fetched = XFetchName(display, window, &native_title);
    XSync(display, False);
    if (last_x11_error != Success || fetched == 0 ||
        native_title == NULL) {
        if (native_title != NULL) {
            XFree(native_title);
        }
        return false;
    }
    length = strlen(native_title);
    if (length >= size) {
        XFree(native_title);
        return false;
    }
    memcpy(title, native_title, length + 1u);
    XFree(native_title);
    return true;
}

static bool wait_for_title(
    Display *display,
    Window root,
    Window window,
    sample_process *process,
    const char *expected)
{
    uint64_t deadline = monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;

    for (;;) {
        char title[128] = "";
        int timeout = remaining_timeout(deadline);

        if (!process_window_events(display, root, window, NULL)) {
            return false;
        }
        if (read_title(window, display, title, sizeof(title)) &&
            strcmp(title, expected) == 0) {
            return true;
        }
        if (strncmp(
                title,
                BOXRIG_TEST_FRONTEND_SAMPLE_UNEXPECTED_PREFIX,
                strlen(BOXRIG_TEST_FRONTEND_SAMPLE_UNEXPECTED_PREFIX)) == 0) {
            fprintf(stderr, "controlled child reported %s\n", title);
            return false;
        }
        if (timeout == 0 ||
            !poll_sample(display, process, timeout)) {
            return false;
        }
    }
}

static bool wait_for_ready_title(
    Display *display,
    Window root,
    Window window,
    sample_process *process,
    unsigned int *cell_height_out,
    unsigned int *cell_width_out)
{
    uint64_t deadline = monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;
    size_t prefix_length = strlen(BOXRIG_TEST_FRONTEND_SAMPLE_READY_PREFIX);

    for (;;) {
        char title[128];
        int parsed_length = 0;
        int timeout = remaining_timeout(deadline);

        if (!process_window_events(display, root, window, NULL)) {
            return false;
        }
        if (read_title(window, display, title, sizeof(title)) &&
            strncmp(
                title,
                BOXRIG_TEST_FRONTEND_SAMPLE_READY_PREFIX,
                prefix_length) == 0 &&
            sscanf(
                title + prefix_length,
                "%u-%u%n",
                cell_height_out,
                cell_width_out,
                &parsed_length) == 2 &&
            parsed_length > 0 &&
            title[prefix_length + (size_t)parsed_length] == '\0' &&
            *cell_height_out != 0u && *cell_width_out != 0u) {
            return true;
        }
        if (timeout == 0 ||
            !poll_sample(display, process, timeout)) {
            return false;
        }
    }
}

static bool window_has_semantic_pixels(Display *display, Window window)
{
    XWindowAttributes attributes;
    XImage *image;
    unsigned long first;
    int x;
    int y;
    bool differs = false;

    if (XGetWindowAttributes(display, window, &attributes) == 0 ||
        attributes.width <= 1 || attributes.height <= 1) {
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
    first = XGetPixel(image, 0, 0);
    for (y = 0; y < attributes.height && !differs; y += 2) {
        for (x = 0; x < attributes.width; x += 2) {
            if (XGetPixel(image, x, y) != first) {
                differs = true;
                break;
            }
        }
    }
    XDestroyImage(image);
    return differs;
}

static bool wait_for_semantic_pixels(
    Display *display,
    Window root,
    Window window,
    sample_process *process)
{
    uint64_t deadline = monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;

    for (;;) {
        int timeout = remaining_timeout(deadline);

        if (!process_window_events(display, root, window, NULL)) {
            return false;
        }
        XSync(display, False);
        if (window_has_semantic_pixels(display, window)) {
            return true;
        }
        if (timeout == 0 ||
            !poll_sample(display, process, timeout < 20 ? timeout : 20)) {
            return false;
        }
    }
}

static bool read_frame_extents(
    Display *display,
    Window window,
    unsigned long *left_out,
    unsigned long *right_out,
    unsigned long *top_out,
    unsigned long *bottom_out)
{
    Atom frame_extents = XInternAtom(display, "_GTK_FRAME_EXTENTS", False);
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0u;
    unsigned long bytes_after = 0u;
    unsigned char *property = NULL;
    bool valid;

    if (frame_extents == None ||
        XGetWindowProperty(
            display,
            window,
            frame_extents,
            0,
            4,
            False,
            XA_CARDINAL,
            &actual_type,
            &actual_format,
            &item_count,
            &bytes_after,
            &property) != Success) {
        return false;
    }
    valid = actual_type == XA_CARDINAL && actual_format == 32 &&
            item_count == 4u && bytes_after == 0u && property != NULL;
    if (valid) {
        const unsigned long *extents = (const unsigned long *)property;

        *left_out = extents[0];
        *right_out = extents[1];
        *top_out = extents[2];
        *bottom_out = extents[3];
    }
    if (property != NULL) {
        XFree(property);
    }
    return valid;
}

static bool locate_terminal_viewport(
    Display *display,
    Window window,
    unsigned int grid_width,
    unsigned int grid_height,
    int *x_out,
    int *y_out)
{
    XWindowAttributes attributes;
    XImage *image;
    int best_horizontal_excess = INT_MAX;
    int best_vertical_excess = INT_MAX;
    int best_x = -1;
    int best_y = -1;
    int x;
    int y;

    if (XGetWindowAttributes(display, window, &attributes) == 0 ||
        attributes.width < (int)grid_width ||
        attributes.height < (int)grid_height) {
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
    for (y = 0; y < attributes.height; y++) {
        x = 0;
        while (x < attributes.width) {
            unsigned long pixel = XGetPixel(image, x, y);
            int end = x + 1;
            int excess;

            while (end < attributes.width &&
                   XGetPixel(image, end, y) == pixel) {
                end++;
            }
            excess = end - x - (int)grid_width;
            if (excess >= 0 && excess < best_horizontal_excess) {
                best_horizontal_excess = excess;
                best_x = x;
            }
            x = end;
        }
    }
    if (best_x >= 0) {
        x = best_x + (int)grid_width / 2;
        y = 0;
        while (y < attributes.height) {
            unsigned long pixel = XGetPixel(image, x, y);
            int end = y + 1;
            int excess;

            while (end < attributes.height &&
                   XGetPixel(image, x, end) == pixel) {
                end++;
            }
            excess = end - y - (int)grid_height;
            if (excess >= 0 && excess < best_vertical_excess) {
                best_vertical_excess = excess;
                best_y = y;
            }
            y = end;
        }
    }
    XDestroyImage(image);
    if (best_x < 0 || best_y < 0) {
        return false;
    }
    *x_out = best_x;
    *y_out = best_y;
    return true;
}

static bool send_key(
    Display *display,
    Window window,
    KeySym symbol,
    bool control,
    bool shift)
{
    KeyCode control_code = XKeysymToKeycode(display, XK_Control_L);
    KeyCode shift_code = XKeysymToKeycode(display, XK_Shift_L);
    KeyCode key_code = XKeysymToKeycode(display, symbol);

    if (key_code == 0 || (control && control_code == 0) ||
        (shift && shift_code == 0)) {
        return false;
    }
    XSetInputFocus(display, window, RevertToParent, CurrentTime);
    if ((control &&
         XTestFakeKeyEvent(display, control_code, True, CurrentTime) == 0) ||
        (shift &&
         XTestFakeKeyEvent(display, shift_code, True, CurrentTime) == 0) ||
        XTestFakeKeyEvent(display, key_code, True, CurrentTime) == 0 ||
        XTestFakeKeyEvent(display, key_code, False, CurrentTime) == 0 ||
        (shift &&
         XTestFakeKeyEvent(display, shift_code, False, CurrentTime) == 0) ||
        (control &&
         XTestFakeKeyEvent(display, control_code, False, CurrentTime) == 0)) {
        return false;
    }
    XSync(display, False);
    return true;
}

static bool send_drag(
    Display *display,
    Window window,
    int origin_x,
    int origin_y,
    int start_x,
    int start_y,
    int end_x,
    int end_y,
    KeySym modifier)
{
    Window child;
    int root_x;
    int root_y;
    KeyCode modifier_code = modifier == NoSymbol
                                ? 0
                                : XKeysymToKeycode(display, modifier);

    if ((modifier != NoSymbol && modifier_code == 0) ||
        XTranslateCoordinates(
            display,
            window,
            DefaultRootWindow(display),
            0,
            0,
            &root_x,
            &root_y,
            &child) == 0) {
        return false;
    }
    if (XTestFakeMotionEvent(
            display,
            DefaultScreen(display),
            root_x + origin_x + start_x,
            root_y + origin_y + start_y,
            CurrentTime) == 0) {
        return false;
    }
    XSync(display, False);
    if (modifier_code != 0) {
        if (XTestFakeKeyEvent(
                display,
                modifier_code,
                True,
                CurrentTime) == 0) {
            return false;
        }
        XSync(display, False);
    }
    if (XTestFakeButtonEvent(display, Button1, True, CurrentTime) == 0) {
        return false;
    }
    XSync(display, False);
    if ((start_x != end_x || start_y != end_y) &&
        XTestFakeMotionEvent(
            display,
            DefaultScreen(display),
            root_x + origin_x + end_x,
            root_y + origin_y + end_y,
            CurrentTime) == 0) {
        return false;
    }
    XSync(display, False);
    if (XTestFakeButtonEvent(display, Button1, False, CurrentTime) == 0) {
        return false;
    }
    XSync(display, False);
    if (modifier_code != 0) {
        if (XTestFakeKeyEvent(
                display,
                modifier_code,
                False,
                CurrentTime) == 0) {
            return false;
        }
        XSync(display, False);
    }
    return true;
}

static bool clear_selection(Display *display, Atom selection)
{
    XSetSelectionOwner(display, selection, None, CurrentTime);
    XSync(display, False);
    return XGetSelectionOwner(display, selection) == None;
}

static bool wait_for_selection(
    Display *display,
    Window root,
    Window window,
    sample_process *process,
    Atom selection)
{
    uint64_t deadline = monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;

    for (;;) {
        int timeout = remaining_timeout(deadline);

        if (!process_window_events(display, root, window, NULL)) {
            return false;
        }
        if (XGetSelectionOwner(display, selection) != None) {
            return true;
        }
        if (timeout == 0 ||
            !poll_sample(display, process, timeout)) {
            return false;
        }
    }
}

static bool wait_for_selection_text(
    Display *display,
    Window root,
    Window sample_window,
    sample_process *process,
    Atom selection,
    const char *expected)
{
    Atom utf8_string = XInternAtom(display, "UTF8_STRING", False);
    Atom property = XInternAtom(
        display,
        "_BOXRIG_FRONTEND_SAMPLE_SELECTION",
        False);
    Window requestor = XCreateSimpleWindow(
        display,
        root,
        0,
        0,
        1u,
        1u,
        0u,
        0u,
        0u);
    uint64_t deadline = monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;
    bool matched = false;
    bool complete = false;

    if (utf8_string == None || property == None || requestor == None) {
        if (requestor != None) {
            XDestroyWindow(display, requestor);
        }
        return false;
    }
    XConvertSelection(
        display,
        selection,
        utf8_string,
        property,
        requestor,
        CurrentTime);
    XFlush(display);
    while (!complete) {
        while (XPending(display) > 0 && !complete) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == ConfigureRequest &&
                event.xconfigurerequest.parent == root) {
                apply_configure_request(display, &event.xconfigurerequest);
            } else if (event.type == MapRequest &&
                       event.xmaprequest.parent == root) {
                XMapWindow(display, event.xmaprequest.window);
                XFlush(display);
            } else if (event.type == DestroyNotify &&
                       event.xdestroywindow.window == sample_window) {
                complete = true;
            } else if (event.type == SelectionNotify &&
                       event.xselection.requestor == requestor &&
                       event.xselection.selection == selection &&
                       event.xselection.target == utf8_string) {
                Atom actual_type = None;
                int actual_format = 0;
                unsigned long item_count = 0u;
                unsigned long bytes_after = 0u;
                unsigned char *bytes = NULL;
                size_t expected_length = strlen(expected);

                if (event.xselection.property == property &&
                    XGetWindowProperty(
                        display,
                        requestor,
                        property,
                        0,
                        4096,
                        True,
                        AnyPropertyType,
                        &actual_type,
                        &actual_format,
                        &item_count,
                        &bytes_after,
                        &bytes) == Success &&
                    actual_type == utf8_string && actual_format == 8 &&
                    bytes_after == 0u && item_count == expected_length &&
                    bytes != NULL &&
                    memcmp(bytes, expected, expected_length) == 0) {
                    matched = true;
                } else if (bytes != NULL) {
                    fprintf(
                        stderr,
                        "unexpected selection payload (%lu bytes): %.*s\n",
                        item_count,
                        (int)item_count,
                        (const char *)bytes);
                }
                if (bytes != NULL) {
                    XFree(bytes);
                }
                complete = true;
            }
        }
        if (!complete) {
            int timeout = remaining_timeout(deadline);

            if (timeout == 0 ||
                !poll_sample(
                    display,
                    process,
                    timeout < 20 ? timeout : 20)) {
                complete = true;
            }
        }
    }
    XDestroyWindow(display, requestor);
    XSync(display, False);
    return matched;
}

static bool copy_selection_and_paste(
    Display *display,
    Window root,
    Window window,
    sample_process *process,
    const char *expected)
{
    Atom clipboard = XInternAtom(display, "CLIPBOARD", False);

    if (clipboard == None || !clear_selection(display, clipboard)) {
        fprintf(stderr, "failed to clear CLIPBOARD\n");
        return false;
    }
    if (!send_key(display, window, XK_c, true, true)) {
        fprintf(stderr, "failed to deliver native copy shortcut\n");
        return false;
    }
    if (!wait_for_selection(
            display,
            root,
            window,
            process,
            clipboard)) {
        fprintf(stderr, "failed to acquire CLIPBOARD ownership\n");
        return false;
    }
    if (!wait_for_selection_text(
            display,
            root,
            window,
            process,
            clipboard,
            expected)) {
        fprintf(stderr, "failed to transfer expected CLIPBOARD payload\n");
        return false;
    }
    if (!send_key(display, window, XK_v, true, true)) {
        fprintf(stderr, "failed to deliver native paste shortcut\n");
        return false;
    }
    return true;
}

static bool wait_for_resize(
    Display *display,
    Window root,
    Window window,
    sample_process *process,
    int previous_width,
    int previous_height)
{
    uint64_t deadline = monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;

    for (;;) {
        XWindowAttributes attributes;
        int timeout = remaining_timeout(deadline);

        if (!process_window_events(display, root, window, NULL) ||
            XGetWindowAttributes(display, window, &attributes) == 0) {
            return false;
        }
        if (attributes.width != previous_width ||
            attributes.height != previous_height) {
            return true;
        }
        if (timeout == 0 ||
            !poll_sample(display, process, timeout)) {
            return false;
        }
    }
}

static bool send_wm_delete(Display *display, Window window)
{
    Atom protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    Atom delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XEvent event;

    if (protocols == None || delete_window == None) {
        return false;
    }
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.display = display;
    event.xclient.window = window;
    event.xclient.message_type = protocols;
    event.xclient.format = 32;
    event.xclient.data.l[0] = (long)delete_window;
    event.xclient.data.l[1] = CurrentTime;
    if (XSendEvent(display, window, False, NoEventMask, &event) == 0) {
        return false;
    }
    XFlush(display);
    return true;
}

static bool wait_for_exit_with_display(
    Display *display,
    sample_process *process)
{
    uint64_t deadline = monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;

    for (;;) {
        int status = 0;
        pid_t result = waitpid(process->identifier, &status, WNOHANG);
        int timeout = remaining_timeout(deadline);

        if (result == process->identifier) {
            process->identifier = -1;
            if (held_clipboard_may_cancel && held_clipboard_requested &&
                WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                /* The probe required real cancellation completion. Discard a
                 * held request whose process has already completed teardown. */
                held_clipboard_released = true;
            }
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (result == -1 || timeout == 0) {
            return false;
        }
        if (held_clipboard_requested && !held_clipboard_released &&
            (strstr(process->diagnostics,
                    "READY sample pending GTK3 clipboard callback after teardown\n") != NULL ||
             strstr(process->diagnostics,
                    "READY sample pending X11 selection before quiesce\n") != NULL ||
             (held_clipboard_may_cancel && strstr(process->diagnostics,
                    "READY sample pending GTK4 clipboard callback after teardown\n") != NULL))) {
            if (!reply_to_held_clipboard_request(display, &held_clipboard_request, true)) {
                return false;
            }
            held_clipboard_released = true;
        }
        if (held_clipboard_may_cancel && held_clipboard_released &&
            !process_window_events(display, DefaultRootWindow(display), None, NULL)) {
            return false;
        }
        if (!poll_sample(
                display,
                process,
                timeout < 20 ? timeout : 20)) {
            do {
                result = waitpid(process->identifier, &status, 0);
            } while (result == -1 && errno == EINTR);
            if (result != process->identifier) {
                return false;
            }
            process->identifier = -1;
            if (held_clipboard_may_cancel && held_clipboard_requested &&
                WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                held_clipboard_released = true;
            }
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
    }
}

static void stop_sample(sample_process *process)
{
    if (process->identifier > 0) {
        kill(-process->identifier, SIGTERM);
        waitpid(process->identifier, NULL, 0);
        process->identifier = -1;
    }
    if (process->output != -1) {
        close(process->output);
        process->output = -1;
    }
}

static bool capture_overlay_region(
    Display *display, Window window, int x, int y,
    unsigned int width, unsigned int height, uint64_t *value_out)
{
    XImage *image = XGetImage(
        display, window, x, y, width, height, AllPlanes, ZPixmap);
    uint64_t hash = UINT64_C(14695981039346656037);
    unsigned int row;
    unsigned int column;

    if (image == NULL) {
        return false;
    }
    for (row = 0u; row < height; row++) {
        for (column = 0u; column < width; column++) {
            hash ^= XGetPixel(image, (int)column, (int)row) & 0xfffffful;
            hash *= UINT64_C(1099511628211);
        }
    }
    *value_out = width == 1u && height == 1u
                     ? XGetPixel(image, 0, 0) & 0xfffffful : hash;
    XDestroyImage(image);
    return true;
}

static bool wait_for_overlay_region(
    Display *display, Window root, Window window, sample_process *process,
    int x, int y, unsigned int width, unsigned int height,
    uint64_t expected, bool equal)
{
    uint64_t deadline = monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;
    uint64_t observed = 0u;

    for (;;) {
        int timeout = remaining_timeout(deadline);

        if (!process_window_events(display, root, window, NULL)) {
            return false;
        }
        if (capture_overlay_region(display, window, x, y, width, height,
                                   &observed) &&
            ((observed == expected) == equal)) {
            return true;
        }
        if (timeout == 0 ||
            !poll_sample(display, process, timeout < 20 ? timeout : 20)) {
            fprintf(stderr, "overlay region %d,%d %ux%u: %llx expected %s %llx\n",
                    x, y, width, height, (unsigned long long)observed,
                    equal ? "==" : "!=", (unsigned long long)expected);
            return false;
        }
    }
}

static bool run_overlay_scenario(
    Display *display, Window root, Window window, sample_process *process,
    int x, int y, unsigned int width, unsigned int height, const char **stage_out)
{
    uint64_t hyperlink;
    uint64_t search;
    uint64_t preedit;
    uint64_t cursor;
    uint64_t background;
    Window child;
    int root_x;
    int root_y;

#define CAPTURE(column, row, columns, rows, output)                             \
    capture_overlay_region(display, window, x + (int)((column) * width),       \
        y + (int)((row) * height), (columns) * width, (rows) * height, output)
#define WAIT_PIXEL(column, row, color)                                         \
    wait_for_overlay_region(display, root, window, process,                    \
        x + (int)((column) * width + width / 2u),                              \
        y + (int)((row) * height + height / 2u), 1u, 1u, color, true)
#define WAIT_HASH(row, columns, value, equal)                                   \
    wait_for_overlay_region(display, root, window, process,                    \
        x + (int)((row) == 9u ? 8u * width : 0u),                              \
        y + (int)((row) * height), (columns) * width, height, value, equal)
#define MOVE(column, row)                                                       \
    (XTestFakeMotionEvent(display, DefaultScreen(display),                     \
        root_x + x + (int)((column) * width + width / 2u),                     \
        root_y + y + (int)((row) * height + height / 2u), CurrentTime) != 0 &&  \
     (XSync(display, False), true))
#define CHECKPOINT(title)                                                       \
    (send_key(display, window, XK_v, false, false) &&                           \
     wait_for_title(display, root, window, process, title))

    *stage_out = "present initial sample overlays";
    if (!WAIT_PIXEL(14u, 11u, 0x205080u) || !WAIT_PIXEL(0u, 14u, 0x00ff00u) ||
        !CAPTURE(8u, 9u, 19u, 1u, &hyperlink) ||
        !CAPTURE(0u, 11u, 14u, 1u, &search) ||
        !CAPTURE(0u, 13u, 16u, 1u, &preedit) ||
        !capture_overlay_region(display, window,
            x + (int)(30u * width + width / 2u),
            y + (int)(18u * height + height / 2u), 1u, 1u, &background) ||
        XTranslateCoordinates(display, window, root, 0, 0,
                              &root_x, &root_y, &child) == 0) {
        return false;
    }
    *stage_out = "route native OSC8 URL and annotation hover";
    if (!MOVE(1u, 9u) || !WAIT_HASH(9u, 19u, hyperlink, false) ||
        !MOVE(1u, 10u) || !WAIT_HASH(9u, 19u, hyperlink, true) ||
        !MOVE(14u, 11u) ||
        !CHECKPOINT(BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_COMMIT_TITLE)) {
        return false;
    }
    *stage_out = "present changed search preedit annotation and image";
    if (!WAIT_PIXEL(14u, 11u, 0x804020u) || !WAIT_PIXEL(0u, 14u, 0x0000ffu) ||
        !WAIT_HASH(11u, 14u, search, false) ||
        !WAIT_HASH(13u, 16u, preedit, false) ||
        !CAPTURE(0u, 13u, 16u, 1u, &cursor)) {
        return false;
    }
    /* Xvfb exposes premultiplied view pixels, not desktop alpha composition. */
    *stage_out = "change and restore view opacity without X11 compositor";
    if (!send_key(display, window, XK_comma, true, true) ||
        !CHECKPOINT(BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_OPACITY_TITLE) ||
        !wait_for_overlay_region(display, root, window, process,
            x + (int)(30u * width + width / 2u),
            y + (int)(18u * height + height / 2u), 1u, 1u, background, false) ||
        !send_key(display, window, XK_period, true, true) ||
        !WAIT_PIXEL(30u, 18u, background) ||
        !CHECKPOINT(BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_CLEARED_TITLE)) {
        return false;
    }
    *stage_out = "present cleared overlays and changed cursor";
    return WAIT_PIXEL(14u, 11u, background) &&
           WAIT_PIXEL(0u, 14u, background) &&
           WAIT_HASH(13u, 16u, cursor, false) &&
           CHECKPOINT(BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_VERIFIED_TITLE);
#undef CAPTURE
#undef WAIT_PIXEL
#undef WAIT_HASH
#undef MOVE
#undef CHECKPOINT
}

static bool run_geometry_scenario(
    Display *display, Window root, Window window, sample_process *process,
    const XWindowAttributes *initial, unsigned int cell_width,
    unsigned int cell_height, const char **stage_out)
{
    static const char *const names[] = {
        "resize", "resize-reset", "maximize", "maximize-reset"};
    Atom state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom maximized[2] = {
        XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False),
        XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False)};
    XWindowAttributes root_attributes;
    Atom counter_property = XInternAtom(display, "_NET_WM_SYNC_REQUEST_COUNTER", False);
    Atom protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    Atom sync_request = XInternAtom(display, "_NET_WM_SYNC_REQUEST", False);
    Atom actual_type;
    int actual_format;
    unsigned long count;
    unsigned long remaining;
    unsigned char *property = NULL;
    XSyncCounter counter;
    XSyncValue value;
    int major;
    int minor;
    unsigned int maximized_rows = 0u;
    unsigned int maximized_columns = 0u;
    size_t index;

    if (!XSyncInitialize(display, &major, &minor) ||
        XGetWindowAttributes(display, root, &root_attributes) == 0 ||
        XGetWindowProperty(display, window, counter_property, 0, 2, False,
            XA_CARDINAL, &actual_type, &actual_format, &count, &remaining,
            &property) != Success) {
        return false;
    }
    if (actual_type != XA_CARDINAL || actual_format != 32 ||
        count == 0u || count > 2u || remaining != 0u || property == NULL) {
        if (property != NULL) {
            XFree(property);
        }
        return false;
    }
    counter = (XSyncCounter)((unsigned long *)property)[0];
    XFree(property);
    /* EWMH requires the managing WM to initialize the basic counter. Only
     * the application can acknowledge the subsequent nonzero requests. */
    XSyncIntToValue(&value, 0);
    if (counter == None || !XSyncSetCounter(display, counter, value)) {
        return false;
    }
    for (index = 0u; index < 4u; index++) {
        XWindowChanges changes;
        XEvent request;
        uint64_t deadline = monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;
        bool checkpoint = false;
        unsigned int last_pending_sequence = 0u;
        char prefix[96];

        *stage_out = names[index];
        memset(&changes, 0, sizeof(changes));
        changes.width = initial->width + (index == 0u ? 8 * (int)cell_width : 0);
        changes.height = initial->height + (index == 0u ? 4 * (int)cell_height : 0);
        if (index >= 2u) {
            /* This connection owns SubstructureRedirect: publish the WM's
             * actual state together with its constrained allocation. */
            XChangeProperty(display, window, state, XA_ATOM, 32,
                PropModeReplace, (const unsigned char *)maximized,
                index == 2u ? 2 : 0);
        }
        if (index == 2u) {
            /* Use whole-cell increments so NORMAL would not accidentally
             * report constrained merely because of fractional excess. */
            changes.width = initial->width +
                ((root_attributes.width - 80 - initial->width) /
                 (int)cell_width) * (int)cell_width;
            changes.height = initial->height +
                ((root_attributes.height - 80 - initial->height) /
                 (int)cell_height) * (int)cell_height;
        }
        if (index != 3u) {
            memset(&request, 0, sizeof(request));
            request.xclient.type = ClientMessage;
            request.xclient.display = display;
            request.xclient.window = window;
            request.xclient.message_type = protocols;
            request.xclient.format = 32;
            request.xclient.data.l[0] = (long)sync_request;
            request.xclient.data.l[1] = CurrentTime;
            request.xclient.data.l[2] = (long)index + 1;
            if (XSendEvent(display, window, False, NoEventMask, &request) == 0) {
                return false;
            }
        }
        XConfigureWindow(display, window, CWWidth | CWHeight, &changes);
        XSync(display, False);
        snprintf(prefix, sizeof(prefix), "%s%s-grid-",
                 BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PREFIX, names[index]);
        for (;;) {
            XWindowAttributes actual;
            char title[128];
            unsigned int rows;
            unsigned int columns;
            int parsed = 0;
            int timeout = remaining_timeout(deadline);

            if (!process_window_events(display, root, window, NULL) ||
                XGetWindowAttributes(display, window, &actual) == 0 ||
                !XSyncQueryCounter(display, counter, &value)) {
                return false;
            }
            if (!checkpoint &&
                (index == 3u ||
                 (actual.width == changes.width &&
                  actual.height == changes.height)) &&
                (index == 3u ||
                 (XSyncValueHigh32(value) == 0 &&
                  XSyncValueLow32(value) == index + 1u))) {
                if (!send_key(display, window, index < 2u ? XK_r : XK_m,
                              false, false)) {
                    return false;
                }
                checkpoint = true;
            }
            if (read_title(window, display, title, sizeof(title))) {
                unsigned int pending_sequence;
                unsigned int pending_rows;
                unsigned int pending_columns;
                int pending_parsed = 0;
                const char *pending_prefix =
                    BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PENDING_PREFIX
                    "maximize-reset-";

                if (index == 3u &&
                    strncmp(title, pending_prefix, strlen(pending_prefix)) == 0 &&
                    sscanf(title + strlen(pending_prefix), "%u-grid-%u-%u%n",
                           &pending_sequence, &pending_rows, &pending_columns,
                           &pending_parsed) == 3 &&
                    title[strlen(pending_prefix) + (size_t)pending_parsed] == '\0' &&
                    pending_sequence > last_pending_sequence) {
                    if (!send_key(display, window, XK_m, false, false)) {
                        return false;
                    }
                    last_pending_sequence = pending_sequence;
                }
                if (strncmp(title, prefix, strlen(prefix)) == 0 &&
                    sscanf(title + strlen(prefix), "%u-%u%n",
                           &rows, &columns, &parsed) == 2 &&
                    title[strlen(prefix) + (size_t)parsed] == '\0') {
                    if (!checkpoint ||
                        (index == 2u ? rows <= 24u || columns <= 80u :
                         rows != (index == 0u ? 28u : 24u) ||
                         columns != (index == 0u ? 88u : 80u))) {
                        return false;
                    }
                    if (index == 2u) {
                        maximized_rows = rows;
                        maximized_columns = columns;
                    }
                    break;
                }
                if (strncmp(title, BOXRIG_TEST_FRONTEND_SAMPLE_UNEXPECTED_PREFIX,
                            strlen(BOXRIG_TEST_FRONTEND_SAMPLE_UNEXPECTED_PREFIX)) == 0) {
                    fprintf(stderr, "controlled child reported %s\n", title);
                    return false;
                }
            }
            /* Counter changes do not necessarily generate a selected X event. */
            if (timeout == 0 ||
                !poll_sample(display, process, timeout < 20 ? timeout : 20)) {
                return false;
            }
        }
        if (index == 2u) {
            uint64_t zoom_deadline =
                monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;
            unsigned int last_zoom_pending_sequence = 0u;

            *stage_out = "maximize zoom";
            if (!send_key(display, window, XK_minus, true, false) ||
                !send_key(display, window, XK_z, false, false)) {
                return false;
            }
            XSync(display, False);
            for (;;) {
                char title[128];
                unsigned int rows;
                unsigned int columns;
                int parsed = 0;
                int timeout = remaining_timeout(zoom_deadline);
                const char *prefix =
                    BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PREFIX
                    "maximize-zoom-grid-";
                const char *pending_prefix =
                    BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PENDING_PREFIX
                    "maximize-zoom-";

                if (!process_window_events(display, root, window, NULL)) {
                    return false;
                }
                if (read_title(window, display, title, sizeof(title))) {
                    unsigned int pending_sequence;
                    unsigned int pending_rows;
                    unsigned int pending_columns;
                    int pending_parsed = 0;

                    if (strncmp(title, pending_prefix,
                                strlen(pending_prefix)) == 0 &&
                        sscanf(title + strlen(pending_prefix),
                               "%u-grid-%u-%u%n",
                               &pending_sequence,
                               &pending_rows,
                               &pending_columns,
                               &pending_parsed) == 3 &&
                        title[strlen(pending_prefix) +
                              (size_t)pending_parsed] == '\0' &&
                        pending_sequence > last_zoom_pending_sequence) {
                        if (!send_key(display, window, XK_z, false, false)) {
                            return false;
                        }
                        last_zoom_pending_sequence = pending_sequence;
                    }
                }
                if (read_title(window, display, title, sizeof(title)) &&
                    strncmp(title, prefix, strlen(prefix)) == 0 &&
                    sscanf(title + strlen(prefix), "%u-%u%n",
                           &rows, &columns, &parsed) == 2 &&
                    title[strlen(prefix) + (size_t)parsed] == '\0') {
                    if (rows <= maximized_rows &&
                        columns <= maximized_columns) {
                        return false;
                    }
                    break;
                }
                if (timeout == 0 ||
                    !poll_sample(
                        display,
                        process,
                        timeout < 20 ? timeout : 20)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool start_pending_clipboard(
    Display *display, Window root, Window window, sample_process *process)
{
    Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
    uint64_t deadline = monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;

    if (held_clipboard_owner != None) {
        XDestroyWindow(display, held_clipboard_owner);
    }
    held_clipboard_owner = XCreateSimpleWindow(display, root, 0, 0, 1, 1, 0, 0, 0);
    held_clipboard_requested = false;
    held_clipboard_released = false;
    memset(&held_clipboard_request, 0, sizeof(held_clipboard_request));
    XSetSelectionOwner(display, clipboard, held_clipboard_owner, CurrentTime);
    XSync(display, False);
    if (held_clipboard_owner == None ||
        XGetSelectionOwner(display, clipboard) != held_clipboard_owner ||
        !send_key(display, window, XK_v, true, true)) {
        return false;
    }
    while (!held_clipboard_requested) {
        int timeout = remaining_timeout(deadline);
        if (timeout == 0 ||
            !process_window_events(display, root, window, NULL)) {
            return false;
        }
        if (!held_clipboard_requested && !poll_sample(display, process, timeout)) {
            return false;
        }
    }
    return held_clipboard_request.selection == clipboard &&
           held_clipboard_request.requestor != None && !held_clipboard_released;
}

static bool run_child_exit_scenario(
    Display *display,
    Window root,
    const char *sample_path,
    const char *helper_path,
    bool pending_clipboard,
    const char **stage_out,
    sample_process *process)
{
    Window window = None;
    XWindowAttributes before_zoom;
    XWindowAttributes after_zoom;
    XWindowAttributes initial_attributes;
    unsigned int cell_height = 0u;
    unsigned int cell_width = 0u;
    unsigned long frame_left = 0u;
    unsigned long frame_right = 0u;
    unsigned long frame_top = 0u;
    unsigned long frame_bottom = 0u;
    unsigned int grid_width;
    unsigned int grid_height;
    int viewport_x;
    int viewport_y;
    bool wheel_input = getenv("BOXRIG_TEST_SAMPLE_WHEEL") != NULL;
    const char *overlays = getenv("BOXRIG_TEST_SAMPLE_OVERLAYS");
    bool expect_overlays = overlays != NULL && strcmp(overlays, "1") == 0;
    const char *geometry = getenv("BOXRIG_TEST_SAMPLE_GEOMETRY");

    *stage_out = "start controlled child";
    if (!start_sample(sample_path, helper_path, pending_clipboard ? "1" : "0", "--sample-scenario", process) ||
        !wait_for_window(display, root, process, &window)) {
        return false;
    }
    *stage_out = "observe shared scene";
    if (!wait_for_ready_title(
            display,
            root,
            window,
            process,
            &cell_height,
            &cell_width) ||
        !wait_for_semantic_pixels(display, root, window, process)) {
        return false;
    }
    grid_width = BOXRIG_TEST_FRONTEND_SAMPLE_COLUMNS * cell_width;
    grid_height = BOXRIG_TEST_FRONTEND_SAMPLE_ROWS * cell_height;
    (void)read_frame_extents(
        display,
        window,
        &frame_left,
        &frame_right,
        &frame_top,
        &frame_bottom);
    if (XGetWindowAttributes(display, window, &initial_attributes) == 0 ||
        initial_attributes.width <
            (int)(frame_left + frame_right + grid_width) ||
        initial_attributes.height <
            (int)(frame_top + frame_bottom + grid_height)) {
        return false;
    }
    viewport_x = (int)frame_left;
    viewport_y = initial_attributes.height - (int)frame_bottom -
                 (int)grid_height;
    (void)locate_terminal_viewport(
        display,
        window,
        grid_width,
        grid_height,
        &viewport_x,
        &viewport_y);
    *stage_out = "deliver native key";
    if (!send_key(display, window, XK_k, false, false) ||
        !wait_for_title(
            display,
            root,
            window,
            process,
            BOXRIG_TEST_FRONTEND_SAMPLE_KEY_TITLE)) {
        return false;
    }
    *stage_out = "round trip linear selection";
    if (!clear_selection(display, XA_PRIMARY) ||
        !send_drag(
            display,
            window,
            viewport_x,
            viewport_y,
            (int)cell_width / 4,
            (int)cell_height / 2,
            (int)(10u * cell_width + (3u * cell_width) / 4u),
            (int)cell_height / 2,
            NoSymbol) ||
        !wait_for_selection(
            display,
            root,
            window,
            process,
            XA_PRIMARY) ||
        !wait_for_selection_text(
            display,
            root,
            window,
            process,
            XA_PRIMARY,
            "copy-target") ||
        !copy_selection_and_paste(
            display,
            root,
            window,
            process,
            "copy-target") ||
        !wait_for_title(
            display,
            root,
            window,
            process,
            BOXRIG_TEST_FRONTEND_SAMPLE_LINEAR_TITLE)) {
        return false;
    }
    *stage_out = "enable mouse reporting after native input barrier";
    if (!send_key(display, window, XK_m, false, false) ||
        !wait_for_title(
            display,
            root,
            window,
            process,
            BOXRIG_TEST_FRONTEND_SAMPLE_MOUSE_READY_TITLE)) {
        return false;
    }
    *stage_out = "deliver unmodified terminal mouse click";
    if (!send_drag(
            display,
            window,
            viewport_x,
            viewport_y,
            (int)(20u * cell_width + cell_width / 2u),
            (int)(8u * cell_height + cell_height / 2u),
            (int)(20u * cell_width + cell_width / 2u),
            (int)(8u * cell_height + cell_height / 2u),
            NoSymbol) ||
        !wait_for_title(
            display,
            root,
            window,
            process,
            wheel_input ? "boxrig-wheel-ready-0" :
                          BOXRIG_TEST_FRONTEND_SAMPLE_MOUSE_TITLE)) {
        return false;
    }
    if (wheel_input) {
        size_t batch;
        boxrig_test_video_hold();
        *stage_out = "deliver wheel reports while synchronized drawing is held";
        for (batch = 0; batch < 16; batch++) {
            char title[64];
            size_t event;
            for (event = 0; event < 8; event++) {
                unsigned int button = event < 4 ? 5 : 4;
                if (!XTestFakeButtonEvent(display, button, True, CurrentTime) ||
                    !XTestFakeButtonEvent(display, button, False, CurrentTime)) {
                    return false;
                }
            }
            XSync(display, False);
            snprintf(title, sizeof(title), "boxrig-wheel-ready-%zu", batch + 1);
            if (!wait_for_title(display, root, window, process,
                    batch + 1 < 16 ? title : "boxrig-wheel-control-ready")) {
                return false;
            }
        }
        if (!send_key(display, window, XK_c, true, false) ||
            !wait_for_title(display, root, window, process, "boxrig-wheel-complete")) {
            return false;
        }
        boxrig_test_video_hold();
    }
    *stage_out = "clear primary before Shift selection override";
    if (!clear_selection(display, XA_PRIMARY)) {
        return false;
    }
    *stage_out = "deliver Shift selection override";
    if (!send_drag(
            display,
            window,
            viewport_x,
            viewport_y,
            (int)cell_width / 4,
            (int)(2u * cell_height + cell_height / 2u),
            (int)(10u * cell_width + (3u * cell_width) / 4u),
            (int)(2u * cell_height + cell_height / 2u),
            XK_Shift_L)) {
        return false;
    }
    *stage_out = "wait for Shift selection ownership";
    if (!wait_for_selection(
            display,
            root,
            window,
            process,
            XA_PRIMARY)) {
        return false;
    }
    *stage_out = "read Shift selection payload";
    if (!wait_for_selection_text(
            display,
            root,
            window,
            process,
            XA_PRIMARY,
            "copy-target")) {
        return false;
    }
    *stage_out = "round trip Shift selection payload";
    if (!copy_selection_and_paste(
            display,
            root,
            window,
            process,
            "copy-target") ||
        !wait_for_title(
            display,
            root,
            window,
            process,
            BOXRIG_TEST_FRONTEND_SAMPLE_SHIFT_TITLE)) {
        return false;
    }
    *stage_out = "round trip rectangular selection override";
    if (!clear_selection(display, XA_PRIMARY) ||
        !send_drag(
            display,
            window,
            viewport_x,
            viewport_y,
            (int)cell_width / 4,
            (int)cell_height / 2,
            (int)(4u * cell_width + (3u * cell_width) / 4u),
            (int)(cell_height + cell_height / 2u),
            XK_Control_L) ||
        !wait_for_selection(
            display,
            root,
            window,
            process,
            XA_PRIMARY) ||
        !wait_for_selection_text(
            display,
            root,
            window,
            process,
            XA_PRIMARY,
            "copy-\nblock") ||
        !copy_selection_and_paste(
            display,
            root,
            window,
            process,
            "copy-\nblock") ||
        !wait_for_title(
            display,
            root,
            window,
            process,
            BOXRIG_TEST_FRONTEND_SAMPLE_RECTANGLE_TITLE)) {
        return false;
    }
    *stage_out = "converge zoom grid";
    if (XGetWindowAttributes(display, window, &before_zoom) == 0 ||
        !send_key(display, window, XK_equal, true, true) ||
        !wait_for_resize(
            display,
            root,
            window,
            process,
            before_zoom.width,
            before_zoom.height) ||
        XGetWindowAttributes(display, window, &after_zoom) == 0 ||
        !send_key(display, window, XK_g, false, false) ||
        !wait_for_title(
            display,
            root,
            window,
            process,
            BOXRIG_TEST_FRONTEND_SAMPLE_ZOOM_TITLE)) {
        return false;
    }
    *stage_out = "converge reset grid";
    if (!send_key(display, window, XK_0, true, false) ||
        !wait_for_resize(
            display,
            root,
            window,
            process,
            after_zoom.width,
            after_zoom.height) ||
        !send_key(display, window, XK_g, false, false) ||
        !wait_for_title(
            display,
            root,
            window,
            process,
            expect_overlays ? BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_READY_TITLE :
                              BOXRIG_TEST_FRONTEND_SAMPLE_RESET_TITLE)) {
        return false;
    }
    if (expect_overlays &&
        !run_overlay_scenario(display, root, window, process,
            viewport_x, viewport_y, cell_width, cell_height, stage_out)) {
        return false;
    }
    if (geometry != NULL && strcmp(geometry, "1") == 0 &&
        !run_geometry_scenario(display, root, window, process,
            &initial_attributes, cell_width, cell_height, stage_out)) {
        return false;
    }
    if (pending_clipboard) {
        *stage_out = "hold external clipboard before child exit";
        if (!start_pending_clipboard(display, root, window, process)) {
            return false;
        }
    }
    *stage_out = "follow controlled child exit";
    return send_key(display, window, XK_q, false, false) &&
           wait_for_exit_with_display(display, process) &&
           (!pending_clipboard || held_clipboard_released);
}

static bool run_window_close_scenario(
    Display *display,
    Window root,
    const char *sample_path,
    const char *helper_path,
    bool copy_before_close,
    bool pending_clipboard,
    const char **stage_out,
    sample_process *process)
{
    Window window = None;
    unsigned int cell_height;
    unsigned int cell_width;

    *stage_out = "start close-path child";
    if (!start_sample(sample_path, helper_path, pending_clipboard ? "close" : "0", "--sample-scenario", process) ||
        !wait_for_window(display, root, process, &window) ||
        !wait_for_ready_title(
            display,
            root,
            window,
            process,
            &cell_height,
            &cell_width)) {
        return false;
    }
    if (copy_before_close) {
        XWindowAttributes attributes;
        unsigned long left = 0u;
        unsigned long right = 0u;
        unsigned long top = 0u;
        unsigned long bottom = 0u;
        unsigned int width = BOXRIG_TEST_FRONTEND_SAMPLE_COLUMNS * cell_width;
        unsigned int height = BOXRIG_TEST_FRONTEND_SAMPLE_ROWS * cell_height;
        int viewport_x;
        int viewport_y;

        *stage_out = "copy and paste before native close";
        if (!wait_for_semantic_pixels(display, root, window, process) ||
            XGetWindowAttributes(display, window, &attributes) == 0) {
            return false;
        }
        (void)read_frame_extents(display, window, &left, &right, &top, &bottom);
        if (attributes.width < (int)(left + right + width) ||
            attributes.height < (int)(top + bottom + height)) {
            return false;
        }
        viewport_x = (int)left;
        viewport_y = attributes.height - (int)bottom - (int)height;
        (void)locate_terminal_viewport(display, window, width, height,
                                      &viewport_x, &viewport_y);
        if (!send_key(display, window, XK_k, false, false) ||
            !wait_for_title(display, root, window, process,
                            BOXRIG_TEST_FRONTEND_SAMPLE_KEY_TITLE) ||
            !clear_selection(display, XA_PRIMARY) ||
            !send_drag(display, window, viewport_x, viewport_y,
                (int)cell_width / 4, (int)cell_height / 2,
                (int)(10u * cell_width + 3u * cell_width / 4u),
                (int)cell_height / 2, NoSymbol) ||
            !wait_for_selection(display, root, window, process, XA_PRIMARY) ||
            !wait_for_selection_text(display, root, window, process,
                                     XA_PRIMARY, "copy-target") ||
            !copy_selection_and_paste(display, root, window, process, "copy-target") ||
            !wait_for_title(display, root, window, process,
                            BOXRIG_TEST_FRONTEND_SAMPLE_LINEAR_TITLE)) {
            return false;
        }
    }
    if (pending_clipboard) {
        *stage_out = "hold external clipboard before window close";
        if (!send_key(display, window, XK_k, false, false) ||
            !wait_for_title(display, root, window, process,
                            BOXRIG_TEST_FRONTEND_SAMPLE_KEY_TITLE) ||
            !start_pending_clipboard(display, root, window, process)) {
            return false;
        }
    }
    *stage_out = "close native window";
    return send_wm_delete(display, window) &&
           wait_for_exit_with_display(display, process) &&
           (!pending_clipboard || held_clipboard_released);
}

static bool run_continuous_output_scenario(
    Display *display, Window root, const char *sample_path,
    const char *helper_path, const char **stage_out, sample_process *process)
{
    Window window = None;
    int x;
    int y;

    *stage_out = "start continuous-output child";
    if (!start_sample(sample_path, helper_path, "0", "--continuous-output", process) ||
        !wait_for_window(display, root, process, &window) ||
        !wait_for_title(display, root, window, process, "boxrig-flood-ready")) return false;
    x = 100;
    y = 150;
    *stage_out = "present initial frame before continuous output";
    if (!wait_for_overlay_region(display, root, window, process,
            x, y, 1, 1, UINT64_C(0x113355), true)) return false;
    if (getenv("BOXRIG_TEST_VIDEO") != NULL) {
        Window child;
        int root_x, root_y;
        if (!XTranslateCoordinates(display, window, root, x, y,
                &root_x, &root_y, &child)) return false;
        printf("FLOOD_VIDEO_PIXEL %d %d\n", root_x, root_y);
    }
    boxrig_test_video_hold();
    /* Stationary pointer feedback must not prevent GTK from painting
     * new PTY output or completing a subsequent native allocation. */
    if (!XWarpPointer(display, None, window, 0, 0, 0, 0, x + 40, y + 20) ||
        !send_key(display, window, XK_f, false, false) ||
        !wait_for_title(display, root, window, process, "boxrig-flood-active")) return false;
    *stage_out = "present a changed frame while PTY output remains continuous";
    if (!wait_for_overlay_region(display, root, window, process,
            x, y, 1, 1, UINT64_C(0x993322), true)) return false;
    boxrig_test_video_hold();
    *stage_out = "complete native resize while PTY output remains continuous";
    XResizeWindow(display, window, 1200, 900);
    XSync(display, False);
    if (!wait_for_overlay_region(display, root, window, process,
            1100, 850, 1, 1, UINT64_C(0x993322), true)) return false;
    if (getenv("BOXRIG_TEST_VIDEO") != NULL) {
        Window child;
        int root_x, root_y;
        if (!XTranslateCoordinates(display, window, root, 1100, 850,
                &root_x, &root_y, &child)) return false;
        printf("FLOOD_VIDEO_RESIZED_PIXEL %d %d\n", root_x, root_y);
    }
    boxrig_test_video_hold();
    *stage_out = "deliver motion, wheel and key while PTY output remains continuous";
    if (!XWarpPointer(display, None, window, 0, 0, 0, 0, x + 40, y + 20) ||
        !XTestFakeButtonEvent(display, 5, True, CurrentTime) ||
        !XTestFakeButtonEvent(display, 5, False, CurrentTime) ||
        !send_key(display, window, XK_k, false, false)) return false;
    *stage_out = "receive wheel and key acknowledgement during continuous output";
    if (!wait_for_title(display, root, window, process,
            "boxrig-flood-input-received")) return false;
    *stage_out = "present acknowledged input during continuous output";
    if (!wait_for_overlay_region(display, root, window, process,
            x, y, 1, 1, UINT64_C(0x227744), true)) return false;
    boxrig_test_video_hold();
    *stage_out = "deliver Ctrl-C while PTY output remains continuous";
    return send_key(display, window, XK_c, true, false) &&
           wait_for_exit_with_display(display, process);
}

static bool run_synchronized_output_recovery_scenario(
    Display *display, Window root, const char *sample_path,
    const char *helper_path, const char **stage_out, sample_process *process)
{
    Window window = None;

    *stage_out = "start synchronized output recovery child";
    if (!start_sample(sample_path, helper_path, "0", "--sync-recovery", process) ||
        !wait_for_window(display, root, process, &window) ||
        !wait_for_title(display, root, window, process, "boxrig-sync-ready") ||
        !wait_for_overlay_region(display, root, window, process,
            100, 150, 1, 1, UINT64_C(0x113355), true)) return false;
    if (getenv("BOXRIG_TEST_VIDEO") != NULL) {
        Window child;
        int x, y;
        if (!XTranslateCoordinates(display, window, root, 100, 150,
                &x, &y, &child)) return false;
        printf("SYNC_VIDEO_PIXEL %d %d\n", x, y);
    }
    boxrig_test_video_hold();
    *stage_out = "recover an unclosed synchronized frame without further PTY output";
    if (!send_key(display, window, XK_f, false, false) ||
        !wait_for_title(display, root, window, process, "boxrig-sync-held") ||
        !wait_for_overlay_region(display, root, window, process,
            100, 150, 1, 1, UINT64_C(0x993322), true)) return false;
    boxrig_test_video_hold();
    *stage_out = "deliver Ctrl-C and show shell output after another unclosed batch";
    if (!send_key(display, window, XK_k, false, false) ||
        !wait_for_title(display, root, window, process, "boxrig-sync-interruptible") ||
        !send_key(display, window, XK_c, true, false) ||
        !wait_for_title(display, root, window, process, "boxrig-sync-shell") ||
        !wait_for_overlay_region(display, root, window, process,
            100, 150, 1, 1, UINT64_C(0x997722), true)) return false;
    boxrig_test_video_hold();
    *stage_out = "exit after synchronized output recovery";
    return send_key(display, window, XK_q, false, false) &&
           wait_for_exit_with_display(display, process);
}

static bool run_color_filter_demo(
    Display *display, Window root, const char *sample_path,
    const char *helper_path, const char **stage_out, sample_process *process)
{
    Window window = None;
    XWindowAttributes attributes;
    uint64_t filtered;
    uint64_t plain;
    int x;
    int y;

    *stage_out = "start color-filter demo";
    if (setenv("BOXRIG_COLOR_FILTER_DEMO", "1", 1) == -1) return false;
    bool started = start_sample(sample_path, helper_path, "0", NULL, process);
    unsetenv("BOXRIG_COLOR_FILTER_DEMO");
    if (!started || !wait_for_window(display, root, process, &window)) return false;
    *stage_out = "query color-filter grid";
    if (!send_key(display, window, XK_g, false, false) ||
        !wait_for_title(display, root, window, process, "grid-24-80") ||
        !XGetWindowAttributes(display, window, &attributes)) return false;
    const char *expected_api = getenv("BOXRIG_TEST_SAMPLE_GRAPHICS_API");
    if (expected_api != NULL) {
        char expected_line[128];
        *stage_out = "report the selected graphics API";
        int length = snprintf(expected_line, sizeof(expected_line),
            "boxrig_term: graphics-api=%s\n", expected_api);
        if (length < 0 || (size_t)length >= sizeof(expected_line) ||
            !drain_output(process) ||
            strstr(process->diagnostics, expected_line) == NULL) return false;
    }
    x = attributes.width / 2;
    y = attributes.height * 2 / 3;
    *stage_out = "display corrected blank cell";
    if (!wait_for_overlay_region(display, root, window, process,
            x, y, 1, 1, UINT64_C(0x404040), true)) return false;
    *stage_out = "disable color filter";
    if (!send_key(display, window, XK_F12, true, true) ||
        !wait_for_overlay_region(display, root, window, process,
            x, y, 1, 1, UINT64_C(0x101010), true) ||
        !capture_overlay_region(display, window, 0, 0,
            (unsigned int)attributes.width, (unsigned int)attributes.height,
            &plain)) return false;
    *stage_out = "display identity filter";
    if (!send_key(display, window, XK_F12, true, true)) return false;
    XStoreName(display, window, "awaiting-color-grid");
    if (!send_key(display, window, XK_g, false, false) ||
        !wait_for_title(display, root, window, process, "grid-24-80") ||
        !wait_for_overlay_region(display, root, window, process,
            0, 0, (unsigned int)attributes.width, (unsigned int)attributes.height,
            plain, true)) return false;
    /* The first mapped frame may precede PTY output. Record the corrected
     * reference after a completed mode transition has painted that output. */
    *stage_out = "record corrected output";
    if (!send_key(display, window, XK_F12, true, true) ||
        !wait_for_overlay_region(display, root, window, process,
            0, 0, (unsigned int)attributes.width, (unsigned int)attributes.height,
            plain, false) ||
        !capture_overlay_region(display, window, 0, 0,
            (unsigned int)attributes.width, (unsigned int)attributes.height,
            &filtered)) return false;
    *stage_out = "repeat color-filter cycle";
    if (!send_key(display, window, XK_F12, true, true) ||
        !wait_for_overlay_region(display, root, window, process,
            0, 0, (unsigned int)attributes.width, (unsigned int)attributes.height,
            plain, true) ||
        !send_key(display, window, XK_F12, true, true)) return false;
    XStoreName(display, window, "awaiting-color-grid");
    if (!send_key(display, window, XK_g, false, false) ||
        !wait_for_title(display, root, window, process, "grid-24-80") ||
        !send_key(display, window, XK_F12, true, true) ||
        !wait_for_overlay_region(display, root, window, process,
            0, 0, (unsigned int)attributes.width, (unsigned int)attributes.height,
            filtered, true)) return false;
    *stage_out = "close color-filter demo";
    return send_key(display, window, XK_q, false, false) &&
           wait_for_exit_with_display(display, process);
}

int main(int argument_count, char **arguments)
{
    Display *display;
    Window root;
    sample_process process;
    const char *stage = "open display";
    const char *pending_profile = getenv("BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD");
    bool passed = false;
    boxrig_test_video video = {0};
    char video_size[64];

    if ((argument_count != 4 && argument_count != 5) ||
        (argument_count == 5 && strcmp(arguments[4], "--continuous-output") != 0 &&
         strcmp(arguments[4], "--sync-recovery") != 0 &&
         strcmp(arguments[4], "--color-demo") != 0)) {
        fprintf(
            stderr,
            "usage: %s FRONTEND SAMPLE PTY_HELPER [--continuous-output|--sync-recovery|--color-demo]\n",
            arguments[0]);
        return 2;
    }
    process.identifier = -1;
    process.output = -1;
    held_clipboard_may_cancel = strcmp(arguments[1], "GTK4") == 0;
    if ((strcmp(arguments[1], "GTK3") == 0 || strcmp(arguments[1], "GTK4") == 0) &&
        setenv("BOXRIG_TEST_SAMPLE_WHEEL", "1", 1) == -1) {
        return 1;
    }
    display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "shared sample scenario could not open display\n");
        return 1;
    }
    XSetErrorHandler(record_x11_error);
    root = DefaultRootWindow(display);
    snprintf(video_size, sizeof(video_size), "%dx%d",
        DisplayWidth(display, DefaultScreen(display)),
        DisplayHeight(display, DefaultScreen(display)));
    if (!boxrig_test_video_start(&video, DisplayString(display), video_size)) {
        XCloseDisplay(display);
        return 1;
    }
    XSelectInput(
        display,
        root,
        SubstructureNotifyMask | SubstructureRedirectMask);
    XSync(display, False);
    if (argument_count == 5 && strcmp(arguments[4], "--color-demo") == 0) {
        passed = run_color_filter_demo(display, root, arguments[2], arguments[3],
                                      &stage, &process);
        if (passed) printf("PASS %s sample color filter toggle and identity pixels\n", arguments[1]);
        goto cleanup;
    }
    if (argument_count == 5 && strcmp(arguments[4], "--sync-recovery") == 0) {
        passed = run_synchronized_output_recovery_scenario(
            display, root, arguments[2], arguments[3], &stage, &process);
        if (passed) printf("PASS %s recovers unclosed synchronized output and handles Ctrl-C\n", arguments[1]);
        goto cleanup;
    }
    if (argument_count == 5) {
        passed = run_continuous_output_scenario(display, root, arguments[2], arguments[3],
                                                &stage, &process);
        if (passed) printf("PASS %s displays and handles input during continuous PTY output\n", arguments[1]);
        goto cleanup;
    }
    if (!run_child_exit_scenario(
            display,
            root,
            arguments[2],
            arguments[3],
            false,
            &stage,
            &process)) {
        goto cleanup;
    }
    stop_sample(&process);
    while (XPending(display) > 0) {
        XEvent event;

        XNextEvent(display, &event);
    }
    if (!run_window_close_scenario(
            display,
            root,
            arguments[2],
            arguments[3],
            false,
            false,
            &stage,
            &process)) {
        goto cleanup;
    }
    stop_sample(&process);
    while (XPending(display) > 0) {
        XEvent event;
        XNextEvent(display, &event);
    }
    if (!run_window_close_scenario(display, root, arguments[2], arguments[3],
                                   true, false, &stage, &process)) {
        goto cleanup;
    }
    printf("PASS %s sample closes after native clipboard copy and completed paste\n",
           arguments[1]);
    if (pending_profile != NULL && strcmp(pending_profile, "1") == 0) {
        stop_sample(&process);
        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
        }
        if (!run_child_exit_scenario(display, root, arguments[2], arguments[3],
                                     true, &stage, &process)) {
            goto cleanup;
        }
        printf("PASS %s sample completes pending clipboard shutdown on child exit\n",
               arguments[1]);
        stop_sample(&process);
        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
        }
        if (!run_window_close_scenario(display, root, arguments[2], arguments[3],
                                       false, true, &stage, &process)) {
            goto cleanup;
        }
        printf("PASS %s sample completes pending clipboard shutdown on window close\n",
               arguments[1]);
    }
    passed = true;

cleanup:
    if (!passed) {
        fprintf(
            stderr,
            "%s shared sample scenario failed at %s\n%s",
            arguments[1],
            stage,
            process.diagnostics);
    }
    stop_sample(&process);
    if (held_clipboard_owner != None) {
        XDestroyWindow(display, held_clipboard_owner);
    }
    if (!boxrig_test_video_stop(&video)) passed = false;
    XCloseDisplay(display);
    if (!passed) {
        return 1;
    }
    if (argument_count == 4) {
        printf(
            "PASS %s sample follows shared PTY scene input selection geometry and shutdown\n",
            arguments[1]);
    }
    return 0;
}
