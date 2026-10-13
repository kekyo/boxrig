/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#ifndef BOXRIG_TERM_WIN32_ARGUMENTS_H
#define BOXRIG_TERM_WIN32_ARGUMENTS_H
#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

/** Requested renderer; auto prefers OpenGL ES/GL and falls back to software. */
typedef enum boxrig_term_win32_renderer {
    BOXRIG_TERM_WIN32_RENDERER_AUTO,
    BOXRIG_TERM_WIN32_RENDERER_SOFTWARE,
    BOXRIG_TERM_WIN32_RENDERER_OPENGL,
    BOXRIG_TERM_WIN32_RENDERER_D3D11
} boxrig_term_win32_renderer;

/** Explicit application transparency policy, independent of renderer identity. */
typedef enum boxrig_term_win32_transparency_mode {
    /** Try background alpha and retain opaque rendering when unavailable. */
    BOXRIG_TERM_WIN32_TRANSPARENCY_BACKGROUND,
    /** Apply a separate opacity to the entire window, with opaque rendering. */
    BOXRIG_TERM_WIN32_TRANSPARENCY_WINDOW,
    /** Disable transparency and opacity shortcuts. */
    BOXRIG_TERM_WIN32_TRANSPARENCY_OPAQUE
} boxrig_term_win32_transparency_mode;

/**
 * Parses the optional BOXRIG_TRANSPARENCY_MODE application setting.
 * @param value Environment value, or NULL for the background default.
 * @param result Receives the selected mode on success.
 * @return NULL on success, or a static diagnostic on invalid input.
 */
const char *boxrig_term_win32_parse_transparency_mode(const char *value,
    boxrig_term_win32_transparency_mode *result);

/**
 * Parses BOXRIG_TRANSPARENCY_FALLBACK independently of the selected mode.
 * @param value Environment value: window or opaque; NULL defaults to opaque.
 * @param result Receives whether whole-window substitution is explicitly allowed.
 * @return NULL on success, or a static diagnostic; invalid input leaves result unchanged.
 */
const char *boxrig_term_win32_parse_transparency_fallback(const char *value,
    bool *result);

/**
 * Parses the separate BOXRIG_WINDOW_OPACITY percentage.
 * @param value Environment value in 1..100, or NULL for 100 percent.
 * @param result Receives the validated percentage, unchanged on invalid input.
 * @return NULL on success, or a static diagnostic. Background zero remains valid
 * independently; newly requested whole-window zero is deliberately disallowed.
 */
const char *boxrig_term_win32_parse_window_opacity(const char *value,
    uint32_t *result);

/** Parsed leading options and a borrowed, unmodified child command suffix. */
typedef struct boxrig_term_win32_arguments {
    /** Requested renderer, independently of compiled availability. */
    boxrig_term_win32_renderer renderer;
    /** Raw UTF-16 child command, or an empty suffix for the default shell. */
    const wchar_t *command_line;
    /** Whether only usage information was requested. */
    bool help;
} boxrig_term_win32_arguments;

/**
 * Parses leading application options without reserializing child arguments.
 * @param command_line Raw wWinMain command line; must outlive the result.
 * @param result Receives options and the borrowed child suffix.
 * @return NULL on success, or a static diagnostic on invalid options.
 */
const char *boxrig_term_win32_parse_arguments(
    const wchar_t *command_line, boxrig_term_win32_arguments *result);

/** Returns the stable CLI spelling of a renderer selection. */
const char *boxrig_term_win32_renderer_name(boxrig_term_win32_renderer renderer);
#endif
