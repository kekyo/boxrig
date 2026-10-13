/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/win32.h"
#include "boxrig/terminal.h"

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int spawn_test_pty(const wchar_t *command, size_t rows, size_t columns,
    boxrig_win32_pty **pty)
{
    boxrig_win32_pty_config config = boxrig_win32_pty_config_default();
    const wchar_t *policy = _wgetenv(L"BOXRIG_CONPTY");
    if (policy != NULL && wcscmp(policy, L"packaged-required") == 0) {
        config.policy = BOXRIG_WIN32_PTY_PACKAGED_REQUIRED;
        config.packaged_directory = _wgetenv(L"BOXRIG_CONPTY_DIRECTORY");
    }
    boxrig_win32_pty_diagnostics diagnostic;
    int result = boxrig_win32_pty_spawn_with_config(command, rows, columns, &config, &diagnostic, pty);
    if (result == 0) printf("test-pty-backend=%s\n",
        diagnostic.selected_backend == BOXRIG_WIN32_PTY_BACKEND_PACKAGED ? "packaged" : "system");
    else fprintf(stderr, "PTY creation failed: stage=%d error=%lu\n",
        (int)diagnostic.stage, (unsigned long)diagnostic.win32_error);
    return result;
}

typedef struct read_request {
    HANDLE pipe;
    char *buffer;
    size_t capacity;
    size_t *length;
    const char *needle;
    bool found;
} read_request;

typedef struct raw_read_request {
    HANDLE pipe;
    uint8_t *buffer;
    size_t capacity;
    size_t length;
    bool reached_end;
} raw_read_request;

static const uint8_t transport_expected[] =
    "boxrig CLI test emitter.\r\n"
    "TEST transport-bytes\r\n"
    "EXPECT: every fixture byte reaches the PTY unchanged\r\n"
    "BEGIN ----------\r\n"
    "C0:A\aB\bC\x18" "D\r\n"
    "\033[31mCSI\033[0m\r\n"
    "\033]2;BOXRIG\a\r\n"
    "\033P$qm\033\\\r\n"
    "\033_Gi=1;APC\033\\\r\n"
    "UTF8:\xe7\x95\x8c" "\xf0\x9f\x91\xa9\r\n"
    "INVALID:\xf0\x28\x8c\x28\r\n"
    "END ------------\r\n";

static DWORD WINAPI read_until_thread(LPVOID parameter)
{
    read_request *request = parameter;

    while (strstr(request->buffer, request->needle) == NULL) {
        DWORD received = 0;
        size_t remaining;

        if (*request->length + 1 >= request->capacity) {
            return 1;
        }
        remaining = request->capacity - *request->length - 1;
        if (!ReadFile(
                request->pipe,
                request->buffer + *request->length,
                (DWORD)remaining,
                &received,
                NULL) ||
            received == 0) {
            return 1;
        }
        *request->length += received;
        request->buffer[*request->length] = '\0';
    }
    request->found = true;
    return 0;
}

static DWORD WINAPI read_to_end_thread(LPVOID parameter)
{
    raw_read_request *request = parameter;

    for (;;) {
        DWORD received = 0;
        size_t remaining;

        if (request->length == request->capacity) {
            return 1;
        }
        remaining = request->capacity - request->length;
        if (!ReadFile(
                request->pipe,
                request->buffer + request->length,
                (DWORD)remaining,
                &received,
                NULL)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                request->reached_end = true;
                return 0;
            }
            return 1;
        }
        if (received == 0) {
            request->reached_end = true;
            return 0;
        }
        request->length += received;
    }
}

static size_t find_bytes(
    const uint8_t *haystack,
    size_t haystack_length,
    const uint8_t *needle,
    size_t needle_length)
{
    size_t index;

    if (needle_length == 0) {
        return 0;
    }
    if (needle_length > haystack_length) {
        return SIZE_MAX;
    }
    for (index = 0; index <= haystack_length - needle_length; index++) {
        if (memcmp(haystack + index, needle, needle_length) == 0) {
            return index;
        }
    }
    return SIZE_MAX;
}

static void print_hex_bytes(const uint8_t *bytes, size_t length)
{
    size_t index;

    fprintf(stderr, "received hex:");
    for (index = 0; index < length; index++) {
        fprintf(stderr, "%s%02X", index % 32 == 0 ? "\n" : " ", bytes[index]);
    }
    fprintf(stderr, "\n");
}

static bool terminal_contains(
    boxrig_terminal *terminal,
    const uint8_t *expected,
    size_t expected_length)
{
    uint8_t row_text[8192];
    size_t rows = 0;
    size_t columns = 0;
    size_t row;

    boxrig_terminal_size(terminal, &rows, &columns);
    for (row = 0; row < rows; row++) {
        size_t row_length = 0;
        size_t column;

        for (column = 0; column < columns; column++) {
            boxrig_cell_view cell;

            if (boxrig_terminal_get_cell(
                    terminal,
                    row,
                    column,
                    &cell) != BOXRIG_STATUS_OK) {
                return false;
            }
            if (cell.continuation) {
                continue;
            }
            if (cell.text_length > sizeof(row_text) - row_length) {
                return false;
            }
            memcpy(row_text + row_length, cell.text, cell.text_length);
            row_length += cell.text_length;
        }
        if (find_bytes(
                row_text,
                row_length,
                expected,
                expected_length) != SIZE_MAX) {
            return true;
        }
    }
    return false;
}

