/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "renderer_lifecycle.h"
#include "view_private.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct boxrig_renderer_dirty_snapshot_entry {
    boxrig_ui_dirty_layer layer;
    boxrig_ui_dirty dirty;
} boxrig_renderer_dirty_snapshot_entry;

struct boxrig_renderer_lifecycle {
    boxrig_renderer_lifecycle_config config;
    boxrig_renderer_target target;
    boxrig_renderer_target target_candidate;
    boxrig_ui_view *frame_view;
    boxrig_renderer_dirty_snapshot_entry *dirty_snapshot;
    size_t dirty_snapshot_count;
    size_t dirty_snapshot_capacity;
    uint64_t dirty_revisions[BOXRIG_UI_DIRTY_LAYER_COUNT];
    boxrig_renderer_frame_token frame_token;
    boxrig_renderer_frame_work frame_work;
    boxrig_renderer_counters counters;
    boxrig_renderer_failure_point failure_point;
    uint64_t next_frame_serial;
    bool target_available;
    bool target_candidate_active;
    bool frame_active;
};

static bool valid_target(const boxrig_renderer_target *target)
{
    return target != NULL && target->generation != 0 && target->width != 0 &&
           target->height != 0 && target->scale_numerator != 0 &&
           target->scale_denominator != 0;
}

static bool valid_config(const boxrig_renderer_lifecycle_config *config)
{
    bool adapter_presents;
    bool managed_runtime;

    if (config == NULL || !valid_target(&config->initial_target) ||
        config->integration < BOXRIG_RENDERER_INTEGRATION_RENDERER_ONLY ||
        config->integration > BOXRIG_RENDERER_INTEGRATION_COMPONENT ||
        config->runtime_ownership < BOXRIG_RENDERER_RUNTIME_BORROWED ||
        config->runtime_ownership > BOXRIG_RENDERER_RUNTIME_MANAGED ||
        config->presentation_ownership < BOXRIG_RENDERER_PRESENTATION_CALLER ||
        config->presentation_ownership > BOXRIG_RENDERER_PRESENTATION_ADAPTER ||
        config->backend_context == NULL ||
        config->operations.prepare_frame == NULL ||
        config->operations.finish_frame == NULL ||
        config->operations.abort_frame == NULL ||
        config->operations.prepare_target == NULL ||
        config->operations.commit_target == NULL ||
        config->operations.abort_target == NULL ||
        config->operations.target_lost == NULL ||
        config->operations.release_resources == NULL ||
        config->limits.maximum_upload_bytes == 0 ||
        config->limits.maximum_dirty_rectangle_count == 0 ||
        config->limits.maximum_draw_count == 0 ||
        config->limits.maximum_submission_count == 0 ||
        config->limits.maximum_cache_entry_count == 0) {
        return false;
    }
    adapter_presents = config->presentation_ownership ==
                       BOXRIG_RENDERER_PRESENTATION_ADAPTER;
    managed_runtime = config->runtime_ownership ==
                      BOXRIG_RENDERER_RUNTIME_MANAGED;
    return adapter_presents == (config->operations.present_frame != NULL) &&
           managed_runtime == (config->operations.destroy_runtime != NULL);
}

static size_t saturated_add(size_t left, size_t right)
{
    return right > SIZE_MAX - left ? SIZE_MAX : left + right;
}

static void add_frame_work(
    boxrig_renderer_counters *counters,
    const boxrig_renderer_frame_work *work)
{
    counters->upload_bytes = saturated_add(
        counters->upload_bytes,
        work->upload_bytes);
    counters->dirty_rectangle_count = saturated_add(
        counters->dirty_rectangle_count,
        work->dirty_rectangle_count);
    counters->draw_count = saturated_add(
        counters->draw_count,
        work->draw_count);
    counters->submission_count = saturated_add(
        counters->submission_count,
        work->submission_count);
    counters->cache_hit_count = saturated_add(
        counters->cache_hit_count,
        work->cache_hit_count);
    counters->eviction_count = saturated_add(
        counters->eviction_count,
        work->eviction_count);
    counters->rebuild_count = saturated_add(
        counters->rebuild_count,
        work->rebuild_count);
}

