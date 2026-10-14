/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_SCROLLBAR_H
#define BOXRIG_UI_SCROLLBAR_H

#include "boxrig/terminal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Identifies one physical-row scrollbar operation. */
typedef enum boxrig_scrollbar_action {
    /** Moves one physical row toward older history. */
    BOXRIG_SCROLLBAR_LINE_UP = 0,
    /** Moves one physical row toward live output. */
    BOXRIG_SCROLLBAR_LINE_DOWN,
    /** Moves one viewport page toward older history. */
    BOXRIG_SCROLLBAR_PAGE_UP,
    /** Moves one viewport page toward live output. */
    BOXRIG_SCROLLBAR_PAGE_DOWN,
    /** Moves to the oldest retained physical row. */
    BOXRIG_SCROLLBAR_TOP,
    /** Moves to live output. */
    BOXRIG_SCROLLBAR_BOTTOM,
    /** Moves to an absolute oldest-to-live scrollbar position. */
    BOXRIG_SCROLLBAR_POSITION
} boxrig_scrollbar_action;

/** Describes terminal viewport state in oldest-to-live scrollbar coordinates. */
typedef struct boxrig_scrollbar_model {
    /** Number of retained primary-screen physical rows. */
    size_t history_rows;
    /** Number of physical rows visible in the terminal viewport. */
    size_t viewport_rows;
    /** Number of retained rows above the live viewport. */
    size_t viewport_offset;
    /** Position from zero at oldest history to history_rows at live output. */
    size_t position;
    /** Whether primary history exists and can currently be navigated. */
    bool enabled;
} boxrig_scrollbar_model;

/** Synchronized scrollbar values shared by the views of one UI model. */
typedef struct boxrig_scrollbar_state {
    /** Current physical-row range, position and availability. */
    boxrig_scrollbar_model model;
    /** Nonzero revision changed only when one of the model values changes.
     * Compare revisions only within the lifetime of the same UI model. */
    uint64_t revision;
} boxrig_scrollbar_state;

/** Describes a proportional thumb inside a one-dimensional track. */
typedef struct boxrig_scrollbar_thumb {
    /** Zero-based thumb start in track units. */
    size_t start;
    /** Thumb length in track units. */
    size_t length;
} boxrig_scrollbar_thumb;

/**
 * Reads terminal viewport state into an oldest-to-live scrollbar model.
 *
 * @param terminal Terminal whose primary viewport is represented.
 * @param model_out Receives the synchronized model.
 * @return Zero on success or -1 for invalid arguments.
 *
 * @remarks The current offset is submitted unchanged through the terminal API
 * to distinguish an active primary screen from an alternate screen. No dirty
 * is generated when the primary offset is unchanged.
 */
int boxrig_scrollbar_model_read(
    boxrig_terminal *terminal,
    boxrig_scrollbar_model *model_out);

/**
 * Applies one scrollbar operation to the terminal primary viewport.
 *
 * @param terminal Terminal whose viewport is changed.
 * @param action Operation expressed in oldest-to-live coordinates.
 * @param position Absolute position used only by BOXRIG_SCROLLBAR_POSITION.
 * @param changed_out Receives whether the viewport offset changed.
 * @return Zero on success or -1 for invalid arguments or terminal rejection.
 *
 * @remarks Operations are harmless no-ops while the scrollbar is disabled.
 */
int boxrig_scrollbar_apply_action(
    boxrig_terminal *terminal,
    boxrig_scrollbar_action action,
    size_t position,
    bool *changed_out);

/**
 * Calculates a proportional thumb for a fixed-length track.
 *
 * @param model Current oldest-to-live viewport model.
 * @param track_length Track length in caller-defined units.
 * @param minimum_length Smallest visible thumb length in the same units.
 * @param thumb_out Receives a thumb clamped inside the track.
 */
void boxrig_scrollbar_calculate_thumb(
    const boxrig_scrollbar_model *model,
    size_t track_length,
    size_t minimum_length,
    boxrig_scrollbar_thumb *thumb_out);

/**
 * Converts a dragged thumb start into an oldest-to-live position.
 *
 * @param model Current oldest-to-live viewport model.
 * @param track_length Track length in caller-defined units.
 * @param minimum_length Minimum passed to boxrig_scrollbar_calculate_thumb().
 * @param thumb_start Requested thumb start in track units.
 * @return A position clamped from zero through model history_rows.
 */
size_t boxrig_scrollbar_position_from_thumb(
    const boxrig_scrollbar_model *model,
    size_t track_length,
    size_t minimum_length,
    size_t thumb_start);

#ifdef __cplusplus
}
#endif

#endif