static bool terminal_contains_red_csi(boxrig_terminal *terminal)
{
    size_t rows = 0;
    size_t columns = 0;
    size_t row;

    boxrig_terminal_size(terminal, &rows, &columns);
    for (row = 0; row < rows; row++) {
        size_t column;

        for (column = 0; column + 2 < columns; column++) {
            static const char expected[] = "CSI";
            size_t offset;
            bool match = true;

            for (offset = 0; offset < sizeof(expected) - 1; offset++) {
                boxrig_cell_view cell;

                if (boxrig_terminal_get_cell(
                        terminal,
                        row,
                        column + offset,
                        &cell) != BOXRIG_STATUS_OK ||
                    cell.text_length != 1 ||
                    cell.text[0] != expected[offset] ||
                    cell.foreground.kind != BOXRIG_COLOR_INDEXED ||
                    cell.foreground.index != 1) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return true;
            }
        }
    }
    return false;
}

static bool normalized_transport_has_expected_meaning(
    const uint8_t *bytes,
    size_t length,
    bool require_emoji)
{
    static const uint8_t banner[] = "boxrig CLI test emitter.";
    static const uint8_t case_name[] = "TEST transport-bytes";
    static const uint8_t c0_result[] = "C0:ACD";
    static const uint8_t utf8_label[] = "UTF8:";
    static const uint8_t cjk[] = "\xe7\x95\x8c";
    static const uint8_t emoji[] = "\xf0\x9f\x91\xa9";
    static const uint8_t invalid_label[] = "INVALID:";
    static const uint8_t replacement[] = "\xef\xbf\xbd";
    static const uint8_t ending[] = "END ------------";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    size_t consumed = 0;
    bool has_banner;
    bool has_case_name;
    bool has_c0_result;
    bool has_red_csi;
    bool has_utf8_label;
    bool has_cjk;
    bool has_emoji;
    bool has_invalid_label;
    bool has_replacement;
    bool has_ending;
    bool passed = false;

    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_terminal_feed(terminal, bytes, length, &consumed) !=
            BOXRIG_STATUS_OK ||
        consumed != length) {
        goto cleanup;
    }
    has_banner = terminal_contains(
        terminal,
        banner,
        sizeof(banner) - 1);
    has_case_name = terminal_contains(
        terminal,
        case_name,
        sizeof(case_name) - 1);
    has_c0_result = terminal_contains(
        terminal,
        c0_result,
        sizeof(c0_result) - 1);
    has_red_csi = terminal_contains_red_csi(terminal);
    has_utf8_label = terminal_contains(
        terminal,
        utf8_label,
        sizeof(utf8_label) - 1);
    has_cjk = terminal_contains(
        terminal,
        cjk,
        sizeof(cjk) - 1);
    has_emoji = terminal_contains(
        terminal,
        emoji,
        sizeof(emoji) - 1);
    has_invalid_label = terminal_contains(
        terminal,
        invalid_label,
        sizeof(invalid_label) - 1);
    has_replacement = terminal_contains(
        terminal,
        replacement,
        sizeof(replacement) - 1);
    has_ending = terminal_contains(
        terminal,
        ending,
        sizeof(ending) - 1);
    if (!has_banner || !has_case_name || !has_c0_result || !has_red_csi ||
        !has_utf8_label || !has_cjk || (require_emoji && !has_emoji) ||
        !has_invalid_label ||
        !has_replacement || !has_ending) {
        fprintf(
            stderr,
            "normalized meaning: banner=%d case=%d c0=%d red=%d "
            "utf8-label=%d cjk=%d emoji=%d invalid=%d replacement=%d "
            "ending=%d\n",
            has_banner,
            has_case_name,
            has_c0_result,
            has_red_csi,
            has_utf8_label,
            has_cjk,
            has_emoji,
            has_invalid_label,
            has_replacement,
            has_ending);
        goto cleanup;
    }
    passed = true;

cleanup:
    boxrig_terminal_destroy(terminal);
    return passed;
}

static bool image_has_rgba(
    const boxrig_image_view *image,
    size_t x,
    size_t y,
    uint8_t red,
    uint8_t green,
    uint8_t blue,
    uint8_t alpha)
{
    size_t offset;

    if (x >= image->width || y >= image->height) {
        return false;
    }
    offset = y * image->stride + x * 4;
    return offset <= image->rgba_length &&
           image->rgba_length - offset >= 4 &&
           image->rgba[offset] == red && image->rgba[offset + 1] == green &&
           image->rgba[offset + 2] == blue &&
           image->rgba[offset + 3] == alpha;
}

