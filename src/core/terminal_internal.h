/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERMINAL_INTERNAL_H
#define BOXRIG_TERMINAL_INTERNAL_H

#include "boxrig/terminal.h"
#include "dcs.h"
#include "graphics.h"
#include "parser.h"
#include "unicode.h"
#include "codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct boxrig_cell_style {
    uint32_t attributes;
    boxrig_underline_style underline_style;
    boxrig_color underline_color;
    boxrig_color foreground;
    boxrig_color background;
    bool protected_cell;
} boxrig_cell_style;

typedef struct boxrig_cell {
    char *text;
    size_t text_length;
    uint8_t column_width;
    bool continuation;
    uint32_t hyperlink_handle;
    boxrig_cell_style style;
} boxrig_cell;

typedef struct boxrig_text_fragment_metadata {
    boxrig_text_line_id line_id;
    size_t logical_column_offset;
    uint64_t line_revision;
    uint64_t content_hash_a;
    uint64_t content_hash_b;
    size_t projected_length;
    bool needs_sync;
    bool newly_created;
} boxrig_text_fragment_metadata;

typedef struct boxrig_hyperlink_entry {
    char *uri;
    size_t uri_length;
    char *id;
    size_t id_length;
    bool marked;
} boxrig_hyperlink_entry;

typedef struct boxrig_history_line {
    boxrig_cell *cell_storage;
    bool soft_wrapped;
    boxrig_text_fragment_metadata text;
} boxrig_history_line;

typedef struct boxrig_saved_cursor {
    size_t row;
    size_t column;
    bool pending_wrap;
    bool visible;
    bool auto_wrap;
    bool origin_mode;
    bool line_drawing_character_sets[2];
    unsigned int active_character_set;
    boxrig_cell_style style;
} boxrig_saved_cursor;

typedef struct boxrig_cell_position {
    size_t row;
    size_t column;
} boxrig_cell_position;

typedef struct boxrig_host_item_entry {
    boxrig_host_item item;
    uint8_t *payload_storage;
    size_t payload_storage_length;
    boxrig_notification_view notification;
    boxrig_notification_text_view notification_types[16];
    boxrig_graphics_transport_view graphics_transport;
} boxrig_host_item_entry;

typedef struct boxrig_notification_state boxrig_notification_state;
typedef struct boxrig_text_monitor_state boxrig_text_monitor_state;
typedef struct boxrig_text_recognizer_state boxrig_text_recognizer_state;

typedef struct boxrig_pending_request {
    uint64_t request_id;
    boxrig_host_item_type type;
    bool active;
} boxrig_pending_request;

typedef enum boxrig_output_segment_kind {
    BOXRIG_OUTPUT_SEGMENT_DATA = 0,
    BOXRIG_OUTPUT_SEGMENT_BARRIER
} boxrig_output_segment_kind;

typedef struct boxrig_output_segment {
    struct boxrig_output_segment *next;
    boxrig_output_segment_kind kind;
    uint8_t *bytes;
    size_t length;
    size_t offset;
    size_t reservation;
    uint64_t request_id;
    uint8_t clipboard_selectors[12];
    size_t clipboard_selector_count;
    boxrig_parser_string_terminator terminator;
} boxrig_output_segment;

enum {
    BOXRIG_KITTY_KEYBOARD_STACK_CAPACITY = 16,
    BOXRIG_KITTY_DISAMBIGUATE = 1u << 0,
    BOXRIG_KITTY_REPORT_EVENTS = 1u << 1,
    BOXRIG_KITTY_REPORT_ALTERNATE_KEYS = 1u << 2,
    BOXRIG_KITTY_REPORT_ALL_KEYS = 1u << 3,
    BOXRIG_KITTY_REPORT_ASSOCIATED_TEXT = 1u << 4,
    BOXRIG_KITTY_ALL_FLAGS = (1u << 5) - 1
};

typedef enum boxrig_mouse_tracking_mode {
    BOXRIG_MOUSE_TRACKING_NONE = 0,
    BOXRIG_MOUSE_TRACKING_X10,
    BOXRIG_MOUSE_TRACKING_VT200,
    BOXRIG_MOUSE_TRACKING_BUTTON_EVENT,
    BOXRIG_MOUSE_TRACKING_ANY_EVENT
} boxrig_mouse_tracking_mode;

