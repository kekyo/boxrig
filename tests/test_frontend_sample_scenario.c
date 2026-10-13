/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "frontend_sample_scenario.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

typedef struct scenario_fixture {
    const uint8_t *input;
    size_t input_length;
    size_t input_offset;
    uint8_t output[4096];
    size_t output_length;
    char titles[19][96];
    size_t title_count;
    size_t grid_query_count;
    bool unexpected_grid;
    unsigned int queried_rows[7];
    unsigned int queried_columns[7];
} scenario_fixture;

static bool write_terminal(
    void *opaque_fixture,
    const uint8_t *bytes,
    size_t length)
{
    scenario_fixture *fixture = opaque_fixture;

    if (bytes == NULL || length > sizeof(fixture->output) - fixture->output_length) {
        return false;
    }
    memcpy(fixture->output + fixture->output_length, bytes, length);
    fixture->output_length += length;
    return true;
}

static bool read_terminal(
    void *opaque_fixture,
    uint8_t *bytes,
    size_t length)
{
    scenario_fixture *fixture = opaque_fixture;

    if (bytes == NULL || length > fixture->input_length - fixture->input_offset) {
        return false;
    }
    memcpy(bytes, fixture->input + fixture->input_offset, length);
    fixture->input_offset += length;
    return true;
}

static bool set_title(void *opaque_fixture, const char *title)
{
    scenario_fixture *fixture = opaque_fixture;
    size_t length;

    if (title == NULL ||
        fixture->title_count >=
            sizeof(fixture->titles) / sizeof(fixture->titles[0])) {
        return false;
    }
    length = strlen(title);
    if (length >= sizeof(fixture->titles[0])) {
        return false;
    }
    memcpy(fixture->titles[fixture->title_count], title, length + 1u);
    fixture->title_count++;
    return true;
}

static bool query_grid(
    void *opaque_fixture,
    unsigned int *rows_out,
    unsigned int *columns_out)
{
    scenario_fixture *fixture = opaque_fixture;

    if (rows_out == NULL || columns_out == NULL) {
        return false;
    }
    fixture->grid_query_count++;
    if (fixture->queried_rows[0] != 0u) {
        if (fixture->grid_query_count >
            sizeof(fixture->queried_rows) / sizeof(fixture->queried_rows[0])) {
            return false;
        }
        *rows_out = fixture->queried_rows[fixture->grid_query_count - 1u];
        *columns_out = fixture->queried_columns[fixture->grid_query_count - 1u];
        return true;
    }
    *rows_out = fixture->unexpected_grid
                    ? BOXRIG_TEST_FRONTEND_SAMPLE_ROWS - 1u
                    : BOXRIG_TEST_FRONTEND_SAMPLE_ROWS;
    *columns_out = BOXRIG_TEST_FRONTEND_SAMPLE_COLUMNS;
    return true;
}