static bool graphics_output_has_expected_meaning(
    const uint8_t *bytes,
    size_t length,
    bool system_backend)
{
    static const uint8_t primary_device_attributes[] =
        "\x1b[?62;6;22c";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;
    size_t index;
    bool found_rgb = false;
    bool found_chunked_alpha = false;
    bool found_zlib = false;
    bool found_png = false;
    bool found_crop = false;
    bool found_scroll = false;
    bool found_primary = false;
    bool found_placeholder = false;
    bool found_history = false;
    bool passed = false;
    size_t placeholder_count = 0;

    config.rows = 24;
    config.columns = 80;
    config.scrollback_capacity = 16;
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_terminal_set_cell_pixel_geometry(terminal, 8, 16) !=
            BOXRIG_STATUS_OK ||
        boxrig_terminal_feed(terminal, bytes, length, &consumed) !=
            BOXRIG_STATUS_OK ||
        consumed != length) {
        goto cleanup;
    }
    output_length = boxrig_terminal_output_peek(terminal, &output);
    if (system_backend && find_bytes(bytes, length, (const uint8_t *)"\033_G", 3) == SIZE_MAX) {
        /* Older OS hosts filter Kitty APC before boxrig can receive it.
         * Observe that transport limitation explicitly while still requiring
         * the complete text scene. Packaged-required never takes this branch. */
        static const uint8_t first_label[] = "raw RGB landscape";
        static const uint8_t last_label[] = "scrolled sailboat";
        passed = boxrig_terminal_image_count(terminal) == 0 &&
            boxrig_terminal_image_placement_count(terminal) == 0 &&
            boxrig_terminal_scrollback_rows(terminal) == 1 &&
            terminal_contains(terminal, first_label, sizeof(first_label) - 1) &&
            terminal_contains(terminal, last_label, sizeof(last_label) - 1);
        if (passed) puts("OBSERVE system ConPTY Kitty APC=filtered images=0 placements=0; text scene preserved");
        goto cleanup;
    }
    if ((output_length != 0 &&
         (output_length != sizeof(primary_device_attributes) - 1 ||
          memcmp(
              output,
              primary_device_attributes,
              sizeof(primary_device_attributes) - 1) != 0)) ||
        boxrig_terminal_image_count(terminal) != 8 ||
        boxrig_terminal_image_placement_count(terminal) != 15 ||
        boxrig_terminal_scrollback_rows(terminal) != 1) {
        fprintf(
            stderr,
            "ConPTY graphics state: bytes=%lu consumed=%lu output=%lu "
            "images=%lu placements=%lu history=%lu\n",
            (unsigned long)length,
            (unsigned long)consumed,
            (unsigned long)output_length,
            (unsigned long)boxrig_terminal_image_count(terminal),
            (unsigned long)boxrig_terminal_image_placement_count(terminal),
            (unsigned long)boxrig_terminal_scrollback_rows(terminal));
        for (index = 0;
             index < boxrig_terminal_image_count(terminal);
             index++) {
            boxrig_image_handle handle = 0;
            boxrig_image_view image;

            if (boxrig_terminal_image_handle_at(terminal, index, &handle) ==
                    BOXRIG_STATUS_OK &&
                boxrig_terminal_get_image(terminal, handle, &image) ==
                    BOXRIG_STATUS_OK) {
                fprintf(
                    stderr,
                    "  image id=%lu size=%lux%lu\n",
                    (unsigned long)image.image_id,
                    (unsigned long)image.width,
                    (unsigned long)image.height);
            }
        }
        for (index = 0;
             index < boxrig_terminal_image_placement_count(terminal);
             index++) {
            boxrig_image_placement_view placement;

            if (boxrig_terminal_get_image_placement(
                    terminal,
                    index,
                    &placement) == BOXRIG_STATUS_OK) {
                fprintf(
                    stderr,
                    "  placement id=%lu p=%lu kind=%d visual=%lu,%lu\n",
                    (unsigned long)placement.image_id,
                    (unsigned long)placement.placement_id,
                    (int)placement.kind,
                    (unsigned long)placement.visual_row,
                    (unsigned long)placement.visual_column);
            }
        }
        goto cleanup;
    }
    for (index = 0; index < boxrig_terminal_image_count(terminal); index++) {
        boxrig_image_handle handle = 0;
        boxrig_image_view image;

        if (boxrig_terminal_image_handle_at(terminal, index, &handle) !=
                BOXRIG_STATUS_OK ||
            boxrig_terminal_get_image(terminal, handle, &image) !=
                BOXRIG_STATUS_OK) {
            goto cleanup;
        }
        if (image.image_id == 9101 && image.width == 16 &&
            image.height == 16 && image.rgba_length == 1024 &&
            image_has_rgba(&image, 4, 1, 255, 220, 64, 255) &&
            image_has_rgba(&image, 10, 10, 224, 48, 48, 255) &&
            image_has_rgba(&image, 7, 14, 40, 120, 240, 255)) {
            found_rgb = true;
        } else if (image.image_id == 9102 && image.width == 16 &&
                   image.height == 16 && image.rgba_length == 1024 &&
                   image_has_rgba(&image, 0, 4, 0, 0, 0, 0) &&
                   image_has_rgba(&image, 10, 10, 48, 224, 96, 128)) {
            found_chunked_alpha = true;
        } else if (image.image_id == 9103 && image.width == 16 &&
                   image.height == 16 && image.rgba_length == 1024 &&
                   image_has_rgba(&image, 10, 10, 144, 64, 224, 255)) {
            found_zlib = true;
        } else if (image.image_id == 9104 && image.width == 16 &&
                   image.height == 16 && image.rgba_length == 1024 &&
                   image_has_rgba(&image, 10, 10, 224, 176, 40, 255)) {
            found_png = true;
        } else if (image.image_id == 9201) {
            goto cleanup;
        }
    }
    for (index = 0;
         index < boxrig_terminal_image_placement_count(terminal);
         index++) {
        boxrig_image_placement_view placement;

        if (boxrig_terminal_get_image_placement(
                terminal,
                index,
                &placement) != BOXRIG_STATUS_OK) {
            goto cleanup;
        }
        if (placement.image_id == 9101 && placement.placement_id == 15 &&
            placement.source_x == 8 && placement.source_width == 8 &&
            placement.source_height == 16 && placement.columns == 2 &&
            placement.rows == 2 && placement.visual_row == 3 &&
            placement.visual_column == 6) {
            found_crop = true;
        } else if (placement.image_id == 9200 &&
                   placement.placement_id == 22 &&
                   placement.visual_row == 21 &&
                   placement.visual_column == 1 &&
                   placement.columns == 4 && placement.rows == 1) {
            found_scroll = true;
        } else if (placement.image_id == 9202 &&
                   placement.placement_id == 24 &&
                   placement.visual_row == 12 &&
                   placement.visual_column == 0 &&
                   placement.columns == 4 && placement.rows == 2) {
            found_primary = true;
        } else if (placement.image_id == 42 &&
                   placement.placement_id == 26 &&
                   placement.kind == BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER &&
                   placement.visual_row >= 15 &&
                   placement.visual_row <= 16 &&
                   placement.visual_column < 4 &&
                   placement.source_width == 16 &&
                   placement.source_height == 16 &&
                   placement.columns == 4 && placement.rows == 2 &&
                   placement.clip_width == 8 &&
                   placement.clip_height == 16) {
            placeholder_count++;
            found_placeholder = found_placeholder ||
                (placement.visual_row == 16 &&
                 placement.visual_column == 3 &&
                 placement.clip_x == 24 && placement.clip_y == 16);
        }
        if (placement.image_id == 9201 || placement.image_id == 9203) {
            goto cleanup;
        }
    }
    if (!found_rgb || !found_chunked_alpha || !found_zlib || !found_png ||
        !found_crop || !found_scroll || !found_primary ||
        !found_placeholder || placeholder_count != 8 ||
        boxrig_terminal_set_viewport_offset(terminal, 1) !=
            BOXRIG_STATUS_OK) {
        goto cleanup;
    }
    for (index = 0;
         index < boxrig_terminal_image_placement_count(terminal);
         index++) {
        boxrig_image_placement_view placement;

        if (boxrig_terminal_get_image_placement(
                terminal,
                index,
                &placement) != BOXRIG_STATUS_OK) {
            goto cleanup;
        }
        if (placement.image_id == 9200 && placement.placement_id == 21 &&
            placement.visual_row == 0 && placement.visual_column == 0) {
            found_history = true;
        }
    }
    passed = found_history;

