/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_INPUT_TEXT_H
#define BOXRIG_UI_INPUT_TEXT_H

#include <stddef.h>
#include <stdint.h>

/**
 * Validates and normalizes untrusted UTF-8 clipboard text.
 *
 * @param bytes Length-delimited clipboard bytes.
 * @param length Number of bytes in bytes.
 * @param capacity Maximum accepted source and normalized lengths.
 * @param text_out Receives allocated normalized UTF-8.
 * @param length_out Receives the normalized byte length.
 * @return Zero on success, or -1 for invalid input or allocation failure.
 *
 * @remarks CRLF and CR become LF. Tab and LF are preserved, while other C0,
 * DEL, and C1 controls are removed. The caller owns text_out on success.
 */
int boxrig_input_text_normalize_utf8(
    const uint8_t *bytes,
    size_t length,
    size_t capacity,
    char **text_out,
    size_t *length_out);

/**
 * Chooses a complete UTF-8 prefix near a preferred paste chunk size.
 * @param text Valid, normalized UTF-8 text.
 * @param length Remaining number of bytes.
 * @param preferred Preferred nonzero byte count per chunk.
 * @return Longest complete prefix within preferred, or the first scalar if it
 * exceeds preferred. Returns zero for empty text, NULL, or zero preferred.
 * @remarks Input must already be validated. A scalar never exceeds four bytes.
 */
size_t boxrig_input_text_chunk_length(const char *text, size_t length, size_t preferred);

#endif
