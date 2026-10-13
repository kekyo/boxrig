#include "search_transform_prototype.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require_true(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static search_transform_prototype_text create_text(
    search_transform_prototype_mode mode,
    const uint8_t *bytes,
    size_t byte_length,
    const size_t *boundaries,
    size_t boundary_count)
{
    search_transform_prototype_text text = {0};
    search_transform_prototype_status status =
        search_transform_prototype_create(
            mode,
            bytes,
            byte_length,
            boundaries,
            boundary_count,
            SEARCH_TRANSFORM_PROTOTYPE_OUTPUT_HARD_LIMIT,
            &text);

    require_true(status == SEARCH_TRANSFORM_PROTOTYPE_OK,
                 "prototype transform succeeds");
    return text;
}

static void test_case_folding_and_offset_mapping(void)
{
    static const uint8_t text_bytes[] = "Stra\xC3\x9F" "e";
    static const size_t text_boundaries[] = {0u, 1u, 2u, 3u, 4u, 6u, 7u};
    static const uint8_t pattern_bytes[] = "STRASSE";
    static const size_t pattern_boundaries[] = {
        0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u
    };
    static const uint8_t sharp_s_bytes[] = "\xC3\x9F";
    static const size_t sharp_s_boundaries[] = {0u, 2u};
    static const uint8_t ss_bytes[] = "ss";
    static const size_t ss_boundaries[] = {0u, 1u, 2u};
    static const uint8_t s_bytes[] = "s";
    static const size_t s_boundaries[] = {0u, 1u};
    search_transform_prototype_text text = create_text(
        SEARCH_TRANSFORM_PROTOTYPE_CASE_FOLD,
        text_bytes,
        sizeof(text_bytes) - 1u,
        text_boundaries,
        sizeof(text_boundaries) / sizeof(text_boundaries[0]));
    search_transform_prototype_text pattern = create_text(
        SEARCH_TRANSFORM_PROTOTYPE_CASE_FOLD,
        pattern_bytes,
        sizeof(pattern_bytes) - 1u,
        pattern_boundaries,
        sizeof(pattern_boundaries) / sizeof(pattern_boundaries[0]));
    search_transform_prototype_text sharp_s = create_text(
        SEARCH_TRANSFORM_PROTOTYPE_CASE_FOLD,
        sharp_s_bytes,
        sizeof(sharp_s_bytes) - 1u,
        sharp_s_boundaries,
        sizeof(sharp_s_boundaries) / sizeof(sharp_s_boundaries[0]));
    search_transform_prototype_text ss = create_text(
        SEARCH_TRANSFORM_PROTOTYPE_CASE_FOLD,
        ss_bytes,
        sizeof(ss_bytes) - 1u,
        ss_boundaries,
        sizeof(ss_boundaries) / sizeof(ss_boundaries[0]));
    search_transform_prototype_text s = create_text(
        SEARCH_TRANSFORM_PROTOTYPE_CASE_FOLD,
        s_bytes,
        sizeof(s_bytes) - 1u,
        s_boundaries,
        sizeof(s_boundaries) / sizeof(s_boundaries[0]));
    size_t source_begin = SIZE_MAX;
    size_t source_end = SIZE_MAX;

    require_true(text.byte_length == 7u &&
                     memcmp(text.bytes, "strasse", 7u) == 0,
                 "default case folding expands sharp s");
    require_true(search_transform_prototype_find(
                     &text, &pattern, &source_begin, &source_end) &&
                     source_begin == 0u && source_end == 7u,
                 "case-folded full match maps to original bytes");
    require_true(search_transform_prototype_find(
                     &sharp_s, &ss, &source_begin, &source_end) &&
                     source_begin == 0u && source_end == 2u,
                 "expanded sharp s maps both bytes to one source grapheme");
    require_true(!search_transform_prototype_find(
                     &sharp_s, &s, &source_begin, &source_end),
                 "match cannot end inside a source grapheme expansion");

    search_transform_prototype_destroy(&s);
    search_transform_prototype_destroy(&ss);
    search_transform_prototype_destroy(&sharp_s);
    search_transform_prototype_destroy(&pattern);
    search_transform_prototype_destroy(&text);
}

