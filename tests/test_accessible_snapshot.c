/* boxrig - Under MIT. */
#include "boxrig/ui.h"
#include "../src/ui/accessible.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    abort(); } } while (0)

static void feed(boxrig_terminal *terminal, const char *text)
{
    size_t consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text,
        strlen(text), &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == strlen(text));
}

static boxrig_ui_accessible_snapshot *read_snapshot(boxrig_ui_view *view,
    const char *text, size_t caret)
{
    CHECK(boxrig_ui_model_synchronize(boxrig_ui_view_model(view)) == BOXRIG_UI_OK);
    boxrig_ui_accessible_snapshot *snapshot = NULL;
    CHECK(boxrig_ui_accessible_snapshot_create(view,
        BOXRIG_UI_ACCESSIBLE_MAXIMUM_BYTES, &snapshot) == BOXRIG_UI_OK);
    CHECK(snapshot != NULL);
    CHECK(boxrig_ui_accessible_snapshot_is_current(view, snapshot));
    if (strcmp(snapshot->text, text) != 0)
        fprintf(stderr, "expected [%s], got [%s]\n", text, snapshot->text);
    CHECK(strcmp(snapshot->text, text) == 0);
    CHECK(snapshot->byte_count == strlen(text));
    CHECK(snapshot->caret == caret);
    return snapshot;
}

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 4;
    config.columns = 20;
    config.scrollback_capacity = 100;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL, *second = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &second) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    boxrig_ui_allocation_input allocation = {
        .allocation_width = 200 * BOXRIG_UI_FIXED_ONE,
        .allocation_height = 80 * BOXRIG_UI_FIXED_ONE,
        .cell_width = 10 * BOXRIG_UI_FIXED_ONE,
        .cell_height = 20 * BOXRIG_UI_FIXED_ONE,
        .cell_metrics_revision = 1,
        .window_state = BOXRIG_UI_WINDOW_NORMAL
    };
    boxrig_ui_layout_result layout;
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    boxrig_ui_accessible_snapshot *empty = read_snapshot(view, "\n\n\n\n", 0);
    CHECK(empty->character_count == 4 && empty->row_count == 4);

    feed(terminal, "A日本é🙂\r\nline two");
    boxrig_ui_accessible_snapshot *unicode = read_snapshot(view,
        "A日本é🙂\nline two\n\n\n", 15);
    CHECK(unicode->character_count == 18);
    CHECK(unicode->characters[1].scalar == 0x65e5);
    CHECK(unicode->characters[1].byte_offset == 1);
    CHECK(unicode->characters[2].byte_offset == 4);
    CHECK(unicode->characters[3].column == 5);
    CHECK(unicode->characters[4].column == 5);
    CHECK(unicode->characters[3].grapheme_start == 3);
    CHECK(unicode->characters[4].grapheme_start == 3);
    CHECK(unicode->characters[3].grapheme_end == 5);
    CHECK(unicode->characters[1].width == 20 * BOXRIG_UI_FIXED_ONE);
    CHECK(unicode->characters[1].x == 10 * BOXRIG_UI_FIXED_ONE);
    CHECK(unicode->characters[4].x == unicode->characters[3].x);
    CHECK(unicode->characters[18].byte_offset == unicode->byte_count);

    boxrig_ui_hit_test_result start, end;
    CHECK(boxrig_ui_view_hit_test(view, 10 * BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &start) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 50 * BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &end) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER, &start) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &end) == BOXRIG_UI_OK);
    boxrig_ui_accessible_snapshot *selected = read_snapshot(view,
        "A日本é🙂\nline two\n\n\n", 15);
    CHECK(!boxrig_ui_accessible_snapshot_is_current(view, unicode));
    CHECK(selected->selection_count == 1);
    CHECK(selected->selections[0].start == 1 && selected->selections[0].end == 5);
    boxrig_ui_accessible_snapshot *other = read_snapshot(second,
        "A日本é🙂\nline two\n\n\n", 15);
    CHECK(other->selection_count == 0);
    CHECK(unicode->selection_count == 0);
    boxrig_ui_accessible_snapshot_destroy(other);
    CHECK(boxrig_ui_view_clear_selection(view) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_accessible_snapshot_is_current(view, selected));
    boxrig_ui_accessible_snapshot_destroy(selected);

    feed(terminal, "\rX\033[K");
    boxrig_ui_accessible_snapshot *replacement = read_snapshot(view,
        "A日本é🙂\nX\n\n\n", 8);
    boxrig_ui_accessible_change change;
    boxrig_ui_accessible_snapshot_diff(unicode, replacement, &change);
    CHECK(change.start == 7 && change.removed == 8 && change.inserted == 1);
    boxrig_ui_accessible_snapshot_diff(replacement, replacement, &change);
    CHECK(change.removed == 0 && change.inserted == 0);
    CHECK(strcmp(unicode->text, "A日本é🙂\nline two\n\n\n") == 0);

    feed(terminal, "\033[2J\033[H0123456789abcdefghijK\r\nlast");
    boxrig_ui_accessible_snapshot *wrapped = read_snapshot(view,
        "0123456789abcdefghijK\nlast\n\n", 26);
    CHECK(wrapped->rows[0].end == 20 && wrapped->rows[1].start == 20);
    CHECK(wrapped->characters[20].row == 1);
    CHECK(boxrig_terminal_resize(terminal, 4, 10) == BOXRIG_STATUS_OK);
    /* boxrig uses the vacant final row during reflow, retaining the first
     * fragment in this viewport; VTE scrolls that fragment into history. */
    boxrig_ui_accessible_snapshot *reflow = read_snapshot(view,
        "0123456789abcdefghijK\nlast\n", 26);

    feed(terminal, "\033[2J\033[Hrow1\r\nrow2\r\nrow3\r\nrow4\r\nrow5");
    boxrig_ui_accessible_snapshot *live = read_snapshot(view,
        "row2\nrow3\nrow4\nrow5\n", 19);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) == BOXRIG_STATUS_OK);
    boxrig_ui_accessible_snapshot *history = read_snapshot(view,
        "row1\nrow2\nrow3\nrow4\n", 20);
    feed(terminal, "\033[?1049h\033[Halt日本");
    boxrig_ui_accessible_snapshot *alternate = read_snapshot(view,
        "alt日本\n\n\n\n", 5);
    feed(terminal, "\033[?1049l");
    boxrig_ui_accessible_snapshot *primary = read_snapshot(view,
        "row1\nrow2\nrow3\nrow4\n", 20);

    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) == BOXRIG_STATUS_OK);
    feed(terminal, "\033[2J\033[H\033[4G\033[1;3;4;31mX \033[0m");
    boxrig_ui_accessible_snapshot *spaces = read_snapshot(view, "   X \n\n\n\n", 5);
    CHECK((spaces->characters[3].attributes & (BOXRIG_CELL_BOLD | BOXRIG_CELL_ITALIC)) ==
        (BOXRIG_CELL_BOLD | BOXRIG_CELL_ITALIC));
    CHECK(spaces->characters[3].underline == BOXRIG_UNDERLINE_SINGLE);
    CHECK(spaces->characters[3].foreground.red > spaces->characters[3].foreground.green);
    boxrig_ui_accessible_snapshot *limited = (void *)1;
    CHECK(boxrig_ui_accessible_snapshot_create(view, 1, &limited) == BOXRIG_UI_LIMIT_EXCEEDED);
    CHECK(limited == NULL);
    CHECK(boxrig_ui_accessible_snapshot_create(view, spaces->memory_bytes - 1,
        &limited) == BOXRIG_UI_LIMIT_EXCEEDED && limited == NULL);
    CHECK(boxrig_ui_accessible_snapshot_create(view, spaces->memory_bytes,
        &limited) == BOXRIG_UI_OK);
    boxrig_ui_accessible_snapshot_destroy(limited);

    feed(terminal, "\033[2J\033[Habcde\r\nfghij");
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 11 * BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &start) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 31 * BOXRIG_UI_FIXED_ONE,
        21 * BOXRIG_UI_FIXED_ONE, &end) == BOXRIG_UI_OK);
    boxrig_ui_selection_options rectangle = {
        .shape = BOXRIG_UI_SELECTION_RECTANGULAR,
        .kind = BOXRIG_UI_SELECTION_CHARACTER
    };
    CHECK(boxrig_ui_view_begin_selection_with_options(view, &rectangle, &start) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &end) == BOXRIG_UI_OK);
    boxrig_ui_accessible_snapshot *rectangular = read_snapshot(view, "abcde\nfghij\n\n\n", 11);
    CHECK(rectangular->selection_count == 2);
    CHECK(rectangular->selections[0].start == 1 && rectangular->selections[0].end == 3);
    CHECK(rectangular->selections[1].start == 7 && rectangular->selections[1].end == 9);
    CHECK(boxrig_ui_view_clear_selection(view) == BOXRIG_UI_OK);

    feed(terminal, "\033[2J\033[HAאבB");
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    const boxrig_text_cluster_input bidi_clusters[] = {
        {0, 1, 0, 1, 0, 1, 0, 1, 1, 1},
        {1, 1, 1, 2, 1, 1, 0, 1, 1, 1},
        {2, 1, 3, 2, 1, 1, 0, 1, 1, 1},
        {3, 1, 5, 1, 0, 1, 0, 1, 1, 1},
        {4, 1, 6, 1, 0, 1, 0, 1, 1, 1},
        {5, 1, 7, 1, 0, 1, 0, 1, 1, 1},
        {6, 1, 8, 1, 0, 1, 0, 1, 1, 1},
        {7, 1, 9, 1, 0, 1, 0, 1, 1, 1},
        {8, 1, 10, 1, 0, 1, 0, 1, 1, 1},
        {9, 1, 11, 1, 0, 1, 0, 1, 1, 1}
    };
    boxrig_text_layout *bidi_layout = NULL;
    CHECK(boxrig_text_layout_create(bidi_clusters, 10, 10, BOXRIG_TEXT_DIRECTION_LTR,
        &bidi_layout) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(boxrig_ui_view_store_row_layout(view, 0, bidi_layout, 1024, 1) == BOXRIG_UI_OK);
    boxrig_ui_accessible_snapshot *bidi = read_snapshot(view, "AאבB\n\n\n\n", 4);
    CHECK(bidi->characters[1].x == 20 * BOXRIG_UI_FIXED_ONE);
    CHECK(bidi->characters[2].x == 10 * BOXRIG_UI_FIXED_ONE);

    boxrig_ui_view_destroy(second);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(strcmp(unicode->text, "A日本é🙂\nline two\n\n\n") == 0);
    boxrig_ui_accessible_snapshot_destroy(empty);
    boxrig_ui_accessible_snapshot_destroy(unicode);
    boxrig_ui_accessible_snapshot_destroy(replacement);
    boxrig_ui_accessible_snapshot_destroy(wrapped);
    boxrig_ui_accessible_snapshot_destroy(reflow);
    boxrig_ui_accessible_snapshot_destroy(live);
    boxrig_ui_accessible_snapshot_destroy(history);
    boxrig_ui_accessible_snapshot_destroy(alternate);
    boxrig_ui_accessible_snapshot_destroy(primary);
    boxrig_ui_accessible_snapshot_destroy(spaces);
    boxrig_ui_accessible_snapshot_destroy(rectangular);
    boxrig_ui_accessible_snapshot_destroy(bidi);
    puts("PASS accessible viewport snapshots");
    return 0;
}
