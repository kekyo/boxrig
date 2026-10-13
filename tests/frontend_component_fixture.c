/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "frontend_component_fixture.h"
#include "frontend_conformance_cases.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

#define BOXRIG_FIXTURE_ENUM_BUILD_ARTIFACT(...)
#define BOXRIG_FIXTURE_ENUM_CORE_UI_VALUE(...)
#define BOXRIG_FIXTURE_ENUM_RENDERER_MEANING(...)
#define BOXRIG_FIXTURE_ENUM_NATIVE_FRONTEND(...)
#define BOXRIG_FIXTURE_ENUM_SAMPLE_POLICY(...)
#define BOXRIG_FIXTURE_ENUM_EXTERNAL_P2(...)
#define BOXRIG_FIXTURE_ENUM_PORTABLE_COMPONENT(token, id, ...)                  \
    BOXRIG_FIXTURE_CASE_##token,
#define BOXRIG_FRONTEND_CONFORMANCE_CASE(token, id, scope, ...)                 \
    BOXRIG_FIXTURE_ENUM_##scope(token, id, __VA_ARGS__)

typedef enum frontend_fixture_case {
#include "frontend_conformance_matrix.def"
    BOXRIG_FIXTURE_CASE_COUNT
} frontend_fixture_case;

#undef BOXRIG_FRONTEND_CONFORMANCE_CASE
#undef BOXRIG_FIXTURE_ENUM_BUILD_ARTIFACT
#undef BOXRIG_FIXTURE_ENUM_CORE_UI_VALUE
#undef BOXRIG_FIXTURE_ENUM_PORTABLE_COMPONENT
#undef BOXRIG_FIXTURE_ENUM_RENDERER_MEANING
#undef BOXRIG_FIXTURE_ENUM_NATIVE_FRONTEND
#undef BOXRIG_FIXTURE_ENUM_SAMPLE_POLICY
#undef BOXRIG_FIXTURE_ENUM_EXTERNAL_P2

#define BOXRIG_FIXTURE_ID_BUILD_ARTIFACT(...)
#define BOXRIG_FIXTURE_ID_CORE_UI_VALUE(...)
#define BOXRIG_FIXTURE_ID_RENDERER_MEANING(...)
#define BOXRIG_FIXTURE_ID_NATIVE_FRONTEND(...)
#define BOXRIG_FIXTURE_ID_SAMPLE_POLICY(...)
#define BOXRIG_FIXTURE_ID_EXTERNAL_P2(...)
#define BOXRIG_FIXTURE_ID_PORTABLE_COMPONENT(token, id, ...) id,
#define BOXRIG_FRONTEND_CONFORMANCE_CASE(token, id, scope, ...)                 \
    BOXRIG_FIXTURE_ID_##scope(token, id, __VA_ARGS__)

static const char *const frontend_fixture_case_ids[] = {
#include "frontend_conformance_matrix.def"
};

#undef BOXRIG_FRONTEND_CONFORMANCE_CASE
#undef BOXRIG_FIXTURE_ID_BUILD_ARTIFACT
#undef BOXRIG_FIXTURE_ID_CORE_UI_VALUE
#undef BOXRIG_FIXTURE_ID_PORTABLE_COMPONENT
#undef BOXRIG_FIXTURE_ID_RENDERER_MEANING
#undef BOXRIG_FIXTURE_ID_NATIVE_FRONTEND
#undef BOXRIG_FIXTURE_ID_SAMPLE_POLICY
#undef BOXRIG_FIXTURE_ID_EXTERNAL_P2

