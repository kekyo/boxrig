/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "word_break.h"

#include <stdlib.h>
#include <stdint.h>

typedef enum boxrig_word_break_property {
    BOXRIG_WORD_BREAK_OTHER = 0,
    BOXRIG_WORD_BREAK_ALETTER,
    BOXRIG_WORD_BREAK_CR,
    BOXRIG_WORD_BREAK_DOUBLE_QUOTE,
    BOXRIG_WORD_BREAK_EXTEND,
    BOXRIG_WORD_BREAK_EXTEND_NUM_LET,
    BOXRIG_WORD_BREAK_FORMAT,
    BOXRIG_WORD_BREAK_HEBREW_LETTER,
    BOXRIG_WORD_BREAK_KATAKANA,
    BOXRIG_WORD_BREAK_LF,
    BOXRIG_WORD_BREAK_MID_LETTER,
    BOXRIG_WORD_BREAK_MID_NUM,
    BOXRIG_WORD_BREAK_MID_NUM_LET,
    BOXRIG_WORD_BREAK_NEWLINE,
    BOXRIG_WORD_BREAK_NUMERIC,
    BOXRIG_WORD_BREAK_REGIONAL_INDICATOR,
    BOXRIG_WORD_BREAK_SINGLE_QUOTE,
    BOXRIG_WORD_BREAK_WSEG_SPACE,
    BOXRIG_WORD_BREAK_ZWJ
} boxrig_word_break_property;

typedef struct boxrig_word_break_range {
    uint32_t first;
    uint32_t last;
    boxrig_word_break_property property;
} boxrig_word_break_range;

typedef struct boxrig_unicode_range {
    uint32_t first;
    uint32_t last;
} boxrig_unicode_range;

#include "word_break_data.inc"

static boxrig_word_break_property word_break_property(uint32_t codepoint)
{
    size_t low = 0;
    size_t high = sizeof(boxrig_word_break_ranges) /
                  sizeof(boxrig_word_break_ranges[0]);

    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const boxrig_word_break_range *range =
            &boxrig_word_break_ranges[middle];

        if (codepoint < range->first) {
            high = middle;
        } else if (codepoint > range->last) {
            low = middle + 1;
        } else {
            return range->property;
        }
    }
    return BOXRIG_WORD_BREAK_OTHER;
}

static bool is_extended_pictographic(uint32_t codepoint)
{
    size_t low = 0;
    size_t high = sizeof(boxrig_extended_pictographic_ranges) /
                  sizeof(boxrig_extended_pictographic_ranges[0]);

    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const boxrig_unicode_range *range =
            &boxrig_extended_pictographic_ranges[middle];

        if (codepoint < range->first) {
            high = middle;
        } else if (codepoint > range->last) {
            low = middle + 1;
        } else {
            return true;
        }
    }
    return false;
}

static bool is_newline(boxrig_word_break_property property)
{
    return property == BOXRIG_WORD_BREAK_CR ||
           property == BOXRIG_WORD_BREAK_LF ||
           property == BOXRIG_WORD_BREAK_NEWLINE;
}

static bool is_ignored(boxrig_word_break_property property)
{
    return property == BOXRIG_WORD_BREAK_EXTEND ||
           property == BOXRIG_WORD_BREAK_FORMAT ||
           property == BOXRIG_WORD_BREAK_ZWJ;
}

static bool is_ah_letter(boxrig_word_break_property property)
{
    return property == BOXRIG_WORD_BREAK_ALETTER ||
           property == BOXRIG_WORD_BREAK_HEBREW_LETTER;
}

static bool is_mid_num_let_q(boxrig_word_break_property property)
{
    return property == BOXRIG_WORD_BREAK_MID_NUM_LET ||
           property == BOXRIG_WORD_BREAK_SINGLE_QUOTE;
}

static bool is_mid_letter_or_q(boxrig_word_break_property property)
{
    return property == BOXRIG_WORD_BREAK_MID_LETTER ||
           is_mid_num_let_q(property);
}

static bool is_mid_num_or_q(boxrig_word_break_property property)
{
    return property == BOXRIG_WORD_BREAK_MID_NUM ||
           is_mid_num_let_q(property);
}

static bool is_extend_num_left(boxrig_word_break_property property)
{
    return is_ah_letter(property) ||
           property == BOXRIG_WORD_BREAK_NUMERIC ||
           property == BOXRIG_WORD_BREAK_KATAKANA ||
           property == BOXRIG_WORD_BREAK_EXTEND_NUM_LET;
}

static bool is_extend_num_right(boxrig_word_break_property property)
{
    return is_ah_letter(property) ||
           property == BOXRIG_WORD_BREAK_NUMERIC ||
           property == BOXRIG_WORD_BREAK_KATAKANA;
}

static size_t previous_significant(
    const boxrig_word_break_property *properties,
    size_t before)
{
    while (before != 0) {
        before--;
        if (!is_ignored(properties[before])) {
            return before;
        }
    }
    return SIZE_MAX;
}

static size_t next_significant(
    const boxrig_word_break_property *properties,
    size_t count,
    size_t from)
{
    while (from < count && is_ignored(properties[from])) {
        from++;
    }
    return from;
}

