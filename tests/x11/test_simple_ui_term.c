#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Timeouts are watchdogs; every assertion waits for an observable event. */
static bool wait_descriptor(int descriptor)
{
    struct pollfd entry = { descriptor, POLLIN, 0 };
    int result;

    do {
        result = poll(&entry, 1, 60000);
    } while (result == -1 && errno == EINTR);
    return result > 0 && (entry.revents & (POLLIN | POLLHUP)) != 0;
}

static bool read_line(int descriptor, char *line, size_t capacity)
{
    size_t length = 0;

    while (length + 1 < capacity) {
        if (!wait_descriptor(descriptor) ||
            read(descriptor, line + length, 1) != 1) {
            return false;
        }
        if (line[length++] == '\n') {
            line[length] = '\0';
            return true;
        }
    }
    return false;
}

static Window wait_window(Display *display)
{
    for (;;) {
        Window root;
        Window parent;
        Window *children = NULL;
        unsigned int count = 0;
        Window found = None;

        if (!XQueryTree(display, DefaultRootWindow(display),
                        &root, &parent, &children, &count)) {
            return None;
        }
        for (unsigned int index = 0; index < count; index++) {
            char *name = NULL;
            XWindowAttributes attributes;

            if (XFetchName(display, children[index], &name) && name != NULL &&
                strncmp(name, "boxrig UI guide", 15) == 0 &&
                XGetWindowAttributes(display, children[index], &attributes) &&
                attributes.map_state == IsViewable) {
                found = children[index];
            }
            if (name != NULL) {
                XFree(name);
            }
        }
        if (children != NULL) {
            XFree(children);
        }
        if (found != None) {
            return found;
        }
        if (XPending(display) == 0 &&
            !wait_descriptor(ConnectionNumber(display))) {
            return None;
        }
        if (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
        }
    }
}

static bool send_text(Display *display, Window window, const char *text)
{
    XSetInputFocus(display, window, RevertToParent, CurrentTime);
    for (size_t index = 0; text[index] != '\0'; index++) {
        XEvent event = {0};
        KeySym symbol = text[index] == '\n' ? XK_Return : (KeySym)text[index];

        event.xkey.display = display;
        event.xkey.window = window;
        event.xkey.root = DefaultRootWindow(display);
        event.xkey.same_screen = True;
        event.xkey.keycode = XKeysymToKeycode(display, symbol);
        if (event.xkey.keycode == 0) {
            return false;
        }
        event.type = KeyPress;
        if (!XSendEvent(display, window, False, KeyPressMask, &event)) {
            return false;
        }
        event.type = KeyRelease;
        if (!XSendEvent(display, window, False, KeyReleaseMask, &event)) {
            return false;
        }
    }
    XFlush(display);
    return true;
}