cleanup:
    boxrig_terminal_destroy(terminal);
    return passed;
}

static bool read_pipe_to_end(
    HANDLE pipe,
    uint8_t *buffer,
    size_t capacity,
    size_t *length_out)
{
    size_t length = 0;

    for (;;) {
        DWORD received = 0;
        size_t remaining;

        if (length == capacity) {
            return false;
        }
        remaining = capacity - length;
        if (!ReadFile(
                pipe,
                buffer + length,
                (DWORD)remaining,
                &received,
                NULL)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                *length_out = length;
                return true;
            }
            return false;
        }
        if (received == 0) {
            *length_out = length;
            return true;
        }
        length += received;
    }
}

static bool read_until(
    HANDLE pipe,
    char *buffer,
    size_t capacity,
    size_t *length,
    const char *needle)
{
    read_request request;
    HANDLE thread;
    DWORD wait_result;
    DWORD thread_result = 1;

    if (strstr(buffer, needle) != NULL) {
        return true;
    }
    request.pipe = pipe;
    request.buffer = buffer;
    request.capacity = capacity;
    request.length = length;
    request.needle = needle;
    request.found = false;
    thread = CreateThread(NULL, 0, read_until_thread, &request, 0, NULL);
    if (thread == NULL) {
        return false;
    }
    wait_result = WaitForSingleObject(thread, 10000);
    if (wait_result != WAIT_OBJECT_0) {
        (void)CancelSynchronousIo(thread);
        (void)WaitForSingleObject(thread, INFINITE);
    }
    if (wait_result == WAIT_OBJECT_0) {
        (void)GetExitCodeThread(thread, &thread_result);
    }
    CloseHandle(thread);
    return wait_result == WAIT_OBJECT_0 && thread_result == 0 && request.found;
}

static bool write_byte(HANDLE pipe, char byte)
{
    DWORD written = 0;

    return WriteFile(pipe, &byte, 1, &written, NULL) && written == 1;
}

