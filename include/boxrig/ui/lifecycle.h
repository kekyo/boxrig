/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_LIFECYCLE_H
#define BOXRIG_UI_LIFECYCLE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Ordered phases of a platform UI shutdown. */
typedef enum boxrig_ui_lifecycle_state {
    /** Native callbacks may reach live frontend resources. */
    BOXRIG_UI_LIFECYCLE_RUNNING = 0,
    /** New callback delivery is closed while resources remain valid. */
    BOXRIG_UI_LIFECYCLE_QUIESCING = 1,
    /** Callback targets have been detached from the native window. */
    BOXRIG_UI_LIFECYCLE_DETACHED = 2,
    /** Native and frontend resources have been released. */
    BOXRIG_UI_LIFECYCLE_STOPPED = 3,
} boxrig_ui_lifecycle_state;

/** Platform-independent lifecycle state owned by one example frontend. */
typedef struct boxrig_ui_lifecycle {
    /** Current ordered shutdown phase. */
    boxrig_ui_lifecycle_state state;
} boxrig_ui_lifecycle;

/**
 * Initializes a lifecycle in the running phase.
 *
 * @param lifecycle Lifecycle to initialize, or NULL.
 */
void boxrig_ui_lifecycle_initialize(boxrig_ui_lifecycle *lifecycle);

/**
 * Returns the current lifecycle phase.
 *
 * @param lifecycle Lifecycle to inspect.
 * @return Current phase, or stopped for NULL.
 */
boxrig_ui_lifecycle_state boxrig_ui_lifecycle_get_state(
    const boxrig_ui_lifecycle *lifecycle);

/**
 * Reports whether native callbacks may reach frontend resources.
 *
 * @param lifecycle Lifecycle to inspect.
 * @return True only during the running phase.
 */
bool boxrig_ui_lifecycle_accepts_events(
    const boxrig_ui_lifecycle *lifecycle);

/**
 * Closes delivery of new native callbacks.
 *
 * Repeating this transition after shutdown has started is harmless.
 *
 * @param lifecycle Lifecycle to advance.
 * @return Zero on success, or -1 for NULL or an invalid phase.
 */
int boxrig_ui_lifecycle_begin_shutdown(boxrig_ui_lifecycle *lifecycle);

/**
 * Records that all callback targets have been detached.
 *
 * The quiescing phase must have been reached first. Repeating this transition
 * after detachment is harmless.
 *
 * @param lifecycle Lifecycle to advance.
 * @return Zero on success, or -1 for NULL or a skipped phase.
 */
int boxrig_ui_lifecycle_mark_detached(boxrig_ui_lifecycle *lifecycle);

/**
 * Records that native and frontend resources have been released.
 *
 * The detached phase must have been reached first. Repeating this transition
 * after shutdown is harmless.
 *
 * @param lifecycle Lifecycle to advance.
 * @return Zero on success, or -1 for NULL or a skipped phase.
 */
int boxrig_ui_lifecycle_mark_stopped(boxrig_ui_lifecycle *lifecycle);

#ifdef __cplusplus
}
#endif

#endif
