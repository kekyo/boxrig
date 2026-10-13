/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "boxrig/win32.h"
#include "host_private.h"

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

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
    char encoded[8192];
    char command[8352];
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
    size_t output_length = boxrig_terminal_output_peek(terminal, &output);
    size_t expected_length = strlen(expected);

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

#define FAKE_FILE_HANDLE ((HANDLE)(uintptr_t)1)
#define FAKE_MAPPING_HANDLE ((HANDLE)(uintptr_t)2)

typedef struct fake_graphics_provider {
    wchar_t opened_name[256];
    wchar_t mapping_name[256];
    wchar_t final_path[256];
    wchar_t temporary_path[256];
    wchar_t deleted_path[256];
    BY_HANDLE_FILE_INFORMATION information;
    uint64_t file_size;
    uint64_t file_offset;
    uint8_t data[64];
    size_t data_length;
    size_t information_calls;
    size_t create_file_calls;
    size_t read_calls;
    size_t delete_calls;
    size_t close_file_calls;
    size_t open_mapping_calls;
    size_t map_calls;
    size_t query_calls;
    size_t unmap_calls;
    size_t close_mapping_calls;
    DWORD file_type;
    bool changed_after_read;
    bool force_end_of_file;
    bool delete_succeeds;
} fake_graphics_provider;

static HANDLE fake_create_file(
    void *context,
    LPCWSTR name,
    DWORD desired_access,
    DWORD share_mode,
    DWORD creation_disposition,
    DWORD flags_and_attributes)
{
    fake_graphics_provider *fake = context;

    CHECK(desired_access == GENERIC_READ);
    CHECK(share_mode ==
          (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE));
    CHECK(creation_disposition == OPEN_EXISTING);
    CHECK((flags_and_attributes & FILE_FLAG_OPEN_REPARSE_POINT) == 0);
    CHECK(wcslen(name) < sizeof(fake->opened_name) / sizeof(wchar_t));
    wcscpy(fake->opened_name, name);
    fake->create_file_calls++;
    return FAKE_FILE_HANDLE;
}

static DWORD fake_get_file_type(void *context, HANDLE handle)
{
    fake_graphics_provider *fake = context;

    CHECK(handle == FAKE_FILE_HANDLE);
    return fake->file_type;
}

static BOOL fake_get_file_information(
    void *context,
    HANDLE handle,
    BY_HANDLE_FILE_INFORMATION *information)
{
    fake_graphics_provider *fake = context;

    CHECK(handle == FAKE_FILE_HANDLE);
    *information = fake->information;
    if (fake->changed_after_read && fake->information_calls != 0) {
        information->nFileSizeLow++;
    }
    fake->information_calls++;
    return TRUE;
}

static BOOL fake_get_file_size(
    void *context,
    HANDLE handle,
    LARGE_INTEGER *size)
{
    fake_graphics_provider *fake = context;

    CHECK(handle == FAKE_FILE_HANDLE);
    size->QuadPart = (LONGLONG)fake->file_size;
    return TRUE;
}

static BOOL fake_set_file_pointer(
    void *context,
    HANDLE handle,
    LARGE_INTEGER distance,
    DWORD move_method)
{
    fake_graphics_provider *fake = context;

    CHECK(handle == FAKE_FILE_HANDLE);
    CHECK(move_method == FILE_BEGIN);
    CHECK(distance.QuadPart >= 0);
    fake->file_offset = (uint64_t)distance.QuadPart;
    return TRUE;
}

static BOOL fake_read_file(
    void *context,
    HANDLE handle,
    void *bytes,
    DWORD capacity,
    DWORD *length_out)
{
    fake_graphics_provider *fake = context;
    uint64_t available;
    size_t length;

    CHECK(handle == FAKE_FILE_HANDLE);
    fake->read_calls++;
    if (fake->force_end_of_file) {
        *length_out = 0;
        return TRUE;
    }
    available = fake->file_offset >= fake->data_length
                    ? 0
                    : fake->data_length - fake->file_offset;
    length = available < capacity ? (size_t)available : (size_t)capacity;
    if (length != 0) {
        memcpy(bytes, fake->data + fake->file_offset, length);
    }
    fake->file_offset += length;
    *length_out = (DWORD)length;
    return TRUE;
}

static DWORD fake_get_final_path(
    void *context,
    HANDLE handle,
    wchar_t *path,
    DWORD capacity,
    DWORD flags)
{
    fake_graphics_provider *fake = context;
    size_t length = wcslen(fake->final_path);

    CHECK(handle == FAKE_FILE_HANDLE);
    CHECK(flags == (FILE_NAME_NORMALIZED | VOLUME_NAME_DOS));
    if (path == NULL || capacity <= length) {
        return (DWORD)length + 1;
    }
    memcpy(path, fake->final_path, (length + 1) * sizeof(*path));
    return (DWORD)length;
}

static DWORD fake_get_temporary_path(
    void *context,
    DWORD capacity,
    wchar_t *path)
{
    fake_graphics_provider *fake = context;
    size_t length = wcslen(fake->temporary_path);

    if (capacity <= length) {
        return (DWORD)length + 1;
    }
    memcpy(path, fake->temporary_path, (length + 1) * sizeof(*path));
    return (DWORD)length;
}

