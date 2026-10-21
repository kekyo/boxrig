/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "ui/renderer_lifecycle.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

typedef struct fake_backend {
    size_t prepare_count;
    size_t finish_count;
    size_t present_count;
    size_t abort_count;
    size_t target_prepare_count;
    size_t target_commit_count;
    size_t target_abort_count;
    size_t target_loss_count;
    size_t resource_release_count;
    size_t runtime_destroy_count;
    bool fail_present;
} fake_backend;

typedef struct view_fixture {
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_view *view;
} view_fixture;

static int fake_prepare_frame(
    void *context,
    boxrig_ui_view *view,
    boxrig_composition *composition,
    const boxrig_renderer_target *target,
    boxrig_renderer_frame_work *work_out)
{
    fake_backend *backend = context;

    (void)view;
    (void)composition;
    if (target == NULL || target->generation == 0 || work_out == NULL) {
        return -1;
    }
    backend->prepare_count++;
    work_out->draw_count = 2;
    work_out->upload_bytes = 32;
    return 0;
}

static int fake_finish_frame(
    void *context,
    boxrig_renderer_frame_work *work_in_out)
{
    fake_backend *backend = context;

    backend->finish_count++;
    work_in_out->cache_hit_count++;
    work_in_out->eviction_count++;
    return 0;
}

static int fake_present_frame(
    void *context,
    boxrig_renderer_frame_work *work_in_out)
{
    fake_backend *backend = context;

    backend->present_count++;
    work_in_out->submission_count++;
    return backend->fail_present ? -1 : 0;
}

static void fake_abort_frame(void *context)
{
    fake_backend *backend = context;

    backend->abort_count++;
}

static int fake_prepare_target(
    void *context,
    const boxrig_renderer_target *target)
{
    fake_backend *backend = context;

    if (target == NULL || target->generation == 0) {
        return -1;
    }
    backend->target_prepare_count++;
    return 0;
}

static int fake_commit_target(void *context)
{
    fake_backend *backend = context;

    backend->target_commit_count++;
    return 0;
}

static void fake_abort_target(void *context)
{
    fake_backend *backend = context;

    backend->target_abort_count++;
}

static void fake_target_lost(void *context)
{
    fake_backend *backend = context;

    backend->target_loss_count++;
}

static void fake_release_resources(void *context)
{
    fake_backend *backend = context;

    backend->resource_release_count++;
}

static void fake_destroy_runtime(void *context)
{
    fake_backend *backend = context;

    backend->runtime_destroy_count++;
}

static boxrig_renderer_backend_operations fake_operations(void)
{
    boxrig_renderer_backend_operations operations;

    memset(&operations, 0, sizeof(operations));
    operations.prepare_frame = fake_prepare_frame;
    operations.finish_frame = fake_finish_frame;
    operations.present_frame = fake_present_frame;
    operations.abort_frame = fake_abort_frame;
    operations.prepare_target = fake_prepare_target;
    operations.commit_target = fake_commit_target;
    operations.abort_target = fake_abort_target;
    operations.target_lost = fake_target_lost;
    operations.release_resources = fake_release_resources;
    return operations;
}