static bool boundary_at(
    const uint32_t *codepoints,
    const boxrig_word_break_property *properties,
    size_t count,
    size_t boundary)
{
    boxrig_word_break_property immediate_left = properties[boundary - 1];
    boxrig_word_break_property immediate_right = properties[boundary];
    size_t left;
    size_t right;
    size_t left_previous;
    size_t right_next;

    if (immediate_left == BOXRIG_WORD_BREAK_CR &&
        immediate_right == BOXRIG_WORD_BREAK_LF) {
        return false;
    }
    if (is_newline(immediate_left) || is_newline(immediate_right)) {
        return true;
    }
    if (immediate_left == BOXRIG_WORD_BREAK_ZWJ &&
        is_extended_pictographic(codepoints[boundary])) {
        return false;
    }
    if (immediate_left == BOXRIG_WORD_BREAK_WSEG_SPACE &&
        immediate_right == BOXRIG_WORD_BREAK_WSEG_SPACE) {
        return false;
    }
    if (is_ignored(immediate_right)) {
        return false;
    }
    left = previous_significant(properties, boundary);
    right = next_significant(properties, count, boundary);
    if (left == SIZE_MAX || right == count) {
        return true;
    }
    if (is_newline(properties[left]) || is_newline(properties[right])) {
        return true;
    }
    if (is_ah_letter(properties[left]) &&
        is_ah_letter(properties[right])) {
        return false;
    }
    right_next = next_significant(properties, count, right + 1);
    if (is_ah_letter(properties[left]) &&
        is_mid_letter_or_q(properties[right]) && right_next < count &&
        is_ah_letter(properties[right_next])) {
        return false;
    }
    left_previous = previous_significant(properties, left);
    if (left_previous != SIZE_MAX &&
        is_ah_letter(properties[left_previous]) &&
        is_mid_letter_or_q(properties[left]) &&
        is_ah_letter(properties[right])) {
        return false;
    }
    if (properties[left] == BOXRIG_WORD_BREAK_HEBREW_LETTER &&
        properties[right] == BOXRIG_WORD_BREAK_SINGLE_QUOTE) {
        return false;
    }
    if (properties[left] == BOXRIG_WORD_BREAK_HEBREW_LETTER &&
        properties[right] == BOXRIG_WORD_BREAK_DOUBLE_QUOTE &&
        right_next < count &&
        properties[right_next] == BOXRIG_WORD_BREAK_HEBREW_LETTER) {
        return false;
    }
    if (left_previous != SIZE_MAX &&
        properties[left_previous] == BOXRIG_WORD_BREAK_HEBREW_LETTER &&
        properties[left] == BOXRIG_WORD_BREAK_DOUBLE_QUOTE &&
        properties[right] == BOXRIG_WORD_BREAK_HEBREW_LETTER) {
        return false;
    }
    if (properties[left] == BOXRIG_WORD_BREAK_NUMERIC &&
        properties[right] == BOXRIG_WORD_BREAK_NUMERIC) {
        return false;
    }
    if ((is_ah_letter(properties[left]) &&
         properties[right] == BOXRIG_WORD_BREAK_NUMERIC) ||
        (properties[left] == BOXRIG_WORD_BREAK_NUMERIC &&
         is_ah_letter(properties[right]))) {
        return false;
    }
    if (properties[left] == BOXRIG_WORD_BREAK_NUMERIC &&
        is_mid_num_or_q(properties[right]) && right_next < count &&
        properties[right_next] == BOXRIG_WORD_BREAK_NUMERIC) {
        return false;
    }
    if (left_previous != SIZE_MAX &&
        properties[left_previous] == BOXRIG_WORD_BREAK_NUMERIC &&
        is_mid_num_or_q(properties[left]) &&
        properties[right] == BOXRIG_WORD_BREAK_NUMERIC) {
        return false;
    }
    if (properties[left] == BOXRIG_WORD_BREAK_KATAKANA &&
        properties[right] == BOXRIG_WORD_BREAK_KATAKANA) {
        return false;
    }
    if (is_extend_num_left(properties[left]) &&
        properties[right] == BOXRIG_WORD_BREAK_EXTEND_NUM_LET) {
        return false;
    }
    if (properties[left] == BOXRIG_WORD_BREAK_EXTEND_NUM_LET &&
        is_extend_num_right(properties[right])) {
        return false;
    }
    if (properties[left] == BOXRIG_WORD_BREAK_REGIONAL_INDICATOR &&
        properties[right] == BOXRIG_WORD_BREAK_REGIONAL_INDICATOR) {
        size_t regional_count = 0;
        size_t candidate = left;

        while (candidate != SIZE_MAX &&
               properties[candidate] ==
                   BOXRIG_WORD_BREAK_REGIONAL_INDICATOR) {
            regional_count++;
            candidate = previous_significant(properties, candidate);
        }
        if ((regional_count & 1u) != 0) {
            return false;
        }
    }
    return true;
}

bool boxrig_word_break_resolve(
    const uint32_t *codepoints,
    size_t codepoint_count,
    bool *boundaries_out,
    size_t boundary_capacity)
{
    boxrig_word_break_property *properties = NULL;
    size_t index;

    if ((codepoint_count != 0 && codepoints == NULL) ||
        boundaries_out == NULL || boundary_capacity < codepoint_count + 1 ||
        codepoint_count == SIZE_MAX ||
        codepoint_count > SIZE_MAX / sizeof(*properties)) {
        return false;
    }
    if (codepoint_count != 0) {
        properties = malloc(codepoint_count * sizeof(*properties));
        if (properties == NULL) {
            return false;
        }
    }
    for (index = 0; index < codepoint_count; index++) {
        properties[index] = word_break_property(codepoints[index]);
    }
    boundaries_out[0] = true;
    for (index = 1; index < codepoint_count; index++) {
        boundaries_out[index] = boundary_at(
            codepoints,
            properties,
            codepoint_count,
            index);
    }
    boundaries_out[codepoint_count] = true;
    free(properties);
    return true;
}