static BOOL fake_delete_file(void *context, LPCWSTR path)
{
    fake_graphics_provider *fake = context;

    CHECK(wcslen(path) < sizeof(fake->deleted_path) / sizeof(wchar_t));
    wcscpy(fake->deleted_path, path);
    fake->delete_calls++;
    return fake->delete_succeeds ? TRUE : FALSE;
}

static HANDLE fake_open_file_mapping(
    void *context,
    DWORD desired_access,
    LPCWSTR name)
{
    fake_graphics_provider *fake = context;

    CHECK(desired_access == FILE_MAP_READ);
    CHECK(wcslen(name) < sizeof(fake->mapping_name) / sizeof(wchar_t));
    wcscpy(fake->mapping_name, name);
    fake->open_mapping_calls++;
    return FAKE_MAPPING_HANDLE;
}

static void *fake_map_view(void *context, HANDLE handle, DWORD desired_access)
{
    fake_graphics_provider *fake = context;

    CHECK(handle == FAKE_MAPPING_HANDLE);
    CHECK(desired_access == FILE_MAP_READ);
    fake->map_calls++;
    return fake->data;
}

static SIZE_T fake_virtual_query(
    void *context,
    const void *address,
    MEMORY_BASIC_INFORMATION *information,
    SIZE_T information_length)
{
    fake_graphics_provider *fake = context;

    CHECK(address == fake->data);
    CHECK(information_length == sizeof(*information));
    memset(information, 0, sizeof(*information));
    information->AllocationBase = fake->data;
    information->BaseAddress = fake->data;
    information->RegionSize = fake->data_length;
    information->State = MEM_COMMIT;
    information->Protect = PAGE_READONLY;
    information->Type = MEM_MAPPED;
    fake->query_calls++;
    return sizeof(*information);
}

static BOOL fake_unmap_view(void *context, const void *address)
{
    fake_graphics_provider *fake = context;

    CHECK(address == fake->data);
    fake->unmap_calls++;
    return TRUE;
}

static BOOL fake_close_handle(void *context, HANDLE handle)
{
    fake_graphics_provider *fake = context;

    if (handle == FAKE_FILE_HANDLE) {
        fake->close_file_calls++;
    } else {
        CHECK(handle == FAKE_MAPPING_HANDLE);
        fake->close_mapping_calls++;
    }
    return TRUE;
}

static boxrig_win32_graphics_provider graphics_provider(
    fake_graphics_provider *fake)
{
    boxrig_win32_graphics_provider provider;

    provider.create_file = fake_create_file;
    provider.get_file_type = fake_get_file_type;
    provider.get_file_information = fake_get_file_information;
    provider.get_file_size = fake_get_file_size;
    provider.set_file_pointer = fake_set_file_pointer;
    provider.read_file = fake_read_file;
    provider.get_final_path = fake_get_final_path;
    provider.get_temporary_path = fake_get_temporary_path;
    provider.delete_file = fake_delete_file;
    provider.open_file_mapping = fake_open_file_mapping;
    provider.map_view = fake_map_view;
    provider.virtual_query = fake_virtual_query;
    provider.unmap_view = fake_unmap_view;
    provider.close_handle = fake_close_handle;
    provider.context = fake;
    return provider;
}

static void initialize_fake_graphics_provider(fake_graphics_provider *fake)
{
    static const uint8_t data[] = {9, 8, 1, 2, 3, 4};

    memset(fake, 0, sizeof(*fake));
    memcpy(fake->data, data, sizeof(data));
    fake->data_length = sizeof(data);
    fake->file_size = sizeof(data);
    fake->file_type = FILE_TYPE_DISK;
    fake->delete_succeeds = true;
    fake->information.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    fake->information.dwVolumeSerialNumber = 7;
    fake->information.nFileIndexLow = 11;
    fake->information.nFileSizeLow = sizeof(data);
    fake->information.ftLastWriteTime.dwLowDateTime = 13;
    wcscpy(fake->temporary_path, L"C:\\Temp\\");
    wcscpy(
        fake->final_path,
        L"\\\\?\\C:\\Temp\\tty-graphics-protocol-image.bin");
}

static bool clipboard_equals(HWND window, const wchar_t *expected)
{
    HANDLE handle;
    const wchar_t *text;
    bool matches;

    if (!OpenClipboard(window)) {
        return false;
    }
    handle = GetClipboardData(CF_UNICODETEXT);
    if (handle == NULL) {
        (void)CloseClipboard();
        return false;
    }
    text = GlobalLock(handle);
    if (text == NULL) {
        (void)CloseClipboard();
        return false;
    }
    matches = wcscmp(text, expected) == 0;
    (void)GlobalUnlock(handle);
    (void)CloseClipboard();
    return matches;
}

enum {
    FAKE_NOTIFICATION_CALL_CAPACITY = 64
};

typedef struct fake_notification_call {
    DWORD message;
    NOTIFYICONDATAW data;
} fake_notification_call;

typedef struct fake_notification_provider {
    fake_notification_call calls[FAKE_NOTIFICATION_CALL_CAPACITY];
    size_t call_count;
    bool failure_armed;
    DWORD failure_message;
    bool focused;
    bool visible;
    uint64_t milliseconds;
} fake_notification_provider;

