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

typedef struct owner {
    boxrig_ui_editable_document *endpoint;
    char text[256];
    uint64_t revision;
    uint64_t last_request;
    unsigned int called;
    boxrig_ui_status result;
    boxrig_ui_editable_range last_range;
    char last_replacement[256];
} owner;

static boxrig_ui_status apply(
    const boxrig_ui_editable_replacement_request *request,
    const boxrig_ui_editable_document_snapshot *proposed,
    void *context)
{
    owner *o = context;
    o->called++;
    assert(request->request_id > o->last_request);
    o->last_request = request->request_id;
    assert(request->document_identity == 7);
    assert(request->base_document_revision == o->revision);
    assert(request->base_input_generation ==
        boxrig_ui_editable_document_input_generation(o->endpoint));
    assert(proposed->document_revision == o->revision + 1);
    assert(proposed->input_generation == request->base_input_generation + 1);
    boxrig_ui_editable_capabilities caps = boxrig_ui_editable_document_capabilities(o->endpoint);
    assert(caps.context_available && caps.synchronous_edit_available && caps.busy);
    assert(boxrig_ui_editable_document_clear(o->endpoint) == BOXRIG_UI_BUSY);
    assert(boxrig_ui_editable_document_invalidate_input(o->endpoint) == BOXRIG_UI_BUSY);
    assert(boxrig_ui_editable_document_set_apply_handler(o->endpoint, NULL, NULL) ==
        BOXRIG_UI_BUSY);
    assert(boxrig_ui_editable_document_set_limits(o->endpoint,
        boxrig_ui_editable_limits_default()) == BOXRIG_UI_BUSY);
    boxrig_ui_editable_context_input current = {
        7, o->revision, o->text, strlen(o->text), 100, 100, 100};
    assert(boxrig_ui_editable_document_publish_context(o->endpoint, &current) == BOXRIG_UI_BUSY);
    assert(boxrig_ui_editable_document_cancel_reconversion(o->endpoint) == BOXRIG_UI_BUSY);
    if (o->result != BOXRIG_UI_OK) return o->result;
    size_t start = (size_t)(request->range.offset - 100);
    size_t end = start + request->range.length;
    size_t old_length = strlen(o->text);
    assert(end <= old_length);
    assert(old_length - request->range.length + request->replacement_length < sizeof(o->text));
    memmove(o->text + start + request->replacement_length, o->text + end,
        old_length - end + 1);
    if (request->replacement_length != 0)
        memcpy(o->text + start, request->replacement, request->replacement_length);
    assert(proposed->context_length == strlen(o->text));
    assert(memcmp(proposed->context, o->text, proposed->context_length) == 0);
    o->revision = proposed->document_revision;
    o->last_range = request->range;
    assert(request->replacement_length < sizeof(o->last_replacement));
    if (request->replacement_length != 0)
        memcpy(o->last_replacement, request->replacement, request->replacement_length);
    o->last_replacement[request->replacement_length] = '\0';
    return BOXRIG_UI_OK;
}