int main(int argument_count, char **arguments)
{
    /* Descriptor 3 reports the child's observations outside its PTY. */
    static const char script[] =
        "printf '\033[31mHello boxrig UI!\033[0m\r\n'; "
        "trap 'printf \"resize \" >&3; stty size >&3' WINCH; "
        "printf 'ready %s ' \"$TERM\" >&3; stty size >&3; "
        "IFS= read -r line; printf 'input %s\n' \"$line\" >&3; "
        "i=0; while [ $i -lt 300 ]; do "
        "printf '\033]0;ignored\007\033]52;c;aGVsbG8=\007'; i=$((i+1)); done; "
        "printf 'host-drained\n' >&3; "
        "while :; do IFS= read -r line || continue; "
        "[ \"$line\" = exit ] && exit 0; done";
    bool hello = argument_count == 3 && strcmp(arguments[1], "--hello") == 0;
    bool closing = argument_count == 3 && strcmp(arguments[1], "--close") == 0;
    Display *display = NULL;
    pid_t process = -1;
    int reports[2] = {-1, -1};
    int result = 1;
    Window window;
    Window root;
    Window parent;
    Window *children = NULL;
    unsigned int child_count = 0;
    Window component;
    char line[256];
    int status;

    if (argument_count != 3 ||
        (!hello && !closing && strcmp(arguments[1], "--pty") != 0)) {
        fprintf(stderr, "usage: %s --hello|--pty|--close PROGRAM\n", arguments[0]);
        return 2;
    }
    display = XOpenDisplay(NULL);
    if (display == NULL || pipe(reports) == -1) {
        goto cleanup;
    }
    XSelectInput(display, DefaultRootWindow(display), SubstructureNotifyMask);
    XSync(display, False);
    process = fork();
    if (process == -1) {
        goto cleanup;
    }
    if (process == 0) {
        close(ConnectionNumber(display));
        close(reports[0]);
        if (dup2(reports[1], 3) == -1) {
            _exit(127);
        }
        if (reports[1] != 3) {
            close(reports[1]);
        }
        if (hello) {
            execl(arguments[2], arguments[2], (char *)NULL);
        } else {
            execl(arguments[2], arguments[2], "/bin/sh", "-c", script,
                  (char *)NULL);
        }
        _exit(127);
    }
    close(reports[1]);
    reports[1] = -1;
    window = wait_window(display);
    if (window == None ||
        !XQueryTree(display, window, &root, &parent, &children, &child_count) ||
        child_count != 1) {
        fprintf(stderr, "FAIL visible parent with one terminal component\n");
        goto cleanup;
    }
    component = children[0];
    XFree(children);
    children = NULL;
    if (!hello) {
        size_t rows = 0;
        size_t columns = 0;

        if (!read_line(reports[0], line, sizeof(line)) ||
            strcmp(line, "ready xterm 25 80\n") != 0) {
            fprintf(stderr, "FAIL initial TERM and PTY grid\n");
            goto cleanup;
        }
        if (!closing) {
            if (!send_text(display, component, "hello\n") ||
                !read_line(reports[0], line, sizeof(line)) ||
                strcmp(line, "input hello\n") != 0 ||
                !read_line(reports[0], line, sizeof(line)) ||
                strcmp(line, "host-drained\n") != 0) {
                fprintf(stderr, "FAIL input or host queue\n");
                goto cleanup;
            }
            XResizeWindow(display, window, 1600, 1000);
            XFlush(display);
            if (!read_line(reports[0], line, sizeof(line)) ||
                sscanf(line, "resize %zu %zu", &rows, &columns) != 2 ||
                rows <= 25 || columns <= 80 ||
                !send_text(display, component, "exit\n")) {
                fprintf(stderr, "FAIL PTY grid follows parent allocation\n");
                goto cleanup;
            }
        }
    }
    if (hello || closing) {
        XEvent event = {0};

        event.xclient.type = ClientMessage;
        event.xclient.window = window;
        event.xclient.message_type = XInternAtom(display, "WM_PROTOCOLS", False);
        event.xclient.format = 32;
        event.xclient.data.l[0] =
            (long)XInternAtom(display, "WM_DELETE_WINDOW", False);
        XSendEvent(display, window, False, NoEventMask, &event);
        XFlush(display);
    }
    /* The report descriptor closes only after the application releases the PTY. */
    bool clean_exit = false;

    if (wait_descriptor(reports[0]) && waitpid(process, &status, 0) == process) {
        process = -1;
        clean_exit = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    if (!clean_exit) {
        fprintf(stderr, "FAIL clean application shutdown\n");
        goto cleanup;
    }
    puts(closing ? "PASS simple UI terminal: window close terminates child" :
         hello ? "PASS UI guide component creation and window close" :
                "PASS simple UI terminal: TERM, input, host fallback, resize, EOF");
    result = 0;

cleanup:
    if (children != NULL) {
        XFree(children);
    }
    if (process > 0) {
        kill(process, SIGKILL);
        waitpid(process, NULL, 0);
    }
    if (reports[0] != -1) {
        close(reports[0]);
    }
    if (reports[1] != -1) {
        close(reports[1]);
    }
    if (display != NULL) {
        XCloseDisplay(display);
    }
    return result;
}
