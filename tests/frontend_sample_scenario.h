/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TEST_FRONTEND_SAMPLE_SCENARIO_H
#define BOXRIG_TEST_FRONTEND_SAMPLE_SCENARIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Current controlled PTY sample scenario contract. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_SCENARIO_VERSION 7

/** Expected initial and post-zoom terminal row count. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_ROWS 24u

/** Expected initial and post-zoom terminal column count. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_COLUMNS 80u

/** Title prefix followed by the reported cell height and width. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_READY_PREFIX "boxrig-scenario-ready-"

/** Title published after one native key reaches the controlled child. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_KEY_TITLE "boxrig-scenario-key-received"

/** Title published after the linear selection round trip. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_LINEAR_TITLE \
    "boxrig-scenario-linear-copy-received"

/** Title published after native input enables mouse reporting. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_MOUSE_READY_TITLE \
    "boxrig-scenario-mouse-reporting-ready"

/** Title published after an unmodified native click reaches the child. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_MOUSE_TITLE \
    "boxrig-scenario-mouse-received"

/** Title published after the Shift override selection round trip. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_SHIFT_TITLE \
    "boxrig-scenario-shift-copy-received"

/** Title published after the rectangular selection round trip. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_RECTANGLE_TITLE \
    "boxrig-scenario-rectangle-copy-received"

/** Title expected after zoom preserves the controlled grid. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_ZOOM_TITLE \
    "boxrig-scenario-zoom-grid-24-80"

/** Title expected after zoom reset preserves the controlled grid. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_RESET_TITLE \
    "boxrig-scenario-reset-grid-24-80"

/** Title requesting the initial instrumented overlay scene. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_READY_TITLE \
    "boxrig-scenario-overlay-ready"

/** Title requesting the second overlay frame after native hover checks. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_UPDATED_TITLE \
    "boxrig-scenario-overlay-updated"

/** Title published after the frontend composition commit reaches the child. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_COMMIT_TITLE \
    "boxrig-scenario-overlay-ime-received"

/** Title confirming the native opacity decrease checkpoint. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_OPACITY_TITLE \
    "boxrig-scenario-overlay-opacity-received"

/** Title requesting overlay cleanup after native opacity restoration. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_CLEARED_TITLE \
    "boxrig-scenario-overlay-cleared"

/** Final title requiring all native overlay observations to be complete. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_VERIFIED_TITLE \
    "boxrig-scenario-overlay-verified"

/** Recognizable visible URL enclosed by an explicit OSC 8 link. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_OSC8_TEXT \
    "https://example.test/explicit"

/** Recognizable visible URL without an explicit OSC 8 link. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_URL_TEXT \
    "https://example.test/inferred"

/** UTF-8 text committed through the sample's frontend composition sink. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_IME_COMMIT "ime-\347\225\214"

/** Shared geometry checkpoint prefix, followed by operation and actual grid. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PREFIX "boxrig-scenario-geometry-"

/** Retry prefix followed by operation, sequence, and observed grid. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PENDING_PREFIX \
    "boxrig-scenario-pending-geometry-"

/** Required ordinary resize row count. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_RESIZED_ROWS 28u

/** Required ordinary resize column count. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_RESIZED_COLUMNS 88u

/** Explicit external-environment boundary for Wine's fixed console query. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_FIXED_GRID_SUFFIX "-p2-wine-fixed-console"

/** Title prefix used to expose an unexpected native-input transcript. */
#define BOXRIG_TEST_FRONTEND_SAMPLE_UNEXPECTED_PREFIX \
    "boxrig-scenario-unexpected-"

/** Platform I/O used by the shared controlled PTY child scenario. */
typedef struct boxrig_test_frontend_sample_scenario_operations {
    /** Caller-owned state passed to each operation. */
    void *context;
    /** Expects the test probe's outbound pressure prefix before the first paste. */
    bool expect_paste_pressure;
    /** Exercises the instrumented sample's overlay and composition checkpoints. */
    bool expect_overlays;
    /** Exercises ordinary resize, maximize, and both normal-state restorations. */
    bool expect_geometry;
    /** Exercises wheel reports and Ctrl-C while synchronized drawing is held. */
    bool expect_wheel;
    /** Expects Wine's initial console query, without claiming resized PTY coverage. */
    bool expect_fixed_console_grid;
    /** Writes the complete byte span to the terminal, or returns false. */
    bool (*write_terminal)(
        void *context,
        const uint8_t *bytes,
        size_t length);
    /** Reads exactly length input bytes, or returns false. */
    bool (*read_terminal)(
        void *context,
        uint8_t *bytes,
        size_t length);
    /** Publishes a checkpoint title, or returns false. */
    bool (*set_title)(void *context, const char *title);
    /** Queries the current PTY row and column counts, or returns false. */
    bool (*query_grid)(
        void *context,
        unsigned int *rows_out,
        unsigned int *columns_out);
} boxrig_test_frontend_sample_scenario_operations;

/**
 * Runs the shared sample scene and validates its native-input transcript.
 *
 * @param operations Platform PTY and title operations.
 * @return true when every scenario checkpoint completes with the common
 * expected result.
 */
bool boxrig_test_run_frontend_sample_scenario(
    const boxrig_test_frontend_sample_scenario_operations *operations);

#endif