static bool write_all(HANDLE pipe, const char *bytes)
{
    size_t length = strlen(bytes);
    size_t offset = 0;

    while (offset < length) {
        DWORD written = 0;

        if (!WriteFile(
                pipe,
                bytes + offset,
                (DWORD)(length - offset),
                &written,
                NULL) ||
            written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

static wchar_t *make_shell_command_line(const wchar_t *launcher_path)
{
    size_t capacity = wcslen(launcher_path) + 32;
    wchar_t *command_line = calloc(capacity, sizeof(*command_line));
    int length;

    if (command_line == NULL) {
        return NULL;
    }
    length = swprintf(
        command_line,
        capacity,
        L"\"%ls\" -- cmd.exe /d /q",
        launcher_path);
    if (length < 0 || (size_t)length >= capacity) {
        free(command_line);
        return NULL;
    }
    return command_line;
}

static wchar_t *make_emitter_command_line(const wchar_t *emitter_path)
{
    size_t capacity = wcslen(emitter_path) + 40;
    wchar_t *command_line = calloc(capacity, sizeof(*command_line));
    int length;

    if (command_line == NULL) {
        return NULL;
    }
    length = swprintf(
        command_line,
        capacity,
        L"\"%ls\" -t transport-bytes --no-hold",
        emitter_path);
    if (length < 0 || (size_t)length >= capacity) {
        free(command_line);
        return NULL;
    }
    return command_line;
}

static wchar_t *make_emitter_pty_command_line(
    const wchar_t *launcher_path,
    const wchar_t *emitter_path)
{
    size_t capacity = wcslen(launcher_path) + wcslen(emitter_path) + 48;
    wchar_t *command_line = calloc(capacity, sizeof(*command_line));
    int length;

    if (command_line == NULL) {
        return NULL;
    }
    length = swprintf(
        command_line,
        capacity,
        L"\"%ls\" -- \"%ls\" -t transport-bytes --no-hold",
        launcher_path,
        emitter_path);
    if (length < 0 || (size_t)length >= capacity) {
        free(command_line);
        return NULL;
    }
    return command_line;
}

static wchar_t *make_emitter_graphics_pty_command_line(
    const wchar_t *launcher_path,
    const wchar_t *emitter_path)
{
    size_t capacity = wcslen(launcher_path) + wcslen(emitter_path) + 56;
    wchar_t *command_line = calloc(capacity, sizeof(*command_line));
    int length;

    if (command_line == NULL) {
        return NULL;
    }
    length = swprintf(
        command_line,
        capacity,
        L"\"%ls\" -- \"%ls\" -t kitty-image-static --no-hold",
        launcher_path,
        emitter_path);
    if (length < 0 || (size_t)length >= capacity) {
        free(command_line);
        return NULL;
    }
    return command_line;
}

static bool test_emitter_redirected_bytes(const wchar_t *emitter_path)
{
    SECURITY_ATTRIBUTES security;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process_information;
    HANDLE output_read = NULL;
    HANDLE output_write = NULL;
    HANDLE input = INVALID_HANDLE_VALUE;
    wchar_t *command_line = NULL;
    uint8_t received[4096];
    size_t received_length = 0;
    DWORD exit_code = 1;
    bool passed = false;

    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process_information, 0, sizeof(process_information));
    if (!CreatePipe(&output_read, &output_write, &security, 0) ||
        !SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0)) {
        goto cleanup;
    }
    input = CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        0,
        NULL);
    command_line = make_emitter_command_line(emitter_path);
    if (input == INVALID_HANDLE_VALUE || command_line == NULL) {
        goto cleanup;
    }
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = output_write;
    startup.hStdError = output_write;
    if (!CreateProcessW(
            NULL,
            command_line,
            NULL,
            NULL,
            TRUE,
            0,
            NULL,
            NULL,
            &startup,
            &process_information)) {
        goto cleanup;
    }
    CloseHandle(process_information.hThread);
    process_information.hThread = NULL;
    CloseHandle(output_write);
    output_write = NULL;
    if (!read_pipe_to_end(
            output_read,
            received,
            sizeof(received),
            &received_length) ||
        WaitForSingleObject(process_information.hProcess, 10000) !=
            WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process_information.hProcess, &exit_code) ||
        exit_code != 0 ||
        received_length != sizeof(transport_expected) - 1 ||
        memcmp(
            received,
            transport_expected,
            sizeof(transport_expected) - 1) != 0) {
        fprintf(
            stderr,
            "redirected emitter mismatch: process=%lu bytes=%lu "
            "expected=%lu\n",
            (unsigned long)exit_code,
            (unsigned long)received_length,
            (unsigned long)(sizeof(transport_expected) - 1));
        goto cleanup;
    }
    passed = true;

cleanup:
    if (process_information.hThread != NULL) {
        CloseHandle(process_information.hThread);
    }
    if (process_information.hProcess != NULL) {
        CloseHandle(process_information.hProcess);
    }
    if (input != INVALID_HANDLE_VALUE) {
        CloseHandle(input);
    }
    if (output_write != NULL) {
        CloseHandle(output_write);
    }
    if (output_read != NULL) {
        CloseHandle(output_read);
    }
    free(command_line);
    return passed;
}

