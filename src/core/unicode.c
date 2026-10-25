/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "unicode.h"

#include "utf8proc.h"

#include <string.h>

#define BOXRIG_REPLACEMENT_CHARACTER ((int32_t)0xfffd)

static uint8_t expected_length(uint8_t byte)
{
    if (byte >= 0xc2 && byte <= 0xdf) {
        return 2;
    }
    if (byte >= 0xe0 && byte <= 0xef) {
        return 3;
    }
    if (byte >= 0xf0 && byte <= 0xf4) {
        return 4;
    }
    return 0;
}

void boxrig_unicode_decoder_initialize(boxrig_unicode_decoder *decoder)
{
    memset(decoder, 0, sizeof(*decoder));
}

boxrig_status boxrig_unicode_decoder_put(
    boxrig_unicode_decoder *decoder,
    uint8_t byte,
    boxrig_unicode_emit emit,
    void *context)
{
    if (decoder->length == 0) {
        uint8_t length;

        if (byte <= 0x7f) {
            return emit(context, (int32_t)byte);
        }
        length = expected_length(byte);
        if (length == 0) {
            return emit(context, BOXRIG_REPLACEMENT_CHARACTER);
        }
        decoder->bytes[0] = byte;
        decoder->length = 1;
        decoder->expected_length = length;
        return BOXRIG_STATUS_OK;
    }

    if (byte < 0x80 || byte > 0xbf) {
        boxrig_status status = emit(context, BOXRIG_REPLACEMENT_CHARACTER);

        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
        decoder->length = 0;
        decoder->expected_length = 0;
        return boxrig_unicode_decoder_put(decoder, byte, emit, context);
    }

    if ((uint8_t)(decoder->length + 1) < decoder->expected_length) {
        decoder->bytes[decoder->length] = byte;
        decoder->length++;
        return BOXRIG_STATUS_OK;
    }

    {
        uint8_t candidate[4];
        utf8proc_int32_t codepoint = -1;
        utf8proc_ssize_t result;
        boxrig_status status;

        memcpy(candidate, decoder->bytes, decoder->length);
        candidate[decoder->length] = byte;
        result = utf8proc_iterate(
            candidate,
            (utf8proc_ssize_t)decoder->expected_length,
            &codepoint);
        status = emit(
            context,
            result == (utf8proc_ssize_t)decoder->expected_length
                ? (int32_t)codepoint
                : BOXRIG_REPLACEMENT_CHARACTER);
        if (status == BOXRIG_STATUS_OK) {
            decoder->length = 0;
            decoder->expected_length = 0;
        }
        return status;
    }
}

boxrig_status boxrig_unicode_decoder_flush(
    boxrig_unicode_decoder *decoder,
    boxrig_unicode_emit emit,
    void *context)
{
    boxrig_status status;

    if (decoder->length == 0) {
        return BOXRIG_STATUS_OK;
    }
    status = emit(context, BOXRIG_REPLACEMENT_CHARACTER);
    if (status == BOXRIG_STATUS_OK) {
        decoder->length = 0;
        decoder->expected_length = 0;
    }
    return status;
}

size_t boxrig_unicode_encode(int32_t codepoint, uint8_t output[4])
{
    utf8proc_ssize_t length = utf8proc_encode_char(
        (utf8proc_int32_t)codepoint,
        output);

    return length > 0 ? (size_t)length : 0;
}

bool boxrig_unicode_grapheme_break(
    int32_t previous,
    int32_t current,
    int32_t *state)
{
    utf8proc_int32_t utf8proc_state = (utf8proc_int32_t)*state;
    bool result = utf8proc_grapheme_break_stateful(
                      (utf8proc_int32_t)previous,
                      (utf8proc_int32_t)current,
                      &utf8proc_state) != 0;

    *state = (int32_t)utf8proc_state;
    return result;
}

int boxrig_unicode_column_width(int32_t codepoint, bool ambiguous_is_wide)
{
    /* Kitty image placeholders encode exactly one grid cell, not a glyph. */
    if (codepoint == 0x10eeee) return 1;
    int width = utf8proc_charwidth((utf8proc_int32_t)codepoint);

    if (ambiguous_is_wide && width == 1 &&
        utf8proc_charwidth_ambiguous((utf8proc_int32_t)codepoint)) {
        return 2;
    }
    return width;
}

bool boxrig_unicode_is_regional_indicator(int32_t codepoint)
{
    const utf8proc_property_t *property = utf8proc_get_property(
        (utf8proc_int32_t)codepoint);

    return property->boundclass == UTF8PROC_BOUNDCLASS_REGIONAL_INDICATOR;
}

