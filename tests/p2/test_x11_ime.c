/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "../x11_video.h"

enum {
    PROCESS_TIMEOUT_MILLISECONDS = 15000,
    INPUT_DELAY_MILLISECONDS = 45,
    RESULT_CAPACITY = 4096
};

typedef struct test_processes {
    pid_t xvfb;
    pid_t dbus;
    pid_t ibus;
    pid_t sample;
    int sample_output;
} test_processes;

static void pause_milliseconds(long milliseconds)
{
    struct timespec requested;

    requested.tv_sec = milliseconds / 1000;
    requested.tv_nsec = (milliseconds % 1000) * 1000000L;
    while (nanosleep(&requested, &requested) == -1 && errno == EINTR) {
    }
}

static int remove_entry(
    const char *path,
    const struct stat *status,
    int type,
    struct FTW *walk)
{
    (void)status;
    (void)type;
    (void)walk;
    return remove(path);
}

static void remove_tree(const char *path)
{
    if (path != NULL && path[0] != '\0') {
        (void)nftw(path, remove_entry, 32, FTW_DEPTH | FTW_PHYS);
    }
}

static bool make_directory(const char *path)
{
    return mkdir(path, 0700) == 0;
}

static bool write_text_file(const char *path, const char *text)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    size_t length = strlen(text);
    size_t offset = 0;

    if (descriptor == -1) {
        return false;
    }
    while (offset < length) {
        ssize_t written = write(descriptor, text + offset, length - offset);

        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            close(descriptor);
            return false;
        }
        offset += (size_t)written;
    }
    return close(descriptor) == 0;
}

