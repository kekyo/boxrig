/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/editable.h"

#include "editable_private.h"
#include "utf8.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct editable_snapshot_candidate {
    char *context;
    uint16_t *utf16;
    uint32_t *utf8_to_utf16;
    uint32_t *utf16_to_utf8;
    uint8_t *grapheme_boundaries;
    size_t utf16_length;
} editable_snapshot_candidate;

struct boxrig_ui_editable_document {
    boxrig_terminal *terminal;
    boxrig_ui_editable_limits limits;
    boxrig_ui_editable_document_snapshot snapshot;
    uint16_t *utf16;
    size_t utf16_length;
    uint32_t *utf8_to_utf16;
    uint32_t *utf16_to_utf8;
    uint8_t *grapheme_boundaries;
    boxrig_ui_editable_reconversion_state state;
    uint64_t fixed_document_identity;
    uint64_t fixed_document_revision;
    uint64_t fixed_input_generation;
    uint64_t input_generation;
    boxrig_ui_editable_range fixed_range;
    boxrig_ui_editable_replacement_request request;
    char *replacement;
    uint64_t next_request_id;
    bool snapshot_active;
    bool request_active;
    bool request_taken;
    boxrig_ui_editable_apply_handler apply_handler;
    void *apply_context;
    bool applying;
};

static bool range_end(uint64_t offset, size_t length, uint64_t *end_out)
{
    if ((uint64_t)length > UINT64_MAX - offset) {
        return false;
    }
    *end_out = offset + (uint64_t)length;
    return true;
}

static bool valid_outcome(boxrig_ui_editable_replacement_outcome outcome)
{
    return outcome >= BOXRIG_UI_EDITABLE_REPLACEMENT_ACCEPTED &&
           outcome <= BOXRIG_UI_EDITABLE_REPLACEMENT_REJECTED;
}

static void candidate_destroy(editable_snapshot_candidate *candidate)
{
    free(candidate->context);
    free(candidate->utf16);
    free(candidate->utf8_to_utf16);
    free(candidate->utf16_to_utf8);
    free(candidate->grapheme_boundaries);
    memset(candidate, 0, sizeof(*candidate));
}

static void snapshot_storage_destroy(boxrig_ui_editable_document *document)
{
    free((char *)document->snapshot.context);
    free(document->utf16);
    free(document->utf8_to_utf16);
    free(document->utf16_to_utf8);
    free(document->grapheme_boundaries);
    memset(&document->snapshot, 0, sizeof(document->snapshot));
    document->utf16 = NULL;
    document->utf16_length = 0;
    document->utf8_to_utf16 = NULL;
    document->utf16_to_utf8 = NULL;
    document->grapheme_boundaries = NULL;
    document->snapshot_active = false;
}

static bool valid_limits(boxrig_ui_editable_limits limits)
{
    return limits.maximum_context_bytes <=
               BOXRIG_UI_EDITABLE_CONTEXT_CAPACITY_MAX &&
           limits.maximum_selection_bytes <=
               BOXRIG_UI_EDITABLE_SELECTION_CAPACITY_MAX &&
           limits.maximum_pending_replacements <=
               BOXRIG_UI_EDITABLE_PENDING_REPLACEMENTS_MAX;
}

static boxrig_ui_status validate_range(
    const boxrig_ui_editable_document *document,
    boxrig_ui_editable_range range)
{
    uint64_t context_end;
    uint64_t selection_end;
    size_t relative_start;
    size_t relative_end;

    if (!document->snapshot_active || range.length == 0 ||
        range.length > document->limits.maximum_selection_bytes ||
        !range_end(
            document->snapshot.context_offset,
            document->snapshot.context_length,
            &context_end) ||
        !range_end(range.offset, range.length, &selection_end) ||
        range.offset < document->snapshot.context_offset ||
        selection_end > context_end) {
        return range.length > document->limits.maximum_selection_bytes
                   ? BOXRIG_UI_LIMIT_EXCEEDED
                   : BOXRIG_UI_INVALID_ARGUMENT;
    }
    relative_start =
        (size_t)(range.offset - document->snapshot.context_offset);
    relative_end =
        (size_t)(selection_end - document->snapshot.context_offset);
    if (!document->grapheme_boundaries[relative_start] ||
        !document->grapheme_boundaries[relative_end]) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return BOXRIG_UI_OK;
}