static void add_frame_work_delta(
    boxrig_renderer_counters *counters,
    const boxrig_renderer_frame_work *before,
    const boxrig_renderer_frame_work *after)
{
    boxrig_renderer_frame_work delta;

    memset(&delta, 0, sizeof(delta));
    delta.upload_bytes = after->upload_bytes >= before->upload_bytes
                             ? after->upload_bytes - before->upload_bytes
                             : 0;
    delta.dirty_rectangle_count =
        after->dirty_rectangle_count >= before->dirty_rectangle_count
            ? after->dirty_rectangle_count - before->dirty_rectangle_count
            : 0;
    delta.draw_count = after->draw_count >= before->draw_count
                           ? after->draw_count - before->draw_count
                           : 0;
    delta.submission_count =
        after->submission_count >= before->submission_count
            ? after->submission_count - before->submission_count
            : 0;
    delta.cache_hit_count =
        after->cache_hit_count >= before->cache_hit_count
            ? after->cache_hit_count - before->cache_hit_count
            : 0;
    delta.eviction_count = after->eviction_count >= before->eviction_count
                               ? after->eviction_count - before->eviction_count
                               : 0;
    delta.rebuild_count = after->rebuild_count >= before->rebuild_count
                              ? after->rebuild_count - before->rebuild_count
                              : 0;
    add_frame_work(counters, &delta);
}

static bool work_within_limits(
    const boxrig_renderer_lifecycle *lifecycle,
    const boxrig_renderer_frame_work *work)
{
    return work->upload_bytes <= lifecycle->config.limits.maximum_upload_bytes &&
           work->dirty_rectangle_count <=
               lifecycle->config.limits.maximum_dirty_rectangle_count &&
           work->draw_count <= lifecycle->config.limits.maximum_draw_count &&
           work->submission_count <=
               lifecycle->config.limits.maximum_submission_count &&
           work->cache_entry_count <=
               lifecycle->config.limits.maximum_cache_entry_count;
}

static bool failure_requested(
    boxrig_renderer_lifecycle *lifecycle,
    boxrig_renderer_failure_point point)
{
    if (lifecycle->failure_point != point) {
        return false;
    }
    lifecycle->failure_point = BOXRIG_RENDERER_FAILURE_NONE;
    return true;
}

static boxrig_renderer_status reserve_dirty_snapshot(
    boxrig_renderer_lifecycle *lifecycle,
    size_t required)
{
    boxrig_renderer_dirty_snapshot_entry *entries;
    size_t capacity;

    if (required <= lifecycle->dirty_snapshot_capacity) {
        return BOXRIG_RENDERER_OK;
    }
    if (required > SIZE_MAX / sizeof(*entries)) {
        return BOXRIG_RENDERER_LIMIT_EXCEEDED;
    }
    capacity = lifecycle->dirty_snapshot_capacity == 0
                   ? 16
                   : lifecycle->dirty_snapshot_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    entries = realloc(
        lifecycle->dirty_snapshot,
        capacity * sizeof(*entries));
    if (entries == NULL) {
        return BOXRIG_RENDERER_OUT_OF_MEMORY;
    }
    lifecycle->dirty_snapshot = entries;
    lifecycle->dirty_snapshot_capacity = capacity;
    return BOXRIG_RENDERER_OK;
}

/* The view retains one interval per row. Equal adjacent intervals become one
 * frame rectangle so the resource limit describes backend work, not rows. */