static BOOL fake_shell_notify_icon(
    void *context,
    DWORD message,
    PNOTIFYICONDATAW data)
{
    fake_notification_provider *provider = context;

    if (provider->call_count == FAKE_NOTIFICATION_CALL_CAPACITY) {
        return FALSE;
    }
    provider->calls[provider->call_count].message = message;
    provider->calls[provider->call_count].data = *data;
    provider->call_count++;
    if (provider->failure_armed &&
        provider->failure_message == message) {
        provider->failure_armed = false;
        return FALSE;
    }
    return TRUE;
}

static bool fake_window_focused(void *context, HWND window)
{
    fake_notification_provider *provider = context;

    (void)window;
    return provider->focused;
}

static bool fake_window_visible(void *context, HWND window)
{
    fake_notification_provider *provider = context;

    (void)window;
    return provider->visible;
}

static uint64_t fake_notification_clock(void *context)
{
    fake_notification_provider *provider = context;

    return provider->milliseconds;
}

static bool notification_request_is_completed(
    boxrig_terminal *terminal,
    uint64_t request_id)
{
    return boxrig_terminal_complete_host_request(
               terminal,
               request_id,
               BOXRIG_HOST_REQUEST_SUCCESS,
               NULL,
               0) == BOXRIG_STATUS_INVALID_ARGUMENT;
}

static bool submit_notification(
    boxrig_terminal *terminal,
    boxrig_win32_host_adapter *adapter,
    const uint8_t *bytes,
    size_t length,
    uint64_t *request_id_out)
{
    boxrig_host_item item;

    if (!feed(terminal, bytes, length) ||
        !boxrig_terminal_host_item_peek(terminal, &item) ||
        item.type != BOXRIG_HOST_ITEM_NOTIFICATION) {
        return false;
    }
    *request_id_out = item.request_id;
    return boxrig_win32_host_adapter_process_items(adapter) == 0 &&
           notification_request_is_completed(terminal, item.request_id);
}

static bool create_notification_adapter(
    HWND window,
    fake_notification_provider *fake,
    bool allow_notifications,
    boxrig_terminal **terminal_out,
    boxrig_ui_model **model_out,
    boxrig_ui_host_item_consumer **consumer_out,
    boxrig_win32_host_adapter **adapter_out)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_win32_host_policy policy = boxrig_win32_host_policy_default();
    boxrig_win32_notification_provider provider;

    config.rows = 1;
    config.columns = 1;
    policy.notification.allow_notifications = allow_notifications;
    policy.notification.token_capacity = 32;
    policy.notification.clock = fake_notification_clock;
    policy.notification.clock_context = fake;
    provider.shell_notify_icon = fake_shell_notify_icon;
    provider.window_focused = fake_window_focused;
    provider.window_visible = fake_window_visible;
    provider.context = fake;
    return boxrig_terminal_create(&config, terminal_out) == BOXRIG_STATUS_OK &&
           boxrig_ui_model_create(*terminal_out, model_out) == BOXRIG_UI_OK &&
           boxrig_ui_host_item_consumer_create(*model_out, consumer_out) ==
               BOXRIG_UI_OK &&
           boxrig_win32_host_adapter_create_with_notification_provider(
               &(boxrig_win32_host_targets){window, window, window},
               *consumer_out,
               config.clipboard_capacity,
               &policy,
               &provider,
               adapter_out) == 0;
}