static bool create_view_fixture(view_fixture *fixture)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();

    memset(fixture, 0, sizeof(*fixture));
    terminal_config.rows = 2;
    terminal_config.columns = 4;
    CHECK(boxrig_terminal_create(&terminal_config, &fixture->terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(fixture->terminal, &fixture->model) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(fixture->model, &view_config, &fixture->view) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_model_synchronize(fixture->model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_frame_requested(fixture->view));
    return true;
}

static void destroy_view_fixture(view_fixture *fixture)
{
    boxrig_ui_view_destroy(fixture->view);
    boxrig_ui_model_destroy(fixture->model);
    boxrig_terminal_destroy(fixture->terminal);
    memset(fixture, 0, sizeof(*fixture));
}

static boxrig_renderer_lifecycle_config lifecycle_config(
    fake_backend *backend,
    boxrig_renderer_integration integration,
    boxrig_renderer_runtime_ownership runtime_ownership,
    boxrig_renderer_presentation_ownership presentation_ownership)
{
    boxrig_renderer_lifecycle_config config =
        boxrig_renderer_lifecycle_config_default();

    config.integration = integration;
    config.runtime_ownership = runtime_ownership;
    config.presentation_ownership = presentation_ownership;
    config.backend_context = backend;
    config.operations = fake_operations();
    if (presentation_ownership == BOXRIG_RENDERER_PRESENTATION_CALLER) {
        config.operations.present_frame = NULL;
    }
    if (runtime_ownership == BOXRIG_RENDERER_RUNTIME_MANAGED) {
        config.operations.destroy_runtime = fake_destroy_runtime;
    }
    config.initial_target.generation = 7;
    config.initial_target.width = 64;
    config.initial_target.height = 32;
    return config;
}

static bool borrowed_renderer_only_retains_dirty_until_accept(void)
{
    view_fixture fixture;
    fake_backend backend;
    boxrig_renderer_lifecycle_config config;
    boxrig_renderer_lifecycle *lifecycle = NULL;
    boxrig_renderer_frame_token token;
    boxrig_renderer_counters counters;
    boxrig_ui_dirty repeated_dirty;

    memset(&backend, 0, sizeof(backend));
    CHECK(create_view_fixture(&fixture));
    config = lifecycle_config(
        &backend,
        BOXRIG_RENDERER_INTEGRATION_RENDERER_ONLY,
        BOXRIG_RENDERER_RUNTIME_BORROWED,
        BOXRIG_RENDERER_PRESENTATION_CALLER);
    CHECK(boxrig_renderer_lifecycle_create(&config, &lifecycle) ==
          BOXRIG_RENDERER_OK);

    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle, fixture.view, NULL, 7, &token) ==
          BOXRIG_RENDERER_OK);
    CHECK(boxrig_ui_view_frame_requested(fixture.view));
    CHECK(backend.prepare_count == 1 && backend.present_count == 0);
    CHECK(boxrig_renderer_lifecycle_abort_frame(lifecycle, &token) ==
          BOXRIG_RENDERER_OK);
    CHECK(boxrig_ui_view_frame_requested(fixture.view));
    CHECK(backend.abort_count == 1);

    boxrig_renderer_lifecycle_fail_next(
        lifecycle,
        BOXRIG_RENDERER_FAILURE_PREPARE_FRAME);
    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle, fixture.view, NULL, 7, &token) ==
          BOXRIG_RENDERER_UNAVAILABLE);
    CHECK(backend.prepare_count == 1);
    CHECK(boxrig_ui_view_frame_requested(fixture.view));

    repeated_dirty.row = 0;
    repeated_dirty.row_count = 2;
    repeated_dirty.column = 0;
    repeated_dirty.column_count = 4;
    CHECK(boxrig_ui_view_invalidate(
              fixture.view,
              BOXRIG_UI_DIRTY_PRESENT,
              repeated_dirty) == BOXRIG_UI_OK);
    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle, fixture.view, NULL, 7, &token) ==
          BOXRIG_RENDERER_OK);
    CHECK(boxrig_ui_view_invalidate(
              fixture.view,
              BOXRIG_UI_DIRTY_PRESENT,
              repeated_dirty) == BOXRIG_UI_OK);
    CHECK(boxrig_renderer_lifecycle_accept_frame(lifecycle, &token) ==
          BOXRIG_RENDERER_OK);
    CHECK(boxrig_ui_view_frame_requested(fixture.view));
    CHECK(backend.finish_count == 1 && backend.present_count == 0);
    CHECK(boxrig_renderer_lifecycle_accept_frame(lifecycle, &token) ==
          BOXRIG_RENDERER_INVALID_ARGUMENT);
    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle, fixture.view, NULL, 7, &token) ==
          BOXRIG_RENDERER_OK);
    CHECK(boxrig_renderer_lifecycle_accept_frame(lifecycle, &token) ==
          BOXRIG_RENDERER_OK);
    CHECK(!boxrig_ui_view_frame_requested(fixture.view));

    CHECK(boxrig_renderer_lifecycle_get_counters(lifecycle, &counters) ==
          BOXRIG_RENDERER_OK);
    CHECK(counters.frame_prepare_count == 3);
    CHECK(counters.frame_accept_count == 2);
    CHECK(counters.frame_abort_count == 1);
    CHECK(counters.failure_count == 1);
    CHECK(counters.draw_count == 6 && counters.upload_bytes == 96);
    CHECK(counters.submission_count == 0);

    boxrig_renderer_lifecycle_destroy(lifecycle);
    CHECK(backend.resource_release_count == 1);
    CHECK(backend.runtime_destroy_count == 0);
    destroy_view_fixture(&fixture);
    return true;
}

