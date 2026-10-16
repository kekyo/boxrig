/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "boxrig/x11.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool feed(boxrig_terminal *terminal, const uint8_t *bytes, size_t length)
{
    size_t consumed = 0;

    return boxrig_terminal_feed(terminal, bytes, length, &consumed) ==
               BOXRIG_STATUS_OK &&
           consumed == length;
}

static bool encode_base64(
    const uint8_t *input,
    size_t input_length,
    char *output,
    size_t output_capacity)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t encoded_length;
    size_t input_offset = 0;
    size_t output_offset = 0;

    if (input_length > (SIZE_MAX - 2) / 4 * 3) {
        return false;
    }
    encoded_length = ((input_length + 2) / 3) * 4;
    if (encoded_length >= output_capacity) {
        return false;
    }
    while (input_offset + 3 <= input_length) {
        uint32_t value = ((uint32_t)input[input_offset] << 16) |
                         ((uint32_t)input[input_offset + 1] << 8) |
                         (uint32_t)input[input_offset + 2];

        output[output_offset++] = alphabet[(value >> 18) & 0x3f];
        output[output_offset++] = alphabet[(value >> 12) & 0x3f];
        output[output_offset++] = alphabet[(value >> 6) & 0x3f];
        output[output_offset++] = alphabet[value & 0x3f];
        input_offset += 3;
    }
    if (input_offset < input_length) {
        uint32_t value = (uint32_t)input[input_offset] << 16;

        if (input_offset + 1 < input_length) {
            value |= (uint32_t)input[input_offset + 1] << 8;
        }
        output[output_offset++] = alphabet[(value >> 18) & 0x3f];
        output[output_offset++] = alphabet[(value >> 12) & 0x3f];
        output[output_offset++] = input_offset + 1 < input_length
                                      ? alphabet[(value >> 6) & 0x3f]
                                      : '=';
        output[output_offset++] = '=';
    }
    output[output_offset] = '\0';
    return output_offset == encoded_length;
}

static bool feed_external_graphics(
    boxrig_terminal *terminal,
    uint32_t image_id,
    char medium,
    uint64_t offset,
    uint64_t size,
    const uint8_t *name,
    size_t name_length)
{
    char encoded[PATH_MAX * 2];
    char command[PATH_MAX * 2 + 160];
    int command_length;

    if (!encode_base64(name, name_length, encoded, sizeof(encoded))) {
        return false;
    }
    command_length = snprintf(
        command,
        sizeof(command),
        "\x1b_Ga=t,i=%lu,t=%c,f=32,s=1,v=1,O=%llu,S=%llu;%s\x1b\\",
        (unsigned long)image_id,
        medium,
        (unsigned long long)offset,
        (unsigned long long)size,
        encoded);
    return command_length > 0 && (size_t)command_length < sizeof(command) &&
           feed(terminal, (const uint8_t *)command, (size_t)command_length);
}

static bool output_equals(boxrig_terminal *terminal, const char *expected)
{
    const uint8_t *output = NULL;
    size_t expected_length = strlen(expected);
    size_t output_length = boxrig_terminal_output_peek(terminal, &output);

    return output_length == expected_length &&
           memcmp(output, expected, output_length) == 0 &&
           boxrig_terminal_output_consume(terminal, output_length) ==
               BOXRIG_STATUS_OK;
}

static bool image_equals(
    boxrig_terminal *terminal,
    uint32_t image_id,
    const uint8_t *rgba,
    size_t rgba_length)
{
    size_t index;

    for (index = 0; index < boxrig_terminal_image_count(terminal); index++) {
        boxrig_image_handle handle = 0;
        boxrig_image_view image;

        if (boxrig_terminal_image_handle_at(terminal, index, &handle) !=
                BOXRIG_STATUS_OK ||
            boxrig_terminal_get_image(terminal, handle, &image) !=
                BOXRIG_STATUS_OK) {
            return false;
        }
        if (image.image_id == image_id) {
            return image.rgba_length == rgba_length &&
                   memcmp(image.rgba, rgba, rgba_length) == 0;
        }
    }
    return false;
}

static bool write_all(int descriptor, const uint8_t *bytes, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        ssize_t written = write(descriptor, bytes + offset, length - offset);

        if (written == -1 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        offset += (size_t)written;
    }
    return true;
}

