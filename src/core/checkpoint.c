/* boxrig - Under MIT. */
#include "checkpoint_internal.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* The eight-byte signature includes the incompatible wire-format version.
 * A little-endian CRC-32 of the entire payload follows the payload. */
static const uint8_t signature[8] = { 'B', 'O', 'X', 'R', 'I', 'G', 2, 0 };

static boxrig_status capture_config(const boxrig_terminal *t, boxrig_terminal_config *cfg)
{
    if (t->host_item_count || t->pending_request_count || t->graphics.active ||
        !boxrig_notification_checkpoint_ready(t) ||
        !boxrig_monitor_checkpoint_config(t, cfg) ||
        !boxrig_recognizer_checkpoint_config(t, cfg)) return BOXRIG_STATUS_BUSY;

    const boxrig_kitty_graphics_protocol_state *g = &t->graphics_protocol;
    cfg->rows = t->screen.rows;
    cfg->columns = t->screen.columns;
    cfg->scrollback_capacity = t->screen.history_capacity;
    cfg->control_string_capacity = t->osc_buffer_capacity;
    cfg->host_item_capacity = t->host_item_capacity;
    cfg->host_payload_capacity = t->host_payload_capacity;
    cfg->output_capacity = t->output_capacity;
    cfg->hyperlink_capacity = t->hyperlink_capacity;
    cfg->clipboard_capacity = t->clipboard_capacity;
    cfg->notification_capacity = t->notification_capacity;
    cfg->notification_pending_capacity = t->notification_pending_capacity;
    cfg->receive_newline = t->receive_newline;
    cfg->paste_newline = t->paste_newline;
    cfg->graphics_storage_capacity = g->storage_capacity;
    cfg->graphics_decode_capacity = g->decode_capacity;
    cfg->graphics_image_capacity = g->image_capacity;
    cfg->graphics_placement_capacity = g->placement_capacity;
    cfg->graphics_transport_name_capacity = g->transport_name_capacity;
    cfg->graphics_animation_storage_capacity = g->animation_storage_capacity;
    cfg->graphics_frame_capacity = g->frame_capacity;
    cfg->input = t->input_config;
    cfg->ambiguous_width = t->screen.ambiguous_width;
    cfg->emoji_variation_width = t->screen.emoji_variation_width;
    cfg->cursor_column_mode = t->screen.cursor_column_mode;
    return BOXRIG_STATUS_OK;
}

/* Config values for construction precede the body so that all internal
 * callback tables can be installed by the normal terminal constructor. */
