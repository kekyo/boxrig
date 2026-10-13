/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TEST_COMPOSITION_FIXTURE_H
#define BOXRIG_TEST_COMPOSITION_FIXTURE_H

#include "boxrig/ui/composition.h"

#include <stddef.h>
#include <stdint.h>

typedef enum boxrig_composition_fixture_attribute {
    BOXRIG_COMPOSITION_FIXTURE_INPUT = 0,
    BOXRIG_COMPOSITION_FIXTURE_CONVERTED,
    BOXRIG_COMPOSITION_FIXTURE_TARGET
} boxrig_composition_fixture_attribute;

typedef struct boxrig_composition_fixture_data {
    const uint32_t *codepoints;
    const boxrig_composition_fixture_attribute *attributes;
    size_t character_count;
    const char *utf8;
    size_t utf8_length;
    const boxrig_composition_segment *segments;
    size_t segment_count;
    size_t selection_offset;
    size_t selection_length;
    size_t caret_offset;
    const char *commit_utf8;
    size_t commit_utf8_length;
} boxrig_composition_fixture_data;

static const uint32_t boxrig_fixture_codepoints[] = {
    0x0041,
    0x754c,
    0x0065,
    0x0301,
};

static const boxrig_composition_fixture_attribute boxrig_fixture_attributes[] = {
    BOXRIG_COMPOSITION_FIXTURE_INPUT,
    BOXRIG_COMPOSITION_FIXTURE_CONVERTED,
    BOXRIG_COMPOSITION_FIXTURE_TARGET,
    BOXRIG_COMPOSITION_FIXTURE_TARGET,
};

static const boxrig_composition_segment boxrig_fixture_segments[] = {
    {0, 1, BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE},
    {1, 3,
     BOXRIG_COMPOSITION_FEEDBACK_PRIMARY |
         BOXRIG_COMPOSITION_FEEDBACK_CONVERTED},
    {4, 3,
     BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
         BOXRIG_COMPOSITION_FEEDBACK_TARGET},
};

static const boxrig_composition_fixture_data boxrig_composition_fixture = {
    boxrig_fixture_codepoints,
    boxrig_fixture_attributes,
    sizeof(boxrig_fixture_codepoints) / sizeof(boxrig_fixture_codepoints[0]),
    "A\xe7\x95\x8c" "e\xcc\x81",
    7,
    boxrig_fixture_segments,
    sizeof(boxrig_fixture_segments) / sizeof(boxrig_fixture_segments[0]),
    4,
    3,
    4,
    "\xe6\x97\xa5\xe6\x9c\xac",
    6,
};

#endif