static bool create_file(
    char *path_template,
    const uint8_t *bytes,
    size_t length)
{
    int descriptor = mkstemp(path_template);
    bool success;

    if (descriptor == -1) {
        return false;
    }
    success = write_all(descriptor, bytes, length);
    if (close(descriptor) == -1) {
        success = false;
    }
    if (!success) {
        (void)unlink(path_template);
    }
    return success;
}

static bool create_graphics_adapter(
    Display *display,
    bool allow_external,
    size_t decode_capacity,
    boxrig_terminal **terminal_out,
    boxrig_ui_model **model_out,
    boxrig_ui_host_item_consumer **consumer_out,
    Window *window_out,
    boxrig_x11_host_adapter **adapter_out)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_x11_host_policy policy = boxrig_x11_host_policy_default();
    int screen = DefaultScreen(display);

    *terminal_out = NULL;
    *model_out = NULL;
    *consumer_out = NULL;
    *adapter_out = NULL;
    *window_out = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        16,
        16,
        0,
        BlackPixel(display, screen),
        BlackPixel(display, screen));
    if (*window_out == None) {
        return false;
    }
    config.rows = 1;
    config.columns = 1;
    config.graphics_decode_capacity = decode_capacity;
    policy.allow_graphics_external = allow_external;
    if (boxrig_terminal_create(&config, terminal_out) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(*terminal_out, model_out) != BOXRIG_UI_OK ||
        boxrig_ui_host_item_consumer_create(*model_out, consumer_out) !=
            BOXRIG_UI_OK ||
        boxrig_x11_host_adapter_create(
            display,
            &(boxrig_x11_host_targets){
                *window_out,
                *window_out,
                *window_out,
            },
            *consumer_out,
            config.clipboard_capacity,
            &policy,
            adapter_out) == -1) {
        boxrig_x11_host_adapter_destroy(*adapter_out);
        *adapter_out = NULL;
        boxrig_ui_host_item_consumer_destroy(*consumer_out);
        *consumer_out = NULL;
        boxrig_ui_model_destroy(*model_out);
        *model_out = NULL;
        boxrig_terminal_destroy(*terminal_out);
        *terminal_out = NULL;
        XDestroyWindow(display, *window_out);
        *window_out = None;
        return false;
    }
    return true;
}

static void destroy_graphics_adapter(
    Display *display,
    boxrig_terminal *terminal,
    boxrig_ui_model *model,
    boxrig_ui_host_item_consumer *consumer,
    Window window,
    boxrig_x11_host_adapter *adapter)
{
    boxrig_x11_host_adapter_destroy(adapter);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, window);
}

static bool property_equals(
    Display *display,
    Window window,
    Atom property,
    Atom expected_type,
    const uint8_t *expected,
    size_t expected_length)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *value = NULL;
    int status = XGetWindowProperty(
        display,
        window,
        property,
        0,
        1024,
        False,
        AnyPropertyType,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &value);
    bool matches = status == Success && actual_type == expected_type &&
                   actual_format == 8 && bytes_after == 0 &&
                   item_count == expected_length &&
                   (expected_length == 0 ||
                    memcmp(value, expected, expected_length) == 0);

    if (value != NULL) {
        XFree(value);
    }
    return matches;
}

static bool drive_selection(
    Display *display,
    boxrig_x11_host_adapter *adapter,
    boxrig_terminal *terminal)
{
    size_t cycle;

    for (cycle = 0; cycle < 8; cycle++) {
        const uint8_t *output = NULL;

        XSync(display, False);
        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            CHECK(boxrig_x11_host_adapter_process_event(adapter, &event) >= 0);
        }
        if (boxrig_terminal_output_peek(terminal, &output) != 0) {
            return true;
        }
    }
    return false;
}