static void configuration(boxrig_checkpoint_io *io, boxrig_terminal_config *cfg,
                          boxrig_key_mapping *mappings)
{
    boxrig_cp_size(io, &cfg->rows);
    boxrig_cp_size(io, &cfg->columns);
    boxrig_cp_size(io, &cfg->host_item_capacity);
    boxrig_cp_size(io, &cfg->host_payload_capacity);
    boxrig_cp_size(io, &cfg->output_capacity);
    boxrig_cp_size(io, &cfg->control_string_capacity);
    boxrig_cp_size(io, &cfg->hyperlink_capacity);
    boxrig_cp_size(io, &cfg->clipboard_capacity);
    boxrig_cp_size(io, &cfg->notification_capacity);
    boxrig_cp_size(io, &cfg->notification_pending_capacity);
    boxrig_cp_size(io, &cfg->scrollback_capacity);
    boxrig_cp_size(io, &cfg->text_monitor_capacity);
    boxrig_cp_size(io, &cfg->text_monitor_pattern_capacity);
    boxrig_cp_size(io, &cfg->text_monitor_snapshot_byte_capacity);
    boxrig_cp_size(io, &cfg->text_monitor_snapshot_line_capacity);
    boxrig_cp_size(io, &cfg->text_monitor_staging_capacity);
    boxrig_cp_size(io, &cfg->text_monitor_event_capacity);
    boxrig_cp_size(io, &cfg->text_monitor_change_byte_capacity);
    boxrig_cp_size(io, &cfg->text_monitor_change_line_capacity);
    boxrig_cp_size(io, &cfg->text_recognizer_capacity);
    boxrig_cp_size(io, &cfg->text_recognizer_pattern_capacity);
    boxrig_cp_size(io, &cfg->text_recognizer_event_capacity);
    boxrig_cp_size(io, &cfg->graphics_storage_capacity);
    boxrig_cp_size(io, &cfg->graphics_decode_capacity);
    boxrig_cp_size(io, &cfg->graphics_image_capacity);
    boxrig_cp_size(io, &cfg->graphics_placement_capacity);
    boxrig_cp_size(io, &cfg->graphics_transport_name_capacity);
    boxrig_cp_size(io, &cfg->graphics_animation_storage_capacity);
    boxrig_cp_size(io, &cfg->graphics_frame_capacity);
    boxrig_cp_enum(io, &cfg->ambiguous_width);
    boxrig_cp_enum(io, &cfg->emoji_variation_width);
    boxrig_cp_enum(io, &cfg->cursor_column_mode);
    boxrig_cp_enum(io, &cfg->receive_newline);
    boxrig_cp_enum(io, &cfg->paste_newline);
    boxrig_cp_enum(io, &cfg->input.enter_newline);
    boxrig_cp_enum(io, &cfg->input.backspace_binding);
    boxrig_cp_enum(io, &cfg->input.delete_binding);
    boxrig_cp_size(io, &cfg->input.key_mapping_count);
    if (cfg->input.key_mapping_count > BOXRIG_KEY_MAPPING_COUNT_MAX) {
        boxrig_cp_fail(io);
        return;
    }
    if (io->reading) cfg->input.key_mappings = mappings;
    for (size_t i = 0; i < cfg->input.key_mapping_count; ++i) {
        boxrig_key_mapping entry = cfg->input.key_mappings[i];
        boxrig_cp_enum(io, &entry.key);
        boxrig_cp_u32(io, &entry.modifiers);
        boxrig_cp_size(io, &entry.length);
        if (entry.length > BOXRIG_KEY_MAPPING_BYTES_MAX || entry.length > io->limit - io->offset) {
            if (io->reading) boxrig_cp_fail(io);
            else io->status = BOXRIG_STATUS_LIMIT_EXCEEDED;
            return;
        }
        if (io->reading) {
            entry.bytes = entry.length ? io->input + io->offset : NULL;
            mappings[i] = entry;
            io->offset += entry.length;
        } else boxrig_cp_bytes(io, (void *)entry.bytes, entry.length);
    }
}

/* A tiny damaged blob must not provoke an enormous speculative allocation.
 * Lazy graphics/storage limits may exceed the checkpoint size. Only bounds
 * that eagerly allocate in the constructor are checked here. */
static bool allocation_sizes_valid(const boxrig_terminal_config *cfg, size_t maximum_bytes)
{
    size_t budget = maximum_bytes < 16 * 1024 * 1024 ? 16 * 1024 * 1024 : maximum_bytes;
    if (budget > (size_t)1024 * 1024 * 1024) budget = (size_t)1024 * 1024 * 1024;
    return cfg->columns && cfg->rows && cfg->rows <= budget / cfg->columns / (2 * sizeof(boxrig_cell)) &&
           cfg->scrollback_capacity <= budget / sizeof(boxrig_history_line) &&
           cfg->host_item_capacity <= budget / sizeof(boxrig_host_item_entry) &&
           cfg->notification_pending_capacity <= budget / 1024 &&
           cfg->control_string_capacity <= budget &&
           cfg->text_monitor_change_byte_capacity <= budget &&
           cfg->text_monitor_change_line_capacity <= budget / 64 &&
           cfg->text_monitor_capacity <= budget / 1024 &&
           cfg->text_monitor_event_capacity <= budget / sizeof(boxrig_text_monitor_event) &&
           cfg->text_recognizer_capacity <= budget / 1024 &&
           cfg->text_recognizer_event_capacity <= budget / sizeof(boxrig_text_recognizer_event);
}