static pid_t start_xvfb(char *display_name, size_t capacity)
{
    int descriptor_pipe[2];
    pid_t process;
    char number[32];
    size_t length = 0;

    if (pipe(descriptor_pipe) == -1) {
        return -1;
    }
    process = fork();
    if (process == -1) {
        close(descriptor_pipe[0]);
        close(descriptor_pipe[1]);
        return -1;
    }
    if (process == 0) {
        char descriptor[16];

        close(descriptor_pipe[0]);
        if (snprintf(
                descriptor,
                sizeof(descriptor),
                "%d",
                descriptor_pipe[1]) < 0) {
            _exit(127);
        }
        execlp(
            "Xvfb",
            "Xvfb",
            "-displayfd",
            descriptor,
            "-screen",
            "0",
            "1024x768x24",
            "-nolisten",
            "tcp",
            "-noreset",
            (char *)NULL);
        _exit(127);
    }
    close(descriptor_pipe[1]);
    while (length + 1 < sizeof(number)) {
        ssize_t count = read(
            descriptor_pipe[0],
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
    close(descriptor_pipe[0]);
    if (length == 0) {
        kill(process, SIGTERM);
        waitpid(process, NULL, 0);
        return -1;
    }
    number[length] = '\0';
    number[strcspn(number, "\r\n")] = '\0';
    if (snprintf(display_name, capacity, ":%s", number) < 0) {
        kill(process, SIGTERM);
        waitpid(process, NULL, 0);
        return -1;
    }
    return process;
}

static pid_t start_dbus(char *address, size_t capacity)
{
    int address_pipe[2];
    pid_t process;
    size_t length = 0;

    if (pipe(address_pipe) == -1) {
        return -1;
    }
    process = fork();
    if (process == -1) {
        close(address_pipe[0]);
        close(address_pipe[1]);
        return -1;
    }
    if (process == 0) {
        char descriptor[16];

        close(address_pipe[0]);
        if (snprintf(
                descriptor,
                sizeof(descriptor),
                "%d",
                address_pipe[1]) < 0) {
            _exit(127);
        }
        execlp(
            "dbus-daemon",
            "dbus-daemon",
            "--session",
            "--nofork",
            "--nopidfile",
            "--print-address",
            descriptor,
            (char *)NULL);
        _exit(127);
    }
    close(address_pipe[1]);
    while (length + 1 < capacity) {
        ssize_t count = read(
            address_pipe[0],
            address + length,
            capacity - length - 1);

        if (count <= 0) {
            break;
        }
        length += (size_t)count;
        if (memchr(address, '\n', length) != NULL) {
            break;
        }
    }
    close(address_pipe[0]);
    if (length == 0) {
        kill(process, SIGTERM);
        waitpid(process, NULL, 0);
        return -1;
    }
    address[length] = '\0';
    address[strcspn(address, "\r\n")] = '\0';
    return process;
}

static pid_t start_ibus(const char *log_path)
{
    pid_t process = fork();

    if (process == -1) {
        return -1;
    }
    if (process == 0) {
        int log_descriptor;

        (void)setpgid(0, 0);
        log_descriptor = open(
            log_path,
            O_WRONLY | O_CREAT | O_TRUNC,
            0600);
        if (log_descriptor == -1 ||
            dup2(log_descriptor, STDOUT_FILENO) == -1 ||
            dup2(log_descriptor, STDERR_FILENO) == -1) {
            _exit(127);
        }
        close(log_descriptor);
        execlp(
            "ibus-daemon",
            "ibus-daemon",
            "--xim",
            "--single",
            "--panel=disable",
            "--config=disable",
            "--emoji-extension=disable",
            "--address",
            getenv("IBUS_ADDRESS"),
            (char *)NULL);
        _exit(127);
    }
    (void)setpgid(process, process);
    return process;
}

static int wait_process(pid_t process, int timeout_milliseconds, int *status)
{
    int elapsed = 0;

    while (elapsed <= timeout_milliseconds) {
        pid_t result = waitpid(process, status, WNOHANG);

        if (result == process) {
            return 1;
        }
        if (result == -1) {
            return -1;
        }
        pause_milliseconds(25);
        elapsed += 25;
    }
    return 0;
}

static int run_command(char *const arguments[])
{
    pid_t process = fork();
    int status = 0;
    int result;

    if (process == -1) {
        return -1;
    }
    if (process == 0) {
        int null_descriptor = open("/dev/null", O_RDWR);

        if (null_descriptor == -1 ||
            dup2(null_descriptor, STDOUT_FILENO) == -1 ||
            dup2(null_descriptor, STDERR_FILENO) == -1) {
            _exit(127);
        }
        close(null_descriptor);
        execvp(arguments[0], arguments);
        _exit(127);
    }
    result = wait_process(process, 5000, &status);
    if (result == 0) {
        kill(process, SIGKILL);
        waitpid(process, &status, 0);
        return -1;
    }
    return result == 1 && WIFEXITED(status) && WEXITSTATUS(status) == 0
               ? 0
               : -1;
}

static bool wait_for_ibus(Display *display, pid_t ibus_process)
{
    Atom servers = XInternAtom(display, "XIM_SERVERS", False);
    int last_property_status = Success;
    Atom last_actual_type = None;
    int last_actual_format = 0;
    unsigned long last_item_count = 0;
    int attempt;

    for (attempt = 0; attempt < 150; attempt++) {
        int status;

        if (waitpid(ibus_process, &status, WNOHANG) == ibus_process) {
            return false;
        }
        {
            unsigned long remaining = 0;
            unsigned char *value = NULL;

            last_property_status = XGetWindowProperty(
                display,
                DefaultRootWindow(display),
                servers,
                0,
                32,
                False,
                XA_ATOM,
                &last_actual_type,
                &last_actual_format,
                &last_item_count,
                &remaining,
                &value);
            if (value != NULL) {
                XFree(value);
            }
            if (last_property_status == Success &&
                last_actual_type == XA_ATOM && last_actual_format == 32 &&
                last_item_count != 0) {
                XIM probe;

                (void)setlocale(LC_CTYPE, "");
                (void)XSetLocaleModifiers("");
                probe = XOpenIM(display, NULL, NULL, NULL);
                if (probe != NULL) {
                    XCloseIM(probe);
                    pause_milliseconds(250);
                    return true;
                }
            }
        }
        pause_milliseconds(100);
    }
    fprintf(
        stderr,
        "IBus readiness: property_status=%d type=%lu format=%d items=%lu\n",
        last_property_status,
        (unsigned long)last_actual_type,
        last_actual_format,
        last_item_count);
    return false;
}

static void print_file(const char *path)
{
    char buffer[1024];
    int descriptor = open(path, O_RDONLY);

    if (descriptor == -1) {
        return;
    }
    fprintf(stderr, "--- IBus log ---\n");
    for (;;) {
        ssize_t count = read(descriptor, buffer, sizeof(buffer));

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        (void)fwrite(buffer, 1, (size_t)count, stderr);
    }
    close(descriptor);
    fprintf(stderr, "--- end IBus log ---\n");
}

static pid_t start_sample(
    const char *sample_path,
    const char *child_path,
    const char *result_path,
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
        if (dup2(output_pipe[1], STDOUT_FILENO) == -1 ||
            dup2(output_pipe[1], STDERR_FILENO) == -1 ||
            setenv("BOXRIG_TERM", "xterm-256color", 1) == -1) {
            _exit(127);
        }
        close(output_pipe[1]);
        execl(
            sample_path,
            sample_path,
            "--renderer=opengl",
            "--",
            child_path,
            result_path,
            (char *)NULL);
        _exit(127);
    }
    close(output_pipe[1]);
    *output_descriptor = output_pipe[0];
    return process;
}

static void drain_output(int descriptor)
{
    char buffer[1024];
    int flags;

    if (descriptor == -1) {
        return;
    }
    flags = fcntl(descriptor, F_GETFL);
    if (flags != -1) {
        (void)fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
    }
    while (read(descriptor, buffer, sizeof(buffer)) > 0) {
    }
}

static void print_output(int descriptor)
{
    char buffer[1024];

    if (descriptor == -1) {
        return;
    }
    fprintf(stderr, "--- boxrig sample output ---\n");
    for (;;) {
        ssize_t count = read(descriptor, buffer, sizeof(buffer));

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        (void)fwrite(buffer, 1, (size_t)count, stderr);
    }
    fprintf(stderr, "--- end boxrig sample output ---\n");
}

static bool wait_for_window(
    Display *display,
    pid_t sample,
    int output_descriptor,
    Window *window_out)
{
    int attempt;

    XSelectInput(display, DefaultRootWindow(display), SubstructureNotifyMask);
    XSync(display, False);
    for (attempt = 0; attempt < 200; attempt++) {
        int status;

        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == MapNotify) {
                *window_out = event.xmap.window;
                return true;
            }
        }
        if (waitpid(sample, &status, WNOHANG) == sample) {
            return false;
        }
        drain_output(output_descriptor);
        pause_milliseconds(50);
    }
    return false;
}