static bool test_emitter_transport(
    const wchar_t *launcher_path,
    const wchar_t *emitter_path)
{
    boxrig_win32_pty *pty = NULL;
    wchar_t *command_line = NULL;
    raw_read_request request;
    HANDLE reader = NULL;
    HANDLE process;
    DWORD process_exit = 1;
    DWORD reader_exit = 1;
    DWORD wait_result;
    const char *allow_normalization =
        getenv("BOXRIG_TEST_ALLOW_CONPTY_NORMALIZATION");
    bool passed = false;

    memset(&request, 0, sizeof(request));
    request.capacity = 4096;
    request.buffer = calloc(request.capacity, 1);
    command_line = make_emitter_pty_command_line(
        launcher_path,
        emitter_path);
    if (request.buffer == NULL || command_line == NULL ||
        spawn_test_pty(command_line, 24, 80, &pty) == -1) {
        goto cleanup;
    }
    request.pipe = boxrig_win32_pty_output_handle(pty);
    process = boxrig_win32_pty_process_handle(pty);
    reader = CreateThread(NULL, 0, read_to_end_thread, &request, 0, NULL);
    if (reader == NULL ||
        WaitForSingleObject(process, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process, &process_exit)) {
        goto cleanup;
    }
    boxrig_win32_pty_close(pty);
    wait_result = WaitForSingleObject(reader, 10000);
    if (wait_result != WAIT_OBJECT_0) {
        (void)CancelSynchronousIo(reader);
        (void)WaitForSingleObject(reader, INFINITE);
        goto cleanup;
    }
    if (!GetExitCodeThread(reader, &reader_exit) || reader_exit != 0 ||
        !request.reached_end || process_exit != 0) {
        fprintf(
            stderr,
            "emitter transport failed: process=%lu reader=%lu bytes=%lu\n",
            (unsigned long)process_exit,
            (unsigned long)reader_exit,
            (unsigned long)request.length);
        goto cleanup;
    }
    if (request.length == sizeof(transport_expected) - 1 &&
        memcmp(
            request.buffer,
            transport_expected,
            sizeof(transport_expected) - 1) == 0) {
        passed = true;
        goto cleanup;
    }
    if (allow_normalization != NULL &&
        (strcmp(allow_normalization, "wine") == 0 ||
         strcmp(allow_normalization, "windows") == 0)) {
        if (normalized_transport_has_expected_meaning(
                request.buffer,
                request.length,
                strcmp(allow_normalization, "windows") == 0)) {
            printf(
                "PASS Win32 ConPTY preserved the fixture screen meaning "
                "while normalizing its VT stream\n");
            passed = true;
            goto cleanup;
        }
    }
    fprintf(
        stderr,
        "emitter transport mismatch: process=%lu reader=%lu bytes=%lu "
        "expected=%lu\n",
        (unsigned long)process_exit,
        (unsigned long)reader_exit,
        (unsigned long)request.length,
        (unsigned long)(sizeof(transport_expected) - 1));
    print_hex_bytes(request.buffer, request.length);
    goto cleanup;

cleanup:
    if (reader != NULL) {
        CloseHandle(reader);
    }
    boxrig_win32_pty_destroy(pty);
    free(command_line);
    free(request.buffer);
    return passed;
}

static bool test_emitter_graphics(
    const wchar_t *launcher_path,
    const wchar_t *emitter_path)
{
    boxrig_win32_pty *pty = NULL;
    wchar_t *command_line = NULL;
    raw_read_request request;
    HANDLE reader = NULL;
    HANDLE process;
    DWORD process_exit = 1;
    DWORD reader_exit = 1;
    DWORD wait_result;
    bool passed = false;
    boxrig_win32_pty_diagnostics diagnostic;

    memset(&request, 0, sizeof(request));
    request.capacity = 16384;
    request.buffer = calloc(request.capacity, 1);
    command_line = make_emitter_graphics_pty_command_line(
        launcher_path,
        emitter_path);
    if (request.buffer == NULL || command_line == NULL ||
        spawn_test_pty(command_line, 24, 80, &pty) == -1) {
        goto cleanup;
    }
    request.pipe = boxrig_win32_pty_output_handle(pty);
    if (boxrig_win32_pty_get_diagnostics(pty, &diagnostic) != 0) goto cleanup;
    process = boxrig_win32_pty_process_handle(pty);
    reader = CreateThread(NULL, 0, read_to_end_thread, &request, 0, NULL);
    if (reader == NULL ||
        WaitForSingleObject(process, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process, &process_exit)) {
        goto cleanup;
    }
    boxrig_win32_pty_close(pty);
    wait_result = WaitForSingleObject(reader, 10000);
    if (wait_result != WAIT_OBJECT_0) {
        (void)CancelSynchronousIo(reader);
        (void)WaitForSingleObject(reader, INFINITE);
        goto cleanup;
    }
    if (!GetExitCodeThread(reader, &reader_exit) || reader_exit != 0 ||
        !request.reached_end || process_exit != 0 ||
        !graphics_output_has_expected_meaning(
            request.buffer,
            request.length,
            diagnostic.selected_backend == BOXRIG_WIN32_PTY_BACKEND_SYSTEM)) {
        fprintf(
            stderr,
            "emitter graphics failed: process=%lu reader=%lu bytes=%lu\n",
            (unsigned long)process_exit,
            (unsigned long)reader_exit,
            (unsigned long)request.length);
        print_hex_bytes(request.buffer, request.length);
        goto cleanup;
    }
    passed = true;

cleanup:
    if (reader != NULL) {
        CloseHandle(reader);
    }
    boxrig_win32_pty_destroy(pty);
    free(command_line);
    free(request.buffer);
    return passed;
}

static bool test_emitter_graphics_when_supported(
    const wchar_t *launcher_path,
    const wchar_t *emitter_path)
{
    const char *normalization =
        getenv("BOXRIG_TEST_ALLOW_CONPTY_NORMALIZATION");

    if (normalization != NULL && strcmp(normalization, "wine") == 0) {
        printf(
            "SKIP Win32 ConPTY graphics: Wine does not preserve Kitty APC "
            "output\n");
        return true;
    }
    return test_emitter_graphics(launcher_path, emitter_path);
}