static bool managed_component_handles_target_and_present_failures(void)
{
    view_fixture fixture;
    fake_backend backend;
    boxrig_renderer_lifecycle_config config;
    boxrig_renderer_lifecycle *lifecycle = NULL;
    boxrig_renderer_frame_token token;
    boxrig_renderer_target candidate;
    boxrig_renderer_counters counters;

    memset(&backend, 0, sizeof(backend));
    CHECK(create_view_fixture(&fixture));
    config = lifecycle_config(
        &backend,
        BOXRIG_RENDERER_INTEGRATION_COMPONENT,
        BOXRIG_RENDERER_RUNTIME_MANAGED,
        BOXRIG_RENDERER_PRESENTATION_ADAPTER);
    CHECK(boxrig_renderer_lifecycle_create(&config, &lifecycle) ==
          BOXRIG_RENDERER_OK);

    backend.fail_present = true;
    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle, fixture.view, NULL, 7, &token) ==
          BOXRIG_RENDERER_OK);
    CHECK(boxrig_renderer_lifecycle_accept_frame(lifecycle, &token) ==
          BOXRIG_RENDERER_UNAVAILABLE);
    CHECK(boxrig_ui_view_frame_requested(fixture.view));
    CHECK(backend.present_count == 1 && backend.abort_count == 1);
    backend.fail_present = false;

    candidate = config.initial_target;
    candidate.generation = 8;
    candidate.width = 80;
    CHECK(boxrig_renderer_lifecycle_prepare_target(lifecycle, &candidate) ==
          BOXRIG_RENDERER_OK);
    CHECK(boxrig_renderer_lifecycle_abort_target(lifecycle) ==
          BOXRIG_RENDERER_OK);
    boxrig_renderer_lifecycle_fail_next(
        lifecycle,
        BOXRIG_RENDERER_FAILURE_PREPARE_TARGET);
    CHECK(boxrig_renderer_lifecycle_prepare_target(lifecycle, &candidate) ==
          BOXRIG_RENDERER_UNAVAILABLE);
    boxrig_renderer_lifecycle_fail_next(
        lifecycle,
        BOXRIG_RENDERER_FAILURE_COMMIT_TARGET);
    CHECK(boxrig_renderer_lifecycle_prepare_target(lifecycle, &candidate) ==
          BOXRIG_RENDERER_OK);
    CHECK(boxrig_renderer_lifecycle_commit_target(lifecycle) ==
          BOXRIG_RENDERER_UNAVAILABLE);
    CHECK(backend.target_abort_count == 2);

    CHECK(boxrig_renderer_lifecycle_prepare_target(lifecycle, &candidate) ==
          BOXRIG_RENDERER_OK);
    CHECK(boxrig_renderer_lifecycle_commit_target(lifecycle) ==
          BOXRIG_RENDERER_OK);
    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle, fixture.view, NULL, 7, &token) ==
          BOXRIG_RENDERER_STALE);
    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle, fixture.view, NULL, 8, &token) ==
          BOXRIG_RENDERER_OK);
    CHECK(boxrig_renderer_lifecycle_accept_frame(lifecycle, &token) ==
          BOXRIG_RENDERER_OK);
    CHECK(!boxrig_ui_view_frame_requested(fixture.view));
    CHECK(boxrig_renderer_lifecycle_target_lost(
              lifecycle, fixture.view, 8) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_ui_view_frame_requested(fixture.view));
    CHECK(backend.abort_count == 1 && backend.target_loss_count == 1);
    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle, fixture.view, NULL, 8, &token) ==
          BOXRIG_RENDERER_UNAVAILABLE);

    candidate.generation = 9;
    CHECK(boxrig_renderer_lifecycle_prepare_target(lifecycle, &candidate) ==
          BOXRIG_RENDERER_OK);
    CHECK(boxrig_renderer_lifecycle_commit_target(lifecycle) ==
          BOXRIG_RENDERER_OK);
    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle, fixture.view, NULL, 9, &token) ==
          BOXRIG_RENDERER_OK);
    boxrig_renderer_lifecycle_fail_next(
        lifecycle,
        BOXRIG_RENDERER_FAILURE_PRESENT_FRAME);
    CHECK(boxrig_renderer_lifecycle_accept_frame(lifecycle, &token) ==
          BOXRIG_RENDERER_UNAVAILABLE);
    CHECK(boxrig_ui_view_frame_requested(fixture.view));
    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle, fixture.view, NULL, 9, &token) ==
          BOXRIG_RENDERER_OK);
    boxrig_renderer_lifecycle_fail_next(
        lifecycle,
        BOXRIG_RENDERER_FAILURE_FINISH_FRAME);
    CHECK(boxrig_renderer_lifecycle_accept_frame(lifecycle, &token) ==
          BOXRIG_RENDERER_UNAVAILABLE);
    CHECK(boxrig_ui_view_frame_requested(fixture.view));
    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle, fixture.view, NULL, 9, &token) ==
          BOXRIG_RENDERER_OK);
    CHECK(boxrig_renderer_lifecycle_accept_frame(lifecycle, &token) ==
          BOXRIG_RENDERER_OK);
    CHECK(!boxrig_ui_view_frame_requested(fixture.view));
    CHECK(backend.present_count == 4);

    CHECK(boxrig_renderer_lifecycle_get_counters(lifecycle, &counters) ==
          BOXRIG_RENDERER_OK);
    CHECK(counters.target_prepare_count == 4);
    CHECK(counters.target_commit_count == 2);
    CHECK(counters.target_abort_count == 2);
    CHECK(counters.target_loss_count == 1);
    CHECK(counters.frame_accept_count == 2);
    CHECK(counters.frame_abort_count == 3);
    CHECK(counters.failure_count == 5);
    CHECK(counters.submission_count == 2);
    CHECK(counters.cache_hit_count == 2);
    CHECK(counters.eviction_count == 2);
    CHECK(counters.rebuild_count == 2);

    boxrig_renderer_lifecycle_destroy(lifecycle);
    CHECK(backend.resource_release_count == 1);
    CHECK(backend.runtime_destroy_count == 1);
    destroy_view_fixture(&fixture);
    return true;
}