static bool window_is_descendant(
    Display *display,
    Window window,
    Window ancestor)
{
    Window current = window;

    while (current != None && current != PointerRoot) {
        Window root = None;
        Window parent = None;
        Window *children = NULL;
        unsigned int child_count = 0;
        int status;

        if (current == ancestor) {
            return true;
        }
        status = XQueryTree(
            display,
            current,
            &root,
            &parent,
            &children,
            &child_count);
        if (children != NULL) {
            XFree(children);
        }
        if (status == 0 || parent == None || parent == current) {
            return false;
        }
        current = parent;
    }
    return false;
}

static bool wait_for_component_focus(
    Display *display,
    Window toplevel,
    pid_t sample)
{
    int attempt;

    for (attempt = 0; attempt < 200; attempt++) {
        Window focused = None;
        int revert_to = RevertToNone;
        int status;

        if (waitpid(sample, &status, WNOHANG) == sample) {
            return false;
        }
        if (XGetInputFocus(display, &focused, &revert_to) != 0 &&
            (focused == toplevel ||
             window_is_descendant(display, focused, toplevel))) {
            return true;
        }
        pause_milliseconds(25);
    }
    return false;
}

static bool fake_key(Display *display, KeySym symbol)
{
    KeyCode code = XKeysymToKeycode(display, symbol);

    if (code == 0 ||
        XTestFakeKeyEvent(display, code, True, CurrentTime) == 0 ||
        XTestFakeKeyEvent(display, code, False, CurrentTime) == 0) {
        return false;
    }
    XFlush(display);
    pause_milliseconds(INPUT_DELAY_MILLISECONDS);
    return true;
}

static bool fake_control_key(Display *display, KeySym symbol)
{
    KeyCode control = XKeysymToKeycode(display, XK_Control_L);
    KeyCode code = XKeysymToKeycode(display, symbol);

    if (control == 0 || code == 0 ||
        XTestFakeKeyEvent(display, control, True, CurrentTime) == 0 ||
        XTestFakeKeyEvent(display, code, True, CurrentTime) == 0 ||
        XTestFakeKeyEvent(display, code, False, CurrentTime) == 0 ||
        XTestFakeKeyEvent(display, control, False, CurrentTime) == 0) {
        return false;
    }
    XFlush(display);
    pause_milliseconds(INPUT_DELAY_MILLISECONDS);
    return true;
}

