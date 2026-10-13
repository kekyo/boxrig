/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TEST_FRONTEND_INPUT_FIXTURE_H
#define BOXRIG_TEST_FRONTEND_INPUT_FIXTURE_H

#include "boxrig/ui.h"

#include <stdbool.h>
#include <stddef.h>

#define BOXRIG_TEST_FRONTEND_KEY_TRANSITIONS 3
#define BOXRIG_TEST_FRONTEND_MOUSE_TRANSITIONS 3

/** Native input coordinates shared by every frontend adapter. */
typedef struct boxrig_test_frontend_input_script {
    /** Horizontal fixed-point coordinate in frontend output pixels. */
    boxrig_ui_fixed x;
    /** Vertical fixed-point coordinate in frontend output pixels. */
    boxrig_ui_fixed y;
} boxrig_test_frontend_input_script;

/** Portable events observed after one native frontend input transcript. */
typedef struct boxrig_test_frontend_input_observation {
    /** Normalized physical key transitions. */
    boxrig_key_event key_events[BOXRIG_TEST_FRONTEND_KEY_TRANSITIONS];
    /** Normalized pointer transitions. */
    boxrig_mouse_event mouse_events[BOXRIG_TEST_FRONTEND_MOUSE_TRANSITIONS];
    /** Immutable hit paired with each normalized pointer transition. */
    boxrig_ui_hit_test_result
        mouse_hits[BOXRIG_TEST_FRONTEND_MOUSE_TRANSITIONS];
    /** Number of captured key transitions. */
    size_t key_count;
    /** Number of captured pointer transitions. */
    size_t mouse_count;
    /** Whether the native focus-enter operation took effect. */
    bool focused_after_enter;
    /** Whether focus remained after the native focus-leave operation. */
    bool focused_after_leave;
} boxrig_test_frontend_input_observation;

/** Captures a normalized key transition for the shared transcript. */
boxrig_input_key_route boxrig_test_frontend_capture_key(
    const boxrig_key_event *event,
    void *context);

/** Captures a normalized pointer transition and its immutable hit. */
boxrig_input_mouse_route boxrig_test_frontend_capture_mouse(
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    void *context);

/** Checks that a native adapter reached the portable transcript result. */
bool boxrig_test_frontend_input_matches(
    const boxrig_test_frontend_input_script *script,
    const boxrig_test_frontend_input_observation *observation);

#endif
