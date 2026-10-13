/* boxrig - Under MIT. */
#ifndef BOXRIG_TEST_BLINK_RENDERER_FIXTURE_H
#define BOXRIG_TEST_BLINK_RENDERER_FIXTURE_H

#include "color_renderer_fixture.h"

/* Shared capture adapters observe phases without depending on native timers. */
bool boxrig_test_renderer_cursor_blink(
    bool (*create)(boxrig_test_color_renderer *renderer));

bool boxrig_test_renderer_text_blink(
    bool (*create)(boxrig_test_color_renderer *renderer));

bool boxrig_test_renderer_blink_composition(
    bool (*create)(boxrig_test_color_renderer *renderer));

bool boxrig_test_renderer_cursor_style(
    bool (*create)(boxrig_test_color_renderer *renderer));

bool boxrig_test_renderer_filled_cursor(
    bool (*create)(boxrig_test_color_renderer *renderer), unsigned int stroke);

bool boxrig_test_renderer_cursor_style_at_scale(
    bool (*create)(boxrig_test_color_renderer *renderer),
    unsigned int expected_stroke_x, unsigned int expected_stroke_y);

/* Selection fill, foreground override and translucent markers survive blink. */
bool boxrig_test_renderer_selection_blink(
    bool (*create)(boxrig_test_color_renderer *renderer));

#endif
