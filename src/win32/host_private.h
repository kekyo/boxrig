/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WIN32_HOST_ADAPTER_PRIVATE_H
#define BOXRIG_WIN32_HOST_ADAPTER_PRIVATE_H

#include "boxrig/win32/host.h"

#include <shellapi.h>

/** Replaceable Win32 operations used by notification policy tests. */
typedef struct boxrig_win32_notification_provider {
    /** Calls Shell_NotifyIconW with the prepared native structure. */
    BOOL (*shell_notify_icon)(
        void *context,
        DWORD message,
        PNOTIFYICONDATAW data);
    /** Returns whether the terminal window currently owns keyboard focus. */
    bool (*window_focused)(void *context, HWND window);
    /** Returns whether the terminal window is currently visible. */
    bool (*window_visible)(void *context, HWND window);
    /** Opaque value passed to every provider callback. */
    void *context;
} boxrig_win32_notification_provider;

/** Replaceable Win32 operations used by graphics transport tests. */
typedef struct boxrig_win32_graphics_provider {
    /** Opens one file path for bounded synchronous reading. */
    HANDLE (*create_file)(
        void *context,
        LPCWSTR name,
        DWORD desired_access,
        DWORD share_mode,
        DWORD creation_disposition,
        DWORD flags_and_attributes);
    /** Returns the native type of an opened file handle. */
    DWORD (*get_file_type)(void *context, HANDLE handle);
    /** Reads stable identity and regular-file metadata. */
    BOOL (*get_file_information)(
        void *context,
        HANDLE handle,
        BY_HANDLE_FILE_INFORMATION *information);
    /** Reads the current 64-bit file size. */
    BOOL (*get_file_size)(
        void *context,
        HANDLE handle,
        LARGE_INTEGER *size);
    /** Positions a file handle before a synchronous read. */
    BOOL (*set_file_pointer)(
        void *context,
        HANDLE handle,
        LARGE_INTEGER distance,
        DWORD move_method);
    /** Reads at most one DWORD-sized file chunk. */
    BOOL (*read_file)(
        void *context,
        HANDLE handle,
        void *bytes,
        DWORD capacity,
        DWORD *length_out);
    /** Resolves the normalized final path of an opened file. */
    DWORD (*get_final_path)(
        void *context,
        HANDLE handle,
        wchar_t *path,
        DWORD capacity,
        DWORD flags);
    /** Returns the current user temporary directory. */
    DWORD (*get_temporary_path)(
        void *context,
        DWORD capacity,
        wchar_t *path);
    /** Deletes one policy-approved temporary path. */
    BOOL (*delete_file)(void *context, LPCWSTR path);
    /** Opens one named file mapping for read-only access. */
    HANDLE (*open_file_mapping)(
        void *context,
        DWORD desired_access,
        LPCWSTR name);
    /** Maps the complete named file-mapping object read-only. */
    void *(*map_view)(
        void *context,
        HANDLE handle,
        DWORD desired_access);
    /** Reads the mapped view's committed native extent. */
    SIZE_T (*virtual_query)(
        void *context,
        const void *address,
        MEMORY_BASIC_INFORMATION *information,
        SIZE_T information_length);
    /** Releases one mapped view. */
    BOOL (*unmap_view)(void *context, const void *address);
    /** Closes one file or mapping handle. */
    BOOL (*close_handle)(void *context, HANDLE handle);
    /** Opaque value passed to every provider callback. */
    void *context;
} boxrig_win32_graphics_provider;

/**
 * Creates an adapter with replaceable notification operations.
 *
 * This boundary is private to deterministic host-adapter tests. Production
 * callers use boxrig_win32_host_adapter_create().
 */
int boxrig_win32_host_adapter_create_with_notification_provider(
    const boxrig_win32_host_targets *targets,
    boxrig_ui_host_item_consumer *consumer,
    size_t clipboard_capacity,
    const boxrig_win32_host_policy *policy,
    const boxrig_win32_notification_provider *notification_provider,
    boxrig_win32_host_adapter **adapter_out);

/**
 * Creates an adapter with replaceable graphics transport operations.
 *
 * This boundary is private to deterministic host-adapter tests. Production
 * callers use boxrig_win32_host_adapter_create().
 */
int boxrig_win32_host_adapter_create_with_graphics_provider(
    const boxrig_win32_host_targets *targets,
    boxrig_ui_host_item_consumer *consumer,
    size_t clipboard_capacity,
    const boxrig_win32_host_policy *policy,
    const boxrig_win32_graphics_provider *graphics_provider,
    boxrig_win32_host_adapter **adapter_out);

#endif
