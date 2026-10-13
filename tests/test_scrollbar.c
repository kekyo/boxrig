/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "boxrig/ui/scrollbar.h"
#include "boxrig/ui/view.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(                                                        \
                stderr,                                                     \
                "FAIL %s:%d: %s\n",                                       \
                __FILE__,                                                   \
                __LINE__,                                                   \
                #condition);                                                \
            return false;                                                   \
        }                                                                   \
    } while (0)

static bool feed_text(boxrig_terminal *terminal, const char *text)
{
    size_t consumed = 0;
    size_t length = strlen(text);

    return boxrig_terminal_feed(
               terminal,
               (const uint8_t *)text,
               length,
               &consumed) == BOXRIG_STATUS_OK &&
           consumed == length;
}

static bool test_scrollbar_actions_follow_terminal_viewport(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_scrollbar_model model;
    size_t original_history;
    size_t original_position;
    size_t expected_offset;
    bool changed = false;

    config.rows = 4;
    config.columns = 8;
    config.scrollback_capacity = 32;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_scrollbar_model_read(terminal, &model) == 0);
    CHECK(model.viewport_rows == 4);
    CHECK(model.history_rows == 0);
    CHECK(model.viewport_offset == 0);
    CHECK(model.position == 0);
    CHECK(!model.enabled);

    CHECK(feed_text(
        terminal,
        "00\r\n01\r\n02\r\n03\r\n04\r\n05\r\n06\r\n07\r\n"));
    CHECK(boxrig_scrollbar_model_read(terminal, &model) == 0);
    CHECK(model.history_rows >= 4);
    CHECK(model.viewport_offset == 0);
    CHECK(model.position == model.history_rows);
    CHECK(model.enabled);
    original_history = model.history_rows;

    CHECK(boxrig_scrollbar_apply_action(
              terminal,
              BOXRIG_SCROLLBAR_LINE_UP,
              0,
              &changed) == 0);
    CHECK(changed);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 1);

    CHECK(boxrig_scrollbar_apply_action(
              terminal,
              BOXRIG_SCROLLBAR_PAGE_UP,
              0,
              &changed) == 0);
    expected_offset = original_history < 5 ? original_history : 5;
    CHECK(changed == (expected_offset != 1));
    CHECK(boxrig_terminal_viewport_offset(terminal) == expected_offset);

    CHECK(boxrig_scrollbar_apply_action(
              terminal,
              BOXRIG_SCROLLBAR_TOP,
              0,
              &changed) == 0);
    CHECK(boxrig_terminal_viewport_offset(terminal) == original_history);
    CHECK(boxrig_scrollbar_model_read(terminal, &model) == 0);
    CHECK(model.position == 0);

    CHECK(boxrig_scrollbar_apply_action(
              terminal,
              BOXRIG_SCROLLBAR_LINE_UP,
              0,
              &changed) == 0);
    CHECK(!changed);
    CHECK(boxrig_scrollbar_apply_action(
              terminal,
              BOXRIG_SCROLLBAR_PAGE_DOWN,
              0,
              &changed) == 0);
    expected_offset = original_history > model.viewport_rows
                          ? original_history - model.viewport_rows
                          : 0;
    CHECK(boxrig_terminal_viewport_offset(terminal) == expected_offset);

    CHECK(boxrig_scrollbar_apply_action(
              terminal,
              BOXRIG_SCROLLBAR_BOTTOM,
              0,
              &changed) == 0);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 0);
    CHECK(boxrig_scrollbar_apply_action(
              terminal,
              BOXRIG_SCROLLBAR_POSITION,
              original_history / 2,
              &changed) == 0);
    CHECK(
        boxrig_terminal_viewport_offset(terminal) ==
        original_history - original_history / 2);
    CHECK(boxrig_scrollbar_apply_action(
              terminal,
              BOXRIG_SCROLLBAR_POSITION,
              SIZE_MAX,
              &changed) == 0);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 0);

    CHECK(boxrig_scrollbar_apply_action(
              terminal,
              BOXRIG_SCROLLBAR_LINE_UP,
              0,
              &changed) == 0);
    CHECK(boxrig_scrollbar_model_read(terminal, &model) == 0);
    original_position = model.position;
    CHECK(feed_text(terminal, "08\r\n"));
    CHECK(boxrig_scrollbar_model_read(terminal, &model) == 0);
    CHECK(model.history_rows == original_history + 1);
    CHECK(model.viewport_offset == 2);
    CHECK(model.position == original_position);

    CHECK(boxrig_scrollbar_apply_action(
              terminal,
              (boxrig_scrollbar_action)99,
              0,
              &changed) == -1);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_scrollbar_disables_on_alternate_screen(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_scrollbar_model model;
    size_t primary_offset;
    bool changed = true;

    config.rows = 3;
    config.columns = 8;
    config.scrollback_capacity = 16;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed_text(terminal, "0\r\n1\r\n2\r\n3\r\n4\r\n"));
    CHECK(boxrig_scrollbar_apply_action(
              terminal,
              BOXRIG_SCROLLBAR_LINE_UP,
              0,
              &changed) == 0);
    CHECK(changed);
    primary_offset = boxrig_terminal_viewport_offset(terminal);
    CHECK(primary_offset == 1);

    CHECK(feed_text(terminal, "\033[?1049h"));
    CHECK(boxrig_scrollbar_model_read(terminal, &model) == 0);
    CHECK(!model.enabled);
    CHECK(boxrig_scrollbar_apply_action(
              terminal,
              BOXRIG_SCROLLBAR_TOP,
              0,
              &changed) == 0);
    CHECK(!changed);
    CHECK(feed_text(terminal, "\033[?1049l"));
    CHECK(boxrig_scrollbar_model_read(terminal, &model) == 0);
    CHECK(model.enabled);
    CHECK(model.viewport_offset == primary_offset);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_scrollbar_thumb_geometry_and_drag_mapping(void)
{
    boxrig_scrollbar_model model;
    boxrig_scrollbar_thumb thumb;
    size_t midpoint;

    memset(&model, 0, sizeof(model));
    boxrig_scrollbar_calculate_thumb(&model, 100, 12, &thumb);
    CHECK(thumb.start == 0);
    CHECK(thumb.length == 100);
    CHECK(boxrig_scrollbar_position_from_thumb(&model, 100, 12, 80) == 0);

    model.enabled = true;
    model.history_rows = 1000;
    model.viewport_rows = 1;
    model.viewport_offset = 1000;
    model.position = 0;
    boxrig_scrollbar_calculate_thumb(&model, 100, 12, &thumb);
    CHECK(thumb.start == 0);
    CHECK(thumb.length == 12);
    CHECK(boxrig_scrollbar_position_from_thumb(&model, 100, 12, 0) == 0);
    CHECK(
        boxrig_scrollbar_position_from_thumb(&model, 100, 12, 100) ==
        model.history_rows);

    model.viewport_offset = 0;
    model.position = model.history_rows;
    boxrig_scrollbar_calculate_thumb(&model, 100, 12, &thumb);
    CHECK(thumb.start + thumb.length == 100);

    midpoint = boxrig_scrollbar_position_from_thumb(
        &model,
        100,
        12,
        (100 - 12) / 2);
    CHECK(midpoint >= 499 && midpoint <= 501);
    model.position = midpoint;
    model.viewport_offset = model.history_rows - midpoint;
    boxrig_scrollbar_calculate_thumb(&model, 100, 12, &thumb);
    CHECK(thumb.start == (100 - 12) / 2);

    boxrig_scrollbar_calculate_thumb(&model, 5, 12, &thumb);
    CHECK(thumb.start == 0);
    CHECK(thumb.length == 5);
    boxrig_scrollbar_calculate_thumb(&model, 0, 12, &thumb);
    CHECK(thumb.start == 0);
    CHECK(thumb.length == 0);
    return true;
}

