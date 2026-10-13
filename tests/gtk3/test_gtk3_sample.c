/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
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

#define TEST_TIMEOUT_MILLISECONDS UINT64_C(15000)
#define GTK3_SAMPLE_RESIZE_BORDER 6

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1 || now.tv_sec < 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static bool apply_configure_request(
    Display *display,
    const XConfigureRequestEvent *request);

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
    const char *geometry_trace_path,
    int *output_descriptor)
{
    int output_pipe[2];
    pid_t process;

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
            setenv("G_DEBUG", "fatal-criticals", 1) == -1 ||
            (geometry_trace_path != NULL &&
             setenv(
                 "BOXRIG_TERM_GTK3_GEOMETRY_TRACE",
                 geometry_trace_path,
                 1) == -1) ||
            unsetenv("BOXRIG_BACKGROUND_OPACITY") == -1) {
            _exit(127);
        }
        close(output_pipe[1]);
        execl(
            sample_path,
            sample_path,
            "--",
            helper_path,
            "--report-resize",
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

static bool configure_fake_window_manager(Display *display, Window root)
{
    Atom supported = XInternAtom(display, "_NET_SUPPORTED", False);
    Atom supporting_check =
        XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", False);
    Atom wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    Atom utf8_string = XInternAtom(display, "UTF8_STRING", False);
    Atom state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom maximized_horizontal =
        XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    Atom maximized_vertical =
        XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    Atom moveresize = XInternAtom(display, "_NET_WM_MOVERESIZE", False);
    Atom atoms[4];
    Window check_window;
    static const unsigned char name[] = "boxrig-test-wm";

    if (supported == None || supporting_check == None || wm_name == None ||
        utf8_string == None || state == None || maximized_horizontal == None ||
        maximized_vertical == None || moveresize == None) {
        return false;
    }
    check_window = XCreateSimpleWindow(
        display, root, -1, -1, 1, 1, 0, 0, 0);
    if (check_window == None) {
        return false;
    }
    XChangeProperty(
        display,
        root,
        supporting_check,
        XA_WINDOW,
        32,
        PropModeReplace,
        (unsigned char *)&check_window,
        1);
    XChangeProperty(
        display,
        check_window,
        supporting_check,
        XA_WINDOW,
        32,
        PropModeReplace,
        (unsigned char *)&check_window,
        1);
    XChangeProperty(
        display,
        check_window,
        wm_name,
        utf8_string,
        8,
        PropModeReplace,
        name,
        sizeof(name) - 1);
    atoms[0] = state;
    atoms[1] = maximized_horizontal;
    atoms[2] = maximized_vertical;
    atoms[3] = moveresize;
    XChangeProperty(
        display,
        root,
        supported,
        XA_ATOM,
        32,
        PropModeReplace,
        (unsigned char *)atoms,
        4);
    XSync(display, False);
    return true;
}

static bool read_latest_trace_allocations(
    const char *path,
    int *window_width_out,
    int *window_height_out,
    int *content_width_out,
    int *content_height_out)
{
    FILE *input = fopen(path, "r");
    char line[512];
    bool found = false;

    if (input == NULL || fgets(line, sizeof(line), input) == NULL) {
        if (input != NULL) {
            fclose(input);
        }
        return false;
    }
    while (fgets(line, sizeof(line), input) != NULL) {
        unsigned long long sequence;
        unsigned long long time;
        char event[32];
        char state[16];
        int native_width;
        int native_height;
        int window_width;
        int window_height;
        int content_width;
        int content_height;
        int terminal_width;
        int terminal_height;
        size_t rows;
        size_t columns;

        if (sscanf(
                line,
                "%llu,%llu,%31[^,],%15[^,],%d,%d,%d,%d,%d,%d,%d,%d,%zu,%zu",
                &sequence,
                &time,
                event,
                state,
                &native_width,
                &native_height,
                &window_width,
                &window_height,
                &content_width,
                &content_height,
                &terminal_width,
                &terminal_height,
                &rows,
                &columns) != 14) {
            fclose(input);
            return false;
        }
        if (window_width > 0 && window_height > 0 &&
            content_width > 0 && content_height > 0) {
            *window_width_out = window_width;
            *window_height_out = window_height;
            *content_width_out = content_width;
            *content_height_out = content_height;
            found = true;
        }
    }
    fclose(input);
    return found;
}

static bool wait_for_moveresize(
    Display *display,
    Window terminal_window,
    int output_descriptor,
    int root_x,
    int root_y,
    long expected_direction,
    char *output,
    size_t output_capacity,
    size_t *output_length)
{
    Atom moveresize = XInternAtom(display, "_NET_WM_MOVERESIZE", False);
    uint64_t deadline =
        monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;
    bool pressed = false;

    if (moveresize == None ||
        XTestFakeMotionEvent(
            display,
            DefaultScreen(display),
            root_x,
            root_y,
            CurrentTime) == 0 ||
        XTestFakeButtonEvent(display, 1, True, CurrentTime) == 0) {
        return false;
    }
    pressed = true;
    XSync(display, False);
    for (;;) {
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == ClientMessage &&
                event.xclient.message_type == moveresize &&
                event.xclient.window == terminal_window) {
                bool matches =
                    event.xclient.data.l[2] == expected_direction &&
                    event.xclient.data.l[3] == 1;
                int grab_status = XGrabPointer(
                    display,
                    DefaultRootWindow(display),
                    False,
                    ButtonReleaseMask,
                    GrabModeAsync,
                    GrabModeAsync,
                    None,
                    None,
                    CurrentTime);

                matches = matches && grab_status == GrabSuccess;
                (void)XTestFakeButtonEvent(display, 1, False, CurrentTime);
                XSync(display, False);
                if (grab_status == GrabSuccess) {
                    XUngrabPointer(display, CurrentTime);
                    XSync(display, False);
                }
                return matches;
            }
            if (event.type == ConfigureRequest &&
                event.xconfigurerequest.window == terminal_window) {
                if (!apply_configure_request(
                        display,
                        &event.xconfigurerequest)) {
                    goto fail;
                }
            } else if (event.type == DestroyNotify &&
                       event.xdestroywindow.window == terminal_window) {
                goto fail;
            }
        }
        {
            struct pollfd descriptors[2];
            int timeout = remaining_timeout(deadline);
            int result;

            if (timeout == 0) {
                goto fail;
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
            if (result <= 0 ||
                ((descriptors[1].revents &
                  (POLLIN | POLLHUP | POLLERR)) != 0 &&
                 !drain_output(
                     output_descriptor,
                     output,
                     output_capacity,
                     output_length)) ||
                (descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
                goto fail;
            }
        }
    }

fail:
    if (pressed) {
        (void)XTestFakeButtonEvent(display, 1, False, CurrentTime);
        XSync(display, False);
    }
    return false;
}

static bool verify_inner_resize_handles(
    Display *display,
    Window window,
    int output_descriptor,
    const char *trace_path,
    int *resize_edge_x_out,
    int *scrollbar_x_out,
    char *output,
    size_t output_capacity,
    size_t *output_length)
{
    enum { HANDLE_COUNT = 8 };
    static const long directions[HANDLE_COUNT] = {0, 1, 2, 7, 3, 6, 5, 4};
    static const char *const cursor_names[HANDLE_COUNT] = {
        "nw-resize", "n-resize", "ne-resize", "w-resize", "e-resize",
        "sw-resize", "s-resize", "se-resize"};
    static const char *const cursor_aliases[HANDLE_COUNT] = {
        "top_left_corner", "top_side", "top_right_corner", "left_side",
        "right_side", "bottom_left_corner", "bottom_side", "bottom_right_corner"};
    int fixes_major = 2;
    int fixes_minor = 0;
    int x[HANDLE_COUNT];
    int y[HANDLE_COUNT];
    XWindowAttributes window_attributes;
    size_t index;
    Window ignored_child;
    int window_root_x;
    int window_root_y;
    int traced_window_width;
    int traced_window_height;
    int content_width;
    int content_height;
    int frame_inset;
    int left;
    int right;
    int top;
    int bottom;

    if (!XFixesQueryVersion(display, &fixes_major, &fixes_minor) ||
        fixes_major < 2 ||
        resize_edge_x_out == NULL || scrollbar_x_out == NULL ||
        XGetWindowAttributes(display, window, &window_attributes) == 0 ||
        !read_latest_trace_allocations(
            trace_path,
            &traced_window_width,
            &traced_window_height,
            &content_width,
            &content_height) ||
        traced_window_width != window_attributes.width ||
        traced_window_height != window_attributes.height ||
        content_width > traced_window_width ||
        content_height > traced_window_height ||
        (traced_window_width - content_width) % 2 != 0 ||
        !XTranslateCoordinates(
            display,
            window,
            DefaultRootWindow(display),
            0,
            0,
            &window_root_x,
            &window_root_y,
            &ignored_child)) {
        fprintf(stderr, "could not locate GTK3 resize handles\n");
        return false;
    }
    frame_inset = (traced_window_width - content_width) / 2;
    if (content_width <= GTK3_SAMPLE_RESIZE_BORDER ||
        traced_window_height - content_height < frame_inset * 2) {
        fprintf(stderr, "invalid GTK3 resize frame allocation\n");
        return false;
    }
    left = frame_inset + GTK3_SAMPLE_RESIZE_BORDER / 2;
    right = window_attributes.width - frame_inset -
            GTK3_SAMPLE_RESIZE_BORDER / 2;
    top = frame_inset + GTK3_SAMPLE_RESIZE_BORDER / 2;
    bottom = window_attributes.height - frame_inset -
             GTK3_SAMPLE_RESIZE_BORDER / 2;
    x[0] = left;
    x[1] = window_attributes.width / 2;
    x[2] = right;
    x[3] = left;
    x[4] = right;
    x[5] = left;
    x[6] = window_attributes.width / 2;
    x[7] = right;
    y[0] = top;
    y[1] = top;
    y[2] = top;
    y[3] = window_attributes.height / 2;
    y[4] = window_attributes.height / 2;
    y[5] = bottom;
    y[6] = bottom;
    y[7] = bottom;
    for (index = 0; index < HANDLE_COUNT; index++) {
        if (!wait_for_moveresize(
                display,
                window,
                output_descriptor,
                window_root_x + x[index],
                window_root_y + y[index],
                directions[index],
                output,
                output_capacity,
                output_length)) {
            fprintf(
                stderr,
                "GTK3 resize handle %zu did not request direction %ld\n",
                index,
                directions[index]);
            return false;
        }
        XFixesCursorImage *cursor = XFixesGetCursorImage(display);
        bool cursor_matches = cursor != NULL && cursor->name != NULL &&
            (strcmp(cursor->name, cursor_names[index]) == 0 ||
             strcmp(cursor->name, cursor_aliases[index]) == 0);
        if (cursor != NULL) {
            if (!cursor_matches) {
                fprintf(stderr, "GTK3 resize handle %zu cursor: %s, expected %s\n",
                    index, cursor->name == NULL ? "unnamed" : cursor->name,
                    cursor_names[index]);
            }
            XFree(cursor);
        }
        if (!cursor_matches) {
            return false;
        }
    }
    /* Returning to the terminal must not inherit any resize handle's cursor. */
    XFixesSelectCursorInput(display, DefaultRootWindow(display),
        XFixesDisplayCursorNotifyMask);
    XTestFakeMotionEvent(display, DefaultScreen(display),
        window_root_x + window_attributes.width / 2,
        window_root_y + window_attributes.height / 2, CurrentTime);
    XSync(display, False);
    uint64_t deadline = monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;
    for (;;) {
        XFixesCursorImage *cursor = XFixesGetCursorImage(display);
        bool cursor_matches = cursor != NULL;
        if (cursor != NULL && cursor->name != NULL) {
            for (index = 0; index < HANDLE_COUNT; index++) {
                if (strcmp(cursor->name, cursor_names[index]) == 0 ||
                    strcmp(cursor->name, cursor_aliases[index]) == 0) {
                    cursor_matches = false;
                }
            }
        }
        if (cursor != NULL) XFree(cursor);
        if (cursor_matches) break;
        if (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
            continue;
        }
        struct pollfd descriptor = {ConnectionNumber(display), POLLIN, 0};
        int timeout = remaining_timeout(deadline);
        if (timeout == 0 || poll(&descriptor, 1, timeout) <= 0) {
            fprintf(stderr, "GTK3 terminal inherited the resize cursor\n");
            return false;
        }
    }
    XFixesSelectCursorInput(display, DefaultRootWindow(display), 0);
    *resize_edge_x_out = right;
    *scrollbar_x_out =
        right - GTK3_SAMPLE_RESIZE_BORDER / 2 - 3;
    return true;
}

static bool set_fake_maximized(
    Display *display,
    Window window,
    bool maximized,
    unsigned int width,
    unsigned int height)
{
    Atom state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom maximized_atoms[2];

    maximized_atoms[0] =
        XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    maximized_atoms[1] =
        XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    if (state == None || maximized_atoms[0] == None ||
        maximized_atoms[1] == None || width == 0 || height == 0) {
        return false;
    }
    if (maximized) {
        XChangeProperty(
            display,
            window,
            state,
            XA_ATOM,
            32,
            PropModeReplace,
            (unsigned char *)maximized_atoms,
            2);
    } else {
        XDeleteProperty(display, window, state);
    }
    XResizeWindow(display, window, width, height);
    XSync(display, False);
    return true;
}

static bool wait_for_window_geometry(
    Display *display,
    Window window,
    int output_descriptor,
    int expected_width,
    int expected_height,
    char *output,
    size_t output_capacity,
    size_t *output_length)
{
    uint64_t deadline =
        monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;

    for (;;) {
        while (XPending(display) > 0) {
            XEvent event;
            XWindowAttributes attributes;

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
            if (XGetWindowAttributes(display, window, &attributes) != 0 &&
                attributes.width == expected_width &&
                attributes.height == expected_height) {
                return true;
            }
        }
        {
            XWindowAttributes attributes;

            if (XGetWindowAttributes(display, window, &attributes) != 0 &&
                attributes.width == expected_width &&
                attributes.height == expected_height) {
                return true;
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
            if (result <= 0 ||
                ((descriptors[1].revents &
                  (POLLIN | POLLHUP | POLLERR)) != 0 &&
                 !drain_output(
                     output_descriptor,
                     output,
                     output_capacity,
                     output_length)) ||
                (descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
                return false;
            }
        }
    }
}

static bool validate_geometry_trace(const char *path)
{
    FILE *input = fopen(path, "r");
    char line[512];
    uint64_t previous_sequence = 0;
    uint64_t previous_time = 0;
    size_t pending_rows = 0;
    size_t pending_columns = 0;
    bool saw_native_configure = false;
    bool saw_window_allocation = false;
    bool saw_content_allocation = false;
    bool saw_terminal_allocation = false;
    bool saw_draw = false;
    bool saw_initial_normal = false;
    bool saw_maximized = false;
    bool saw_restored_normal = false;
    bool saw_resize_pair = false;
    bool saw_maximized_resize = false;
    bool saw_restored_resize = false;

    if (input == NULL || fgets(line, sizeof(line), input) == NULL ||
        strcmp(
            line,
            "sequence,monotonic_us,event,state,native_width,native_height,"
            "window_width,window_height,content_width,content_height,"
            "terminal_width,terminal_height,rows,columns\n") != 0) {
        if (input != NULL) {
            fclose(input);
        }
        return false;
    }
    while (fgets(line, sizeof(line), input) != NULL) {
        unsigned long long sequence;
        unsigned long long time;
        char event[32];
        char state[16];
        int native_width;
        int native_height;
        int window_width;
        int window_height;
        int content_width;
        int content_height;
        int terminal_width;
        int terminal_height;
        size_t rows;
        size_t columns;

        if (sscanf(
                line,
                "%llu,%llu,%31[^,],%15[^,],%d,%d,%d,%d,%d,%d,%d,%d,%zu,%zu",
                &sequence,
                &time,
                event,
                state,
                &native_width,
                &native_height,
                &window_width,
                &window_height,
                &content_width,
                &content_height,
                &terminal_width,
                &terminal_height,
                &rows,
                &columns) != 14 ||
            sequence != previous_sequence + 1 || time < previous_time) {
            fclose(input);
            return false;
        }
        previous_sequence = sequence;
        previous_time = time;
        if (strcmp(event, "native-configure") == 0) {
            saw_native_configure = native_width > 0 && native_height > 0;
        } else if (strcmp(event, "window-allocation") == 0) {
            saw_window_allocation =
                saw_window_allocation ||
                (window_width > 0 && window_height > 0);
        } else if (strcmp(event, "content-allocation") == 0) {
            saw_content_allocation =
                saw_content_allocation ||
                (content_width > 0 && content_height > 0);
        } else if (strcmp(event, "terminal-allocation") == 0) {
            saw_terminal_allocation =
                saw_terminal_allocation ||
                (terminal_width > 0 && terminal_height > 0);
        } else if (strcmp(event, "draw") == 0) {
            saw_draw = saw_draw ||
                       (terminal_width > 0 && terminal_height > 0);
        } else if (strcmp(event, "pty-resize") == 0) {
            if (pending_rows != 0 || rows == 0 || columns == 0 ||
                window_width != content_width) {
                if (window_width != content_width) {
                    fprintf(
                        stderr,
                        "GTK3 PTY resize retained %dpx of horizontal "
                        "outer decoration\n",
                        window_width - content_width);
                }
                fclose(input);
                return false;
            }
            pending_rows = rows;
            pending_columns = columns;
            saw_maximized_resize =
                saw_maximized_resize || strcmp(state, "maximized") == 0;
            saw_restored_resize =
                saw_restored_resize ||
                (saw_maximized && strcmp(state, "normal") == 0);
        } else if (strcmp(event, "grid-complete") == 0) {
            if (pending_rows != rows || pending_columns != columns) {
                fclose(input);
                return false;
            }
            pending_rows = 0;
            pending_columns = 0;
            saw_resize_pair = true;
        }
        if (strcmp(event, "state") == 0) {
            if (strcmp(state, "normal") == 0 && !saw_maximized) {
                saw_initial_normal = true;
            } else if (strcmp(state, "maximized") == 0 &&
                       saw_initial_normal) {
                saw_maximized = true;
            } else if (strcmp(state, "normal") == 0 && saw_maximized) {
                saw_restored_normal = true;
            }
        }
    }
    fclose(input);
    return previous_sequence != 0 && pending_rows == 0 &&
           saw_native_configure && saw_window_allocation &&
           saw_content_allocation && saw_terminal_allocation && saw_draw &&
           saw_resize_pair && saw_initial_normal && saw_maximized &&
           saw_restored_normal && saw_maximized_resize &&
           saw_restored_resize;
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

static bool window_owns_frame_chrome(Display *display, Window window)
{
    enum { MWM_HINTS_DECORATIONS = 1L << 1 };
    Atom motif_hints =
        XInternAtom(display, "_MOTIF_WM_HINTS", False);
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *property = NULL;
    bool owns_frame;

    if (motif_hints == None ||
        XGetWindowProperty(
            display,
            window,
            motif_hints,
            0,
            5,
            False,
            motif_hints,
            &actual_type,
            &actual_format,
            &item_count,
            &bytes_after,
            &property) != Success) {
        return false;
    }
    owns_frame = actual_type == motif_hints && actual_format == 32 &&
                 item_count >= 3 && bytes_after == 0 && property != NULL &&
                 (((unsigned long *)property)[0] & MWM_HINTS_DECORATIONS) != 0 &&
                 ((unsigned long *)property)[2] == 0;
    if (property != NULL) {
        XFree(property);
    }
    return owns_frame;
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

static bool read_window_alpha(
    Display *display,
    Window window,
    const XWindowAttributes *attributes,
    int x,
    int y,
    unsigned int *alpha_out,
    unsigned int *opaque_alpha_out)
{
    XRenderPictFormat *format;
    XImage *image;
    unsigned long pixel;

    if (display == NULL || window == None || attributes == NULL ||
        x < 0 || x >= attributes->width || y < 0 ||
        y >= attributes->height || alpha_out == NULL ||
        opaque_alpha_out == NULL) {
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
        x,
        y,
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
    *opaque_alpha_out = (unsigned int)format->direct.alphaMask;
    XDestroyImage(image);
    return true;
}

static bool wait_for_background_and_scrollbar_alpha(
    Display *display,
    Window window,
    Damage dirty,
    int dirty_event_base,
    int output_descriptor,
    int resize_edge_x,
    int scrollbar_x,
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
                unsigned int background_alpha;
                unsigned int background_opaque_alpha;
                unsigned int scrollbar_alpha;
                unsigned int scrollbar_opaque_alpha;
                unsigned int resize_edge_alpha;
                unsigned int resize_edge_opaque_alpha;

                if (notification->drawable == window &&
                    notification->damage == dirty) {
                    XDamageSubtract(display, dirty, None, None);
                    if (XGetWindowAttributes(
                            display,
                            window,
                            &attributes) != 0 &&
                        attributes.width >= 3 &&
                        read_window_alpha(
                            display,
                            window,
                            &attributes,
                            attributes.width / 2,
                            attributes.height / 2,
                            &background_alpha,
                            &background_opaque_alpha) &&
                        read_window_alpha(
                            display,
                            window,
                            &attributes,
                            resize_edge_x,
                            attributes.height / 2,
                            &resize_edge_alpha,
                            &resize_edge_opaque_alpha) &&
                        read_window_alpha(
                            display,
                            window,
                            &attributes,
                            scrollbar_x,
                            attributes.height / 2,
                            &scrollbar_alpha,
                            &scrollbar_opaque_alpha) &&
                        background_alpha == expected_alpha) {
                        return resize_edge_opaque_alpha ==
                                   background_opaque_alpha &&
                               resize_edge_alpha == expected_alpha &&
                               background_opaque_alpha ==
                                   scrollbar_opaque_alpha &&
                               scrollbar_alpha == scrollbar_opaque_alpha;
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
    bool send_query,
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
        (send_query &&
         (XStoreName(display, window, sentinel) == 0 ||
          !send_key(display, window, XK_g, false, false)))) {
        return false;
    }
    for (;;) {
        char *title = NULL;
        bool matches = false;
        bool retry = false;
        bool resized = false;
        bool event_received = false;

        XSync(display, False);
        if (XFetchName(display, window, &title) != 0 && title != NULL) {
            matches = strcmp(title, expected) == 0;
            retry = send_query && !matches &&
                    strncmp(title, "grid-", 5) == 0;
            XFree(title);
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            event_received = true;
            if (event.type == ConfigureRequest &&
                event.xconfigurerequest.window == window) {
                if (!accept_initial_resize ||
                    !apply_configure_request(
                        display,
                        &event.xconfigurerequest)) {
                    fprintf(
                        stderr,
                        "GTK3 sample requested %dx%d while awaiting %s\n",
                        event.xconfigurerequest.width,
                        event.xconfigurerequest.height,
                        expected);
                    return false;
                }
                resized = true;
            }
            if (event.type == DestroyNotify &&
                event.xdestroywindow.window == window) {
                return false;
            }
        }
        if (send_query && (resized || retry)) {
            if (XStoreName(display, window, sentinel) == 0 ||
                !send_key(display, window, XK_g, false, false)) {
                return false;
            }
            matches = false;
        }
        if (matches) {
            return true;
        }
        if (event_received) {
            continue;
        }
        {
            struct pollfd descriptors[2];
            int timeout = remaining_timeout(deadline);
            int result;

            if (timeout == 0) {
                char *title = NULL;

                if (XFetchName(display, window, &title) != 0 &&
                    title != NULL) {
                    fprintf(
                        stderr,
                        "GTK3 sample timed out awaiting %s; last title %s\n",
                        expected,
                        title);
                    XFree(title);
                }
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
                char *title = NULL;

                if (XFetchName(display, window, &title) != 0 &&
                    title != NULL) {
                    fprintf(
                        stderr,
                        "GTK3 sample poll timed out awaiting %s; "
                        "last title %s\n",
                        expected,
                        title);
                    XFree(title);
                }
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

static bool query_larger_grid_without_resize_request(
    Display *display,
    Window window,
    int output_descriptor,
    unsigned int previous_rows,
    unsigned int previous_columns,
    char *output,
    size_t output_capacity,
    size_t *output_length)
{
    static const char sentinel[] = "awaiting-maximized-grid";
    uint64_t deadline =
        monotonic_milliseconds() + TEST_TIMEOUT_MILLISECONDS;

    if (XStoreName(display, window, sentinel) == 0 ||
        !send_key(display, window, XK_g, false, false)) {
        return false;
    }
    for (;;) {
        char *title = NULL;
        bool retry = false;
        bool event_received = false;

        XSync(display, False);
        if (XFetchName(display, window, &title) != 0 && title != NULL) {
            unsigned int rows;
            unsigned int columns;
            char trailing;

            if (sscanf(
                    title,
                    "grid-%u-%u%c",
                    &rows,
                    &columns,
                    &trailing) == 2) {
                if (rows > previous_rows && columns > previous_columns) {
                    XFree(title);
                    return true;
                }
                retry = true;
            }
            XFree(title);
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            event_received = true;
            if (event.type == ConfigureRequest &&
                event.xconfigurerequest.window == window) {
                if (!apply_configure_request(
                        display,
                        &event.xconfigurerequest)) {
                    return false;
                }
                retry = true;
            } else if (event.type == DestroyNotify &&
                       event.xdestroywindow.window == window) {
                return false;
            }
        }
        if (retry &&
            (XStoreName(display, window, sentinel) == 0 ||
             !send_key(display, window, XK_g, false, false))) {
            return false;
        }
        if (event_received) {
            continue;
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
            if (result <= 0 ||
                ((descriptors[1].revents &
                  (POLLIN | POLLHUP | POLLERR)) != 0 &&
                 !drain_output(
                     output_descriptor,
                     output,
                     output_capacity,
                     output_length)) ||
                (descriptors[1].revents & (POLLHUP | POLLERR)) != 0) {
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
        bool retry = false;
        bool event_received = false;

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
            retry = !matches && strncmp(title, "grid-", 5) == 0;
            XFree(title);
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            event_received = true;
            if ((event.type == ConfigureRequest &&
                 event.xconfigurerequest.window == window) ||
                (event.type == DestroyNotify &&
                 event.xdestroywindow.window == window)) {
                return false;
            }
        }
        if (retry &&
            (XStoreName(display, window, "awaiting-cell-geometry") == 0 ||
             !send_key(display, window, XK_p, false, false))) {
            return false;
        }
        if (matches) {
            *height_out = height;
            *width_out = width;
            return true;
        }
        if (event_received) {
            continue;
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
        bool event_received = false;

        XSync(display, False);
        if (XFetchName(display, window, &title) != 0 && title != NULL) {
            matches = strcmp(title, expected) == 0;
            XFree(title);
        }
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            event_received = true;
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
        if (event_received) {
            continue;
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
    static const KeySym control_symbols[] = {
        XK_2,
        XK_3,
        XK_slash,
        XK_question,
        XK_c,
        XK_z};
    static const bool control_shift[] = {
        false,
        false,
        false,
        true,
        false,
        false};
    Display *display = NULL;
    Window root;
    Window window = None;
    XWindowAttributes attributes = {0};
    XWindowAttributes resized_attributes = {0};
    XWindowAttributes zoomed_attributes = {0};
    XWindowAttributes reset_attributes = {0};
    Damage dirty = None;
    pid_t process = -1;
    int output_descriptor = -1;
    int dirty_event_base;
    int dirty_error_base;
    unsigned int cell_height = 0;
    unsigned int cell_width = 0;
    int resize_edge_x = 0;
    int scrollbar_x = 0;
    int status = 0;
    char output[4096] = "";
    size_t output_length = 0;
    bool passed = false;
    size_t control_index;
    uint64_t allocation_extent_generation;
    char trace_path[PATH_MAX];
    const char *artifact_directory;
    int trace_descriptor = -1;
    bool remove_trace = false;

    if (argument_count != 3) {
        fprintf(stderr, "usage: %s GTK3_SAMPLE PTY_HELPER\n", arguments[0]);
        return 2;
    }
    if (boxrig_term_gtk3_dispatch_needs_immediate_wake(
            false,
            false,
            false) ||
        boxrig_term_gtk3_dispatch_needs_immediate_wake(
            false,
            false,
            true) ||
        !boxrig_term_gtk3_dispatch_needs_immediate_wake(
            false,
            true,
            true) ||
        !boxrig_term_gtk3_dispatch_needs_immediate_wake(
            true,
            false,
            false)) {
        fprintf(
            stderr,
            "unmapped GTK3 frame request caused an immediate wakeup\n");
        return 1;
    }
    if (boxrig_term_gtk3_descriptor_events_need_update(5u, 5u) ||
        !boxrig_term_gtk3_descriptor_events_need_update(5u, 7u)) {
        fprintf(stderr, "unchanged GTK3 descriptor events caused a wakeup\n");
        return 1;
    }
    allocation_extent_generation =
        boxrig_term_gtk3_allocation_extent_generation(0, 0, 0, 3840, 2160);
    if (allocation_extent_generation != 1 ||
        boxrig_term_gtk3_allocation_extent_generation(
            allocation_extent_generation,
            3840,
            2160,
            3840,
            2160) != allocation_extent_generation ||
        boxrig_term_gtk3_allocation_extent_generation(
            allocation_extent_generation,
            3840,
            2160,
            602,
            402) != 2 ||
        boxrig_term_gtk3_allocation_extent_generation(
            UINT64_MAX,
            602,
            402,
            3840,
            2160) != 1) {
        fprintf(stderr, "GTK3 restore extent generation was not stable\n");
        return 1;
    }
    if (!boxrig_term_gtk3_normal_state_remains_deferred(
            true,
            false,
            BOXRIG_UI_WINDOW_MAXIMIZED,
            BOXRIG_UI_WINDOW_NORMAL) ||
        !boxrig_term_gtk3_normal_state_remains_deferred(
            true,
            true,
            BOXRIG_UI_WINDOW_NORMAL,
            BOXRIG_UI_WINDOW_NORMAL) ||
        boxrig_term_gtk3_normal_state_remains_deferred(
            true,
            true,
            BOXRIG_UI_WINDOW_NORMAL,
            BOXRIG_UI_WINDOW_MAXIMIZED) ||
        boxrig_term_gtk3_normal_state_remains_deferred(
            true,
            false,
            BOXRIG_UI_WINDOW_NORMAL,
            BOXRIG_UI_WINDOW_NORMAL)) {
        fprintf(stderr, "GTK3 repeated NORMAL state bypassed restore wait\n");
        return 1;
    }
    display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "could not open the X11 display\n");
        goto cleanup;
    }
    root = DefaultRootWindow(display);
    if (!configure_fake_window_manager(display, root)) {
        fprintf(stderr, "could not configure the GTK3 test window manager\n");
        goto cleanup;
    }
    XSelectInput(
        display,
        root,
        SubstructureNotifyMask | SubstructureRedirectMask);
    XSync(display, False);
    process = start_sample(arguments[1], arguments[2], NULL, &output_descriptor);
    if (process == -1) {
        perror("start GTK3 sample");
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
        fprintf(stderr, "GTK3 sample did not map a window\n%s", output);
        goto cleanup;
    }
    if (XGetWindowAttributes(display, window, &attributes) == 0 ||
        attributes.map_state != IsViewable || attributes.width <= 1 ||
        attributes.height <= 1 ||
        !window_owns_frame_chrome(display, window)) {
        fprintf(
            stderr,
            "GTK3 sample did not keep its titlebar and resize border in "
            "the client frame\n");
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
        fprintf(stderr, "GTK3 sample did not close after WM_DELETE\n%s", output);
        goto cleanup;
    }
    process = -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "GTK3 sample exited abnormally\n%s", output);
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

    artifact_directory =
        getenv("BOXRIG_TEST_GTK3_GEOMETRY_ARTIFACT_DIR");
    if (artifact_directory != NULL && artifact_directory[0] != '\0') {
        int path_length = snprintf(
                trace_path,
                sizeof(trace_path),
                "%s/gtk3-geometry.csv",
                artifact_directory);

        if (path_length <= 0 || (size_t)path_length >= sizeof(trace_path)) {
            fprintf(stderr, "could not construct GTK3 geometry trace path\n");
            goto cleanup;
        }
    } else {
        memcpy(
            trace_path,
            "/tmp/boxrig-gtk3-geometry.XXXXXX",
            sizeof("/tmp/boxrig-gtk3-geometry.XXXXXX"));
        trace_descriptor = mkstemp(trace_path);
        if (trace_descriptor == -1 || close(trace_descriptor) == -1) {
            trace_descriptor = -1;
            fprintf(stderr, "could not create GTK3 geometry trace\n");
            goto cleanup;
        }
        trace_descriptor = -1;
        remove_trace = true;
    }

    process = start_sample(
        arguments[1], arguments[2], trace_path, &output_descriptor);
    if (process == -1) {
        perror("restart GTK3 sample");
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
        fprintf(stderr, "GTK3 sample did not remap a window\n%s", output);
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
            true,
            output,
            sizeof(output),
            &output_length) ||
        XGetWindowAttributes(display, window, &attributes) == 0) {
        fprintf(
            stderr,
            "GTK3 sample did not start with a 24x80 grid\n%s",
            output);
        goto cleanup;
    }
    if (!verify_inner_resize_handles(
            display,
            window,
            output_descriptor,
            trace_path,
            &resize_edge_x,
            &scrollbar_x,
            output,
            sizeof(output),
            &output_length)) {
        fprintf(
            stderr,
            "GTK3 sample did not expose eight working inner resize handles\n%s",
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
        fprintf(stderr, "could not observe GTK3 sample dirty\n");
        goto cleanup;
    }
    XDamageSubtract(display, dirty, None, None);
    XSync(display, False);
    if (!send_key(display, window, XK_o, true, true) ||
        !wait_for_background_and_scrollbar_alpha(
            display,
            window,
            dirty,
            dirty_event_base,
            output_descriptor,
            resize_edge_x,
            scrollbar_x,
            204,
            output,
            sizeof(output),
            &output_length)) {
        fprintf(
            stderr,
            "GTK3 sample did not preserve 80%% background alpha with an "
            "opaque focused scrollbar\n%s",
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
            "GTK3 sample did not report its cell geometry\n%s",
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
            "GTK3 sample corrected the first user allocation "
            "(%dx%d, expected %dx%d)\n%s",
            resized_attributes.width,
            resized_attributes.height,
            attributes.width + (int)(cell_width * 2u) + 1,
            attributes.height + (int)cell_height + 1,
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
            "GTK3 sample did not preserve the user-resized 26x84 grid\n%s",
            output);
        goto cleanup;
    }
    if (!send_key(display, window, XK_w, false, true) ||
        !wait_for_title(
            display,
            window,
            output_descriptor,
            "resize-report-stopped",
            output,
            sizeof(output),
            &output_length)) {
        fprintf(stderr, "GTK3 sample did not stop its resize report\n%s", output);
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
            true,
            output,
            sizeof(output),
            &output_length)) {
        fprintf(
            stderr,
            "GTK3 Ctrl+Plus did not resize once while preserving 26x84\n%s",
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
            true,
            output,
            sizeof(output),
            &output_length)) {
        fprintf(
            stderr,
            "GTK3 Ctrl+0 did not restore once while preserving 26x84\n%s",
            output);
        goto cleanup;
    }
    if (!send_key(display, window, XK_k, false, true) ||
        !wait_for_title(
            display,
            window,
            output_descriptor,
            "control-input-ready",
            output,
            sizeof(output),
            &output_length)) {
        fprintf(
            stderr,
            "GTK3 control input did not reach the raw PTY fixture\n%s",
            output);
        goto cleanup;
    }
    for (control_index = 0;
         control_index < sizeof(control_symbols) / sizeof(control_symbols[0]);
         control_index++) {
        char expected_title[64];
        int expected_length;

        expected_length = snprintf(
            expected_title,
            sizeof(expected_title),
            control_index + 1 ==
                    sizeof(control_symbols) / sizeof(control_symbols[0])
                ? "control-input-received"
                : "control-input-%zu",
            control_index + 1);
        if (expected_length <= 0 ||
            (size_t)expected_length >= sizeof(expected_title) ||
            !send_key(
                display,
                window,
                control_symbols[control_index],
                true,
                control_shift[control_index]) ||
            !wait_for_title(
                display,
                window,
                output_descriptor,
                expected_title,
                output,
                sizeof(output),
                &output_length)) {
            fprintf(
                stderr,
                "GTK3 control input %zu did not reach the raw PTY fixture\n%s",
                control_index + 1,
                output);
            goto cleanup;
        }
    }
    {
        XWindowAttributes transition_attributes;
        const char *failure = NULL;
        int maximized_width = DisplayWidth(display, DefaultScreen(display));
        int maximized_height = DisplayHeight(display, DefaultScreen(display));

        if (XGetWindowAttributes(
                display, window, &transition_attributes) == 0) {
            failure = "read-normal-bounds";
        } else if (!set_fake_maximized(
                display,
                window,
                true,
                (unsigned int)maximized_width,
                (unsigned int)maximized_height)) {
            failure = "request-maximize";
        } else if (!wait_for_window_geometry(
                display,
                window,
                output_descriptor,
                maximized_width,
                maximized_height,
                output,
                sizeof(output),
                &output_length)) {
            failure = "observe-maximized-geometry";
        } else if (!query_larger_grid_without_resize_request(
                display,
                window,
                output_descriptor,
                26,
                84,
                output,
                sizeof(output),
                &output_length)) {
            failure = "observe-maximized-grid";
        } else if (!set_fake_maximized(
                display,
                window,
                false,
                (unsigned int)transition_attributes.width,
                (unsigned int)transition_attributes.height)) {
            failure = "request-restore";
        } else if (!wait_for_window_geometry(
                display,
                window,
                output_descriptor,
                transition_attributes.width,
                transition_attributes.height,
                output,
                sizeof(output),
                &output_length)) {
            failure = "observe-restored-geometry";
        } else if (!query_grid_without_resize_request(
                display,
                window,
                output_descriptor,
                "awaiting-restored-grid",
                26,
                84,
                false,
                true,
                output,
                sizeof(output),
                &output_length)) {
            failure = "verify-restored-grid";
        }
        if (failure != NULL) {
            fprintf(
                stderr,
                "GTK3 geometry transition failed at %s\n%s",
                failure,
                output);
            goto cleanup;
        }
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
            "GTK3 sample did not exit after zoom and reset\n%s",
            output);
        goto cleanup;
    }
    process = -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "GTK3 sample exited abnormally\n%s", output);
        goto cleanup;
    }
    if (!validate_geometry_trace(trace_path)) {
        fprintf(stderr, "GTK3 geometry trace was incomplete or unordered\n");
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
    if (trace_descriptor != -1) {
        close(trace_descriptor);
    }
    if (remove_trace && trace_path[0] != '\0') {
        unlink(trace_path);
    }
    if (display != NULL) {
        XCloseDisplay(display);
    }
    if (!passed) {
        return 1;
    }
    printf(
        "PASS GTK3 sample traces maximize/restore geometry and preserves "
        "user resize, alpha, opaque scrollbar, and zoom grid\n");
    return 0;
}
