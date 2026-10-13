/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/posix.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static bool valid_utf8_native_name(const uint8_t *bytes, size_t length)
{
    size_t offset = 0;

    if (bytes == NULL || length == 0) {
        return false;
    }
    while (offset < length) {
        uint8_t first = bytes[offset];
        size_t sequence_length;

        if (first == 0) {
            return false;
        }
        if (first <= 0x7f) {
            offset++;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            sequence_length = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            sequence_length = 3;
        } else if (first >= 0xf0 && first <= 0xf4) {
            sequence_length = 4;
        } else {
            return false;
        }
        if (sequence_length > length - offset) {
            return false;
        }
        if ((bytes[offset + 1] & 0xc0) != 0x80 ||
            (sequence_length >= 3 &&
             (bytes[offset + 2] & 0xc0) != 0x80) ||
            (sequence_length == 4 &&
             (bytes[offset + 3] & 0xc0) != 0x80)) {
            return false;
        }
        if ((first == 0xe0 && bytes[offset + 1] < 0xa0) ||
            (first == 0xed && bytes[offset + 1] >= 0xa0) ||
            (first == 0xf0 && bytes[offset + 1] < 0x90) ||
            (first == 0xf4 && bytes[offset + 1] >= 0x90)) {
            return false;
        }
        offset += sequence_length;
    }
    return true;
}

static char *graphics_native_name_create(
    const boxrig_graphics_transport_view *transport)
{
    char *name;

    if (transport == NULL ||
        !valid_utf8_native_name(transport->name, transport->name_length) ||
        transport->name_length == SIZE_MAX) {
        return NULL;
    }
    name = malloc(transport->name_length + 1);
    if (name == NULL) {
        return NULL;
    }
    memcpy(name, transport->name, transport->name_length);
    name[transport->name_length] = '\0';
    return name;
}

static bool graphics_range(
    const boxrig_graphics_transport_view *transport,
    off_t resource_size,
    off_t *offset_out,
    size_t *length_out)
{
    uint64_t available;
    uint64_t length;
    off_t native_offset;

    if (transport == NULL || resource_size < 0 ||
        transport->offset > (uint64_t)resource_size ||
        transport->size > UINT64_MAX - transport->offset) {
        return false;
    }
    native_offset = (off_t)transport->offset;
    if (native_offset < 0 || (uint64_t)native_offset != transport->offset) {
        return false;
    }
    available = (uint64_t)resource_size - transport->offset;
    length = transport->size == 0 ? available : transport->size;
    if (length > available || length > transport->data_capacity ||
        length > SIZE_MAX) {
        return false;
    }
    *offset_out = native_offset;
    *length_out = (size_t)length;
    return true;
}

static bool same_file_state(const struct stat *first, const struct stat *last)
{
    return first->st_dev == last->st_dev && first->st_ino == last->st_ino &&
           first->st_size == last->st_size &&
           first->st_mtim.tv_sec == last->st_mtim.tv_sec &&
           first->st_mtim.tv_nsec == last->st_mtim.tv_nsec &&
           first->st_ctim.tv_sec == last->st_ctim.tv_sec &&
           first->st_ctim.tv_nsec == last->st_ctim.tv_nsec;
}

static bool read_regular_descriptor(
    int descriptor,
    const boxrig_graphics_transport_view *transport,
    uint8_t **bytes_out,
    size_t *length_out)
{
    struct stat initial_state;
    struct stat final_state;
    off_t read_offset;
    size_t length;
    size_t offset = 0;
    uint8_t *bytes;

    if (fstat(descriptor, &initial_state) == -1 ||
        !S_ISREG(initial_state.st_mode) ||
        !graphics_range(transport, initial_state.st_size, &read_offset, &length)) {
        return false;
    }
    bytes = malloc(length == 0 ? 1 : length);
    if (bytes == NULL) {
        return false;
    }
    while (offset < length) {
        size_t remaining = length - offset;
        size_t chunk = remaining > (size_t)SSIZE_MAX
                           ? (size_t)SSIZE_MAX
                           : remaining;
        ssize_t count = pread(
            descriptor,
            bytes + offset,
            chunk,
            read_offset + (off_t)offset);

        if (count == -1 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            free(bytes);
            return false;
        }
        offset += (size_t)count;
    }
    if (fstat(descriptor, &final_state) == -1 ||
        !same_file_state(&initial_state, &final_state)) {
        free(bytes);
        return false;
    }
    *bytes_out = bytes;
    *length_out = length;
    return true;
}