static bool test_host_adapter(Display *display)
{
    static const uint8_t title_stack[] =
        "\x1b[22;0;0t\x1b]2;temporary\x07\x1b[23;0;0t";
    static const uint8_t metadata_input[] =
        "\x1b[8;25;90t"
        "\x1b]2;t\xc3\xa9\x1b\\"
        "\x1b]7;file://host/untrusted\x07"
        "\x1b]8;;https://example.test/\x07"
        "A"
        "\x1b]8;;\x07";
    static const uint8_t clipboard_write[] = "\x1b]52;c;SGVsbG8=\x07";
    static const uint8_t clipboard_query[] = "\x1b]52;c;?\x07";
    static const uint8_t denied_reply[] = "\x1b]52;c;\x07";
    static const uint8_t allowed_reply[] = "\x1b]52;c;SGVsbG8=\x07";
    static const uint8_t expected_title[] = {'t', 0xc3, 0xa9};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_host_item_consumer *consumer = NULL;
    boxrig_ui_host_item_consumer *duplicate_consumer = NULL;
    boxrig_x11_host_adapter *adapter = NULL;
    boxrig_x11_host_policy policy = boxrig_x11_host_policy_default();
    int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        64,
        32,
        0,
        BlackPixel(display, screen),
        BlackPixel(display, screen));
    Window clipboard_window = XCreateSimpleWindow(
        display,
        window,
        0,
        0,
        16,
        16,
        0,
        BlackPixel(display, screen),
        BlackPixel(display, screen));
    boxrig_x11_host_targets targets = {
        window,
        clipboard_window,
        clipboard_window,
    };
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    Atom net_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
    boxrig_cell_view cell;
    const uint8_t *output = NULL;
    size_t output_length;

    CHECK(window != None && clipboard_window != None && utf8 != None &&
          net_wm_name != None && clipboard != None);
    CHECK(policy.allow_title_change);
    CHECK(policy.allow_clipboard_write);
    CHECK(!policy.allow_clipboard_read);
    config.rows = 1;
    config.columns = 2;
    config.clipboard_capacity = 32;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_host_item_consumer_create(model, &consumer) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_host_item_consumer_create(
              model,
              &duplicate_consumer) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_x11_host_adapter_create(
              display,
              &targets,
              consumer,
              config.clipboard_capacity,
              &policy,
              &adapter) == 0);
    (void)XChangeProperty(
        display,
        window,
        net_wm_name,
        utf8,
        8,
        PropModeReplace,
        (const unsigned char *)"initial",
        7);
    CHECK(XStoreName(display, window, "initial") != 0);
    CHECK(feed(terminal, title_stack, sizeof(title_stack) - 1));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    XSync(display, False);
    CHECK(property_equals(
        display,
        window,
        net_wm_name,
        utf8,
        (const uint8_t *)"initial",
        7));
    CHECK(feed(terminal, metadata_input, sizeof(metadata_input) - 1));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    size_t kept_rows, kept_columns;
    boxrig_terminal_size(terminal, &kept_rows, &kept_columns);
    CHECK(kept_rows == config.rows && kept_columns == config.columns);
    XSync(display, False);
    CHECK(property_equals(
        display,
        window,
        net_wm_name,
        utf8,
        expected_title,
        sizeof(expected_title)));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.hyperlink_uri_length == 21);
    CHECK(memcmp(
              cell.hyperlink_uri,
              "https://example.test/",
              cell.hyperlink_uri_length) == 0);

    CHECK(feed(terminal, clipboard_write, sizeof(clipboard_write) - 1));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    XSync(display, False);
    CHECK(XGetSelectionOwner(display, clipboard) == clipboard_window);
    CHECK(feed(terminal, clipboard_query, sizeof(clipboard_query) - 1));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(denied_reply) - 1);
    CHECK(memcmp(output, denied_reply, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    boxrig_x11_host_adapter_destroy(adapter);
    adapter = NULL;
    policy.allow_clipboard_read = true;
    CHECK(boxrig_x11_host_adapter_create(
              display,
              &targets,
              consumer,
              config.clipboard_capacity,
              &policy,
              &adapter) == 0);
    CHECK(feed(terminal, clipboard_write, sizeof(clipboard_write) - 1));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(feed(terminal, clipboard_query, sizeof(clipboard_query) - 1));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(drive_selection(display, adapter, terminal));
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(allowed_reply) - 1);
    CHECK(memcmp(output, allowed_reply, output_length) == 0);

    boxrig_x11_host_adapter_destroy(adapter);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, window);
    XSync(display, False);
    return true;
}