static boxrig_renderer_status capture_dirty(
    boxrig_renderer_lifecycle *lifecycle,
    boxrig_ui_view *view)
{
    boxrig_terminal *terminal = boxrig_ui_view_terminal(view);
    boxrig_ui_dirty_layer layer;
    size_t rows;
    size_t columns;

    if (terminal == NULL) {
        return BOXRIG_RENDERER_INVALID_ARGUMENT;
    }
    boxrig_terminal_size(terminal, &rows, &columns);
    lifecycle->dirty_snapshot_count = 0;
    for (layer = BOXRIG_UI_DIRTY_MODEL;
         layer < BOXRIG_UI_DIRTY_LAYER_COUNT;
         layer++) {
        size_t row = 0;

        lifecycle->dirty_revisions[layer] =
            boxrig_ui_view_dirty_revision(view, layer);
        while (row < rows) {
            boxrig_ui_dirty dirty;
            size_t end_row;
            boxrig_renderer_status status;

            if (!boxrig_ui_view_get_row_dirty(view, layer, row, &dirty)) {
                row++;
                continue;
            }
            end_row = row + 1;
            while (end_row < rows) {
                boxrig_ui_dirty next;

                if (!boxrig_ui_view_get_row_dirty(
                        view,
                        layer,
                        end_row,
                        &next) ||
                    next.column != dirty.column ||
                    next.column_count != dirty.column_count) {
                    break;
                }
                end_row++;
            }
            if (lifecycle->dirty_snapshot_count >=
                lifecycle->config.limits.maximum_dirty_rectangle_count) {
                lifecycle->dirty_snapshot_count = 0;
                return BOXRIG_RENDERER_LIMIT_EXCEEDED;
            }
            status = reserve_dirty_snapshot(
                lifecycle,
                lifecycle->dirty_snapshot_count + 1);
            if (status != BOXRIG_RENDERER_OK) {
                lifecycle->dirty_snapshot_count = 0;
                return status;
            }
            dirty.row_count = end_row - row;
            lifecycle->dirty_snapshot[lifecycle->dirty_snapshot_count].layer =
                layer;
            lifecycle->dirty_snapshot[lifecycle->dirty_snapshot_count].dirty =
                dirty;
            lifecycle->dirty_snapshot_count++;
            row = end_row;
        }
    }
    (void)columns;
    return BOXRIG_RENDERER_OK;
}

static bool same_dirty(
    const boxrig_ui_dirty *left,
    const boxrig_ui_dirty *right)
{
    return left->row == right->row &&
           left->row_count == right->row_count &&
           left->column == right->column &&
           left->column_count == right->column_count;
}

/* If new dirty coalesced with a captured interval, retain the whole current
 * interval. This may redraw old cells but never consumes work added later. */
static boxrig_renderer_status consume_captured_dirty(
    boxrig_renderer_lifecycle *lifecycle)
{
    size_t index;

    for (index = 0; index < lifecycle->dirty_snapshot_count; index++) {
        const boxrig_renderer_dirty_snapshot_entry *entry =
            &lifecycle->dirty_snapshot[index];
        boxrig_ui_dirty current;

        if (boxrig_ui_view_dirty_revision(
                lifecycle->frame_view,
                entry->layer) != lifecycle->dirty_revisions[entry->layer] ||
            !boxrig_ui_view_peek_dirty(
                lifecycle->frame_view,
                entry->layer,
                &current) ||
            !same_dirty(&current, &entry->dirty)) {
            continue;
        }
        if (boxrig_ui_view_consume_dirty(
                lifecycle->frame_view,
                entry->layer) != BOXRIG_UI_OK) {
            return BOXRIG_RENDERER_UNAVAILABLE;
        }
    }
    return BOXRIG_RENDERER_OK;
}

static bool matching_token(
    const boxrig_renderer_lifecycle *lifecycle,
    const boxrig_renderer_frame_token *token)
{
    return lifecycle != NULL && token != NULL && lifecycle->frame_active &&
           token->serial == lifecycle->frame_token.serial &&
           token->target_generation ==
               lifecycle->frame_token.target_generation;
}

static void clear_frame(boxrig_renderer_lifecycle *lifecycle)
{
    lifecycle->frame_active = false;
    lifecycle->frame_view = NULL;
    lifecycle->dirty_snapshot_count = 0;
    memset(&lifecycle->frame_token, 0, sizeof(lifecycle->frame_token));
    memset(&lifecycle->frame_work, 0, sizeof(lifecycle->frame_work));
}

