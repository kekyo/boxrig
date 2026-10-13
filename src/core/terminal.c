/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "terminal_internal.h"

#include <stdlib.h>
#include <string.h>

static boxrig_rgb initial_palette_color(uint8_t index)
{
    static const uint8_t ansi[16][3] = {
        {0x00, 0x00, 0x00},
        {0xcd, 0x31, 0x31},
        {0x0d, 0xbc, 0x79},
        {0xe5, 0xe5, 0x10},
        {0x24, 0x72, 0xc8},
        {0xbc, 0x3f, 0xbc},
        {0x11, 0xa8, 0xcd},
        {0xe5, 0xe5, 0xe5},
        {0x66, 0x66, 0x66},
        {0xf1, 0x4c, 0x4c},
        {0x23, 0xd1, 0x8b},
        {0xf5, 0xf5, 0x43},
        {0x3b, 0x8e, 0xea},
        {0xd6, 0x70, 0xd6},
        {0x29, 0xb8, 0xdb},
        {0xff, 0xff, 0xff},
    };
    static const uint8_t cube[6] = {0, 95, 135, 175, 215, 255};
    boxrig_rgb color;

    if (index < 16) {
        color.red = ansi[index][0];
        color.green = ansi[index][1];
        color.blue = ansi[index][2];
    } else if (index < 232) {
        unsigned int value = (unsigned int)index - 16u;

        color.red = cube[value / 36u];
        color.green = cube[(value / 6u) % 6u];
        color.blue = cube[value % 6u];
    } else {
        uint8_t level = (uint8_t)(8u + ((unsigned int)index - 232u) * 10u);

        color.red = level;
        color.green = level;
        color.blue = level;
    }
    return color;
}

static boxrig_rgb initial_dynamic_color(boxrig_dynamic_color color)
{
    boxrig_rgb value;

    if (color == BOXRIG_DYNAMIC_COLOR_BACKGROUND) {
        value.red = 0x10;
        value.green = 0x10;
        value.blue = 0x10;
    } else {
        value.red = 0xd0;
        value.green = 0xd0;
        value.blue = 0xd0;
    }
    return value;
}

static bool rgb_equal(boxrig_rgb left, boxrig_rgb right)
{
    return left.red == right.red && left.green == right.green &&
           left.blue == right.blue;
}

static void initialize_colors(boxrig_terminal *terminal)
{
    size_t index;

    for (index = 0; index < 256; index++) {
        terminal->palette[index] = initial_palette_color((uint8_t)index);
    }
    terminal->dynamic_colors[BOXRIG_DYNAMIC_COLOR_FOREGROUND] =
        initial_dynamic_color(BOXRIG_DYNAMIC_COLOR_FOREGROUND);
    terminal->dynamic_colors[BOXRIG_DYNAMIC_COLOR_BACKGROUND] =
        initial_dynamic_color(BOXRIG_DYNAMIC_COLOR_BACKGROUND);
    terminal->dynamic_colors[BOXRIG_DYNAMIC_COLOR_CURSOR] =
        initial_dynamic_color(BOXRIG_DYNAMIC_COLOR_CURSOR);
    terminal->color_revision = 1;
}

bool boxrig_terminal_reset_palette_color(
    boxrig_terminal *terminal,
    uint8_t index)
{
    boxrig_rgb initial = initial_palette_color(index);
    bool changed = !rgb_equal(terminal->palette[index], initial);

    terminal->palette[index] = initial;
    return changed;
}

bool boxrig_terminal_reset_dynamic_color(
    boxrig_terminal *terminal,
    boxrig_dynamic_color color)
{
    boxrig_rgb initial = initial_dynamic_color(color);
    bool changed = !rgb_equal(terminal->dynamic_colors[color], initial);

    terminal->dynamic_colors[color] = initial;
    return changed;
}

static bool graphics_config_valid(const boxrig_terminal_config *config)
{
    bool storage_zero = config->graphics_storage_capacity == 0;
    bool decode_zero = config->graphics_decode_capacity == 0;
    bool image_zero = config->graphics_image_capacity == 0;
    bool placement_zero = config->graphics_placement_capacity == 0;
    bool animation_storage_zero =
        config->graphics_animation_storage_capacity == 0;
    bool frame_zero = config->graphics_frame_capacity == 0;

    if (storage_zero && decode_zero && image_zero && placement_zero) {
        return config->graphics_transport_name_capacity == 0 &&
               animation_storage_zero && frame_zero;
    }
    if (storage_zero || decode_zero || image_zero || placement_zero) {
        return false;
    }
    if (animation_storage_zero != frame_zero) {
        return false;
    }
    return config->graphics_image_capacity <=
               SIZE_MAX / sizeof(boxrig_image_view) &&
           config->graphics_placement_capacity <=
               SIZE_MAX / sizeof(boxrig_image_placement_view) &&
           config->graphics_frame_capacity <= SIZE_MAX / sizeof(void *);
}

boxrig_terminal_config boxrig_terminal_config_default(void)
{
    boxrig_terminal_config config;

    config.rows = 24;
    config.columns = 80;
    config.host_item_capacity = 64;
    config.host_payload_capacity = 1024 * 1024;
    config.output_capacity = 64 * 1024;
    config.control_string_capacity = 1024 * 1024;
    config.hyperlink_capacity = 1024 * 1024;
    config.clipboard_capacity = 32 * 1024;
    config.notification_capacity = 32 * 1024;
    config.notification_pending_capacity = 16;
    config.scrollback_capacity = 10000;
    config.text_monitor_capacity = 32;
    config.text_monitor_pattern_capacity = 256u * 1024u;
    config.text_monitor_snapshot_byte_capacity = 8u * 1024u * 1024u;
    config.text_monitor_snapshot_line_capacity = 262144;
    config.text_monitor_staging_capacity = 1024;
    config.text_monitor_event_capacity = 4096;
    config.text_monitor_change_byte_capacity = 1024u * 1024u;
    config.text_monitor_change_line_capacity = 4096;
    config.text_recognizer_capacity = 32u;
    config.text_recognizer_pattern_capacity = 256u * 1024u;
    config.text_recognizer_event_capacity = 4096u;
    config.graphics_storage_capacity = 128u * 1024u * 1024u;
    config.graphics_decode_capacity = 192u * 1024u * 1024u;
    config.graphics_image_capacity = 4096;
    config.graphics_placement_capacity = 8192;
    config.graphics_transport_name_capacity = 4u * 1024u;
    config.graphics_animation_storage_capacity = 640u * 1024u * 1024u;
    config.graphics_frame_capacity = 16384;
    config.ambiguous_width = BOXRIG_AMBIGUOUS_WIDTH_NARROW;
    config.cursor_column_mode = BOXRIG_CURSOR_COLUMN_MODE_TERMINAL;
    config.enter_newline = BOXRIG_NEWLINE_TERMINAL;
    config.paste_newline = BOXRIG_NEWLINE_LF;
    config.receive_newline = BOXRIG_RECEIVE_NEWLINE_TERMINAL;
    config.key_mappings = NULL;
    config.key_mapping_count = 0;
    config.encoding = BOXRIG_ENCODING_UTF8;
    return config;
}