static bool test_notification_policy_and_presentation(HWND window)
{
    static const uint8_t rich[] =
        "\x1b]99;i=rich:d=0:u=2;Alert\x1b\\"
        "\x1b]99;i=rich:p=body;Body\x1b\\";
    static const uint8_t update[] =
        "\x1b]99;i=rich;Changed\x1b\\";
    static const uint8_t unfocused[] =
        "\x1b]99;i=focus:o=unfocused;Focus\x1b\\";
    static const uint8_t invisible[] =
        "\x1b]99;i=hidden:o=invisible;Hidden\x1b\\";
    static const uint8_t low[] =
        "\x1b]99;i=low:u=0;Low\x1b\\";
    fake_notification_provider fake;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_host_item_consumer *consumer = NULL;
    boxrig_win32_host_adapter *adapter = NULL;
    boxrig_win32_host_policy default_policy =
        boxrig_win32_host_policy_default();
    uint64_t request_id;
    char long_notification[1024];
    size_t offset = 0;
    size_t index;

    memset(&fake, 0, sizeof(fake));
    fake.focused = true;
    fake.visible = true;
    fake.milliseconds = 1000;
    CHECK(!default_policy.notification.allow_notifications);
    CHECK(create_notification_adapter(
        window,
        &fake,
        false,
        &terminal,
        &model,
        &consumer,
        &adapter));
    CHECK(submit_notification(
        terminal,
        adapter,
        rich,
        sizeof(rich) - 1,
        &request_id));
    CHECK(fake.call_count == 0);
    boxrig_win32_host_adapter_destroy(adapter);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    model = NULL;
    consumer = NULL;
    adapter = NULL;
    CHECK(create_notification_adapter(
        window,
        &fake,
        true,
        &terminal,
        &model,
        &consumer,
        &adapter));
    CHECK(submit_notification(
        terminal,
        adapter,
        rich,
        sizeof(rich) - 1,
        &request_id));
    CHECK(fake.call_count == 3);
    CHECK(fake.calls[0].message == NIM_ADD);
    CHECK(fake.calls[0].data.cbSize == sizeof(NOTIFYICONDATAW));
    CHECK(fake.calls[0].data.hWnd == window);
    CHECK(fake.calls[0].data.uID != 0);
    CHECK(fake.calls[0].data.uFlags == NIF_ICON);
    CHECK(fake.calls[0].data.hIcon != NULL);
    CHECK(fake.calls[1].message == NIM_SETVERSION);
    CHECK(fake.calls[1].data.uVersion == NOTIFYICON_VERSION_4);
    CHECK(fake.calls[2].message == NIM_MODIFY);
    CHECK(fake.calls[2].data.uFlags == NIF_INFO);
    CHECK(wcscmp(fake.calls[2].data.szInfoTitle, L"Alert") == 0);
    CHECK(wcscmp(fake.calls[2].data.szInfo, L"Body") == 0);
    CHECK(fake.calls[2].data.dwInfoFlags == NIIF_ERROR);

    CHECK(submit_notification(
        terminal,
        adapter,
        rich,
        sizeof(rich) - 1,
        &request_id));
    CHECK(fake.call_count == 3);
    CHECK(submit_notification(
        terminal,
        adapter,
        update,
        sizeof(update) - 1,
        &request_id));
    CHECK(fake.call_count == 4);
    CHECK(fake.calls[3].message == NIM_MODIFY);
    CHECK(wcscmp(fake.calls[3].data.szInfoTitle, L"Changed") == 0);
    CHECK(fake.calls[3].data.dwInfoFlags == NIIF_INFO);

    CHECK(submit_notification(
        terminal,
        adapter,
        unfocused,
        sizeof(unfocused) - 1,
        &request_id));
    CHECK(fake.call_count == 4);
    fake.focused = false;
    CHECK(submit_notification(
        terminal,
        adapter,
        unfocused,
        sizeof(unfocused) - 1,
        &request_id));
    CHECK(fake.call_count == 5);
    CHECK(submit_notification(
        terminal,
        adapter,
        invisible,
        sizeof(invisible) - 1,
        &request_id));
    CHECK(fake.call_count == 5);
    fake.visible = false;
    CHECK(submit_notification(
        terminal,
        adapter,
        invisible,
        sizeof(invisible) - 1,
        &request_id));
    CHECK(fake.call_count == 6);
    CHECK(submit_notification(
        terminal,
        adapter,
        low,
        sizeof(low) - 1,
        &request_id));
    CHECK(fake.call_count == 7);
    CHECK(fake.calls[6].data.dwInfoFlags == NIIF_INFO);

    memcpy(
        long_notification + offset,
        "\x1b]99;i=long:d=0;",
        sizeof("\x1b]99;i=long:d=0;") - 1);
    offset += sizeof("\x1b]99;i=long:d=0;") - 1;
    for (index = 0; index < 61; index++) {
        long_notification[offset++] = 'A';
    }
    memcpy(long_notification + offset, "\xf0\x9f\x98\x80", 4);
    offset += 4;
    for (index = 0; index < 20; index++) {
        long_notification[offset++] = 'B';
    }
    memcpy(long_notification + offset, "\x1b\\", 2);
    offset += 2;
    memcpy(
        long_notification + offset,
        "\x1b]99;i=long:p=body;",
        sizeof("\x1b]99;i=long:p=body;") - 1);
    offset += sizeof("\x1b]99;i=long:p=body;") - 1;
    for (index = 0; index < 253; index++) {
        long_notification[offset++] = 'C';
    }
    memcpy(long_notification + offset, "\xf0\x9f\x98\x80tail\x1b\\", 10);
    offset += 10;
    CHECK(submit_notification(
        terminal,
        adapter,
        (const uint8_t *)long_notification,
        offset,
        &request_id));
    CHECK(fake.call_count == 8);
    CHECK(wcslen(fake.calls[7].data.szInfoTitle) == 62);
    for (index = 0; index < 61; index++) {
        CHECK(fake.calls[7].data.szInfoTitle[index] == L'A');
    }
    CHECK(fake.calls[7].data.szInfoTitle[61] == L'\x2026');
    CHECK(wcslen(fake.calls[7].data.szInfo) == 254);
    for (index = 0; index < 253; index++) {
        CHECK(fake.calls[7].data.szInfo[index] == L'C');
    }
    CHECK(fake.calls[7].data.szInfo[253] == L'\x2026');

    boxrig_win32_host_adapter_destroy(adapter);
    CHECK(fake.call_count == 9);
    CHECK(fake.calls[8].message == NIM_DELETE);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_notification_provider_failures(HWND window)
{
    static const uint8_t notification[] =
        "\x1b]99;i=failure;Retry\x1b\\";
    fake_notification_provider fake;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_host_item_consumer *consumer = NULL;
    boxrig_win32_host_adapter *adapter = NULL;
    uint64_t request_id;

    memset(&fake, 0, sizeof(fake));
    fake.milliseconds = 1000;
    fake.failure_armed = true;
    fake.failure_message = NIM_ADD;
    CHECK(create_notification_adapter(
        window,
        &fake,
        true,
        &terminal,
        &model,
        &consumer,
        &adapter));
    CHECK(submit_notification(
        terminal,
        adapter,
        notification,
        sizeof(notification) - 1,
        &request_id));
    CHECK(fake.call_count == 1);
    CHECK(fake.calls[0].message == NIM_ADD);
    CHECK(submit_notification(
        terminal,
        adapter,
        notification,
        sizeof(notification) - 1,
        &request_id));
    CHECK(fake.call_count == 4);
    CHECK(fake.calls[1].message == NIM_ADD);
    CHECK(fake.calls[2].message == NIM_SETVERSION);
    CHECK(fake.calls[3].message == NIM_MODIFY);
    boxrig_win32_host_adapter_destroy(adapter);
    CHECK(fake.call_count == 5);
    CHECK(fake.calls[4].message == NIM_DELETE);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);

    memset(&fake, 0, sizeof(fake));
    fake.milliseconds = 1000;
    fake.failure_armed = true;
    fake.failure_message = NIM_SETVERSION;
    terminal = NULL;
    model = NULL;
    consumer = NULL;
    adapter = NULL;
    CHECK(create_notification_adapter(
        window,
        &fake,
        true,
        &terminal,
        &model,
        &consumer,
        &adapter));
    CHECK(submit_notification(
        terminal,
        adapter,
        notification,
        sizeof(notification) - 1,
        &request_id));
    CHECK(fake.call_count == 3);
    CHECK(fake.calls[0].message == NIM_ADD);
    CHECK(fake.calls[1].message == NIM_SETVERSION);
    CHECK(fake.calls[2].message == NIM_DELETE);
    boxrig_win32_host_adapter_destroy(adapter);
    CHECK(fake.call_count == 3);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);

    memset(&fake, 0, sizeof(fake));
    fake.milliseconds = 1000;
    fake.failure_armed = true;
    fake.failure_message = NIM_MODIFY;
    terminal = NULL;
    model = NULL;
    consumer = NULL;
    adapter = NULL;
    CHECK(create_notification_adapter(
        window,
        &fake,
        true,
        &terminal,
        &model,
        &consumer,
        &adapter));
    CHECK(submit_notification(
        terminal,
        adapter,
        notification,
        sizeof(notification) - 1,
        &request_id));
    CHECK(fake.call_count == 4);
    CHECK(fake.calls[0].message == NIM_ADD);
    CHECK(fake.calls[1].message == NIM_SETVERSION);
    CHECK(fake.calls[2].message == NIM_MODIFY);
    CHECK(fake.calls[3].message == NIM_DELETE);
    CHECK(submit_notification(
        terminal,
        adapter,
        notification,
        sizeof(notification) - 1,
        &request_id));
    CHECK(fake.call_count == 7);
    CHECK(fake.calls[4].message == NIM_ADD);
    CHECK(fake.calls[5].message == NIM_SETVERSION);
    CHECK(fake.calls[6].message == NIM_MODIFY);
    boxrig_win32_host_adapter_destroy(adapter);
    CHECK(fake.call_count == 8);
    CHECK(fake.calls[7].message == NIM_DELETE);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_host_adapter(void)
{
    static const uint8_t title_stack[] =
        "\x1b[22;0;0t\x1b]2;temporary\x07\x1b[23;0;0t";
    static const uint8_t metadata_input[] =
        "\x1b]2;t\xc3\xa9\x1b\\"
        "\x1b]7;file://host/untrusted\x07"
        "\x1b]8;;https://example.test/\x07"
        "A"
        "\x1b]8;;\x07";
    static const uint8_t clipboard_write[] = "\x1b]52;c;SGVsbG8=\x07";
    static const uint8_t clipboard_query_bel[] = "\x1b]52;c;?\x07";
    static const uint8_t clipboard_query_st[] = "\x1b]52;c;?\x1b\\";
    static const uint8_t denied_reply[] = "\x1b]52;c;\x07";
    static const uint8_t allowed_reply[] = "\x1b]52;c;SGVsbG8=\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_host_item_consumer *consumer = NULL;
    boxrig_ui_host_item_consumer *duplicate_consumer = NULL;
    boxrig_win32_host_adapter *adapter = NULL;
    boxrig_win32_host_policy policy = boxrig_win32_host_policy_default();
    HWND window = CreateWindowExW(
        0,
        L"STATIC",
        L"initial",
        WS_OVERLAPPED,
        0,
        0,
        64,
        32,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    HWND clipboard_window = CreateWindowExW(
        0,
        L"STATIC",
        L"clipboard",
        WS_CHILD,
        0,
        0,
        16,
        16,
        window,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    boxrig_win32_host_targets targets = {
        window,
        clipboard_window,
        clipboard_window,
    };
    wchar_t title[16];
    boxrig_cell_view cell;
    const uint8_t *output = NULL;
    size_t output_length;

    CHECK(window != NULL && clipboard_window != NULL);
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
    CHECK(boxrig_win32_host_adapter_create(
              &targets,
              consumer,
              config.clipboard_capacity,
              &policy,
              &adapter) == 0);
    CHECK(feed(terminal, title_stack, sizeof(title_stack) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(GetWindowTextW(window, title, 16) == 7);
    CHECK(wcscmp(title, L"initial") == 0);
    CHECK(feed(terminal, metadata_input, sizeof(metadata_input) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(GetWindowTextW(window, title, 16) == 2);
    CHECK(wcscmp(title, L"t\x00e9") == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.hyperlink_uri_length == 21);
    CHECK(memcmp(
              cell.hyperlink_uri,
              "https://example.test/",
              cell.hyperlink_uri_length) == 0);

    CHECK(feed(terminal, clipboard_write, sizeof(clipboard_write) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(GetClipboardOwner() == clipboard_window);
    CHECK(clipboard_equals(clipboard_window, L"Hello"));
    CHECK(feed(
        terminal,
        clipboard_query_bel,
        sizeof(clipboard_query_bel) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(denied_reply) - 1);
    CHECK(memcmp(output, denied_reply, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    boxrig_win32_host_adapter_destroy(adapter);
    adapter = NULL;
    policy.allow_clipboard_read = true;
    CHECK(boxrig_win32_host_adapter_create(
              &targets,
              consumer,
              config.clipboard_capacity,
              &policy,
              &adapter) == 0);
    CHECK(feed(
        terminal,
        clipboard_query_st,
        sizeof(clipboard_query_st) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(allowed_reply) - 1);
    CHECK(memcmp(output, allowed_reply, output_length) == 0);

    boxrig_win32_host_adapter_destroy(adapter);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    DestroyWindow(window);
    return true;
}

static bool test_graphics_external_policy(void)
{
    wchar_t saved[256];
    DWORD saved_length = GetEnvironmentVariableW(
        L"BOXRIG_ALLOW_GRAPHICS_EXTERNAL",
        saved,
        (DWORD)(sizeof(saved) / sizeof(saved[0])));
    bool had_saved = saved_length != 0 &&
                     saved_length < sizeof(saved) / sizeof(saved[0]);
    boxrig_win32_host_policy policy;
    bool valid;

    valid = SetEnvironmentVariableW(
                L"BOXRIG_ALLOW_GRAPHICS_EXTERNAL",
                NULL) != FALSE;
    policy = boxrig_win32_host_policy_default();
    valid = valid && !policy.allow_graphics_external;
    valid = valid &&
            SetEnvironmentVariableW(
                L"BOXRIG_ALLOW_GRAPHICS_EXTERNAL",
                L"true") != FALSE;
    policy = boxrig_win32_host_policy_default();
    valid = valid && !policy.allow_graphics_external;
    valid = valid &&
            SetEnvironmentVariableW(
                L"BOXRIG_ALLOW_GRAPHICS_EXTERNAL",
                L"1") != FALSE;
    policy = boxrig_win32_host_policy_default();
    valid = valid && !policy.allow_graphics_external;
    valid = valid &&
            SetEnvironmentVariableW(
                L"BOXRIG_ALLOW_GRAPHICS_EXTERNAL",
                had_saved ? saved : NULL) != FALSE;
    CHECK(valid);
    return true;
}

static bool create_graphics_adapter(
    HWND window,
    fake_graphics_provider *fake,
    bool allow_external,
    boxrig_terminal **terminal_out,
    boxrig_ui_model **model_out,
    boxrig_ui_host_item_consumer **consumer_out,
    boxrig_win32_host_adapter **adapter_out)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_win32_host_policy policy = boxrig_win32_host_policy_default();
    boxrig_win32_graphics_provider provider = graphics_provider(fake);

    *terminal_out = NULL;
    *model_out = NULL;
    *consumer_out = NULL;
    *adapter_out = NULL;
    config.rows = 1;
    config.columns = 1;
    config.graphics_decode_capacity = sizeof(fake->data);
    policy.allow_graphics_external = allow_external;
    if (boxrig_terminal_create(&config, terminal_out) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(*terminal_out, model_out) != BOXRIG_UI_OK ||
        boxrig_ui_host_item_consumer_create(*model_out, consumer_out) !=
            BOXRIG_UI_OK) {
        return false;
    }
    if (boxrig_win32_host_adapter_create_with_graphics_provider(
            &(boxrig_win32_host_targets){window, window, window},
            *consumer_out,
            config.clipboard_capacity,
            &policy,
            &provider,
            adapter_out) == -1) {
        boxrig_ui_host_item_consumer_destroy(*consumer_out);
        *consumer_out = NULL;
        boxrig_ui_model_destroy(*model_out);
        *model_out = NULL;
        boxrig_terminal_destroy(*terminal_out);
        *terminal_out = NULL;
        return false;
    }
    return true;
}

static bool test_graphics_file_and_temporary_transports(HWND window)
{
    static const uint8_t utf8_path[] =
        "C:\\Temp\\t\xc3\xa9.bin";
    static const uint8_t invalid_utf8[] = {0xff};
    static const uint8_t rgba[] = {1, 2, 3, 4};
    fake_graphics_provider fake;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_host_item_consumer *consumer = NULL;
    boxrig_win32_host_adapter *adapter = NULL;

    initialize_fake_graphics_provider(&fake);
    CHECK(create_graphics_adapter(
        window, &fake, true, &terminal, &model, &consumer, &adapter));
    CHECK(feed_external_graphics(
        terminal,
        601,
        'f',
        2,
        4,
        utf8_path,
        sizeof(utf8_path) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(terminal, "\x1b_Gi=601;OK\x1b\\"));
    CHECK(image_equals(terminal, 601, rgba, sizeof(rgba)));
    CHECK(wcscmp(fake.opened_name, L"C:\\Temp\\t\x00e9.bin") == 0);
    CHECK(fake.create_file_calls == 1);
    CHECK(fake.information_calls == 2);
    CHECK(fake.read_calls == 1);
    CHECK(fake.close_file_calls == 1);

    initialize_fake_graphics_provider(&fake);
    fake.file_type = FILE_TYPE_PIPE;
    CHECK(feed_external_graphics(
        terminal,
        602,
        'f',
        0,
        4,
        utf8_path,
        sizeof(utf8_path) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=602;EIO:external graphics data unavailable\x1b\\"));
    CHECK(fake.read_calls == 0 && fake.close_file_calls == 1);

    initialize_fake_graphics_provider(&fake);
    fake.force_end_of_file = true;
    CHECK(feed_external_graphics(
        terminal,
        603,
        'f',
        2,
        4,
        utf8_path,
        sizeof(utf8_path) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=603;EIO:external graphics data unavailable\x1b\\"));
    CHECK(fake.read_calls == 1 && fake.close_file_calls == 1);

    initialize_fake_graphics_provider(&fake);
    fake.changed_after_read = true;
    CHECK(feed_external_graphics(
        terminal,
        604,
        'f',
        2,
        4,
        utf8_path,
        sizeof(utf8_path) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=604;EIO:external graphics data unavailable\x1b\\"));
    CHECK(fake.information_calls == 2);

    initialize_fake_graphics_provider(&fake);
    fake.file_size = sizeof(fake.data) + 1;
    fake.information.nFileSizeLow = (DWORD)fake.file_size;
    CHECK(feed_external_graphics(
        terminal,
        605,
        'f',
        0,
        0,
        utf8_path,
        sizeof(utf8_path) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=605;EIO:external graphics data unavailable\x1b\\"));
    CHECK(fake.read_calls == 0);

    initialize_fake_graphics_provider(&fake);
    CHECK(feed_external_graphics(
        terminal,
        606,
        'f',
        0,
        4,
        invalid_utf8,
        sizeof(invalid_utf8)));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=606;EIO:external graphics data unavailable\x1b\\"));
    CHECK(fake.create_file_calls == 0);

    initialize_fake_graphics_provider(&fake);
    CHECK(feed_external_graphics(
        terminal,
        611,
        't',
        2,
        4,
        utf8_path,
        sizeof(utf8_path) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(terminal, "\x1b_Gi=611;OK\x1b\\"));
    CHECK(image_equals(terminal, 611, rgba, sizeof(rgba)));
    CHECK(fake.delete_calls == 1);
    CHECK(wcscmp(fake.deleted_path, fake.final_path) == 0);

    initialize_fake_graphics_provider(&fake);
    fake.force_end_of_file = true;
    CHECK(feed_external_graphics(
        terminal,
        612,
        't',
        2,
        4,
        utf8_path,
        sizeof(utf8_path) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=612;EIO:external graphics data unavailable\x1b\\"));
    CHECK(fake.delete_calls == 1);

    initialize_fake_graphics_provider(&fake);
    wcscpy(fake.final_path, L"\\\\?\\C:\\Temp\\ordinary-image.bin");
    CHECK(feed_external_graphics(
        terminal,
        613,
        't',
        2,
        4,
        utf8_path,
        sizeof(utf8_path) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=613;EACCES:external graphics data denied\x1b\\"));
    CHECK(fake.read_calls == 0 && fake.delete_calls == 0 &&
          fake.close_file_calls == 1);

    initialize_fake_graphics_provider(&fake);
    wcscpy(
        fake.final_path,
        L"\\\\?\\C:\\Temporary\\tty-graphics-protocol-image.bin");
    CHECK(feed_external_graphics(
        terminal,
        614,
        't',
        2,
        4,
        utf8_path,
        sizeof(utf8_path) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=614;EACCES:external graphics data denied\x1b\\"));
    CHECK(fake.read_calls == 0 && fake.delete_calls == 0);

    initialize_fake_graphics_provider(&fake);
    fake.delete_succeeds = false;
    CHECK(feed_external_graphics(
        terminal,
        615,
        't',
        2,
        4,
        utf8_path,
        sizeof(utf8_path) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=615;EIO:external graphics data unavailable\x1b\\"));
    CHECK(fake.read_calls == 1 && fake.delete_calls == 1 &&
          fake.close_file_calls == 1);

    boxrig_win32_host_adapter_destroy(adapter);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_graphics_named_mapping_and_denial(HWND window)
{
    static const uint8_t mapping_name[] =
        "Local\\boxrig-\xc3\xa9";
    static const uint8_t file_name[] = "C:\\Temp\\image.bin";
    static const uint8_t rgba[] = {1, 2, 3, 4};
    fake_graphics_provider fake;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_host_item_consumer *consumer = NULL;
    boxrig_win32_host_adapter *adapter = NULL;

    initialize_fake_graphics_provider(&fake);
    CHECK(create_graphics_adapter(
        window, &fake, true, &terminal, &model, &consumer, &adapter));
    CHECK(feed_external_graphics(
        terminal,
        621,
        's',
        2,
        0,
        mapping_name,
        sizeof(mapping_name) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(terminal, "\x1b_Gi=621;OK\x1b\\"));
    CHECK(image_equals(terminal, 621, rgba, sizeof(rgba)));
    CHECK(wcscmp(fake.mapping_name, L"Local\\boxrig-\x00e9") == 0);
    CHECK(fake.open_mapping_calls == 1 && fake.map_calls == 1 &&
          fake.query_calls == 1 && fake.unmap_calls == 1 &&
          fake.close_mapping_calls == 1);

    initialize_fake_graphics_provider(&fake);
    CHECK(feed_external_graphics(
        terminal,
        622,
        's',
        0,
        7,
        mapping_name,
        sizeof(mapping_name) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=622;EIO:external graphics data unavailable\x1b\\"));
    CHECK(fake.unmap_calls == 1 && fake.close_mapping_calls == 1);
    boxrig_win32_host_adapter_destroy(adapter);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);

    initialize_fake_graphics_provider(&fake);
    terminal = NULL;
    model = NULL;
    consumer = NULL;
    adapter = NULL;
    CHECK(create_graphics_adapter(
        window,
        &fake,
        false,
        &terminal,
        &model,
        &consumer,
        &adapter));
    CHECK(feed_external_graphics(
        terminal,
        623,
        'f',
        0,
        4,
        file_name,
        sizeof(file_name) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(output_equals(
        terminal,
        "\x1b_Gi=623;EACCES:external graphics data denied\x1b\\"));
    CHECK(fake.create_file_calls == 0 && fake.open_mapping_calls == 0);
    boxrig_win32_host_adapter_destroy(adapter);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_graphics_transports(void)
{
    HWND window = CreateWindowExW(
        0,
        L"STATIC",
        L"graphics",
        WS_OVERLAPPED,
        0,
        0,
        64,
        32,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    bool passed;

    CHECK(window != NULL);
    passed = test_graphics_external_policy() &&
             test_graphics_file_and_temporary_transports(window) &&
             test_graphics_named_mapping_and_denial(window);
    DestroyWindow(window);
    return passed;
}

typedef struct custom_notification_provider {
    boxrig_notification_dispatcher *dispatcher;
    const boxrig_notification_view *notification;
    uint64_t request_id;
    size_t submissions;
    size_t cancellations;
} custom_notification_provider;

static bool custom_notification_available(void *context)
{
    return context != NULL;
}

static int custom_notification_submit(
    void *context, boxrig_notification_dispatcher *dispatcher,
    uint64_t request_id, const boxrig_notification_view *notification)
{
    custom_notification_provider *provider = context;
    provider->dispatcher = dispatcher;
    provider->request_id = request_id;
    provider->notification = notification;
    provider->submissions++;
    return 0;
}

static void custom_notification_cancel(void *context, uint64_t request_id)
{
    custom_notification_provider *provider = context;
    if (provider->request_id == request_id) {
        provider->cancellations++;
    }
}

static bool test_custom_notification_provider(HWND window)
{
    custom_notification_provider custom = {0};
    boxrig_notification_provider provider = {
        custom_notification_available, custom_notification_submit,
        custom_notification_cancel, &custom
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_host_item_consumer *consumer = NULL;
    boxrig_win32_host_adapter *adapter = NULL;
    boxrig_win32_host_policy policy = boxrig_win32_host_policy_default();
    policy.notification.allow_notifications = true;
    policy.notification_provider = &provider;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_host_item_consumer_create(model, &consumer) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_host_adapter_create(
        &(boxrig_win32_host_targets){window, window, window}, consumer,
        config.clipboard_capacity, &policy, &adapter) == 0);
    static const uint8_t first[] = "\033]99;i=custom;custom title\033\\";
    CHECK(feed(terminal, first, sizeof(first) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(custom.submissions == 1);
    CHECK(custom.notification->title.length == 12);
    CHECK(memcmp(custom.notification->title.bytes, "custom title", 12) == 0);
    uint64_t id = custom.request_id;
    CHECK(boxrig_notification_dispatcher_complete(
        custom.dispatcher, id, BOXRIG_HOST_REQUEST_SUCCESS) == 0);
    CHECK(notification_request_is_completed(terminal, id));
    CHECK(boxrig_notification_dispatcher_complete(
        custom.dispatcher, id, BOXRIG_HOST_REQUEST_SUCCESS) == -1);
    CHECK(feed(terminal, first, sizeof(first) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(custom.submissions == 1);
    static const uint8_t pending[] = "\033]99;;cancel me\033\\";
    CHECK(feed(terminal, pending, sizeof(pending) - 1));
    CHECK(boxrig_win32_host_adapter_process_items(adapter) == 0);
    CHECK(custom.submissions == 2);
    id = custom.request_id;
    boxrig_win32_host_adapter_destroy(adapter);
    CHECK(custom.cancellations == 1);
    CHECK(notification_request_is_completed(terminal, id));
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_notifications(void)
{
    HWND window = CreateWindowExW(
        0,
        L"STATIC",
        L"notifications",
        WS_OVERLAPPED,
        0,
        0,
        64,
        32,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        NULL);
    bool passed;

    CHECK(window != NULL);
    passed = test_notification_policy_and_presentation(window) &&
             test_notification_provider_failures(window) &&
             test_custom_notification_provider(window);
    DestroyWindow(window);
    return passed;
}

int main(void)
{
    if (!test_host_adapter() || !test_graphics_transports() ||
        !test_notifications()) {
        return 1;
    }
    printf("PASS Win32 host adapter applies policy clipboard and notifications\n");
    return 0;
}