static void count_scrollbar_frame(void *context)
{
    (*(size_t *)context)++;
}

static bool test_external_scrollbar_state_and_notifications(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 3;
    config.columns = 8;
    config.scrollback_capacity = 4;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *first = NULL, *second = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &second) == BOXRIG_UI_OK);
    size_t first_frames = 0, second_frames = 0;
    CHECK(boxrig_ui_view_set_frame_request_callback(first, count_scrollbar_frame, &first_frames) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_frame_request_callback(second, count_scrollbar_frame, &second_frames) == BOXRIG_UI_OK);
    for (int layer = 0; layer < BOXRIG_UI_DIRTY_LAYER_COUNT; layer++) {
        boxrig_ui_dirty dirty;
        while (boxrig_ui_view_peek_dirty(first, layer, &dirty))
            CHECK(boxrig_ui_view_consume_dirty(first, layer) == BOXRIG_UI_OK);
        while (boxrig_ui_view_peek_dirty(second, layer, &dirty))
            CHECK(boxrig_ui_view_consume_dirty(second, layer) == BOXRIG_UI_OK);
    }
    boxrig_scrollbar_state before, after, peer;
    CHECK(boxrig_ui_view_get_scrollbar_state(first, &before) == BOXRIG_UI_OK);
    CHECK(before.revision != 0 && !before.model.enabled && before.model.viewport_rows == 3);
    CHECK(feed_text(terminal, "0\r\n1\r\n2\r\n3\r\n4\r\n5"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_scrollbar_state(first, &after) == BOXRIG_UI_OK);
    CHECK(after.revision != before.revision && after.model.enabled);
    CHECK(after.model.history_rows == 3 && after.model.position == 3);
    CHECK(first_frames == 1 && second_frames == 1);
    CHECK(boxrig_ui_view_get_scrollbar_state(second, &peer) == BOXRIG_UI_OK);
    CHECK(peer.revision == after.revision && peer.model.position == after.model.position);
    before = after;
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_scrollbar_state(first, &after) == BOXRIG_UI_OK);
    CHECK(after.revision == before.revision && first_frames == 1);

    bool changed = false;
    CHECK(boxrig_ui_view_apply_scrollbar_action(first, BOXRIG_SCROLLBAR_TOP, 0, &changed) == BOXRIG_UI_OK && changed);
    CHECK(boxrig_ui_view_get_scrollbar_state(second, &after) == BOXRIG_UI_OK);
    CHECK(after.model.position == 0 && after.model.viewport_offset == 3 && after.revision != before.revision);
    boxrig_cell_view cell;
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == '0');
    before = after;
    CHECK(boxrig_ui_view_apply_scrollbar_action(second, BOXRIG_SCROLLBAR_POSITION, after.model.position, &changed) == BOXRIG_UI_OK && !changed);
    CHECK(boxrig_ui_view_get_scrollbar_state(first, &after) == BOXRIG_UI_OK);
    CHECK(before.revision == after.revision);

    /* Output while browsing history retains the viewed row until eviction. */
    CHECK(feed_text(terminal, "\r\n6"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_scrollbar_state(first, &after) == BOXRIG_UI_OK);
    CHECK(after.model.history_rows == 4 && after.model.viewport_offset == 4 && after.model.position == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK && cell.text[0] == '0');
    CHECK(feed_text(terminal, "\r\n7"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK && cell.text[0] == '1');
    CHECK(boxrig_terminal_resize(terminal, 4, 8) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_scrollbar_state(first, &after) == BOXRIG_UI_OK && after.model.viewport_rows == 4);
    CHECK(feed_text(terminal, "\033[?1049h"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_scrollbar_state(first, &after) == BOXRIG_UI_OK && !after.model.enabled);
    before = after;
    CHECK(boxrig_ui_view_apply_scrollbar_action(first, BOXRIG_SCROLLBAR_BOTTOM, 0, &changed) == BOXRIG_UI_OK && !changed);
    CHECK(boxrig_ui_view_get_scrollbar_state(first, &after) == BOXRIG_UI_OK && after.revision == before.revision);
    CHECK(feed_text(terminal, "\033[?1049l"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_apply_scrollbar_action(first, BOXRIG_SCROLLBAR_BOTTOM, 0, &changed) == BOXRIG_UI_OK);
    for (int layer = 0; layer < BOXRIG_UI_DIRTY_LAYER_COUNT; layer++) {
        boxrig_ui_dirty dirty;
        while (boxrig_ui_view_peek_dirty(first, layer, &dirty))
            CHECK(boxrig_ui_view_consume_dirty(first, layer) == BOXRIG_UI_OK);
        while (boxrig_ui_view_peek_dirty(second, layer, &dirty))
            CHECK(boxrig_ui_view_consume_dirty(second, layer) == BOXRIG_UI_OK);
    }
    const size_t previous_frames = first_frames;
    CHECK(boxrig_ui_view_get_scrollbar_state(first, &before) == BOXRIG_UI_OK);
    /* Removing only history still wakes a clean external scrollbar. */
    CHECK(feed_text(terminal, "\033[3J"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_scrollbar_state(first, &after) == BOXRIG_UI_OK);
    CHECK(!after.model.enabled && after.model.history_rows == 0 && after.revision != before.revision);
    CHECK(first_frames == previous_frames + 1);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK && first_frames == previous_frames + 1);
    CHECK(boxrig_ui_view_get_scrollbar_state(NULL, &after) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_ui_view_get_scrollbar_state(first, NULL) == BOXRIG_UI_INVALID_ARGUMENT);
    boxrig_ui_view_destroy(second);
    boxrig_ui_view_destroy(first);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    static const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"scrollbar actions follow terminal viewport",
         test_scrollbar_actions_follow_terminal_viewport},
        {"scrollbar disables on alternate screen",
         test_scrollbar_disables_on_alternate_screen},
        {"scrollbar thumb geometry and drag mapping",
         test_scrollbar_thumb_geometry_and_drag_mapping},
        {"external scrollbar state, text and frame notifications",
         test_external_scrollbar_state_and_notifications},
    };
    size_t index;

    for (index = 0; index < sizeof(tests) / sizeof(tests[0]); index++) {
        if (!tests[index].run()) {
            fprintf(stderr, "FAILED: %s\n", tests[index].name);
            return 1;
        }
        printf("PASS %s\n", tests[index].name);
    }
    return 0;
}