boxrig_status boxrig_terminal_create(
    const boxrig_terminal_config *config,
    boxrig_terminal **terminal_out)
{
    boxrig_terminal *terminal;
    boxrig_status status;
    boxrig_parser_actions actions;

    if (config == NULL || terminal_out == NULL || config->rows == 0 ||
        config->columns == 0 || config->host_item_capacity == 0 ||
        config->host_payload_capacity == 0 ||
        config->output_capacity == 0 || config->control_string_capacity == 0 ||
        config->hyperlink_capacity == 0 ||
        ((config->notification_capacity == 0) !=
         (config->notification_pending_capacity == 0)) ||
        (config->notification_capacity != 0 &&
         config->notification_capacity >
             SIZE_MAX / config->notification_pending_capacity) ||
        config->text_recognizer_capacity == 0u ||
        config->text_recognizer_pattern_capacity == 0u ||
        config->text_recognizer_event_capacity == 0u ||
        config->text_recognizer_capacity >
            SIZE_MAX / sizeof(boxrig_text_recognizer_state *) ||
        !graphics_config_valid(config) ||
        (unsigned int)config->encoding > (unsigned int)BOXRIG_ENCODING_SHIFT_JIS ||
        (unsigned int)config->enter_newline > BOXRIG_NEWLINE_TERMINAL ||
        (unsigned int)config->paste_newline > BOXRIG_NEWLINE_CRLF ||
        (unsigned int)config->receive_newline > BOXRIG_RECEIVE_NEWLINE_CRLF ||
        (config->ambiguous_width != BOXRIG_AMBIGUOUS_WIDTH_NARROW &&
         config->ambiguous_width != BOXRIG_AMBIGUOUS_WIDTH_WIDE) ||
        (config->cursor_column_mode != BOXRIG_CURSOR_COLUMN_MODE_TERMINAL &&
         config->cursor_column_mode != BOXRIG_CURSOR_COLUMN_MODE_UTF16)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *terminal_out = NULL;
    terminal = calloc(1, sizeof(*terminal));
    if (terminal == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    status = boxrig_input_initialize_key_mappings(terminal, config);
    if (status != BOXRIG_STATUS_OK) {
        boxrig_terminal_destroy(terminal);
        return status;
    }
    terminal->host_items = calloc(
        config->host_item_capacity,
        sizeof(*terminal->host_items));
    terminal->pending_requests = calloc(
        config->host_item_capacity,
        sizeof(*terminal->pending_requests));
    terminal->osc_buffer = malloc(config->control_string_capacity);
    if (terminal->host_items == NULL || terminal->pending_requests == NULL ||
        terminal->osc_buffer == NULL) {
        boxrig_terminal_destroy(terminal);
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    terminal->host_item_capacity = config->host_item_capacity;
    terminal->host_payload_capacity = config->host_payload_capacity;
    terminal->osc_buffer_capacity = config->control_string_capacity;
    terminal->hyperlink_capacity = config->hyperlink_capacity;
    terminal->clipboard_capacity = config->clipboard_capacity;
    terminal->notification_capacity = config->notification_capacity;
    terminal->notification_pending_capacity =
        config->notification_pending_capacity;
    terminal->output_capacity = config->output_capacity;
    terminal->enter_newline = config->enter_newline;
    terminal->paste_newline = config->paste_newline;
    terminal->receive_newline = config->receive_newline;
    terminal->encoding = config->encoding;
    terminal->next_request_id = 1;
    terminal->next_text_search_result_id = 1;
    initialize_colors(terminal);
    boxrig_dcs_reset(&terminal->dcs);
    boxrig_kitty_graphics_initialize(&terminal->graphics);
    boxrig_kitty_graphics_protocol_initialize(terminal, config);
    boxrig_input_reset(&terminal->input);
    status = boxrig_notification_initialize(terminal);
    if (status != BOXRIG_STATUS_OK) {
        boxrig_terminal_destroy(terminal);
        return status;
    }
    status = boxrig_screen_initialize_configured(
        &terminal->screen,
        config->rows,
        config->columns,
        config->scrollback_capacity,
        config->ambiguous_width,
        config->cursor_column_mode);
    if (status != BOXRIG_STATUS_OK) {
        boxrig_terminal_destroy(terminal);
        return status;
    }
    terminal->screen.owner = terminal;
    terminal->screen.graphics_sink.scroll_will_change =
        boxrig_kitty_graphics_screen_scroll_will_change;
    terminal->screen.graphics_sink.scroll_did_change =
        boxrig_kitty_graphics_screen_scroll_did_change;
    terminal->screen.graphics_sink.erase_display =
        boxrig_kitty_graphics_screen_erase_display;
    terminal->screen.graphics_sink.viewport_will_change =
        boxrig_kitty_graphics_viewport_will_change;
    terminal->screen.graphics_sink.viewport_did_change =
        boxrig_kitty_graphics_viewport_did_change;
    terminal->screen.graphics_sink.buffer_will_switch =
        boxrig_kitty_graphics_screen_buffer_will_switch;
    terminal->screen.graphics_sink.buffer_did_switch =
        boxrig_kitty_graphics_screen_buffer_did_switch;
    terminal->screen.graphics_sink.buffer_reset =
        boxrig_kitty_graphics_screen_buffer_reset;
    status = boxrig_text_monitor_initialize(terminal, config);
    if (status != BOXRIG_STATUS_OK) {
        boxrig_terminal_destroy(terminal);
        return status;
    }
    status = boxrig_text_recognizer_initialize(terminal, config);
    if (status != BOXRIG_STATUS_OK) {
        boxrig_terminal_destroy(terminal);
        return status;
    }
    actions = boxrig_interpreter_actions(terminal);
    memset(&terminal->decoder, 0, sizeof(terminal->decoder));
    boxrig_parser_initialize(
        &terminal->parser,
        &actions,
        config->control_string_capacity);
    *terminal_out = terminal;
    return BOXRIG_STATUS_OK;
}

void boxrig_terminal_destroy(boxrig_terminal *terminal)
{
    boxrig_output_segment *segment;
    size_t index;

    if (terminal == NULL) {
        return;
    }
    if (terminal->animation_clock != NULL) {
        terminal->animation_clock->terminal = NULL;
    }
    free(terminal->key_mappings);
    boxrig_dcs_reset(&terminal->dcs);
    boxrig_kitty_graphics_reset(&terminal->graphics);
    boxrig_kitty_graphics_protocol_destroy(terminal);
    boxrig_notification_destroy(terminal);
    boxrig_text_recognizer_destroy(terminal);
    boxrig_text_monitor_destroy(terminal);
    boxrig_screen_destroy(&terminal->screen);
    for (index = 0; index < terminal->hyperlink_slot_count; index++) {
        free(terminal->hyperlinks[index].id);
        free(terminal->hyperlinks[index].uri);
    }
    for (index = 0; index < terminal->host_item_capacity; index++) {
        free(terminal->host_items[index].payload_storage);
    }
    segment = terminal->output_head;
    while (segment != NULL) {
        boxrig_output_segment *next = segment->next;

        free(segment->bytes);
        free(segment);
        segment = next;
    }
    free(terminal->osc_buffer);
    free(terminal->host_items);
    free(terminal->pending_requests);
    free(terminal->hyperlinks);
    free(terminal);
}

boxrig_status boxrig_terminal_measure_text_cluster(
    const boxrig_terminal *terminal,
    const uint8_t *text,
    size_t text_length,
    boxrig_text_cluster_measurement *measurement_out)
{
    if (terminal == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    return boxrig_unicode_measure_text_cluster(
        text,
        text_length,
        terminal->screen.ambiguous_width == BOXRIG_AMBIGUOUS_WIDTH_WIDE,
        measurement_out);
}

boxrig_status boxrig_terminal_feed(
    boxrig_terminal *terminal,
    const uint8_t *bytes,
    size_t length,
    size_t *consumed_out)
{
    boxrig_status status;

    if (terminal == NULL || consumed_out == NULL ||
        (bytes == NULL && length != 0)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *consumed_out = 0;
    if (length == 0) {
        return BOXRIG_STATUS_OK;
    }
    status = boxrig_parser_feed(
        &terminal->parser,
        bytes,
        length,
        consumed_out);
    boxrig_kitty_graphics_refresh_placeholders(terminal);
    boxrig_screen_text_sync(&terminal->screen);
    return status;
}

boxrig_status boxrig_terminal_resize(
    boxrig_terminal *terminal,
    size_t rows,
    size_t columns)
{
    if (terminal == NULL || rows == 0 || columns == 0) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    boxrig_screen_end_text(&terminal->screen);
    boxrig_text_monitor_begin_mutation(terminal);
    {
        boxrig_status status = boxrig_kitty_graphics_protocol_resize(
            terminal,
            rows,
            columns);

        boxrig_text_monitor_end_mutation(terminal);
        return status;
    }
}

void boxrig_terminal_size(
    const boxrig_terminal *terminal,
    size_t *rows_out,
    size_t *columns_out)
{
    if (terminal == NULL || rows_out == NULL || columns_out == NULL) {
        return;
    }
    *rows_out = terminal->screen.rows;
    *columns_out = terminal->screen.columns;
}

size_t boxrig_terminal_scrollback_rows(const boxrig_terminal *terminal)
{
    return terminal == NULL ? 0 : terminal->screen.history_count;
}

size_t boxrig_terminal_scrollback_capacity(const boxrig_terminal *terminal)
{
    return terminal == NULL ? 0 : terminal->screen.history_capacity;
}

boxrig_status boxrig_terminal_set_scrollback_capacity(
    boxrig_terminal *terminal,
    size_t row_capacity)
{
    if (terminal == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    boxrig_screen_end_text(&terminal->screen);
    boxrig_text_monitor_begin_mutation(terminal);
    {
        boxrig_status status = boxrig_screen_set_scrollback_capacity(
            &terminal->screen,
            row_capacity);

        boxrig_text_monitor_end_mutation(terminal);
        return status;
    }
}

size_t boxrig_terminal_viewport_offset(const boxrig_terminal *terminal)
{
    if (terminal == NULL || terminal->screen.alternate_active) {
        return 0;
    }
    return terminal->screen.viewport_offset;
}

uint64_t boxrig_terminal_active_buffer_revision(
    const boxrig_terminal *terminal)
{
    return terminal == NULL ? 0 : terminal->screen.active_buffer_revision;
}

boxrig_text_screen boxrig_terminal_active_text_screen(
    const boxrig_terminal *terminal)
{
    return terminal != NULL && terminal->screen.alternate_active
               ? BOXRIG_TEXT_SCREEN_ALTERNATE
               : BOXRIG_TEXT_SCREEN_PRIMARY;
}

boxrig_status boxrig_terminal_set_viewport_offset(
    boxrig_terminal *terminal,
    size_t row_offset)
{
    if (terminal == NULL || terminal->screen.alternate_active ||
        row_offset > terminal->screen.history_count) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (terminal->screen.viewport_offset != row_offset) {
        boxrig_kitty_graphics_viewport_will_change(terminal);
        terminal->screen.viewport_offset = row_offset;
        terminal->screen.viewport_anchor_column = 0;
        boxrig_kitty_graphics_viewport_did_change(terminal);
        boxrig_screen_mark_all(&terminal->screen);
    }
    return BOXRIG_STATUS_OK;
}

void boxrig_terminal_test_fail_scrollback_allocation(
    boxrig_terminal *terminal,
    size_t allocation_countdown)
{
    if (terminal != NULL) {
        terminal->screen.test_scrollback_allocation_countdown =
            allocation_countdown;
    }
}

void boxrig_terminal_test_fail_text_snapshot_allocation(
    boxrig_terminal *terminal,
    size_t allocation_countdown)
{
    if (terminal != NULL) {
        terminal->screen.test_text_snapshot_allocation_countdown =
            allocation_countdown;
    }
}

boxrig_status boxrig_terminal_get_cell(
    const boxrig_terminal *terminal,
    size_t row,
    size_t column,
    boxrig_cell_view *cell_out)
{
    const boxrig_cell *cell;

    if (terminal == NULL || cell_out == NULL || row >= terminal->screen.rows ||
        column >= terminal->screen.columns) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    cell = boxrig_screen_view_cell(&terminal->screen, row, column);
    if (terminal->graphics_protocol.enabled && !cell->continuation &&
        cell->text_length >= 4 && cell->text != NULL &&
        memcmp(cell->text, "\xf4\x8e\xbb\xae", 4) == 0) {
        cell_out->text = " ";
        cell_out->text_length = 1;
    } else if (cell->text_length == 0 && !cell->continuation) {
        cell_out->text = " ";
        cell_out->text_length = 1;
    } else {
        cell_out->text = cell->text != NULL ? cell->text : "";
        cell_out->text_length = cell->text_length;
    }
    cell_out->column_width = cell->column_width;
    cell_out->continuation = cell->continuation;
    cell_out->hyperlink_handle = 0;
    cell_out->hyperlink_uri = NULL;
    cell_out->hyperlink_uri_length = 0;
    cell_out->hyperlink_id = NULL;
    cell_out->hyperlink_id_length = 0;
    if (cell->hyperlink_handle != 0 &&
        cell->hyperlink_handle <= terminal->hyperlink_slot_count) {
        const boxrig_hyperlink_entry *entry =
            &terminal->hyperlinks[cell->hyperlink_handle - 1];

        if (entry->uri != NULL) {
            cell_out->hyperlink_handle = cell->hyperlink_handle;
            cell_out->hyperlink_uri = entry->uri;
            cell_out->hyperlink_uri_length = entry->uri_length;
            cell_out->hyperlink_id = entry->id;
            cell_out->hyperlink_id_length = entry->id_length;
        }
    }
    cell_out->attributes = cell->style.attributes;
    if (terminal->screen.reverse_video) {
        cell_out->attributes ^= BOXRIG_CELL_INVERSE;
    }
    cell_out->underline_style = cell->style.underline_style;
    cell_out->underline_color = cell->style.underline_color;
    cell_out->protected_cell = cell->style.protected_cell;
    cell_out->foreground = cell->style.foreground;
    cell_out->background = cell->style.background;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_resolve_viewport_cell(
    const boxrig_terminal *terminal,
    size_t row,
    size_t storage_column,
    boxrig_viewport_cell_resolution *resolution_out)
{
    boxrig_viewport_cell_resolution resolution;
    boxrig_text_screen text_screen;
    size_t fragment_count;
    size_t viewport_offset;
    size_t viewport_start;
    boxrig_status status;

    if (terminal == NULL || resolution_out == NULL ||
        row >= terminal->screen.rows ||
        storage_column >= terminal->screen.columns) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    text_screen = boxrig_terminal_active_text_screen(terminal);
    fragment_count =
        boxrig_terminal_text_fragment_count(terminal, text_screen);
    viewport_offset = text_screen == BOXRIG_TEXT_SCREEN_PRIMARY
                          ? terminal->screen.viewport_offset
                          : 0u;
    if (fragment_count < terminal->screen.rows ||
        viewport_offset > fragment_count - terminal->screen.rows) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    viewport_start =
        fragment_count - terminal->screen.rows - viewport_offset;
    if (row > SIZE_MAX - viewport_start) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    resolution.fragment_index = viewport_start + row;
    resolution.canonical_storage_column = storage_column;
    status = boxrig_terminal_get_cell(
        terminal,
        row,
        resolution.canonical_storage_column,
        &resolution.cell);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    while (resolution.cell.continuation) {
        if (resolution.canonical_storage_column == 0u) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        resolution.canonical_storage_column--;
        status = boxrig_terminal_get_cell(
            terminal,
            row,
            resolution.canonical_storage_column,
            &resolution.cell);
        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
    }
    resolution.column_span = resolution.cell.column_width == 0u
                                 ? 1u
                                 : resolution.cell.column_width;
    if (resolution.column_span >
        terminal->screen.columns - resolution.canonical_storage_column) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    resolution.blank =
        boxrig_screen_view_cell(
            &terminal->screen,
            row,
            resolution.canonical_storage_column)->text_length == 0u;
    status = boxrig_terminal_get_text_fragment(
        terminal,
        text_screen,
        resolution.fragment_index,
        &resolution.fragment);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    status = boxrig_terminal_get_text_position(
        terminal,
        text_screen,
        resolution.fragment_index,
        resolution.canonical_storage_column,
        &resolution.range.start);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    status = boxrig_terminal_get_text_position(
        terminal,
        text_screen,
        resolution.fragment_index,
        resolution.canonical_storage_column + resolution.column_span,
        &resolution.range.end);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    *resolution_out = resolution;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_get_palette_color(
    const boxrig_terminal *terminal,
    uint8_t index,
    boxrig_rgb *color_out)
{
    if (terminal == NULL || color_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *color_out = terminal->palette[index];
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_get_dynamic_color(
    const boxrig_terminal *terminal,
    boxrig_dynamic_color color,
    boxrig_rgb *color_out)
{
    if (terminal == NULL || color_out == NULL ||
        color < BOXRIG_DYNAMIC_COLOR_FOREGROUND ||
        color > BOXRIG_DYNAMIC_COLOR_CURSOR) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *color_out = terminal->dynamic_colors[color];
    return BOXRIG_STATUS_OK;
}

uint64_t boxrig_terminal_color_revision(const boxrig_terminal *terminal)
{
    return terminal == NULL ? 0 : terminal->color_revision;
}

void boxrig_terminal_get_cursor(
    const boxrig_terminal *terminal,
    boxrig_cursor_view *cursor_out)
{
    if (terminal == NULL || cursor_out == NULL) {
        return;
    }
    cursor_out->row = terminal->screen.active->cursor.row;
    cursor_out->column = terminal->screen.active->cursor.column;
    cursor_out->visible = terminal->screen.cursor_visible &&
                          (terminal->screen.alternate_active ||
                           terminal->screen.viewport_offset == 0);
    cursor_out->shape = terminal->screen.cursor_shape;
    cursor_out->blinking = terminal->screen.cursor_blinking;
}

bool boxrig_terminal_synchronized_output(
    const boxrig_terminal *terminal)
{
    return terminal != NULL && terminal->screen.synchronized_output;
}

bool boxrig_terminal_take_dirty(
    boxrig_terminal *terminal,
    boxrig_dirty *dirty_out)
{
    size_t row;

    if (terminal == NULL || dirty_out == NULL) {
        return false;
    }
    for (row = 0; row < terminal->screen.rows; row++) {
        if (terminal->screen.dirty_start[row] <
            terminal->screen.dirty_end[row]) {
            dirty_out->row = row;
            dirty_out->column = terminal->screen.dirty_start[row];
            dirty_out->column_count = terminal->screen.dirty_end[row] -
                                       terminal->screen.dirty_start[row];
            terminal->screen.dirty_start[row] = terminal->screen.columns;
            terminal->screen.dirty_end[row] = 0;
            return true;
        }
    }
    return false;
}

void boxrig_terminal_invalidate(boxrig_terminal *terminal)
{
    if (terminal != NULL) {
        boxrig_screen_mark_all(&terminal->screen);
    }
}

boxrig_status boxrig_terminal_push_host_item(
    boxrig_terminal *terminal,
    boxrig_host_item_type type,
    uint64_t request_id,
    uint32_t flags,
    const uint8_t *payload,
    size_t payload_length)
{
    boxrig_host_item_entry *entry;
    uint8_t *payload_copy = NULL;
    size_t tail;

    if (payload_length > terminal->host_payload_capacity) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (terminal->host_item_count + terminal->pending_request_count >=
            terminal->host_item_capacity ||
        payload_length >
            terminal->host_payload_capacity - terminal->host_payload_length) {
        return BOXRIG_STATUS_WOULD_BLOCK;
    }
    if (payload_length != 0) {
        payload_copy = malloc(payload_length);
        if (payload_copy == NULL) {
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
        memcpy(payload_copy, payload, payload_length);
    }
    tail = (terminal->host_item_head + terminal->host_item_count) %
           terminal->host_item_capacity;
    entry = &terminal->host_items[tail];
    entry->payload_storage = payload_copy;
    entry->payload_storage_length = payload_length;
    entry->item.type = type;
    entry->item.request_id = request_id;
    entry->item.flags = flags;
    entry->item.payload = payload_copy;
    entry->item.payload_length = payload_length;
    entry->item.notification = NULL;
    entry->item.graphics_transport = NULL;
    terminal->host_item_count++;
    terminal->host_payload_length += payload_length;
    return BOXRIG_STATUS_OK;
}

static void mark_hyperlink_handle(
    boxrig_terminal *terminal,
    uint32_t handle)
{
    if (handle != 0 && handle <= terminal->hyperlink_slot_count &&
        terminal->hyperlinks[handle - 1].uri != NULL) {
        terminal->hyperlinks[handle - 1].marked = true;
    }
}

static void mark_cell_hyperlinks(
    boxrig_terminal *terminal,
    const boxrig_cell *cells,
    size_t count)
{
    size_t index;

    if (cells == NULL) {
        return;
    }
    for (index = 0; index < count; index++) {
        mark_hyperlink_handle(terminal, cells[index].hyperlink_handle);
    }
}

static void collect_hyperlinks(boxrig_terminal *terminal)
{
    size_t cell_count = terminal->screen.rows * terminal->screen.columns;
    size_t index;

    for (index = 0; index < terminal->hyperlink_slot_count; index++) {
        terminal->hyperlinks[index].marked = false;
    }
    mark_hyperlink_handle(terminal, terminal->screen.active_hyperlink);
    mark_cell_hyperlinks(
        terminal,
        terminal->screen.primary.cell_storage,
        cell_count);
    mark_cell_hyperlinks(
        terminal,
        terminal->screen.alternate.cell_storage,
        cell_count);
    for (index = 0; index < terminal->screen.history_capacity; index++) {
        mark_cell_hyperlinks(
            terminal,
            terminal->screen.history[index].cell_storage,
            terminal->screen.columns);
    }
    for (index = 0; index < terminal->hyperlink_slot_count; index++) {
        boxrig_hyperlink_entry *entry = &terminal->hyperlinks[index];

        if (entry->uri == NULL) {
            continue;
        }
        if (entry->marked) {
            entry->marked = false;
            continue;
        }
        terminal->hyperlink_length -= entry->uri_length + entry->id_length;
        free(entry->id);
        free(entry->uri);
        memset(entry, 0, sizeof(*entry));
    }
}

static bool hyperlink_matches(
    const boxrig_hyperlink_entry *entry,
    const uint8_t *id,
    size_t id_length,
    const uint8_t *uri,
    size_t uri_length)
{
    return entry->uri != NULL && entry->uri_length == uri_length &&
           entry->id_length == id_length &&
           memcmp(entry->uri, uri, uri_length) == 0 &&
           (id_length == 0 || memcmp(entry->id, id, id_length) == 0);
}

static boxrig_status ensure_hyperlink_slot(
    boxrig_terminal *terminal,
    size_t *slot_out)
{
    boxrig_hyperlink_entry *replacement;
    size_t maximum_slots = terminal->hyperlink_capacity < UINT32_MAX
                               ? terminal->hyperlink_capacity
                               : UINT32_MAX;
    size_t capacity;
    size_t index;

    if (maximum_slots > SIZE_MAX / sizeof(*terminal->hyperlinks)) {
        maximum_slots = SIZE_MAX / sizeof(*terminal->hyperlinks);
    }

    for (index = 0; index < terminal->hyperlink_slot_count; index++) {
        if (terminal->hyperlinks[index].uri == NULL) {
            *slot_out = index;
            return BOXRIG_STATUS_OK;
        }
    }
    if (terminal->hyperlink_slot_count < terminal->hyperlink_entry_capacity) {
        *slot_out = terminal->hyperlink_slot_count;
        terminal->hyperlink_slot_count++;
        return BOXRIG_STATUS_OK;
    }
    if (terminal->hyperlink_entry_capacity >= maximum_slots) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    capacity = terminal->hyperlink_entry_capacity == 0
                   ? 16
                   : terminal->hyperlink_entry_capacity * 2;
    if (capacity < terminal->hyperlink_entry_capacity ||
        capacity > maximum_slots) {
        capacity = maximum_slots;
    }
    replacement = realloc(
        terminal->hyperlinks,
        capacity * sizeof(*terminal->hyperlinks));
    if (replacement == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    memset(
        replacement + terminal->hyperlink_entry_capacity,
        0,
        (capacity - terminal->hyperlink_entry_capacity) *
            sizeof(*replacement));
    terminal->hyperlinks = replacement;
    terminal->hyperlink_entry_capacity = capacity;
    *slot_out = terminal->hyperlink_slot_count;
    terminal->hyperlink_slot_count++;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_set_active_hyperlink(
    boxrig_terminal *terminal,
    const uint8_t *id,
    size_t id_length,
    const uint8_t *uri,
    size_t uri_length)
{
    size_t required;
    size_t index;
    size_t slot;
    char *uri_copy;
    char *id_copy = NULL;
    boxrig_status status;

    if (uri_length == 0) {
        terminal->screen.active_hyperlink = 0;
        return BOXRIG_STATUS_OK;
    }
    if (id_length > SIZE_MAX - uri_length) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    required = id_length + uri_length;
    if (required > terminal->hyperlink_capacity) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    for (index = 0; index < terminal->hyperlink_slot_count; index++) {
        if (hyperlink_matches(
                &terminal->hyperlinks[index],
                id,
                id_length,
                uri,
                uri_length)) {
            terminal->screen.active_hyperlink = (uint32_t)(index + 1);
            return BOXRIG_STATUS_OK;
        }
    }
    if (required > terminal->hyperlink_capacity - terminal->hyperlink_length) {
        collect_hyperlinks(terminal);
    }
    if (required > terminal->hyperlink_capacity - terminal->hyperlink_length) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    status = ensure_hyperlink_slot(terminal, &slot);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    uri_copy = malloc(uri_length);
    if (uri_copy == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    if (id_length != 0) {
        id_copy = malloc(id_length);
        if (id_copy == NULL) {
            free(uri_copy);
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
    }
    memcpy(uri_copy, uri, uri_length);
    if (id_length != 0) {
        memcpy(id_copy, id, id_length);
    }
    terminal->hyperlinks[slot].uri = uri_copy;
    terminal->hyperlinks[slot].uri_length = uri_length;
    terminal->hyperlinks[slot].id = id_copy;
    terminal->hyperlinks[slot].id_length = id_length;
    terminal->hyperlink_length += required;
    terminal->screen.active_hyperlink = (uint32_t)(slot + 1);
    return BOXRIG_STATUS_OK;
}

static bool request_id_is_active(
    const boxrig_terminal *terminal,
    uint64_t request_id)
{
    size_t index;

    for (index = 0; index < terminal->host_item_count; index++) {
        size_t slot = (terminal->host_item_head + index) %
                      terminal->host_item_capacity;

        if (terminal->host_items[slot].item.request_id == request_id) {
            return true;
        }
    }
    for (index = 0; index < terminal->host_item_capacity; index++) {
        if (terminal->pending_requests[index].active &&
            terminal->pending_requests[index].request_id == request_id) {
            return true;
        }
    }
    return false;
}

static uint64_t allocate_request_id(boxrig_terminal *terminal)
{
    size_t attempt;

    for (attempt = 0; attempt <= terminal->host_item_capacity; attempt++) {
        uint64_t request_id = terminal->next_request_id;

        terminal->next_request_id = request_id == UINT64_MAX
                                        ? 1
                                        : request_id + 1;
        if (request_id != 0 &&
            !request_id_is_active(terminal, request_id)) {
            return request_id;
        }
    }
    return 0;
}

static bool add_notification_length(size_t *total, size_t length)
{
    if (length > SIZE_MAX - *total) {
        return false;
    }
    *total += length;
    return true;
}

static void copy_notification_text(
    uint8_t *storage,
    size_t *offset,
    boxrig_notification_text_view source,
    boxrig_notification_text_view *destination)
{
    destination->bytes = NULL;
    destination->length = source.length;
    if (source.length != 0) {
        memcpy(storage + *offset, source.bytes, source.length);
        destination->bytes = storage + *offset;
        *offset += source.length;
    }
}

boxrig_status boxrig_terminal_queue_notification(
    boxrig_terminal *terminal,
    const boxrig_notification_view *notification)
{
    boxrig_host_item_entry *entry;
    uint8_t *storage;
    uint64_t request_id;
    size_t total = 0;
    size_t offset = 0;
    size_t tail;
    size_t index;

    if (terminal == NULL || notification == NULL ||
        notification->type_count > 16 ||
        (notification->type_count != 0 && notification->types == NULL) ||
        notification->occasion < BOXRIG_NOTIFICATION_ALWAYS ||
        notification->occasion > BOXRIG_NOTIFICATION_INVISIBLE ||
        notification->urgency < BOXRIG_NOTIFICATION_URGENCY_LOW ||
        notification->urgency > BOXRIG_NOTIFICATION_URGENCY_CRITICAL ||
        (notification->identifier.length != 0 &&
         notification->identifier.bytes == NULL) ||
        (notification->title.length != 0 &&
         notification->title.bytes == NULL) ||
        (notification->body.length != 0 &&
         notification->body.bytes == NULL) ||
        (notification->application_name.length != 0 &&
         notification->application_name.bytes == NULL)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (!add_notification_length(
            &total,
            notification->has_identifier
                ? notification->identifier.length
                : 0) ||
        !add_notification_length(&total, notification->title.length) ||
        !add_notification_length(&total, notification->body.length) ||
        !add_notification_length(
            &total,
            notification->application_name.length)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    for (index = 0; index < notification->type_count; index++) {
        if ((notification->types[index].length != 0 &&
             notification->types[index].bytes == NULL) ||
            !add_notification_length(
                &total,
                notification->types[index].length)) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
    }
    if (total > terminal->host_payload_capacity) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (terminal->host_item_count + terminal->pending_request_count >=
            terminal->host_item_capacity ||
        total >
            terminal->host_payload_capacity - terminal->host_payload_length) {
        return BOXRIG_STATUS_WOULD_BLOCK;
    }
    request_id = allocate_request_id(terminal);
    if (request_id == 0) {
        return BOXRIG_STATUS_WOULD_BLOCK;
    }
    storage = total == 0 ? NULL : malloc(total);
    if (total != 0 && storage == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    tail = (terminal->host_item_head + terminal->host_item_count) %
           terminal->host_item_capacity;
    entry = &terminal->host_items[tail];
    entry->payload_storage = storage;
    entry->payload_storage_length = total;
    entry->notification.has_identifier = notification->has_identifier;
    copy_notification_text(
        storage,
        &offset,
        notification->has_identifier
            ? notification->identifier
            : (boxrig_notification_text_view){NULL, 0},
        &entry->notification.identifier);
    copy_notification_text(
        storage,
        &offset,
        notification->title,
        &entry->notification.title);
    copy_notification_text(
        storage,
        &offset,
        notification->body,
        &entry->notification.body);
    copy_notification_text(
        storage,
        &offset,
        notification->application_name,
        &entry->notification.application_name);
    for (index = 0; index < notification->type_count; index++) {
        copy_notification_text(
            storage,
            &offset,
            notification->types[index],
            &entry->notification_types[index]);
    }
    entry->notification.types = notification->type_count == 0
                                    ? NULL
                                    : entry->notification_types;
    entry->notification.type_count = notification->type_count;
    entry->notification.occasion = notification->occasion;
    entry->notification.urgency = notification->urgency;
    entry->item.type = BOXRIG_HOST_ITEM_NOTIFICATION;
    entry->item.request_id = request_id;
    entry->item.flags = 0;
    entry->item.payload = NULL;
    entry->item.payload_length = 0;
    entry->item.notification = &entry->notification;
    entry->item.graphics_transport = NULL;
    terminal->host_item_count++;
    terminal->host_payload_length += total;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_queue_clipboard_request(
    boxrig_terminal *terminal,
    bool read,
    uint32_t flags,
    const uint8_t *payload,
    size_t payload_length,
    const uint8_t *selectors,
    size_t selector_count,
    boxrig_parser_string_terminator terminator,
    size_t response_reservation)
{
    boxrig_output_segment *barrier = NULL;
    uint64_t request_id;
    boxrig_status status;

    if (selector_count >
        sizeof(((boxrig_output_segment *)0)->clipboard_selectors)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (read) {
        if (response_reservation > terminal->output_capacity) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        if (response_reservation >
            terminal->output_capacity - terminal->output_usage) {
            return BOXRIG_STATUS_WOULD_BLOCK;
        }
        barrier = calloc(1, sizeof(*barrier));
        if (barrier == NULL) {
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
        barrier->kind = BOXRIG_OUTPUT_SEGMENT_BARRIER;
        barrier->reservation = response_reservation;
        barrier->clipboard_selector_count = selector_count;
        barrier->terminator = terminator;
        memcpy(barrier->clipboard_selectors, selectors, selector_count);
    }
    request_id = allocate_request_id(terminal);
    if (request_id == 0) {
        free(barrier);
        return BOXRIG_STATUS_WOULD_BLOCK;
    }
    status = boxrig_terminal_push_host_item(
        terminal,
        read ? BOXRIG_HOST_ITEM_CLIPBOARD_READ
             : BOXRIG_HOST_ITEM_CLIPBOARD_WRITE,
        request_id,
        flags,
        payload,
        payload_length);
    if (status != BOXRIG_STATUS_OK) {
        free(barrier);
        return status;
    }
    if (barrier != NULL) {
        barrier->request_id = request_id;
        if (terminal->output_tail == NULL) {
            terminal->output_head = barrier;
        } else {
            terminal->output_tail->next = barrier;
        }
        terminal->output_tail = barrier;
        terminal->output_usage += response_reservation;
    }
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_queue_graphics_request(
    boxrig_terminal *terminal,
    const boxrig_graphics_transport_view *transport,
    size_t response_reservation,
    uint64_t *request_id_out)
{
    boxrig_host_item_entry *entry;
    boxrig_output_segment *barrier = NULL;
    uint8_t *name_copy;
    uint64_t request_id;
    size_t tail;

    if (terminal == NULL || transport == NULL || request_id_out == NULL ||
        transport->name == NULL || transport->name_length == 0 ||
        transport->data_capacity == 0 ||
        transport->medium < BOXRIG_GRAPHICS_TRANSPORT_FILE ||
        transport->medium > BOXRIG_GRAPHICS_TRANSPORT_SHARED_MEMORY ||
        (transport->size != 0 &&
         transport->offset > UINT64_MAX - transport->size)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (transport->name_length > terminal->host_payload_capacity ||
        response_reservation > terminal->output_capacity) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (terminal->host_item_count + terminal->pending_request_count >=
            terminal->host_item_capacity ||
        transport->name_length >
            terminal->host_payload_capacity - terminal->host_payload_length ||
        response_reservation >
            terminal->output_capacity - terminal->output_usage) {
        return BOXRIG_STATUS_WOULD_BLOCK;
    }
    name_copy = malloc(transport->name_length);
    if (name_copy == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    memcpy(name_copy, transport->name, transport->name_length);
    if (response_reservation != 0) {
        barrier = calloc(1, sizeof(*barrier));
        if (barrier != NULL) {
            barrier->bytes = malloc(response_reservation);
        }
        if (barrier == NULL || barrier->bytes == NULL) {
            if (barrier != NULL) {
                free(barrier->bytes);
            }
            free(barrier);
            free(name_copy);
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
        barrier->kind = BOXRIG_OUTPUT_SEGMENT_BARRIER;
        barrier->reservation = response_reservation;
    }
    request_id = allocate_request_id(terminal);
    if (request_id == 0) {
        if (barrier != NULL) {
            free(barrier->bytes);
        }
        free(barrier);
        free(name_copy);
        return BOXRIG_STATUS_WOULD_BLOCK;
    }
    tail = (terminal->host_item_head + terminal->host_item_count) %
           terminal->host_item_capacity;
    entry = &terminal->host_items[tail];
    entry->payload_storage = name_copy;
    entry->payload_storage_length = transport->name_length;
    entry->graphics_transport = *transport;
    entry->graphics_transport.name = name_copy;
    entry->item.type = BOXRIG_HOST_ITEM_GRAPHICS_DATA;
    entry->item.request_id = request_id;
    entry->item.flags = 0;
    entry->item.payload = NULL;
    entry->item.payload_length = 0;
    entry->item.notification = NULL;
    entry->item.graphics_transport = &entry->graphics_transport;
    terminal->host_item_count++;
    terminal->host_payload_length += transport->name_length;
    if (barrier != NULL) {
        barrier->request_id = request_id;
        if (terminal->output_tail == NULL) {
            terminal->output_head = barrier;
        } else {
            terminal->output_tail->next = barrier;
        }
        terminal->output_tail = barrier;
        terminal->output_usage += response_reservation;
    }
    *request_id_out = request_id;
    return BOXRIG_STATUS_OK;
}

static boxrig_pending_request *find_pending_request(
    boxrig_terminal *terminal,
    uint64_t request_id)
{
    size_t index;

    for (index = 0; index < terminal->host_item_capacity; index++) {
        if (terminal->pending_requests[index].active &&
            terminal->pending_requests[index].request_id == request_id) {
            return &terminal->pending_requests[index];
        }
    }
    return NULL;
}

static boxrig_output_segment *find_output_barrier(
    boxrig_terminal *terminal,
    uint64_t request_id)
{
    boxrig_output_segment *segment;

    for (segment = terminal->output_head;
         segment != NULL;
         segment = segment->next) {
        if (segment->kind == BOXRIG_OUTPUT_SEGMENT_BARRIER &&
            segment->request_id == request_id) {
            return segment;
        }
    }
    return NULL;
}

boxrig_status boxrig_terminal_resolve_graphics_barrier(
    boxrig_terminal *terminal,
    uint64_t request_id,
    const uint8_t *response,
    size_t response_length)
{
    boxrig_output_segment *barrier;
    boxrig_output_segment *previous = NULL;
    boxrig_output_segment *segment;

    if (terminal == NULL || request_id == 0 ||
        (response == NULL && response_length != 0)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    barrier = find_output_barrier(terminal, request_id);
    if (barrier == NULL) {
        return response_length == 0 ? BOXRIG_STATUS_OK
                                    : BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (response_length > barrier->reservation) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    terminal->output_usage -= barrier->reservation;
    if (response_length == 0) {
        for (segment = terminal->output_head;
             segment != barrier;
             segment = segment->next) {
            previous = segment;
        }
        if (previous == NULL) {
            terminal->output_head = barrier->next;
        } else {
            previous->next = barrier->next;
        }
        if (terminal->output_tail == barrier) {
            terminal->output_tail = previous;
        }
        free(barrier->bytes);
        free(barrier);
        return BOXRIG_STATUS_OK;
    }
    memcpy(barrier->bytes, response, response_length);
    barrier->kind = BOXRIG_OUTPUT_SEGMENT_DATA;
    barrier->length = response_length;
    barrier->offset = 0;
    barrier->reservation = 0;
    terminal->output_usage += response_length;
    return BOXRIG_STATUS_OK;
}

static void rebase_host_item_entry(boxrig_host_item_entry *entry)
{
    entry->item.payload = entry->item.payload_length == 0
                              ? NULL
                              : entry->payload_storage;
    if (entry->item.type == BOXRIG_HOST_ITEM_NOTIFICATION) {
        entry->notification.types = entry->notification.type_count == 0
                                        ? NULL
                                        : entry->notification_types;
        entry->item.notification = &entry->notification;
    }
    if (entry->item.type == BOXRIG_HOST_ITEM_GRAPHICS_DATA) {
        entry->graphics_transport.name = entry->payload_storage;
        entry->item.graphics_transport = &entry->graphics_transport;
    }
}

bool boxrig_terminal_remove_queued_graphics_request(
    boxrig_terminal *terminal,
    uint64_t request_id)
{
    size_t logical_index;
    size_t move_index;

    if (terminal == NULL || request_id == 0) {
        return false;
    }
    for (logical_index = 0;
         logical_index < terminal->host_item_count;
         logical_index++) {
        size_t slot = (terminal->host_item_head + logical_index) %
                      terminal->host_item_capacity;
        boxrig_host_item_entry *entry = &terminal->host_items[slot];

        if (entry->item.type == BOXRIG_HOST_ITEM_GRAPHICS_DATA &&
            entry->item.request_id == request_id) {
            terminal->host_payload_length -= entry->payload_storage_length;
            free(entry->payload_storage);
            memset(entry, 0, sizeof(*entry));
            for (move_index = logical_index;
                 move_index + 1 < terminal->host_item_count;
                 move_index++) {
                size_t destination =
                    (terminal->host_item_head + move_index) %
                    terminal->host_item_capacity;
                size_t source =
                    (terminal->host_item_head + move_index + 1) %
                    terminal->host_item_capacity;

                terminal->host_items[destination] =
                    terminal->host_items[source];
                rebase_host_item_entry(&terminal->host_items[destination]);
                memset(
                    &terminal->host_items[source],
                    0,
                    sizeof(terminal->host_items[source]));
            }
            terminal->host_item_count--;
            return true;
        }
    }
    return false;
}

static bool base64_encoded_length(size_t length, size_t *encoded_length_out)
{
    if (length > SIZE_MAX - 2) {
        return false;
    }
    length = (length + 2) / 3;
    if (length > SIZE_MAX / 4) {
        return false;
    }
    *encoded_length_out = length * 4;
    return true;
}

static void encode_base64(
    const uint8_t *input,
    size_t input_length,
    uint8_t *output)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t input_offset = 0;
    size_t output_offset = 0;

    while (input_offset + 3 <= input_length) {
        uint32_t value = ((uint32_t)input[input_offset] << 16) |
                         ((uint32_t)input[input_offset + 1] << 8) |
                         (uint32_t)input[input_offset + 2];

        output[output_offset] = (uint8_t)alphabet[(value >> 18) & 0x3f];
        output[output_offset + 1] =
            (uint8_t)alphabet[(value >> 12) & 0x3f];
        output[output_offset + 2] =
            (uint8_t)alphabet[(value >> 6) & 0x3f];
        output[output_offset + 3] = (uint8_t)alphabet[value & 0x3f];
        input_offset += 3;
        output_offset += 4;
    }
    if (input_offset < input_length) {
        uint32_t value = (uint32_t)input[input_offset] << 16;

        if (input_offset + 1 < input_length) {
            value |= (uint32_t)input[input_offset + 1] << 8;
        }
        output[output_offset] = (uint8_t)alphabet[(value >> 18) & 0x3f];
        output[output_offset + 1] =
            (uint8_t)alphabet[(value >> 12) & 0x3f];
        output[output_offset + 2] = input_offset + 1 < input_length
                                       ? (uint8_t)alphabet[(value >> 6) & 0x3f]
                                       : (uint8_t)'=';
        output[output_offset + 3] = (uint8_t)'=';
    }
}

static boxrig_status resolve_clipboard_barrier(
    boxrig_terminal *terminal,
    boxrig_output_segment *barrier,
    const uint8_t *payload,
    size_t payload_length)
{
    size_t encoded_length;
    size_t terminator_length =
        barrier->terminator == BOXRIG_PARSER_STRING_TERMINATOR_BEL ? 1 : 2;
    size_t response_length;
    uint8_t *response;
    size_t offset = 0;

    if (!base64_encoded_length(payload_length, &encoded_length) ||
        encoded_length > SIZE_MAX - 6 - terminator_length ||
        barrier->clipboard_selector_count >
            SIZE_MAX - 6 - terminator_length - encoded_length) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    response_length = 6 + barrier->clipboard_selector_count +
                      encoded_length + terminator_length;
    if (response_length > barrier->reservation) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    response = malloc(response_length);
    if (response == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    memcpy(response, "\x1b]52;", 5);
    offset = 5;
    memcpy(
        response + offset,
        barrier->clipboard_selectors,
        barrier->clipboard_selector_count);
    offset += barrier->clipboard_selector_count;
    response[offset] = (uint8_t)';';
    offset++;
    encode_base64(payload, payload_length, response + offset);
    offset += encoded_length;
    if (barrier->terminator == BOXRIG_PARSER_STRING_TERMINATOR_BEL) {
        response[offset] = 0x07;
    } else {
        response[offset] = 0x1b;
        response[offset + 1] = (uint8_t)'\\';
    }
    terminal->output_usage -= barrier->reservation;
    terminal->output_usage += response_length;
    barrier->kind = BOXRIG_OUTPUT_SEGMENT_DATA;
    barrier->bytes = response;
    barrier->length = response_length;
    barrier->offset = 0;
    barrier->reservation = 0;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_complete_host_request(
    boxrig_terminal *terminal,
    uint64_t request_id,
    boxrig_host_request_result result,
    const uint8_t *payload,
    size_t payload_length)
{
    boxrig_pending_request *pending;
    boxrig_status status;

    if (terminal == NULL || request_id == 0 ||
        (payload == NULL && payload_length != 0) ||
        result < BOXRIG_HOST_REQUEST_SUCCESS ||
        result > BOXRIG_HOST_REQUEST_UNAVAILABLE) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    pending = find_pending_request(terminal, request_id);
    if (pending == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (pending->type == BOXRIG_HOST_ITEM_CLIPBOARD_WRITE ||
        pending->type == BOXRIG_HOST_ITEM_NOTIFICATION) {
        if (payload_length != 0) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
    } else if (pending->type == BOXRIG_HOST_ITEM_CLIPBOARD_READ) {
        boxrig_output_segment *barrier = find_output_barrier(
            terminal,
            request_id);

        if (barrier == NULL ||
            (result != BOXRIG_HOST_REQUEST_SUCCESS && payload_length != 0)) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        if (result == BOXRIG_HOST_REQUEST_SUCCESS &&
            payload_length > terminal->clipboard_capacity) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        status = resolve_clipboard_barrier(
            terminal,
            barrier,
            result == BOXRIG_HOST_REQUEST_SUCCESS ? payload : NULL,
            result == BOXRIG_HOST_REQUEST_SUCCESS ? payload_length : 0);
        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
    } else if (pending->type == BOXRIG_HOST_ITEM_GRAPHICS_DATA) {
        if (result != BOXRIG_HOST_REQUEST_SUCCESS && payload_length != 0) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        status = boxrig_kitty_graphics_protocol_complete_external(
            terminal,
            request_id,
            result,
            payload,
            payload_length);
        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
    } else {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    memset(pending, 0, sizeof(*pending));
    terminal->pending_request_count--;
    return BOXRIG_STATUS_OK;
}

bool boxrig_terminal_host_item_peek(
    const boxrig_terminal *terminal,
    boxrig_host_item *item_out)
{
    if (terminal == NULL || item_out == NULL || terminal->host_item_count == 0) {
        return false;
    }
    *item_out = terminal->host_items[terminal->host_item_head].item;
    return true;
}

boxrig_status boxrig_terminal_host_item_consume(boxrig_terminal *terminal)
{
    boxrig_host_item_entry *entry;
    size_t pending_index;

    if (terminal == NULL || terminal->host_item_count == 0) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    entry = &terminal->host_items[terminal->host_item_head];
    if (entry->item.request_id != 0) {
        for (pending_index = 0;
             pending_index < terminal->host_item_capacity;
             pending_index++) {
            if (!terminal->pending_requests[pending_index].active) {
                break;
            }
        }
        if (pending_index == terminal->host_item_capacity) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        terminal->pending_requests[pending_index].request_id =
            entry->item.request_id;
        terminal->pending_requests[pending_index].type = entry->item.type;
        terminal->pending_requests[pending_index].active = true;
        terminal->pending_request_count++;
        if (entry->item.type == BOXRIG_HOST_ITEM_GRAPHICS_DATA) {
            boxrig_kitty_graphics_protocol_host_item_consumed(
                terminal,
                entry->item.request_id);
        }
    }
    terminal->host_payload_length -= entry->payload_storage_length;
    free(entry->payload_storage);
    memset(entry, 0, sizeof(*entry));
    terminal->host_item_head =
        (terminal->host_item_head + 1) % terminal->host_item_capacity;
    terminal->host_item_count--;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_append_output(
    boxrig_terminal *terminal,
    const uint8_t *bytes,
    size_t length)
{
    boxrig_output_segment *segment;
    uint8_t *copy;

    if (length == 0) {
        return BOXRIG_STATUS_OK;
    }
    if (length > terminal->output_capacity) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (length > terminal->output_capacity - terminal->output_usage) {
        return BOXRIG_STATUS_WOULD_BLOCK;
    }
    if (terminal->output_tail != NULL &&
        terminal->output_tail->kind == BOXRIG_OUTPUT_SEGMENT_DATA) {
        size_t retained = terminal->output_tail->length -
                          terminal->output_tail->offset;

        copy = malloc(retained + length);
        if (copy == NULL) {
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
        memcpy(
            copy,
            terminal->output_tail->bytes + terminal->output_tail->offset,
            retained);
        memcpy(copy + retained, bytes, length);
        free(terminal->output_tail->bytes);
        terminal->output_tail->bytes = copy;
        terminal->output_tail->length = retained + length;
        terminal->output_tail->offset = 0;
        terminal->output_usage += length;
        return BOXRIG_STATUS_OK;
    }
    segment = calloc(1, sizeof(*segment));
    copy = malloc(length);
    if (segment == NULL || copy == NULL) {
        free(copy);
        free(segment);
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    memcpy(copy, bytes, length);
    segment->kind = BOXRIG_OUTPUT_SEGMENT_DATA;
    segment->bytes = copy;
    segment->length = length;
    if (terminal->output_tail == NULL) {
        terminal->output_head = segment;
    } else {
        terminal->output_tail->next = segment;
    }
    terminal->output_tail = segment;
    terminal->output_usage += length;
    return BOXRIG_STATUS_OK;
}


boxrig_status boxrig_terminal_finish_input(boxrig_terminal *terminal)
{
    if (terminal == NULL) return BOXRIG_STATUS_INVALID_ARGUMENT;
    boxrig_status status = boxrig_parser_finish(&terminal->parser);
    boxrig_kitty_graphics_refresh_placeholders(terminal);
    boxrig_screen_text_sync(&terminal->screen);
    return status;
}

boxrig_status boxrig_terminal_send_bytes(
    boxrig_terminal *terminal, const uint8_t *bytes, size_t length)
{
    if (terminal == NULL || (bytes == NULL && length != 0))
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    return boxrig_terminal_append_output(terminal, bytes, length);
}

boxrig_status boxrig_terminal_send_text(
    boxrig_terminal *terminal,
    const char *text,
    size_t length)
{
    if (terminal == NULL || (text == NULL && length != 0)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    return boxrig_terminal_append_encoded_text(
        terminal,
        (const uint8_t *)text,
        length, BOXRIG_NEWLINE_LF, false, false, false);
}

size_t boxrig_terminal_output_peek(
    const boxrig_terminal *terminal,
    const uint8_t **bytes_out)
{
    if (terminal == NULL || bytes_out == NULL) {
        return 0;
    }
    *bytes_out = NULL;
    if (terminal->output_head == NULL ||
        terminal->output_head->kind == BOXRIG_OUTPUT_SEGMENT_BARRIER) {
        return 0;
    }
    *bytes_out = terminal->output_head->bytes + terminal->output_head->offset;
    return terminal->output_head->length - terminal->output_head->offset;
}

boxrig_status boxrig_terminal_output_consume(
    boxrig_terminal *terminal,
    size_t length)
{
    boxrig_output_segment *segment;
    size_t available;

    if (terminal == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (length == 0) {
        return BOXRIG_STATUS_OK;
    }
    segment = terminal->output_head;
    if (segment == NULL || segment->kind != BOXRIG_OUTPUT_SEGMENT_DATA) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    available = segment->length - segment->offset;
    if (length > available) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    segment->offset += length;
    terminal->output_usage -= length;
    if (segment->offset == segment->length) {
        terminal->output_head = segment->next;
        if (terminal->output_head == NULL) {
            terminal->output_tail = NULL;
        }
        free(segment->bytes);
        free(segment);
    }
    return BOXRIG_STATUS_OK;
}
