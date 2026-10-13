/* Direct resource bounds and ownership checks for the shared POSIX reader. */
#include "boxrig/posix.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    const uint8_t data[] = {99, 88, 1, 2, 3, 255};
    char path[] = "/tmp/boxrig-transport-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor != -1);
    assert(write(descriptor, data, sizeof(data)) == (ssize_t)sizeof(data));
    assert(close(descriptor) == 0);
    boxrig_graphics_transport_view transport = {0};
    transport.medium = BOXRIG_GRAPHICS_TRANSPORT_FILE;
    transport.name = (const uint8_t *)path;
    transport.name_length = strlen(path);
    transport.offset = 2;
    transport.data_capacity = 4;
    uint8_t *bytes = NULL;
    size_t length = 0;
    assert(boxrig_posix_read_graphics_transport(&transport, false, &bytes, &length) == BOXRIG_HOST_REQUEST_DENIED);
    assert(bytes == NULL && length == 0 && access(path, F_OK) == 0);
    assert(boxrig_posix_read_graphics_transport(&transport, true, &bytes, &length) == BOXRIG_HOST_REQUEST_SUCCESS);
    assert(length == 4 && memcmp(bytes, data + 2, 4) == 0);
    free(bytes);
    transport.data_capacity = 3;
    assert(boxrig_posix_read_graphics_transport(&transport, true, &bytes, &length) == BOXRIG_HOST_REQUEST_UNAVAILABLE);
    assert(bytes == NULL && length == 0);
    transport.data_capacity = 4;
    transport.offset = UINT64_MAX;
    transport.size = 1;
    assert(boxrig_posix_read_graphics_transport(&transport, true, &bytes, &length) == BOXRIG_HOST_REQUEST_UNAVAILABLE);
    assert(bytes == NULL && length == 0);
    transport.offset = 0;
    transport.size = 7;
    assert(boxrig_posix_read_graphics_transport(&transport, true, &bytes, &length) == BOXRIG_HOST_REQUEST_UNAVAILABLE);
    assert(bytes == NULL && length == 0 && access(path, F_OK) == 0);
    assert(unlink(path) == 0);
    assert(mkfifo(path, 0600) == 0);
    transport.size = 0;
    assert(boxrig_posix_read_graphics_transport(&transport, true, &bytes, &length) == BOXRIG_HOST_REQUEST_UNAVAILABLE);
    assert(bytes == NULL && length == 0);
    assert(unlink(path) == 0);
    assert(boxrig_posix_read_graphics_transport(NULL, true, &bytes, &length) == BOXRIG_HOST_REQUEST_UNAVAILABLE);
    assert(bytes == NULL && length == 0);
    assert(boxrig_posix_read_graphics_transport(&transport, true, NULL, &length) == BOXRIG_HOST_REQUEST_UNAVAILABLE);
    assert(boxrig_posix_read_graphics_transport(&transport, true, &bytes, NULL) == BOXRIG_HOST_REQUEST_UNAVAILABLE);
    puts("PASS POSIX graphics permission, offset, capacity, invalid ranges and nonregular resources");
    return 0;
}
