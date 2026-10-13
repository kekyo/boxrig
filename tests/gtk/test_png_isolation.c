/* boxrig - Under MIT. Private and system PNG codecs must coexist. */
#include <boxrig/terminal.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static const char png[] =
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAF0lEQVR4nGP4z8Dw"
        "n+E/QwMDmP7/vx4AP1cH+TNnSIYAAAAASUVORK5CYII=";
    static const uint8_t expected[] = {
        255, 0, 0, 255, 0, 255, 0, 128,
        0, 0, 255, 0, 255, 255, 255, 127
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    char command[256];
    int length = snprintf(command, sizeof(command), "\033_Ga=t,i=1,f=100;%s\033\\", png);
    assert(length > 0 && (size_t)length < sizeof(command));
    size_t consumed = 0;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)command, (size_t)length,
        &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == (size_t)length);
    boxrig_image_handle handle;
    boxrig_image_view image;
    assert(boxrig_terminal_image_handle_at(terminal, 0, &handle) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_get_image(terminal, handle, &image) == BOXRIG_STATUS_OK);
    assert(image.width == 2 && image.height == 2 && image.rgba_length == sizeof(expected));
    assert(memcmp(image.rgba, expected, sizeof(expected)) == 0);

    /* GTK theme icons use this shared codec in the same process as boxrig.
     * Mixing private and system libpng allocation/destruction crashes here. */
    gsize png_length;
    guchar *bytes = g_base64_decode(png, &png_length);
    GInputStream *stream = g_memory_input_stream_new_from_data(bytes, png_length, g_free);
    GError *error = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream(stream, NULL, &error);
    assert(error == NULL && pixbuf != NULL);
    assert(gdk_pixbuf_get_width(pixbuf) == 2 && gdk_pixbuf_get_height(pixbuf) == 2);
    assert(gdk_pixbuf_get_n_channels(pixbuf) == 4);
    const guchar *pixels = gdk_pixbuf_read_pixels(pixbuf);
    for (size_t row = 0; row < 2; row++)
        assert(memcmp(pixels + row * gdk_pixbuf_get_rowstride(pixbuf), expected + row * 8, 8) == 0);
    g_object_unref(pixbuf);
    assert(g_input_stream_close(stream, NULL, &error));
    assert(error == NULL);
    g_object_unref(stream);
    boxrig_terminal_destroy(terminal);
    puts("PASS private Kitty PNG and GTK system PNG decode independently");
    return 0;
}