static bool test_launcher_term(
    const wchar_t *launcher_path,
    const wchar_t *term_override,
    const char *expected_term,
    DWORD expected_exit_code)
{
    boxrig_win32_pty *pty = NULL;
    wchar_t *command_line = NULL;
    HANDLE input;
    HANDLE output;
    HANDLE process;
    char received[8192] = {0};
    char commands[96];
    char expected[64];
    size_t received_length = 0;
    DWORD exit_code = 1;
    bool passed = false;

    if (!SetEnvironmentVariableW(L"BOXRIG_TERM", term_override)) {
        goto cleanup;
    }
    command_line = make_shell_command_line(launcher_path);
    if (command_line == NULL ||
        spawn_test_pty(command_line, 24, 80, &pty) == -1) {
        goto cleanup;
    }
    input = boxrig_win32_pty_input_handle(pty);
    output = boxrig_win32_pty_output_handle(pty);
    process = boxrig_win32_pty_process_handle(pty);
    if (snprintf(
            commands,
            sizeof(commands),
            "echo TERM=%%TERM%%\rexit %lu\r",
            (unsigned long)expected_exit_code) < 0 ||
        snprintf(
            expected,
            sizeof(expected),
            "TERM=%s",
            expected_term) < 0 ||
        !write_all(input, commands) ||
        !read_until(
            output,
            received,
            sizeof(received),
            &received_length,
            expected) ||
        WaitForSingleObject(process, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process, &exit_code) ||
        exit_code != expected_exit_code) {
        fprintf(
            stderr,
            "console launcher TERM contract failed; output: %s\n",
            received);
        goto cleanup;
    }
    passed = true;

cleanup:
    boxrig_win32_pty_destroy(pty);
    free(command_line);
    return passed;
}

static bool test_launcher_survives_more_ctrl_c(
    const wchar_t *launcher_path)
{
    const char *normalization =
        getenv("BOXRIG_TEST_ALLOW_CONPTY_NORMALIZATION");
    boxrig_win32_pty *pty = NULL;
    wchar_t *command_line = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE input;
    HANDLE output;
    HANDLE process;
    char received[8192] = {0};
    char run_more[256];
    char file_path[128];
    wchar_t wide_file_path[128] = {0};
    size_t received_length = 0;
    DWORD exit_code = 1;
    unsigned int line;
    bool file_created = false;
    bool passed = false;

    if (normalization != NULL && strcmp(normalization, "wine") == 0) {
        printf(
            "SKIP Win32 more Ctrl-C: Wine ConPTY does not reliably "
            "translate ETX to a console control event\n");
        return true;
    }
    if (snprintf(
            file_path,
            sizeof(file_path),
            "boxrig_more_ctrl_c_%lu.txt",
            (unsigned long)GetCurrentProcessId()) < 0 ||
        swprintf(
            wide_file_path,
            sizeof(wide_file_path) / sizeof(wide_file_path[0]),
            L"boxrig_more_ctrl_c_%lu.txt",
            (unsigned long)GetCurrentProcessId()) < 0 ||
        snprintf(
            run_more,
            sizeof(run_more),
            "more < \"%s\"\r",
            file_path) < 0) {
        goto cleanup;
    }
    file = CreateFileW(
        wide_file_path,
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        NULL);
    if (file == INVALID_HANDLE_VALUE) {
        goto cleanup;
    }
    file_created = true;
    for (line = 1; line <= 80; line++) {
        char text[64];
        int length = snprintf(
            text,
            sizeof(text),
            "BOXRIG_MORE_LINE_%u\r\n",
            line);
        DWORD written = 0;

        if (length < 0 || (size_t)length >= sizeof(text) ||
            !WriteFile(file, text, (DWORD)length, &written, NULL) ||
            written != (DWORD)length) {
            goto cleanup;
        }
    }
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    command_line = make_shell_command_line(launcher_path);
    if (command_line == NULL ||
        spawn_test_pty(command_line, 24, 80, &pty) == -1) {
        goto cleanup;
    }
    input = boxrig_win32_pty_input_handle(pty);
    output = boxrig_win32_pty_output_handle(pty);
    process = boxrig_win32_pty_process_handle(pty);

    /* Twenty lines fit before the first page boundary at the 24-row test
     * size. Seeing that line proves more is consuming the file before ETX is
     * sent through the pseudoconsole input pipe. */
    if (!write_all(input, "prompt @\r") ||
        !write_all(input, run_more) ||
        !read_until(
            output,
            received,
            sizeof(received),
            &received_length,
            "BOXRIG_MORE_LINE_20")) {
        goto failure;
    }
    received[0] = '\0';
    received_length = 0;

    /* Wait for cmd.exe to print the single-character prompt after handling
     * Ctrl-C. This avoids racing its input-buffer flush with the next command;
     * the file and more's localized prompt deliberately contain no '@'. */
    if (!write_byte(input, '\x03') ||
        !read_until(
            output,
            received,
            sizeof(received),
            &received_length,
            "@")) {
        goto failure;
    }
    received[0] = '\0';
    received_length = 0;

    /* A surviving shell must accept another command. The frontend monitors
     * the launcher process, so its final zero exit also proves Ctrl-C did not
     * terminate that launcher while it was waiting for the shell. */
    if (!write_all(input, "echo BOXRIG_AFTER_CTRL_C & exit 0\r") ||
        !read_until(
            output,
            received,
            sizeof(received),
            &received_length,
            "BOXRIG_AFTER_CTRL_C") ||
        WaitForSingleObject(process, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process, &exit_code) || exit_code != 0) {
        goto failure;
    }
    passed = true;
    goto cleanup;

failure:
    (void)GetExitCodeProcess(process, &exit_code);
    fprintf(
        stderr,
        "console launcher did not survive more Ctrl-C "
        "(exit=%lu); output: %s\n",
        (unsigned long)exit_code,
        received);

cleanup:
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
    boxrig_win32_pty_destroy(pty);
    if (file_created) {
        (void)DeleteFileW(wide_file_path);
    }
    free(command_line);
    return passed;
}

