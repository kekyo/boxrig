/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_EDITABLE_PRIVATE_H
#define BOXRIG_UI_EDITABLE_PRIVATE_H

#include "boxrig/ui/editable.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

boxrig_ui_status boxrig_ui_editable_document_create(
    boxrig_terminal *terminal,
    boxrig_ui_editable_document **document_out);

void boxrig_ui_editable_document_destroy(
    boxrig_ui_editable_document *document);

bool boxrig_ui_editable_document_get_utf16_context(
    const boxrig_ui_editable_document *document,
    const uint16_t **context_out,
    size_t *length_out);

bool boxrig_ui_editable_document_utf8_to_utf16(
    const boxrig_ui_editable_document *document,
    size_t utf8_offset,
    size_t *utf16_offset_out);

bool boxrig_ui_editable_document_utf16_to_utf8(
    const boxrig_ui_editable_document *document,
    size_t utf16_offset,
    size_t *utf8_offset_out);

bool boxrig_ui_editable_document_get_reconversion_range(
    const boxrig_ui_editable_document *document,
    boxrig_ui_editable_range *range_out);

boxrig_ui_status boxrig_ui_editable_document_validate_reconversion_range(
    const boxrig_ui_editable_document *document,
    boxrig_ui_editable_range range);

#endif