static void abort_active_frame(boxrig_renderer_lifecycle *lifecycle)
{
    lifecycle->config.operations.abort_frame(
        lifecycle->config.backend_context);
    lifecycle->counters.frame_abort_count = saturated_add(
        lifecycle->counters.frame_abort_count,
        1);
    clear_frame(lifecycle);
}

boxrig_renderer_lifecycle_config boxrig_renderer_lifecycle_config_default(
    void)
{
    boxrig_renderer_lifecycle_config config;

    memset(&config, 0, sizeof(config));
    config.integration = BOXRIG_RENDERER_INTEGRATION_RENDERER_ONLY;
    config.runtime_ownership = BOXRIG_RENDERER_RUNTIME_BORROWED;
    config.presentation_ownership = BOXRIG_RENDERER_PRESENTATION_CALLER;
    config.initial_target.generation = 1;
    config.initial_target.width = 1;
    config.initial_target.height = 1;
    config.initial_target.scale_numerator = 1;
    config.initial_target.scale_denominator = 1;
    config.limits.maximum_upload_bytes = SIZE_MAX;
    config.limits.maximum_dirty_rectangle_count = SIZE_MAX;
    config.limits.maximum_draw_count = SIZE_MAX;
    config.limits.maximum_submission_count = SIZE_MAX;
    config.limits.maximum_cache_entry_count = SIZE_MAX;
    return config;
}

boxrig_renderer_status boxrig_renderer_lifecycle_create(
    const boxrig_renderer_lifecycle_config *config,
    boxrig_renderer_lifecycle **lifecycle_out)
{
    boxrig_renderer_lifecycle *lifecycle;

    if (!valid_config(config) || lifecycle_out == NULL) {
        return BOXRIG_RENDERER_INVALID_ARGUMENT;
    }
    *lifecycle_out = NULL;
    lifecycle = calloc(1, sizeof(*lifecycle));
    if (lifecycle == NULL) {
        return BOXRIG_RENDERER_OUT_OF_MEMORY;
    }
    lifecycle->config = *config;
    lifecycle->target = config->initial_target;
    lifecycle->target_available = true;
    lifecycle->next_frame_serial = 1;
    *lifecycle_out = lifecycle;
    return BOXRIG_RENDERER_OK;
}

void boxrig_renderer_lifecycle_destroy(
    boxrig_renderer_lifecycle *lifecycle)
{
    if (lifecycle == NULL) {
        return;
    }
    if (lifecycle->frame_active) {
        abort_active_frame(lifecycle);
    }
    if (lifecycle->target_candidate_active) {
        lifecycle->config.operations.abort_target(
            lifecycle->config.backend_context);
    }
    lifecycle->config.operations.release_resources(
        lifecycle->config.backend_context);
    if (lifecycle->config.runtime_ownership ==
        BOXRIG_RENDERER_RUNTIME_MANAGED) {
        lifecycle->config.operations.destroy_runtime(
            lifecycle->config.backend_context);
    }
    free(lifecycle->dirty_snapshot);
    free(lifecycle);
}

