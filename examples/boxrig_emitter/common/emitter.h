/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_EMITTER_H
#define BOXRIG_EMITTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Identifies the destination stream for emitter output. */
typedef enum boxrig_emitter_stream {
    /** Test payload and normal command output. */
    BOXRIG_EMITTER_STANDARD = 0,
    /** Command-line diagnostics and I/O errors. */
    BOXRIG_EMITTER_DIAGNOSTIC = 1
} boxrig_emitter_stream;

/** Classifies how an emitter case interacts with a terminal. */
typedef enum boxrig_emitter_case_class {
    /** Writes a fixed terminal-directed payload. */
    BOXRIG_EMITTER_CASE_OUTPUT = 0,
    /** Writes a query and captures its response. */
    BOXRIG_EMITTER_CASE_ROUND_TRIP = 1,
    /** Enables an input mode and captures a user event. */
    BOXRIG_EMITTER_CASE_INTERACTIVE = 2,
    /** Describes a frontend operation that is not an escape sequence. */
    BOXRIG_EMITTER_CASE_FRONTEND = 3
} boxrig_emitter_case_class;

/** Describes a command-line selectable emitter case. */
typedef struct boxrig_emitter_case_info {
    /** Stable lowercase ASCII command-line name. */
    const char *name;
    /** Short category shown by `--list`. */
    const char *category;
    /** Human-readable purpose. */
    const char *description;
    /** Observable result expected from the terminal. */
    const char *expectation;
    /** Matching section identifier in compatibility.md. */
    const char *compatibility_section;
    /** Control or protocol family exercised by the case. */
    const char *sequence_family;
    /** Interaction class. */
    boxrig_emitter_case_class case_class;
    /** Whether the case can change the entire screen. */
    bool screen_wide;
    /** Whether the case requests an effect outside the terminal grid. */
    bool host_effect;
    /** Whether the case reads terminal or user input. */
    bool input_required;
} boxrig_emitter_case_info;

/** Maximum byte length of each platform-created graphics fixture name. */
#define BOXRIG_EMITTER_GRAPHICS_NAME_CAPACITY 1024u

/** Pixel width of each external graphics fixture. */
#define BOXRIG_EMITTER_GRAPHICS_FIXTURE_WIDTH 16u

/** Pixel height of each external graphics fixture. */
#define BOXRIG_EMITTER_GRAPHICS_FIXTURE_HEIGHT 16u

/** Byte length of each external RGBA graphics fixture. */
#define BOXRIG_EMITTER_GRAPHICS_FIXTURE_RGBA_LENGTH                         \
    (BOXRIG_EMITTER_GRAPHICS_FIXTURE_WIDTH *                                \
     BOXRIG_EMITTER_GRAPHICS_FIXTURE_HEIGHT * 4u)

/** Selects the palette for one platform-created graphics resource. */
typedef enum boxrig_emitter_graphics_fixture_kind {
    /** Red mountain palette for the regular-file resource. */
    BOXRIG_EMITTER_GRAPHICS_FIXTURE_REGULAR = 0,
    /** Green mountain palette for the temporary-file resource. */
    BOXRIG_EMITTER_GRAPHICS_FIXTURE_TEMPORARY = 1,
    /** Blue mountain palette for the shared-memory resource. */
    BOXRIG_EMITTER_GRAPHICS_FIXTURE_SHARED = 2
} boxrig_emitter_graphics_fixture_kind;

/** Names of platform-owned resources used by the external graphics case. */
typedef struct boxrig_emitter_graphics_fixture {
    /** Length-delimited regular-file name encoded by the common emitter. */
    uint8_t regular_name[BOXRIG_EMITTER_GRAPHICS_NAME_CAPACITY];
    /** Number of bytes in regular_name. */
    size_t regular_name_length;
    /** Length-delimited safe temporary-file name. */
    uint8_t temporary_name[BOXRIG_EMITTER_GRAPHICS_NAME_CAPACITY];
    /** Number of bytes in temporary_name. */
    size_t temporary_name_length;
    /** Length-delimited shared-memory or named-mapping name. */
    uint8_t shared_name[BOXRIG_EMITTER_GRAPHICS_NAME_CAPACITY];
    /** Number of bytes in shared_name. */
    size_t shared_name_length;
} boxrig_emitter_graphics_fixture;

