/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_STATUS_H
#define BOXRIG_STATUS_H

/** Result codes returned by fallible boxrig operations. */
typedef enum boxrig_status {
    /** The operation completed successfully. */
    BOXRIG_STATUS_OK = 0,
    /** An argument or configuration is invalid. */
    BOXRIG_STATUS_INVALID_ARGUMENT,
    /** Memory allocation failed. */
    BOXRIG_STATUS_OUT_OF_MEMORY,
    /** A configured or public resource bound was exceeded. */
    BOXRIG_STATUS_LIMIT_EXCEEDED,
    /** Retry after the required capacity or I/O readiness is available. */
    BOXRIG_STATUS_WOULD_BLOCK,
    /** A current result set is not available until bounded work completes. */
    BOXRIG_STATUS_INCOMPLETE,
    /** A retained text position or range no longer names the same text. */
    BOXRIG_STATUS_STALE,
    /** An exclusive owner already controls this operation. */
    BOXRIG_STATUS_BUSY
} boxrig_status;

#endif
