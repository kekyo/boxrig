/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/text_layout.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool test_bidi_cell_mapping_and_runs(void)
{
    const char logical_text[] = "ab\327\251\327\234\327\225\327\235\347\225\214!";
    const boxrig_text_cluster_input clusters[] = {
        {0, 1, 0, 1, 0, 1, 0, 10, 20, 30},
        {1, 1, 1, 1, 0, 1, 0, 10, 20, 30},
        {2, 1, 2, 2, 1, 2, 11, 21, 31, 41},
        {3, 1, 4, 2, 1, 2, 11, 21, 31, 41},
        {4, 1, 6, 2, 1, 2, 11, 21, 31, 41},
        {5, 1, 8, 2, 1, 2, 11, 21, 31, 41},
        {6, 2, 10, 3, 0, 3, 12, 22, 32, 42},
        {8, 1, 13, 1, 0, 1, 0, 10, 20, 30},
    };
    const size_t expected_logical_to_visual[] = {0, 1, 5, 4, 3, 2, 6, 7, 8};
    boxrig_text_layout *layout = NULL;
    size_t index;

    CHECK(boxrig_text_layout_create(
              clusters,
              sizeof(clusters) / sizeof(clusters[0]),
              9,
              BOXRIG_TEXT_DIRECTION_LTR,
              &layout) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(layout != NULL);
    CHECK(boxrig_text_layout_column_count(layout) == 9);
    CHECK(boxrig_text_layout_run_count(layout) == 4);
    for (index = 0; index < 9; index++) {
        size_t visual = 99;
        size_t logical = 99;

        CHECK(boxrig_text_layout_logical_to_visual(
                  layout,
                  index,
                  &visual) == BOXRIG_TEXT_LAYOUT_OK);
        CHECK(visual == expected_logical_to_visual[index]);
        CHECK(boxrig_text_layout_visual_to_logical(
                  layout,
                  visual,
                  &logical) == BOXRIG_TEXT_LAYOUT_OK);
        CHECK(logical == index);
    }
    {
        boxrig_text_layout_run run;

        CHECK(boxrig_text_layout_get_run(layout, 1, &run) ==
              BOXRIG_TEXT_LAYOUT_OK);
        CHECK(run.logical_column == 2);
        CHECK(run.logical_column_count == 4);
        CHECK(run.visual_column == 2);
        CHECK(run.visual_column_count == 4);
        CHECK(run.direction == BOXRIG_TEXT_DIRECTION_RTL);
        CHECK(run.bidi_level == 1);
        CHECK(run.style_key == 2);
        CHECK(run.hyperlink_key == 11);
        CHECK(run.font_key == 21);
        CHECK(run.script_key == 31);
        CHECK(run.language_key == 41);
    }
    CHECK(memcmp(
              logical_text,
              "ab\327\251\327\234\327\225\327\235\347\225\214!",
              sizeof(logical_text)) == 0);
    boxrig_text_layout_destroy(layout);
    return true;
}

static bool test_dirty_uses_shaping_context(void)
{
    const boxrig_text_cluster_input clusters[] = {
        {0, 1, 0, 2, 1, 1, 0, 7, 8, 9},
        {1, 1, 2, 2, 1, 1, 0, 7, 8, 9},
        {2, 1, 4, 2, 1, 1, 0, 7, 8, 9},
        {3, 1, 6, 2, 1, 2, 0, 7, 8, 9},
        {4, 2, 8, 3, 2, 3, 0, 7, 8, 9},
        {6, 1, 11, 1, 2, 3, 0, 7, 8, 9},
    };
    boxrig_text_layout *layout = NULL;
    boxrig_text_layout_range ranges[3];
    size_t required;

    CHECK(boxrig_text_layout_create(
              clusters,
              sizeof(clusters) / sizeof(clusters[0]),
              7,
              BOXRIG_TEXT_DIRECTION_RTL,
              &layout) == BOXRIG_TEXT_LAYOUT_OK);
    required = boxrig_text_layout_dirty_ranges(layout, 1, 1, ranges, 3);
    CHECK(required == 1);
    CHECK(ranges[0].column == 4);
    CHECK(ranges[0].column_count == 3);

    required = boxrig_text_layout_dirty_ranges(layout, 4, 1, ranges, 3);
    CHECK(required == 1);
    CHECK(ranges[0].column == 0);
    CHECK(ranges[0].column_count == 3);

    required = boxrig_text_layout_dirty_ranges(layout, 2, 3, ranges, 1);
    CHECK(required == 3);
    CHECK(ranges[0].column == 0);
    CHECK(ranges[0].column_count == 3);
    CHECK(boxrig_text_layout_dirty_ranges(layout, 7, 1, ranges, 3) == 0);
    boxrig_text_layout_destroy(layout);
    return true;
}

static bool test_visual_ranges_follow_exact_bidi_cells(void)
{
    const boxrig_text_cluster_input clusters[] = {
        {0, 1, 0, 1, 0, 1, 0, 1, 1, 1},
        {1, 1, 1, 1, 0, 1, 0, 1, 1, 1},
        {2, 1, 2, 2, 1, 1, 0, 1, 1, 1},
        {3, 1, 4, 2, 1, 1, 0, 1, 1, 1},
        {4, 1, 6, 2, 1, 1, 0, 1, 1, 1},
        {5, 1, 8, 1, 0, 1, 0, 1, 1, 1},
    };
    boxrig_text_layout_range ranges[3];
    boxrig_text_layout *layout = NULL;
    size_t required;

    CHECK(boxrig_text_layout_create(
              clusters,
              sizeof(clusters) / sizeof(clusters[0]),
              6,
              BOXRIG_TEXT_DIRECTION_LTR,
              &layout) == BOXRIG_TEXT_LAYOUT_OK);
    required = boxrig_text_layout_visual_ranges(layout, 1, 3, ranges, 3);
    CHECK(required == 2);
    CHECK(ranges[0].column == 1 && ranges[0].column_count == 1);
    CHECK(ranges[1].column == 3 && ranges[1].column_count == 2);
    required = boxrig_text_layout_visual_ranges(layout, 2, 3, ranges, 1);
    CHECK(required == 1);
    CHECK(ranges[0].column == 2 && ranges[0].column_count == 3);
    CHECK(boxrig_text_layout_visual_ranges(layout, 6, 1, ranges, 3) == 0);
    boxrig_text_layout_destroy(layout);
    return true;
}

static bool test_invalid_layout_is_rejected(void)
{
    const boxrig_text_cluster_input gap[] = {
        {0, 1, 0, 1, 0, 1, 0, 1, 1, 1},
        {2, 1, 1, 1, 0, 1, 0, 1, 1, 1},
    };
    const boxrig_text_cluster_input zero_width[] = {
        {0, 0, 0, 1, 0, 1, 0, 1, 1, 1},
    };
    boxrig_text_layout *layout = NULL;
    size_t mapped = 0;

    CHECK(boxrig_text_layout_create(
              gap,
              sizeof(gap) / sizeof(gap[0]),
              3,
              BOXRIG_TEXT_DIRECTION_LTR,
              &layout) == BOXRIG_TEXT_LAYOUT_INVALID_ARGUMENT);
    CHECK(layout == NULL);
    CHECK(boxrig_text_layout_create(
              zero_width,
              sizeof(zero_width) / sizeof(zero_width[0]),
              1,
              BOXRIG_TEXT_DIRECTION_LTR,
              &layout) == BOXRIG_TEXT_LAYOUT_INVALID_ARGUMENT);
    CHECK(boxrig_text_layout_create(
              NULL,
              0,
              0,
              BOXRIG_TEXT_DIRECTION_LTR,
              &layout) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(boxrig_text_layout_logical_to_visual(layout, 0, &mapped) ==
          BOXRIG_TEXT_LAYOUT_INVALID_ARGUMENT);
    CHECK(boxrig_text_layout_get_run(layout, 0, NULL) ==
          BOXRIG_TEXT_LAYOUT_INVALID_ARGUMENT);
    boxrig_text_layout_destroy(layout);
    return true;
}

static bool test_portable_font_defaults(void)
{
    boxrig_font_specification specification =
        boxrig_font_specification_default();
    boxrig_font_rendering_policy policy =
        boxrig_font_rendering_policy_default();
    boxrig_font_catalog_limits limits = boxrig_font_catalog_limits_default();

    CHECK(specification.families == NULL);
    CHECK(specification.family_count == 0);
    CHECK(specification.size_dip > 0.0f);
    CHECK(specification.line_height_adjustment_dip == 0.0f);
    CHECK(specification.weight == 400);
    CHECK(specification.style == BOXRIG_FONT_STYLE_NORMAL);
    CHECK(specification.stretch == BOXRIG_FONT_STRETCH_NORMAL);
    CHECK(specification.fallback == BOXRIG_FONT_FALLBACK_SYSTEM);
    CHECK(specification.monospace == BOXRIG_FONT_MONOSPACE_REQUIRED);
    CHECK(specification.features == NULL);
    CHECK(specification.feature_count == 0);
    CHECK(specification.variations == NULL);
    CHECK(specification.variation_count == 0);
    CHECK(policy.dpi_x == 96.0f);
    CHECK(policy.dpi_y == 96.0f);
    CHECK(policy.output_scale == 1.0f);
    CHECK(policy.antialias == BOXRIG_FONT_ANTIALIAS_SYSTEM);
    CHECK(policy.hinting == BOXRIG_FONT_HINTING_SYSTEM);
    CHECK(policy.fractional_positions);
    CHECK(policy.color_glyphs);
    CHECK(limits.maximum_files > 0);
    CHECK(limits.maximum_bytes > 0);
    CHECK(limits.maximum_faces >= limits.maximum_files);
    return true;
}

typedef struct metrics_resolver_context {
    size_t call_count;
} metrics_resolver_context;

static boxrig_font_catalog_status resolve_test_metrics(
    float requested_size_dip,
    void *opaque_context,
    boxrig_font_effective_metrics *metrics_out)
{
    metrics_resolver_context *context = opaque_context;
    unsigned int cell_width = (unsigned int)floorf(requested_size_dip / 2.0f);

    context->call_count++;
    memset(metrics_out, 0, sizeof(*metrics_out));
    metrics_out->requested_size_dip = requested_size_dip;
    metrics_out->primary_resource_id = 31;
    metrics_out->primary_face_key = UINT64_C(0x12345678);
    metrics_out->dpi_x = 144.0f;
    metrics_out->dpi_y = 144.0f;
    metrics_out->output_scale = 1.25f;
    metrics_out->glyph_width = (float)cell_width - 0.125f;
    metrics_out->ascent = requested_size_dip * 0.75f;
    metrics_out->descent = requested_size_dip * 0.25f;
    metrics_out->glyph_height =
        metrics_out->ascent + metrics_out->descent;
    metrics_out->line_gap = 0.25f;
    metrics_out->cell_width = cell_width;
    metrics_out->cell_height =
        (unsigned int)ceilf(metrics_out->glyph_height +
                            metrics_out->line_gap);
    metrics_out->baseline = (unsigned int)ceilf(metrics_out->ascent);
    metrics_out->layout_revision = 9;
    return BOXRIG_FONT_CATALOG_OK;
}

static bool test_effective_metrics_and_visual_steps(void)
{
    metrics_resolver_context context = {0};
    boxrig_font_effective_metrics natural;
    boxrig_font_effective_metrics current;
    boxrig_font_effective_metrics candidate;
    boxrig_font_effective_metrics round_trip;
    boxrig_font_visual_step_policy policy = {8.0f, 12.0f, 0.5f};
    boxrig_font_visual_step_policy default_policy =
        boxrig_font_visual_step_policy_default();
    boxrig_font_visual_step_sequence *sequence = NULL;
    unsigned int natural_height;
    unsigned int natural_baseline;

    CHECK(default_policy.minimum_size_dip == 6.0f);
    CHECK(default_policy.maximum_size_dip == 72.0f);
    /* Match the stored float even when x87 evaluates literals with extra precision. */
    CHECK(default_policy.candidate_increment_dip == (float)0.1f);
    CHECK(resolve_test_metrics(10.0f, &context, &natural) ==
          BOXRIG_FONT_CATALOG_OK);
    natural_height = natural.cell_height;
    natural_baseline = natural.baseline;
    CHECK(boxrig_font_effective_metrics_apply_line_height(
              &natural,
              2.0f,
              &candidate) == BOXRIG_FONT_CATALOG_OK);
    CHECK(candidate.requested_size_dip == natural.requested_size_dip);
    CHECK(candidate.primary_face_key == natural.primary_face_key);
    CHECK(candidate.cell_width == natural.cell_width);
    CHECK(candidate.glyph_width == natural.glyph_width);
    CHECK(candidate.glyph_height == natural.glyph_height);
    CHECK(candidate.line_gap == natural.line_gap);
    CHECK(candidate.line_height_adjustment == 3.75f);
    CHECK(candidate.cell_height == natural_height + 4);
    CHECK(candidate.baseline == natural_baseline + 2);
    natural.line_gap = -0.25f;
    CHECK(boxrig_font_effective_metrics_apply_line_height(
              &natural,
              0.0f,
              &candidate) == BOXRIG_FONT_CATALOG_OK);
    CHECK(candidate.line_gap == -0.25f);
    natural.line_gap = 0.25f;
    CHECK(boxrig_font_effective_metrics_apply_line_height(
              &natural,
              1.0f,
              &current) == BOXRIG_FONT_CATALOG_OK);

    context.call_count = 0;
    CHECK(boxrig_font_visual_step_sequence_create(
              &current,
              1.0f,
              policy,
              resolve_test_metrics,
              &context,
              &sequence) == BOXRIG_FONT_CATALOG_OK);
    CHECK(sequence != NULL);
    CHECK(context.call_count == 0);
    CHECK(boxrig_font_visual_step_sequence_current(sequence, &round_trip));
    CHECK(memcmp(&round_trip, &current, sizeof(current)) == 0);

    CHECK(boxrig_font_visual_step_sequence_peek(
        sequence,
        BOXRIG_FONT_ZOOM_INCREASE,
        &candidate));
    CHECK(candidate.requested_size_dip == 12.0f);
    CHECK(candidate.cell_width == 6);
    CHECK(candidate.line_height_adjustment == current.line_height_adjustment);
    CHECK(context.call_count == 4);
    CHECK(boxrig_font_visual_step_sequence_peek(
        sequence,
        BOXRIG_FONT_ZOOM_INCREASE,
        &round_trip));
    CHECK(memcmp(&round_trip, &candidate, sizeof(candidate)) == 0);
    CHECK(context.call_count == 4);
    CHECK(boxrig_font_visual_step_sequence_peek(
        sequence,
        BOXRIG_FONT_ZOOM_DECREASE,
        &candidate));
    CHECK(candidate.requested_size_dip == 9.5f);
    CHECK(candidate.cell_width == 4);
    CHECK(context.call_count == 5);
    CHECK(boxrig_font_visual_step_sequence_peek(
        sequence,
        BOXRIG_FONT_ZOOM_DECREASE,
        &round_trip));
    CHECK(memcmp(&round_trip, &candidate, sizeof(candidate)) == 0);
    CHECK(context.call_count == 5);

    CHECK(boxrig_font_visual_step_sequence_advance(
        sequence,
        BOXRIG_FONT_ZOOM_INCREASE));
    CHECK(context.call_count == 5);
    CHECK(boxrig_font_visual_step_sequence_peek_reset(
        sequence,
        &candidate));
    CHECK(memcmp(&candidate, &current, sizeof(current)) == 0);
    CHECK(boxrig_font_visual_step_sequence_reset(sequence));
    CHECK(boxrig_font_visual_step_sequence_current(sequence, &round_trip));
    CHECK(memcmp(&round_trip, &current, sizeof(current)) == 0);
    CHECK(!boxrig_font_visual_step_sequence_reset(sequence));

    CHECK(boxrig_font_visual_step_sequence_advance(
        sequence,
        BOXRIG_FONT_ZOOM_INCREASE));
    CHECK(!boxrig_font_visual_step_sequence_peek(
        sequence,
        BOXRIG_FONT_ZOOM_INCREASE,
        &candidate));
    CHECK(boxrig_font_visual_step_sequence_peek(
        sequence,
        BOXRIG_FONT_ZOOM_DECREASE,
        &candidate));
    CHECK(candidate.requested_size_dip == 10.0f);
    CHECK(boxrig_font_visual_step_sequence_advance(
        sequence,
        BOXRIG_FONT_ZOOM_DECREASE));
    CHECK(boxrig_font_visual_step_sequence_current(sequence, &round_trip));
    CHECK(memcmp(&round_trip, &current, sizeof(current)) == 0);
    CHECK(!boxrig_font_visual_step_sequence_advance(
        sequence,
        (boxrig_font_zoom_direction)-1));
    boxrig_font_visual_step_sequence_destroy(sequence);

    sequence = (boxrig_font_visual_step_sequence *)(uintptr_t)1;
    policy.candidate_increment_dip = 0.0f;
    CHECK(boxrig_font_visual_step_sequence_create(
              &current,
              1.0f,
              policy,
              resolve_test_metrics,
              &context,
              &sequence) == BOXRIG_FONT_CATALOG_INVALID_ARGUMENT);
    CHECK(sequence == NULL);
    CHECK(boxrig_font_effective_metrics_apply_line_height(
              &natural,
              NAN,
              &candidate) == BOXRIG_FONT_CATALOG_INVALID_ARGUMENT);
    return true;
}

typedef struct validation_context {
    const char *expected_path;
    size_t call_count;
} validation_context;

static boxrig_font_catalog_status validate_font_resource(
    const boxrig_font_resource_candidate *candidate,
    size_t *byte_count_out,
    size_t *face_count_out,
    void *opaque_context)
{
    validation_context *context = opaque_context;

    context->call_count++;
    if (candidate->kind == BOXRIG_FONT_RESOURCE_FILE) {
        FILE *file;
        long length;

        if (candidate->path_length != strlen(context->expected_path) ||
            memcmp(
                candidate->path,
                context->expected_path,
                candidate->path_length) != 0) {
            return BOXRIG_FONT_CATALOG_INVALID_FONT;
        }
        file = fopen(context->expected_path, "rb");
        if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
            if (file != NULL) {
                fclose(file);
            }
            return BOXRIG_FONT_CATALOG_IO_ERROR;
        }
        length = ftell(file);
        fclose(file);
        if (length <= 0) {
            return BOXRIG_FONT_CATALOG_INVALID_FONT;
        }
        *byte_count_out = (size_t)length;
        *face_count_out = 2;
        return BOXRIG_FONT_CATALOG_OK;
    }
    if (candidate->kind != BOXRIG_FONT_RESOURCE_MEMORY ||
        candidate->data_length < 4 ||
        memcmp(candidate->data, "FONT", 4) != 0) {
        return BOXRIG_FONT_CATALOG_INVALID_FONT;
    }
    *byte_count_out = candidate->data_length;
    *face_count_out = 1;
    return BOXRIG_FONT_CATALOG_OK;
}

static bool test_font_catalog_resources_and_limits(void)
{
    static const char fixture_path[] =
        "tests/fixtures/font_catalog_resource.bin";
    boxrig_font_catalog_limits limits = {2, 64, 3};
    validation_context context = {fixture_path, 0};
    uint8_t memory_font[] = {'F', 'O', 'N', 'T', 1};
    const uint8_t invalid_font[] = {'N', 'O', 'P', 'E'};
    boxrig_font_catalog *catalog = NULL;
    boxrig_font_resource resource;
    boxrig_font_resource_id memory_id = BOXRIG_FONT_RESOURCE_NONE;
    boxrig_font_resource_id file_id = BOXRIG_FONT_RESOURCE_NONE;
    boxrig_font_family family;
    boxrig_font_specification specification =
        boxrig_font_specification_default();
    size_t file_count;
    size_t byte_count;
    size_t face_count;
    uint64_t revision;

    CHECK(boxrig_font_catalog_create(
              limits,
              validate_font_resource,
              &context,
              &catalog) == BOXRIG_FONT_CATALOG_OK);
    CHECK(catalog != NULL);
    CHECK(boxrig_font_catalog_revision(catalog) == 1);
    CHECK(boxrig_font_catalog_register_memory(
              catalog,
              memory_font,
              sizeof(memory_font),
              0,
              &memory_id) == BOXRIG_FONT_CATALOG_OK);
    CHECK(memory_id != BOXRIG_FONT_RESOURCE_NONE);
    memory_font[0] = 'X';
    CHECK(boxrig_font_catalog_get_resource(
              catalog,
              memory_id,
              &resource) == BOXRIG_FONT_CATALOG_OK);
    CHECK(resource.kind == BOXRIG_FONT_RESOURCE_MEMORY);
    CHECK(resource.path == NULL);
    CHECK(resource.data != memory_font);
    CHECK(resource.data_length == sizeof(memory_font));
    CHECK(memcmp(resource.data, "FONT\1", sizeof(memory_font)) == 0);
    CHECK(resource.byte_count == sizeof(memory_font));
    CHECK(resource.face_index == 0);
    CHECK(resource.face_count == 1);

    CHECK(boxrig_font_catalog_register_file(
              catalog,
              fixture_path,
              strlen(fixture_path),
              1,
              &file_id) == BOXRIG_FONT_CATALOG_OK);
    CHECK(file_id != BOXRIG_FONT_RESOURCE_NONE && file_id != memory_id);
    CHECK(boxrig_font_catalog_get_resource(catalog, file_id, &resource) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(resource.kind == BOXRIG_FONT_RESOURCE_FILE);
    CHECK(resource.path != fixture_path);
    CHECK(resource.path_length == strlen(fixture_path));
    CHECK(memcmp(resource.path, fixture_path, resource.path_length) == 0);
    CHECK(resource.path[resource.path_length] == '\0');
    CHECK(resource.data == NULL);
    CHECK(resource.data_length == 0);
    CHECK(resource.face_index == 1);
    CHECK(resource.face_count == 2);
    CHECK(boxrig_font_catalog_usage(
              catalog,
              &file_count,
              &byte_count,
              &face_count) == BOXRIG_FONT_CATALOG_OK);
    CHECK(file_count == 2);
    CHECK(byte_count == sizeof(memory_font) + resource.byte_count);
    CHECK(face_count == 3);
    CHECK(context.call_count == 2);

    CHECK(boxrig_font_catalog_register_memory(
              catalog,
              invalid_font,
              sizeof(invalid_font),
              0,
              &file_id) == BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED);
    CHECK(context.call_count == 2);
    family.name = NULL;
    family.name_length = 0;
    family.resource_id = memory_id;
    specification.families = &family;
    specification.family_count = 1;
    specification.fallback = BOXRIG_FONT_FALLBACK_REGISTERED_ONLY;
    CHECK(boxrig_font_specification_validate(catalog, &specification) ==
          BOXRIG_FONT_CATALOG_OK);

    revision = boxrig_font_catalog_revision(catalog);
    CHECK(boxrig_font_catalog_remove(catalog, memory_id) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_font_catalog_revision(catalog) == revision + 1);
    CHECK(boxrig_font_catalog_get_resource(catalog, memory_id, &resource) ==
          BOXRIG_FONT_CATALOG_NOT_FOUND);
    CHECK(boxrig_font_specification_validate(catalog, &specification) ==
          BOXRIG_FONT_CATALOG_NOT_FOUND);
    CHECK(boxrig_font_catalog_remove(catalog, memory_id) ==
          BOXRIG_FONT_CATALOG_NOT_FOUND);
    CHECK(boxrig_font_catalog_register_memory(
              catalog,
              invalid_font,
              sizeof(invalid_font),
              0,
              &memory_id) == BOXRIG_FONT_CATALOG_INVALID_FONT);
    CHECK(memory_id == BOXRIG_FONT_RESOURCE_NONE);
    CHECK(context.call_count == 3);
    boxrig_font_catalog_destroy(catalog);
    return true;
}

static bool test_font_configuration_validation(void)
{
    boxrig_font_specification specification =
        boxrig_font_specification_default();
    boxrig_font_rendering_policy policy =
        boxrig_font_rendering_policy_default();
    boxrig_font_variation variation = {UINT32_C(0x77676874), NAN};
    boxrig_font_catalog_limits invalid_limits = {0, 1, 1};
    boxrig_font_catalog *catalog = (boxrig_font_catalog *)(uintptr_t)1;

    CHECK(boxrig_font_specification_validate(NULL, &specification) ==
          BOXRIG_FONT_CATALOG_OK);
    specification.size_dip = 0.0f;
    CHECK(boxrig_font_specification_validate(NULL, &specification) ==
          BOXRIG_FONT_CATALOG_INVALID_ARGUMENT);
    specification = boxrig_font_specification_default();
    specification.line_height_adjustment_dip = -1.0f;
    CHECK(boxrig_font_specification_validate(NULL, &specification) ==
          BOXRIG_FONT_CATALOG_INVALID_ARGUMENT);
    specification = boxrig_font_specification_default();
    specification.style = (boxrig_font_style)-1;
    CHECK(boxrig_font_specification_validate(NULL, &specification) ==
          BOXRIG_FONT_CATALOG_INVALID_ARGUMENT);
    specification = boxrig_font_specification_default();
    specification.variations = &variation;
    specification.variation_count = 1;
    CHECK(boxrig_font_specification_validate(NULL, &specification) ==
          BOXRIG_FONT_CATALOG_INVALID_ARGUMENT);
    policy.dpi_x = 0.0f;
    CHECK(boxrig_font_rendering_policy_validate(&policy) ==
          BOXRIG_FONT_CATALOG_INVALID_ARGUMENT);
    policy = boxrig_font_rendering_policy_default();
    policy.antialias = (boxrig_font_antialias)-1;
    CHECK(boxrig_font_rendering_policy_validate(&policy) ==
          BOXRIG_FONT_CATALOG_INVALID_ARGUMENT);
    CHECK(boxrig_font_catalog_create(
              invalid_limits,
              NULL,
              NULL,
              &catalog) == BOXRIG_FONT_CATALOG_INVALID_ARGUMENT);
    CHECK(catalog == NULL);
    return true;
}

int main(void)
{
    if (!test_bidi_cell_mapping_and_runs() ||
        !test_dirty_uses_shaping_context() ||
        !test_visual_ranges_follow_exact_bidi_cells() ||
        !test_invalid_layout_is_rejected() ||
        !test_portable_font_defaults() ||
        !test_effective_metrics_and_visual_steps() ||
        !test_font_catalog_resources_and_limits() ||
        !test_font_configuration_validation()) {
        return 1;
    }
    printf("PASS portable multilingual text layout contract\n");
    return 0;
}
