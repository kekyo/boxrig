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

static void check_quartets(void)
{
    const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t encoded[64], expected[48], output[50];
    for (size_t i = 0; i < sizeof(encoded); i++) encoded[i] = (uint8_t)alphabet[i];
    for (unsigned int i = 0; i < 16; i++) {
        unsigned int a = 4 * i, b = a + 1, c = a + 2, d = a + 3;
        expected[3 * i] = (uint8_t)((a << 2) | (b >> 4));
        expected[3 * i + 1] = (uint8_t)((b << 4) | (c >> 2));
        expected[3 * i + 2] = (uint8_t)((c << 6) | d);
    }
    /* Limit the output at every quartet, including within a vector block. */
    for (size_t count = 0; count <= sizeof(encoded) / 4; count++) {
        memset(output, 0x5a, sizeof(output));
        assert(boxrig_graphics_decode_quartets(encoded, count, output + 1) == count);
        assert(memcmp(output + 1, expected, count * 3) == 0);
        assert(output[0] == 0x5a && output[1 + count * 3] == 0x5a);
    }
    const uint8_t invalid[] = {'=', ' ', 0x1b, 0x7f, 0x80, 0xff};
    for (size_t offset = 0; offset < sizeof(encoded); offset++) {
        for (size_t i = 0; i < sizeof(invalid); i++) {
            encoded[offset] = invalid[i];
            memset(output, 0x5a, sizeof(output));
            size_t count = boxrig_graphics_decode_quartets(encoded, sizeof(encoded) / 4, output + 1);
            assert(count == offset / 4 && memcmp(output + 1, expected, count * 3) == 0);
            assert(output[0] == 0x5a);
            for (size_t j = 1 + count * 3; j < sizeof(output); j++) assert(output[j] == 0x5a);
        }
        encoded[offset] = (uint8_t)alphabet[offset];
    }
}

/* The second APC must start at the same byte after every split, whether
 * the first image ends with valid padding or reports an invalid quartet. */
static void check_split_uploads(void)
{
    const char prefix[] = "\033_Ga=t,i=20,f=32,s=4,v=1;";
    const char payload[] = "AAECAwQFBgcICQoLDA0ODw==";
    const char next[] = "\033\\\033_Ga=t,i=21,f=32,s=1,v=1;AAECAw==\033\\";
    const size_t positions[] = {sizeof(payload), 0, 15, 16, 21, 22, 23};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
        uint8_t input[sizeof(prefix) + sizeof(payload) + sizeof(next)];
        const size_t start = sizeof(prefix) - 1;
        memcpy(input, prefix, start);
        memcpy(input + start, payload, sizeof(payload) - 1);
        memcpy(input + start + sizeof(payload) - 1, next, sizeof(next) - 1);
        const size_t length = start + sizeof(payload) - 1 + sizeof(next) - 1;
        bool valid = positions[i] >= sizeof(payload) - 1;
        if (!valid) input[start + positions[i]] = '!';
        const char *reply = valid ? "\033_Gi=20;OK\033\\\033_Gi=21;OK\033\\" :
            "\033_Gi=20;EINVAL:invalid command\033\\\033_Gi=21;OK\033\\";
        for (size_t split = start; split <= start + sizeof(payload) - 1; split++) {
            boxrig_terminal *terminal;
            assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
            size_t used;
            assert(boxrig_terminal_feed(terminal, input, split, &used) == BOXRIG_STATUS_OK && used == split);
            assert(boxrig_terminal_feed(terminal, input + split, length - split, &used) == BOXRIG_STATUS_OK &&
                used == length - split);
            assert(boxrig_terminal_image_count(terminal) == (valid ? 2u : 1u));
            for (size_t j = 0; j < boxrig_terminal_image_count(terminal); j++) {
                boxrig_image_handle handle;
                boxrig_image_view image;
                assert(boxrig_terminal_image_handle_at(terminal, j, &handle) == BOXRIG_STATUS_OK);
                assert(boxrig_terminal_get_image(terminal, handle, &image) == BOXRIG_STATUS_OK);
                assert(image.image_id == (valid && j == 0 ? 20u : 21u));
                assert(image.rgba_length == (image.image_id == 20 ? 16u : 4u));
                for (size_t k = 0; k < image.rgba_length; k++) assert(image.rgba[k] == k);
            }
            const uint8_t *actual;
            assert(boxrig_terminal_output_peek(terminal, &actual) == strlen(reply));
            assert(memcmp(actual, reply, strlen(reply)) == 0);
            boxrig_terminal_destroy(terminal);
        }
    }
}

int main(void)
{
    check_split_uploads();
    check_quartets();
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