static void fixed_buffer(boxrig_checkpoint_io *io, uint8_t *bytes, size_t *used, size_t capacity)
{
    boxrig_cp_size(io, used);
    if (*used > capacity) boxrig_cp_fail(io);
    boxrig_cp_bytes(io, bytes, *used);
}

static void parser_state(boxrig_checkpoint_io *io, boxrig_terminal *t)
{
    boxrig_parser *p = &t->parser;
    boxrig_cp_enum(io, &p->state);
    boxrig_cp_size(io, &p->parameter_count);
    if (p->state > BOXRIG_PARSER_STRING_ESCAPE || p->parameter_count > BOXRIG_PARSER_MAX_PARAMETERS) {
        boxrig_cp_fail(io);
        return;
    }
    for (size_t i = 0; i < p->parameter_count; ++i) {
        boxrig_cp_u32(io, &p->parameters[i].value);
        boxrig_cp_bool(io, &p->parameters[i].has_value);
        boxrig_cp_bool(io, &p->parameters[i].subparameter);
    }
    fixed_buffer(io, p->intermediates, &p->intermediate_count, sizeof(p->intermediates));
    boxrig_cp_u8(io, &p->private_marker);
    boxrig_cp_enum(io, &p->string_kind);
    boxrig_cp_size(io, &p->string_length);
    boxrig_cp_size(io, &p->dcs_length);
    boxrig_cp_bool(io, &p->string_escape_delivered);
    boxrig_cp_bool(io, &p->string_discarding);
    boxrig_cp_bool(io, &p->dcs_escape_delivered);
    boxrig_cp_bool(io, &p->ground_print_flushed);
    boxrig_cp_bool(io, &p->last_input_was_cr);
    boxrig_cp_enum(io, &t->active_string_kind);
    if (p->string_kind > BOXRIG_PARSER_STRING_SOS || t->active_string_kind > BOXRIG_PARSER_STRING_SOS ||
        p->string_length > p->string_capacity || p->dcs_length > p->string_capacity) boxrig_cp_fail(io);
    fixed_buffer(io, t->osc_buffer, &t->osc_buffer_length, t->osc_buffer_capacity);
    fixed_buffer(io, t->dcs.request, &t->dcs.request_length, sizeof(t->dcs.request));
    fixed_buffer(io, t->dcs.response, &t->dcs.response_length, sizeof(t->dcs.response));
    boxrig_cp_bool(io, &t->dcs.request_overflow);
    boxrig_cp_bool(io, &t->dcs.response_ready);
    boxrig_cp_bool(io, &t->dcs.active);
    boxrig_unicode_decoder *u = &t->decoder;
    boxrig_cp_u8(io, &u->length);
    boxrig_cp_u8(io, &u->expected_length);
    if ((u->length && (u->length >= u->expected_length || u->expected_length > 4)) ||
        (!u->length && u->expected_length)) boxrig_cp_fail(io);
    boxrig_cp_bytes(io, u->bytes, u->length);
}

static void keyboard(boxrig_checkpoint_io *io, boxrig_kitty_keyboard_mode *k)
{
    boxrig_cp_u32(io, &k->flags);
    boxrig_cp_size(io, &k->stack_count);
    if (k->stack_count > BOXRIG_KITTY_KEYBOARD_STACK_CAPACITY) {
        boxrig_cp_fail(io);
        return;
    }
    for (size_t i = 0; i < k->stack_count; ++i) boxrig_cp_u32(io, &k->stack[i]);
}

