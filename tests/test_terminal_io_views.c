/* boxrig - Under MIT. */
#include "boxrig/ui.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    for (unsigned encoding = 0; encoding < 2; encoding++) {
        const uint8_t mapped[] = {0x1b, 'U', 0};
        const boxrig_key_mapping mapping = {BOXRIG_KEY_UP, 0, mapped, sizeof(mapped)};
        boxrig_terminal_config config = boxrig_terminal_config_default();
        config.rows = 2;
        config.columns = 6;
        config.encoding = (boxrig_encoding)encoding;
        config.input.enter_newline = BOXRIG_NEWLINE_LF;
        config.paste_newline = BOXRIG_NEWLINE_CRLF;
        config.receive_newline = BOXRIG_RECEIVE_NEWLINE_CRLF;
        config.input.key_mappings = &mapping;
        config.input.key_mapping_count = 1;
        boxrig_terminal *terminal = NULL;
        boxrig_ui_model *model = NULL;
        boxrig_ui_view *views[2] = {NULL, NULL};
        boxrig_text_snapshot *copies[2] = {NULL, NULL};
        assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        const char *wire = encoding ? "\x93\xfa\x96\x7b\x8c\xea\r\nABC" : "日本語\r\nABC";
        for (size_t offset = 0; offset < strlen(wire); offset++) {
            size_t consumed = 0;
            assert(boxrig_terminal_feed(terminal, (const uint8_t *)wire + offset,
                1, &consumed) == BOXRIG_STATUS_OK && consumed == 1);
        }
        assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
        boxrig_ui_view_config vc = boxrig_ui_view_config_default();
        for (unsigned index = 0; index < 2; index++)
            assert(boxrig_ui_view_create(model, &vc, &views[index]) == BOXRIG_UI_OK);
        assert(boxrig_ui_view_acquire_layout_authority(views[0]) == BOXRIG_UI_OK);
        const boxrig_font_effective_metrics metrics = {
            .requested_size_dip = 16, .primary_face_key = 1,
            .dpi_x = 96, .dpi_y = 96, .output_scale = 1,
            .glyph_width = 10, .glyph_height = 20, .ascent = 16, .descent = 4,
            .cell_width = 10, .cell_height = 20, .baseline = 16,
            .layout_revision = 1};
        assert(boxrig_ui_view_commit_cell_metrics(views[0], &metrics) == BOXRIG_UI_OK);
        assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        for (unsigned index = 0; index < 2; index++) {
            boxrig_ui_view *view = views[index];
            assert(boxrig_ui_view_terminal(view) == terminal);
            boxrig_ui_allocation_input allocation = {0};
            allocation.allocation_width = 60 * BOXRIG_UI_FIXED_ONE;
            allocation.allocation_height = 40 * BOXRIG_UI_FIXED_ONE;
            allocation.cell_width = 10 * BOXRIG_UI_FIXED_ONE;
            allocation.cell_height = 20 * BOXRIG_UI_FIXED_ONE;
            allocation.cell_metrics_revision = 1;
            allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
            boxrig_ui_layout_result layout_result;
            assert(boxrig_ui_view_accept_allocation(view, &allocation,
                &layout_result) == BOXRIG_UI_OK);
            const boxrig_text_cluster_input clusters[] = {
                {0, 2, 0, 3, 0, 1, 0, 1, 1, 1, false},
                {2, 2, 3, 3, 0, 1, 0, 1, 1, 1, false},
                {4, 2, 6, 3, 0, 1, 0, 1, 1, 1, false}};
            boxrig_text_layout *layout = NULL;
            assert(boxrig_text_layout_create(clusters, 3, 6,
                BOXRIG_TEXT_DIRECTION_LTR, &layout) == BOXRIG_TEXT_LAYOUT_OK);
            assert(boxrig_ui_view_store_row_layout(view, 0, layout, 512, 1) == BOXRIG_UI_OK);
            boxrig_text_search_options search = {0};
            search.screen = BOXRIG_TEXT_SCREEN_PRIMARY;
            search.pattern = (const uint8_t *)"本";
            search.pattern_length = 3;
            search.direction = BOXRIG_TEXT_SEARCH_FORWARD;
            assert(boxrig_ui_view_begin_search(view, &search) == BOXRIG_UI_OK);
            boxrig_text_search_progress progress = BOXRIG_TEXT_SEARCH_IN_PROGRESS;
            while (progress == BOXRIG_TEXT_SEARCH_IN_PROGRESS) {
                size_t used;
                assert(boxrig_ui_view_resume_search(view, 1, &used, &progress) == BOXRIG_UI_OK);
                assert(used <= 1);
            }
            assert(progress == BOXRIG_TEXT_SEARCH_MATCH);
            boxrig_text_search_result match;
            assert(boxrig_ui_view_get_search_result(view, &match));
            assert(match.range.start.column == 2 && match.range.end.column == 4);
            boxrig_ui_hit_test_result hit;
            assert(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE,
                BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
            assert(hit.text_position_valid);
            assert(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_LINE, &hit) == BOXRIG_UI_OK);
            assert(boxrig_ui_view_create_selection_snapshot(view, &copies[index]) == BOXRIG_UI_OK);
            size_t copied_length;
            const uint8_t *copied = boxrig_text_snapshot_data(copies[index], &copied_length);
            assert(copied_length == 10 && memcmp(copied, "日本語\n", 10) == 0);

            boxrig_composition *composition = NULL;
            assert(boxrig_composition_create(boxrig_ui_view_terminal(view),
                boxrig_composition_limits_default(), &composition) == BOXRIG_COMPOSITION_OK);
            assert(boxrig_composition_focus_changed(composition, true) == BOXRIG_COMPOSITION_OK);
            boxrig_composition_event event = {0};
            event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
            event.text = "日本語";
            event.text_length = 9;
            event.caret_offset = 9;
            assert(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
            const uint8_t *output;
            assert(boxrig_terminal_output_peek(terminal, &output) == 0);
            event.type = BOXRIG_COMPOSITION_COMMIT;
            event.caret_offset = 0;
            assert(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
            boxrig_key_event key = {.key = BOXRIG_KEY_UP, .action = BOXRIG_KEY_PRESS};
            assert(boxrig_terminal_send_key(boxrig_ui_view_terminal(view), &key) == BOXRIG_STATUS_OK);
            key.key = BOXRIG_KEY_ENTER;
            assert(boxrig_terminal_send_key(boxrig_ui_view_terminal(view), &key) == BOXRIG_STATUS_OK);
            const uint8_t utf8_expected[] = "日本語\033U\0\n";
            const uint8_t sjis_expected[] = "\x93\xfa\x96\x7b\x8c\xea\033U\0\n";
            const uint8_t *expected = encoding ? sjis_expected : utf8_expected;
            size_t expected_length = encoding ? sizeof(sjis_expected) - 1 : sizeof(utf8_expected) - 1;
            assert(boxrig_terminal_output_peek(terminal, &output) == expected_length);
            assert(memcmp(output, expected, expected_length) == 0);
            assert(boxrig_terminal_output_consume(terminal, expected_length) == BOXRIG_STATUS_OK);
            boxrig_composition_destroy(composition);
        }
        assert(boxrig_ui_view_clear_search(views[0]) == BOXRIG_UI_OK);
        assert(boxrig_ui_view_clear_selection(views[0]) == BOXRIG_UI_OK);
        boxrig_text_search_result retained_match;
        boxrig_ui_selection retained_selection;
        assert(boxrig_ui_view_get_search_result(views[1], &retained_match));
        assert(boxrig_ui_view_get_selection(views[1], &retained_selection));
        for (unsigned index = 0; index < 2; index++) {
            size_t length;
            const uint8_t *copy = boxrig_text_snapshot_data(copies[index], &length);
            assert(length == 10 && memcmp(copy, "日本語\n", 10) == 0);
            boxrig_text_snapshot_destroy(copies[index]);
            boxrig_ui_view_destroy(views[index]);
        }
        boxrig_ui_model_destroy(model);
        boxrig_terminal_destroy(terminal);
    }
    puts("PASS shared terminal codec, independent Unicode search/copy and two-view IME/key input");
    return 0;
}