/** Platform I/O callbacks used by the common emitter implementation. */
typedef struct boxrig_emitter_io {
    /** Opaque callback context. */
    void *context;
    /**
     * Writes the complete byte range to the selected stream.
     *
     * @param context Opaque callback context.
     * @param stream Destination stream.
     * @param bytes Binary byte range. May contain NUL.
     * @param length Exact byte count.
     * @return 0 on success, -1 on failure.
     */
    int (*write)(
        void *context,
        boxrig_emitter_stream stream,
        const uint8_t *bytes,
        size_t length);
    /**
     * Reads one byte from standard input.
     *
     * @param context Opaque callback context.
     * @param byte_out Receives the byte on success.
     * @return 1 when a byte was read, 0 at EOF, -1 on failure.
     */
    int (*read_byte)(void *context, uint8_t *byte_out);
    /**
     * Switches input to noncanonical, no-echo capture mode.
     *
     * @param context Opaque callback context.
     * @return 0 on success, -1 on failure.
     */
    int (*capture_begin)(void *context);
    /**
     * Restores input after capture mode.
     *
     * @param context Opaque callback context.
     * @return 0 on success, -1 on failure.
     */
    int (*capture_end)(void *context);
    /**
     * Creates the three safe resources used by the external graphics case.
     *
     * @param context Opaque callback context.
     * @param fixture_out Receives resource names that remain valid by value.
     * @return 0 on success, -1 on failure.
     *
     * @remarks Each resource contains one 16-by-16 RGBA pixel-art landscape:
     * regular file is red, temporary file is green, and shared memory is
     * blue. A failed call must release any resources it created.
     */
    int (*graphics_fixture_begin)(
        void *context,
        boxrig_emitter_graphics_fixture *fixture_out);
    /**
     * Releases resources created by graphics_fixture_begin.
     *
     * @param context Opaque callback context.
     * @return 0 when all resources were released and lifecycle checks passed,
     * or -1 on failure.
     */
    int (*graphics_fixture_end)(void *context);
} boxrig_emitter_io;

/**
 * Builds the RGBA pixel-art data for one external graphics resource.
 *
 * @param kind Resource palette to generate.
 * @param rgba Destination byte buffer.
 * @param rgba_capacity Available bytes in rgba.
 * @return 0 on success, or -1 for an unknown kind, NULL destination, or a
 * buffer smaller than BOXRIG_EMITTER_GRAPHICS_FIXTURE_RGBA_LENGTH.
 */
int boxrig_emitter_make_graphics_fixture_pixels(
    boxrig_emitter_graphics_fixture_kind kind,
    uint8_t *rgba,
    size_t rgba_capacity);

/**
 * Writes an exact binary range through an emitter I/O callback.
 *
 * @param io Configured I/O callbacks.
 * @param stream Destination stream.
 * @param bytes Binary byte range. May contain NUL.
 * @param length Exact byte count.
 * @return 0 on success, -1 for invalid arguments or callback failure.
 */
int boxrig_emitter_write(
    const boxrig_emitter_io *io,
    boxrig_emitter_stream stream,
    const uint8_t *bytes,
    size_t length);

/**
 * Returns the number of selectable cases in stable display order.
 *
 * @return Case count.
 */
size_t boxrig_emitter_case_count(void);

/**
 * Returns case metadata at a stable catalog index.
 *
 * @param index Zero-based catalog index.
 * @return Case metadata, or NULL when index is out of range.
 */
const boxrig_emitter_case_info *boxrig_emitter_case_at(size_t index);

/**
 * Runs the platform-independent emitter command line.
 *
 * @param argument_count Number of command-line arguments including argv[0].
 * @param arguments ASCII command-line arguments.
 * @param io Configured platform I/O callbacks.
 * @return 0 on success, 1 on I/O failure, or 2 on command-line error.
 */
int boxrig_emitter_run(
    int argument_count,
    const char *const *arguments,
    const boxrig_emitter_io *io);

#endif
