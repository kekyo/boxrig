/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

/* Verify image throughput without a machine-dependent time threshold. */
#include "graphics.h"
#include "terminal_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t scalar_calls;

boxrig_status __real_boxrig_kitty_graphics_put(boxrig_kitty_graphics_state *, uint8_t);

/** Counts scalar graphics dispatches while feeding a large direct image.
 * @param state Graphics parser receiving the byte.
 * @param byte Byte delivered to the parser.
 * @return Status returned by the real parser.
 */
boxrig_status __wrap_boxrig_kitty_graphics_put(boxrig_kitty_graphics_state *state, uint8_t byte)
{
    scalar_calls++;
    return __real_boxrig_kitty_graphics_put(state, byte);
}

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_set_cell_pixel_geometry(terminal, 7, 14) == BOXRIG_STATUS_OK);
    const char prefix[] = "\033_Ga=T,i=1,p=1,f=32,s=24,v=24,C=1,q=2;";
    char input[sizeof(prefix) + 24 * 24 * 4 / 3 * 4 + 2];
    size_t length = sizeof(prefix) - 1;
    memcpy(input, prefix, length);
    /* Opaque white RGBA has a padding-free Base64 payload at these dimensions. */
    memset(input + length, '/', 3072);
    length += 3072;
    memcpy(input + length, "\033\\", 2);
    length += 2;
    size_t consumed;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)input, length,
        &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == length);
    assert(boxrig_terminal_image_count(terminal) == 1);
    assert(boxrig_terminal_image_placement_count(terminal) == 1);
    boxrig_image_handle handle;
    boxrig_image_view image;
    assert(boxrig_terminal_image_handle_at(terminal, 0, &handle) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_get_image(terminal, handle, &image) == BOXRIG_STATUS_OK);
    assert(image.rgba_length == 24 * 24 * 4);
    for (size_t i = 0; i < image.rgba_length; i++) assert(image.rgba[i] == 255);
    printf("scalar_calls=%zu bytes=%zu\n", scalar_calls, length);
    assert(scalar_calls < 100);
    printf("scalar_decode_bytes=%zu\n", terminal->graphics_protocol.scalar_decode_byte_count);
    fflush(stdout);
    assert(terminal->graphics_protocol.scalar_decode_byte_count < 100);
    boxrig_terminal_destroy(terminal);
    const size_t chunks[] = {1, 2, 3, 4, 5, 7, 17, 4096};
    for (size_t chunk = 0; chunk < sizeof(chunks) / sizeof(chunks[0]); chunk++) {
        assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        assert(boxrig_terminal_set_cell_pixel_geometry(terminal, 7, 14) == BOXRIG_STATUS_OK);
        for (size_t offset = 0; offset < length;) {
            size_t count = length - offset;
            if (count > chunks[chunk]) count = chunks[chunk];
            assert(boxrig_terminal_feed(terminal, (const uint8_t *)input + offset,
                count, &consumed) == BOXRIG_STATUS_OK && consumed == count);
            offset += consumed;
        }
        assert(boxrig_terminal_image_count(terminal) == 1);
        assert(boxrig_terminal_image_placement_count(terminal) == 1);
        assert(boxrig_terminal_image_handle_at(terminal, 0, &handle) == BOXRIG_STATUS_OK);
        assert(boxrig_terminal_get_image(terminal, handle, &image) == BOXRIG_STATUS_OK);
        assert(image.rgba_length == 24 * 24 * 4);
        for (size_t i = 0; i < image.rgba_length; i++) assert(image.rgba[i] == 255);
        const uint8_t *reply;
        assert(boxrig_terminal_output_peek(terminal, &reply) == 0);
        boxrig_terminal_destroy(terminal);
    }
    puts("PASS direct image pixels and bounded scalar dispatch");
    return 0;
}
