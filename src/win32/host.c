/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "host_private.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef BOXRIG_WIN32_SHELL_NOTIFY_ICON
#define BOXRIG_WIN32_SHELL_NOTIFY_ICON Shell_NotifyIconW
#else
BOOL WINAPI BOXRIG_WIN32_SHELL_NOTIFY_ICON(
    DWORD message,
    PNOTIFYICONDATAW data);
#endif

enum {
    TITLE_STACK_CAPACITY = 10
};

struct boxrig_win32_host_adapter {
    HWND toplevel;
    HWND clipboard_owner;
    HWND view;
    boxrig_ui_host_item_consumer *consumer;
    size_t clipboard_capacity;
    boxrig_win32_host_policy policy;
    wchar_t *window_titles[TITLE_STACK_CAPACITY];
    size_t window_title_count;
    boxrig_notification_dispatcher *notification_dispatcher;
    boxrig_win32_notification_provider notification_provider;
    boxrig_win32_graphics_provider graphics_provider;
    bool notification_icon_added;
    UINT notification_icon_id;
};

static uint64_t monotonic_milliseconds(void *context)
{
    (void)context;
    return (uint64_t)GetTickCount64();
}

static wchar_t *utf8_to_utf16(
    const uint8_t *bytes,
    size_t length,
    bool reject_embedded_nul)
{
    wchar_t *text;
    int text_length;

    if (length > INT_MAX ||
        (reject_embedded_nul && length != 0 &&
         memchr(bytes, 0, length) != NULL)) {
        return NULL;
    }
    if (length == 0) {
        text = malloc(sizeof(*text));
        if (text != NULL) {
            text[0] = L'\0';
        }
        return text;
    }
    text_length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        (const char *)bytes,
        (int)length,
        NULL,
        0);
    if (text_length <= 0 ||
        (size_t)text_length > SIZE_MAX / sizeof(*text) - 1) {
        return NULL;
    }
    text = malloc(((size_t)text_length + 1) * sizeof(*text));
    if (text == NULL) {
        return NULL;
    }
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            (const char *)bytes,
            (int)length,
            text,
            text_length) != text_length) {
        free(text);
        return NULL;
    }
    text[text_length] = L'\0';
    return text;
}

static bool graphics_range(
    const boxrig_graphics_transport_view *transport,
    uint64_t resource_size,
    LARGE_INTEGER *offset_out,
    size_t *length_out)
{
    uint64_t available;
    uint64_t length;

    if (transport == NULL || transport->offset > resource_size ||
        transport->offset > INT64_MAX ||
        transport->size > UINT64_MAX - transport->offset) {
        return false;
    }
    available = resource_size - transport->offset;
    length = transport->size == 0 ? available : transport->size;
    if (length > available || length > transport->data_capacity ||
        length > SIZE_MAX) {
        return false;
    }
    offset_out->QuadPart = (LONGLONG)transport->offset;
    *length_out = (size_t)length;
    return true;
}

static bool same_file_state(
    const BY_HANDLE_FILE_INFORMATION *first,
    const BY_HANDLE_FILE_INFORMATION *last,
    const LARGE_INTEGER *first_size,
    const LARGE_INTEGER *last_size)
{
    return first->dwVolumeSerialNumber == last->dwVolumeSerialNumber &&
           first->nFileIndexHigh == last->nFileIndexHigh &&
           first->nFileIndexLow == last->nFileIndexLow &&
           first->nFileSizeHigh == last->nFileSizeHigh &&
           first->nFileSizeLow == last->nFileSizeLow &&
           CompareFileTime(
               &first->ftLastWriteTime,
               &last->ftLastWriteTime) == 0 &&
           first_size->QuadPart == last_size->QuadPart;
}

static wchar_t *final_path_create(
    const boxrig_win32_graphics_provider *provider,
    HANDLE file)
{
    DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    DWORD capacity = provider->get_final_path(
        provider->context,
        file,
        NULL,
        0,
        flags);
    wchar_t *path;
    DWORD length;

    if (capacity == 0 || capacity > 32768) {
        return NULL;
    }
    path = malloc((size_t)capacity * sizeof(*path));
    if (path == NULL) {
        return NULL;
    }
    length = provider->get_final_path(
        provider->context,
        file,
        path,
        capacity,
        flags);
    if (length == 0 || length >= capacity) {
        free(path);
        return NULL;
    }
    path[length] = L'\0';
    return path;
}

static wchar_t *temporary_path_create(
    const boxrig_win32_graphics_provider *provider)
{
    DWORD capacity = provider->get_temporary_path(
        provider->context,
        0,
        NULL);
    wchar_t *path;
    DWORD length;

    if (capacity == 0 || capacity > 32768) {
        return NULL;
    }
    path = malloc((size_t)capacity * sizeof(*path));
    if (path == NULL) {
        return NULL;
    }
    length = provider->get_temporary_path(
        provider->context,
        capacity,
        path);
    if (length == 0 || length >= capacity) {
        free(path);
        return NULL;
    }
    path[length] = L'\0';
    return path;
}