static void test_normalization_semantics(void)
{
    static const uint8_t decomposed_bytes[] = "Cafe\xCC\x81";
    static const size_t decomposed_boundaries[] = {0u, 1u, 2u, 3u, 6u};
    static const uint8_t composed_bytes[] = "Caf\xC3\xA9";
    static const size_t composed_boundaries[] = {0u, 1u, 2u, 3u, 5u};
    static const uint8_t circled_one_bytes[] = "\xE2\x91\xA0";
    static const size_t circled_one_boundaries[] = {0u, 3u};
    static const uint8_t one_bytes[] = "1";
    static const size_t one_boundaries[] = {0u, 1u};
    search_transform_prototype_text decomposed = create_text(
        SEARCH_TRANSFORM_PROTOTYPE_NFC,
        decomposed_bytes,
        sizeof(decomposed_bytes) - 1u,
        decomposed_boundaries,
        sizeof(decomposed_boundaries) / sizeof(decomposed_boundaries[0]));
    search_transform_prototype_text composed = create_text(
        SEARCH_TRANSFORM_PROTOTYPE_NFC,
        composed_bytes,
        sizeof(composed_bytes) - 1u,
        composed_boundaries,
        sizeof(composed_boundaries) / sizeof(composed_boundaries[0]));
    search_transform_prototype_text circled_one = create_text(
        SEARCH_TRANSFORM_PROTOTYPE_NFC,
        circled_one_bytes,
        sizeof(circled_one_bytes) - 1u,
        circled_one_boundaries,
        sizeof(circled_one_boundaries) / sizeof(circled_one_boundaries[0]));
    search_transform_prototype_text one = create_text(
        SEARCH_TRANSFORM_PROTOTYPE_NFC,
        one_bytes,
        sizeof(one_bytes) - 1u,
        one_boundaries,
        sizeof(one_boundaries) / sizeof(one_boundaries[0]));
    size_t source_begin = SIZE_MAX;
    size_t source_end = SIZE_MAX;

    require_true(search_transform_prototype_find(
                     &decomposed, &composed, &source_begin, &source_end) &&
                     source_begin == 0u && source_end == 6u,
                 "NFC canonical match maps decomposed source bytes");
    require_true(!search_transform_prototype_find(
                     &circled_one, &one, &source_begin, &source_end),
                 "NFC does not apply compatibility mappings");

    search_transform_prototype_destroy(&one);
    search_transform_prototype_destroy(&circled_one);
    search_transform_prototype_destroy(&composed);
    search_transform_prototype_destroy(&decomposed);
}

