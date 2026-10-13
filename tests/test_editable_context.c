/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t allocations_before_failure = SIZE_MAX;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);

void *__wrap_malloc(size_t size)
{
    if (allocations_before_failure == 0) return NULL;
    if (allocations_before_failure != SIZE_MAX) allocations_before_failure--;
    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (allocations_before_failure == 0) return NULL;
    if (allocations_before_failure != SIZE_MAX) allocations_before_failure--;
    return __real_calloc(count, size);
}

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2;
    config.columns = 20;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(view);
    boxrig_ui_editable_document_snapshot_input input = {
        .document_identity = 1,
        .document_revision = 1,
        .context = "",
        .context_length = 0,
        .context_offset = 100,
        .selection = {100, 0}};
    assert(boxrig_ui_editable_document_publish(document, &input) == BOXRIG_UI_OK);
    boxrig_ui_editable_document_snapshot snapshot;
    assert(boxrig_ui_editable_document_get_snapshot(document, &snapshot));
    assert(snapshot.context_length == 0 && snapshot.selection.length == 0);
    assert(boxrig_ui_editable_document_begin_reconversion(document) ==
        BOXRIG_UI_UNAVAILABLE);
    assert(snapshot.cursor == 100 && snapshot.anchor == 100);
    uint64_t empty_generation = snapshot.input_generation;
    assert(empty_generation != 0);
    assert(boxrig_ui_editable_document_publish(document, &input) == BOXRIG_UI_OK);
    assert(boxrig_ui_editable_document_input_generation(document) == empty_generation);

    char text[] = "A日😀e\xcc\x81\nZ";
    boxrig_ui_editable_context_input context = {
        .document_identity = 2,
        .document_revision = 5,
        .context = text,
        .context_length = sizeof(text) - 1,
        .context_offset = 1000,
        .cursor = 1001,
        .anchor = 1008};
    assert(boxrig_ui_editable_document_publish_context(document, &context) == BOXRIG_UI_OK);
    assert(boxrig_ui_editable_document_get_snapshot(document, &snapshot));
    assert(snapshot.cursor == 1001 && snapshot.anchor == 1008);
    assert(snapshot.selection.offset == 1001 && snapshot.selection.length == 7);
    assert(snapshot.input_generation > empty_generation);
    text[0] = 'B';
    assert(snapshot.context[0] == 'A');
    assert(boxrig_ui_editable_document_publish_context(document, &context) ==
        BOXRIG_UI_INVALID_ARGUMENT);
    text[0] = 'A';
    uint64_t selected_generation = snapshot.input_generation;
    assert(boxrig_ui_editable_document_begin_reconversion(document) == BOXRIG_UI_OK);
    context.cursor = context.anchor = 1004;
    assert(boxrig_ui_editable_document_publish_context(document, &context) == BOXRIG_UI_OK);
    assert(boxrig_ui_editable_document_get_snapshot(document, &snapshot));
    assert(snapshot.document_revision == 5);
    assert(snapshot.input_generation > selected_generation);
    assert(snapshot.selection.length == 0 && snapshot.selection.offset == 1004);
    assert(boxrig_ui_editable_document_reconversion_state(document) ==
        BOXRIG_UI_EDITABLE_RECONVERSION_STALE);
    assert(boxrig_ui_editable_document_commit_reconversion("wrong", 5, document) ==
        BOXRIG_STATUS_STALE);
    assert(boxrig_ui_editable_document_cancel_reconversion(document) == BOXRIG_UI_OK);

    uint64_t generation = snapshot.input_generation;
    context.cursor = 1002;
    assert(boxrig_ui_editable_document_publish_context(document, &context) ==
        BOXRIG_UI_INVALID_ARGUMENT);
    assert(boxrig_ui_editable_document_input_generation(document) == generation);
    context.cursor = context.anchor = 1009; /* Between a base and combining mark. */
    assert(boxrig_ui_editable_document_publish_context(document, &context) == BOXRIG_UI_OK);
    assert(boxrig_ui_editable_document_begin_reconversion(document) == BOXRIG_UI_UNAVAILABLE);
    boxrig_ui_editable_document_snapshot_input grapheme_input = {
        context.document_identity, context.document_revision,
        context.context, context.context_length, context.context_offset,
        {context.cursor, 0}};
    assert(boxrig_ui_editable_document_publish(document, &grapheme_input) ==
        BOXRIG_UI_INVALID_ARGUMENT);
    context.cursor = 1008;
    context.anchor = 1004;
    assert(boxrig_ui_editable_document_publish_context(document, &context) == BOXRIG_UI_OK);
    assert(boxrig_ui_editable_document_get_context(document, 4, &snapshot) == BOXRIG_UI_OK);
    assert(snapshot.context_offset == 1004 && snapshot.context_length == 4);
    assert(memcmp(snapshot.context, "😀", 4) == 0);
    assert(snapshot.cursor == 1008 && snapshot.anchor == 1004);
    assert(boxrig_ui_editable_document_get_context(document, 3, &snapshot) ==
        BOXRIG_UI_LIMIT_EXCEEDED);
    assert(boxrig_ui_editable_document_get_context(document, 7, &snapshot) == BOXRIG_UI_OK);
    assert(snapshot.context_length <= 7 && snapshot.context_offset <= context.anchor);
    assert(snapshot.context_offset + snapshot.context_length >= context.cursor);
    assert(snapshot.context_length == 7 && memcmp(snapshot.context, "😀e\xcc\x81", 7) == 0);

    /* Window changes preserve the body revision, but may not contradict bytes
     * that overlap the previously published window of the same revision. */
    context.context++;
    context.context_length--;
    context.context_offset++;
    assert(boxrig_ui_editable_document_publish_context(document, &context) == BOXRIG_UI_OK);
    assert(boxrig_ui_editable_document_get_snapshot(document, &snapshot));
    assert(snapshot.context_length == sizeof(text) - 2);
    generation = snapshot.input_generation;
    context.document_revision++;
    for (size_t fail_at = 0; fail_at < 5; fail_at++) {
        allocations_before_failure = fail_at;
        assert(boxrig_ui_editable_document_publish_context(document, &context) ==
            BOXRIG_UI_OUT_OF_MEMORY);
        allocations_before_failure = SIZE_MAX;
        assert(boxrig_ui_editable_document_get_snapshot(document, &snapshot));
        assert(snapshot.document_revision == 5 && snapshot.input_generation == generation);
    }
    boxrig_ui_editable_limits limits = boxrig_ui_editable_limits_default();
    limits.maximum_context_bytes = context.context_length;
    limits.maximum_selection_bytes = 4;
    assert(boxrig_ui_editable_document_set_limits(document, limits) == BOXRIG_UI_OK);
    context.anchor = context.context_offset;
    assert(boxrig_ui_editable_document_publish_context(document, &context) ==
        BOXRIG_UI_LIMIT_EXCEEDED);
    context.anchor = 1004;
    context.context_length++;
    assert(boxrig_ui_editable_document_publish_context(document, &context) ==
        BOXRIG_UI_LIMIT_EXCEEDED);
    context.context_length--;
    context.context_offset = UINT64_MAX;
    assert(boxrig_ui_editable_document_publish_context(document, &context) ==
        BOXRIG_UI_INVALID_ARGUMENT);
    assert(boxrig_ui_editable_document_input_generation(document) == generation);
    assert(boxrig_ui_editable_document_invalidate_input(document) == BOXRIG_UI_OK);
    assert(boxrig_ui_editable_document_get_snapshot(document, &snapshot));
    assert(snapshot.input_generation > generation && snapshot.document_revision == 5);
    generation = snapshot.input_generation;
    assert(boxrig_ui_editable_document_clear(document) == BOXRIG_UI_OK);
    assert(!boxrig_ui_editable_document_get_snapshot(document, &snapshot));
    assert(boxrig_ui_editable_document_input_generation(document) > generation);
    assert(boxrig_ui_editable_document_get_context(document, 4000, &snapshot) ==
        BOXRIG_UI_UNAVAILABLE);
    input.context = NULL;
    assert(boxrig_ui_editable_document_publish(document, &input) == BOXRIG_UI_OK);
    assert(boxrig_ui_editable_document_get_context(document, 0, &snapshot) == BOXRIG_UI_OK);
    assert(snapshot.context_length == 0 && snapshot.context[0] == '\0');
    char large_context[4100];
    memset(large_context, 'x', sizeof(large_context));
    limits = boxrig_ui_editable_limits_default();
    assert(boxrig_ui_editable_document_set_limits(document, limits) == BOXRIG_UI_OK);
    context = (boxrig_ui_editable_context_input){3, 1, large_context,
        sizeof(large_context), 0, 2001, 2001};
    assert(boxrig_ui_editable_document_publish_context(document, &context) == BOXRIG_UI_OK);
    assert(boxrig_ui_editable_document_get_context(document, 4000, &snapshot) == BOXRIG_UI_OK);
    assert(snapshot.context_length == 4000 && snapshot.context_offset == 1);
    assert(snapshot.cursor == 2001 && snapshot.anchor == 2001);
    context.cursor = 4001;
    context.anchor = 0;
    assert(boxrig_ui_editable_document_publish_context(document, &context) == BOXRIG_UI_OK);
    assert(boxrig_ui_editable_document_get_context(document, 4000, &snapshot) == BOXRIG_UI_LIMIT_EXCEEDED);
    const uint8_t *bytes;
    assert(boxrig_terminal_output_peek(terminal, &bytes) == 0);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS editable context, insertion, anchor, generation, limits and allocation failures");
    return 0;
}