static void input_state(boxrig_checkpoint_io *io, boxrig_terminal *t)
{
    boxrig_input_state *input = &t->input;
    boxrig_cp_bool(io, &input->application_cursor);
    boxrig_cp_bool(io, &input->application_keypad);
    boxrig_cp_bool(io, &input->bracketed_paste);
    boxrig_cp_bool(io, &input->focus_reporting);
    boxrig_cp_bool(io, &input->sgr_mouse);
    boxrig_cp_bool(io, &input->pixel_mouse);
    boxrig_cp_bool(io, &input->alternate_scroll);
    boxrig_cp_bool(io, &input->eight_bit_meta);
    boxrig_cp_bool(io, &input->paste_active);
    boxrig_cp_bool(io, &input->paste_bracketed);
    boxrig_cp_enum(io, &input->mouse_tracking);
    if (input->mouse_tracking > BOXRIG_MOUSE_TRACKING_ANY_EVENT) boxrig_cp_fail(io);
    keyboard(io, &input->primary_keyboard);
    keyboard(io, &input->alternate_keyboard);
    boxrig_cp_bool(io, &t->tty_erase.available);
    boxrig_cp_u8(io, &t->tty_erase.byte);
}

static void link_string(boxrig_checkpoint_io *io, char **ptr, size_t *length, size_t *budget)
{
    boxrig_cp_size(io, length);
    if (io->status != BOXRIG_STATUS_OK || !*length) return;
    if (*length > *budget || *length == SIZE_MAX) {
        boxrig_cp_fail(io);
        return;
    }
    if (io->reading) {
        if (*length > io->limit - io->offset) {
            boxrig_cp_fail(io);
            return;
        }
        *ptr = malloc(*length + 1);
        if (!*ptr) {
            io->status = BOXRIG_STATUS_OUT_OF_MEMORY;
            return;
        }
        (*ptr)[*length] = '\0';
    }
    boxrig_cp_bytes(io, *ptr, *length);
    *budget -= *length;
}

static void hyperlinks(boxrig_checkpoint_io *io, boxrig_terminal *t)
{
    size_t slots = t->hyperlink_slot_count;
    boxrig_cp_size(io, &slots);
    if (io->status != BOXRIG_STATUS_OK) return;
    if (slots > t->hyperlink_capacity || slots > UINT32_MAX ||
        (io->reading && slots > (io->limit - io->offset) / 16)) {
        boxrig_cp_fail(io);
        return;
    }
    if (io->reading && slots) {
        t->hyperlinks = calloc(slots, sizeof(*t->hyperlinks));
        if (!t->hyperlinks) {
            io->status = BOXRIG_STATUS_OUT_OF_MEMORY;
            return;
        }
        t->hyperlink_slot_count = t->hyperlink_entry_capacity = slots;
    }
    size_t budget = t->hyperlink_capacity;
    for (size_t i = 0; i < slots && !io->status; ++i) {
        boxrig_hyperlink_entry *entry = &t->hyperlinks[i];
        link_string(io, &entry->uri, &entry->uri_length, &budget);
        link_string(io, &entry->id, &entry->id_length, &budget);
        if (!entry->uri_length && entry->id_length) boxrig_cp_fail(io);
    }
    if (io->reading) t->hyperlink_length = t->hyperlink_capacity - budget;
}

static void output_queue(boxrig_checkpoint_io *io, boxrig_terminal *t)
{
    size_t length = t->output_usage;
    boxrig_cp_size(io, &length);
    if (io->status != BOXRIG_STATUS_OK) return;
    if (io->reading) {
        if (length > io->limit - io->offset || length > t->output_capacity) {
            boxrig_cp_fail(io);
            return;
        }
        io->status = boxrig_terminal_append_output(t, io->input + io->offset, length);
        io->offset += length;
    } else {
        for (boxrig_output_segment *segment = t->output_head; segment; segment = segment->next) {
            if (segment->kind != BOXRIG_OUTPUT_SEGMENT_DATA) {
                io->status = BOXRIG_STATUS_BUSY;
                return;
            }
            boxrig_cp_bytes(io, segment->bytes + segment->offset, segment->length - segment->offset);
        }
    }
}

static void color(boxrig_checkpoint_io *io, boxrig_rgb *rgb)
{
    boxrig_cp_u8(io, &rgb->red);
    boxrig_cp_u8(io, &rgb->green);
    boxrig_cp_u8(io, &rgb->blue);
}

