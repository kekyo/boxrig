/* boxrig - Under MIT. */
#include "boxrig/terminal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *original = NULL;
    boxrig_terminal *restored = NULL;
    uint8_t *checkpoint = NULL;
    size_t length = 0;
    size_t consumed = 0;
    boxrig_cell_view before;
    boxrig_cell_view after;
    /* Typical sparse screens should fit in a small IPC message. */
    boxrig_terminal *empty = NULL;
    assert(boxrig_terminal_create(&config, &empty) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_export_checkpoint(empty, 16 * 1024,
        &checkpoint, &length) == BOXRIG_STATUS_OK);
    boxrig_checkpoint_free(checkpoint);
    boxrig_terminal_destroy(empty);
    config.rows = 3;
    config.columns = 6;
    config.scrollback_capacity = 8;
    assert(boxrig_terminal_create(&config, &original) == BOXRIG_STATUS_OK);
    const char *prefix = "\033[31mAe\314\201\347\225\214\033[0m\r\nBB\r\nCC\033[S";
    assert(boxrig_terminal_feed(original, (const uint8_t *)prefix,
        strlen(prefix), &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == strlen(prefix));
    assert(boxrig_terminal_set_viewport_offset(original, 1) == BOXRIG_STATUS_OK);
    boxrig_text_range selection;
    assert(!boxrig_terminal_get_text_position(original, BOXRIG_TEXT_SCREEN_PRIMARY,
        0, 1, &selection.start));
    assert(!boxrig_terminal_get_text_position(original, BOXRIG_TEXT_SCREEN_PRIMARY,
        0, 4, &selection.end));
    assert(!boxrig_terminal_set_scrollback_compression(original,
        BOXRIG_SCROLLBACK_COMPRESSION_DEFLATE_LEVEL_1));
    assert(boxrig_terminal_get_cell(original, 0, 1, &before) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_export_checkpoint(original, 1024 * 1024,
        &checkpoint, &length) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_import_checkpoint(checkpoint, length, 1024 * 1024,
        &restored) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_scrollback_rows(restored) == boxrig_terminal_scrollback_rows(original));
    assert(boxrig_terminal_scrollback_compression(restored) ==
        BOXRIG_SCROLLBACK_COMPRESSION_DEFLATE_LEVEL_1);
    boxrig_text_range current;
    assert(!boxrig_terminal_rebase_text_range(restored, &selection, &current));
    boxrig_text_snapshot_options options = {BOXRIG_TEXT_SCREEN_PRIMARY, &current, 256, 8};
    boxrig_text_snapshot *selected = NULL;
    assert(!boxrig_terminal_create_text_snapshot(restored, &options, &selected));
    size_t selected_bytes;
    const uint8_t *text = boxrig_text_snapshot_data(selected, &selected_bytes);
    const char expected[] = "e\314\201\347\225\214";
    assert(selected_bytes == sizeof(expected) - 1 && !memcmp(text, expected, selected_bytes));
    boxrig_text_snapshot_destroy(selected);
    assert(boxrig_terminal_viewport_offset(restored) == boxrig_terminal_viewport_offset(original));
    assert(boxrig_terminal_get_cell(restored, 0, 1, &after) == BOXRIG_STATUS_OK);
    assert(after.text_length == before.text_length);
    assert(memcmp(after.text, before.text, before.text_length) == 0);
    assert(after.foreground.kind == before.foreground.kind);
    assert(after.foreground.index == before.foreground.index);
    assert(boxrig_terminal_get_cell(restored, 0, 2, &after) == BOXRIG_STATUS_OK);
    assert(after.column_width == 2);
    const char *suffix = "\r\nDD\033[?1049hALT\033[?1049l";
    assert(boxrig_terminal_feed(original, (const uint8_t *)suffix,
        strlen(suffix), &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == strlen(suffix));
    assert(boxrig_terminal_feed(restored, (const uint8_t *)suffix,
        strlen(suffix), &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == strlen(suffix));
    assert(boxrig_terminal_scrollback_rows(restored) == boxrig_terminal_scrollback_rows(original));
    for (size_t row = 0; row < config.rows; row++) {
        for (size_t column = 0; column < config.columns; column++) {
            assert(boxrig_terminal_get_cell(original, row, column, &before) == BOXRIG_STATUS_OK);
            assert(boxrig_terminal_get_cell(restored, row, column, &after) == BOXRIG_STATUS_OK);
            assert(before.text_length == after.text_length);
            if (before.text_length) assert(memcmp(before.text, after.text, before.text_length) == 0);
            assert(before.column_width == after.column_width);
            assert(before.continuation == after.continuation);
        }
    }
    boxrig_checkpoint_free(checkpoint);
    boxrig_terminal_destroy(restored);
    boxrig_terminal_destroy(original);
    return 0;
}