static boxrig_host_request_result read_regular_graphics_file(
    const char *path,
    const boxrig_graphics_transport_view *transport,
    bool delete_after_read,
    uint8_t **bytes_out,
    size_t *length_out)
{
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    bool success;

    if (descriptor == -1) {
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    success = read_regular_descriptor(
        descriptor,
        transport,
        bytes_out,
        length_out);
    if (delete_after_read && unlink(path) == -1) {
        success = false;
    }
    if (close(descriptor) == -1) {
        success = false;
    }
    if (!success) {
        free(*bytes_out);
        *bytes_out = NULL;
        *length_out = 0;
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    return BOXRIG_HOST_REQUEST_SUCCESS;
}

static bool path_is_within_directory(
    const char *path,
    const char *directory)
{
    size_t length = strlen(directory);

    if (length == 1 && directory[0] == '/') {
        return path[0] == '/';
    }
    return strncmp(path, directory, length) == 0 &&
           (path[length] == '/' || path[length] == '\0');
}

static bool temporary_graphics_path_allowed(const char *canonical_path)
{
    static const char *default_directories[] = {
        "/tmp",
        "/var/tmp",
        "/dev/shm"
    };
    const char *temporary_directory = getenv("TMPDIR");
    size_t index;

    if (strstr(canonical_path, "tty-graphics-protocol") == NULL) {
        return false;
    }
    if (temporary_directory != NULL && temporary_directory[0] != '\0') {
        char *canonical_directory = realpath(temporary_directory, NULL);
        bool allowed = canonical_directory != NULL &&
                       path_is_within_directory(
                           canonical_path,
                           canonical_directory);

        free(canonical_directory);
        if (allowed) {
            return true;
        }
    }
    for (index = 0;
         index < sizeof(default_directories) / sizeof(default_directories[0]);
         index++) {
        char *canonical_directory = realpath(default_directories[index], NULL);
        bool allowed = canonical_directory != NULL &&
                       path_is_within_directory(
                           canonical_path,
                           canonical_directory);

        free(canonical_directory);
        if (allowed) {
            return true;
        }
    }
    return false;
}

static boxrig_host_request_result read_shared_graphics_memory(
    const char *name,
    const boxrig_graphics_transport_view *transport,
    uint8_t **bytes_out,
    size_t *length_out)
{
    int descriptor = shm_open(name, O_RDONLY | O_CLOEXEC, 0);
    struct stat initial_state;
    struct stat final_state;
    off_t offset;
    size_t length;
    uint8_t *bytes = NULL;
    void *mapping = MAP_FAILED;
    size_t mapping_length = 0;
    bool success = true;

    if (descriptor == -1) {
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    if (shm_unlink(name) == -1 || fstat(descriptor, &initial_state) == -1 ||
        !graphics_range(transport, initial_state.st_size, &offset, &length)) {
        success = false;
    }
    if (success) {
        bytes = malloc(length == 0 ? 1 : length);
        if (bytes == NULL) {
            success = false;
        }
    }
    if (success && length != 0) {
        long page_size = sysconf(_SC_PAGESIZE);
        uint64_t page_offset;
        uint64_t delta;

        if (page_size <= 0) {
            success = false;
        } else {
            page_offset = transport->offset /
                          (uint64_t)page_size * (uint64_t)page_size;
            delta = transport->offset - page_offset;
            if (delta > SIZE_MAX || length > SIZE_MAX - (size_t)delta) {
                success = false;
            } else {
                off_t native_page_offset = (off_t)page_offset;

                mapping_length = (size_t)delta + length;
                if (native_page_offset < 0 ||
                    (uint64_t)native_page_offset != page_offset) {
                    success = false;
                } else {
                    mapping = mmap(
                        NULL,
                        mapping_length,
                        PROT_READ,
                        MAP_SHARED,
                        descriptor,
                        native_page_offset);
                    if (mapping == MAP_FAILED) {
                        success = false;
                    } else {
                        memcpy(bytes, (const uint8_t *)mapping + delta, length);
                    }
                }
            }
        }
    }
    if (mapping != MAP_FAILED && munmap(mapping, mapping_length) == -1) {
        success = false;
    }
    if (success &&
        (fstat(descriptor, &final_state) == -1 ||
         !same_file_state(&initial_state, &final_state))) {
        success = false;
    }
    if (close(descriptor) == -1) {
        success = false;
    }
    if (!success) {
        free(bytes);
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    *bytes_out = bytes;
    *length_out = length;
    return BOXRIG_HOST_REQUEST_SUCCESS;
}

boxrig_host_request_result boxrig_posix_read_graphics_transport(
    const boxrig_graphics_transport_view *transport,
    bool allowed,
    uint8_t **bytes_out,
    size_t *length_out)
{
    char *name;
    boxrig_host_request_result result;

    if (bytes_out != NULL) {
        *bytes_out = NULL;
    }
    if (length_out != NULL) {
        *length_out = 0;
    }
    if (bytes_out == NULL || length_out == NULL) {
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    if (!allowed) {
        return BOXRIG_HOST_REQUEST_DENIED;
    }
    name = graphics_native_name_create(transport);
    if (name == NULL) {
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    if (transport->medium == BOXRIG_GRAPHICS_TRANSPORT_FILE) {
        result = read_regular_graphics_file(
            name,
            transport,
            false,
            bytes_out,
            length_out);
    } else if (transport->medium ==
               BOXRIG_GRAPHICS_TRANSPORT_TEMPORARY_FILE) {
        char *canonical_path = realpath(name, NULL);

        if (canonical_path == NULL) {
            result = BOXRIG_HOST_REQUEST_UNAVAILABLE;
        } else if (!temporary_graphics_path_allowed(canonical_path)) {
            result = BOXRIG_HOST_REQUEST_DENIED;
        } else {
            result = read_regular_graphics_file(
                canonical_path,
                transport,
                true,
                bytes_out,
                length_out);
        }
        free(canonical_path);
    } else if (transport->medium ==
               BOXRIG_GRAPHICS_TRANSPORT_SHARED_MEMORY) {
        result = read_shared_graphics_memory(
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