static wchar_t *normalized_win32_path_create(
    const wchar_t *path,
    bool directory)
{
    static const wchar_t extended_unc[] = L"\\\\?\\UNC\\";
    static const wchar_t extended[] = L"\\\\?\\";
    const wchar_t *source = path;
    bool unc = false;
    size_t source_length;
    size_t prefix_length = 0;
    size_t capacity;
    size_t offset = 0;
    wchar_t *normalized;
    size_t index;

    if (wcsncmp(
            path,
            extended_unc,
            sizeof(extended_unc) / sizeof(extended_unc[0]) - 1) == 0) {
        source += sizeof(extended_unc) / sizeof(extended_unc[0]) - 1;
        unc = true;
        prefix_length = 2;
    } else if (wcsncmp(
                   path,
                   extended,
                   sizeof(extended) / sizeof(extended[0]) - 1) == 0) {
        source += sizeof(extended) / sizeof(extended[0]) - 1;
    }
    source_length = wcslen(source);
    if (source_length > SIZE_MAX - prefix_length - 2) {
        return NULL;
    }
    capacity = source_length + prefix_length + 2;
    if (capacity > SIZE_MAX / sizeof(*normalized)) {
        return NULL;
    }
    normalized = malloc(capacity * sizeof(*normalized));
    if (normalized == NULL) {
        return NULL;
    }
    if (unc) {
        normalized[offset++] = L'\\';
        normalized[offset++] = L'\\';
    }
    for (index = 0; index < source_length; index++) {
        normalized[offset++] = source[index] == L'/' ? L'\\' : source[index];
    }
    if (directory && offset != 0 && normalized[offset - 1] != L'\\') {
        normalized[offset++] = L'\\';
    }
    normalized[offset] = L'\0';
    return normalized;
}

static bool temporary_path_allowed(
    const boxrig_win32_graphics_provider *provider,
    const wchar_t *final_path)
{
    wchar_t *temporary_path = temporary_path_create(provider);
    wchar_t *normalized_final = normalized_win32_path_create(
        final_path,
        false);
    wchar_t *normalized_temporary = temporary_path == NULL
                                        ? NULL
                                        : normalized_win32_path_create(
                                              temporary_path,
                                              true);
    bool allowed = false;

    if (normalized_final != NULL && normalized_temporary != NULL) {
        size_t temporary_length = wcslen(normalized_temporary);

        allowed = wcsstr(
                      normalized_final,
                      L"tty-graphics-protocol") != NULL &&
                  wcslen(normalized_final) > temporary_length &&
                  _wcsnicmp(
                      normalized_final,
                      normalized_temporary,
                      temporary_length) == 0;
    }
    free(normalized_temporary);
    free(normalized_final);
    free(temporary_path);
    return allowed;
}

static bool read_file_bytes(
    const boxrig_win32_graphics_provider *provider,
    HANDLE file,
    const boxrig_graphics_transport_view *transport,
    uint8_t **bytes_out,
    size_t *length_out)
{
    BY_HANDLE_FILE_INFORMATION initial_information;
    BY_HANDLE_FILE_INFORMATION final_information;
    LARGE_INTEGER initial_size;
    LARGE_INTEGER final_size;
    LARGE_INTEGER file_offset;
    size_t length;
    size_t offset = 0;
    uint8_t *bytes;

    if (provider->get_file_type(provider->context, file) != FILE_TYPE_DISK ||
        !provider->get_file_information(
            provider->context,
            file,
            &initial_information) ||
        initial_information.dwFileAttributes == 0 ||
        (initial_information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) != 0 ||
        !provider->get_file_size(provider->context, file, &initial_size) ||
        initial_size.QuadPart < 0 ||
        !graphics_range(
            transport,
            (uint64_t)initial_size.QuadPart,
            &file_offset,
            &length) ||
        !provider->set_file_pointer(
            provider->context,
            file,
            file_offset,
            FILE_BEGIN)) {
        return false;
    }
    bytes = malloc(length == 0 ? 1 : length);
    if (bytes == NULL) {
        return false;
    }
    while (offset < length) {
        size_t remaining = length - offset;
        DWORD chunk = remaining > UINT32_MAX
                          ? UINT32_MAX
                          : (DWORD)remaining;
        DWORD read_length = 0;

        if (!provider->read_file(
                provider->context,
                file,
                bytes + offset,
                chunk,
                &read_length) ||
            read_length == 0 || read_length > chunk) {
            free(bytes);
            return false;
        }
        offset += read_length;
    }
    if (!provider->get_file_information(
            provider->context,
            file,
            &final_information) ||
        !provider->get_file_size(provider->context, file, &final_size) ||
        !same_file_state(
            &initial_information,
            &final_information,
            &initial_size,
            &final_size)) {
        free(bytes);
        return false;
    }
    *bytes_out = bytes;
    *length_out = length;
    return true;
}