#define BOXRIG_FIXTURE_REQUIRED_COMMON 1u
#define BOXRIG_FIXTURE_REQUIRED_NATIVE 0u
#define BOXRIG_FIXTURE_REQUIRED_UNSUPPORTED_BY_CONTRACT 0u
#define BOXRIG_FIXTURE_REQUIRED_P2 0u
#define BOXRIG_FIXTURE_REQUIRED_BUILD_ARTIFACT(...)
#define BOXRIG_FIXTURE_REQUIRED_CORE_UI_VALUE(...)
#define BOXRIG_FIXTURE_REQUIRED_RENDERER_MEANING(...)
#define BOXRIG_FIXTURE_REQUIRED_NATIVE_FRONTEND(...)
#define BOXRIG_FIXTURE_REQUIRED_SAMPLE_POLICY(...)
#define BOXRIG_FIXTURE_REQUIRED_EXTERNAL_P2(...)
#define BOXRIG_FIXTURE_REQUIRED_PORTABLE_COMPONENT(                            \
    token, id, description, x11_verdict, x11_target,                         \
    win32_verdict, win32_target, gtk3_verdict, gtk3_target,                   \
    gtk4_verdict, gtk4_target, wayland_verdict, wayland_target)               \
    (BOXRIG_FIXTURE_REQUIRED_##x11_verdict |                                 \
     (BOXRIG_FIXTURE_REQUIRED_##win32_verdict << 1) |                        \
     (BOXRIG_FIXTURE_REQUIRED_##gtk3_verdict << 2) |                         \
     (BOXRIG_FIXTURE_REQUIRED_##gtk4_verdict << 3) |                         \
     (BOXRIG_FIXTURE_REQUIRED_##wayland_verdict << 4)),
#define BOXRIG_FRONTEND_CONFORMANCE_CASE(token, id, scope, ...)                 \
    BOXRIG_FIXTURE_REQUIRED_##scope(token, id, __VA_ARGS__)

static const unsigned int frontend_fixture_case_required[] = {
#include "frontend_conformance_matrix.def"
};

#undef BOXRIG_FRONTEND_CONFORMANCE_CASE
#undef BOXRIG_FIXTURE_REQUIRED_COMMON
#undef BOXRIG_FIXTURE_REQUIRED_NATIVE
#undef BOXRIG_FIXTURE_REQUIRED_UNSUPPORTED_BY_CONTRACT
#undef BOXRIG_FIXTURE_REQUIRED_P2
#undef BOXRIG_FIXTURE_REQUIRED_BUILD_ARTIFACT
#undef BOXRIG_FIXTURE_REQUIRED_CORE_UI_VALUE
#undef BOXRIG_FIXTURE_REQUIRED_PORTABLE_COMPONENT
#undef BOXRIG_FIXTURE_REQUIRED_RENDERER_MEANING
#undef BOXRIG_FIXTURE_REQUIRED_NATIVE_FRONTEND
#undef BOXRIG_FIXTURE_REQUIRED_SAMPLE_POLICY
#undef BOXRIG_FIXTURE_REQUIRED_EXTERNAL_P2

typedef struct frontend_fixture_coverage {
    bool passed[BOXRIG_FIXTURE_CASE_COUNT];
} frontend_fixture_coverage;

static void pass_frontend_case(
    frontend_fixture_coverage *coverage,
    frontend_fixture_case test_case)
{
    if (coverage != NULL && test_case >= 0 &&
        test_case < BOXRIG_FIXTURE_CASE_COUNT) {
        coverage->passed[test_case] = true;
    }
}

static bool audit_frontend_cases(
    const frontend_fixture_coverage *coverage,
    const char *frontend_name)
{
    size_t index;

    CHECK(coverage != NULL && frontend_name != NULL);
    CHECK(sizeof(frontend_fixture_case_ids) /
              sizeof(frontend_fixture_case_ids[0]) ==
          BOXRIG_FIXTURE_CASE_COUNT);
    CHECK(sizeof(frontend_fixture_case_required) /
              sizeof(frontend_fixture_case_required[0]) ==
          BOXRIG_FIXTURE_CASE_COUNT);
    for (index = 0; index < BOXRIG_FIXTURE_CASE_COUNT; index++) {
        if (!boxrig_test_frontend_case_satisfied(frontend_name,
                frontend_fixture_case_required[index], coverage->passed[index])) {
            fprintf(
                stderr,
                "FAIL %s lacks shared case %s\n",
                frontend_name,
                frontend_fixture_case_ids[index]);
            return false;
        }
    }
    return true;
}

static bool feed_bytes(
    boxrig_terminal *terminal,
    const uint8_t *bytes,
    size_t length)
{
    size_t consumed = 0;

    CHECK(boxrig_terminal_feed(terminal, bytes, length, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == length);
    return true;
}

static void count_frame_request(void *context)
{
    size_t *count = context;

    (*count)++;
}

static bool pixel_rgb_is(
    uint32_t pixel,
    uint8_t red,
    uint8_t green,
    uint8_t blue)
{
    return (uint8_t)(pixel >> 16) == red &&
           (uint8_t)(pixel >> 8) == green && (uint8_t)pixel == blue;
}

boxrig_input_key_route boxrig_test_frontend_capture_key(
    const boxrig_key_event *event,
    void *context)
{
    boxrig_test_frontend_input_observation *observation = context;

    if (event == NULL || observation == NULL ||
        observation->key_count >= BOXRIG_TEST_FRONTEND_KEY_TRANSITIONS) {
        return BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    observation->key_events[observation->key_count++] = *event;
    return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
}

boxrig_input_mouse_route boxrig_test_frontend_capture_mouse(
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    void *context)
{
    boxrig_test_frontend_input_observation *observation = context;

    if (event == NULL || hit == NULL || observation == NULL ||
        observation->mouse_count >= BOXRIG_TEST_FRONTEND_MOUSE_TRANSITIONS) {
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    observation->mouse_events[observation->mouse_count] = *event;
    observation->mouse_hits[observation->mouse_count++] = *hit;
    return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
}

bool boxrig_test_frontend_input_matches(
    const boxrig_test_frontend_input_script *script,
    const boxrig_test_frontend_input_observation *observation)
{
    size_t index;

    if (script == NULL || observation == NULL ||
        !observation->focused_after_enter ||
        observation->focused_after_leave ||
        observation->key_count != BOXRIG_TEST_FRONTEND_KEY_TRANSITIONS ||
        observation->mouse_count != BOXRIG_TEST_FRONTEND_MOUSE_TRANSITIONS ||
        observation->key_events[0].action != BOXRIG_KEY_PRESS ||
        observation->key_events[1].action != BOXRIG_KEY_REPEAT ||
        observation->key_events[2].action != BOXRIG_KEY_RELEASE) {
        if (observation != NULL) {
            fprintf(
                stderr,
                "frontend input focus/key shape: focus=%d/%d keys=%zu "
                "actions=%d/%d/%d\n",
                observation->focused_after_enter,
                observation->focused_after_leave,
                observation->key_count,
                observation->key_events[0].action,
                observation->key_events[1].action,
                observation->key_events[2].action);
        }
        return false;
    }
    for (index = 0; index < BOXRIG_TEST_FRONTEND_KEY_TRANSITIONS; index++) {
        if (observation->key_events[index].key != BOXRIG_KEY_F1 ||
            observation->key_events[index].codepoint != 0 ||
            observation->key_events[index].shifted_codepoint != 0 ||
            observation->key_events[index].base_layout_codepoint != 0 ||
            observation->key_events[index].modifiers != 0) {
            fprintf(
                stderr,
                "frontend input key[%zu]: key=%d code=%u shifted=%u "
                "base=%u modifiers=%u\n",
                index,
                observation->key_events[index].key,
                observation->key_events[index].codepoint,
                observation->key_events[index].shifted_codepoint,
                observation->key_events[index].base_layout_codepoint,
                observation->key_events[index].modifiers);
            return false;
        }
    }
    if (observation->mouse_events[0].action != BOXRIG_MOUSE_PRESS ||
        observation->mouse_events[1].action != BOXRIG_MOUSE_MOTION ||
        observation->mouse_events[2].action != BOXRIG_MOUSE_RELEASE ||
        observation->mouse_events[0].button != BOXRIG_MOUSE_BUTTON_LEFT ||
        observation->mouse_events[0].click_count != 1 ||
        observation->mouse_events[0].modifiers != BOXRIG_MODIFIER_CONTROL ||
        observation->mouse_events[1].modifiers != BOXRIG_MODIFIER_SHIFT ||
        observation->mouse_events[2].modifiers != 0) {
        fprintf(
            stderr,
            "frontend input mouse shape: count=%zu actions=%d/%d/%d "
            "button=%d click=%u modifiers=%u/%u/%u\n",
            observation->mouse_count,
            observation->mouse_events[0].action,
            observation->mouse_events[1].action,
            observation->mouse_events[2].action,
            observation->mouse_events[0].button,
            observation->mouse_events[0].click_count,
            observation->mouse_events[0].modifiers,
            observation->mouse_events[1].modifiers,
            observation->mouse_events[2].modifiers);
        return false;
    }
    for (index = 0; index < BOXRIG_TEST_FRONTEND_MOUSE_TRANSITIONS; index++) {
        const boxrig_mouse_event *event = &observation->mouse_events[index];
        const boxrig_ui_hit_test_result *hit = &observation->mouse_hits[index];

        if (!hit->inside_grid || hit->x != script->x ||
            hit->y != script->y || hit->layout_generation == 0 ||
            hit->annotation_generation == 0 || event->row != hit->row ||
            event->column != hit->visual_column ||
            hit->layout_generation !=
                observation->mouse_hits[0].layout_generation ||
            hit->annotation_generation !=
                observation->mouse_hits[0].annotation_generation) {
            fprintf(
                stderr,
                "frontend input hit[%zu]: inside=%d point=%lld,%lld "
                "expected=%lld,%lld cell=%zu,%zu event=%zu,%zu "
                "generation=%llu/%llu\n",
                index,
                hit->inside_grid,
                (long long)hit->x,
                (long long)hit->y,
                (long long)script->x,
                (long long)script->y,
                hit->row,
                hit->visual_column,
                event->row,
                event->column,
                (unsigned long long)hit->layout_generation,
                (unsigned long long)hit->annotation_generation);
            return false;
        }
    }
    return true;
}

static bool select_rectangle(
    boxrig_ui_view *view,
    const boxrig_font_effective_metrics *metrics,
    const uint8_t *expected,
    size_t expected_length)
{
    boxrig_ui_hit_test_result anchor_hit;
    boxrig_ui_hit_test_result focus_hit;
    boxrig_ui_hit_test_result selected_hit;
    boxrig_ui_selection_options options;
    boxrig_ui_selection selection;
    boxrig_text_layout_range ranges[2];
    boxrig_text_snapshot *snapshot = NULL;
    const uint8_t *data;
    size_t length;

    CHECK(view != NULL && metrics != NULL);
    CHECK(boxrig_ui_view_hit_test(
              view,
              (boxrig_ui_fixed)metrics->cell_width *
                      BOXRIG_UI_FIXED_ONE +
                  (boxrig_ui_fixed)metrics->cell_width *
                      BOXRIG_UI_FIXED_ONE / 4,
              (boxrig_ui_fixed)metrics->cell_height *
                  BOXRIG_UI_FIXED_ONE / 2,
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              view,
              (boxrig_ui_fixed)metrics->cell_width * 3 *
                      BOXRIG_UI_FIXED_ONE +
                  (boxrig_ui_fixed)metrics->cell_width * 3 *
                      BOXRIG_UI_FIXED_ONE / 4,
              (boxrig_ui_fixed)metrics->cell_height *
                      BOXRIG_UI_FIXED_ONE +
                  (boxrig_ui_fixed)metrics->cell_height *
                      BOXRIG_UI_FIXED_ONE / 2,
              &focus_hit) == BOXRIG_UI_OK);
    CHECK(anchor_hit.inside_grid && anchor_hit.text_position_valid);
    CHECK(focus_hit.inside_grid && focus_hit.text_position_valid);
    options.shape = BOXRIG_UI_SELECTION_RECTANGULAR;
    options.kind = BOXRIG_UI_SELECTION_CHARACTER;
    CHECK(boxrig_ui_view_begin_selection_with_options(
              view,
              &options,
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &focus_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR &&
          selection.visual_start_row == 0 && selection.row_count == 2 &&
          selection.visual_start_column == 1 &&
          selection.visual_column_count == 3);
    CHECK(boxrig_ui_view_selection_visual_ranges(view, 0, ranges, 2) == 1);
    CHECK(ranges[0].column == 1 && ranges[0].column_count == 3);
    CHECK(boxrig_ui_view_selection_visual_ranges(view, 1, ranges, 2) == 1);
    CHECK(ranges[0].column == 1 && ranges[0].column_count == 3);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &snapshot) ==
          BOXRIG_UI_OK);
    data = boxrig_text_snapshot_data(snapshot, &length);
    CHECK(length == expected_length);
    CHECK(data != NULL && memcmp(data, expected, length) == 0);
    boxrig_text_snapshot_destroy(snapshot);
    CHECK(boxrig_ui_view_hit_test(
              view,
              (boxrig_ui_fixed)metrics->cell_width * 2 *
                      BOXRIG_UI_FIXED_ONE +
                  (boxrig_ui_fixed)metrics->cell_width *
                      BOXRIG_UI_FIXED_ONE / 2,
              (boxrig_ui_fixed)metrics->cell_height *
                      BOXRIG_UI_FIXED_ONE +
                  (boxrig_ui_fixed)metrics->cell_height *
                      BOXRIG_UI_FIXED_ONE / 2,
              &selected_hit) == BOXRIG_UI_OK);
    CHECK((selected_hit.overlay_roles & BOXRIG_UI_OVERLAY_SELECTION) != 0);
    return true;
}

bool boxrig_test_run_frontend_component_fixture(
    const boxrig_test_frontend_component_operations *operations)
{
    static const uint8_t screen_text[] = "fixture\r\npicture";
    static const uint8_t selected_text[] = "ixt\nict";
    static const uint8_t paste_text[] = "abcdef";
    static const uint8_t history_text[] =
        "\r\nthird\r\nfourth\r\nfifth";
    static const uint8_t presentation_text[] =
        "\033[2J\033[H\033[?12l"
        "\033]8;id=fixture;https://example.test/\033\\"
        "link"
        "\033]8;;\033\\ g"
        "\033[2;1Hm  m"
        "\033[1;1H";
    static const uint8_t presentation_pattern[] = "link";
    static const uint8_t presentation_image[] =
        "\033[2J\033[H\033[?25l"
        "\033]11;#204060\007"
        "\033_Ga=T,f=32,s=1,v=1,c=1,r=1,z=0,C=1;/wAA/w==\033\\";
    static const char presentation_preedit[] = "x";
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_component_allocation allocation;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame_result;
    boxrig_ui_component_diagnostic diagnostic;
    boxrig_test_frontend_input_script input_script;
    boxrig_test_frontend_input_observation input_observation;
    boxrig_font_specification font_specification;
    boxrig_font_specification reported_font_specification;
    boxrig_font_specification invalid_font_specification;
    boxrig_font_family resource_family;
    boxrig_font_effective_metrics resource_metrics;
    boxrig_font_effective_metrics line_height_metrics;
    boxrig_font_effective_metrics zoomed_metrics;
    boxrig_font_effective_metrics reset_metrics;
    boxrig_font_visual_step_policy zoom_policy;
    boxrig_font_resource_id font_resource;
    boxrig_ui_view_action_capabilities action_capabilities;
    boxrig_ui_view_action reconvert_action;
    boxrig_ui_dirty dirty;
    boxrig_ui_hit_test_result annotation_hit;
    boxrig_ui_hit_test_result presentation_hit;
    boxrig_ui_hit_test_result composed_hit;
    boxrig_ui_text_annotation annotation;
    boxrig_text_search_options search_options;
    boxrig_text_search_progress search_progress;
    boxrig_composition_event composition_event;
    boxrig_composition *composition;
    boxrig_test_frontend_presentation_probe presentation_probe;
    boxrig_test_frontend_presentation_observation cursor_presentation;
    boxrig_test_frontend_presentation_observation composition_presentation;
    boxrig_test_frontend_presentation_observation dirty_presentation;
    boxrig_test_frontend_presentation_observation image_presentation;
    boxrig_ui_transparency_config transparency;
    boxrig_ui_transparency_state transparency_before;
    boxrig_ui_transparency_state transparency_after;
    boxrig_ui_size_command_state command_state;
    boxrig_ui_layout_action layout_action;
    boxrig_scrollbar_model scrollbar;
    const uint8_t *output;
    size_t output_length;
    size_t search_work;
    size_t preferred_rows;
    size_t preferred_columns;
    size_t frame_request_count = 0;
    size_t frame_request_count_at_destroy;
    uint64_t catalog_revision;
    float line_height;
    bool applied;
    bool changed;
    void *host;
    void *component = NULL;
    void *conflict = NULL;
    void *presentation = NULL;
    void *replacement = NULL;
    uint32_t capabilities;
    frontend_fixture_coverage coverage = {{false}};

    CHECK(operations != NULL);
    CHECK(operations->frontend_name != NULL);
    CHECK(operations->lifecycle.create_host != NULL);
    CHECK(operations->lifecycle.destroy_host != NULL);
    CHECK(operations->lifecycle.create_component != NULL);
    CHECK(operations->lifecycle.quiesce_component != NULL);
    CHECK(operations->lifecycle.destroy_component != NULL);
    CHECK(operations->layout.component_view != NULL);
    CHECK(operations->layout.get_effective_metrics != NULL);
    CHECK(operations->layout.allocate != NULL);
    CHECK(operations->layout.set_preferred_grid != NULL);
    CHECK(operations->layout.get_preferred_grid != NULL);
    CHECK(operations->layout.get_size_command_state != NULL);
    CHECK(operations->layout.take_layout_action != NULL);
    CHECK(operations->layout.complete_size_command != NULL);
    CHECK(operations->layout.complete_grid_resize != NULL);
    CHECK(operations->layout.set_visible != NULL);
    CHECK(operations->frame.set_frame_request_callback != NULL);
    CHECK(operations->frame.frame != NULL);
    CHECK(operations->input.queue_paste != NULL);
    CHECK(operations->input.run_native_transcript != NULL);
    CHECK(operations->font.fixture_font_path != NULL);
    CHECK(operations->font.fixture_font_path[0] != '\0');
    CHECK(operations->font.register_font_file != NULL);
    CHECK(operations->font.set_specification != NULL);
    CHECK(operations->font.get_specification != NULL);
    CHECK(operations->font.primary_resource != NULL);
    CHECK(operations->font.catalog_revision != NULL);
    CHECK(operations->font.set_line_height != NULL);
    CHECK(operations->font.get_line_height != NULL);
    CHECK(operations->font.visual_zoom != NULL);
    CHECK(operations->font.visual_zoom_reset != NULL);
    CHECK(operations->font.set_text_direction != NULL);
    CHECK(operations->font.text_direction != NULL);
    CHECK(operations->view_action.copy_selection != NULL);
    CHECK(operations->view_action.capabilities != NULL);
    CHECK(operations->view_action.apply != NULL);
    CHECK(operations->view_action.get_scrollbar_model != NULL);
    CHECK(operations->view_action.get_scrollbar_state != NULL);
    CHECK(operations->view_action.apply_scrollbar_action != NULL);
    CHECK(operations->presentation.set_transparency != NULL);
    CHECK(operations->presentation.get_transparency != NULL);
    CHECK(operations->presentation.capabilities != NULL);
    CHECK(operations->presentation.get_diagnostic != NULL);
    CHECK(operations->presentation.composition != NULL);
    CHECK(operations->presentation.observe != NULL);

    host = operations->lifecycle.create_host();
    CHECK(host != NULL);
    terminal_config.rows = 2;
    terminal_config.columns = 16;
    terminal_config.output_capacity = 4;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);

    CHECK(operations->lifecycle.create_component(
              host,
              model,
              BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY,
              &component) == BOXRIG_UI_OK);
    CHECK(component != NULL);
    CHECK(operations->lifecycle.create_component(
              host,
              model,
              BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY,
              &conflict) == BOXRIG_UI_UNAVAILABLE);
    CHECK(conflict == NULL);
    CHECK(operations->lifecycle.create_component(
              host,
              model,
              BOXRIG_UI_COMPONENT_PRESENTATION_ONLY,
              &presentation) == BOXRIG_UI_OK);
    CHECK(presentation != NULL);
    operations->lifecycle.destroy_component(presentation);
    presentation = NULL;
    pass_frontend_case(&coverage, BOXRIG_FIXTURE_CASE_COMPONENT_AUTHORITY);

    view = operations->layout.component_view(component);
    CHECK(view != NULL && boxrig_ui_view_model(view) == model);
    action_capabilities = operations->view_action.capabilities(component);
    CHECK(!action_capabilities.reconversion);
    memset(&reconvert_action, 0, sizeof(reconvert_action));
    reconvert_action.kind = BOXRIG_UI_VIEW_ACTION_RECONVERT;
    CHECK(!boxrig_ui_view_action_supported(
        &action_capabilities,
        &reconvert_action));
    CHECK(operations->view_action.apply(component, &reconvert_action) ==
          BOXRIG_UI_UNAVAILABLE);
    printf(
        "PASS %s through %s public API\n",
        BOXRIG_TEST_CASE_EDITABLE_PROVIDER_ABSENT,
        operations->frontend_name);
    pass_frontend_case(
        &coverage,
        BOXRIG_FIXTURE_CASE_COMPONENT_EDITABLE_PROVIDER);

    CHECK(operations->font.get_specification(
              component,
              &font_specification) == BOXRIG_UI_OK);
    catalog_revision = operations->font.catalog_revision(component);
    CHECK(catalog_revision != 0);
    CHECK(operations->font.register_font_file(
              component,
              operations->font.fixture_font_path,
              strlen(operations->font.fixture_font_path),
              &font_resource) == BOXRIG_FONT_CATALOG_OK);
    CHECK(font_resource != BOXRIG_FONT_RESOURCE_NONE);
    CHECK(operations->font.catalog_revision(component) ==
          catalog_revision + 1);
    CHECK(operations->font.primary_resource(component) ==
          BOXRIG_FONT_RESOURCE_NONE);
    memset(&resource_family, 0, sizeof(resource_family));
    resource_family.resource_id = font_resource;
    font_specification.families = &resource_family;
    font_specification.family_count = 1;
    CHECK(operations->font.set_specification(
              component,
              &font_specification) == BOXRIG_UI_OK);
    CHECK(operations->font.primary_resource(component) == font_resource);
    CHECK(operations->font.get_specification(
              component,
              &reported_font_specification) == BOXRIG_UI_OK);
    CHECK(reported_font_specification.family_count == 1);
    CHECK(reported_font_specification.families[0].resource_id ==
          font_resource);
    CHECK(operations->layout.get_effective_metrics(
        component,
        &resource_metrics));
    CHECK(resource_metrics.primary_resource_id == font_resource);

    invalid_font_specification = reported_font_specification;
    invalid_font_specification.size_dip = 0.0f;
    CHECK(operations->font.set_specification(
              component,
              &invalid_font_specification) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(operations->layout.get_effective_metrics(component, &metrics));
    CHECK(metrics.layout_revision == resource_metrics.layout_revision);

    CHECK(operations->font.set_line_height(component, 2.0f) == BOXRIG_UI_OK);
    CHECK(operations->font.get_line_height(component, &line_height) ==
          BOXRIG_UI_OK);
    CHECK(line_height == 2.0f);
    CHECK(operations->layout.get_effective_metrics(
        component,
        &line_height_metrics));
    CHECK(line_height_metrics.glyph_height == resource_metrics.glyph_height);
    CHECK(line_height_metrics.cell_height > resource_metrics.cell_height);
    CHECK(operations->font.set_line_height(component, 0.0f) == BOXRIG_UI_OK);
    CHECK(operations->font.set_text_direction(
              component,
              BOXRIG_TEXT_DIRECTION_FORCE_RTL) == BOXRIG_UI_OK);
    CHECK(operations->font.text_direction(component) ==
          BOXRIG_TEXT_DIRECTION_FORCE_RTL);
    CHECK(operations->font.set_text_direction(
              component,
              BOXRIG_TEXT_DIRECTION_AUTO) == BOXRIG_UI_OK);

    CHECK(operations->layout.get_effective_metrics(component, &reset_metrics));
    zoom_policy = boxrig_font_visual_step_policy_default();
    zoom_policy.minimum_size_dip = reset_metrics.requested_size_dip - 2.0f;
    zoom_policy.maximum_size_dip = reset_metrics.requested_size_dip + 2.0f;
    CHECK(operations->font.visual_zoom(
              component,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(operations->layout.get_effective_metrics(
        component,
        &zoomed_metrics));
    CHECK(zoomed_metrics.requested_size_dip >
          reset_metrics.requested_size_dip);
    CHECK(operations->font.get_specification(
              component,
              &reported_font_specification) == BOXRIG_UI_OK);
    CHECK(reported_font_specification.size_dip ==
          reset_metrics.requested_size_dip);
    CHECK(operations->font.visual_zoom_reset(component, &changed) ==
          BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(operations->font.get_specification(
              component,
              &reported_font_specification) == BOXRIG_UI_OK);
    CHECK(reported_font_specification.size_dip ==
          reset_metrics.requested_size_dip);
    CHECK(operations->font.visual_zoom_reset(component, &changed) ==
          BOXRIG_UI_OK);
    CHECK(!changed);
    CHECK(operations->font.set_specification(
              component,
              &font_specification) == BOXRIG_UI_OK);
    CHECK(operations->layout.get_effective_metrics(component, &metrics));
    CHECK(metrics.requested_size_dip == reset_metrics.requested_size_dip);
    pass_frontend_case(&coverage, BOXRIG_FIXTURE_CASE_COMPONENT_FONT);

    CHECK(operations->layout.get_effective_metrics(component, &metrics));
    CHECK(metrics.cell_width != 0 && metrics.cell_height != 0);
    memset(&allocation, 0, sizeof(allocation));
    allocation.width = metrics.cell_width * (unsigned int)terminal_config.columns;
    allocation.height = metrics.cell_height * (unsigned int)terminal_config.rows;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(operations->layout.allocate(component, &allocation, &layout) ==
          BOXRIG_UI_OK);
    CHECK(layout.rows == terminal_config.rows);
    CHECK(layout.columns == terminal_config.columns);
    pass_frontend_case(&coverage, BOXRIG_FIXTURE_CASE_COMPONENT_ALLOCATION);
    CHECK(operations->layout.set_visible(component, true) == BOXRIG_UI_OK);
    CHECK(operations->frame.frame(component, &frame_result) == BOXRIG_UI_OK);
    CHECK(!frame_result.frame_pending);

    CHECK(operations->layout.set_visible(component, false) == BOXRIG_UI_OK);
    CHECK(operations->presentation.get_diagnostic(component, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(!diagnostic.visible);
    CHECK(operations->layout.set_visible(component, true) == BOXRIG_UI_OK);
    CHECK(operations->presentation.get_diagnostic(component, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.visible);
    memset(&input_observation, 0, sizeof(input_observation));
    input_script.x =
        ((boxrig_ui_fixed)metrics.cell_width + 1) * BOXRIG_UI_FIXED_ONE;
    input_script.y = BOXRIG_UI_FIXED_ONE;
    CHECK(operations->input.run_native_transcript(
        component,
        &input_script,
        &input_observation));
    CHECK(boxrig_test_frontend_input_matches(
        &input_script,
        &input_observation));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    pass_frontend_case(
        &coverage,
        BOXRIG_FIXTURE_CASE_COMPONENT_VISIBILITY_FOCUS);
    pass_frontend_case(&coverage, BOXRIG_FIXTURE_CASE_COMPONENT_KEY_INPUT);
    pass_frontend_case(&coverage, BOXRIG_FIXTURE_CASE_COMPONENT_MOUSE_INPUT);

    /* Visibility and input can leave presentation dirty on an asynchronous
     * backend. Start the new-dirty notification check from a completed frame. */
    CHECK(operations->frame.frame(component, &frame_result) == BOXRIG_UI_OK);
    CHECK(!frame_result.frame_pending);
    CHECK(operations->frame.set_frame_request_callback(
              component,
              count_frame_request,
              &frame_request_count) == BOXRIG_UI_OK);
    CHECK(feed_bytes(terminal, screen_text, sizeof(screen_text) - 1));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(frame_request_count == 1);
    CHECK(boxrig_ui_view_peek_dirty(
        view,
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));
    CHECK(operations->frame.frame(component, &frame_result) == BOXRIG_UI_OK);
    CHECK(!frame_result.frame_pending);
    CHECK(!boxrig_ui_view_peek_dirty(
        view,
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));
    pass_frontend_case(&coverage, BOXRIG_FIXTURE_CASE_COMPONENT_FRAME_DIRTY);

    CHECK(boxrig_ui_view_hit_test(
              view,
              (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE / 2,
              (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE / 2,
              &annotation_hit) == BOXRIG_UI_OK);
    CHECK(annotation_hit.inside_grid && annotation_hit.text_position_valid);
    memset(&annotation, 0, sizeof(annotation));
    annotation.source_id = 73;
    annotation.item_id = 1;
    annotation.range.start = annotation_hit.leading_position;
    annotation.range.end = annotation_hit.trailing_position;
    annotation.style.field_mask = BOXRIG_UI_ANNOTATION_BACKGROUND;
    annotation.style.background.kind = BOXRIG_COLOR_RGB;
    annotation.style.background.red = 0x12;
    annotation.style.background.green = 0x34;
    annotation.style.background.blue = 0x56;
    CHECK(boxrig_ui_view_replace_text_annotations(
              view,
              73,
              annotation_hit.leading_position.screen,
              annotation_hit.leading_position.line_id,
              annotation_hit.leading_position.line_revision,
              &annotation,
              1) == BOXRIG_UI_OK);
    CHECK(frame_request_count == 2);
    CHECK(boxrig_ui_view_peek_dirty(
        view,
        BOXRIG_UI_DIRTY_OVERLAY,
        &dirty));
    CHECK(operations->frame.frame(component, &frame_result) == BOXRIG_UI_OK);
    CHECK(!frame_result.frame_pending);
    CHECK(!boxrig_ui_view_peek_dirty(
        view,
        BOXRIG_UI_DIRTY_OVERLAY,
        &dirty));
    pass_frontend_case(&coverage, BOXRIG_FIXTURE_CASE_COMPONENT_ANNOTATION);

    dirty.row = 0;
    dirty.row_count = terminal_config.rows;
    dirty.column = 0;
    dirty.column_count = terminal_config.columns;
    CHECK(boxrig_ui_view_invalidate(
              view,
              BOXRIG_UI_DIRTY_PRESENT,
              dirty) == BOXRIG_UI_OK);
    CHECK(frame_request_count == 3);
    CHECK(operations->frame.frame(component, &frame_result) == BOXRIG_UI_OK);
    CHECK(!frame_result.frame_pending);
    CHECK(!boxrig_ui_view_peek_dirty(
        view,
        BOXRIG_UI_DIRTY_PRESENT,
        &dirty));

    CHECK(operations->input.queue_paste(
              component,
              paste_text,
              sizeof(paste_text) - 1) == BOXRIG_UI_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == terminal_config.output_capacity);
    CHECK(memcmp(output, paste_text, output_length) == 0);
    CHECK(operations->frame.frame(component, &frame_result) == BOXRIG_UI_OK);
    CHECK(frame_result.input_blocked);
    CHECK(frame_result.additional_dispatch);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(operations->frame.frame(component, &frame_result) == BOXRIG_UI_OK);
    CHECK(!frame_result.input_blocked);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(paste_text) - 1 -
                               terminal_config.output_capacity);
    CHECK(memcmp(
              output,
              paste_text + terminal_config.output_capacity,
              output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    pass_frontend_case(&coverage, BOXRIG_FIXTURE_CASE_COMPONENT_PASTE);

    CHECK(select_rectangle(
        view,
        &metrics,
        selected_text,
        sizeof(selected_text) - 1));
    CHECK(operations->frame.frame(component, &frame_result) == BOXRIG_UI_OK);
    CHECK(!frame_result.frame_pending);
    CHECK(operations->view_action.copy_selection(
        host,
        component,
        selected_text,
        sizeof(selected_text) - 1));
    pass_frontend_case(&coverage, BOXRIG_FIXTURE_CASE_COMPONENT_SELECTION_COPY);

    CHECK(feed_bytes(terminal, history_text, sizeof(history_text) - 1));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(operations->frame.frame(component, &frame_result) == BOXRIG_UI_OK);
    CHECK(operations->view_action.get_scrollbar_model(
              component,
              &scrollbar));
    CHECK(scrollbar.enabled);
    CHECK(scrollbar.history_rows > 0);
    CHECK(scrollbar.position == scrollbar.history_rows);
    boxrig_scrollbar_state initial_scrollbar, moved_scrollbar, repeated_scrollbar;
    CHECK(operations->view_action.get_scrollbar_state(component, &initial_scrollbar));
    CHECK(initial_scrollbar.revision != 0 && initial_scrollbar.model.position == scrollbar.position);
    CHECK(operations->view_action.apply_scrollbar_action(
              component,
              BOXRIG_SCROLLBAR_TOP,
              0,
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(operations->view_action.get_scrollbar_model(
              component,
              &scrollbar));
    CHECK(scrollbar.position == 0);
    CHECK(scrollbar.viewport_offset == scrollbar.history_rows);
    CHECK(operations->view_action.get_scrollbar_state(component, &moved_scrollbar));
    CHECK(moved_scrollbar.revision != initial_scrollbar.revision && moved_scrollbar.model.position == 0);
    CHECK(operations->view_action.apply_scrollbar_action(component,
        BOXRIG_SCROLLBAR_POSITION, moved_scrollbar.model.position, &changed) == BOXRIG_UI_OK);
    CHECK(!changed);
    CHECK(operations->view_action.get_scrollbar_state(component, &repeated_scrollbar));
    CHECK(repeated_scrollbar.revision == moved_scrollbar.revision);
    CHECK(operations->view_action.apply_scrollbar_action(
              component,
              BOXRIG_SCROLLBAR_BOTTOM,
              0,
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(operations->view_action.get_scrollbar_model(
              component,
              &scrollbar));
    CHECK(scrollbar.position == scrollbar.history_rows);
    CHECK(scrollbar.viewport_offset == 0);
    pass_frontend_case(&coverage, BOXRIG_FIXTURE_CASE_COMPONENT_SCROLLBAR);

    CHECK(boxrig_ui_view_clear_selection(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_clear_search(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_clear_hyperlink_hover(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_clear_text_annotations(view, 73) == BOXRIG_UI_OK);
    CHECK(feed_bytes(
        terminal,
        presentation_text,
        sizeof(presentation_text) - 1));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              view,
              (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE / 4,
              (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE / 2,
              &presentation_hit) == BOXRIG_UI_OK);
    CHECK(presentation_hit.inside_grid &&
          presentation_hit.text_position_valid &&
          presentation_hit.hyperlink_handle != 0);
    CHECK(boxrig_ui_view_begin_selection(
              view,
              BOXRIG_UI_SELECTION_WORD,
              &presentation_hit) == BOXRIG_UI_OK);
    memset(&search_options, 0, sizeof(search_options));
    search_options.screen = BOXRIG_TEXT_SCREEN_PRIMARY;
    search_options.pattern = presentation_pattern;
    search_options.pattern_length = sizeof(presentation_pattern) - 1;
    search_options.direction = BOXRIG_TEXT_SEARCH_FORWARD;
    CHECK(boxrig_ui_view_begin_search(view, &search_options) == BOXRIG_UI_OK);
    search_progress = BOXRIG_TEXT_SEARCH_IN_PROGRESS;
    while (search_progress == BOXRIG_TEXT_SEARCH_IN_PROGRESS) {
        CHECK(boxrig_ui_view_resume_search(
                  view,
                  4,
                  &search_work,
                  &search_progress) == BOXRIG_UI_OK);
        CHECK(search_work <= 4);
    }
    CHECK(search_progress == BOXRIG_TEXT_SEARCH_MATCH);
    CHECK(boxrig_ui_view_update_hyperlink_hover(
              view,
              &presentation_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              view,
              ((boxrig_ui_fixed)metrics.cell_width +
               (boxrig_ui_fixed)metrics.cell_width / 2) *
                  BOXRIG_UI_FIXED_ONE,
              ((boxrig_ui_fixed)metrics.cell_height +
               (boxrig_ui_fixed)metrics.cell_height / 2) *
                  BOXRIG_UI_FIXED_ONE,
              &annotation_hit) == BOXRIG_UI_OK);
    CHECK(annotation_hit.inside_grid && annotation_hit.text_position_valid);
    memset(&annotation, 0, sizeof(annotation));
    annotation.source_id = 73;
    annotation.item_id = 2;
    annotation.range.start = annotation_hit.leading_position;
    annotation.range.end = annotation_hit.trailing_position;
    annotation.style.field_mask = BOXRIG_UI_ANNOTATION_BACKGROUND;
    annotation.style.background.kind = BOXRIG_COLOR_RGB;
    annotation.style.background.red = 0x12;
    annotation.style.background.green = 0x34;
    annotation.style.background.blue = 0x56;
    CHECK(boxrig_ui_view_replace_text_annotations(
              view,
              73,
              annotation_hit.leading_position.screen,
              annotation_hit.leading_position.line_id,
              annotation_hit.leading_position.line_revision,
              &annotation,
              1) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              view,
              (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE / 4,
              (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE / 2,
              &presentation_hit) == BOXRIG_UI_OK);
    CHECK((presentation_hit.overlay_roles &
           BOXRIG_UI_OVERLAY_SEARCH_MATCH) != 0);
    CHECK((presentation_hit.overlay_roles & BOXRIG_UI_OVERLAY_SELECTION) != 0);
    CHECK((presentation_hit.overlay_roles &
           BOXRIG_UI_OVERLAY_HYPERLINK_HOVER) != 0);
    CHECK((presentation_hit.overlay_roles & BOXRIG_UI_OVERLAY_CURSOR) != 0);
    CHECK(operations->frame.frame(component, &frame_result) == BOXRIG_UI_OK);
    CHECK(!frame_result.frame_pending);
    memset(&presentation_probe, 0, sizeof(presentation_probe));
    presentation_probe.background_x = (int)metrics.cell_width * 3 / 2;
    presentation_probe.background_y = 0;
    presentation_probe.meaning_x = (int)metrics.cell_width * 3 / 2;
    presentation_probe.meaning_y = (int)metrics.cell_height * 3 / 2;
    presentation_probe.coverage_x = (int)metrics.cell_width;
    presentation_probe.coverage_y = 0;
    presentation_probe.coverage_width = metrics.cell_width;
    presentation_probe.coverage_height = metrics.cell_height;
    presentation_probe.stable_x = (int)metrics.cell_width * 12;
    presentation_probe.stable_y = (int)metrics.cell_height;
    presentation_probe.stable_width = metrics.cell_width * 4;
    presentation_probe.stable_height = metrics.cell_height;
    CHECK(operations->presentation.observe(
        component,
        &presentation_probe,
        &cursor_presentation));
    CHECK(cursor_presentation.generation != 0);
    CHECK(cursor_presentation.width == allocation.width);
    CHECK(cursor_presentation.height == allocation.height);
    CHECK(pixel_rgb_is(cursor_presentation.meaning_argb, 0x12, 0x34, 0x56));
    CHECK(cursor_presentation.coverage_pixels != 0);

    composition = operations->presentation.composition(component);
    CHECK(composition != NULL);
    CHECK(boxrig_composition_focus_changed(composition, true) ==
          BOXRIG_COMPOSITION_OK);
    memset(&composition_event, 0, sizeof(composition_event));
    composition_event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    composition_event.text = presentation_preedit;
    composition_event.text_length = sizeof(presentation_preedit) - 1;
    composition_event.caret_offset = sizeof(presentation_preedit) - 1;
    composition_event.caret_visible = true;
    CHECK(operations->frame.frame(component, &frame_result) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_frame_requested(view));
    CHECK(boxrig_composition_apply_event(composition, &composition_event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(operations->frame.frame(component, &frame_result) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test_with_composition(
              view,
              composition,
              (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE / 4,
              (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE / 2,
              &composed_hit) == BOXRIG_UI_OK);
    CHECK((composed_hit.overlay_roles & BOXRIG_UI_OVERLAY_SEARCH_MATCH) != 0);
    CHECK((composed_hit.overlay_roles & BOXRIG_UI_OVERLAY_SELECTION) != 0);
    CHECK((composed_hit.overlay_roles &
           BOXRIG_UI_OVERLAY_HYPERLINK_HOVER) != 0);
    CHECK((composed_hit.overlay_roles & BOXRIG_UI_OVERLAY_IME) != 0);
    CHECK((composed_hit.overlay_roles & BOXRIG_UI_OVERLAY_CURSOR) == 0);
    CHECK(operations->presentation.observe(
        component,
        &presentation_probe,
        &composition_presentation));
    CHECK(composition_presentation.generation >
          cursor_presentation.generation);
    CHECK(composition_presentation.stable_hash ==
          cursor_presentation.stable_hash);

    /* Preedit-only edits must update both presentation and candidate placement. */
    for (size_t caret = 4; caret >= 2; caret--) {
        boxrig_composition_view composition_view;
        uint64_t previous_generation = composition_presentation.generation;
        CHECK(operations->frame.frame(component, &frame_result) == BOXRIG_UI_OK);
        CHECK(!boxrig_ui_view_frame_requested(view));
        composition_event.text = "abcd";
        composition_event.text_length = 4;
        composition_event.caret_offset = caret;
        CHECK(boxrig_composition_apply_event(composition, &composition_event) ==
              BOXRIG_COMPOSITION_OK);
        CHECK(operations->frame.frame(component, &frame_result) == BOXRIG_UI_OK);
        CHECK(boxrig_composition_get_view(composition, &composition_view) ==
              BOXRIG_COMPOSITION_OK);
        CHECK(composition_view.candidate.visible &&
              composition_view.candidate.spot_x == metrics.cell_width * caret);
        CHECK(operations->presentation.observe(component, &presentation_probe,
            &composition_presentation));
        CHECK(composition_presentation.generation > previous_generation);
    }

    annotation.style.background.red = 0x65;
    annotation.style.background.green = 0x43;
    annotation.style.background.blue = 0x21;
    CHECK(boxrig_ui_view_replace_text_annotations(
              view,
              73,
              annotation_hit.leading_position.screen,
              annotation_hit.leading_position.line_id,
              annotation_hit.leading_position.line_revision,
              &annotation,
              1) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_row_dirty(
              view,
              BOXRIG_UI_DIRTY_OVERLAY,
              1,
              &dirty));
    CHECK(dirty.column <= 1 &&
          dirty.column + dirty.column_count >= 2);
    CHECK(operations->frame.frame(component, &frame_result) == BOXRIG_UI_OK);
    CHECK(operations->presentation.observe(
        component,
        &presentation_probe,
        &dirty_presentation));
    CHECK(dirty_presentation.generation >
          composition_presentation.generation);
    CHECK(dirty_presentation.stable_hash ==
          composition_presentation.stable_hash);
    CHECK(pixel_rgb_is(dirty_presentation.meaning_argb, 0x65, 0x43, 0x21));

    CHECK(operations->presentation.get_transparency(
              component,
              &transparency_before) == BOXRIG_UI_OK);
    transparency = boxrig_ui_transparency_config_default();
    transparency.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
    transparency.requested_scopes =
        BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    transparency.background_opacity = 128;
    CHECK(operations->presentation.set_transparency(component, &transparency) ==
          (operations->presentation.requires_recreation_for_transparency
               ? BOXRIG_UI_UNAVAILABLE : BOXRIG_UI_OK));
    CHECK(operations->presentation.get_transparency(
              component,
              &transparency_after) ==
          BOXRIG_UI_OK);
    if (operations->presentation.requires_recreation_for_transparency) {
        CHECK(transparency_after.revision == transparency_before.revision);
        CHECK(transparency_after.effective_background_opacity == 255);
    } else {
        CHECK(transparency_after.config.request == transparency.request);
        CHECK(transparency_after.config.requested_scopes ==
              transparency.requested_scopes);
        CHECK(transparency_after.config.background_opacity ==
              transparency.background_opacity);
        CHECK(transparency_after.revision != transparency_before.revision);
    }
    transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
    CHECK(operations->presentation.set_transparency(
              component,
              &transparency) ==
          BOXRIG_UI_UNAVAILABLE);
    CHECK(operations->presentation.get_transparency(
              component,
              &transparency_after) ==
          BOXRIG_UI_OK);
    CHECK(transparency_after.config.request ==
          (operations->presentation.requires_recreation_for_transparency
               ? transparency_before.config.request
               : BOXRIG_UI_TRANSPARENCY_PREFERRED));
    pass_frontend_case(&coverage, BOXRIG_FIXTURE_CASE_COMPONENT_TRANSPARENCY);

    CHECK(boxrig_composition_focus_changed(composition, false) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_ui_view_clear_selection(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_clear_search(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_clear_hyperlink_hover(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_clear_text_annotations(view, 73) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(
              terminal,
              metrics.cell_width,
              metrics.cell_height) == BOXRIG_STATUS_OK);
    CHECK(feed_bytes(
        terminal,
        presentation_image,
        sizeof(presentation_image) - 1));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(operations->frame.frame(component, &frame_result) == BOXRIG_UI_OK);
    presentation_probe.background_x = (int)metrics.cell_width * 3 / 2;
    presentation_probe.background_y = (int)metrics.cell_height / 2;
    presentation_probe.meaning_x = (int)metrics.cell_width / 2;
    presentation_probe.meaning_y = (int)metrics.cell_height / 2;
    presentation_probe.coverage_x = 0;
    presentation_probe.coverage_y = 0;
    presentation_probe.coverage_width = metrics.cell_width;
    presentation_probe.coverage_height = metrics.cell_height;
    presentation_probe.stable_x = (int)metrics.cell_width;
    presentation_probe.stable_y = 0;
    presentation_probe.stable_width = metrics.cell_width;
    presentation_probe.stable_height = metrics.cell_height;
    CHECK(operations->presentation.observe(
        component,
        &presentation_probe,
        &image_presentation));
    CHECK(image_presentation.generation > dirty_presentation.generation);
    CHECK(image_presentation.width == allocation.width);
    CHECK(image_presentation.height == allocation.height);
    CHECK(pixel_rgb_is(image_presentation.meaning_argb, 0xff, 0x00, 0x00));
    CHECK(image_presentation.coverage_pixels != 0);
    if (transparency_after.effective ==
        BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA) {
        CHECK((transparency_after.effective_scopes &
               BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE) != 0);
        CHECK(transparency_after.effective_background_opacity == 128);
        CHECK(pixel_rgb_is(
            image_presentation.background_argb,
            0x10,
            0x20,
            0x30));
        if (image_presentation.alpha_observable) {
            CHECK((uint8_t)(image_presentation.background_argb >> 24) == 128);
            CHECK((uint8_t)(image_presentation.meaning_argb >> 24) == 255);
        }
    } else {
        CHECK(transparency_after.effective ==
              BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
        CHECK(transparency_after.effective_background_opacity == 255);
        CHECK(pixel_rgb_is(
            image_presentation.background_argb,
            0x20,
            0x40,
            0x60));
    }
    pass_frontend_case(
        &coverage,
        BOXRIG_FIXTURE_CASE_COMPONENT_PRESENTATION);

    capabilities = operations->presentation.capabilities(component);
    CHECK((capabilities & BOXRIG_UI_COMPONENT_CAPABILITY_OWNED_VISIBLE_CHILD) !=
          0);
    CHECK((capabilities & BOXRIG_UI_COMPONENT_CAPABILITY_VIEW_DIRTY) != 0);
    CHECK((capabilities & BOXRIG_UI_COMPONENT_CAPABILITY_FONT_RESOURCES) != 0);
    CHECK((capabilities & BOXRIG_UI_COMPONENT_CAPABILITY_CLIPBOARD_PASTE) != 0);
    CHECK(operations->presentation.get_diagnostic(component, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.running);
    CHECK(diagnostic.visible);
    CHECK(diagnostic.layout_authority);
    CHECK(diagnostic.layout_configured);
    CHECK(!diagnostic.input_blocked);
    pass_frontend_case(&coverage, BOXRIG_FIXTURE_CASE_COMPONENT_DIAGNOSTIC);

    CHECK(operations->layout.set_preferred_grid(component, 3, 12) ==
          BOXRIG_UI_OK);
    CHECK(operations->layout.get_preferred_grid(
              component,
              &preferred_rows,
              &preferred_columns));
    CHECK(preferred_rows == 3 && preferred_columns == 12);
    command_state = operations->layout.get_size_command_state(component);
    CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_READY);
    CHECK(operations->layout.take_layout_action(component, &layout_action));
    CHECK(layout_action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE);
    CHECK(layout_action.value.content_size.cause ==
          BOXRIG_UI_LAYOUT_CHANGE_PREFERRED_GRID);
    CHECK(layout_action.value.content_size.width ==
          (boxrig_ui_fixed)metrics.cell_width * 12 * BOXRIG_UI_FIXED_ONE);
    CHECK(layout_action.value.content_size.height ==
          (boxrig_ui_fixed)metrics.cell_height * 3 * BOXRIG_UI_FIXED_ONE);
    CHECK(!operations->layout.take_layout_action(component, &layout_action));
    command_state = operations->layout.get_size_command_state(component);
    CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_DISPATCHED);
    CHECK(operations->layout.complete_size_command(
              component,
              layout_action.revision + 1,
              BOXRIG_UI_SIZE_ACCEPTED,
              layout_action.value.content_size.width,
              layout_action.value.content_size.height,
              &applied,
              &layout) == BOXRIG_UI_OK);
    CHECK(!applied);
    command_state = operations->layout.get_size_command_state(component);
    CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_DISPATCHED);
    CHECK(operations->layout.complete_size_command(
              component,
              layout_action.revision,
              BOXRIG_UI_SIZE_REJECTED,
              (boxrig_ui_fixed)allocation.width * BOXRIG_UI_FIXED_ONE,
              (boxrig_ui_fixed)allocation.height * BOXRIG_UI_FIXED_ONE,
              &applied,
              &layout) == BOXRIG_UI_OK);
    CHECK(applied && layout.constrained);
    CHECK(layout.rows == terminal_config.rows);
    CHECK(layout.columns == terminal_config.columns);
    CHECK(operations->layout.get_preferred_grid(
              component,
              &preferred_rows,
              &preferred_columns));
    CHECK(preferred_rows == terminal_config.rows);
    CHECK(preferred_columns == terminal_config.columns);

    CHECK(operations->layout.set_preferred_grid(component, 3, 12) ==
          BOXRIG_UI_OK);
    CHECK(operations->layout.take_layout_action(component, &layout_action));
    CHECK(layout_action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE);
    CHECK(operations->layout.complete_size_command(
              component,
              layout_action.revision,
              BOXRIG_UI_SIZE_ACCEPTED,
              layout_action.value.content_size.width,
              layout_action.value.content_size.height,
              &applied,
              &layout) == BOXRIG_UI_OK);
    CHECK(applied && !layout.constrained);
    CHECK(layout.rows == 3 && layout.columns == 12);
    CHECK(operations->layout.get_size_command_state(component) ==
          BOXRIG_UI_SIZE_COMMAND_IDLE);
    CHECK(boxrig_terminal_resize(terminal, 3, 12) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    pass_frontend_case(&coverage, BOXRIG_FIXTURE_CASE_COMPONENT_SIZE_COMMAND);

    CHECK(operations->lifecycle.quiesce_component(component) == BOXRIG_UI_OK);
    CHECK(operations->presentation.get_diagnostic(component, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(!diagnostic.running);
    frame_request_count_at_destroy = frame_request_count;
    CHECK(feed_bytes(terminal, (const uint8_t *)"!", 1));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(frame_request_count == frame_request_count_at_destroy);
    pass_frontend_case(&coverage, BOXRIG_FIXTURE_CASE_COMPONENT_QUIESCE);
    operations->lifecycle.destroy_component(component);
    component = NULL;
    CHECK(operations->lifecycle.create_component(
              host,
              model,
              BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY,
              &replacement) == BOXRIG_UI_OK);
    CHECK(replacement != NULL);
    CHECK(operations->presentation.get_diagnostic(replacement, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.layout_authority);
    pass_frontend_case(&coverage, BOXRIG_FIXTURE_CASE_COMPONENT_LIFECYCLE);
    CHECK(audit_frontend_cases(&coverage, operations->frontend_name));

    operations->lifecycle.destroy_component(replacement);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    operations->lifecycle.destroy_host(host);
    printf(
        "PASS shared frontend component conformance through %s public API\n",
        operations->frontend_name);
    return true;
}