bool boxrig_unicode_text_is_ambiguous(const uint8_t *text, size_t text_length)
{
    bool ambiguous = false;
    for (size_t offset = 0; offset < text_length;) {
        utf8proc_int32_t codepoint;
        size_t available = text_length - offset;
        utf8proc_ssize_t decoded = utf8proc_iterate(text + offset,
            (utf8proc_ssize_t)(available > 4 ? 4 : available), &codepoint);
        if (decoded <= 0) return false;
        /* A font's missing-glyph advance must not resize an image cell. */
        if (codepoint == 0x10eeee) return false;
        /* Explicit emoji presentation and intrinsically wide bases retain
         * their Unicode width even if a combining mark is ambiguous. */
        int width = utf8proc_charwidth(codepoint);
        if (width == 2 || codepoint == 0xfe0f || codepoint == 0x20e3) return false;
        if (width == 1 && utf8proc_charwidth_ambiguous(codepoint)) ambiguous = true;
        offset += (size_t)decoded;
    }
    return ambiguous;
}

void boxrig_unicode_cluster_analyzer_initialize(
    boxrig_unicode_cluster_analyzer *analyzer)
{
    memset(analyzer, 0, sizeof(*analyzer));
}

bool boxrig_unicode_cluster_analyzer_accept(
    boxrig_unicode_cluster_analyzer *analyzer,
    int32_t codepoint,
    bool ambiguous_is_wide)
{
    int32_t next_grapheme_state = analyzer->grapheme_state;
    int width;

    if (analyzer->active && boxrig_unicode_grapheme_break(
                                analyzer->previous,
                                codepoint,
                                &next_grapheme_state)) {
        return false;
    }
    width = boxrig_unicode_column_width(codepoint, ambiguous_is_wide);
    /* Pictographic grapheme properties do not imply emoji presentation.
     * Keep the scalar width for text-default symbols; VS16 promotes their
     * cluster when emoji presentation is explicitly requested. */
    if (boxrig_unicode_is_regional_indicator(codepoint) ||
        (analyzer->active && (codepoint == 0xfe0f || codepoint == 0x20e3))) {
        width = 2;
    }
    if (width <= 0) {
        width = 1;
    }
    if (!analyzer->active || width > analyzer->column_width) {
        analyzer->column_width = (uint8_t)width;
    }
    analyzer->previous = codepoint;
    analyzer->grapheme_state = next_grapheme_state;
    analyzer->active = true;
    return true;
}

boxrig_status boxrig_unicode_measure_text_cluster(
    const uint8_t *text,
    size_t text_length,
    bool ambiguous_is_wide,
    boxrig_text_cluster_measurement *measurement_out)
{
    boxrig_unicode_cluster_analyzer analyzer;
    boxrig_text_cluster_measurement measurement = {0};
    size_t offset = 0;

    if (text == NULL || text_length == 0 || measurement_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    boxrig_unicode_cluster_analyzer_initialize(&analyzer);
    while (offset < text_length) {
        boxrig_unicode_cluster_analyzer candidate = analyzer;
        utf8proc_int32_t codepoint = -1;
        size_t available = text_length - offset;
        utf8proc_ssize_t decoded;

        if (available > 4) {
            available = 4;
        }
        decoded = utf8proc_iterate(
            text + offset,
            (utf8proc_ssize_t)available,
            &codepoint);
        if (decoded <= 0 || codepoint < 0) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        if (!boxrig_unicode_cluster_analyzer_accept(
                &candidate,
                (int32_t)codepoint,
                ambiguous_is_wide)) {
            measurement.text_length = offset;
            measurement.column_width = analyzer.column_width;
            measurement.split_by_limit = false;
            *measurement_out = measurement;
            return BOXRIG_STATUS_OK;
        }
        if ((size_t)decoded > BOXRIG_TEXT_CLUSTER_CAPACITY_MAX - offset) {
            measurement.text_length = offset;
            measurement.column_width = analyzer.column_width;
            measurement.split_by_limit = true;
            *measurement_out = measurement;
            return BOXRIG_STATUS_OK;
        }
        analyzer = candidate;
        offset += (size_t)decoded;
    }
    measurement.text_length = offset;
    measurement.column_width = analyzer.column_width;
    measurement.split_by_limit = false;
    *measurement_out = measurement;
    return BOXRIG_STATUS_OK;
}