static void body(boxrig_checkpoint_io *io, boxrig_terminal *terminal)
{
    parser_state(io, terminal);
    input_state(io, terminal);
    for (size_t i = 0; i < 256; ++i) color(io, &terminal->palette[i]);
    for (size_t i = 0; i < 3; ++i) color(io, &terminal->dynamic_colors[i]);
    boxrig_cp_u64(io, &terminal->color_revision);
    boxrig_cp_u64(io, &terminal->next_request_id);
    boxrig_cp_u64(io, &terminal->next_text_search_result_id);
    boxrig_cp_bool(io, &terminal->text_search_result_limit);
    output_queue(io, terminal);
    hyperlinks(io, terminal);
    if (!io->status) boxrig_checkpoint_screen(io, terminal);
    if (!io->status) boxrig_checkpoint_graphics(io, terminal);
}

boxrig_status boxrig_terminal_export_checkpoint(const boxrig_terminal *terminal,
    size_t maximum_bytes, uint8_t **data_out, size_t *length_out)
{
    if (data_out) *data_out = NULL;
    if (length_out) *length_out = 0;
    if (!terminal || !data_out || !length_out) return BOXRIG_STATUS_INVALID_ARGUMENT;
    boxrig_terminal_config cfg = {0};
    boxrig_checkpoint_io io = {.limit = maximum_bytes};
    io.status = capture_config(terminal, &cfg);
    if (io.status) return io.status;
    if (!allocation_sizes_valid(&cfg, maximum_bytes)) return BOXRIG_STATUS_LIMIT_EXCEEDED;
    boxrig_cp_bytes(&io, (void *)signature, sizeof(signature));
    configuration(&io, &cfg, NULL);
    if (!io.status) body(&io, (boxrig_terminal *)terminal);
    if (!io.status) {
        uint32_t checksum = (uint32_t)crc32_z(0, io.output, io.offset);
        boxrig_cp_u32(&io, &checksum);
    }
    if (io.status) free(io.output);
    else {
        *data_out = io.output;
        *length_out = io.offset;
    }
    return io.status;
}

boxrig_status boxrig_terminal_import_checkpoint(const uint8_t *data, size_t length,
    size_t maximum_bytes, boxrig_terminal **out)
{
    if (!out) return BOXRIG_STATUS_INVALID_ARGUMENT;
    *out = NULL;
    if (length > maximum_bytes) return BOXRIG_STATUS_LIMIT_EXCEEDED;
    if (!data || length < sizeof(signature) + 4 || memcmp(data, signature, sizeof(signature)))
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    boxrig_checkpoint_io io = {.reading = true, .input = data, .offset = length - 4, .limit = length};
    uint32_t checksum = 0;
    boxrig_cp_u32(&io, &checksum);
    if (checksum != (uint32_t)crc32_z(0, data, length - 4)) return BOXRIG_STATUS_INVALID_ARGUMENT;
    io.offset = sizeof(signature);
    io.limit = length - 4;
    boxrig_terminal_config cfg = {0};
    boxrig_key_mapping mappings[BOXRIG_KEY_MAPPING_COUNT_MAX] = {{0}};
    configuration(&io, &cfg, mappings);
    if (io.status) return io.status;
    /* Allow the default terminal's eager workspaces even with a small wire
     * limit. Cap a single construction allocation independently of wire CRC. */
    if (!allocation_sizes_valid(&cfg, maximum_bytes)) return BOXRIG_STATUS_LIMIT_EXCEEDED;
    boxrig_terminal *terminal = NULL;
    io.status = boxrig_terminal_create(&cfg, &terminal);
    if (!io.status) body(&io, terminal);
    if (io.offset != io.limit) boxrig_cp_fail(&io);
    if (io.status) boxrig_terminal_destroy(terminal);
    else *out = terminal;
    return io.status;
}

void boxrig_checkpoint_free(uint8_t *data)
{
    free(data);
}
