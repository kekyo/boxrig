/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_GRAPHICS_H
#define BOXRIG_GRAPHICS_H

#include "boxrig/terminal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    BOXRIG_KITTY_GRAPHICS_HEADER_CAPACITY = 1024,
    BOXRIG_KITTY_GRAPHICS_PAYLOAD_CAPACITY = 4096,
    BOXRIG_KITTY_GRAPHICS_RESPONSE_CAPACITY = 160
};

typedef enum boxrig_kitty_graphics_field {
    BOXRIG_KITTY_GRAPHICS_FIELD_ACTION = 0,
    BOXRIG_KITTY_GRAPHICS_FIELD_TRANSMISSION,
    BOXRIG_KITTY_GRAPHICS_FIELD_COMPRESSION,
    BOXRIG_KITTY_GRAPHICS_FIELD_DELETE_ACTION,
    BOXRIG_KITTY_GRAPHICS_FIELD_QUIET,
    BOXRIG_KITTY_GRAPHICS_FIELD_FORMAT,
    BOXRIG_KITTY_GRAPHICS_FIELD_DATA_WIDTH,
    BOXRIG_KITTY_GRAPHICS_FIELD_DATA_HEIGHT,
    BOXRIG_KITTY_GRAPHICS_FIELD_DATA_SIZE,
    BOXRIG_KITTY_GRAPHICS_FIELD_DATA_OFFSET,
    BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID,
    BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER,
    BOXRIG_KITTY_GRAPHICS_FIELD_PLACEMENT_ID,
    BOXRIG_KITTY_GRAPHICS_FIELD_MORE,
    BOXRIG_KITTY_GRAPHICS_FIELD_USAGE_HINTS,
    BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_X,
    BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_Y,
    BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_WIDTH,
    BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_HEIGHT,
    BOXRIG_KITTY_GRAPHICS_FIELD_CELL_X_OFFSET,
    BOXRIG_KITTY_GRAPHICS_FIELD_CELL_Y_OFFSET,
    BOXRIG_KITTY_GRAPHICS_FIELD_COLUMNS,
    BOXRIG_KITTY_GRAPHICS_FIELD_ROWS,
    BOXRIG_KITTY_GRAPHICS_FIELD_CURSOR_MOVEMENT,
    BOXRIG_KITTY_GRAPHICS_FIELD_VIRTUAL_PLACEMENT,
    BOXRIG_KITTY_GRAPHICS_FIELD_PARENT_IMAGE_ID,
    BOXRIG_KITTY_GRAPHICS_FIELD_PARENT_PLACEMENT_ID,
    BOXRIG_KITTY_GRAPHICS_FIELD_Z_INDEX,
    BOXRIG_KITTY_GRAPHICS_FIELD_RELATIVE_X_OFFSET,
    BOXRIG_KITTY_GRAPHICS_FIELD_RELATIVE_Y_OFFSET,
    BOXRIG_KITTY_GRAPHICS_FIELD_COUNT
} boxrig_kitty_graphics_field;

typedef struct boxrig_kitty_graphics_command {
    /* Presence is retained separately from values so later stages can apply defaults. */
    uint64_t presence;
    uint8_t action;
    uint8_t transmission;
    uint8_t compression;
    uint8_t delete_action;
    uint32_t quiet;
    uint32_t format;
    uint32_t data_width;
    uint32_t data_height;
    uint32_t data_size;
    uint32_t data_offset;
    uint32_t image_id;
    uint32_t image_number;
    uint32_t placement_id;
    uint32_t more;
    uint32_t usage_hints;
    uint32_t source_x;
    uint32_t source_y;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t cell_x_offset;
    uint32_t cell_y_offset;
    uint32_t columns;
    uint32_t rows;
    uint32_t cursor_movement;
    uint32_t virtual_placement;
    uint32_t parent_image_id;
    uint32_t parent_placement_id;
    int32_t z_index;
    int32_t relative_x_offset;
    int32_t relative_y_offset;
} boxrig_kitty_graphics_command;

typedef enum boxrig_kitty_graphics_value_kind {
    BOXRIG_KITTY_GRAPHICS_VALUE_UNKNOWN = 0,
    BOXRIG_KITTY_GRAPHICS_VALUE_CHARACTER,
    BOXRIG_KITTY_GRAPHICS_VALUE_UNSIGNED,
    BOXRIG_KITTY_GRAPHICS_VALUE_SIGNED
} boxrig_kitty_graphics_value_kind;

typedef enum boxrig_kitty_graphics_header_stage {
    BOXRIG_KITTY_GRAPHICS_HEADER_KEY = 0,
    BOXRIG_KITTY_GRAPHICS_HEADER_VALUE
} boxrig_kitty_graphics_header_stage;