int main(void)
{
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    assert(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &vc, &view) == BOXRIG_UI_OK);
    assert(boxrig_terminal_send_text(terminal, "prior", 5) == BOXRIG_STATUS_OK);
    boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(view);
    owner o = {.endpoint = document, .text = "A日😀e\xcc\x81Z", .revision = 1};
    boxrig_ui_editable_capabilities caps = boxrig_ui_editable_document_capabilities(document);
    assert(!caps.context_available && !caps.synchronous_edit_available && !caps.reconversion_available);
    boxrig_ui_editable_context_input input = {7, o.revision, o.text,
        strlen(o.text), 100, 111, 111};
    assert(boxrig_ui_editable_document_publish_context(document, &input) == BOXRIG_UI_OK);
    caps = boxrig_ui_editable_document_capabilities(document);
    assert(caps.context_available && !caps.synchronous_edit_available && !caps.reconversion_available);
    boxrig_ui_editable_edit edit = {
        boxrig_ui_editable_document_input_generation(document), {109, 2}, NULL, 0, 109, 109};
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_UNAVAILABLE);
    assert(boxrig_ui_editable_document_set_apply_handler(document, apply, &o) == BOXRIG_UI_OK);
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_STALE);
    edit.input_generation = boxrig_ui_editable_document_input_generation(document);
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_OK);
    assert(strcmp(o.text, "A日😀eZ") == 0);
    assert(o.last_range.offset == 108 && o.last_range.length == 3);
    assert(strcmp(o.last_replacement, "e") == 0);
    assert(o.called == 1);
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_STALE);
    assert(o.called == 1);
    boxrig_ui_editable_document_snapshot snapshot;
    assert(boxrig_ui_editable_document_get_snapshot(document, &snapshot));
    assert(snapshot.cursor == 109 && snapshot.anchor == 109 && snapshot.document_revision == 2);

    edit = (boxrig_ui_editable_edit){snapshot.input_generation, {104, 4}, "語", 3, 108, 108};
    o.result = BOXRIG_UI_UNAVAILABLE;
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_UNAVAILABLE);
    o.result = BOXRIG_UI_STALE;
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_STALE);
    o.result = BOXRIG_UI_BUSY;
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_BUSY);
    assert(o.revision == 2 && strcmp(o.text, "A日😀eZ") == 0);
    assert(boxrig_ui_editable_document_input_generation(document) == edit.input_generation);
    o.result = BOXRIG_UI_OK;
    unsigned int calls = o.called;
    bool accepted = false;
    for (size_t fail_at = 0; fail_at < 64; fail_at++) {
        allocations_before_failure = fail_at;
        boxrig_ui_status status = boxrig_ui_editable_document_apply_edit(document, &edit);
        allocations_before_failure = SIZE_MAX;
        if (status == BOXRIG_UI_OK) {
            assert(o.called == calls + 1);
            accepted = true;
            break;
        }
        assert(status == BOXRIG_UI_OUT_OF_MEMORY);
        assert(o.called == calls && o.revision == 2);
        assert(boxrig_ui_editable_document_input_generation(document) == edit.input_generation);
    }
    assert(accepted);
    assert(strcmp(o.text, "A日語eZ") == 0 && o.revision == 3);
    calls = o.called;

    edit.input_generation = boxrig_ui_editable_document_input_generation(document);
    edit.range = (boxrig_ui_editable_range){102, 1};
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_INVALID_ARGUMENT);
    edit.range = (boxrig_ui_editable_range){UINT64_MAX, 2};
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_INVALID_ARGUMENT);
    edit.range = (boxrig_ui_editable_range){100, 0};
    edit.replacement = "\xf0\x80\x80\x80";
    edit.replacement_length = 4;
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_INVALID_ARGUMENT);
    edit.replacement = "😀";
    edit.replacement_length = 4;
    edit.cursor_after = edit.anchor_after = 102;
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_INVALID_ARGUMENT);

    boxrig_ui_editable_limits limits = boxrig_ui_editable_limits_default();
    limits.maximum_context_bytes = strlen(o.text);
    assert(boxrig_ui_editable_document_set_limits(document, limits) == BOXRIG_UI_OK);
    edit.cursor_after = edit.anchor_after = 104;
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_LIMIT_EXCEEDED);
    assert(boxrig_ui_editable_document_set_limits(document,
        boxrig_ui_editable_limits_default()) == BOXRIG_UI_OK);
    assert(o.called == calls && strcmp(o.text, "A日語eZ") == 0);

    /* A queued asynchronous reconversion excludes synchronous editing. */
    input = (boxrig_ui_editable_context_input){7, o.revision, o.text, strlen(o.text), 100, 104, 101};
    assert(boxrig_ui_editable_document_publish_context(document, &input) == BOXRIG_UI_OK);
    assert(boxrig_ui_editable_document_capabilities(document).reconversion_available);
    assert(boxrig_ui_editable_document_begin_reconversion(document) == BOXRIG_UI_OK);
    edit.input_generation = boxrig_ui_editable_document_input_generation(document);
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_BUSY);
    assert(boxrig_ui_editable_document_commit_reconversion("本", 3, document) == BOXRIG_STATUS_OK);
    boxrig_ui_editable_replacement_request pending;
    assert(boxrig_ui_editable_document_take_replacement(document, &pending));
    assert(pending.base_input_generation == edit.input_generation);
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_BUSY);
    boxrig_ui_editable_replacement_completion completion = {
        pending.request_id, BOXRIG_UI_EDITABLE_REPLACEMENT_REJECTED, 0};
    assert(boxrig_ui_editable_document_complete_replacement(document, &completion) == BOXRIG_UI_OK);
    assert(boxrig_ui_editable_document_complete_replacement(document, &completion) == BOXRIG_UI_STALE);

    /* Delete the complete document, then insert into the genuine empty one. */
    edit = (boxrig_ui_editable_edit){boxrig_ui_editable_document_input_generation(document),
        {100, strlen(o.text)}, NULL, 0, 100, 100};
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_OK);
    assert(o.text[0] == '\0');
    edit = (boxrig_ui_editable_edit){boxrig_ui_editable_document_input_generation(document),
        {100, 0}, "日本\n語", strlen("日本\n語"), 110, 110};
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_OK);
    assert(strcmp(o.text, "日本\n語") == 0);
    assert(boxrig_ui_editable_document_invalidate_input(document) == BOXRIG_UI_OK);
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_STALE);
    /* Retain the suffix of a grapheme when its base character is deleted. */
    strcpy(o.text, "e\xcc\x81Z");
    o.revision++;
    input = (boxrig_ui_editable_context_input){7, o.revision, o.text, strlen(o.text), 100, 103, 103};
    assert(boxrig_ui_editable_document_publish_context(document, &input) == BOXRIG_UI_OK);
    edit = (boxrig_ui_editable_edit){boxrig_ui_editable_document_input_generation(document),
        {100, 1}, NULL, 0, 102, 102};
    limits = boxrig_ui_editable_limits_default();
    limits.maximum_selection_bytes = 2;
    assert(boxrig_ui_editable_document_set_limits(document, limits) == BOXRIG_UI_OK);
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_LIMIT_EXCEEDED);
    assert(strcmp(o.text, "e\xcc\x81Z") == 0);
    assert(boxrig_ui_editable_document_set_limits(document,
        boxrig_ui_editable_limits_default()) == BOXRIG_UI_OK);
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_OK);
    assert(strcmp(o.text, "\xcc\x81Z") == 0);
    assert(o.last_range.offset == 100 && o.last_range.length == 3);
    assert(strcmp(o.last_replacement, "\xcc\x81") == 0);
    assert(boxrig_ui_editable_document_set_apply_handler(document, NULL, NULL) == BOXRIG_UI_OK);
    edit.input_generation = boxrig_ui_editable_document_input_generation(document);
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_UNAVAILABLE);
    input.document_identity = 8;
    input.document_revision = 1;
    input.context = "新";
    input.context_length = 3;
    input.cursor = input.anchor = 103;
    assert(boxrig_ui_editable_document_publish_context(document, &input) == BOXRIG_UI_OK);
    assert(boxrig_ui_editable_document_apply_edit(document, &edit) == BOXRIG_UI_STALE);
    assert(boxrig_ui_editable_document_get_snapshot(document, &snapshot));
    assert(snapshot.document_identity == 8 && snapshot.document_revision == 1);
    const uint8_t *bytes;
    assert(boxrig_terminal_output_peek(terminal, &bytes) == 5);
    assert(memcmp(bytes, "prior", 5) == 0);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS synchronous owner edits, grapheme normalization, rejection, replay and backpressure");
    return 0;
}