static boxrig_ui_status build_snapshot_candidate(
    boxrig_ui_editable_document *document,
    const boxrig_ui_editable_document_snapshot_input *input,
    editable_snapshot_candidate *candidate)
{
    size_t offset = 0;
    size_t utf16_offset = 0;

    memset(candidate, 0, sizeof(*candidate));
    if (input->context_length > document->limits.maximum_context_bytes ||
        input->selection.length > document->limits.maximum_selection_bytes) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    if (input->context_length == SIZE_MAX ||
        input->context_length > UINT32_MAX - 1u) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    candidate->context = malloc(input->context_length + 1);
    candidate->utf16 = malloc(
        (input->context_length + 1) * sizeof(*candidate->utf16));
    candidate->utf8_to_utf16 = malloc(
        (input->context_length + 1) *
        sizeof(*candidate->utf8_to_utf16));
    candidate->utf16_to_utf8 = malloc(
        (input->context_length + 1) *
        sizeof(*candidate->utf16_to_utf8));
    candidate->grapheme_boundaries = calloc(
        input->context_length + 1,
        sizeof(*candidate->grapheme_boundaries));
    if (candidate->context == NULL || candidate->utf16 == NULL ||
        candidate->utf8_to_utf16 == NULL ||
        candidate->utf16_to_utf8 == NULL ||
        candidate->grapheme_boundaries == NULL) {
        candidate_destroy(candidate);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    if (input->context_length != 0) {
        memcpy(candidate->context, input->context, input->context_length);
    }
    candidate->context[input->context_length] = '\0';
    memset(
        candidate->utf8_to_utf16,
        0xff,
        (input->context_length + 1) *
            sizeof(*candidate->utf8_to_utf16));
    memset(
        candidate->utf16_to_utf8,
        0xff,
        (input->context_length + 1) *
            sizeof(*candidate->utf16_to_utf8));
    while (offset < input->context_length) {
        uint32_t codepoint;
        size_t scalar_length;

        if (!boxrig_ui_utf8_decode_scalar(
                (const uint8_t *)input->context + offset,
                input->context_length - offset,
                &codepoint,
                &scalar_length)) {
            candidate_destroy(candidate);
            return BOXRIG_UI_INVALID_ARGUMENT;
        }
        candidate->utf8_to_utf16[offset] = (uint32_t)utf16_offset;
        candidate->utf16_to_utf8[utf16_offset] = (uint32_t)offset;
        if (codepoint <= UINT32_C(0xffff)) {
            candidate->utf16[utf16_offset++] = (uint16_t)codepoint;
        } else {
            uint32_t value = codepoint - UINT32_C(0x10000);

            candidate->utf16[utf16_offset++] =
                (uint16_t)(UINT32_C(0xd800) + (value >> 10));
            candidate->utf16[utf16_offset++] =
                (uint16_t)(UINT32_C(0xdc00) + (value & UINT32_C(0x3ff)));
        }
        offset += scalar_length;
    }
    candidate->utf8_to_utf16[input->context_length] =
        (uint32_t)utf16_offset;
    candidate->utf16_to_utf8[utf16_offset] =
        (uint32_t)input->context_length;
    candidate->utf16[utf16_offset] = 0;
    candidate->utf16_length = utf16_offset;

    offset = 0;
    candidate->grapheme_boundaries[0] = 1;
    while (offset < input->context_length) {
        boxrig_text_cluster_measurement measurement;

        if (boxrig_terminal_measure_text_cluster(
                document->terminal,
                (const uint8_t *)input->context + offset,
                input->context_length - offset,
                &measurement) != BOXRIG_STATUS_OK ||
            measurement.text_length == 0 || measurement.split_by_limit) {
            candidate_destroy(candidate);
            return BOXRIG_UI_INVALID_ARGUMENT;
        }
        offset += measurement.text_length;
        candidate->grapheme_boundaries[offset] = 1;
    }
    return BOXRIG_UI_OK;
}

static bool same_snapshot(
    const boxrig_ui_editable_document *document,
    const boxrig_ui_editable_document_snapshot_input *input,
    uint64_t cursor,
    uint64_t anchor)
{
    return document->snapshot.context_offset == input->context_offset &&
           document->snapshot.context_length == input->context_length &&
           document->snapshot.selection.offset == input->selection.offset &&
           document->snapshot.selection.length == input->selection.length &&
           document->snapshot.cursor == cursor &&
           document->snapshot.anchor == anchor &&
           (input->context_length == 0 || memcmp(
               document->snapshot.context,
               input->context,
               input->context_length) == 0);
}

/* A window may move within an unchanged revision. Its overlap must still
 * describe the same bytes; selection changes do not change the body. */
static bool same_body_overlap(
    const boxrig_ui_editable_document *document,
    const boxrig_ui_editable_document_snapshot_input *input)
{
    const boxrig_ui_editable_document_snapshot *old = &document->snapshot;
    uint64_t start = old->context_offset > input->context_offset
        ? old->context_offset : input->context_offset;
    uint64_t old_end = old->context_offset + old->context_length;
    uint64_t new_end = input->context_offset + input->context_length;
    uint64_t end = old_end < new_end ? old_end : new_end;
    return start >= end || memcmp(old->context + (size_t)(start - old->context_offset),
        input->context + (size_t)(start - input->context_offset),
        (size_t)(end - start)) == 0;
}

boxrig_ui_status boxrig_ui_editable_document_invalidate_input(
    boxrig_ui_editable_document *document)
{
    if (document == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (document->applying) return BOXRIG_UI_BUSY;
    if (document->input_generation == UINT64_MAX) return BOXRIG_UI_LIMIT_EXCEEDED;
    document->input_generation++;
    document->snapshot.input_generation = document->input_generation;
    if (document->state == BOXRIG_UI_EDITABLE_RECONVERSION_ACTIVE)
        document->state = BOXRIG_UI_EDITABLE_RECONVERSION_STALE;
    return BOXRIG_UI_OK;
}

uint64_t boxrig_ui_editable_document_input_generation(
    const boxrig_ui_editable_document *document)
{
    return document == NULL ? 0 : document->input_generation;
}

static void install_candidate(
    boxrig_ui_editable_document *document,
    const boxrig_ui_editable_document_snapshot_input *input,
    uint64_t cursor,
    uint64_t anchor,
    editable_snapshot_candidate *candidate)
{
    snapshot_storage_destroy(document);
    document->snapshot.document_identity = input->document_identity;
    document->snapshot.document_revision = input->document_revision;
    document->snapshot.context = candidate->context;
    document->snapshot.context_length = input->context_length;
    document->snapshot.context_offset = input->context_offset;
    document->snapshot.selection = input->selection;
    document->snapshot.cursor = cursor;
    document->snapshot.anchor = anchor;
    document->snapshot.input_generation = document->input_generation;
    document->utf16 = candidate->utf16;
    document->utf16_length = candidate->utf16_length;
    document->utf8_to_utf16 = candidate->utf8_to_utf16;
    document->utf16_to_utf8 = candidate->utf16_to_utf8;
    document->grapheme_boundaries = candidate->grapheme_boundaries;
    document->snapshot_active = true;
    memset(candidate, 0, sizeof(*candidate));
}

static bool valid_utf8(const char *text, size_t text_length)
{
    size_t offset = 0;

    if (text == NULL && text_length != 0) {
        return false;
    }
    while (offset < text_length) {
        uint32_t codepoint;
        size_t scalar_length;

        if (!boxrig_ui_utf8_decode_scalar(
                (const uint8_t *)text + offset,
                text_length - offset,
                &codepoint,
                &scalar_length)) {
            return false;
        }
        offset += scalar_length;
    }
    return true;
}

boxrig_ui_editable_limits boxrig_ui_editable_limits_default(void)
{
    boxrig_ui_editable_limits limits;

    limits.maximum_context_bytes =
        BOXRIG_UI_EDITABLE_CONTEXT_CAPACITY_DEFAULT;
    limits.maximum_selection_bytes =
        BOXRIG_UI_EDITABLE_SELECTION_CAPACITY_DEFAULT;
    limits.maximum_pending_replacements =
        BOXRIG_UI_EDITABLE_PENDING_REPLACEMENTS_DEFAULT;
    return limits;
}

boxrig_ui_status boxrig_ui_editable_document_create(
    boxrig_terminal *terminal,
    boxrig_ui_editable_document **document_out)
{
    boxrig_ui_editable_document *document;

    if (terminal == NULL || document_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *document_out = NULL;
    document = calloc(1, sizeof(*document));
    if (document == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    document->terminal = terminal;
    document->limits = boxrig_ui_editable_limits_default();
    document->next_request_id = 1;
    document->input_generation = 1;
    *document_out = document;
    return BOXRIG_UI_OK;
}

void boxrig_ui_editable_document_destroy(
    boxrig_ui_editable_document *document)
{
    if (document == NULL) {
        return;
    }
    snapshot_storage_destroy(document);
    free(document->replacement);
    free(document);
}

boxrig_ui_status boxrig_ui_editable_document_set_limits(
    boxrig_ui_editable_document *document,
    boxrig_ui_editable_limits limits)
{
    if (document == NULL || !valid_limits(limits)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (document->applying) return BOXRIG_UI_BUSY;
    if ((document->snapshot_active &&
         (document->snapshot.context_length > limits.maximum_context_bytes ||
          document->snapshot.selection.length >
              limits.maximum_selection_bytes)) ||
        (document->request_active &&
         (limits.maximum_pending_replacements == 0 ||
          document->request.replacement_length >
              limits.maximum_selection_bytes))) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    document->limits = limits;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status publish_snapshot(
    boxrig_ui_editable_document *document,
    const boxrig_ui_editable_document_snapshot_input *input,
    uint64_t cursor,
    uint64_t anchor,
    bool require_graphemes)
{
    editable_snapshot_candidate candidate;
    boxrig_ui_status status;
    uint64_t context_end;
    uint64_t selection_end;
    size_t relative_start;
    size_t relative_end;

    if (document != NULL && document->applying) return BOXRIG_UI_BUSY;
    if (document == NULL || input == NULL || input->document_identity == 0 ||
        input->document_revision == 0 ||
        (input->context == NULL && input->context_length != 0) ||
        !range_end(input->context_offset, input->context_length, &context_end) ||
        !range_end(
            input->selection.offset,
            input->selection.length,
            &selection_end) ||
        input->selection.offset < input->context_offset ||
        selection_end > context_end) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (document->snapshot_active &&
        document->snapshot.document_identity == input->document_identity &&
        document->snapshot.document_revision == input->document_revision) {
        if (same_snapshot(document, input, cursor, anchor)) {
            size_t first = (size_t)(input->selection.offset - input->context_offset);
            size_t last = first + input->selection.length;
            if (require_graphemes && (!document->grapheme_boundaries[first] ||
                !document->grapheme_boundaries[last])) return BOXRIG_UI_INVALID_ARGUMENT;
            return BOXRIG_UI_OK;
        }
        if (!same_body_overlap(document, input)) return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (document->input_generation == UINT64_MAX) return BOXRIG_UI_LIMIT_EXCEEDED;
    status = build_snapshot_candidate(document, input, &candidate);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    relative_start = (size_t)(input->selection.offset - input->context_offset);
    relative_end = (size_t)(selection_end - input->context_offset);
    if (candidate.utf8_to_utf16[relative_start] == UINT32_MAX ||
        candidate.utf8_to_utf16[relative_end] == UINT32_MAX ||
        (require_graphemes && (!candidate.grapheme_boundaries[relative_start] ||
            !candidate.grapheme_boundaries[relative_end]))) {
        candidate_destroy(&candidate);
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    (void)boxrig_ui_editable_document_invalidate_input(document);
    install_candidate(document, input, cursor, anchor, &candidate);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_editable_document_publish(
    boxrig_ui_editable_document *document,
    const boxrig_ui_editable_document_snapshot_input *input)
{
    uint64_t cursor;
    if (input == NULL ||
        !range_end(input->selection.offset, input->selection.length, &cursor))
        return BOXRIG_UI_INVALID_ARGUMENT;
    return publish_snapshot(document, input, cursor, input->selection.offset, true);
}

boxrig_ui_status boxrig_ui_editable_document_publish_context(
    boxrig_ui_editable_document *document,
    const boxrig_ui_editable_context_input *input)
{
    uint64_t end;
    if (input == NULL ||
        !range_end(input->context_offset, input->context_length, &end) ||
        input->cursor < input->context_offset || input->cursor > end ||
        input->anchor < input->context_offset || input->anchor > end)
        return BOXRIG_UI_INVALID_ARGUMENT;
    uint64_t first = input->cursor < input->anchor ? input->cursor : input->anchor;
    uint64_t last = input->cursor > input->anchor ? input->cursor : input->anchor;
    boxrig_ui_editable_document_snapshot_input snapshot = {
        input->document_identity, input->document_revision, input->context,
        input->context_length, input->context_offset, {first, (size_t)(last - first)}};
    return publish_snapshot(document, &snapshot, input->cursor, input->anchor, false);
}

boxrig_ui_status boxrig_ui_editable_document_clear(
    boxrig_ui_editable_document *document)
{
    if (document == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    boxrig_ui_status status = boxrig_ui_editable_document_invalidate_input(document);
    if (status != BOXRIG_UI_OK) return status;
    snapshot_storage_destroy(document);
    return BOXRIG_UI_OK;
}

bool boxrig_ui_editable_document_get_snapshot(
    const boxrig_ui_editable_document *document,
    boxrig_ui_editable_document_snapshot *snapshot_out)
{
    if (document == NULL || snapshot_out == NULL ||
        !document->snapshot_active) {
        return false;
    }
    *snapshot_out = document->snapshot;
    return true;
}

boxrig_ui_status boxrig_ui_editable_document_get_context(
    const boxrig_ui_editable_document *document,
    size_t maximum_bytes,
    boxrig_ui_editable_document_snapshot *snapshot_out)
{
    if (document == NULL || snapshot_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (!document->snapshot_active) return BOXRIG_UI_UNAVAILABLE;
    const boxrig_ui_editable_document_snapshot *snapshot = &document->snapshot;
    size_t length = snapshot->context_length;
    if (length <= maximum_bytes) {
        *snapshot_out = *snapshot;
        return BOXRIG_UI_OK;
    }
    size_t first = (size_t)(snapshot->selection.offset - snapshot->context_offset);
    size_t last = first + snapshot->selection.length;
    while (!document->grapheme_boundaries[first]) first--;
    while (!document->grapheme_boundaries[last]) last++;
    if (last - first > maximum_bytes) return BOXRIG_UI_LIMIT_EXCEEDED;
    size_t before = (maximum_bytes - (last - first)) / 2;
    size_t start = before < first ? first - before : 0;
    while (!document->grapheme_boundaries[start]) start++;
    size_t end = maximum_bytes < length - start ? start + maximum_bytes : length;
    while (!document->grapheme_boundaries[end]) end--;
    *snapshot_out = *snapshot;
    snapshot_out->context += start;
    snapshot_out->context_length = end - start;
    snapshot_out->context_offset += start;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_editable_document_begin_reconversion(
    boxrig_ui_editable_document *document)
{
    if (document == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (document->applying) return BOXRIG_UI_BUSY;
    if (!document->snapshot_active ||
        validate_range(document, document->snapshot.selection) != BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (document->state != BOXRIG_UI_EDITABLE_RECONVERSION_NONE ||
        document->request_active) {
        return BOXRIG_UI_BUSY;
    }
    document->fixed_document_identity =
        document->snapshot.document_identity;
    document->fixed_document_revision =
        document->snapshot.document_revision;
    document->fixed_input_generation = document->input_generation;
    document->fixed_range = document->snapshot.selection;
    document->state = BOXRIG_UI_EDITABLE_RECONVERSION_ACTIVE;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_editable_document_confirm_reconversion_range(
    boxrig_ui_editable_document *document,
    boxrig_ui_editable_range range)
{
    boxrig_ui_status status;

    if (document == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (document->applying) return BOXRIG_UI_BUSY;
    if (document->state == BOXRIG_UI_EDITABLE_RECONVERSION_STALE) {
        return BOXRIG_UI_STALE;
    }
    if (document->state != BOXRIG_UI_EDITABLE_RECONVERSION_ACTIVE ||
        !document->snapshot_active ||
        document->fixed_document_identity !=
            document->snapshot.document_identity ||
        document->fixed_document_revision !=
            document->snapshot.document_revision) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = validate_range(document, range);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    document->fixed_range = range;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_editable_document_cancel_reconversion(
    boxrig_ui_editable_document *document)
{
    if (document == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (document->applying) return BOXRIG_UI_BUSY;
    if (document->state ==
        BOXRIG_UI_EDITABLE_RECONVERSION_AWAITING_COMPLETION) {
        return BOXRIG_UI_BUSY;
    }
    document->state = BOXRIG_UI_EDITABLE_RECONVERSION_NONE;
    document->fixed_document_identity = 0;
    document->fixed_document_revision = 0;
    document->fixed_input_generation = 0;
    memset(&document->fixed_range, 0, sizeof(document->fixed_range));
    return BOXRIG_UI_OK;
}

boxrig_status boxrig_ui_editable_document_commit_reconversion(
    const char *text,
    size_t text_length,
    void *context)
{
    boxrig_ui_editable_document *document = context;
    char *copy = NULL;

    if (document == NULL || !valid_utf8(text, text_length)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (document->applying) return BOXRIG_STATUS_WOULD_BLOCK;
    if (text_length > document->limits.maximum_selection_bytes) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (document->state == BOXRIG_UI_EDITABLE_RECONVERSION_STALE) {
        return BOXRIG_STATUS_STALE;
    }
    if (document->state != BOXRIG_UI_EDITABLE_RECONVERSION_ACTIVE ||
        document->request_active) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (document->limits.maximum_pending_replacements == 0) {
        return BOXRIG_STATUS_WOULD_BLOCK;
    }
    if (text_length != 0) {
        copy = malloc(text_length);
        if (copy == NULL) {
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
        memcpy(copy, text, text_length);
    }
    document->request.request_id = document->next_request_id++;
    if (document->next_request_id == 0) {
        document->next_request_id = 1;
    }
    document->request.document_identity =
        document->fixed_document_identity;
    document->request.base_document_revision =
        document->fixed_document_revision;
    document->request.base_input_generation = document->fixed_input_generation;
    document->request.range = document->fixed_range;
    document->request.replacement = copy;
    document->request.replacement_length = text_length;
    document->replacement = copy;
    document->request_active = true;
    document->request_taken = false;
    document->state =
        BOXRIG_UI_EDITABLE_RECONVERSION_AWAITING_COMPLETION;
    return BOXRIG_STATUS_OK;
}

bool boxrig_ui_editable_document_take_replacement(
    boxrig_ui_editable_document *document,
    boxrig_ui_editable_replacement_request *request_out)
{
    if (document == NULL || request_out == NULL ||
        !document->request_active || document->request_taken) {
        return false;
    }
    *request_out = document->request;
    document->request_taken = true;
    return true;
}

boxrig_ui_status boxrig_ui_editable_document_complete_replacement(
    boxrig_ui_editable_document *document,
    const boxrig_ui_editable_replacement_completion *completion)
{
    bool snapshot_is_base;

    if (document == NULL || completion == NULL ||
        !valid_outcome(completion->outcome)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (document->applying) return BOXRIG_UI_BUSY;
    if (!document->request_active || !document->request_taken ||
        completion->request_id != document->request.request_id) {
        return BOXRIG_UI_STALE;
    }
    if ((completion->outcome == BOXRIG_UI_EDITABLE_REPLACEMENT_ACCEPTED &&
         (completion->new_document_revision == 0 ||
          completion->new_document_revision ==
              document->request.base_document_revision)) ||
        (completion->outcome != BOXRIG_UI_EDITABLE_REPLACEMENT_ACCEPTED &&
         completion->new_document_revision != 0)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    snapshot_is_base =
        document->snapshot_active &&
        document->snapshot.document_identity ==
            document->request.document_identity &&
        document->snapshot.document_revision ==
            document->request.base_document_revision;
    free(document->replacement);
    document->replacement = NULL;
    memset(&document->request, 0, sizeof(document->request));
    document->request_active = false;
    document->request_taken = false;
    document->state = BOXRIG_UI_EDITABLE_RECONVERSION_NONE;
    document->fixed_document_identity = 0;
    document->fixed_document_revision = 0;
    document->fixed_input_generation = 0;
    memset(&document->fixed_range, 0, sizeof(document->fixed_range));
    if (completion->outcome == BOXRIG_UI_EDITABLE_REPLACEMENT_ACCEPTED &&
        snapshot_is_base) {
        (void)boxrig_ui_editable_document_invalidate_input(document);
        snapshot_storage_destroy(document);
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_editable_reconversion_state
boxrig_ui_editable_document_reconversion_state(
    const boxrig_ui_editable_document *document)
{
    return document == NULL ? BOXRIG_UI_EDITABLE_RECONVERSION_NONE
                            : document->state;
}

boxrig_ui_status boxrig_ui_editable_document_set_apply_handler(
    boxrig_ui_editable_document *document,
    boxrig_ui_editable_apply_handler handler,
    void *context)
{
    if (document == NULL || (handler == NULL && context != NULL))
        return BOXRIG_UI_INVALID_ARGUMENT;
    if (document->applying) return BOXRIG_UI_BUSY;
    if (handler == document->apply_handler && context == document->apply_context)
        return BOXRIG_UI_OK;
    boxrig_ui_status status = boxrig_ui_editable_document_invalidate_input(document);
    if (status != BOXRIG_UI_OK) return status;
    document->apply_handler = handler;
    document->apply_context = context;
    return BOXRIG_UI_OK;
}

boxrig_ui_editable_capabilities boxrig_ui_editable_document_capabilities(
    const boxrig_ui_editable_document *document)
{
    boxrig_ui_editable_capabilities result = {0};
    if (document == NULL) return result;
    result.context_available = document->snapshot_active;
    result.synchronous_edit_available = document->snapshot_active &&
        document->apply_handler != NULL;
    result.busy = document->applying || document->request_active ||
        document->state != BOXRIG_UI_EDITABLE_RECONVERSION_NONE;
    result.reconversion_available = !result.busy && document->snapshot_active &&
        validate_range(document, document->snapshot.selection) == BOXRIG_UI_OK;
    return result;
}

boxrig_ui_status boxrig_ui_editable_document_apply_edit(
    boxrig_ui_editable_document *document,
    const boxrig_ui_editable_edit *edit)
{
    if (document == NULL || edit == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (document->applying) return BOXRIG_UI_BUSY;
    if (edit->input_generation != document->input_generation) return BOXRIG_UI_STALE;
    if (!document->snapshot_active || document->apply_handler == NULL)
        return BOXRIG_UI_UNAVAILABLE;
    if (document->request_active || document->state != BOXRIG_UI_EDITABLE_RECONVERSION_NONE)
        return BOXRIG_UI_BUSY;

    const boxrig_ui_editable_document_snapshot *old = &document->snapshot;
    uint64_t end;
    if (!range_end(edit->range.offset, edit->range.length, &end) ||
        edit->range.offset < old->context_offset ||
        end > old->context_offset + old->context_length ||
        !valid_utf8(edit->replacement, edit->replacement_length))
        return BOXRIG_UI_INVALID_ARGUMENT;
    size_t requested_first = (size_t)(edit->range.offset - old->context_offset);
    size_t requested_last = requested_first + edit->range.length;
    if (document->utf8_to_utf16[requested_first] == UINT32_MAX ||
        document->utf8_to_utf16[requested_last] == UINT32_MAX)
        return BOXRIG_UI_INVALID_ARGUMENT;
    if (edit->replacement_length > document->limits.maximum_selection_bytes ||
        old->document_revision == UINT64_MAX || document->input_generation == UINT64_MAX ||
        document->next_request_id == UINT64_MAX)
        return BOXRIG_UI_LIMIT_EXCEEDED;

    size_t first = requested_first, last = requested_last;
    while (!document->grapheme_boundaries[first]) first--;
    while (!document->grapheme_boundaries[last]) last++;
    size_t prefix = requested_first - first, suffix = last - requested_last;
    size_t replacement_length = prefix + edit->replacement_length + suffix;
    size_t length = old->context_length - edit->range.length + edit->replacement_length;
    if (last - first > document->limits.maximum_selection_bytes ||
        replacement_length > document->limits.maximum_selection_bytes ||
        length > document->limits.maximum_context_bytes)
        return BOXRIG_UI_LIMIT_EXCEEDED;
    if (!range_end(old->context_offset, length, &end) ||
        edit->cursor_after < old->context_offset || edit->cursor_after > end ||
        edit->anchor_after < old->context_offset || edit->anchor_after > end)
        return BOXRIG_UI_INVALID_ARGUMENT;
    uint64_t selected_first = edit->cursor_after < edit->anchor_after
        ? edit->cursor_after : edit->anchor_after;
    uint64_t selected_last = edit->cursor_after > edit->anchor_after
        ? edit->cursor_after : edit->anchor_after;
    if (selected_last - selected_first > document->limits.maximum_selection_bytes)
        return BOXRIG_UI_LIMIT_EXCEEDED;

    /* Construct both the owner's normalized edit and the complete next cache
     * before invoking application code. Nothing fallible follows acceptance. */
    char *replacement = malloc(replacement_length + 1);
    char *text = malloc(length + 1);
    if (replacement == NULL || text == NULL) {
        free(replacement);
        free(text);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    memcpy(replacement, old->context + first, prefix);
    if (edit->replacement_length != 0)
        memcpy(replacement + prefix, edit->replacement, edit->replacement_length);
    memcpy(replacement + prefix + edit->replacement_length,
        old->context + requested_last, suffix);
    replacement[replacement_length] = '\0';
    memcpy(text, old->context, first);
    memcpy(text + first, replacement, replacement_length);
    memcpy(text + first + replacement_length, old->context + last,
        old->context_length - last);
    text[length] = '\0';
    boxrig_ui_editable_document_snapshot_input input = {
        old->document_identity, old->document_revision + 1, text, length,
        old->context_offset, {selected_first, (size_t)(selected_last - selected_first)}};
    editable_snapshot_candidate candidate;
    boxrig_ui_status status = build_snapshot_candidate(document, &input, &candidate);
    if (status == BOXRIG_UI_OK &&
        (candidate.utf8_to_utf16[(size_t)(edit->cursor_after - old->context_offset)] == UINT32_MAX ||
         candidate.utf8_to_utf16[(size_t)(edit->anchor_after - old->context_offset)] == UINT32_MAX))
        status = BOXRIG_UI_INVALID_ARGUMENT;
    if (status == BOXRIG_UI_OK) {
        boxrig_ui_editable_replacement_request request = {
            .request_id = document->next_request_id++,
            .document_identity = old->document_identity,
            .base_document_revision = old->document_revision,
            .base_input_generation = document->input_generation,
            .range = {old->context_offset + first, last - first},
            .replacement = replacement,
            .replacement_length = replacement_length};
        boxrig_ui_editable_document_snapshot proposed = {
            .document_identity = input.document_identity,
            .document_revision = input.document_revision,
            .context = candidate.context,
            .context_length = length,
            .context_offset = input.context_offset,
            .selection = input.selection,
            .cursor = edit->cursor_after,
            .anchor = edit->anchor_after,
            .input_generation = document->input_generation + 1};
        document->applying = true;
        status = document->apply_handler(&request, &proposed, document->apply_context);
        document->applying = false;
        if (status == BOXRIG_UI_OK) {
            (void)boxrig_ui_editable_document_invalidate_input(document);
            install_candidate(document, &input, edit->cursor_after, edit->anchor_after, &candidate);
        }
    }
    candidate_destroy(&candidate);
    free(text);
    free(replacement);
    return status;
}

bool boxrig_ui_editable_document_get_utf16_context(
    const boxrig_ui_editable_document *document,
    const uint16_t **context_out,
    size_t *length_out)
{
    if (document == NULL || context_out == NULL || length_out == NULL ||
        !document->snapshot_active) {
        return false;
    }
    *context_out = document->utf16;
    *length_out = document->utf16_length;
    return true;
}

bool boxrig_ui_editable_document_utf8_to_utf16(
    const boxrig_ui_editable_document *document,
    size_t utf8_offset,
    size_t *utf16_offset_out)
{
    uint32_t mapped;

    if (document == NULL || utf16_offset_out == NULL ||
        !document->snapshot_active ||
        utf8_offset > document->snapshot.context_length) {
        return false;
    }
    mapped = document->utf8_to_utf16[utf8_offset];
    if (mapped == UINT32_MAX) {
        return false;
    }
    *utf16_offset_out = mapped;
    return true;
}

bool boxrig_ui_editable_document_utf16_to_utf8(
    const boxrig_ui_editable_document *document,
    size_t utf16_offset,
    size_t *utf8_offset_out)
{
    uint32_t mapped;

    if (document == NULL || utf8_offset_out == NULL ||
        !document->snapshot_active || utf16_offset > document->utf16_length) {
        return false;
    }
    mapped = document->utf16_to_utf8[utf16_offset];
    if (mapped == UINT32_MAX) {
        return false;
    }
    *utf8_offset_out = mapped;
    return true;
}

bool boxrig_ui_editable_document_get_reconversion_range(
    const boxrig_ui_editable_document *document,
    boxrig_ui_editable_range *range_out)
{
    if (document == NULL || range_out == NULL ||
        (document->state != BOXRIG_UI_EDITABLE_RECONVERSION_ACTIVE &&
         document->state !=
             BOXRIG_UI_EDITABLE_RECONVERSION_AWAITING_COMPLETION)) {
        return false;
    }
    *range_out = document->fixed_range;
    return true;
}

boxrig_ui_status boxrig_ui_editable_document_validate_reconversion_range(
    const boxrig_ui_editable_document *document,
    boxrig_ui_editable_range range)
{
    if (document == NULL ||
        document->state != BOXRIG_UI_EDITABLE_RECONVERSION_ACTIVE ||
        !document->snapshot_active ||
        document->fixed_document_identity !=
            document->snapshot.document_identity ||
        document->fixed_document_revision !=
            document->snapshot.document_revision) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return validate_range(document, range);
}