static void test_locale_and_combined_semantics(void)
{
    static const uint8_t dotted_i_bytes[] = "\xC4\xB0";
    static const size_t dotted_i_boundaries[] = {0u, 2u};
    static const uint8_t ascii_i_bytes[] = "i";
    static const size_t ascii_i_boundaries[] = {0u, 1u};
    static const uint8_t decomposed_upper_bytes[] = "E\xCC\x81";
    static const size_t decomposed_upper_boundaries[] = {0u, 3u};
    static const uint8_t composed_lower_bytes[] = "\xC3\xA9";
    static const size_t composed_lower_boundaries[] = {0u, 2u};
    search_transform_prototype_text dotted_i = create_text(
        SEARCH_TRANSFORM_PROTOTYPE_CASE_FOLD,
        dotted_i_bytes,
        sizeof(dotted_i_bytes) - 1u,
        dotted_i_boundaries,
        sizeof(dotted_i_boundaries) / sizeof(dotted_i_boundaries[0]));
    search_transform_prototype_text ascii_i = create_text(
        SEARCH_TRANSFORM_PROTOTYPE_CASE_FOLD,
        ascii_i_bytes,
        sizeof(ascii_i_bytes) - 1u,
        ascii_i_boundaries,
        sizeof(ascii_i_boundaries) / sizeof(ascii_i_boundaries[0]));
    search_transform_prototype_text decomposed_upper = create_text(
        SEARCH_TRANSFORM_PROTOTYPE_CASE_FOLD_NFC,
        decomposed_upper_bytes,
        sizeof(decomposed_upper_bytes) - 1u,
        decomposed_upper_boundaries,
        sizeof(decomposed_upper_boundaries) /
            sizeof(decomposed_upper_boundaries[0]));
    search_transform_prototype_text composed_lower = create_text(
        SEARCH_TRANSFORM_PROTOTYPE_CASE_FOLD_NFC,
        composed_lower_bytes,
        sizeof(composed_lower_bytes) - 1u,
        composed_lower_boundaries,
        sizeof(composed_lower_boundaries) /
            sizeof(composed_lower_boundaries[0]));
    size_t source_begin = SIZE_MAX;
    size_t source_end = SIZE_MAX;

    require_true(!search_transform_prototype_find(
                     &dotted_i, &ascii_i, &source_begin, &source_end),
                 "default case folding does not apply Turkish tailoring");
    require_true(search_transform_prototype_find(
                     &decomposed_upper,
                     &composed_lower,
                     &source_begin,
                     &source_end) &&
                     source_begin == 0u && source_end == 3u,
                 "combined case folding and NFC are canonical caseless");

    search_transform_prototype_destroy(&composed_lower);
    search_transform_prototype_destroy(&decomposed_upper);
    search_transform_prototype_destroy(&ascii_i);
    search_transform_prototype_destroy(&dotted_i);
}

static void test_limits_are_transactional(void)
{
    static const uint8_t sharp_s_bytes[] = "\xC3\x9F";
    static const size_t sharp_s_boundaries[] = {0u, 2u};
    search_transform_prototype_text output = {0};
    search_transform_prototype_status status =
        search_transform_prototype_create(
            SEARCH_TRANSFORM_PROTOTYPE_CASE_FOLD,
            sharp_s_bytes,
            sizeof(sharp_s_bytes) - 1u,
            sharp_s_boundaries,
            sizeof(sharp_s_boundaries) / sizeof(sharp_s_boundaries[0]),
            1u,
            &output);

    require_true(status == SEARCH_TRANSFORM_PROTOTYPE_LIMIT,
                 "expanded output respects strict byte limit");
    require_true(output.bytes == NULL && output.byte_length == 0u &&
                     output.source_offsets == NULL &&
                     output.transformed_offsets == NULL &&
                     output.boundary_count == 0u,
                 "limit failure leaves no partial transformed object");
}

static void test_scalar_expansion_measurements(void)
{
    static const search_transform_prototype_mode modes[] = {
        SEARCH_TRANSFORM_PROTOTYPE_CASE_FOLD,
        SEARCH_TRANSFORM_PROTOTYPE_NFC,
        SEARCH_TRANSFORM_PROTOTYPE_CASE_FOLD_NFC
    };
    static const char *const names[] = {"case-fold", "NFC", "case-fold+NFC"};
    size_t index;

    for (index = 0u; index < sizeof(modes) / sizeof(modes[0]); ++index) {
        search_transform_prototype_metrics metrics = {0};
        search_transform_prototype_status status =
            search_transform_prototype_measure_scalars(modes[index], &metrics);

        require_true(status == SEARCH_TRANSFORM_PROTOTYPE_OK,
                     "Unicode scalar expansion scan succeeds");
        require_true(metrics.scalar_count == 1112064u,
                     "every Unicode scalar is inspected");
        require_true(metrics.input_bytes <= 4u && metrics.output_bytes <= 16u,
                     "fixed utf8proc scalar output fits conservative bound");
        printf("MEASURE search transform %s: U+%04X %zu->%zu bytes\n",
               names[index],
               (unsigned int)metrics.largest_scalar,
               metrics.input_bytes,
               metrics.output_bytes);
    }
}

int main(void)
{
    test_case_folding_and_offset_mapping();
    test_normalization_semantics();
    test_locale_and_combined_semantics();
    test_limits_are_transactional();
    test_scalar_expansion_measurements();
    puts("PASS search transform prototype semantics and bounds");
    return 0;
}