typedef struct boxrig_kitty_graphics_header {
    /* Only the current field is retained; the complete header is never stored. */
    boxrig_kitty_graphics_command command;
    size_t length;
    size_t key_length;
    size_t value_length;
    size_t digit_count;
    uint32_t magnitude;
    boxrig_kitty_graphics_field field;
    boxrig_kitty_graphics_value_kind value_kind;
    boxrig_kitty_graphics_header_stage stage;
    uint8_t key;
    uint8_t character_value;
    bool negative;
    bool valid;
    bool finished;
} boxrig_kitty_graphics_header;

typedef struct boxrig_kitty_graphics_consumer {
    void *context;
    boxrig_status (*begin)(
        void *context,
        boxrig_kitty_graphics_command command);
    boxrig_status (*put)(void *context, uint8_t byte);
    boxrig_status (*commit)(void *context);
    void (*abort)(void *context);
} boxrig_kitty_graphics_consumer;

typedef enum boxrig_kitty_graphics_stage {
    BOXRIG_KITTY_GRAPHICS_IDLE = 0,
    BOXRIG_KITTY_GRAPHICS_SELECT,
    BOXRIG_KITTY_GRAPHICS_HEADER,
    BOXRIG_KITTY_GRAPHICS_BEGIN_PAYLOAD,
    BOXRIG_KITTY_GRAPHICS_PAYLOAD,
    BOXRIG_KITTY_GRAPHICS_BEGIN_EMPTY,
    BOXRIG_KITTY_GRAPHICS_COMMIT,
    BOXRIG_KITTY_GRAPHICS_IGNORE
} boxrig_kitty_graphics_stage;

typedef struct boxrig_kitty_graphics_state {
    boxrig_kitty_graphics_header header;
    boxrig_kitty_graphics_command command;
    /* The consumer is copied so no borrowed command or callback table survives. */
    boxrig_kitty_graphics_consumer consumer;
    boxrig_kitty_graphics_stage stage;
    size_t payload_length;
    bool active;
    bool consumer_active;
} boxrig_kitty_graphics_state;

typedef enum boxrig_kitty_graphics_result {
    BOXRIG_KITTY_GRAPHICS_RESULT_OK = 0,
    BOXRIG_KITTY_GRAPHICS_RESULT_INVALID,
    BOXRIG_KITTY_GRAPHICS_RESULT_BAD_ZLIB,
    BOXRIG_KITTY_GRAPHICS_RESULT_BAD_PNG,
    BOXRIG_KITTY_GRAPHICS_RESULT_NOT_FOUND,
    BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE,
    BOXRIG_KITTY_GRAPHICS_RESULT_NO_CELL_GEOMETRY,
    BOXRIG_KITTY_GRAPHICS_RESULT_NO_PARENT,
    BOXRIG_KITTY_GRAPHICS_RESULT_CYCLE,
    BOXRIG_KITTY_GRAPHICS_RESULT_TOO_DEEP,
    BOXRIG_KITTY_GRAPHICS_RESULT_ACCESS_DENIED,
    BOXRIG_KITTY_GRAPHICS_RESULT_IO_ERROR,
    BOXRIG_KITTY_GRAPHICS_RESULT_UNSUPPORTED_ACTION,
    BOXRIG_KITTY_GRAPHICS_RESULT_UNSUPPORTED_TRANSMISSION,
    BOXRIG_KITTY_GRAPHICS_RESULT_UNSUPPORTED_FRAMES
} boxrig_kitty_graphics_result;

typedef struct boxrig_kitty_graphics_upload_state
    boxrig_kitty_graphics_upload_state;
typedef struct boxrig_image_resource_state boxrig_image_resource_state;
typedef struct boxrig_image_placement_state boxrig_image_placement_state;
typedef struct boxrig_placeholder_instance_state
    boxrig_placeholder_instance_state;
typedef struct boxrig_graphics_external_request_state
    boxrig_graphics_external_request_state;

typedef struct boxrig_kitty_graphics_transaction_state {
    boxrig_kitty_graphics_command command;
    boxrig_image_placement_state *prepared_placement;
    boxrig_image_placement_state *replacement_placement;
    boxrig_image_resource_state *animation_resource;
    boxrig_image_placement_state **delete_placements;
    boxrig_image_resource_state **delete_resources;
    uint8_t *prepared_animation_rgba;
    uint8_t response[BOXRIG_KITTY_GRAPHICS_RESPONSE_CAPACITY];
    size_t response_length;
    size_t payload_length;
    size_t delete_placement_count;
    size_t delete_resource_count;
    size_t prepared_animation_frame;
    size_t prepared_current_frame;
    size_t prepared_gap_frame;
    uint8_t *transport_name;
    size_t transport_name_length;
    size_t transport_name_capacity;
    uint8_t transport_quartet[4];
    size_t transport_quartet_length;
    boxrig_kitty_graphics_result result;
    uint32_t more;
    uint32_t prepared_loop_count;
    int32_t prepared_gap_milliseconds;
    uint8_t prepared_playback_state;
    bool active;
    bool response_ready;
    bool uses_upload;
    bool uses_external_transport;
    bool transport_base64_ended;
    bool transport_valid;
    bool prepared;
    bool prepared_animation_transient;
    bool animation_sets_current;
    bool animation_sets_gap;
    bool animation_sets_loops;
    bool animation_sets_playback_state;
} boxrig_kitty_graphics_transaction_state;