static bool bounded_frame_work_is_rejected_without_consuming_dirty(void)
{
    view_fixture fixture;
    fake_backend backend;
    boxrig_renderer_lifecycle_config config;
    boxrig_renderer_lifecycle *lifecycle = NULL;
    boxrig_renderer_frame_token token;

    memset(&backend, 0, sizeof(backend));
    CHECK(create_view_fixture(&fixture));
    config = lifecycle_config(
        &backend,
        BOXRIG_RENDERER_INTEGRATION_RENDERER_ONLY,
        BOXRIG_RENDERER_RUNTIME_BORROWED,
        BOXRIG_RENDERER_PRESENTATION_CALLER);
    config.limits.maximum_draw_count = 1;
    CHECK(boxrig_renderer_lifecycle_create(&config, &lifecycle) ==
          BOXRIG_RENDERER_OK);
    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle, fixture.view, NULL, 7, &token) ==
          BOXRIG_RENDERER_LIMIT_EXCEEDED);
    CHECK(boxrig_ui_view_frame_requested(fixture.view));
    CHECK(backend.prepare_count == 1 && backend.abort_count == 1);

    boxrig_renderer_lifecycle_destroy(lifecycle);
    destroy_view_fixture(&fixture);
    return true;
}

static uint64_t analyzer_revision(void *context)
{
    (void)context;
    return 1;
}