static bool test_graphics_external_policy(void)
{
    const char *current = getenv("BOXRIG_ALLOW_GRAPHICS_EXTERNAL");
    char *saved = NULL;
    boxrig_x11_host_policy policy;
    bool valid;

    if (current != NULL) {
        size_t length = strlen(current);

        saved = malloc(length + 1);
        CHECK(saved != NULL);
        memcpy(saved, current, length + 1);
    }
    valid = unsetenv("BOXRIG_ALLOW_GRAPHICS_EXTERNAL") == 0;
    policy = boxrig_x11_host_policy_default();
    valid = valid && !policy.allow_graphics_external;
    valid = valid && setenv("BOXRIG_ALLOW_GRAPHICS_EXTERNAL", "true", 1) == 0;
    policy = boxrig_x11_host_policy_default();
    valid = valid && !policy.allow_graphics_external;
    valid = valid && setenv("BOXRIG_ALLOW_GRAPHICS_EXTERNAL", "1", 1) == 0;
    policy = boxrig_x11_host_policy_default();
    valid = valid && !policy.allow_graphics_external;
    if (saved == NULL) {
        valid = valid && unsetenv("BOXRIG_ALLOW_GRAPHICS_EXTERNAL") == 0;
    } else {
        valid = valid &&
                setenv("BOXRIG_ALLOW_GRAPHICS_EXTERNAL", saved, 1) == 0;
    }
    free(saved);
    CHECK(valid);
    return true;
}

static bool test_graphics_regular_file_transport(Display *display)
{
    static const uint8_t file_bytes[] = {9, 8, 1, 2, 3, 4};
    static const uint8_t rgba[] = {1, 2, 3, 4};
    static const uint8_t invalid_utf8_name[] = {
        '/', 't', 'm', 'p', '/', 0xff
    };
    char path[] = "/tmp/boxrig-graphics-file-XXXXXX";
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_host_item_consumer *consumer = NULL;
    boxrig_x11_host_adapter *adapter = NULL;
    Window window = None;

    CHECK(create_file(path, file_bytes, sizeof(file_bytes)));
    CHECK(create_graphics_adapter(
        display, false, 16, &terminal, &model, &consumer, &window, &adapter));
    CHECK(feed_external_graphics(
        terminal, 501, 'f', 2, 4, (const uint8_t *)path, strlen(path)));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=501;EACCES:external graphics data denied\x1b\\"));
    CHECK(access(path, F_OK) == 0);
    destroy_graphics_adapter(
        display, terminal, model, consumer, window, adapter);

    CHECK(create_graphics_adapter(
        display, true, 16, &terminal, &model, &consumer, &window, &adapter));
    CHECK(feed_external_graphics(
        terminal, 502, 'f', 2, 4, (const uint8_t *)path, strlen(path)));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(terminal, "\x1b_Gi=502;OK\x1b\\"));
    CHECK(image_equals(terminal, 502, rgba, sizeof(rgba)));
    CHECK(access(path, F_OK) == 0);

    CHECK(feed_external_graphics(
        terminal, 503, 'f', 2, 0, (const uint8_t *)path, strlen(path)));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(terminal, "\x1b_Gi=503;OK\x1b\\"));
    CHECK(image_equals(terminal, 503, rgba, sizeof(rgba)));

    CHECK(feed_external_graphics(
        terminal, 504, 'f', 2, 5, (const uint8_t *)path, strlen(path)));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=504;EIO:external graphics data unavailable\x1b\\"));
    CHECK(feed_external_graphics(
        terminal, 505, 'f', 0, 0, (const uint8_t *)"/tmp", 4));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=505;EIO:external graphics data unavailable\x1b\\"));
    CHECK(feed_external_graphics(
        terminal,
        506,
        'f',
        0,
        4,
        invalid_utf8_name,
        sizeof(invalid_utf8_name)));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=506;EIO:external graphics data unavailable\x1b\\"));
    destroy_graphics_adapter(
        display, terminal, model, consumer, window, adapter);

    CHECK(create_graphics_adapter(
        display,
        true,
        sizeof(rgba),
        &terminal,
        &model,
        &consumer,
        &window,
        &adapter));
    CHECK(feed_external_graphics(
        terminal, 507, 'f', 0, 0, (const uint8_t *)path, strlen(path)));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=507;EIO:external graphics data unavailable\x1b\\"));
    CHECK(boxrig_terminal_image_count(terminal) == 0);
    destroy_graphics_adapter(
        display, terminal, model, consumer, window, adapter);
    CHECK(unlink(path) == 0);
    return true;
}