boxrig_renderer_status boxrig_renderer_lifecycle_prepare_frame(
    boxrig_renderer_lifecycle *lifecycle,
    boxrig_ui_view *view,
    boxrig_composition *composition,
    uint64_t target_generation,
    boxrig_renderer_frame_token *token_out)
{
    boxrig_renderer_frame_work work;
    boxrig_renderer_status status;

    if (lifecycle == NULL || view == NULL || token_out == NULL ||
        target_generation == 0) {
        return BOXRIG_RENDERER_INVALID_ARGUMENT;
    }
    memset(token_out, 0, sizeof(*token_out));
    if (lifecycle->frame_active) {
        return BOXRIG_RENDERER_BUSY;
    }
    if (!lifecycle->target_available) {
        return BOXRIG_RENDERER_UNAVAILABLE;
    }
    if (target_generation != lifecycle->target.generation) {
        return BOXRIG_RENDERER_STALE;
    }
    if (failure_requested(
            lifecycle,
            BOXRIG_RENDERER_FAILURE_PREPARE_FRAME)) {
        lifecycle->counters.failure_count = saturated_add(
            lifecycle->counters.failure_count,
            1);
        return BOXRIG_RENDERER_UNAVAILABLE;
    }
    boxrig_ui_fragment_layout_provider *provider = NULL;
    if (lifecycle->config.operations.fragment_layout_provider != NULL &&
        lifecycle->config.operations.fragment_layout_provider(
            lifecycle->config.backend_context, &provider) != BOXRIG_UI_OK) {
        return BOXRIG_RENDERER_UNAVAILABLE;
    }
    if (boxrig_ui_view_set_fragment_layout_provider(view, provider) != BOXRIG_UI_OK) {
        return BOXRIG_RENDERER_BUSY;
    }
    status = capture_dirty(lifecycle, view);
    if (status != BOXRIG_RENDERER_OK) {
        lifecycle->counters.failure_count = saturated_add(
            lifecycle->counters.failure_count,
            1);
        return status;
    }
    memset(&work, 0, sizeof(work));
    work.dirty_rectangle_count = lifecycle->dirty_snapshot_count;
    if (lifecycle->config.operations.prepare_frame(
            lifecycle->config.backend_context,
            view,
            composition,
            &lifecycle->target,
            &work) != 0) {
        lifecycle->config.operations.abort_frame(
            lifecycle->config.backend_context);
        lifecycle->dirty_snapshot_count = 0;
        lifecycle->counters.failure_count = saturated_add(
            lifecycle->counters.failure_count,
            1);
        return BOXRIG_RENDERER_UNAVAILABLE;
    }
    if (!work_within_limits(lifecycle, &work)) {
        lifecycle->config.operations.abort_frame(
            lifecycle->config.backend_context);
        lifecycle->dirty_snapshot_count = 0;
        lifecycle->counters.failure_count = saturated_add(
            lifecycle->counters.failure_count,
            1);
        return BOXRIG_RENDERER_LIMIT_EXCEEDED;
    }
    lifecycle->frame_active = true;
    lifecycle->frame_view = view;
    lifecycle->frame_work = work;
    lifecycle->frame_token.serial = lifecycle->next_frame_serial++;
    if (lifecycle->next_frame_serial == 0) {
        lifecycle->next_frame_serial = 1;
    }
    lifecycle->frame_token.target_generation = target_generation;
    lifecycle->counters.frame_prepare_count = saturated_add(
        lifecycle->counters.frame_prepare_count,
        1);
    add_frame_work(&lifecycle->counters, &work);
    *token_out = lifecycle->frame_token;
    return BOXRIG_RENDERER_OK;
}

