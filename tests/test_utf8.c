/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "utf8.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool expect_valid(
    const uint8_t *text,
    size_t text_length,
    uint32_t expected_codepoint,
    size_t expected_text_length)
{
    uint32_t codepoint = UINT32_MAX;
    size_t consumed = SIZE_MAX;

    CHECK(boxrig_ui_utf8_decode_scalar(
        text,
        text_length,
        &codepoint,
        &consumed));
    CHECK(codepoint == expected_codepoint);
    CHECK(consumed == expected_text_length);
    return true;
}

static bool expect_invalid(const uint8_t *text, size_t text_length)
{
    uint32_t codepoint = 0x12345678u;
    size_t consumed = 123u;

    CHECK(!boxrig_ui_utf8_decode_scalar(
        text,
        text_length,
        &codepoint,
        &consumed));
    CHECK(codepoint == 0x12345678u);
    CHECK(consumed == 123u);
    return true;
}

static bool test_decodes_rfc_3629_boundaries(void)
{
    static const uint8_t ascii[] = {0x41u, 0x42u};
    static const uint8_t two_byte_minimum[] = {0xc2u, 0x80u};
    static const uint8_t three_byte_minimum[] = {0xe0u, 0xa0u, 0x80u};
    static const uint8_t before_surrogates[] = {0xedu, 0x9fu, 0xbfu};
    static const uint8_t four_byte_minimum[] = {
        0xf0u,
        0x90u,
        0x80u,
        0x80u,
    };
    static const uint8_t unicode_maximum[] = {
        0xf4u,
        0x8fu,
        0xbfu,
        0xbfu,
    };

    CHECK(expect_valid(ascii, sizeof(ascii), 0x41u, 1u));
    CHECK(expect_valid(
        two_byte_minimum,
        sizeof(two_byte_minimum),
        0x80u,
        2u));
    CHECK(expect_valid(
        three_byte_minimum,
        sizeof(three_byte_minimum),
        0x800u,
        3u));
    CHECK(expect_valid(
        before_surrogates,
        sizeof(before_surrogates),
        0xd7ffu,
        3u));
    CHECK(expect_valid(
        four_byte_minimum,
        sizeof(four_byte_minimum),
        0x10000u,
        4u));
    CHECK(expect_valid(
        unicode_maximum,
        sizeof(unicode_maximum),
        0x10ffffu,
        4u));
    return true;
}

static bool test_rejects_non_scalar_and_incomplete_encodings(void)
{
    static const uint8_t valid_ascii[] = {0x41u};
    static const uint8_t stray_continuation[] = {0x80u};
    static const uint8_t overlong_two_byte[] = {0xc0u, 0x80u};
    static const uint8_t overlong_three_byte[] = {0xe0u, 0x9fu, 0xbfu};
    static const uint8_t surrogate[] = {0xedu, 0xa0u, 0x80u};
    static const uint8_t above_unicode_maximum[] = {
        0xf4u,
        0x90u,
        0x80u,
        0x80u,
    };
    static const uint8_t bad_continuation[] = {0xe2u, 0x28u, 0xa1u};
    static const uint8_t truncated_two_byte[] = {0xc2u};
    static const uint8_t truncated_three_byte[] = {0xe2u, 0x82u};
    static const uint8_t truncated_four_byte[] = {0xf0u, 0x90u, 0x80u};

    CHECK(expect_invalid(NULL, 0));
    CHECK(expect_invalid(stray_continuation, sizeof(stray_continuation)));
    CHECK(expect_invalid(overlong_two_byte, sizeof(overlong_two_byte)));
    CHECK(expect_invalid(overlong_three_byte, sizeof(overlong_three_byte)));
    CHECK(expect_invalid(surrogate, sizeof(surrogate)));
    CHECK(expect_invalid(
        above_unicode_maximum,
        sizeof(above_unicode_maximum)));
    CHECK(expect_invalid(bad_continuation, sizeof(bad_continuation)));
    CHECK(expect_invalid(truncated_two_byte, sizeof(truncated_two_byte)));
    CHECK(expect_invalid(truncated_three_byte, sizeof(truncated_three_byte)));
    CHECK(expect_invalid(truncated_four_byte, sizeof(truncated_four_byte)));
    CHECK(!boxrig_ui_utf8_decode_scalar(
        valid_ascii,
        sizeof(valid_ascii),
        NULL,
        &(size_t){0}));
    CHECK(!boxrig_ui_utf8_decode_scalar(
        valid_ascii,
        sizeof(valid_ascii),
        &(uint32_t){0},
        NULL));
    return true;
}

int main(void)
{
    const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"strict decoder accepts RFC 3629 boundaries",
         test_decodes_rfc_3629_boundaries},
        {"strict decoder rejects invalid scalar encodings",
         test_rejects_non_scalar_and_incomplete_encodings},
    };
    size_t index;

    for (index = 0; index < sizeof(tests) / sizeof(tests[0]); index++) {
        if (!tests[index].run()) {
            fprintf(stderr, "FAILED %s\n", tests[index].name);
            return 1;
        }
        printf("PASS %s\n", tests[index].name);
    }
    return 0;
}