static boxrig_host_request_result read_graphics_file(
    const boxrig_win32_graphics_provider *provider,
    const wchar_t *name,
    const boxrig_graphics_transport_view *transport,
    uint8_t **bytes_out,
    size_t *length_out)
{
    bool temporary = transport->medium ==
                     BOXRIG_GRAPHICS_TRANSPORT_TEMPORARY_FILE;
    HANDLE file = provider->create_file(
        provider->context,
        name,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN);
    wchar_t *final_path = NULL;
    bool delete_after_read = false;
    boxrig_host_request_result result = BOXRIG_HOST_REQUEST_UNAVAILABLE;

    if (file == INVALID_HANDLE_VALUE || file == NULL) {
        return result;
    }
    if (temporary) {
        final_path = final_path_create(provider, file);
        if (final_path == NULL) {
            goto cleanup;
        }
        if (!temporary_path_allowed(provider, final_path)) {
            result = BOXRIG_HOST_REQUEST_DENIED;
            goto cleanup;
        }
        delete_after_read = true;
    }
    if (read_file_bytes(
            provider,
            file,
            transport,
            bytes_out,
            length_out)) {
        result = BOXRIG_HOST_REQUEST_SUCCESS;
    }

cleanup:
    if (delete_after_read &&
        !provider->delete_file(provider->context, final_path)) {
        result = BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    if (!provider->close_handle(provider->context, file)) {
        result = BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    if (result != BOXRIG_HOST_REQUEST_SUCCESS) {
        free(*bytes_out);
        *bytes_out = NULL;
        *length_out = 0;
    }
    free(final_path);
    return result;
}

static boxrig_host_request_result read_graphics_mapping(
    const boxrig_win32_graphics_provider *provider,
    const wchar_t *name,
    const boxrig_graphics_transport_view *transport,
    uint8_t **bytes_out,
    size_t *length_out)
{
    HANDLE mapping = provider->open_file_mapping(
        provider->context,
        FILE_MAP_READ,
        name);
    void *view = NULL;
    uint8_t *bytes = NULL;
    boxrig_host_request_result result = BOXRIG_HOST_REQUEST_UNAVAILABLE;

    if (mapping == NULL || mapping == INVALID_HANDLE_VALUE) {
        return result;
    }
    view = provider->map_view(
        provider->context,
        mapping,
        FILE_MAP_READ);
    if (view != NULL) {
        MEMORY_BASIC_INFORMATION information;
        LARGE_INTEGER offset;
        size_t length;

        memset(&information, 0, sizeof(information));
        if (provider->virtual_query(
                provider->context,
                view,
                &information,
                sizeof(information)) == sizeof(information) &&
            information.AllocationBase == view &&
            information.BaseAddress == view &&
            information.State == MEM_COMMIT &&
            information.Type == MEM_MAPPED &&
            (information.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0 &&
            graphics_range(
                transport,
                information.RegionSize,
                &offset,
                &length)) {
            bytes = malloc(length == 0 ? 1 : length);
            if (bytes != NULL) {
                memcpy(
                    bytes,
                    (const uint8_t *)view + (size_t)offset.QuadPart,
                    length);
                result = BOXRIG_HOST_REQUEST_SUCCESS;
                *length_out = length;
            }
        }
        if (!provider->unmap_view(provider->context, view)) {
            result = BOXRIG_HOST_REQUEST_UNAVAILABLE;
        }
    }
    if (!provider->close_handle(provider->context, mapping)) {
        result = BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    if (result != BOXRIG_HOST_REQUEST_SUCCESS) {
        free(bytes);
        bytes = NULL;
        *length_out = 0;
    }
    *bytes_out = bytes;
    return result;
}

static boxrig_host_request_result read_graphics_data(
    const boxrig_win32_host_adapter *adapter,
    const boxrig_graphics_transport_view *transport,
    uint8_t **bytes_out,
    size_t *length_out)
{
    wchar_t *name;
    boxrig_host_request_result result;

    *bytes_out = NULL;
    *length_out = 0;
    if (!adapter->policy.allow_graphics_external) {
        return BOXRIG_HOST_REQUEST_DENIED;
    }
    if (transport == NULL || transport->name_length == 0 ||
        transport->name_length > INT_MAX) {
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    name = utf8_to_utf16(
        transport->name,
        transport->name_length,
        true);
    if (name == NULL || wcslen(name) > 32767) {
        free(name);
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    if (transport->medium == BOXRIG_GRAPHICS_TRANSPORT_FILE &&
        !transport->delete_after_read) {
        result = read_graphics_file(
            &adapter->graphics_provider,
            name,
            transport,
            bytes_out,
            length_out);
    } else if (transport->medium ==
                   BOXRIG_GRAPHICS_TRANSPORT_TEMPORARY_FILE &&
               transport->delete_after_read) {
        result = read_graphics_file(
            &adapter->graphics_provider,
            name,
            transport,
            bytes_out,
            length_out);
    } else if (transport->medium ==
                   BOXRIG_GRAPHICS_TRANSPORT_SHARED_MEMORY &&
               !transport->delete_after_read) {
        result = read_graphics_mapping(
            &adapter->graphics_provider,
            name,
            transport,
            bytes_out,
            length_out);
    } else {
        result = BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    free(name);
    return result;
}

static bool utf16_high_surrogate(wchar_t value)
{
    return value >= (wchar_t)0xd800 && value <= (wchar_t)0xdbff;
}

static bool utf16_low_surrogate(wchar_t value)
{
    return value >= (wchar_t)0xdc00 && value <= (wchar_t)0xdfff;
}

static void copy_notification_text(
    wchar_t *destination,
    size_t capacity,
    const wchar_t *source)
{
    size_t length = wcslen(source);
    size_t retained;

    if (length < capacity) {
        memcpy(destination, source, (length + 1) * sizeof(*destination));
        return;
    }
    retained = capacity - 2;
    if (retained != 0 && retained < length &&
        utf16_high_surrogate(source[retained - 1]) &&
        utf16_low_surrogate(source[retained])) {
        retained--;
    }
    if (retained != 0) {
        memcpy(destination, source, retained * sizeof(*destination));
    }
    destination[retained] = (wchar_t)0x2026;
    destination[retained + 1] = L'\0';
}

static HICON notification_icon(HWND window)
{
    HICON icon = (HICON)SendMessageW(
        window,
        WM_GETICON,
        ICON_SMALL,
        0);

    if (icon == NULL) {
        icon = (HICON)GetClassLongPtrW(window, GCLP_HICONSM);
    }
    if (icon == NULL) {
        icon = (HICON)SendMessageW(window, WM_GETICON, ICON_BIG, 0);
    }
    if (icon == NULL) {
        icon = (HICON)GetClassLongPtrW(window, GCLP_HICON);
    }
    if (icon == NULL) {
        icon = LoadIconW(NULL, MAKEINTRESOURCEW(32512));
    }
    return icon;
}

static void initialize_notification_icon_data(
    boxrig_win32_host_adapter *adapter,
    NOTIFYICONDATAW *data)
{
    memset(data, 0, sizeof(*data));
    data->cbSize = sizeof(*data);
    data->hWnd = adapter->toplevel;
    data->uID = adapter->notification_icon_id;
}

static void delete_notification_icon(boxrig_win32_host_adapter *adapter)
{
    NOTIFYICONDATAW data;

    if (!adapter->notification_icon_added) {
        return;
    }
    initialize_notification_icon_data(adapter, &data);
    (void)adapter->notification_provider.shell_notify_icon(
        adapter->notification_provider.context,
        NIM_DELETE,
        &data);
    adapter->notification_icon_added = false;
}

static bool initialize_notification_icon(
    boxrig_win32_host_adapter *adapter)
{
    NOTIFYICONDATAW data;

    if (adapter->notification_icon_added) {
        return true;
    }
    initialize_notification_icon_data(adapter, &data);
    data.uFlags = NIF_ICON;
    data.hIcon = notification_icon(adapter->toplevel);
    if (data.hIcon == NULL ||
        !adapter->notification_provider.shell_notify_icon(
            adapter->notification_provider.context,
            NIM_ADD,
            &data)) {
        return false;
    }
    adapter->notification_icon_added = true;
    data.uVersion = NOTIFYICON_VERSION_4;
    if (!adapter->notification_provider.shell_notify_icon(
            adapter->notification_provider.context,
            NIM_SETVERSION,
            &data)) {
        delete_notification_icon(adapter);
        return false;
    }
    return true;
}

static boxrig_host_request_result display_notification(
    boxrig_win32_host_adapter *adapter,
    const boxrig_notification_view *notification)
{
    wchar_t *title;
    wchar_t *body;
    NOTIFYICONDATAW data;

    title = utf8_to_utf16(
        notification->title.bytes,
        notification->title.length,
        true);
    body = utf8_to_utf16(
        notification->body.bytes,
        notification->body.length,
        true);
    if (title == NULL || body == NULL) {
        free(body);
        free(title);
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    initialize_notification_icon_data(adapter, &data);
    data.uFlags = NIF_INFO;
    copy_notification_text(
        data.szInfoTitle,
        sizeof(data.szInfoTitle) / sizeof(data.szInfoTitle[0]),
        title);
    copy_notification_text(
        data.szInfo,
        sizeof(data.szInfo) / sizeof(data.szInfo[0]),
        body);
    data.dwInfoFlags =
        notification->urgency == BOXRIG_NOTIFICATION_URGENCY_CRITICAL
            ? NIIF_ERROR
            : NIIF_INFO;
    free(body);
    free(title);
    if (!adapter->notification_provider.shell_notify_icon(
            adapter->notification_provider.context,
            NIM_MODIFY,
            &data)) {
        delete_notification_icon(adapter);
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    return BOXRIG_HOST_REQUEST_SUCCESS;
}

static bool shell_notification_available(void *context)
{
    return initialize_notification_icon(context);
}

static int shell_notification_submit(
    void *context, boxrig_notification_dispatcher *dispatcher,
    uint64_t request_id, const boxrig_notification_view *notification)
{
    return boxrig_notification_dispatcher_complete(dispatcher, request_id,
        display_notification(context, notification));
}

static void shell_notification_cancel(void *context, uint64_t request_id)
{
    /* Shell submission completes inline, so there is no pending operation. */
    (void)context;
    (void)request_id;
}

static int apply_title(
    boxrig_win32_host_adapter *adapter,
    const boxrig_host_item *item)
{
    uint32_t operation = item->flags &
                         (BOXRIG_TITLE_STACK_PUSH | BOXRIG_TITLE_STACK_POP);
    wchar_t *title;

    if (!adapter->policy.allow_title_change ||
        (item->flags & BOXRIG_TITLE_WINDOW) == 0) {
        return 0;
    }
    if (operation == (BOXRIG_TITLE_STACK_PUSH | BOXRIG_TITLE_STACK_POP)) {
        return -1;
    }
    if (operation == BOXRIG_TITLE_STACK_PUSH) {
        int length = GetWindowTextLengthW(adapter->toplevel);

        if (length < 0 || (size_t)length > SIZE_MAX / sizeof(*title) - 1) {
            return -1;
        }
        title = malloc(((size_t)length + 1) * sizeof(*title));
        if (title == NULL ||
            GetWindowTextW(adapter->toplevel, title, length + 1) != length) {
            free(title);
            return -1;
        }
        if (adapter->window_title_count == TITLE_STACK_CAPACITY) {
            free(adapter->window_titles[0]);
            memmove(
                adapter->window_titles,
                adapter->window_titles + 1,
                (TITLE_STACK_CAPACITY - 1) *
                    sizeof(adapter->window_titles[0]));
            adapter->window_title_count--;
        }
        adapter->window_titles[adapter->window_title_count] = title;
        adapter->window_title_count++;
        return 0;
    }
    if (operation == BOXRIG_TITLE_STACK_POP) {
        if (adapter->window_title_count == 0) {
            return 0;
        }
        title = adapter->window_titles[adapter->window_title_count - 1];
        if (!SetWindowTextW(adapter->toplevel, title)) {
            return -1;
        }
        free(title);
        adapter->window_title_count--;
        return 0;
    }
    title = utf8_to_utf16(item->payload, item->payload_length, false);
    if (title == NULL) {
        return -1;
    }
    if (!SetWindowTextW(adapter->toplevel, title)) {
        free(title);
        return -1;
    }
    free(title);
    return 0;
}

static bool uses_system_clipboard(uint32_t flags)
{
    return (flags & (BOXRIG_CLIPBOARD_SELECTION_CLIPBOARD |
                     BOXRIG_CLIPBOARD_SELECTION_SELECT |
                     BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_0)) != 0;
}

static boxrig_host_request_result write_clipboard(
    boxrig_win32_host_adapter *adapter,
    const boxrig_host_item *item)
{
    wchar_t *text;
    size_t character_count;
    HGLOBAL allocation;
    wchar_t *destination;
    bool clipboard_open = false;
    bool transferred = false;

    if (!adapter->policy.allow_clipboard_write) {
        return BOXRIG_HOST_REQUEST_DENIED;
    }
    if (!uses_system_clipboard(item->flags)) {
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    text = utf8_to_utf16(item->payload, item->payload_length, true);
    if (text == NULL) {
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    character_count = wcslen(text) + 1;
    if (character_count > SIZE_MAX / sizeof(*text)) {
        free(text);
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    allocation = GlobalAlloc(
        GMEM_MOVEABLE,
        character_count * sizeof(*text));
    if (allocation == NULL) {
        free(text);
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    destination = GlobalLock(allocation);
    if (destination != NULL) {
        memcpy(destination, text, character_count * sizeof(*text));
        (void)GlobalUnlock(allocation);
        clipboard_open = OpenClipboard(adapter->clipboard_owner) != FALSE;
        if (clipboard_open && EmptyClipboard() &&
            SetClipboardData(CF_UNICODETEXT, allocation) != NULL) {
            transferred = true;
        }
    }
    if (clipboard_open) {
        (void)CloseClipboard();
    }
    if (!transferred) {
        (void)GlobalFree(allocation);
    }
    free(text);
    return transferred ? BOXRIG_HOST_REQUEST_SUCCESS
                       : BOXRIG_HOST_REQUEST_UNAVAILABLE;
}

static boxrig_host_request_result read_clipboard(
    boxrig_win32_host_adapter *adapter,
    uint32_t flags,
    uint8_t **payload_out,
    size_t *payload_length_out)
{
    HANDLE handle;
    const wchar_t *text;
    SIZE_T allocation_size;
    size_t maximum_characters;
    size_t character_count = 0;
    int utf8_length;
    uint8_t *payload = NULL;
    boxrig_host_request_result result = BOXRIG_HOST_REQUEST_UNAVAILABLE;

    *payload_out = NULL;
    *payload_length_out = 0;
    if (!adapter->policy.allow_clipboard_read) {
        return BOXRIG_HOST_REQUEST_DENIED;
    }
    if (!uses_system_clipboard(flags) ||
        !OpenClipboard(adapter->clipboard_owner)) {
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        goto cleanup;
    }
    handle = GetClipboardData(CF_UNICODETEXT);
    if (handle == NULL) {
        goto cleanup;
    }
    allocation_size = GlobalSize(handle);
    text = GlobalLock(handle);
    if (text == NULL) {
        goto cleanup;
    }
    maximum_characters = (size_t)allocation_size / sizeof(*text);
    while (character_count < maximum_characters &&
           text[character_count] != L'\0') {
        character_count++;
    }
    if (character_count == maximum_characters || character_count > INT_MAX) {
        (void)GlobalUnlock(handle);
        goto cleanup;
    }
    if (character_count == 0) {
        result = BOXRIG_HOST_REQUEST_SUCCESS;
        (void)GlobalUnlock(handle);
        goto cleanup;
    }
    utf8_length = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text,
        (int)character_count,
        NULL,
        0,
        NULL,
        NULL);
    if (utf8_length <= 0 ||
        (size_t)utf8_length > adapter->clipboard_capacity) {
        (void)GlobalUnlock(handle);
        goto cleanup;
    }
    payload = malloc((size_t)utf8_length);
    if (payload == NULL ||
        WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text,
            (int)character_count,
            (char *)payload,
            utf8_length,
            NULL,
            NULL) != utf8_length) {
        free(payload);
        payload = NULL;
        (void)GlobalUnlock(handle);
        goto cleanup;
    }
    (void)GlobalUnlock(handle);
    *payload_out = payload;
    *payload_length_out = (size_t)utf8_length;
    result = BOXRIG_HOST_REQUEST_SUCCESS;

cleanup:
    (void)CloseClipboard();
    return result;
}

static int consume_and_complete(
    boxrig_win32_host_adapter *adapter,
    uint64_t request_id,
    boxrig_host_request_result result,
    const uint8_t *payload,
    size_t payload_length)
{
    if (boxrig_ui_host_item_consumer_consume(adapter->consumer) !=
            BOXRIG_UI_OK ||
        boxrig_ui_host_item_consumer_complete(
            adapter->consumer,
            request_id,
            result,
            payload,
            payload_length) != BOXRIG_UI_OK) {
        return -1;
    }
    return 0;
}

boxrig_win32_host_policy boxrig_win32_host_policy_default(void)
{
    boxrig_win32_host_policy policy;

    policy.allow_title_change = true;
    policy.allow_clipboard_write = true;
    policy.allow_clipboard_read = false;
    policy.allow_graphics_external = false;
    policy.notification = boxrig_notification_policy_config_default(
        monotonic_milliseconds,
        NULL);
    policy.notification_provider = NULL;
    return policy;
}

static BOOL real_shell_notify_icon(
    void *context,
    DWORD message,
    PNOTIFYICONDATAW data)
{
    (void)context;
    return BOXRIG_WIN32_SHELL_NOTIFY_ICON(message, data);
}

static bool real_window_focused(void *context, HWND window)
{
    (void)context;
    return GetFocus() == window;
}

static bool real_window_visible(void *context, HWND window)
{
    (void)context;
    return IsWindowVisible(window) != FALSE;
}

static HANDLE real_create_file(
    void *context,
    LPCWSTR name,
    DWORD desired_access,
    DWORD share_mode,
    DWORD creation_disposition,
    DWORD flags_and_attributes)
{
    (void)context;
    return CreateFileW(
        name,
        desired_access,
        share_mode,
        NULL,
        creation_disposition,
        flags_and_attributes,
        NULL);
}

static DWORD real_get_file_type(void *context, HANDLE handle)
{
    (void)context;
    return GetFileType(handle);
}

static BOOL real_get_file_information(
    void *context,
    HANDLE handle,
    BY_HANDLE_FILE_INFORMATION *information)
{
    (void)context;
    return GetFileInformationByHandle(handle, information);
}

static BOOL real_get_file_size(
    void *context,
    HANDLE handle,
    LARGE_INTEGER *size)
{
    (void)context;
    return GetFileSizeEx(handle, size);
}

static BOOL real_set_file_pointer(
    void *context,
    HANDLE handle,
    LARGE_INTEGER distance,
    DWORD move_method)
{
    (void)context;
    return SetFilePointerEx(handle, distance, NULL, move_method);
}

static BOOL real_read_file(
    void *context,
    HANDLE handle,
    void *bytes,
    DWORD capacity,
    DWORD *length_out)
{
    (void)context;
    return ReadFile(handle, bytes, capacity, length_out, NULL);
}

static DWORD real_get_final_path(
    void *context,
    HANDLE handle,
    wchar_t *path,
    DWORD capacity,
    DWORD flags)
{
    (void)context;
    return GetFinalPathNameByHandleW(handle, path, capacity, flags);
}

static DWORD real_get_temporary_path(
    void *context,
    DWORD capacity,
    wchar_t *path)
{
    (void)context;
    return GetTempPathW(capacity, path);
}

static BOOL real_delete_file(void *context, LPCWSTR path)
{
    (void)context;
    return DeleteFileW(path);
}

static HANDLE real_open_file_mapping(
    void *context,
    DWORD desired_access,
    LPCWSTR name)
{
    (void)context;
    return OpenFileMappingW(desired_access, FALSE, name);
}

static void *real_map_view(
    void *context,
    HANDLE handle,
    DWORD desired_access)
{
    (void)context;
    return MapViewOfFile(handle, desired_access, 0, 0, 0);
}

static SIZE_T real_virtual_query(
    void *context,
    const void *address,
    MEMORY_BASIC_INFORMATION *information,
    SIZE_T information_length)
{
    (void)context;
    return VirtualQuery(address, information, information_length);
}

static BOOL real_unmap_view(void *context, const void *address)
{
    (void)context;
    return UnmapViewOfFile(address);
}

static BOOL real_close_handle(void *context, HANDLE handle)
{
    (void)context;
    return CloseHandle(handle);
}

static boxrig_win32_notification_provider real_notification_provider(void)
{
    boxrig_win32_notification_provider provider;

    provider.shell_notify_icon = real_shell_notify_icon;
    provider.window_focused = real_window_focused;
    provider.window_visible = real_window_visible;
    provider.context = NULL;
    return provider;
}

static boxrig_win32_graphics_provider real_graphics_provider(void)
{
    boxrig_win32_graphics_provider provider;

    provider.create_file = real_create_file;
    provider.get_file_type = real_get_file_type;
    provider.get_file_information = real_get_file_information;
    provider.get_file_size = real_get_file_size;
    provider.set_file_pointer = real_set_file_pointer;
    provider.read_file = real_read_file;
    provider.get_final_path = real_get_final_path;
    provider.get_temporary_path = real_get_temporary_path;
    provider.delete_file = real_delete_file;
    provider.open_file_mapping = real_open_file_mapping;
    provider.map_view = real_map_view;
    provider.virtual_query = real_virtual_query;
    provider.unmap_view = real_unmap_view;
    provider.close_handle = real_close_handle;
    provider.context = NULL;
    return provider;
}

static bool graphics_provider_valid(
    const boxrig_win32_graphics_provider *provider)
{
    return provider != NULL && provider->create_file != NULL &&
           provider->get_file_type != NULL &&
           provider->get_file_information != NULL &&
           provider->get_file_size != NULL &&
           provider->set_file_pointer != NULL &&
           provider->read_file != NULL &&
           provider->get_final_path != NULL &&
           provider->get_temporary_path != NULL &&
           provider->delete_file != NULL &&
           provider->open_file_mapping != NULL &&
           provider->map_view != NULL && provider->virtual_query != NULL &&
           provider->unmap_view != NULL && provider->close_handle != NULL;
}

static int create_with_providers(
    const boxrig_win32_host_targets *targets,
    boxrig_ui_host_item_consumer *consumer,
    size_t clipboard_capacity,
    const boxrig_win32_host_policy *policy,
    const boxrig_win32_notification_provider *notification_provider,
    const boxrig_win32_graphics_provider *graphics_provider,
    boxrig_win32_host_adapter **adapter_out)
{
    boxrig_win32_host_adapter *adapter;

    if (targets == NULL || targets->toplevel == NULL ||
        targets->clipboard_owner == NULL || targets->view == NULL ||
        boxrig_ui_host_item_consumer_terminal(consumer) == NULL ||
        policy == NULL ||
        notification_provider == NULL ||
        notification_provider->shell_notify_icon == NULL ||
        notification_provider->window_focused == NULL ||
        notification_provider->window_visible == NULL ||
        !graphics_provider_valid(graphics_provider) || adapter_out == NULL) {
        return -1;
    }
    *adapter_out = NULL;
    adapter = calloc(1, sizeof(*adapter));
    if (adapter == NULL) {
        return -1;
    }
    adapter->toplevel = targets->toplevel;
    adapter->clipboard_owner = targets->clipboard_owner;
    adapter->view = targets->view;
    adapter->consumer = consumer;
    adapter->clipboard_capacity = clipboard_capacity;
    adapter->policy = *policy;
    adapter->notification_provider = *notification_provider;
    adapter->graphics_provider = *graphics_provider;
    adapter->notification_icon_id = 1;
    boxrig_notification_provider provider = {
        shell_notification_available, shell_notification_submit,
        shell_notification_cancel, adapter
    };
    if (boxrig_notification_dispatcher_create(
            consumer, &policy->notification,
            policy->notification_provider == NULL
                ? &provider : policy->notification_provider,
            &adapter->notification_dispatcher) == -1) {
        free(adapter);
        return -1;
    }
    *adapter_out = adapter;
    return 0;
}

int boxrig_win32_host_adapter_create(
    const boxrig_win32_host_targets *targets,
    boxrig_ui_host_item_consumer *consumer,
    size_t clipboard_capacity,
    const boxrig_win32_host_policy *policy,
    boxrig_win32_host_adapter **adapter_out)
{
    boxrig_win32_notification_provider notification_provider =
        real_notification_provider();
    boxrig_win32_graphics_provider graphics_provider =
        real_graphics_provider();

    return create_with_providers(
        targets,
        consumer,
        clipboard_capacity,
        policy,
        &notification_provider,
        &graphics_provider,
        adapter_out);
}

int boxrig_win32_host_adapter_create_with_notification_provider(
    const boxrig_win32_host_targets *targets,
    boxrig_ui_host_item_consumer *consumer,
    size_t clipboard_capacity,
    const boxrig_win32_host_policy *policy,
    const boxrig_win32_notification_provider *notification_provider,
    boxrig_win32_host_adapter **adapter_out)
{
    boxrig_win32_graphics_provider graphics_provider =
        real_graphics_provider();

    return create_with_providers(
        targets,
        consumer,
        clipboard_capacity,
        policy,
        notification_provider,
        &graphics_provider,
        adapter_out);
}

int boxrig_win32_host_adapter_create_with_graphics_provider(
    const boxrig_win32_host_targets *targets,
    boxrig_ui_host_item_consumer *consumer,
    size_t clipboard_capacity,
    const boxrig_win32_host_policy *policy,
    const boxrig_win32_graphics_provider *graphics_provider,
    boxrig_win32_host_adapter **adapter_out)
{
    boxrig_win32_notification_provider notification_provider =
        real_notification_provider();

    return create_with_providers(
        targets,
        consumer,
        clipboard_capacity,
        policy,
        &notification_provider,
        graphics_provider,
        adapter_out);
}

int boxrig_win32_host_adapter_process_items(boxrig_win32_host_adapter *adapter)
{
    boxrig_host_item item;

    if (adapter == NULL) {
        return -1;
    }
    bool focused = adapter->notification_provider.window_focused(
        adapter->notification_provider.context, adapter->view);
    bool visible = adapter->notification_provider.window_visible(
        adapter->notification_provider.context, adapter->view);
    boxrig_notification_dispatcher_set_window_state(
        adapter->notification_dispatcher, focused, visible);
    while (boxrig_ui_host_item_consumer_peek(adapter->consumer, &item)) {
        if (item.type == BOXRIG_HOST_ITEM_BELL) {
            (void)MessageBeep(MB_OK);
        } else if (item.type == BOXRIG_HOST_ITEM_TITLE) {
            if (apply_title(adapter, &item) == -1) {
                return -1;
            }
        } else if (item.type == BOXRIG_HOST_ITEM_WORKING_DIRECTORY) {
            /* The untrusted URI remains metadata and causes no host action. */
        } else if (item.type == BOXRIG_HOST_ITEM_CLIPBOARD_WRITE) {
            boxrig_host_request_result result = write_clipboard(adapter, &item);

            if (consume_and_complete(
                    adapter,
                    item.request_id,
                    result,
                    NULL,
                    0) == -1) {
                return -1;
            }
            continue;
        } else if (item.type == BOXRIG_HOST_ITEM_CLIPBOARD_READ) {
            uint8_t *payload = NULL;
            size_t payload_length = 0;
            boxrig_host_request_result result = read_clipboard(
                adapter,
                item.flags,
                &payload,
                &payload_length);
            int completion = consume_and_complete(
                adapter,
                item.request_id,
                result,
                payload,
                payload_length);

            free(payload);
            if (completion == -1) {
                return -1;
            }
            continue;
        } else if (item.type == BOXRIG_HOST_ITEM_NOTIFICATION) {
            if (boxrig_notification_dispatcher_process(
                    adapter->notification_dispatcher, focused, visible) == -1) {
                return -1;
            }
            continue;
        } else if (item.type == BOXRIG_HOST_ITEM_GRAPHICS_DATA) {
            uint8_t *bytes = NULL;
            size_t length = 0;
            boxrig_host_request_result result = read_graphics_data(
                adapter,
                item.graphics_transport,
                &bytes,
                &length);
            int completion = consume_and_complete(
                adapter,
                item.request_id,
                result,
                result == BOXRIG_HOST_REQUEST_SUCCESS ? bytes : NULL,
                result == BOXRIG_HOST_REQUEST_SUCCESS ? length : 0);

            free(bytes);
            if (completion == -1) {
                return -1;
            }
            continue;
        } else {
            return -1;
        }
        if (boxrig_ui_host_item_consumer_consume(adapter->consumer) !=
            BOXRIG_UI_OK) {
            return -1;
        }
    }
    return 0;
}

void boxrig_win32_host_adapter_destroy(boxrig_win32_host_adapter *adapter)
{
    size_t index;

    if (adapter == NULL) {
        return;
    }
    for (index = 0; index < adapter->window_title_count; index++) {
        free(adapter->window_titles[index]);
    }
    boxrig_notification_dispatcher_destroy(adapter->notification_dispatcher);
    delete_notification_icon(adapter);
    free(adapter);
}