boxrig_renderer_status boxrig_renderer_lifecycle_accept_frame(
    boxrig_renderer_lifecycle *lifecycle,
    const boxrig_renderer_frame_token *token)
{
    boxrig_renderer_frame_work before;
    boxrig_renderer_status status;

    if (!matching_token(lifecycle, token)) {
        return BOXRIG_RENDERER_INVALID_ARGUMENT;
    }
    before = lifecycle->frame_work;
    if (lifecycle->config.presentation_ownership ==
        BOXRIG_RENDERER_PRESENTATION_ADAPTER) {
        if (failure_requested(
                lifecycle,
                BOXRIG_RENDERER_FAILURE_PRESENT_FRAME) ||
            lifecycle->config.operations.present_frame(
                lifecycle->config.backend_context,
                &lifecycle->frame_work) != 0) {
            abort_active_frame(lifecycle);
            lifecycle->counters.failure_count = saturated_add(
                lifecycle->counters.failure_count,
                1);
            return BOXRIG_RENDERER_UNAVAILABLE;
        }
    }
    if (failure_requested(
            lifecycle,
            BOXRIG_RENDERER_FAILURE_FINISH_FRAME) ||
        lifecycle->config.operations.finish_frame(
            lifecycle->config.backend_context,
            &lifecycle->frame_work) != 0) {
        abort_active_frame(lifecycle);
        lifecycle->counters.failure_count = saturated_add(
            lifecycle->counters.failure_count,
            1);
        return BOXRIG_RENDERER_UNAVAILABLE;
    }
    if (!work_within_limits(lifecycle, &lifecycle->frame_work)) {
        abort_active_frame(lifecycle);
        lifecycle->counters.failure_count = saturated_add(
            lifecycle->counters.failure_count,
            1);
        return BOXRIG_RENDERER_LIMIT_EXCEEDED;
    }
    status = consume_captured_dirty(lifecycle);
    if (status != BOXRIG_RENDERER_OK) {
        abort_active_frame(lifecycle);
        lifecycle->counters.failure_count = saturated_add(
            lifecycle->counters.failure_count,
            1);
        return status;
    }
    add_frame_work_delta(
        &lifecycle->counters,
        &before,
        &lifecycle->frame_work);
    lifecycle->counters.frame_accept_count = saturated_add(
        lifecycle->counters.frame_accept_count,
        1);
    clear_frame(lifecycle);
    return BOXRIG_RENDERER_OK;
}

boxrig_renderer_status boxrig_renderer_lifecycle_abort_frame(
    boxrig_renderer_lifecycle *lifecycle,
    const boxrig_renderer_frame_token *token)
{
    if (!matching_token(lifecycle, token)) {
        return BOXRIG_RENDERER_INVALID_ARGUMENT;
    }
    abort_active_frame(lifecycle);
    return BOXRIG_RENDERER_OK;
}

boxrig_renderer_status boxrig_renderer_lifecycle_prepare_target(
    boxrig_renderer_lifecycle *lifecycle,
    const boxrig_renderer_target *candidate)
{
    if (lifecycle == NULL || !valid_target(candidate)) {
        return BOXRIG_RENDERER_INVALID_ARGUMENT;
    }
    if (lifecycle->frame_active || lifecycle->target_candidate_active) {
        return BOXRIG_RENDERER_BUSY;
    }
    if (lifecycle->target_available &&
        candidate->generation == lifecycle->target.generation) {
        return BOXRIG_RENDERER_STALE;
    }
    if (failure_requested(
            lifecycle,
            BOXRIG_RENDERER_FAILURE_PREPARE_TARGET)) {
        lifecycle->counters.failure_count = saturated_add(
            lifecycle->counters.failure_count,
            1);
        return BOXRIG_RENDERER_UNAVAILABLE;
    }
    if (lifecycle->config.operations.prepare_target(
            lifecycle->config.backend_context,
            candidate) != 0) {
        lifecycle->counters.failure_count = saturated_add(
            lifecycle->counters.failure_count,
            1);
        return BOXRIG_RENDERER_UNAVAILABLE;
    }
    lifecycle->target_candidate = *candidate;
    lifecycle->target_candidate_active = true;
    lifecycle->counters.target_prepare_count = saturated_add(
        lifecycle->counters.target_prepare_count,
        1);
    return BOXRIG_RENDERER_OK;
}

boxrig_renderer_status boxrig_renderer_lifecycle_commit_target(
    boxrig_renderer_lifecycle *lifecycle)
{
    if (lifecycle == NULL || !lifecycle->target_candidate_active) {
        return BOXRIG_RENDERER_INVALID_ARGUMENT;
    }
    if (failure_requested(
            lifecycle,
            BOXRIG_RENDERER_FAILURE_COMMIT_TARGET) ||
        lifecycle->config.operations.commit_target(
            lifecycle->config.backend_context) != 0) {
        lifecycle->config.operations.abort_target(
            lifecycle->config.backend_context);
        lifecycle->target_candidate_active = false;
        lifecycle->counters.target_abort_count = saturated_add(
            lifecycle->counters.target_abort_count,
            1);
        lifecycle->counters.failure_count = saturated_add(
            lifecycle->counters.failure_count,
            1);
        return BOXRIG_RENDERER_UNAVAILABLE;
    }
    lifecycle->target = lifecycle->target_candidate;
    lifecycle->target_available = true;
    lifecycle->target_candidate_active = false;
    lifecycle->counters.target_commit_count = saturated_add(
        lifecycle->counters.target_commit_count,
        1);
    lifecycle->counters.rebuild_count = saturated_add(
        lifecycle->counters.rebuild_count,
        1);
    return BOXRIG_RENDERER_OK;
}