int wmain(int argument_count, wchar_t **arguments)
{
    boxrig_win32_pty *pty = NULL;
    HANDLE input;
    HANDLE output;
    HANDLE process;
    char received[8192] = {0};
    size_t received_length = 0;
    DWORD exit_code = 1;
    bool resize_supported;
    bool passed = false;

    if (argument_count == 4 &&
        wcscmp(arguments[1], L"--emitter-transport") == 0) {
        if (!test_launcher_survives_more_ctrl_c(arguments[2]) ||
            !test_emitter_redirected_bytes(arguments[3]) ||
            !test_emitter_transport(arguments[2], arguments[3]) ||
            !test_emitter_graphics_when_supported(
                arguments[2],
                arguments[3])) {
            return 1;
        }
        printf(
            "PASS Win32 redirected and ConPTY emitter transport and "
            "graphics meaning\n");
        return 0;
    }
    if (argument_count != 4) {
        fwprintf(
            stderr,
            L"usage: %ls PTY_HELPER CONSOLE_LAUNCHER BOXRIG_EMITTER\n",
            arguments[0]);
        return 2;
    }
    SetLastError(ERROR_SUCCESS);
    if (boxrig_win32_pty_spawn(NULL, 24, 80, &pty) != -1 ||
        GetLastError() != ERROR_INVALID_PARAMETER || pty != NULL) {
        fprintf(stderr, "invalid spawn arguments were accepted\n");
        goto cleanup;
    }
    if (spawn_test_pty(arguments[1], 24, 80, &pty) == -1) {
        fprintf(stderr, "could not create pseudoconsole: %lu\n", GetLastError());
        goto cleanup;
    }
    input = boxrig_win32_pty_input_handle(pty);
    output = boxrig_win32_pty_output_handle(pty);
    process = boxrig_win32_pty_process_handle(pty);
    if (input == NULL || input == INVALID_HANDLE_VALUE ||
        output == NULL || output == INVALID_HANDLE_VALUE ||
        process == NULL || process == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "pseudoconsole exposed invalid handles\n");
        goto cleanup;
    }
    SetLastError(ERROR_SUCCESS);
    if (boxrig_win32_pty_resize(pty, 0, 100) != -1 ||
        GetLastError() != ERROR_INVALID_PARAMETER) {
        fprintf(stderr, "invalid resize dimensions were accepted\n");
        goto cleanup;
    }
    if (!read_until(
            output,
            received,
            sizeof(received),
            &received_length,
            "READY 80 24")) {
        fprintf(stderr, "child did not observe initial size; output: %s\n", received);
        goto cleanup;
    }
    SetLastError(ERROR_SUCCESS);
    resize_supported = boxrig_win32_pty_resize(pty, 40, 100) == 0;
    if ((!resize_supported && GetLastError() != ERROR_CALL_NOT_IMPLEMENTED) ||
        !write_byte(input, 's') ||
        !read_until(
            output,
            received,
            sizeof(received),
            &received_length,
            resize_supported ? "SIZE 100 40" : "SIZE 80 24")) {
        fprintf(stderr, "child did not observe resized grid; output: %s\n", received);
        goto cleanup;
    }
    if (!write_byte(input, 'q') ||
        !read_until(
            output,
            received,
            sizeof(received),
            &received_length,
            "\033[32mDONE\033[0m") ||
        WaitForSingleObject(process, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process, &exit_code) || exit_code != 0) {
        fprintf(stderr, "pseudoconsole I/O or child exit failed; output: %s\n", received);
        goto cleanup;
    }
    boxrig_win32_pty_destroy(pty);
    pty = NULL;
    if (!test_launcher_term(
            arguments[2],
            NULL,
            "xterm-256color",
            23) ||
        !test_launcher_term(arguments[2], L"xterm", "xterm", 0) ||
        !test_launcher_survives_more_ctrl_c(arguments[2]) ||
        !test_emitter_redirected_bytes(arguments[3]) ||
        !test_emitter_transport(arguments[2], arguments[3]) ||
        !test_emitter_graphics_when_supported(
            arguments[2],
            arguments[3])) {
        goto cleanup;
    }
    passed = true;

cleanup:
    boxrig_win32_pty_destroy(pty);
    if (!passed) {
        return 1;
    }
    printf(
        "PASS Win32 redirected bytes, pseudoconsole I/O, transport and "
        "graphics meaning, resize, TERM selection, more Ctrl-C, and child "
        "exit\n");
    return 0;
}
