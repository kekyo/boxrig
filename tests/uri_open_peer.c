/* boxrig - Under MIT. */
#define _POSIX_C_SOURCE 200809L
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *path = getenv("BOXRIG_TEST_URI_SOCKET");
    if (argc != 2 || path == NULL) return 64;
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address.sun_path)) return 65;
    memcpy(address.sun_path, path, strlen(path) + 1);
    int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0 || connect(descriptor, (struct sockaddr *)&address, sizeof(address)) == -1) return 66;
    size_t length = strlen(argv[1]) + 1;
    size_t written = 0;
    while (written < length) {
        ssize_t count = write(descriptor, argv[1] + written, length - written);
        if (count <= 0) return 67;
        written += (size_t)count;
    }
    char result;
    if (read(descriptor, &result, 1) != 1) return 68;
    close(descriptor);
    return result == '0' ? 0 : 4;
}