typedef struct boxrig_kitty_keyboard_mode {
    uint32_t flags;
    uint32_t stack[BOXRIG_KITTY_KEYBOARD_STACK_CAPACITY];
    size_t stack_count;
} boxrig_kitty_keyboard_mode;

typedef struct boxrig_input_state {
    bool application_cursor;
    bool application_keypad;
    bool bracketed_paste;
    bool focus_reporting;
    bool sgr_mouse;
    bool pixel_mouse;
    bool alternate_scroll;
    bool eight_bit_meta;
    boxrig_mouse_tracking_mode mouse_tracking;
    bool paste_active;
    bool paste_bracketed;
    boxrig_kitty_keyboard_mode primary_keyboard;
    boxrig_kitty_keyboard_mode alternate_keyboard;
} boxrig_input_state;

typedef struct boxrig_screen_buffer {
    boxrig_cell *cell_storage;
    bool *line_soft_wrapped;
    boxrig_text_fragment_metadata *text_fragments;
    boxrig_cell_position cursor;
    size_t scroll_region_top;
    size_t scroll_region_bottom;
    size_t horizontal_margin_left;
    size_t horizontal_margin_right;
    bool pending_wrap;
    boxrig_saved_cursor saved_cursor;
} boxrig_screen_buffer;

typedef struct boxrig_screen_reflow_marker {
    void *context;
    size_t source_row;
    size_t source_column;
    size_t logical_offset;
    int64_t anchor_delta;
    int64_t destination_row;
    size_t destination_column;
} boxrig_screen_reflow_marker;

typedef struct boxrig_screen_graphics_sink {
    void (*scroll_will_change)(
        boxrig_terminal *terminal,
        size_t top,
        size_t bottom,
        size_t count,
        bool downward);
    void (*scroll_did_change)(boxrig_terminal *terminal);
    void (*erase_display)(boxrig_terminal *terminal, uint32_t mode);
    void (*viewport_will_change)(boxrig_terminal *terminal);
    void (*viewport_did_change)(boxrig_terminal *terminal);
    void (*buffer_will_switch)(boxrig_terminal *terminal);
    void (*buffer_did_switch)(boxrig_terminal *terminal);
    void (*buffer_reset)(boxrig_terminal *terminal, bool alternate);
} boxrig_screen_graphics_sink;

typedef struct boxrig_screen {
    boxrig_terminal *owner;
    boxrig_screen_graphics_sink graphics_sink;
    size_t rows;
    size_t columns;
    boxrig_screen_buffer primary;
    boxrig_screen_buffer alternate;
    boxrig_screen_buffer *active;
    bool *tab_stops;
    size_t *dirty_start;
    size_t *dirty_end;
    bool cursor_visible;
    bool auto_wrap;
    bool origin_mode;
    bool insert_mode;
    bool newline_mode;
    bool left_right_margin_mode;
    bool reverse_video;
    bool synchronized_output;
    bool synchronized_output_deadline_set;
    uint64_t synchronized_output_deadline_milliseconds;
    bool alternate_active;
    uint64_t active_buffer_revision;
    uint64_t text_epoch;
    uint64_t next_text_line_serial;
    uint64_t text_revision;
    uint64_t text_layout_revision;
    bool text_identity_limit;
    boxrig_cursor_shape cursor_shape;
    bool cursor_blinking;
    boxrig_history_line *history;
    size_t history_capacity;
    size_t history_start;
    size_t history_count;
    size_t viewport_offset;
    size_t viewport_anchor_column;
    boxrig_ambiguous_width ambiguous_width;
    boxrig_cursor_column_mode cursor_column_mode;
    char *last_printed_text;
    size_t last_printed_length;
    uint8_t last_printed_width;
    bool grapheme_active;
    boxrig_screen_buffer *grapheme_buffer;
    size_t grapheme_row;
    size_t grapheme_column;
    boxrig_unicode_cluster_analyzer grapheme_analyzer;
    uint32_t active_hyperlink;
    size_t test_scrollback_allocation_countdown;
    size_t test_text_snapshot_allocation_countdown;
    bool line_drawing_character_sets[2];
    unsigned int active_character_set;
    boxrig_cell_style style;
} boxrig_screen;

