/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TEST_FRONTEND_COMPONENT_FIXTURE_H
#define BOXRIG_TEST_FRONTEND_COMPONENT_FIXTURE_H

#include "boxrig/ui.h"
#include "frontend_input_fixture.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Current grouped frontend component fixture adapter contract. */
#define BOXRIG_TEST_FRONTEND_COMPONENT_FIXTURE_VERSION 7

/** Native presentation locations selected by the shared scene fixture. */
typedef struct boxrig_test_frontend_presentation_probe {
    int background_x;
    int background_y;
    int meaning_x;
    int meaning_y;
    int coverage_x;
    int coverage_y;
    unsigned int coverage_width;
    unsigned int coverage_height;
    int stable_x;
    int stable_y;
    unsigned int stable_width;
    unsigned int stable_height;
} boxrig_test_frontend_presentation_probe;

/** Normalized evidence captured from one frontend's native presentation. */
typedef struct boxrig_test_frontend_presentation_observation {
    uint64_t generation;
    unsigned int width;
    unsigned int height;
    uint32_t background_argb;
    uint32_t meaning_argb;
    size_t coverage_pixels;
    uint64_t stable_hash;
    bool alpha_observable;
} boxrig_test_frontend_presentation_observation;

typedef struct boxrig_test_frontend_lifecycle_operations {
    void *(*create_host)(void);
    void (*destroy_host)(void *host);
    boxrig_ui_status (*create_component)(
        void *host,
        boxrig_ui_model *model,
        boxrig_ui_component_attachment attachment,
        void **component_out);
    boxrig_ui_status (*quiesce_component)(void *component);
    void (*destroy_component)(void *component);
} boxrig_test_frontend_lifecycle_operations;

typedef struct boxrig_test_frontend_layout_operations {
    boxrig_ui_view *(*component_view)(void *component);
    bool (*get_effective_metrics)(
        void *component,
        boxrig_font_effective_metrics *metrics_out);
    boxrig_ui_status (*allocate)(
        void *component,
        const boxrig_ui_component_allocation *allocation,
        boxrig_ui_layout_result *layout_out);
    boxrig_ui_status (*set_preferred_grid)(
        void *component,
        size_t rows,
        size_t columns);
    bool (*get_preferred_grid)(
        void *component,
        size_t *rows_out,
        size_t *columns_out);
    boxrig_ui_size_command_state (*get_size_command_state)(void *component);
    bool (*take_layout_action)(
        void *component,
        boxrig_ui_layout_action *action_out);
    boxrig_ui_status (*complete_size_command)(
        void *component,
        uint64_t revision,
        boxrig_ui_size_outcome outcome,
        boxrig_ui_fixed allocation_width,
        boxrig_ui_fixed allocation_height,
        bool *applied_out,
        boxrig_ui_layout_result *layout_out);
    boxrig_ui_status (*complete_grid_resize)(
        void *component,
        uint64_t revision,
        bool succeeded);
    boxrig_ui_status (*set_visible)(void *component, bool visible);
} boxrig_test_frontend_layout_operations;

typedef struct boxrig_test_frontend_frame_operations {
    boxrig_ui_status (*set_frame_request_callback)(
        void *component,
        boxrig_ui_frame_request_callback callback,
        void *context);
    boxrig_ui_status (*frame)(
        void *component,
        boxrig_ui_component_frame_result *result_out);
} boxrig_test_frontend_frame_operations;

typedef struct boxrig_test_frontend_input_operations {
    boxrig_ui_status (*queue_paste)(
        void *component,
        const uint8_t *bytes,
        size_t length);
    bool (*run_native_transcript)(
        void *component,
        const boxrig_test_frontend_input_script *script,
        boxrig_test_frontend_input_observation *observation_out);
} boxrig_test_frontend_input_operations;

typedef struct boxrig_test_frontend_font_operations {
    const char *fixture_font_path;
    boxrig_font_catalog_status (*register_font_file)(
        void *component,
        const char *path,
        size_t path_length,
        boxrig_font_resource_id *resource_id_out);
    boxrig_ui_status (*set_specification)(
        void *component,
        const boxrig_font_specification *specification);
    boxrig_ui_status (*get_specification)(
        void *component,
        boxrig_font_specification *specification_out);
    boxrig_font_resource_id (*primary_resource)(void *component);
    uint64_t (*catalog_revision)(void *component);
    boxrig_ui_status (*set_line_height)(
        void *component,
        float adjustment_dip);
    boxrig_ui_status (*get_line_height)(
        void *component,
        float *adjustment_dip_out);
    boxrig_ui_status (*visual_zoom)(
        void *component,
        boxrig_font_zoom_direction direction,
        boxrig_font_visual_step_policy policy,
        bool *changed_out);
    boxrig_ui_status (*visual_zoom_reset)(
        void *component,
        bool *changed_out);
    boxrig_ui_status (*set_text_direction)(
        void *component,
        boxrig_text_direction_policy policy);
    boxrig_text_direction_policy (*text_direction)(void *component);
} boxrig_test_frontend_font_operations;

typedef struct boxrig_test_frontend_view_action_operations {
    bool (*copy_selection)(
        void *host,
        void *component,
        const uint8_t *expected,
        size_t expected_length);
    boxrig_ui_view_action_capabilities (*capabilities)(void *component);
    boxrig_ui_status (*apply)(
        void *component,
        const boxrig_ui_view_action *action);
    bool (*get_scrollbar_model)(
        void *component,
        boxrig_scrollbar_model *model_out);
    bool (*get_scrollbar_state)(void *component, boxrig_scrollbar_state *state_out);
    boxrig_ui_status (*apply_scrollbar_action)(
        void *component,
        boxrig_scrollbar_action action,
        size_t position,
        bool *changed_out);
} boxrig_test_frontend_view_action_operations;

typedef struct boxrig_test_frontend_presentation_operations {
    /* Explicit opaque backends require application recreation for alpha. */
    bool requires_recreation_for_transparency;
    boxrig_ui_status (*set_transparency)(
        void *component,
        const boxrig_ui_transparency_config *config);
    boxrig_ui_status (*get_transparency)(
        void *component,
        boxrig_ui_transparency_state *state_out);
    uint32_t (*capabilities)(void *component);
    boxrig_ui_status (*get_diagnostic)(
        void *component,
        boxrig_ui_component_diagnostic *diagnostic_out);
    boxrig_composition *(*composition)(void *component);
    bool (*observe)(
        void *component,
        const boxrig_test_frontend_presentation_probe *probe,
        boxrig_test_frontend_presentation_observation *observation_out);
} boxrig_test_frontend_presentation_operations;

typedef struct boxrig_test_frontend_component_operations {
    const char *frontend_name;
    boxrig_test_frontend_lifecycle_operations lifecycle;
    boxrig_test_frontend_layout_operations layout;
    boxrig_test_frontend_frame_operations frame;
    boxrig_test_frontend_input_operations input;
    boxrig_test_frontend_font_operations font;
    boxrig_test_frontend_view_action_operations view_action;
    boxrig_test_frontend_presentation_operations presentation;
} boxrig_test_frontend_component_operations;

bool boxrig_test_run_frontend_component_fixture(
    const boxrig_test_frontend_component_operations *operations);

#endif
