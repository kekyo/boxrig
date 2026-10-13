/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_UTF8_H
#define BOXRIG_UI_UTF8_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decodes one strict RFC 3629 scalar without modifying outputs on failure. */
bool boxrig_ui_utf8_decode_scalar(
    const uint8_t *text,
    size_t text_length,
    uint32_t *codepoint_out,
    size_t *text_length_out);

#endif