static bool test_graphics_temporary_file_transport(Display *display)
{
    static const uint8_t rgba[] = {11, 22, 33, 44};
    char safe_path[] =
        "/tmp/boxrig-tty-graphics-protocol-safe-XXXXXX";
    char short_path[] =
        "/tmp/boxrig-tty-graphics-protocol-short-XXXXXX";
    char unsafe_path[] = "/tmp/boxrig-graphics-unsafe-XXXXXX";
    char target_path[] = "/tmp/boxrig-graphics-target-XXXXXX";
    char link_path[PATH_MAX];
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_host_item_consumer *consumer = NULL;
    boxrig_x11_host_adapter *adapter = NULL;
    Window window = None;
    int link_length;

    CHECK(create_file(safe_path, rgba, sizeof(rgba)));
    CHECK(create_file(short_path, rgba, sizeof(rgba)));
    CHECK(create_file(unsafe_path, rgba, sizeof(rgba)));
    CHECK(create_file(target_path, rgba, sizeof(rgba)));
    link_length = snprintf(
        link_path,
        sizeof(link_path),
        "/tmp/boxrig-tty-graphics-protocol-link-%ld",
        (long)getpid());
    CHECK(link_length > 0 && (size_t)link_length < sizeof(link_path));
    (void)unlink(link_path);
    CHECK(symlink(target_path, link_path) == 0);
    CHECK(create_graphics_adapter(
        display, true, 16, &terminal, &model, &consumer, &window, &adapter));

    CHECK(feed_external_graphics(
        terminal,
        511,
        't',
        0,
        sizeof(rgba),
        (const uint8_t *)safe_path,
        strlen(safe_path)));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(terminal, "\x1b_Gi=511;OK\x1b\\"));
    CHECK(image_equals(terminal, 511, rgba, sizeof(rgba)));
    CHECK(access(safe_path, F_OK) == -1 && errno == ENOENT);

    CHECK(feed_external_graphics(
        terminal,
        512,
        't',
        0,
        sizeof(rgba) + 1,
        (const uint8_t *)short_path,
        strlen(short_path)));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=512;EIO:external graphics data unavailable\x1b\\"));
    CHECK(access(short_path, F_OK) == -1 && errno == ENOENT);

    CHECK(feed_external_graphics(
        terminal,
        513,
        't',
        0,
        sizeof(rgba),
        (const uint8_t *)unsafe_path,
        strlen(unsafe_path)));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=513;EACCES:external graphics data denied\x1b\\"));
    CHECK(access(unsafe_path, F_OK) == 0);

    CHECK(feed_external_graphics(
        terminal,
        514,
        't',
        0,
        sizeof(rgba),
        (const uint8_t *)link_path,
        strlen(link_path)));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=514;EACCES:external graphics data denied\x1b\\"));
    CHECK(access(target_path, F_OK) == 0);
    CHECK(access(link_path, F_OK) == 0);

    destroy_graphics_adapter(
        display, terminal, model, consumer, window, adapter);
    CHECK(unlink(link_path) == 0);
    CHECK(unlink(target_path) == 0);
    CHECK(unlink(unsafe_path) == 0);
    return true;
}

static bool test_graphics_cancel_before_host_read(Display *display)
{
    const uint8_t rgba[] = {11, 22, 33, 255};
    char path[] = "/tmp/boxrig-tty-graphics-protocol-cancel-XXXXXX";
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_host_item_consumer *consumer = NULL;
    boxrig_x11_host_adapter *adapter = NULL;
    Window window = None;
    CHECK(create_file(path, rgba, sizeof(rgba)));
    CHECK(create_graphics_adapter(display, true, 16,
        &terminal, &model, &consumer, &window, &adapter));
    CHECK(feed_external_graphics(terminal, 515, 't', 0, sizeof(rgba),
        (const uint8_t *)path, strlen(path)));
    size_t consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033c", 2, &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 2);
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(access(path, F_OK) == 0);
    CHECK(boxrig_terminal_image_count(terminal) == 0);
    CHECK(output_equals(terminal, "\033_Gi=515;EIO:external graphics data unavailable\033\\"));
    destroy_graphics_adapter(display, terminal, model, consumer, window, adapter);
    CHECK(unlink(path) == 0);
    return true;
}