struct boxrig_animation_clock {
    boxrig_terminal *terminal;
};

struct boxrig_terminal {
    boxrig_parser parser;
    boxrig_encoding encoding;
    boxrig_codec_decoder decoder;
    boxrig_dcs_state dcs;
    boxrig_kitty_graphics_state graphics;
    boxrig_kitty_graphics_protocol_state graphics_protocol;
    boxrig_screen screen;
    boxrig_input_state input;
    boxrig_rgb palette[256];
    boxrig_rgb dynamic_colors[3];
    uint64_t color_revision;
    boxrig_hyperlink_entry *hyperlinks;
    size_t hyperlink_entry_capacity;
    size_t hyperlink_slot_count;
    size_t hyperlink_capacity;
    size_t hyperlink_length;
    boxrig_host_item_entry *host_items;
    size_t host_item_capacity;
    size_t host_item_head;
    size_t host_item_count;
    size_t host_payload_capacity;
    size_t host_payload_length;
    boxrig_pending_request *pending_requests;
    size_t pending_request_count;
    uint64_t next_request_id;
    uint8_t *osc_buffer;
    size_t osc_buffer_capacity;
    size_t osc_buffer_length;
    boxrig_parser_string_kind active_string_kind;
    size_t clipboard_capacity;
    size_t notification_capacity;
    size_t notification_pending_capacity;
    boxrig_notification_state *notification_state;
    boxrig_newline enter_newline;
    boxrig_newline paste_newline;
    boxrig_receive_newline receive_newline;
    /* One allocation owns the immutable mapping entries and their byte data. */
    boxrig_key_mapping *key_mappings;
    size_t key_mapping_count;
    size_t output_capacity;
    size_t output_usage;
    boxrig_output_segment *output_head;
    boxrig_output_segment *output_tail;
    uint64_t next_text_search_result_id;
    bool text_search_result_limit;
    boxrig_text_monitor_state *text_monitor_state;
    boxrig_text_recognizer_state *text_recognizer_state;
    boxrig_animation_clock *animation_clock;
};

/* Validate and copy construction-time legacy key sequences into the terminal. */
boxrig_status boxrig_input_initialize_key_mappings(
    boxrig_terminal *terminal, const boxrig_terminal_config *config);

boxrig_status boxrig_screen_initialize(
    boxrig_screen *screen,
    size_t rows,
    size_t columns);
boxrig_status boxrig_screen_initialize_configured(
    boxrig_screen *screen,
    size_t rows,
    size_t columns,
    size_t scrollback_capacity,
    boxrig_ambiguous_width ambiguous_width,
    boxrig_cursor_column_mode cursor_column_mode);
void boxrig_screen_destroy(boxrig_screen *screen);
boxrig_status boxrig_screen_resize(
    boxrig_screen *screen,
    size_t rows,
    size_t columns);
boxrig_status boxrig_screen_resize_with_markers(
    boxrig_screen *screen,
    size_t rows,
    size_t columns,
    boxrig_screen_reflow_marker *markers,
    size_t marker_count);
bool boxrig_screen_prepare_viewport_reflow_marker(
    const boxrig_screen *screen,
    boxrig_screen_reflow_marker *marker_out);
void boxrig_screen_apply_viewport_reflow_marker(
    boxrig_screen *screen,
    const boxrig_screen_reflow_marker *marker);
boxrig_status boxrig_screen_set_scrollback_capacity(
    boxrig_screen *screen,
    size_t row_capacity);
void boxrig_screen_text_initialize(boxrig_screen *screen);
void boxrig_screen_text_reset_fragment(
    boxrig_screen *screen,
    boxrig_text_fragment_metadata *fragment);
void boxrig_screen_text_sync(boxrig_screen *screen);
void boxrig_screen_reset(boxrig_screen *screen);
void boxrig_screen_update_synchronized_output_time(
    boxrig_screen *screen,
    uint64_t milliseconds);