static bool output_contains(
    const scenario_fixture *fixture,
    const char *expected)
{
    size_t expected_length = strlen(expected);
    size_t offset;

    if (expected_length > fixture->output_length) {
        return false;
    }
    for (offset = 0u;
         offset <= fixture->output_length - expected_length;
         offset++) {
        if (memcmp(
                fixture->output + offset,
                expected,
                expected_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool test_shared_sample_scenario(void)
{
    static const uint8_t input[] =
        "\033[6;18;9t"
        "k"
        "copy-target"
        "m"
        "\033[<0;21;9M\033[<0;21;9m"
        "copy-target"
        "copy-\nblock"
        "g"
        "g"
        "q";
    static const char *const expected_titles[] = {
        "boxrig-scenario-ready-18-9",
        BOXRIG_TEST_FRONTEND_SAMPLE_KEY_TITLE,
        BOXRIG_TEST_FRONTEND_SAMPLE_LINEAR_TITLE,
        BOXRIG_TEST_FRONTEND_SAMPLE_MOUSE_READY_TITLE,
        BOXRIG_TEST_FRONTEND_SAMPLE_MOUSE_TITLE,
        BOXRIG_TEST_FRONTEND_SAMPLE_SHIFT_TITLE,
        BOXRIG_TEST_FRONTEND_SAMPLE_RECTANGLE_TITLE,
        BOXRIG_TEST_FRONTEND_SAMPLE_ZOOM_TITLE,
        BOXRIG_TEST_FRONTEND_SAMPLE_RESET_TITLE};
    scenario_fixture fixture;
    boxrig_test_frontend_sample_scenario_operations operations;
    size_t index;

    memset(&fixture, 0, sizeof(fixture));
    fixture.input = input;
    fixture.input_length = sizeof(input) - 1u;
    memset(&operations, 0, sizeof(operations));
    operations.context = &fixture;
    operations.write_terminal = write_terminal;
    operations.read_terminal = read_terminal;
    operations.set_title = set_title;
    operations.query_grid = query_grid;

    CHECK(boxrig_test_run_frontend_sample_scenario(&operations));
    CHECK(fixture.input_offset == fixture.input_length);
    CHECK(fixture.grid_query_count == 2u);
    CHECK(fixture.title_count ==
          sizeof(expected_titles) / sizeof(expected_titles[0]));
    for (index = 0u; index < fixture.title_count; index++) {
        CHECK(strcmp(fixture.titles[index], expected_titles[index]) == 0);
    }
    CHECK(output_contains(&fixture, "copy-target\r\nblock-data"));
    CHECK(output_contains(&fixture, "\347\225\214"));
    CHECK(output_contains(&fixture, "https://example.test/recognized"));
    CHECK(output_contains(&fixture, "\033]8;;https://example.test/link"));
    CHECK(output_contains(&fixture, "\033_G"));
    CHECK(output_contains(&fixture, "\033[?1002;1006h"));
    return true;
}

static bool test_sample_paste_pressure_preserves_input_order(void)
{
    static const uint8_t input[] =
        "\033[6;18;9t"
        "k"
        "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
        "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
        "copy-target"
        "m"
        "\033[<0;21;9M\033[<0;21;9m"
        "copy-target"
        "copy-\nblock"
        "g"
        "g"
        "q";
    scenario_fixture fixture;
    boxrig_test_frontend_sample_scenario_operations operations;

    memset(&fixture, 0, sizeof(fixture));
    fixture.input = input;
    fixture.input_length = sizeof(input) - 1u;
    memset(&operations, 0, sizeof(operations));
    operations.context = &fixture;
    operations.expect_paste_pressure = true;
    operations.write_terminal = write_terminal;
    operations.read_terminal = read_terminal;
    operations.set_title = set_title;
    operations.query_grid = query_grid;

    CHECK(boxrig_test_run_frontend_sample_scenario(&operations));
    CHECK(fixture.input_offset == fixture.input_length);
    CHECK(fixture.title_count == 9u);
    CHECK(strcmp(
              fixture.titles[2],
              BOXRIG_TEST_FRONTEND_SAMPLE_LINEAR_TITLE) == 0);
    CHECK(fixture.grid_query_count == 2u);
    CHECK(strcmp(
              fixture.titles[8],
              BOXRIG_TEST_FRONTEND_SAMPLE_RESET_TITLE) == 0);
    return true;
}

static bool test_sample_overlays_require_native_checkpoints_and_ime_commit(void)
{
    static const uint8_t input[] =
        "\033[6;18;9t"
        "k"
        "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
        "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
        "copy-target"
        "m"
        "\033[<0;21;9M\033[<0;21;9m"
        "copy-target"
        "copy-\nblock"
        "g"
        "g"
        "v"
        BOXRIG_TEST_FRONTEND_SAMPLE_IME_COMMIT
        "v"
        "v"
        "v"
        "q";
    static const char *const expected_titles[] = {
        BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_READY_TITLE,
        BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_UPDATED_TITLE,
        BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_COMMIT_TITLE,
        BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_OPACITY_TITLE,
        BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_CLEARED_TITLE,
        BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_VERIFIED_TITLE};
    scenario_fixture fixture;
    boxrig_test_frontend_sample_scenario_operations operations;
    size_t index;

    memset(&fixture, 0, sizeof(fixture));
    fixture.input = input;
    fixture.input_length = sizeof(input) - 1u;
    memset(&operations, 0, sizeof(operations));
    operations.context = &fixture;
    operations.expect_paste_pressure = true;
    operations.expect_overlays = true;
    operations.write_terminal = write_terminal;
    operations.read_terminal = read_terminal;
    operations.set_title = set_title;
    operations.query_grid = query_grid;

    CHECK(boxrig_test_run_frontend_sample_scenario(&operations));
    CHECK(fixture.input_offset == fixture.input_length);
    CHECK(fixture.title_count == 15u);
    CHECK(fixture.grid_query_count == 2u);
    for (index = 0u; index < sizeof(expected_titles) / sizeof(expected_titles[0]);
         index++) {
        CHECK(strcmp(fixture.titles[index + 9u], expected_titles[index]) == 0);
    }
    CHECK(output_contains(
        &fixture,
        BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_OSC8_TEXT));
    CHECK(output_contains(
        &fixture,
        BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_URL_TEXT));
    CHECK(output_contains(&fixture, "overlay-search marker"));
    return true;
}

static bool test_sample_overlays_reject_corrupted_ime_commit(void)
{
    static const uint8_t input[] =
        "\033[6;18;9tkcopy-targetm"
        "\033[<0;21;9M\033[<0;21;9m"
        "copy-targetcopy-\nblockggv"
        "ime-???q";
    scenario_fixture fixture;
    boxrig_test_frontend_sample_scenario_operations operations;

    memset(&fixture, 0, sizeof(fixture));
    fixture.input = input;
    fixture.input_length = sizeof(input) - 1u;
    memset(&operations, 0, sizeof(operations));
    operations.context = &fixture;
    operations.expect_overlays = true;
    operations.write_terminal = write_terminal;
    operations.read_terminal = read_terminal;
    operations.set_title = set_title;
    operations.query_grid = query_grid;

    CHECK(!boxrig_test_run_frontend_sample_scenario(&operations));
    CHECK(fixture.input_offset == fixture.input_length);
    CHECK(fixture.title_count == 12u);
    CHECK(strcmp(fixture.titles[11],
        BOXRIG_TEST_FRONTEND_SAMPLE_UNEXPECTED_PREFIX
        "overlay-ime-696d652d3f3f3f") == 0);
    return true;
}

static bool test_sample_geometry_requires_resized_and_restored_child_grids(void)
{
    static const uint8_t input[] =
        "\033[6;18;9tkcopy-targetm"
        "\033[<0;21;9M\033[<0;21;9m"
        "copy-targetcopy-\nblockggrrmzmq";
    static const char *const titles[] = {
        BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PREFIX "resize-grid-28-88",
        BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PREFIX "resize-reset-grid-24-80",
        BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PREFIX "maximize-grid-40-120",
        BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PREFIX "maximize-zoom-grid-48-144",
        BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PREFIX "maximize-reset-grid-24-80"};
    scenario_fixture fixture = {
        .input = input,
        .input_length = sizeof(input) - 1u,
        .queried_rows = {24u, 24u, 28u, 24u, 40u, 48u, 24u},
        .queried_columns = {80u, 80u, 88u, 80u, 120u, 144u, 80u}};
    boxrig_test_frontend_sample_scenario_operations operations = {
        .context = &fixture,
        .expect_geometry = true,
        .write_terminal = write_terminal,
        .read_terminal = read_terminal,
        .set_title = set_title,
        .query_grid = query_grid};
    size_t index;

    CHECK(boxrig_test_run_frontend_sample_scenario(&operations));
    CHECK(fixture.input_offset == fixture.input_length);
    CHECK(fixture.title_count == 14u && fixture.grid_query_count == 7u);
    for (index = 0u; index < sizeof(titles) / sizeof(titles[0]); index++) {
        CHECK(strcmp(fixture.titles[index + 9u], titles[index]) == 0);
    }
    return true;
}

static bool test_sample_geometry_rejects_an_unconverged_resize(void)
{
    static const uint8_t input[] =
        "\033[6;18;9tkcopy-targetm"
        "\033[<0;21;9M\033[<0;21;9m"
        "copy-targetcopy-\nblockggrq";
    scenario_fixture fixture = {
        .input = input,
        .input_length = sizeof(input) - 1u,
        .queried_rows = {24u, 24u, 29u},
        .queried_columns = {80u, 80u, 88u}};
    boxrig_test_frontend_sample_scenario_operations operations = {
        .context = &fixture,
        .expect_geometry = true,
        .write_terminal = write_terminal,
        .read_terminal = read_terminal,
        .set_title = set_title,
        .query_grid = query_grid};

    CHECK(!boxrig_test_run_frontend_sample_scenario(&operations));
    CHECK(fixture.input_offset == fixture.input_length);
    CHECK(fixture.title_count == 10u && fixture.grid_query_count == 3u);
    CHECK(strcmp(fixture.titles[9],
        BOXRIG_TEST_FRONTEND_SAMPLE_UNEXPECTED_PREFIX
        "geometry-resize-grid-29-88") == 0);
    return true;
}

static bool test_sample_geometry_reports_fixed_console_values_as_p2(void)
{
    static const uint8_t input[] =
        "\033[6;18;9tkcopy-targetm"
        "\033[<0;21;9M\033[<0;21;9m"
        "copy-targetcopy-\nblockggrrmzmq";
    scenario_fixture fixture = {
        .input = input,
        .input_length = sizeof(input) - 1u};
    boxrig_test_frontend_sample_scenario_operations operations = {
        .context = &fixture,
        .expect_geometry = true,
        .expect_fixed_console_grid = true,
        .write_terminal = write_terminal,
        .read_terminal = read_terminal,
        .set_title = set_title,
        .query_grid = query_grid};

    CHECK(boxrig_test_run_frontend_sample_scenario(&operations));
    CHECK(fixture.input_offset == fixture.input_length);
    CHECK(fixture.title_count == 14u && fixture.grid_query_count == 7u);
    CHECK(strcmp(fixture.titles[9],
        BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PREFIX "resize-grid-24-80"
        BOXRIG_TEST_FRONTEND_SAMPLE_FIXED_GRID_SUFFIX) == 0);
    CHECK(strcmp(fixture.titles[11],
        BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PREFIX "maximize-grid-24-80"
        BOXRIG_TEST_FRONTEND_SAMPLE_FIXED_GRID_SUFFIX) == 0);
    CHECK(strcmp(fixture.titles[12],
        BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PREFIX "maximize-zoom-grid-24-80"
        BOXRIG_TEST_FRONTEND_SAMPLE_FIXED_GRID_SUFFIX) == 0);
    return true;
}

static bool test_selection_override_rejects_leaked_mouse_input(void)
{
    static const uint8_t input[] =
        "\033[6;18;9t"
        "k"
        "copy-target"
        "m"
        "\033[<0;21;9M\033[<0;21;9m"
        "\033[<4;1;3M\033[<4;11;3m"
        "copy-target";
    static const char expected_prefix[] =
        BOXRIG_TEST_FRONTEND_SAMPLE_UNEXPECTED_PREFIX "shift-";
    scenario_fixture fixture;
    boxrig_test_frontend_sample_scenario_operations operations;

    memset(&fixture, 0, sizeof(fixture));
    fixture.input = input;
    fixture.input_length = sizeof(input) - 1u;
    memset(&operations, 0, sizeof(operations));
    operations.context = &fixture;
    operations.write_terminal = write_terminal;
    operations.read_terminal = read_terminal;
    operations.set_title = set_title;
    operations.query_grid = query_grid;

    CHECK(!boxrig_test_run_frontend_sample_scenario(&operations));
    CHECK(fixture.title_count == 6u);
    CHECK(strncmp(
              fixture.titles[fixture.title_count - 1u],
              expected_prefix,
              sizeof(expected_prefix) - 1u) == 0);
    CHECK(fixture.grid_query_count == 0u);
    return true;
}

static bool test_grid_mismatch_reports_observed_dimensions(void)
{
    static const uint8_t input[] =
        "\033[6;18;9t"
        "k"
        "copy-target"
        "m"
        "\033[<0;21;9M\033[<0;21;9m"
        "copy-target"
        "copy-\nblock"
        "g"
        "q";
    scenario_fixture fixture;
    boxrig_test_frontend_sample_scenario_operations operations;

    memset(&fixture, 0, sizeof(fixture));
    fixture.input = input;
    fixture.input_length = sizeof(input) - 1u;
    fixture.unexpected_grid = true;
    memset(&operations, 0, sizeof(operations));
    operations.context = &fixture;
    operations.write_terminal = write_terminal;
    operations.read_terminal = read_terminal;
    operations.set_title = set_title;
    operations.query_grid = query_grid;

    CHECK(!boxrig_test_run_frontend_sample_scenario(&operations));
    CHECK(fixture.grid_query_count == 1u);
    CHECK(fixture.title_count == 8u);
    CHECK(strcmp(
              fixture.titles[fixture.title_count - 1u],
              BOXRIG_TEST_FRONTEND_SAMPLE_UNEXPECTED_PREFIX "grid-23-80") == 0);
    return true;
}

static bool test_copy_policy_counts_button_transitions_with_drag_motion(void)
{
    static const uint8_t input[] =
        "\033[6;18;9tr\033[<2;11;5M\033[<34;12;5M\033[<2;12;5mzq";
    scenario_fixture fixture = {0};
    fixture.input = input;
    fixture.input_length = sizeof(input) - 1u;
    boxrig_test_frontend_sample_scenario_operations operations = {0};
    operations.context = &fixture;
    operations.write_terminal = write_terminal;
    operations.read_terminal = read_terminal;
    operations.set_title = set_title;
    operations.query_grid = query_grid;
    CHECK(setenv("BOXRIG_TEST_SAMPLE_COPY_POLICY", "1", 1) == 0);
    bool completed = boxrig_test_run_frontend_sample_scenario(&operations);
    CHECK(unsetenv("BOXRIG_TEST_SAMPLE_COPY_POLICY") == 0);
    CHECK(completed);
    CHECK(fixture.input_offset == fixture.input_length);
    CHECK(fixture.title_count == 6u);
    CHECK(strcmp(fixture.titles[5], "boxrig-copy-5-z-18-9-3-2") == 0);
    return true;
}

int main(void)
{
    if (!test_shared_sample_scenario() ||
        !test_sample_paste_pressure_preserves_input_order() ||
        !test_sample_overlays_require_native_checkpoints_and_ime_commit() ||
        !test_sample_overlays_reject_corrupted_ime_commit() ||
        !test_sample_geometry_requires_resized_and_restored_child_grids() ||
        !test_sample_geometry_rejects_an_unconverged_resize() ||
        !test_sample_geometry_reports_fixed_console_values_as_p2() ||
        !test_selection_override_rejects_leaked_mouse_input() ||
        !test_grid_mismatch_reports_observed_dimensions() ||
        !test_copy_policy_counts_button_transitions_with_drag_motion()) {
        return 1;
    }
    printf("PASS shared controlled PTY sample scenario\n");
    return 0;
}
