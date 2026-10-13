/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UNICODE_H
#define BOXRIG_UNICODE_H

#include "boxrig/terminal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct boxrig_unicode_decoder {
    uint8_t bytes[4];
    uint8_t length;
    uint8_t expected_length;
} boxrig_unicode_decoder;

typedef struct boxrig_unicode_cluster_analyzer {
    int32_t previous;
    int32_t grapheme_state;
    uint8_t column_width;
    bool active;
} boxrig_unicode_cluster_analyzer;

typedef boxrig_status (*boxrig_unicode_emit)(void *context, int32_t codepoint);

void boxrig_unicode_decoder_initialize(boxrig_unicode_decoder *decoder);
boxrig_status boxrig_unicode_decoder_put(
    boxrig_unicode_decoder *decoder,
    uint8_t byte,
    boxrig_unicode_emit emit,
    void *context);
boxrig_status boxrig_unicode_decoder_flush(
    boxrig_unicode_decoder *decoder,
    boxrig_unicode_emit emit,
    void *context);
size_t boxrig_unicode_encode(int32_t codepoint, uint8_t output[4]);
bool boxrig_unicode_grapheme_break(
    int32_t previous,
    int32_t current,
    int32_t *state);
int boxrig_unicode_column_width(int32_t codepoint, bool ambiguous_is_wide);
bool boxrig_unicode_is_regional_indicator(int32_t codepoint);
void boxrig_unicode_cluster_analyzer_initialize(
    boxrig_unicode_cluster_analyzer *analyzer);
bool boxrig_unicode_cluster_analyzer_accept(
    boxrig_unicode_cluster_analyzer *analyzer,
    int32_t codepoint,
    bool ambiguous_is_wide);
boxrig_status boxrig_unicode_measure_text_cluster(
    const uint8_t *text,
    size_t text_length,
    bool ambiguous_is_wide,
    boxrig_text_cluster_measurement *measurement_out);

#endif