void boxrig_screen_soft_reset(boxrig_screen *screen);
void boxrig_screen_insert_rows(
    boxrig_screen *screen,
    size_t top,
    size_t bottom,
    size_t count);
void boxrig_screen_delete_rows(
    boxrig_screen *screen,
    size_t top,
    size_t bottom,
    size_t count);
void boxrig_screen_erase_cells(
    boxrig_screen *screen,
    size_t row,
    size_t start,
    size_t end);
void boxrig_screen_insert_cells(
    boxrig_screen *screen,
    size_t row,
    size_t start,
    size_t end,
    size_t count);
void boxrig_screen_delete_cells(
    boxrig_screen *screen,
    size_t row,
    size_t start,
    size_t end,
    size_t count);
void boxrig_screen_put_byte(boxrig_screen *screen, uint8_t byte);
bool boxrig_screen_translate_byte(
    const boxrig_screen *screen,
    uint8_t byte,
    int32_t *codepoint_out);
boxrig_status boxrig_screen_put_codepoint(
    boxrig_screen *screen,
    int32_t codepoint);
void boxrig_screen_end_text(boxrig_screen *screen);
void boxrig_screen_move_cursor_for_image(
    boxrig_screen *screen,
    size_t rows,
    size_t columns);
const boxrig_cell *boxrig_screen_view_cell(
    const boxrig_screen *screen,
    size_t row,
    size_t column);
/* Performs one display line feed without changing the stored LNM mode. */
void boxrig_screen_linefeed(boxrig_screen *screen, bool carriage_return);
void boxrig_screen_execute(boxrig_screen *screen, uint8_t control);
void boxrig_screen_dispatch_escape(
    boxrig_screen *screen,
    const uint8_t *intermediates,
    size_t intermediate_count,
    uint8_t final_byte);
void boxrig_screen_dispatch_csi(
    boxrig_screen *screen,
    uint8_t private_marker,
    const boxrig_parser_parameter *parameters,
    size_t parameter_count,
    const uint8_t *intermediates,
    size_t intermediate_count,
    uint8_t final_byte);
void boxrig_screen_mark_all(boxrig_screen *screen);
bool boxrig_screen_query_mode(
    const boxrig_screen *screen,
    bool private_mode,
    uint32_t mode,
    bool *enabled_out);

void boxrig_input_reset(boxrig_input_state *input);
void boxrig_input_soft_reset(boxrig_input_state *input);
void boxrig_input_dispatch_escape(
    boxrig_input_state *input,
    const uint8_t *intermediates,
    size_t intermediate_count,
    uint8_t final_byte);
void boxrig_input_set_private_modes(
    boxrig_input_state *input,
    const boxrig_parser_parameter *parameters,
    size_t parameter_count,
    bool enabled);
bool boxrig_input_query_mode(
    const boxrig_input_state *input,
    bool private_mode,
    uint32_t mode,
    bool *enabled_out);
uint32_t boxrig_input_kitty_flags(
    const boxrig_input_state *input,
    bool alternate_screen);
void boxrig_input_set_kitty_flags(
    boxrig_input_state *input,
    bool alternate_screen,
    uint32_t flags,
    uint32_t mode);
void boxrig_input_push_kitty_flags(
    boxrig_input_state *input,
    bool alternate_screen,
    uint32_t flags);
void boxrig_input_pop_kitty_flags(
    boxrig_input_state *input,
    bool alternate_screen,
    uint32_t count);

boxrig_status boxrig_terminal_push_host_item(
    boxrig_terminal *terminal,
    boxrig_host_item_type type,
    uint64_t request_id,
    uint32_t flags,
    const uint8_t *payload,
    size_t payload_length);
boxrig_status boxrig_terminal_set_active_hyperlink(
    boxrig_terminal *terminal,
    const uint8_t *id,
    size_t id_length,
    const uint8_t *uri,
    size_t uri_length);
boxrig_status boxrig_terminal_queue_clipboard_request(
    boxrig_terminal *terminal,
    bool read,
    uint32_t flags,
    const uint8_t *payload,
    size_t payload_length,
    const uint8_t *selectors,
    size_t selector_count,
    boxrig_parser_string_terminator terminator,
    size_t response_reservation);