static bool type_ascii(Display *display, const char *text)
{
    size_t index;

    for (index = 0; text[index] != '\0'; index++) {
        if (!fake_key(display, (KeySym)(unsigned char)text[index])) {
            return false;
        }
    }
    return true;
}

static ssize_t read_result(
    const char *path,
    uint8_t *result,
    size_t capacity)
{
    int descriptor = open(path, O_RDONLY);
    ssize_t total = 0;

    if (descriptor == -1) {
        return errno == ENOENT ? 0 : -1;
    }
    while ((size_t)total < capacity) {
        ssize_t count = read(
            descriptor,
            result + total,
            capacity - (size_t)total);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            close(descriptor);
            return -1;
        }
        if (count == 0) {
            break;
        }
        total += count;
    }
    close(descriptor);
    return total;
}

static bool wait_for_result_stable(
    const char *path,
    size_t minimum,
    ssize_t *length_out)
{
    uint8_t result[RESULT_CAPACITY];
    ssize_t previous = -1;
    int stable = 0;
    int attempt;

    for (attempt = 0; attempt < 200; attempt++) {
        ssize_t length = read_result(path, result, sizeof(result));

        if (length >= 0 && (size_t)length >= minimum && length == previous) {
            stable++;
            if (stable == 5) {
                *length_out = length;
                return true;
            }
        } else {
            stable = 0;
        }
        previous = length;
        pause_milliseconds(50);
    }
    return false;
}