static boxrig_ui_status failing_analyzer(void *context, const boxrig_terminal *terminal,
    boxrig_text_screen screen, size_t index, size_t maximum_bytes, boxrig_text_layout **out)
{
    (void)terminal; (void)screen; (void)index; (void)maximum_bytes;
    size_t *calls = context;
    (*calls)++;
    *out = NULL;
    return BOXRIG_UI_OUT_OF_MEMORY;
}

static bool caller_registered_analyzer_survives_optional_backend(void)
{
    view_fixture fixture;
    fake_backend backend = {0};
    CHECK(create_view_fixture(&fixture));
    boxrig_ui_allocation_input allocation = {0};
    allocation.allocation_width = 40*BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = 40*BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = 10*BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = 20*BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    boxrig_ui_layout_result layout;
    CHECK(boxrig_ui_view_acquire_layout_authority(fixture.view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_accept_allocation(fixture.view, &allocation, &layout) == BOXRIG_UI_OK);
    boxrig_ui_fragment_layout_provider *provider = NULL;
    size_t calls = 0;
    CHECK(boxrig_ui_fragment_layout_provider_create(failing_analyzer, analyzer_revision,
        &calls, &provider) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_fragment_layout_provider(fixture.view, provider) == BOXRIG_UI_OK);
    boxrig_renderer_lifecycle_config config = lifecycle_config(&backend,
        BOXRIG_RENDERER_INTEGRATION_RENDERER_ONLY, BOXRIG_RENDERER_RUNTIME_BORROWED,
        BOXRIG_RENDERER_PRESENTATION_CALLER);
    boxrig_renderer_lifecycle *lifecycle = NULL;
    CHECK(boxrig_renderer_lifecycle_create(&config, &lifecycle) == BOXRIG_RENDERER_OK);
    boxrig_renderer_frame_token token;
    CHECK(boxrig_renderer_lifecycle_prepare_frame(lifecycle, fixture.view, NULL, 7, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_renderer_lifecycle_abort_frame(lifecycle, &token) == BOXRIG_RENDERER_OK);
    boxrig_renderer_lifecycle_destroy(lifecycle);
    boxrig_ui_hit_test_result hit;
    CHECK(boxrig_ui_view_hit_test(fixture.view, BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    boxrig_ui_selection_options options = {BOXRIG_UI_SELECTION_RECTANGULAR, BOXRIG_UI_SELECTION_CHARACTER};
    CHECK(boxrig_ui_view_begin_selection_with_options(fixture.view, &options, &hit) == BOXRIG_UI_OUT_OF_MEMORY);
    CHECK(calls == 1);
    boxrig_ui_fragment_layout_provider_destroy(provider);
    destroy_view_fixture(&fixture);
    return true;
}

int main(void)
{
    if (!borrowed_renderer_only_retains_dirty_until_accept() ||
        !managed_component_handles_target_and_present_failures() ||
        !bounded_frame_work_is_rejected_without_consuming_dirty() ||
        !caller_registered_analyzer_survives_optional_backend()) return 1;
    printf("PASS private accelerated renderer lifecycle contract\n");
    return 0;
}