boxrig_status boxrig_terminal_queue_graphics_request(
    boxrig_terminal *terminal,
    const boxrig_graphics_transport_view *transport,
    size_t response_reservation,
    uint64_t *request_id_out);
boxrig_status boxrig_terminal_resolve_graphics_barrier(
    boxrig_terminal *terminal,
    uint64_t request_id,
    const uint8_t *response,
    size_t response_length);
bool boxrig_terminal_remove_queued_graphics_request(
    boxrig_terminal *terminal,
    uint64_t request_id);
void boxrig_terminal_test_fail_scrollback_allocation(
    boxrig_terminal *terminal,
    size_t allocation_countdown);
void boxrig_terminal_test_fail_text_snapshot_allocation(
    boxrig_terminal *terminal,
    size_t allocation_countdown);
boxrig_status boxrig_text_validate_current_grapheme_range(
    const boxrig_terminal *terminal,
    const boxrig_text_range *range);
boxrig_status boxrig_text_monitor_initialize(
    boxrig_terminal *terminal,
    const boxrig_terminal_config *config);
void boxrig_text_monitor_destroy(boxrig_terminal *terminal);
void boxrig_text_monitor_begin_mutation(boxrig_terminal *terminal);
void boxrig_text_monitor_end_mutation(boxrig_terminal *terminal);
void boxrig_text_monitor_test_set_mutation_serial(
    boxrig_terminal *terminal,
    uint64_t mutation_serial);
boxrig_status boxrig_text_recognizer_initialize(
    boxrig_terminal *terminal,
    const boxrig_terminal_config *config);
void boxrig_text_recognizer_destroy(boxrig_terminal *terminal);
#ifdef BOXRIG_MEASURE_TEXT_WORKLOADS
typedef struct boxrig_text_workload_counters {
    size_t visited_cells;
    size_t visited_bytes;
    size_t monitor_event_queue_high_water;
    size_t monitor_staging_queue_high_water;
    size_t recognizer_event_queue_high_water;
    size_t recognizer_prepare_steps;
    size_t recognizer_unbudgeted_prepare_steps;
    size_t recognizer_cursor_creations;
    size_t recognizer_match_identity_comparisons;
} boxrig_text_workload_counters;

void boxrig_text_workload_counters_reset(void);
void boxrig_text_workload_counters_read(
    boxrig_text_workload_counters *counters_out);
void boxrig_text_workload_observe_recognizer_event_queue(size_t count);
void boxrig_text_workload_observe_recognizer_prepare_step(bool budgeted);
void boxrig_text_workload_observe_recognizer_cursor_creation(void);
void boxrig_text_workload_observe_recognizer_match_identity_comparison(void);
#else
#define boxrig_text_workload_observe_recognizer_event_queue(count) \
    ((void)(count))
#define boxrig_text_workload_observe_recognizer_prepare_step(budgeted) \
    ((void)(budgeted))
#define boxrig_text_workload_observe_recognizer_cursor_creation() ((void)0)
#define boxrig_text_workload_observe_recognizer_match_identity_comparison() \
    ((void)0)
#endif
boxrig_status boxrig_terminal_queue_notification(
    boxrig_terminal *terminal,
    const boxrig_notification_view *notification);
boxrig_status boxrig_terminal_append_output(
    boxrig_terminal *terminal,
    const uint8_t *bytes,
    size_t length);
bool boxrig_terminal_reset_palette_color(
    boxrig_terminal *terminal,
    uint8_t index);
bool boxrig_terminal_reset_dynamic_color(
    boxrig_terminal *terminal,
    boxrig_dynamic_color color);

boxrig_status boxrig_notification_initialize(boxrig_terminal *terminal);
void boxrig_notification_destroy(boxrig_terminal *terminal);
void boxrig_notification_reset(boxrig_terminal *terminal);
boxrig_status boxrig_notification_dispatch(
    boxrig_terminal *terminal,
    const uint8_t *payload,
    size_t payload_length,
    boxrig_parser_string_terminator terminator);

boxrig_parser_actions boxrig_interpreter_actions(boxrig_terminal *terminal);

#endif