static bool create_shared_memory(
    const char *name,
    const uint8_t *bytes,
    size_t length)
{
    int descriptor;
    bool success;

    (void)shm_unlink(name);
    descriptor = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (descriptor == -1) {
        return false;
    }
    success = ftruncate(descriptor, (off_t)length) == 0 &&
              write_all(descriptor, bytes, length);
    if (close(descriptor) == -1) {
        success = false;
    }
    if (!success) {
        (void)shm_unlink(name);
    }
    return success;
}

static bool test_graphics_shared_memory_transport(Display *display)
{
    static const uint8_t shared_bytes[] = {7, 6, 51, 52, 53, 54};
    static const uint8_t rgba[] = {51, 52, 53, 54};
    char allowed_name[128];
    char denied_name[128];
    char short_name[128];
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_host_item_consumer *consumer = NULL;
    boxrig_x11_host_adapter *adapter = NULL;
    Window window = None;
    int descriptor;

    CHECK(snprintf(
              allowed_name,
              sizeof(allowed_name),
              "/boxrig-graphics-allowed-%ld",
              (long)getpid()) > 0);
    CHECK(snprintf(
              denied_name,
              sizeof(denied_name),
              "/boxrig-graphics-denied-%ld",
              (long)getpid()) > 0);
    CHECK(snprintf(
              short_name,
              sizeof(short_name),
              "/boxrig-graphics-short-%ld",
              (long)getpid()) > 0);
    CHECK(create_shared_memory(
        allowed_name, shared_bytes, sizeof(shared_bytes)));
    CHECK(create_graphics_adapter(
        display, true, 16, &terminal, &model, &consumer, &window, &adapter));
    CHECK(feed_external_graphics(
        terminal,
        521,
        's',
        2,
        0,
        (const uint8_t *)allowed_name,
        strlen(allowed_name)));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(terminal, "\x1b_Gi=521;OK\x1b\\"));
    CHECK(image_equals(terminal, 521, rgba, sizeof(rgba)));
    descriptor = shm_open(allowed_name, O_RDONLY, 0);
    CHECK(descriptor == -1 && errno == ENOENT);
    destroy_graphics_adapter(
        display, terminal, model, consumer, window, adapter);

    CHECK(create_shared_memory(
        denied_name, rgba, sizeof(rgba)));
    CHECK(create_graphics_adapter(
        display,
        false,
        16,
        &terminal,
        &model,
        &consumer,
        &window,
        &adapter));
    CHECK(feed_external_graphics(
        terminal,
        522,
        's',
        0,
        sizeof(rgba),
        (const uint8_t *)denied_name,
        strlen(denied_name)));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=522;EACCES:external graphics data denied\x1b\\"));
    descriptor = shm_open(denied_name, O_RDONLY, 0);
    CHECK(descriptor != -1);
    CHECK(close(descriptor) == 0);
    CHECK(shm_unlink(denied_name) == 0);
    destroy_graphics_adapter(
        display, terminal, model, consumer, window, adapter);

    CHECK(create_shared_memory(short_name, rgba, sizeof(rgba)));
    CHECK(create_graphics_adapter(
        display, true, 16, &terminal, &model, &consumer, &window, &adapter));
    CHECK(feed_external_graphics(
        terminal,
        523,
        's',
        0,
        sizeof(rgba) + 1,
        (const uint8_t *)short_name,
        strlen(short_name)));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=523;EIO:external graphics data unavailable\x1b\\"));
    descriptor = shm_open(short_name, O_RDONLY, 0);
    CHECK(descriptor == -1 && errno == ENOENT);
    destroy_graphics_adapter(
        display, terminal, model, consumer, window, adapter);
    return true;
}

int main(void)
{
    Display *display = XOpenDisplay(NULL);
    bool passed;

    if (display == NULL) {
        fprintf(stderr, "could not open X display\n");
        return 1;
    }
    passed = test_host_adapter(display) && test_graphics_external_policy() &&
             test_graphics_cancel_before_host_read(display) &&
             test_graphics_regular_file_transport(display) &&
             test_graphics_temporary_file_transport(display) &&
             test_graphics_shared_memory_transport(display);
    XCloseDisplay(display);
    if (!passed) {
        return 1;
    }
    printf("PASS X11 host adapter applies policy and selections\n");
    return 0;
}