boxrig_renderer_status boxrig_renderer_lifecycle_abort_target(
    boxrig_renderer_lifecycle *lifecycle)
{
    if (lifecycle == NULL || !lifecycle->target_candidate_active) {
        return BOXRIG_RENDERER_INVALID_ARGUMENT;
    }
    lifecycle->config.operations.abort_target(
        lifecycle->config.backend_context);
    lifecycle->target_candidate_active = false;
    lifecycle->counters.target_abort_count = saturated_add(
        lifecycle->counters.target_abort_count,
        1);
    return BOXRIG_RENDERER_OK;
}

boxrig_renderer_status boxrig_renderer_lifecycle_target_lost(
    boxrig_renderer_lifecycle *lifecycle,
    boxrig_ui_view *view,
    uint64_t target_generation)
{
    boxrig_terminal *terminal;
    boxrig_ui_dirty dirty;

    if (lifecycle == NULL || view == NULL || target_generation == 0) {
        return BOXRIG_RENDERER_INVALID_ARGUMENT;
    }
    terminal = boxrig_ui_view_terminal(view);
    if (terminal == NULL) {
        return BOXRIG_RENDERER_INVALID_ARGUMENT;
    }
    if (!lifecycle->target_available) {
        return BOXRIG_RENDERER_UNAVAILABLE;
    }
    if (target_generation != lifecycle->target.generation) {
        return BOXRIG_RENDERER_STALE;
    }
    if (lifecycle->frame_active) {
        abort_active_frame(lifecycle);
    }
    if (lifecycle->target_candidate_active) {
        lifecycle->config.operations.abort_target(
            lifecycle->config.backend_context);
        lifecycle->target_candidate_active = false;
        lifecycle->counters.target_abort_count = saturated_add(
            lifecycle->counters.target_abort_count,
            1);
    }
    lifecycle->config.operations.target_lost(
        lifecycle->config.backend_context);
    lifecycle->target_available = false;
    lifecycle->counters.target_loss_count = saturated_add(
        lifecycle->counters.target_loss_count,
        1);
    boxrig_terminal_size(terminal, &dirty.row_count, &dirty.column_count);
    dirty.row = 0;
    dirty.column = 0;
    if (dirty.row_count != 0 && dirty.column_count != 0 &&
        boxrig_ui_view_invalidate(
            view,
            BOXRIG_UI_DIRTY_PRESENT,
            dirty) != BOXRIG_UI_OK) {
        return BOXRIG_RENDERER_UNAVAILABLE;
    }
    return BOXRIG_RENDERER_OK;
}

void boxrig_renderer_lifecycle_fail_next(
    boxrig_renderer_lifecycle *lifecycle,
    boxrig_renderer_failure_point point)
{
    if (lifecycle == NULL || point < BOXRIG_RENDERER_FAILURE_NONE ||
        point > BOXRIG_RENDERER_FAILURE_COMMIT_TARGET) {
        return;
    }
    lifecycle->failure_point = point;
}

boxrig_renderer_status boxrig_renderer_lifecycle_get_counters(
    const boxrig_renderer_lifecycle *lifecycle,
    boxrig_renderer_counters *counters_out)
{
    if (lifecycle == NULL || counters_out == NULL) {
        return BOXRIG_RENDERER_INVALID_ARGUMENT;
    }
    *counters_out = lifecycle->counters;
    return BOXRIG_RENDERER_OK;
}
