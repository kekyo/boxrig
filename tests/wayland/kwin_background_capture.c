/* boxrig - Under MIT. Capture completed images from a private KWin session.
 * The public ScreenShot2 D-Bus contract returns QImage format identifiers;
 * no Qt headers or libraries are needed by this test helper.
 * https://invent.kde.org/plasma/kwin/-/blob/v6.7.4/src/plugins/screenshot/org.kde.KWin.ScreenShot2.xml
 */
#define _POSIX_C_SOURCE 200809L
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    assert(argc == 6);
    int x = atoi(argv[1]), y = atoi(argv[2]);
    unsigned int width = (unsigned int)atoi(argv[3]);
    unsigned int height = (unsigned int)atoi(argv[4]);
    unsigned int frames = (unsigned int)atoi(argv[5]);
    assert(width > 0 && width <= 2048 && height > 0 && height <= 2048);
    assert(frames > 0 && frames <= 90);
    alarm(30);
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (bus == NULL) {
        fprintf(stderr, "%s\n", error->message);
        g_error_free(error);
        return 1;
    }
    struct timespec start;
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    for (unsigned int frame = 0; frame < frames; frame++) {
        /* Cadence controls sampling, not the condition for test success.
         * Preserve every returned frame and record its actual capture time. */
        struct timespec deadline = start;
        deadline.tv_sec += frame / 30;
        deadline.tv_nsec += (long)(frame % 30) * 1000000000L / 30;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_nsec -= 1000000000L;
            deadline.tv_sec++;
        }
        int status;
        do {
            status = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        } while (status == EINTR);
        assert(status == 0);

        int ends[2];
        assert(pipe(ends) == 0);
        GUnixFDList *fds = g_unix_fd_list_new();
        int handle = g_unix_fd_list_append(fds, ends[1], &error);
        assert(handle >= 0);
        close(ends[1]);
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "hide-caller-windows", g_variant_new_boolean(FALSE));
        GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(bus,
            "org.kde.KWin.ScreenShot2", "/org/kde/KWin/ScreenShot2",
            "org.kde.KWin.ScreenShot2", "CaptureArea",
            g_variant_new("(iiuua{sv}h)", x, y, width, height, &options, handle),
            G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 30000, fds, NULL, NULL, &error);
        /* Closing our duplicate leaves the pipe writer owned only by KWin. */
        g_object_unref(fds);
        if (reply == NULL) {
            fprintf(stderr, "%s\n", error->message);
            g_error_free(error);
            close(ends[0]);
            g_object_unref(bus);
            return 1;
        }
        GVariant *info = g_variant_get_child_value(reply, 0);
        guint32 w = 0, h = 0, stride = 0, format = 0;
        assert(g_variant_lookup(info, "width", "u", &w) &&
            g_variant_lookup(info, "height", "u", &h) &&
            g_variant_lookup(info, "stride", "u", &stride) &&
            g_variant_lookup(info, "format", "u", &format));
        assert(w == width && h == height && stride >= w * 4 && stride <= w * 4 + 64);
        /* QImage RGB32/ARGB32/premultiplied ARGB32 are native 0xAARRGGBB;
         * RGBX8888/RGBA8888/premultiplied RGBA8888 have byte-ordered channels. */
        assert(format == 4 || format == 5 || format == 6 ||
            format == 16 || format == 17 || format == 18);
        size_t bytes = (size_t)stride * h, position = 0;
        uint8_t *data = malloc(bytes), *rgb = malloc((size_t)w * h * 3);
        assert(data != NULL && rgb != NULL);
        while (position < bytes) {
            ssize_t amount = read(ends[0], data + position, bytes - position);
            if (amount < 0 && errno == EINTR) continue;
            assert(amount > 0);
            position += (size_t)amount;
        }
        close(ends[0]);
        size_t out = 0;
        for (guint32 row = 0; row < h; row++) {
            for (guint32 column = 0; column < w; column++) {
                const uint8_t *p = data + row * stride + column * 4;
                if (format < 16) {
                    uint32_t pixel;
                    memcpy(&pixel, p, sizeof(pixel));
                    rgb[out++] = (uint8_t)(pixel >> 16);
                    rgb[out++] = (uint8_t)(pixel >> 8);
                    rgb[out++] = (uint8_t)pixel;
                } else {
                    rgb[out++] = p[0];
                    rgb[out++] = p[1];
                    rgb[out++] = p[2];
                }
            }
        }
        struct timespec now;
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        fprintf(stderr, "CAPTURE %u time=%lld.%09ld format=%u\n",
            frame, (long long)now.tv_sec, now.tv_nsec, format);
        assert(fwrite(rgb, 1, out, stdout) == out);
        free(rgb);
        free(data);
        g_variant_unref(info);
        g_variant_unref(reply);
    }
    g_object_unref(bus);
    alarm(0);
    return 0;
}