struct boxrig_graphics_external_request_state {
    boxrig_graphics_external_request_state *next;
    boxrig_kitty_graphics_command command;
    uint64_t request_id;
    uint64_t input_serial;
    uint64_t creation_serial;
    boxrig_image_handle target_image_handle;
    size_t data_capacity;
    bool consumed;
    bool cancelled;
};

typedef struct boxrig_kitty_graphics_protocol_state {
    boxrig_kitty_graphics_transaction_state transaction;
    boxrig_kitty_graphics_upload_state *upload;
    boxrig_graphics_external_request_state *external_requests;
    boxrig_image_resource_state **images;
    boxrig_image_placement_state **placements;
    boxrig_placeholder_instance_state *placeholder_instances;
    size_t *dirty_start;
    size_t *dirty_end;
    size_t image_count;
    size_t image_slot_capacity;
    size_t placement_count;
    size_t placement_slot_capacity;
    size_t placeholder_cell_capacity;
    size_t dirty_row_count;
    size_t storage_usage;
    size_t animation_storage_usage;
    size_t animation_frame_count;
    size_t storage_capacity;
    size_t decode_capacity;
    size_t image_capacity;
    size_t placement_capacity;
    size_t transport_name_capacity;
    size_t animation_storage_capacity;
    size_t frame_capacity;
    uint64_t monotonic_time_milliseconds;
    uint64_t revision;
    uint64_t next_image_handle;
    uint64_t next_creation_serial;
    uint64_t next_external_serial;
    uint32_t next_image_id;
    uint32_t cell_pixel_width;
    uint32_t cell_pixel_height;
    bool enabled;
    bool screen_mutation_changed;
    bool monotonic_time_set;
} boxrig_kitty_graphics_protocol_state;

void boxrig_kitty_graphics_header_initialize(
    boxrig_kitty_graphics_header *state);
bool boxrig_kitty_graphics_header_put(
    boxrig_kitty_graphics_header *state,
    uint8_t byte);
bool boxrig_kitty_graphics_header_finish(
    boxrig_kitty_graphics_header *state,
    boxrig_kitty_graphics_command *command_out);
bool boxrig_kitty_graphics_command_has(
    const boxrig_kitty_graphics_command *command,
    boxrig_kitty_graphics_field field);
void boxrig_kitty_graphics_initialize(boxrig_kitty_graphics_state *state);
void boxrig_kitty_graphics_set_consumer(
    boxrig_kitty_graphics_state *state,
    const boxrig_kitty_graphics_consumer *consumer);
void boxrig_kitty_graphics_start(boxrig_kitty_graphics_state *state);
boxrig_status boxrig_kitty_graphics_put(
    boxrig_kitty_graphics_state *state,
    uint8_t byte);
boxrig_status boxrig_kitty_graphics_end(
    boxrig_kitty_graphics_state *state,
    bool cancelled);
void boxrig_kitty_graphics_reset(boxrig_kitty_graphics_state *state);
void boxrig_kitty_graphics_protocol_initialize(
    boxrig_terminal *terminal,
    const boxrig_terminal_config *config);
void boxrig_kitty_graphics_protocol_reset(boxrig_terminal *terminal);
void boxrig_kitty_graphics_protocol_destroy(boxrig_terminal *terminal);
void boxrig_kitty_graphics_protocol_host_item_consumed(
    boxrig_terminal *terminal,
    uint64_t request_id);
boxrig_status boxrig_kitty_graphics_protocol_complete_external(
    boxrig_terminal *terminal,
    uint64_t request_id,
    boxrig_host_request_result result,
    const uint8_t *payload,
    size_t payload_length);
void boxrig_kitty_graphics_refresh_placeholders(boxrig_terminal *terminal);
boxrig_status boxrig_kitty_graphics_protocol_resize(
    boxrig_terminal *terminal,
    size_t rows,
    size_t columns);
void boxrig_kitty_graphics_viewport_will_change(boxrig_terminal *terminal);
void boxrig_kitty_graphics_viewport_did_change(boxrig_terminal *terminal);
void boxrig_kitty_graphics_screen_scroll_will_change(
    boxrig_terminal *terminal,
    size_t top,
    size_t bottom,
    size_t count,
    bool downward);
void boxrig_kitty_graphics_screen_scroll_did_change(
    boxrig_terminal *terminal);
void boxrig_kitty_graphics_screen_erase_display(
    boxrig_terminal *terminal,
    uint32_t mode);
void boxrig_kitty_graphics_screen_buffer_will_switch(
    boxrig_terminal *terminal);
void boxrig_kitty_graphics_screen_buffer_did_switch(
    boxrig_terminal *terminal);
void boxrig_kitty_graphics_screen_buffer_reset(
    boxrig_terminal *terminal,
    bool alternate);

#endif