static bool contains_bytes(
    const uint8_t *bytes,
    size_t length,
    const uint8_t *needle,
    size_t needle_length)
{
    size_t index;

    if (needle_length == 0 || needle_length > length) {
        return false;
    }
    for (index = 0; index <= length - needle_length; index++) {
        if (memcmp(bytes + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool contains_non_ascii(const uint8_t *bytes, size_t length)
{
    size_t index;

    for (index = 0; index < length; index++) {
        if (bytes[index] >= 0x80) {
            return true;
        }
    }
    return false;
}

static XImage *read_window_image(
    Display *display,
    Window window)
{
    XWindowAttributes attributes;

    XSync(display, False);
    if (XGetWindowAttributes(display, window, &attributes) == 0 ||
        attributes.width <= 0 || attributes.height <= 0) {
        return NULL;
    }
    return XGetImage(
        display,
        window,
        0,
        0,
        (unsigned int)attributes.width,
        (unsigned int)attributes.height,
        AllPlanes,
        ZPixmap);
}

static uint64_t window_image_hash(XImage *image)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    int x;
    int y;

    for (y = 0; y < image->height; y++) {
        for (x = 0; x < image->width; x++) {
            hash ^= (uint64_t)XGetPixel(image, x, y);
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

static void print_window_difference(
    XImage *reference,
    XImage *actual)
{
    size_t different = 0;
    int minimum_x = actual->width;
    int minimum_y = actual->height;
    int maximum_x = -1;
    int maximum_y = -1;
    int width = reference->width < actual->width
                    ? reference->width
                    : actual->width;
    int height = reference->height < actual->height
                     ? reference->height
                     : actual->height;
    int x;
    int y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            if (XGetPixel(reference, x, y) == XGetPixel(actual, x, y)) {
                continue;
            }
            different++;
            if (x < minimum_x) {
                minimum_x = x;
            }
            if (y < minimum_y) {
                minimum_y = y;
            }
            if (x > maximum_x) {
                maximum_x = x;
            }
            if (y > maximum_y) {
                maximum_y = y;
            }
        }
    }
    fprintf(
        stderr,
        "difference pixels=%zu bounds=(%d,%d)-(%d,%d) sizes=%dx%d/%dx%d\n",
        different,
        minimum_x,
        minimum_y,
        maximum_x,
        maximum_y,
        reference->width,
        reference->height,
        actual->width,
        actual->height);
}

static void print_bytes(const uint8_t *bytes, size_t length)
{
    size_t index;

    fprintf(stderr, "received bytes:");
    for (index = 0; index < length; index++) {
        fprintf(stderr, " %02x", (unsigned int)bytes[index]);
    }
    fprintf(stderr, "\n");
}

static void stop_process(pid_t process, bool process_group)
{
    int status;

    if (process <= 0) {
        return;
    }
    if (waitpid(process, &status, WNOHANG) == process) {
        return;
    }
    (void)kill(process_group ? -process : process, SIGTERM);
    if (wait_process(process, 2000, &status) == 0) {
        (void)kill(process_group ? -process : process, SIGKILL);
        (void)waitpid(process, &status, 0);
    }
}

int main(int argument_count, char **arguments)
{
    static const char mozc_configuration[] =
        "engines {\n"
        "  name : \"mozc-jp\"\n"
        "  longname : \"Mozc\"\n"
        "  layout : \"default\"\n"
        "  layout_variant : \"\"\n"
        "  layout_option : \"\"\n"
        "  rank : 80\n"
        "}\n"
        "active_on_launch: True\n";
    static const uint8_t canceled_romaji[] = "torikeshi";
    static const char backspace_romaji[] = "aaaaaiiiii";
    static const uint8_t sentinel[] = "endmarker";
    char temporary_template[] = "/tmp/boxrig-p2-x11.XXXXXX";
    char *temporary_root = NULL;
    char home[PATH_MAX];
    char config[PATH_MAX];
    char cache[PATH_MAX];
    char data[PATH_MAX];
    char runtime[PATH_MAX];
    char mozc[PATH_MAX];
    char mozc_config[PATH_MAX];
    char ibus_socket[PATH_MAX];
    char ibus_address[PATH_MAX + 16];
    char ibus_log[PATH_MAX];
    char result_path[PATH_MAX];
    char display_name[40];
    char dbus_address[512];
    test_processes processes;
    Display *display = NULL;
    Window window = None;
    uint8_t result[RESULT_CAPACITY];
    char *preload_command[] = {
        "gsettings",
        "set",
        "org.freedesktop.ibus.general",
        "preload-engines",
        "['mozc-jp']",
        NULL};
    char *order_command[] = {
        "gsettings",
        "set",
        "org.freedesktop.ibus.general",
        "engines-order",
        "['mozc-jp']",
        NULL};
    char *enable_command[] = {
        "gsettings",
        "set",
        "org.freedesktop.ibus.general",
        "enable-by-default",
        "true",
        NULL};
    char *layout_command[] = {
        "gsettings",
        "set",
        "org.freedesktop.ibus.general",
        "use-system-keyboard-layout",
        "true",
        NULL};
    char *engine_command[] = {
        "ibus",
        "engine",
        "mozc-jp",
        NULL};
    char *ascii_engine_command[] = {
        "ibus",
        "engine",
        "xkb:us::eng",
        NULL};
    ssize_t first_length;
    ssize_t final_length;
    XImage *cancel_baseline_image = NULL;
    XImage *converted_image = NULL;
    XImage *reverted_image = NULL;
    XImage *canceled_image = NULL;
    XImage *backspace_deleted_image = NULL;
    uint64_t cancel_baseline_hash = 0;
    uint64_t converted_hash = 0;
    uint64_t reverted_hash = 0;
    uint64_t canceled_hash = 0;
    int status = 0;
    bool passed = false;
    boxrig_test_video video = {0};

    memset(&processes, 0, sizeof(processes));
    processes.sample_output = -1;
    if (argument_count != 3) {
        fprintf(stderr, "usage: %s SAMPLE IME_CHILD\n", arguments[0]);
        return 2;
    }
    temporary_root = mkdtemp(temporary_template);
    if (temporary_root == NULL ||
        snprintf(home, sizeof(home), "%s/home", temporary_root) < 0 ||
        snprintf(config, sizeof(config), "%s/config", temporary_root) < 0 ||
        snprintf(cache, sizeof(cache), "%s/cache", temporary_root) < 0 ||
        snprintf(data, sizeof(data), "%s/data", temporary_root) < 0 ||
        snprintf(runtime, sizeof(runtime), "%s/runtime", temporary_root) < 0 ||
        snprintf(mozc, sizeof(mozc), "%s/mozc", config) < 0 ||
        snprintf(
            mozc_config,
            sizeof(mozc_config),
            "%s/ibus_config.textproto",
            mozc) < 0 ||
        snprintf(
            ibus_socket,
            sizeof(ibus_socket),
            "%s/ibus.sock",
            runtime) < 0 ||
        snprintf(
            ibus_address,
            sizeof(ibus_address),
            "unix:path=%s",
            ibus_socket) < 0 ||
        snprintf(
            ibus_log,
            sizeof(ibus_log),
            "%s/ibus.log",
            temporary_root) < 0 ||
        snprintf(
            result_path,
            sizeof(result_path),
            "%s/input.bin",
            temporary_root) < 0 ||
        !make_directory(home) || !make_directory(config) ||
        !make_directory(cache) || !make_directory(data) ||
        !make_directory(runtime) || !make_directory(mozc) ||
        !write_text_file(mozc_config, mozc_configuration) ||
        setenv("HOME", home, 1) == -1 ||
        setenv("XDG_CONFIG_HOME", config, 1) == -1 ||
        setenv("XDG_CACHE_HOME", cache, 1) == -1 ||
        setenv("XDG_DATA_HOME", data, 1) == -1 ||
        setenv("XDG_RUNTIME_DIR", runtime, 1) == -1 ||
        setenv("TMPDIR", temporary_root, 1) == -1 ||
        setenv("LANG", "ja_JP.UTF-8", 1) == -1 ||
        setenv("LC_ALL", "ja_JP.UTF-8", 1) == -1 ||
        setenv("XMODIFIERS", "@im=ibus", 1) == -1 ||
        setenv("GTK_IM_MODULE", "ibus", 1) == -1 ||
        setenv("QT_IM_MODULE", "ibus", 1) == -1 ||
        setenv("IBUS_ADDRESS", ibus_address, 1) == -1 ||
        unsetenv("IBUS_USE_PORTAL") == -1) {
        perror("prepare isolated X11 IME environment");
        goto cleanup;
    }
    processes.xvfb = start_xvfb(display_name, sizeof(display_name));
    if (processes.xvfb == -1 || setenv("DISPLAY", display_name, 1) == -1) {
        perror("start isolated Xvfb");
        goto cleanup;
    }
    display = XOpenDisplay(display_name);
    if (display == NULL || !XTestQueryExtension(
                               display,
                               &(int){0},
                               &(int){0},
                               &(int){0},
                               &(int){0})) {
        fprintf(stderr, "XTEST is unavailable on isolated Xvfb\n");
        goto cleanup;
    }
    processes.dbus = start_dbus(dbus_address, sizeof(dbus_address));
    if (!boxrig_test_video_start(&video, display_name, "1024x768")) goto cleanup;
    if (processes.dbus == -1 ||
        setenv("DBUS_SESSION_BUS_ADDRESS", dbus_address, 1) == -1) {
        perror("start private D-Bus session");
        goto cleanup;
    }
    if (run_command(preload_command) == -1 ||
        run_command(order_command) == -1 ||
        run_command(enable_command) == -1 ||
        run_command(layout_command) == -1) {
        fprintf(stderr, "could not configure isolated IBus engine order\n");
        goto cleanup;
    }
    processes.ibus = start_ibus(ibus_log);
    if (processes.ibus == -1 ||
        !wait_for_ibus(display, processes.ibus)) {
        fprintf(stderr, "IBus/Mozc did not register its isolated XIM server\n");
        print_file(ibus_log);
        goto cleanup;
    }
    processes.sample = start_sample(
        arguments[1],
        arguments[2],
        result_path,
        &processes.sample_output);
    if (processes.sample == -1 ||
        !wait_for_window(
            display,
            processes.sample,
            processes.sample_output,
            &window)) {
        fprintf(stderr, "boxrig X11 sample did not map under IBus/Mozc\n");
        goto cleanup;
    }
    XSetInputFocus(display, window, RevertToParent, CurrentTime);
    XSync(display, False);
    if (!wait_for_component_focus(display, window, processes.sample)) {
        fprintf(stderr, "boxrig did not focus its terminal component\n");
        goto cleanup;
    }

    if (run_command(engine_command) == -1) {
        fprintf(stderr, "could not activate the Mozc IBus engine\n");
        goto cleanup;
    }
    pause_milliseconds(250);
    if (!type_ascii(display, "kanji") ||
        !fake_key(display, XK_space)) {
        fprintf(stderr, "could not enter the first Mozc conversion\n");
        goto cleanup;
    }
    XResizeWindow(display, window, 760, 420);
    XSync(display, False);
    if (!fake_key(display, XK_Right) || !fake_key(display, XK_Left) ||
        !fake_key(display, XK_Down) || !fake_key(display, XK_Up) ||
        !fake_key(display, XK_Return) ||
        !wait_for_result_stable(result_path, 3, &first_length)) {
        fprintf(stderr, "Mozc conversion/candidate/clause operation failed\n");
        goto cleanup;
    }
    final_length = read_result(result_path, result, sizeof(result));
    if (final_length != first_length ||
        !contains_non_ascii(result, (size_t)first_length) ||
        contains_bytes(
            result,
            (size_t)first_length,
            (const uint8_t *)"kanji",
            strlen("kanji"))) {
        fprintf(stderr, "physical romaji keys did not commit Mozc UTF-8 text\n");
        goto cleanup;
    }

    pause_milliseconds(250);
    cancel_baseline_image = read_window_image(display, window);
    if (cancel_baseline_image == NULL ||
        !type_ascii(display, "torikeshi") ||
        !fake_key(display, XK_space)) {
        fprintf(stderr, "could not exercise Mozc cancellation\n");
        goto cleanup;
    }
    cancel_baseline_hash = window_image_hash(cancel_baseline_image);
    pause_milliseconds(300);
    converted_image = read_window_image(display, window);
    boxrig_test_video_hold();
    if (converted_image == NULL ||
        (converted_hash = window_image_hash(converted_image)) ==
            cancel_baseline_hash ||
        !fake_key(display, XK_Escape)) {
        fprintf(stderr, "Mozc did not enter converted cancellation stage\n");
        goto cleanup;
    }
    pause_milliseconds(300);
    reverted_image = read_window_image(display, window);
    boxrig_test_video_hold();
    if (reverted_image == NULL ||
        (reverted_hash = window_image_hash(reverted_image)) ==
            cancel_baseline_hash ||
        reverted_hash == converted_hash ||
        !fake_key(display, XK_Escape)) {
        fprintf(stderr, "first Escape did not preserve reverted preedit\n");
        goto cleanup;
    }
    pause_milliseconds(300);
    if (waitpid(processes.sample, &status, WNOHANG) == processes.sample) {
        processes.sample = 0;
        fprintf(stderr, "boxrig exited while canceling XIM preedit\n");
        goto cleanup;
    }
    canceled_image = read_window_image(display, window);
    boxrig_test_video_hold();
    if (canceled_image == NULL ||
        (canceled_hash = window_image_hash(canceled_image)) !=
            cancel_baseline_hash) {
        fprintf(stderr, "second Escape did not restore preedit background\n");
        fprintf(
            stderr,
            "hashes baseline=%llu converted=%llu reverted=%llu "
            "canceled=%llu\n",
            (unsigned long long)cancel_baseline_hash,
            (unsigned long long)converted_hash,
            (unsigned long long)reverted_hash,
            (unsigned long long)canceled_hash);
        if (cancel_baseline_image != NULL && canceled_image != NULL) {
            print_window_difference(cancel_baseline_image, canceled_image);
        }
        goto cleanup;
    }
    final_length = read_result(result_path, result, sizeof(result));
    if (final_length != first_length) {
        fprintf(stderr, "Escape reached PTY while canceling preedit\n");
        if (final_length >= first_length) {
            print_bytes(
                result + first_length,
                (size_t)(final_length - first_length));
        }
        goto cleanup;
    }

    if (!type_ascii(display, backspace_romaji) ||
        !fake_key(display, XK_space) ||
        !fake_key(display, XK_BackSpace)) {
        fprintf(stderr, "could not enter Backspace clause cancellation\n");
        goto cleanup;
    }
    for (size_t index = 0;
         index < sizeof(backspace_romaji) - 1;
         index++) {
        if (!fake_key(display, XK_BackSpace)) {
            fprintf(stderr, "could not delete Mozc preedit with Backspace\n");
            goto cleanup;
        }
    }
    pause_milliseconds(300);
    backspace_deleted_image = read_window_image(display, window);
    if (backspace_deleted_image == NULL ||
        window_image_hash(backspace_deleted_image) != cancel_baseline_hash) {
        fprintf(
            stderr,
            "final Backspace did not remove the last Mozc preedit character\n");
        if (cancel_baseline_image != NULL &&
            backspace_deleted_image != NULL) {
            print_window_difference(
                cancel_baseline_image,
                backspace_deleted_image);
        }
        goto cleanup;
    }
    final_length = read_result(result_path, result, sizeof(result));
    if (final_length != first_length) {
        fprintf(stderr, "Backspace reached PTY while deleting preedit\n");
        if (final_length >= first_length) {
            print_bytes(
                result + first_length,
                (size_t)(final_length - first_length));
        }
        goto cleanup;
    }

    if (!type_ascii(display, "nihongo")) {
        fprintf(stderr, "could not start focus/resize composition\n");
        goto cleanup;
    }
    XSetInputFocus(
        display,
        DefaultRootWindow(display),
        RevertToParent,
        CurrentTime);
    XSync(display, False);
    pause_milliseconds(150);
    if (waitpid(processes.sample, &status, WNOHANG) == processes.sample) {
        processes.sample = 0;
        fprintf(stderr, "boxrig exited while moving focus away from preedit\n");
        print_output(processes.sample_output);
        goto cleanup;
    }
    XSetInputFocus(display, window, RevertToParent, CurrentTime);
    XResizeWindow(display, window, 680, 360);
    XSync(display, False);
    if (!wait_for_component_focus(display, window, processes.sample)) {
        fprintf(stderr, "boxrig did not restore terminal component focus\n");
        goto cleanup;
    }
    if (!fake_key(display, XK_space) || !fake_key(display, XK_Down) ||
        !fake_key(display, XK_Return) ||
        !wait_for_result_stable(
            result_path,
            (size_t)first_length + 3,
            &final_length)) {
        fprintf(stderr, "IME did not survive focus and resize operations\n");
        goto cleanup;
    }
    if (read_result(result_path, result, sizeof(result)) != final_length ||
        final_length <= first_length ||
        contains_bytes(
            result + first_length,
            (size_t)(final_length - first_length),
            (const uint8_t *)"nihongo",
            strlen("nihongo"))) {
        fprintf(stderr, "focus/resize path bypassed Mozc conversion\n");
        goto cleanup;
    }

    if (run_command(ascii_engine_command) == -1) {
        fprintf(stderr, "could not activate the ASCII IBus engine\n");
        goto cleanup;
    }
    pause_milliseconds(250);
    if (!type_ascii(display, (const char *)sentinel) ||
        !fake_control_key(display, XK_d)) {
        fprintf(stderr, "could not finish the isolated X11 IME fixture\n");
        goto cleanup;
    }
    if (wait_process(
            processes.sample,
            PROCESS_TIMEOUT_MILLISECONDS,
            &status) != 1 ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "boxrig did not follow child exit from alternate screen\n");
        goto cleanup;
    }
    processes.sample = 0;
    final_length = read_result(result_path, result, sizeof(result));
    if (final_length < 0 ||
        contains_bytes(
            result,
            (size_t)final_length,
            canceled_romaji,
            sizeof(canceled_romaji) - 1) ||
        !contains_bytes(
            result,
            (size_t)final_length,
            sentinel,
            sizeof(sentinel) - 1)) {
        fprintf(stderr, "cancel or final ASCII engine transition was incorrect\n");
        goto cleanup;
    }
    printf("PASS: isolated Xvfb IBus/Mozc multibyte IME E2E\n");
    passed = true;

cleanup:
    if (!boxrig_test_video_stop(&video)) passed = false;
    if (backspace_deleted_image != NULL) {
        XDestroyImage(backspace_deleted_image);
    }
    if (canceled_image != NULL) {
        XDestroyImage(canceled_image);
    }
    if (reverted_image != NULL) {
        XDestroyImage(reverted_image);
    }
    if (converted_image != NULL) {
        XDestroyImage(converted_image);
    }
    if (cancel_baseline_image != NULL) {
        XDestroyImage(cancel_baseline_image);
    }
    if (!passed && processes.sample_output != -1) {
        print_output(processes.sample_output);
    }
    if (processes.sample_output != -1) {
        close(processes.sample_output);
    }
    stop_process(processes.sample, false);
    stop_process(processes.ibus, true);
    stop_process(processes.dbus, false);
    if (display != NULL) {
        XCloseDisplay(display);
    }
    stop_process(processes.xvfb, false);
    remove_tree(temporary_root);
    return passed ? 0 : 1;
}
