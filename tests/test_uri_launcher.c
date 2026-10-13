/* boxrig - Under MIT. */
#define _POSIX_C_SOURCE 200809L
#include "uri_launcher.h"
#include "uri_launcher_glib.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", __func__, __LINE__, #x, errno); return 1; } } while (0)

static int run(boxrig_term_uri_launcher *launcher, int server, FILE *diagnostics,
    const char *original_path, const char *directory, GMainContext *host_context, GSource *source)
{
    const char *uri = "https://example.test/日本?q=\"quoted\"&s=$(nothing);literal";
    CHECK(boxrig_term_uri_launcher_open(launcher, uri, strlen(uri)) == 0);
    CHECK(boxrig_term_uri_launcher_pending(launcher) == 1);
    int peer = accept(server, NULL, NULL);
    CHECK(peer >= 0);
    char received[512];
    size_t used = 0;
    do {
        ssize_t count = read(peer, received + used, sizeof(received) - used);
        CHECK(count > 0);
        used += (size_t)count;
        CHECK(used < sizeof(received));
    } while (received[used - 1] != 0);
    CHECK(used == strlen(uri) + 1 && memcmp(received, uri, used) == 0);
    // The child is still waiting for permission to exit. Preparing a host wait
    // must remain possible while the request is incomplete.
    const struct pollfd *descriptors;
    size_t count;
    int timeout = -1;
    struct pollfd *copy;
    if (host_context != NULL) {
        (void)g_main_context_iteration(host_context, FALSE);
    } else {
    CHECK(boxrig_term_uri_launcher_prepare_poll(launcher, &descriptors, &count, &timeout) == 0);
    CHECK(count > 0);
    copy = malloc(count * sizeof(*copy));
    CHECK(copy != NULL);
    memcpy(copy, descriptors, count * sizeof(*copy));
    CHECK(poll(copy, count, 0) >= 0);
    CHECK(boxrig_term_uri_launcher_process_poll(launcher, copy, count) == 0);
    free(copy);
    }
    CHECK(boxrig_term_uri_launcher_pending(launcher) == 1);
    CHECK(write(peer, "0", 1) == 1);
    close(peer);
    while (boxrig_term_uri_launcher_pending(launcher) != 0) {
        if (host_context != NULL) {
            (void)g_main_context_iteration(host_context, TRUE);
            continue;
        }
        timeout = -1;
        CHECK(boxrig_term_uri_launcher_prepare_poll(launcher, &descriptors, &count, &timeout) == 0);
        copy = malloc(count * sizeof(*copy));
        CHECK(copy != NULL);
        memcpy(copy, descriptors, count * sizeof(*copy));
        CHECK(poll(copy, count, timeout) >= 0);
        CHECK(boxrig_term_uri_launcher_process_poll(launcher, copy, count) == 0);
        free(copy);
    }
    CHECK(ftell(diagnostics) == 0);
    CHECK(boxrig_term_uri_launcher_open(launcher, uri, strlen(uri)) == 0);
    peer = accept(server, NULL, NULL);
    CHECK(peer >= 0);
    used = 0;
    do {
        ssize_t amount = read(peer, received + used, sizeof(received) - used);
        CHECK(amount > 0);
        used += (size_t)amount;
        CHECK(used < sizeof(received));
    } while (received[used - 1] != 0);
    CHECK(used == strlen(uri) + 1 && memcmp(received, uri, used) == 0);
    CHECK(write(peer, "4", 1) == 1);
    close(peer);
    while (boxrig_term_uri_launcher_pending(launcher) != 0) {
        if (host_context != NULL) {
            (void)g_main_context_iteration(host_context, TRUE);
            continue;
        }
        timeout = -1;
        CHECK(boxrig_term_uri_launcher_prepare_poll(launcher, &descriptors, &count, &timeout) == 0);
        copy = malloc(count * sizeof(*copy));
        CHECK(copy != NULL);
        memcpy(copy, descriptors, count * sizeof(*copy));
        CHECK(poll(copy, count, timeout) >= 0);
        CHECK(boxrig_term_uri_launcher_process_poll(launcher, copy, count) == 0);
        free(copy);
    }
    CHECK(ftell(diagnostics) > 0);
    long first_error = ftell(diagnostics);
    CHECK(setenv("PATH", "/boxrig-test-no-handlers", 1) == 0);
    CHECK(boxrig_term_uri_launcher_open(launcher, uri, strlen(uri)) == -1);
    CHECK(boxrig_term_uri_launcher_pending(launcher) == 0 && ftell(diagnostics) > first_error);
    CHECK(setenv("PATH", directory, 1) == 0);
    CHECK(boxrig_term_uri_launcher_open(launcher, uri, strlen(uri)) == 0);
    peer = accept(server, NULL, NULL);
    CHECK(peer >= 0);
    used = 0;
    do {
        ssize_t amount = read(peer, received + used, sizeof(received) - used);
        CHECK(amount > 0);
        used += (size_t)amount;
        CHECK(used < sizeof(received));
    } while (received[used - 1] != 0);
    CHECK(used == strlen(uri) + 1 && memcmp(received, uri, used) == 0);
    // Teardown cancels only the observation. The child remains alive until the
    // endpoint receives this explicit acknowledgement after teardown returns.
    if (source != NULL) {
        g_source_destroy(source);
        g_source_unref(source);
    }
    boxrig_term_uri_launcher_destroy(launcher);
    CHECK(write(peer, "0", 1) == 1);
    close(peer);
    CHECK(setenv("PATH", original_path, 1) == 0);
    puts("PASS exact argv, asynchronous completion, failure recovery and pending teardown");
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 || (argc == 3 && strcmp(argv[2], "--glib") == 0));
    char directory[] = "/tmp/boxrig-uri-test-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char executable[512];
    CHECK(snprintf(executable, sizeof(executable), "%s/xdg-open", directory) > 0);
    CHECK(symlink(argv[1], executable) == 0);
    char socket_path[108];
    CHECK(snprintf(socket_path, sizeof(socket_path), "%s/peer", directory) > 0);
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(server >= 0);
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
    CHECK(bind(server, (struct sockaddr *)&address, sizeof(address)) == 0 && listen(server, 4) == 0);
    CHECK(setenv("BOXRIG_TEST_URI_SOCKET", socket_path, 1) == 0);
    char *original_path = strdup(getenv("PATH"));
    CHECK(original_path != NULL && setenv("PATH", directory, 1) == 0);
    FILE *diagnostics = tmpfile();
    CHECK(diagnostics != NULL);
    boxrig_term_uri_launcher *launcher = NULL;
    CHECK(boxrig_term_uri_launcher_create(diagnostics, &launcher) == 0);
    GMainContext *host_context = argc == 3 ? g_main_context_new() : NULL;
    GSource *source = host_context == NULL ? NULL : boxrig_term_uri_launcher_source_new(launcher);
    CHECK(host_context == NULL || (source != NULL && g_source_attach(source, host_context) != 0));
    int result = run(launcher, server, diagnostics, original_path, directory, host_context, source);
    if (host_context != NULL) g_main_context_unref(host_context);
    if (result != 0) boxrig_term_uri_launcher_destroy(launcher);
    fclose(diagnostics);
    setenv("PATH", original_path, 1);
    free(original_path);
    close(server);
    unlink(socket_path);
    unlink(executable);
    rmdir(directory);
    return result;
}
