/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "terminal_internal.h"

#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define FIELD_BIT(field) (UINT64_C(1) << (unsigned int)(field))

enum {
    BOXRIG_GRAPHICS_IMAGE_SIDE_MAX = 1000000,
    BOXRIG_GRAPHICS_PNG_CHUNK_CACHE_MAX = 128,
    BOXRIG_GRAPHICS_DECOMPRESS_BUFFER_SIZE = 4096,
    BOXRIG_GRAPHICS_RELATIVE_DEPTH_MAX = 16
};

static void destroy_placement(boxrig_image_placement_state *placement);
static void increment_revision(boxrig_kitty_graphics_protocol_state *protocol);
static void mark_active_placements(boxrig_terminal *terminal);
static void remove_resource_at(boxrig_terminal *terminal, size_t index);
static void refresh_derived_placements(boxrig_terminal *terminal, bool all_rows);
static bool placement_is_active(
    const boxrig_terminal *terminal,
    const boxrig_image_placement_state *placement);

static bool command_has(
    const boxrig_kitty_graphics_command *command,
    boxrig_kitty_graphics_field field)
{
    return boxrig_kitty_graphics_command_has(command, field);
}

static void apply_defaults(boxrig_kitty_graphics_command *command)
{
    if (!command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_ACTION)) {
        command->action = (uint8_t)'t';
    }
    if (!command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_TRANSMISSION)) {
        command->transmission = (uint8_t)'d';
    }
    if (!command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_DELETE_ACTION)) {
        command->delete_action = (uint8_t)'a';
    }
    if (!command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_FORMAT)) {
        command->format = 32;
    }
}

static bool has_only_fields(
    const boxrig_kitty_graphics_command *command,
    uint64_t allowed)
{
    return (command->presence & ~allowed) == 0;
}

static bool has_valid_common_values(
    const boxrig_kitty_graphics_command *command)
{
    bool compression_valid =
        !command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_COMPRESSION) ||
        command->compression == (uint8_t)'z';
    bool transmission_valid =
        command->transmission == (uint8_t)'d' ||
        command->transmission == (uint8_t)'f' ||
        command->transmission == (uint8_t)'t' ||
        command->transmission == (uint8_t)'s';

    return command->quiet <= 2 && command->more <= 1 &&
           command->cursor_movement <= 1 && command->virtual_placement <= 1 &&
           compression_valid && transmission_valid &&
           (command->format == 24 || command->format == 32 ||
            command->format == 100) &&
           !(command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID) &&
             command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER));
}

static bool has_valid_display_relationships(
    const boxrig_kitty_graphics_command *command)
{
    bool has_parent_image = command_has(
        command,
        BOXRIG_KITTY_GRAPHICS_FIELD_PARENT_IMAGE_ID);
    bool has_parent_placement = command_has(
        command,
        BOXRIG_KITTY_GRAPHICS_FIELD_PARENT_PLACEMENT_ID);
    bool has_relative_x = command_has(
        command,
        BOXRIG_KITTY_GRAPHICS_FIELD_RELATIVE_X_OFFSET);
    bool has_relative_y = command_has(
        command,
        BOXRIG_KITTY_GRAPHICS_FIELD_RELATIVE_Y_OFFSET);

    if (has_parent_image != has_parent_placement) {
        return false;
    }
    if ((has_relative_x || has_relative_y) && !has_parent_image) {
        return false;
    }
    if (has_parent_image &&
        (command->parent_image_id == 0 ||
         command->parent_placement_id == 0 ||
         command->virtual_placement != 0)) {
        return false;
    }
    if (command->virtual_placement != 0 &&
        (!command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_COLUMNS) ||
         !command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_ROWS) ||
         command->columns == 0 || command->rows == 0)) {
        return false;
    }
    return true;
}

static bool has_nonzero_image_identity(
    const boxrig_kitty_graphics_command *command)
{
    return (command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID) &&
            command->image_id != 0) ||
           (command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER) &&
            command->image_number != 0);
}

static bool transmission_uses_host(
    const boxrig_kitty_graphics_command *command)
{
    return command->transmission == (uint8_t)'f' ||
           command->transmission == (uint8_t)'t' ||
           command->transmission == (uint8_t)'s';
}

static bool has_valid_image_data_source(
    const boxrig_kitty_graphics_command *command,
    size_t payload_length)
{
    bool host_transmission = transmission_uses_host(command);

    if (payload_length == 0 || (host_transmission && command->more != 0)) {
        return false;
    }
    if (!host_transmission && command->data_offset != 0) {
        return false;
    }
    if (command->format == 24 || command->format == 32) {
        if (command->data_width == 0 || command->data_height == 0 ||
            (!host_transmission &&
             command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_DATA_SIZE))) {
            return false;
        }
    } else if (!host_transmission &&
               command_has(
                   command,
                   BOXRIG_KITTY_GRAPHICS_FIELD_COMPRESSION)) {
        if (!command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_DATA_SIZE) ||
            command->data_size == 0) {
            return false;
        }
    } else if (!host_transmission &&
               command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_DATA_SIZE)) {
        return false;
    }
    return true;
}

static boxrig_kitty_graphics_result validate_transmission(
    const boxrig_kitty_graphics_command *command,
    size_t payload_length,
    bool display)
{
    uint64_t transmission_fields =
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_ACTION) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_TRANSMISSION) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_COMPRESSION) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_QUIET) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_FORMAT) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_DATA_WIDTH) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_DATA_HEIGHT) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_DATA_SIZE) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_DATA_OFFSET) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_MORE) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_USAGE_HINTS);

    if (display) {
        transmission_fields |=
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_PLACEMENT_ID) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_X) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_Y) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_WIDTH) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_HEIGHT) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_CELL_X_OFFSET) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_CELL_Y_OFFSET) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_COLUMNS) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_ROWS) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_CURSOR_MOVEMENT) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_VIRTUAL_PLACEMENT) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_PARENT_IMAGE_ID) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_PARENT_PLACEMENT_ID) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_Z_INDEX) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_RELATIVE_X_OFFSET) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_RELATIVE_Y_OFFSET);
    }
    if (!has_only_fields(command, transmission_fields) ||
        (display && !has_valid_display_relationships(command))) {
        return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (!has_valid_image_data_source(command, payload_length)) {
        return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    return BOXRIG_KITTY_GRAPHICS_RESULT_OK;
}

static boxrig_kitty_graphics_result validate_query(
    const boxrig_kitty_graphics_command *command,
    size_t payload_length)
{
    uint64_t query_fields =
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_ACTION) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_TRANSMISSION) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_COMPRESSION) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_QUIET) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_FORMAT) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_DATA_WIDTH) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_DATA_HEIGHT) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_DATA_SIZE) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_DATA_OFFSET) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_MORE) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_USAGE_HINTS);
    boxrig_kitty_graphics_result result;

    if (!command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID) ||
        command->image_id == 0 || !has_only_fields(command, query_fields)) {
        return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    result = validate_transmission(command, payload_length, false);
    return result;
}

static bool has_no_delete_coordinates(
    const boxrig_kitty_graphics_command *command)
{
    return !command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_X) &&
           !command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_Y) &&
           !command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_Z_INDEX) &&
           !command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_PLACEMENT_ID);
}

static boxrig_kitty_graphics_result validate_delete(
    const boxrig_kitty_graphics_command *command,
    size_t payload_length)
{
    uint64_t delete_fields =
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_ACTION) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_DELETE_ACTION) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_QUIET) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_PLACEMENT_ID) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_X) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_Y) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_Z_INDEX);
    uint8_t selector = command->delete_action;
    bool has_x = command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_X);
    bool has_y = command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_Y);
    bool has_z = command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_Z_INDEX);
    bool has_placement = command_has(
        command,
        BOXRIG_KITTY_GRAPHICS_FIELD_PLACEMENT_ID);

    if (selector == (uint8_t)'f' || selector == (uint8_t)'F') {
        uint64_t frame_fields =
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_ACTION) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_DELETE_ACTION) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_QUIET) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER) |
            FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_ROWS);

        return payload_length == 0 && has_only_fields(command, frame_fields) &&
                       has_nonzero_image_identity(command)
                   ? BOXRIG_KITTY_GRAPHICS_RESULT_OK
                   : BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (payload_length != 0 || !has_only_fields(command, delete_fields)) {
        return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (selector == (uint8_t)'a' || selector == (uint8_t)'A' ||
        selector == (uint8_t)'c' || selector == (uint8_t)'C') {
        return has_no_delete_coordinates(command)
                   ? BOXRIG_KITTY_GRAPHICS_RESULT_OK
                   : BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (selector == (uint8_t)'i' || selector == (uint8_t)'I') {
        return command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID) &&
                       command->image_id != 0 && !has_x && !has_y && !has_z
                   ? BOXRIG_KITTY_GRAPHICS_RESULT_OK
                   : BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (selector == (uint8_t)'n' || selector == (uint8_t)'N') {
        return command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER) &&
                       !has_x && !has_y && !has_z
                   ? BOXRIG_KITTY_GRAPHICS_RESULT_OK
                   : BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (selector == (uint8_t)'r' || selector == (uint8_t)'R') {
        return has_x && has_y && !has_z && !has_placement &&
                       command->source_x <= command->source_y
                   ? BOXRIG_KITTY_GRAPHICS_RESULT_OK
                   : BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (selector == (uint8_t)'p' || selector == (uint8_t)'P') {
        return has_x && has_y && !has_z && !has_placement &&
                       command->source_x != 0 && command->source_y != 0
                   ? BOXRIG_KITTY_GRAPHICS_RESULT_OK
                   : BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (selector == (uint8_t)'q' || selector == (uint8_t)'Q') {
        return has_x && has_y && has_z && !has_placement &&
                       command->source_x != 0 && command->source_y != 0
                   ? BOXRIG_KITTY_GRAPHICS_RESULT_OK
                   : BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (selector == (uint8_t)'x' || selector == (uint8_t)'X') {
        return has_x && !has_y && !has_z && !has_placement &&
                       command->source_x != 0
                   ? BOXRIG_KITTY_GRAPHICS_RESULT_OK
                   : BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (selector == (uint8_t)'y' || selector == (uint8_t)'Y') {
        return !has_x && has_y && !has_z && !has_placement &&
                       command->source_y != 0
                   ? BOXRIG_KITTY_GRAPHICS_RESULT_OK
                   : BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (selector == (uint8_t)'z' || selector == (uint8_t)'Z') {
        return !has_x && !has_y && has_z && !has_placement
                   ? BOXRIG_KITTY_GRAPHICS_RESULT_OK
                   : BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
}

static boxrig_kitty_graphics_result validate_frame_transfer(
    const boxrig_kitty_graphics_command *command,
    size_t payload_length)
{
    uint64_t fields =
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_ACTION) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_TRANSMISSION) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_COMPRESSION) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_QUIET) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_FORMAT) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_DATA_WIDTH) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_DATA_HEIGHT) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_DATA_SIZE) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_DATA_OFFSET) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_MORE) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_USAGE_HINTS) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_X) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_Y) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_CELL_X_OFFSET) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_CELL_Y_OFFSET) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_COLUMNS) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_ROWS) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_Z_INDEX);
    bool has_base = command_has(
        command,
        BOXRIG_KITTY_GRAPHICS_FIELD_COLUMNS);
    bool has_edit = command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_ROWS);
    bool has_background = command_has(
        command,
        BOXRIG_KITTY_GRAPHICS_FIELD_CELL_Y_OFFSET);
    bool has_composition = command_has(
        command,
        BOXRIG_KITTY_GRAPHICS_FIELD_CELL_X_OFFSET);

    if (!has_only_fields(command, fields) ||
        !has_nonzero_image_identity(command) ||
        !has_valid_image_data_source(command, payload_length) ||
        (has_base && command->columns == 0) ||
        (has_edit && command->rows == 0) ||
        (has_composition && command->cell_x_offset > 1) ||
        (has_base && has_background) ||
        (has_edit && (has_base || has_background))) {
        return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    return BOXRIG_KITTY_GRAPHICS_RESULT_OK;
}

static boxrig_kitty_graphics_result validate_animation_control(
    const boxrig_kitty_graphics_command *command,
    size_t payload_length)
{
    uint64_t fields =
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_ACTION) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_QUIET) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_DATA_WIDTH) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_DATA_HEIGHT) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_COLUMNS) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_ROWS) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_Z_INDEX);
    bool has_state = command_has(
        command,
        BOXRIG_KITTY_GRAPHICS_FIELD_DATA_WIDTH);
    bool has_loops = command_has(
        command,
        BOXRIG_KITTY_GRAPHICS_FIELD_DATA_HEIGHT);
    bool has_current = command_has(
        command,
        BOXRIG_KITTY_GRAPHICS_FIELD_COLUMNS);
    bool has_gap_frame = command_has(
        command,
        BOXRIG_KITTY_GRAPHICS_FIELD_ROWS);
    bool has_gap = command_has(
        command,
        BOXRIG_KITTY_GRAPHICS_FIELD_Z_INDEX);

    if (payload_length != 0 || !has_only_fields(command, fields) ||
        !has_nonzero_image_identity(command) ||
        (!has_state && !has_loops && !has_current && !has_gap_frame &&
         !has_gap) ||
        (has_state &&
         (command->data_width < 1 || command->data_width > 3)) ||
        (has_current && command->columns == 0) ||
        (has_gap_frame != has_gap) ||
        (has_gap_frame &&
         (command->rows == 0 || command->z_index == 0))) {
        return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    return BOXRIG_KITTY_GRAPHICS_RESULT_OK;
}

static boxrig_kitty_graphics_result validate_frame_composition(
    const boxrig_kitty_graphics_command *command,
    size_t payload_length)
{
    uint64_t fields =
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_ACTION) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_QUIET) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_X) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_Y) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_WIDTH) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_HEIGHT) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_CELL_X_OFFSET) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_CELL_Y_OFFSET) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_COLUMNS) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_ROWS) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_CURSOR_MOVEMENT);

    if (payload_length != 0 || !has_only_fields(command, fields) ||
        !has_nonzero_image_identity(command) ||
        !command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_COLUMNS) ||
        !command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_ROWS) ||
        command->columns == 0 || command->rows == 0) {
        return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    return BOXRIG_KITTY_GRAPHICS_RESULT_OK;
}

static boxrig_kitty_graphics_result validate_command(
    const boxrig_kitty_graphics_command *command,
    size_t payload_length)
{
    uint64_t placement_fields =
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_ACTION) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_QUIET) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_PLACEMENT_ID) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_X) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_Y) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_WIDTH) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_HEIGHT) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_CELL_X_OFFSET) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_CELL_Y_OFFSET) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_COLUMNS) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_ROWS) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_CURSOR_MOVEMENT) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_VIRTUAL_PLACEMENT) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_PARENT_IMAGE_ID) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_PARENT_PLACEMENT_ID) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_Z_INDEX) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_RELATIVE_X_OFFSET) |
        FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_RELATIVE_Y_OFFSET);

    if (!has_valid_common_values(command)) {
        return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (command->action == (uint8_t)'f') {
        return validate_frame_transfer(command, payload_length);
    }
    if (command->action == (uint8_t)'a') {
        return validate_animation_control(command, payload_length);
    }
    if (command->action == (uint8_t)'c') {
        return validate_frame_composition(command, payload_length);
    }
    if (command->action == (uint8_t)'t') {
        return validate_transmission(command, payload_length, false);
    }
    if (command->action == (uint8_t)'T') {
        return validate_transmission(command, payload_length, true);
    }
    if (command->action == (uint8_t)'q') {
        return validate_query(command, payload_length);
    }
    if (command->action == (uint8_t)'p') {
        bool has_identity =
            (command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID) &&
             command->image_id != 0) ||
            command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER);

        if (payload_length != 0 || !has_identity ||
            !has_only_fields(command, placement_fields) ||
            !has_valid_display_relationships(command)) {
            return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
        }
        return BOXRIG_KITTY_GRAPHICS_RESULT_OK;
    }
    if (command->action == (uint8_t)'d') {
        return validate_delete(command, payload_length);
    }
    return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
}

static boxrig_kitty_graphics_result validate_protocol_command(
    const boxrig_kitty_graphics_protocol_state *protocol,
    const boxrig_kitty_graphics_command *command,
    size_t payload_length)
{
    boxrig_kitty_graphics_result result = validate_command(
        command,
        payload_length);

    if (result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
        (command->action == (uint8_t)'f' ||
         command->action == (uint8_t)'a' ||
         command->action == (uint8_t)'c') &&
        (protocol->animation_storage_capacity == 0 ||
         protocol->frame_capacity == 0)) {
        return BOXRIG_KITTY_GRAPHICS_RESULT_UNSUPPORTED_FRAMES;
    }
    return result;
}

typedef struct boxrig_animation_frame_state {
    uint8_t *rgba;
    int32_t gap_milliseconds;
    bool transient;
} boxrig_animation_frame_state;

typedef enum boxrig_animation_playback_state {
    BOXRIG_ANIMATION_PLAYBACK_STOPPED = 1,
    BOXRIG_ANIMATION_PLAYBACK_LOADING = 2,
    BOXRIG_ANIMATION_PLAYBACK_LOOPING = 3
} boxrig_animation_playback_state;

struct boxrig_image_resource_state {
    boxrig_image_view view;
    uint8_t *rgba;
    boxrig_animation_frame_state *frames;
    size_t frame_count;
    size_t current_frame;
    /* The cursor can wait on a gapless tail while current_frame remains visible. */
    size_t animation_frame;
    int32_t root_gap_milliseconds;
    uint64_t creation_serial;
    uint64_t last_external_frame_serial;
    uint64_t animation_deadline_milliseconds;
    uint32_t animation_loop_count;
    uint32_t animation_remaining_wraps;
    boxrig_animation_playback_state animation_playback_state;
    bool transient;
    bool root_transient;
    bool has_image_number;
    bool remove_if_orphaned;
    bool animation_deadline_set;
    bool animation_waiting;
};

struct boxrig_image_placement_state {
    boxrig_image_placement_view view;
    boxrig_image_resource_state *resource;
    uint64_t creation_serial;
    /* The logical c/r request survives metric changes; zero is the protocol
     * default and therefore behaves the same as an omitted key. */
    uint32_t requested_columns;
    uint32_t requested_rows;
    int64_t anchor_line;
    int64_t anchor_column;
    struct boxrig_image_placement_state *parent;
    int32_t relative_x_offset;
    int32_t relative_y_offset;
    size_t clip_cell_top;
    size_t clip_cell_bottom;
    size_t clip_cell_left;
    size_t clip_cell_right;
    bool columns_specified;
    bool rows_specified;
    bool move_cursor;
    bool alternate_buffer;
    bool virtual_placement;
    bool relative_placement;
    bool viewport_anchored;
    bool relative_parent_visible;
};

struct boxrig_placeholder_instance_state {
    boxrig_image_placement_view view;
    uint64_t prototype_serial;
    bool matched;
    /* Only the first cell in each row owns this flag. It is independent of
     * renderer dirty ranges, which a caller may leave unconsumed. */
    bool row_refresh_pending;
};

#include "graphics_diacritics.inc"

typedef union boxrig_graphics_quota_header {
    size_t charged_size;
    long double long_double_alignment;
    void *pointer_alignment;
} boxrig_graphics_quota_header;

/* One logical upload survives successful m=1 APC commits. Encoded input is
 * never retained: only decoder state and decoded/decompressed staging carry. */
struct boxrig_kitty_graphics_upload_state {
    boxrig_kitty_graphics_command command;
    boxrig_image_resource_state *prepared_resource;
    boxrig_image_resource_state *replacement_resource;
    boxrig_image_resource_state **eviction_resources;
    boxrig_image_resource_state **frame_eviction_resources;
    boxrig_image_resource_state *frame_resource;
    boxrig_animation_frame_state *prepared_frames;
    uint8_t *prepared_frame_rgba;
    uint8_t *rgba;
    uint8_t *png_data;
    size_t rgba_length;
    size_t rgba_offset;
    size_t png_length;
    size_t png_capacity;
    size_t raw_expected;
    size_t raw_received;
    size_t eviction_count;
    size_t frame_eviction_count;
    size_t prepared_frame_number;
    size_t planned_removed_placement_count;
    size_t decode_capacity;
    size_t decode_usage;
    size_t chunk_encoded_length;
    uint8_t quartet[4];
    size_t quartet_length;
    uint8_t channels[3];
    size_t channel_count;
    z_stream zlib_stream;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    int32_t prepared_frame_gap;
    uint64_t next_image_handle_after_commit;
    uint32_t next_image_id_after_commit;
    boxrig_kitty_graphics_result result;
    bool base64_ended;
    bool compressed;
    bool png_fixed_capacity;
    bool zlib_initialized;
    bool zlib_ended;
    bool quota_exceeded;
    bool generated_image_id;
    bool prepared_frame_is_new;
    bool prepared_frame_transient;
};

typedef struct boxrig_png_decode_context {
    boxrig_kitty_graphics_upload_state *upload;
    png_structp png;
    png_infop info;
    png_bytepp rows;
    size_t rows_size;
    size_t input_offset;
} boxrig_png_decode_context;

static const char *result_message(boxrig_kitty_graphics_result result)
{
    if (result == BOXRIG_KITTY_GRAPHICS_RESULT_OK) {
        return "OK";
    }
    if (result == BOXRIG_KITTY_GRAPHICS_RESULT_INVALID) {
        return "EINVAL:invalid command";
    }
    if (result == BOXRIG_KITTY_GRAPHICS_RESULT_BAD_ZLIB) {
        return "EBADZLIB:invalid stream";
    }
    if (result == BOXRIG_KITTY_GRAPHICS_RESULT_BAD_PNG) {
        return "EBADPNG:invalid image";
    }
    if (result == BOXRIG_KITTY_GRAPHICS_RESULT_NOT_FOUND) {
        return "ENOENT:image not found";
    }
    if (result == BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE) {
        return "ENOSPC:graphics capacity exceeded";
    }
    if (result == BOXRIG_KITTY_GRAPHICS_RESULT_NO_CELL_GEOMETRY) {
        return "ENOTSUP:cell geometry unavailable";
    }
    if (result == BOXRIG_KITTY_GRAPHICS_RESULT_NO_PARENT) {
        return "ENOPARENT:parent placement not found";
    }
    if (result == BOXRIG_KITTY_GRAPHICS_RESULT_CYCLE) {
        return "ECYCLE:relative placement cycle";
    }
    if (result == BOXRIG_KITTY_GRAPHICS_RESULT_TOO_DEEP) {
        return "ETOODEEP:relative placement depth exceeded";
    }
    if (result == BOXRIG_KITTY_GRAPHICS_RESULT_ACCESS_DENIED) {
        return "EACCES:external graphics data denied";
    }
    if (result == BOXRIG_KITTY_GRAPHICS_RESULT_IO_ERROR) {
        return "EIO:external graphics data unavailable";
    }
    if (result == BOXRIG_KITTY_GRAPHICS_RESULT_UNSUPPORTED_ACTION) {
        return "ENOTSUP:unsupported action";
    }
    if (result == BOXRIG_KITTY_GRAPHICS_RESULT_UNSUPPORTED_TRANSMISSION) {
        return "ENOTSUP:unsupported transmission";
    }
    return "ENOTSUP:animation frames unsupported";
}

static bool should_respond(
    const boxrig_kitty_graphics_command *command,
    boxrig_kitty_graphics_result result)
{
    bool has_selector =
        (command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID) &&
         command->image_id != 0) ||
        command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER);

    if (!has_selector || command->quiet == 2) {
        return false;
    }
    return !(command->quiet == 1 && result == BOXRIG_KITTY_GRAPHICS_RESULT_OK);
}

static bool append_formatted(
    uint8_t *bytes,
    size_t capacity,
    size_t *length,
    const char *format,
    unsigned long value)
{
    int written = snprintf(
        (char *)bytes + *length,
        capacity - *length,
        format,
        value);

    if (written < 0 || (size_t)written >= capacity - *length) {
        return false;
    }
    *length += (size_t)written;
    return true;
}

static bool prepare_response(boxrig_kitty_graphics_transaction_state *state)
{
    const boxrig_kitty_graphics_command *command = &state->command;
    const char *message;
    size_t message_length;
    size_t length = 0;

    state->response_ready = true;
    if (!should_respond(command, state->result)) {
        state->response_length = 0;
        return true;
    }
    if (!append_formatted(
            state->response,
            sizeof(state->response),
            &length,
            "\x1b_Gi=%lu",
            (unsigned long)command->image_id)) {
        return false;
    }
    if (command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER) &&
        !append_formatted(
            state->response,
            sizeof(state->response),
            &length,
            ",I=%lu",
            (unsigned long)command->image_number)) {
        return false;
    }
    if (command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_PLACEMENT_ID) &&
        !append_formatted(
            state->response,
            sizeof(state->response),
            &length,
            ",p=%lu",
            (unsigned long)command->placement_id)) {
        return false;
    }
    message = result_message(state->result);
    message_length = strlen(message);
    if (message_length > sizeof(state->response) - length - 3) {
        return false;
    }
    state->response[length] = (uint8_t)';';
    length++;
    memcpy(state->response + length, message, message_length);
    length += message_length;
    state->response[length] = 0x1b;
    state->response[length + 1] = (uint8_t)'\\';
    length += 2;
    state->response_length = length;
    return true;
}

static void clear_transaction(boxrig_kitty_graphics_transaction_state *state)
{
    destroy_placement(state->prepared_placement);
    free(state->delete_resources);
    free(state->delete_placements);
    free(state->prepared_animation_rgba);
    free(state->transport_name);
    memset(state, 0, sizeof(*state));
}

static bool reserve_decode(
    boxrig_kitty_graphics_upload_state *upload,
    size_t size)
{
    if (upload->decode_usage > upload->decode_capacity ||
        size > upload->decode_capacity - upload->decode_usage) {
        upload->quota_exceeded = true;
        return false;
    }
    upload->decode_usage += size;
    return true;
}

static void release_decode(
    boxrig_kitty_graphics_upload_state *upload,
    size_t size)
{
    if (size > upload->decode_usage) {
        upload->decode_usage = 0;
        return;
    }
    upload->decode_usage -= size;
}

static void *allocate_decode_buffer(
    boxrig_kitty_graphics_upload_state *upload,
    size_t size)
{
    void *buffer;

    if (size == 0 || !reserve_decode(upload, size)) {
        return NULL;
    }
    buffer = malloc(size);
    if (buffer == NULL) {
        release_decode(upload, size);
        upload->quota_exceeded = true;
    }
    return buffer;
}

static void free_decode_buffer(
    boxrig_kitty_graphics_upload_state *upload,
    void *buffer,
    size_t size)
{
    if (buffer == NULL) {
        return;
    }
    free(buffer);
    release_decode(upload, size);
}

static void *allocate_quota_block(
    boxrig_kitty_graphics_upload_state *upload,
    size_t size)
{
    boxrig_graphics_quota_header *header;
    size_t charged_size;

    if (size > SIZE_MAX - sizeof(*header)) {
        upload->quota_exceeded = true;
        return NULL;
    }
    charged_size = sizeof(*header) + size;
    if (!reserve_decode(upload, charged_size)) {
        return NULL;
    }
    header = malloc(charged_size);
    if (header == NULL) {
        release_decode(upload, charged_size);
        upload->quota_exceeded = true;
        return NULL;
    }
    header->charged_size = charged_size;
    return header + 1;
}

static void free_quota_block(
    boxrig_kitty_graphics_upload_state *upload,
    void *pointer)
{
    boxrig_graphics_quota_header *header;
    size_t charged_size;

    if (pointer == NULL) {
        return;
    }
    header = (boxrig_graphics_quota_header *)pointer - 1;
    charged_size = header->charged_size;
    free(header);
    release_decode(upload, charged_size);
}

static voidpf zlib_allocate(voidpf opaque, uInt items, uInt size)
{
    boxrig_kitty_graphics_upload_state *upload = opaque;
    size_t item_count = items;
    size_t item_size = size;

    if (item_count != 0 && item_size > SIZE_MAX / item_count) {
        upload->quota_exceeded = true;
        return Z_NULL;
    }
    return allocate_quota_block(upload, item_count * item_size);
}

static void zlib_free(voidpf opaque, voidpf address)
{
    boxrig_kitty_graphics_upload_state *upload = opaque;

    free_quota_block(upload, address);
}

static png_voidp png_allocate(png_structp png, png_alloc_size_t size)
{
    boxrig_kitty_graphics_upload_state *upload = png_get_mem_ptr(png);
    size_t requested = (size_t)size;

    if ((png_alloc_size_t)requested != size) {
        upload->quota_exceeded = true;
        return NULL;
    }
    return allocate_quota_block(upload, requested);
}

static void png_quota_free(png_structp png, png_voidp pointer)
{
    boxrig_kitty_graphics_upload_state *upload = png_get_mem_ptr(png);

    free_quota_block(upload, pointer);
}

static void png_error_callback(png_structp png, png_const_charp message)
{
    (void)message;
    png_longjmp(png, 1);
}

static void png_warning_callback(png_structp png, png_const_charp message)
{
    (void)png;
    (void)message;
}

static void png_read_callback(
    png_structp png,
    png_bytep output,
    png_size_t length)
{
    boxrig_png_decode_context *context = png_get_io_ptr(png);
    size_t requested = (size_t)length;

    if ((png_size_t)requested != length ||
        context->input_offset > context->upload->png_length ||
        requested > context->upload->png_length - context->input_offset) {
        png_error(png, "truncated PNG input");
    }
    memcpy(
        output,
        context->upload->png_data + context->input_offset,
        requested);
    context->input_offset += requested;
}

static void destroy_resource(boxrig_image_resource_state *resource)
{
    size_t index;

    if (resource == NULL) {
        return;
    }
    for (index = 0; index < resource->frame_count; index++) {
        free(resource->frames[index].rgba);
    }
    free(resource->frames);
    free(resource->rgba);
    free(resource);
}

static void destroy_placement(boxrig_image_placement_state *placement)
{
    free(placement);
}

static void destroy_upload(boxrig_kitty_graphics_upload_state *upload)
{
    if (upload == NULL) {
        return;
    }
    destroy_resource(upload->prepared_resource);
    if (upload->zlib_initialized) {
        inflateEnd(&upload->zlib_stream);
        upload->zlib_initialized = false;
    }
    free_decode_buffer(upload, upload->rgba, upload->rgba_length);
    free_decode_buffer(upload, upload->png_data, upload->png_capacity);
    free(upload->prepared_frame_rgba);
    free(upload->prepared_frames);
    free(upload->frame_eviction_resources);
    free(upload->eviction_resources);
    free(upload);
}

static void clear_upload(boxrig_kitty_graphics_protocol_state *protocol)
{
    destroy_upload(protocol->upload);
    protocol->upload = NULL;
}

static bool is_upload_action(const boxrig_kitty_graphics_command *command)
{
    return command->action == (uint8_t)'t' ||
           command->action == (uint8_t)'T' ||
           command->action == (uint8_t)'q' ||
           command->action == (uint8_t)'f';
}

static bool is_continuation_command(
    const boxrig_kitty_graphics_command *command,
    const boxrig_kitty_graphics_upload_state *upload)
{
    uint64_t allowed = FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_MORE) |
                       FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_QUIET);
    bool frame_upload = upload != NULL &&
                        upload->command.action == (uint8_t)'f';

    if (frame_upload) {
        allowed |= FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_ACTION);
        if (!command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_ACTION) ||
            command->action != (uint8_t)'f') {
            return false;
        }
    } else if (command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_ACTION)) {
        return false;
    }

    return has_only_fields(command, allowed) && command->more <= 1 &&
           command->quiet <= 2;
}

static bool multiply_size(size_t left, size_t right, size_t *result_out)
{
    if (left != 0 && right > SIZE_MAX / left) {
        return false;
    }
    *result_out = left * right;
    return true;
}

static boxrig_kitty_graphics_result validate_raw_dimensions(
    const boxrig_kitty_graphics_command *command,
    size_t *raw_length_out,
    size_t *rgba_length_out)
{
    uint32_t bytes_per_pixel = command->format == 24 ? 3 : 4;
    uint32_t raw_stride_u32;
    uint32_t rgba_stride_u32;
    size_t raw_stride;
    size_t rgba_stride;

    if ((command->format != 24 && command->format != 32) ||
        command->data_width == 0 || command->data_height == 0 ||
        command->data_width > BOXRIG_GRAPHICS_IMAGE_SIDE_MAX ||
        command->data_height > BOXRIG_GRAPHICS_IMAGE_SIDE_MAX ||
        command->data_width > UINT32_MAX / bytes_per_pixel ||
        command->data_width > UINT32_MAX / 4 ||
        !multiply_size(
            command->data_width,
            bytes_per_pixel,
            &raw_stride) ||
        !multiply_size(command->data_width, 4, &rgba_stride)) {
        return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    raw_stride_u32 = command->data_width * bytes_per_pixel;
    rgba_stride_u32 = command->data_width * 4;
    if (command->data_height > UINT32_MAX / raw_stride_u32 ||
        command->data_height > UINT32_MAX / rgba_stride_u32) {
        return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (!multiply_size(
            raw_stride,
            command->data_height,
            raw_length_out) ||
        !multiply_size(
            rgba_stride,
            command->data_height,
            rgba_length_out)) {
        return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    return BOXRIG_KITTY_GRAPHICS_RESULT_OK;
}

static boxrig_kitty_graphics_upload_state *create_upload(
    boxrig_kitty_graphics_protocol_state *protocol,
    const boxrig_kitty_graphics_command *command)
{
    boxrig_kitty_graphics_upload_state *upload = calloc(1, sizeof(*upload));
    int zlib_result;

    if (upload == NULL) {
        return NULL;
    }
    upload->command = *command;
    upload->decode_capacity = protocol->decode_capacity;
    upload->compressed = command_has(
        command,
        BOXRIG_KITTY_GRAPHICS_FIELD_COMPRESSION);
    if (command->format == 24 || command->format == 32) {
        upload->result = validate_raw_dimensions(
            command,
            &upload->raw_expected,
            &upload->rgba_length);
        upload->width = command->data_width;
        upload->height = command->data_height;
        upload->stride = command->data_width * 4;
    } else if (command->format == 100) {
        upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_OK;
        if (upload->compressed) {
            upload->png_fixed_capacity = command->data_size != 0;
            if (upload->png_fixed_capacity) {
                upload->png_capacity = command->data_size;
                upload->png_data = allocate_decode_buffer(
                    upload,
                    upload->png_capacity);
                if (upload->png_data == NULL) {
                    upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
                }
            }
        }
    } else {
        upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (upload->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
        (command->format == 24 || command->format == 32)) {
        upload->rgba = allocate_decode_buffer(upload, upload->rgba_length);
        if (upload->rgba == NULL) {
            upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
        }
    }
    if (upload->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
        upload->compressed) {
        upload->zlib_stream.zalloc = zlib_allocate;
        upload->zlib_stream.zfree = zlib_free;
        upload->zlib_stream.opaque = upload;
        zlib_result = inflateInit(&upload->zlib_stream);
        if (zlib_result == Z_OK) {
            upload->zlib_initialized = true;
        } else if (zlib_result == Z_MEM_ERROR || upload->quota_exceeded) {
            upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
        } else {
            upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_BAD_ZLIB;
        }
    }
    return upload;
}

static bool append_raw_byte(
    boxrig_kitty_graphics_upload_state *upload,
    uint8_t byte)
{
    if (upload->raw_received >= upload->raw_expected) {
        return false;
    }
    if (upload->command.format == 32) {
        if (upload->rgba_offset >= upload->rgba_length) {
            return false;
        }
        upload->rgba[upload->rgba_offset] = byte;
        upload->rgba_offset++;
    } else {
        upload->channels[upload->channel_count] = byte;
        upload->channel_count++;
        if (upload->channel_count == 3) {
            if (upload->rgba_offset > upload->rgba_length - 4) {
                return false;
            }
            memcpy(upload->rgba + upload->rgba_offset, upload->channels, 3);
            upload->rgba[upload->rgba_offset + 3] = 255;
            upload->rgba_offset += 4;
            upload->channel_count = 0;
        }
    }
    upload->raw_received++;
    return true;
}

static bool grow_png_buffer(boxrig_kitty_graphics_upload_state *upload)
{
    uint8_t *data;
    size_t capacity = upload->png_capacity == 0 ? 64 : upload->png_capacity * 2;
    size_t additional;

    if (capacity < upload->png_capacity) {
        upload->quota_exceeded = true;
        return false;
    }
    additional = capacity - upload->png_capacity;
    if (!reserve_decode(upload, additional)) {
        return false;
    }
    data = realloc(upload->png_data, capacity);
    if (data == NULL) {
        release_decode(upload, additional);
        upload->quota_exceeded = true;
        return false;
    }
    upload->png_data = data;
    upload->png_capacity = capacity;
    return true;
}

static bool append_png_byte(
    boxrig_kitty_graphics_upload_state *upload,
    uint8_t byte)
{
    if (upload->png_length == upload->png_capacity) {
        if (upload->png_fixed_capacity || !grow_png_buffer(upload)) {
            if (upload->quota_exceeded) {
                upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
            } else if (upload->png_fixed_capacity) {
                upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_BAD_ZLIB;
            }
            return false;
        }
    }
    upload->png_data[upload->png_length] = byte;
    upload->png_length++;
    return true;
}

static bool append_decoded_byte(
    boxrig_kitty_graphics_upload_state *upload,
    uint8_t byte)
{
    bool appended = upload->command.format == 100
                        ? append_png_byte(upload, byte)
                        : append_raw_byte(upload, byte);

    if (!appended && upload->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK) {
        upload->result = upload->compressed
                             ? BOXRIG_KITTY_GRAPHICS_RESULT_BAD_ZLIB
                             : BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    return appended;
}

static bool append_inflate_output(
    boxrig_kitty_graphics_upload_state *upload,
    const uint8_t *output,
    size_t length)
{
    size_t index;

    for (index = 0; index < length; index++) {
        if (!append_decoded_byte(upload, output[index])) {
            return false;
        }
    }
    return true;
}

static bool fail_zlib(
    boxrig_kitty_graphics_upload_state *upload,
    int zlib_result)
{
    upload->result = zlib_result == Z_MEM_ERROR || upload->quota_exceeded
                         ? BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE
                         : BOXRIG_KITTY_GRAPHICS_RESULT_BAD_ZLIB;
    return false;
}

static bool inflate_byte(
    boxrig_kitty_graphics_upload_state *upload,
    uint8_t byte)
{
    uint8_t output[BOXRIG_GRAPHICS_DECOMPRESS_BUFFER_SIZE];

    if (upload->zlib_ended) {
        return fail_zlib(upload, Z_DATA_ERROR);
    }
    upload->zlib_stream.next_in = &byte;
    upload->zlib_stream.avail_in = 1;
    for (;;) {
        uInt input_before = upload->zlib_stream.avail_in;
        size_t produced;
        int result;

        upload->zlib_stream.next_out = output;
        upload->zlib_stream.avail_out = sizeof(output);
        result = inflate(&upload->zlib_stream, Z_NO_FLUSH);
        produced = sizeof(output) - upload->zlib_stream.avail_out;
        if (!append_inflate_output(upload, output, produced)) {
            return false;
        }
        if (result == Z_STREAM_END) {
            upload->zlib_ended = true;
            return upload->zlib_stream.avail_in == 0 ||
                   fail_zlib(upload, Z_DATA_ERROR);
        }
        if (result != Z_OK) {
            return fail_zlib(upload, result);
        }
        if (upload->zlib_stream.avail_in == 0 &&
            upload->zlib_stream.avail_out != 0) {
            return true;
        }
        if (upload->zlib_stream.avail_in == input_before && produced == 0) {
            return fail_zlib(upload, Z_BUF_ERROR);
        }
    }
}

static bool finish_zlib(boxrig_kitty_graphics_upload_state *upload)
{
    uint8_t output[BOXRIG_GRAPHICS_DECOMPRESS_BUFFER_SIZE];

    if (upload->zlib_ended) {
        return true;
    }
    upload->zlib_stream.next_in = Z_NULL;
    upload->zlib_stream.avail_in = 0;
    for (;;) {
        size_t produced;
        int result;

        upload->zlib_stream.next_out = output;
        upload->zlib_stream.avail_out = sizeof(output);
        result = inflate(&upload->zlib_stream, Z_FINISH);
        produced = sizeof(output) - upload->zlib_stream.avail_out;
        if (!append_inflate_output(upload, output, produced)) {
            return false;
        }
        if (result == Z_STREAM_END) {
            upload->zlib_ended = true;
            return true;
        }
        if ((result == Z_OK || result == Z_BUF_ERROR) && produced != 0 &&
            upload->zlib_stream.avail_out == 0) {
            continue;
        }
        return fail_zlib(upload, result);
    }
}

static bool consume_decoded_byte(
    boxrig_kitty_graphics_upload_state *upload,
    uint8_t byte)
{
    return upload->compressed ? inflate_byte(upload, byte)
                              : append_decoded_byte(upload, byte);
}

static bool decode_quartet(boxrig_kitty_graphics_upload_state *upload)
{
    uint8_t first = upload->quartet[0];
    uint8_t second = upload->quartet[1];
    uint8_t third = upload->quartet[2];
    uint8_t fourth = upload->quartet[3];
    uint8_t decoded[3];
    size_t decoded_length;
    size_t index;

    if (first >= 64 || second >= 64) {
        return false;
    }
    decoded[0] = (uint8_t)((first << 2) | (second >> 4));
    if (third == 64) {
        if (fourth != 64 || (second & 0x0f) != 0) {
            return false;
        }
        decoded_length = 1;
        upload->base64_ended = true;
    } else {
        decoded[1] = (uint8_t)((second << 4) | (third >> 2));
        if (fourth == 64) {
            if ((third & 0x03) != 0) {
                return false;
            }
            decoded_length = 2;
            upload->base64_ended = true;
        } else {
            decoded[2] = (uint8_t)((third << 6) | fourth);
            decoded_length = 3;
        }
    }
    for (index = 0; index < decoded_length; index++) {
        if (!consume_decoded_byte(upload, decoded[index])) {
            return false;
        }
    }
    upload->quartet_length = 0;
    return true;
}

static bool base64_value(uint8_t byte, uint8_t *value_out)
{
    if (byte >= (uint8_t)'A' && byte <= (uint8_t)'Z') {
        *value_out = (uint8_t)(byte - (uint8_t)'A');
        return true;
    }
    if (byte >= (uint8_t)'a' && byte <= (uint8_t)'z') {
        *value_out = (uint8_t)(byte - (uint8_t)'a' + 26);
        return true;
    }
    if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') {
        *value_out = (uint8_t)(byte - (uint8_t)'0' + 52);
        return true;
    }
    if (byte == (uint8_t)'+') {
        *value_out = 62;
        return true;
    }
    if (byte == (uint8_t)'/') {
        *value_out = 63;
        return true;
    }
    return false;
}

static bool put_base64_byte(
    boxrig_kitty_graphics_upload_state *upload,
    uint8_t byte)
{
    uint8_t value;

    if (upload->base64_ended) {
        return false;
    }
    if (byte == (uint8_t)'=') {
        if (upload->quartet_length < 2) {
            return false;
        }
        value = 64;
    } else if (!base64_value(byte, &value)) {
        return false;
    }
    upload->quartet[upload->quartet_length] = value;
    upload->quartet_length++;
    return upload->quartet_length != 4 || decode_quartet(upload);
}

static bool finish_base64(boxrig_kitty_graphics_upload_state *upload)
{
    uint8_t decoded[2];
    size_t decoded_length;
    size_t index;

    if (upload->quartet_length == 0) {
        return true;
    }
    if (upload->base64_ended || upload->quartet_length == 1 ||
        upload->quartet[0] >= 64 || upload->quartet[1] >= 64) {
        return false;
    }
    decoded[0] = (uint8_t)(
        (upload->quartet[0] << 2) | (upload->quartet[1] >> 4));
    if (upload->quartet_length == 2) {
        if ((upload->quartet[1] & 0x0f) != 0) {
            return false;
        }
        decoded_length = 1;
    } else {
        if (upload->quartet[2] >= 64 ||
            (upload->quartet[2] & 0x03) != 0) {
            return false;
        }
        decoded[1] = (uint8_t)(
            (upload->quartet[1] << 4) | (upload->quartet[2] >> 2));
        decoded_length = 2;
    }
    for (index = 0; index < decoded_length; index++) {
        if (!consume_decoded_byte(upload, decoded[index])) {
            return false;
        }
    }
    upload->quartet_length = 0;
    return true;
}

static bool grow_transport_name(
    boxrig_kitty_graphics_protocol_state *protocol,
    boxrig_kitty_graphics_transaction_state *transaction)
{
    size_t capacity = transaction->transport_name_capacity == 0
                          ? 64
                          : transaction->transport_name_capacity * 2;
    uint8_t *replacement;

    if (capacity < transaction->transport_name_capacity ||
        capacity > protocol->transport_name_capacity) {
        capacity = protocol->transport_name_capacity;
    }
    if (capacity <= transaction->transport_name_capacity) {
        return false;
    }
    replacement = realloc(transaction->transport_name, capacity);
    if (replacement == NULL) {
        return false;
    }
    transaction->transport_name = replacement;
    transaction->transport_name_capacity = capacity;
    return true;
}

static bool append_transport_name_byte(
    boxrig_kitty_graphics_protocol_state *protocol,
    boxrig_kitty_graphics_transaction_state *transaction,
    uint8_t byte)
{
    if (byte == 0) {
        return false;
    }
    if (transaction->transport_name_length ==
            transaction->transport_name_capacity &&
        !grow_transport_name(protocol, transaction)) {
        return false;
    }
    transaction->transport_name[transaction->transport_name_length] = byte;
    transaction->transport_name_length++;
    return true;
}

static bool decode_transport_quartet(
    boxrig_kitty_graphics_protocol_state *protocol,
    boxrig_kitty_graphics_transaction_state *transaction)
{
    uint8_t first = transaction->transport_quartet[0];
    uint8_t second = transaction->transport_quartet[1];
    uint8_t third = transaction->transport_quartet[2];
    uint8_t fourth = transaction->transport_quartet[3];
    uint8_t decoded[3];
    size_t decoded_length;
    size_t index;

    if (first >= 64 || second >= 64) {
        return false;
    }
    decoded[0] = (uint8_t)((first << 2) | (second >> 4));
    if (third == 64) {
        if (fourth != 64 || (second & 0x0f) != 0) {
            return false;
        }
        decoded_length = 1;
        transaction->transport_base64_ended = true;
    } else {
        decoded[1] = (uint8_t)((second << 4) | (third >> 2));
        if (fourth == 64) {
            if ((third & 0x03) != 0) {
                return false;
            }
            decoded_length = 2;
            transaction->transport_base64_ended = true;
        } else {
            decoded[2] = (uint8_t)((third << 6) | fourth);
            decoded_length = 3;
        }
    }
    for (index = 0; index < decoded_length; index++) {
        if (!append_transport_name_byte(
                protocol,
                transaction,
                decoded[index])) {
            return false;
        }
    }
    transaction->transport_quartet_length = 0;
    return true;
}

static bool put_transport_base64_byte(
    boxrig_kitty_graphics_protocol_state *protocol,
    boxrig_kitty_graphics_transaction_state *transaction,
    uint8_t byte)
{
    uint8_t value;

    if (transaction->transport_base64_ended) {
        return false;
    }
    if (byte == (uint8_t)'=') {
        if (transaction->transport_quartet_length < 2) {
            return false;
        }
        value = 64;
    } else if (!base64_value(byte, &value)) {
        return false;
    }
    transaction->transport_quartet[
        transaction->transport_quartet_length] = value;
    transaction->transport_quartet_length++;
    return transaction->transport_quartet_length != 4 ||
           decode_transport_quartet(protocol, transaction);
}

static bool finish_transport_base64(
    boxrig_kitty_graphics_protocol_state *protocol,
    boxrig_kitty_graphics_transaction_state *transaction)
{
    uint8_t decoded[2];
    size_t decoded_length;
    size_t index;

    if (transaction->transport_quartet_length == 0) {
        return true;
    }
    if (transaction->transport_base64_ended ||
        transaction->transport_quartet_length == 1 ||
        transaction->transport_quartet[0] >= 64 ||
        transaction->transport_quartet[1] >= 64) {
        return false;
    }
    decoded[0] = (uint8_t)(
        (transaction->transport_quartet[0] << 2) |
        (transaction->transport_quartet[1] >> 4));
    if (transaction->transport_quartet_length == 2) {
        if ((transaction->transport_quartet[1] & 0x0f) != 0) {
            return false;
        }
        decoded_length = 1;
    } else {
        if (transaction->transport_quartet[2] >= 64 ||
            (transaction->transport_quartet[2] & 0x03) != 0) {
            return false;
        }
        decoded[1] = (uint8_t)(
            (transaction->transport_quartet[1] << 4) |
            (transaction->transport_quartet[2] >> 2));
        decoded_length = 2;
    }
    for (index = 0; index < decoded_length; index++) {
        if (!append_transport_name_byte(
                protocol,
                transaction,
                decoded[index])) {
            return false;
        }
    }
    transaction->transport_quartet_length = 0;
    return true;
}

static void destroy_png_decode_context(boxrig_png_decode_context *context)
{
    boxrig_kitty_graphics_upload_state *upload;

    if (context == NULL) {
        return;
    }
    upload = context->upload;
    free_decode_buffer(upload, context->rows, context->rows_size);
    context->rows = NULL;
    context->rows_size = 0;
    if (context->png != NULL) {
        png_destroy_read_struct(&context->png, &context->info, NULL);
    }
    free_decode_buffer(upload, context, sizeof(*context));
}

static bool decode_png(boxrig_kitty_graphics_upload_state *upload)
{
    boxrig_png_decode_context *context;
    png_uint_32 width;
    png_uint_32 height;
    int bit_depth;
    int color_type;
    int interlace_type;
    int compression_type;
    int filter_method;
    bool has_transparency;
    size_t stride;
    size_t rgba_length;
    size_t row_index;

    context = allocate_decode_buffer(upload, sizeof(*context));
    if (context == NULL) {
        upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
        return false;
    }
    memset(context, 0, sizeof(*context));
    context->upload = upload;
    context->png = png_create_read_struct_2(
        PNG_LIBPNG_VER_STRING,
        context,
        png_error_callback,
        png_warning_callback,
        upload,
        png_allocate,
        png_quota_free);
    if (context->png == NULL) {
        upload->result = upload->quota_exceeded
                             ? BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE
                             : BOXRIG_KITTY_GRAPHICS_RESULT_BAD_PNG;
        destroy_png_decode_context(context);
        return false;
    }
    if (setjmp(png_jmpbuf(context->png)) != 0) {
        upload->result = upload->quota_exceeded
                             ? BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE
                             : BOXRIG_KITTY_GRAPHICS_RESULT_BAD_PNG;
        destroy_png_decode_context(context);
        return false;
    }
    context->info = png_create_info_struct(context->png);
    if (context->info == NULL) {
        upload->result = upload->quota_exceeded
                             ? BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE
                             : BOXRIG_KITTY_GRAPHICS_RESULT_BAD_PNG;
        destroy_png_decode_context(context);
        return false;
    }
    png_set_user_limits(
        context->png,
        BOXRIG_GRAPHICS_IMAGE_SIDE_MAX,
        BOXRIG_GRAPHICS_IMAGE_SIDE_MAX);
    png_set_chunk_cache_max(
        context->png,
        BOXRIG_GRAPHICS_PNG_CHUNK_CACHE_MAX);
    png_set_chunk_malloc_max(
        context->png,
        (png_alloc_size_t)upload->decode_capacity);
    png_set_crc_action(
        context->png,
        PNG_CRC_ERROR_QUIT,
        PNG_CRC_ERROR_QUIT);
    png_set_read_fn(context->png, context, png_read_callback);
    png_read_info(context->png, context->info);
    if (png_get_IHDR(
            context->png,
            context->info,
            &width,
            &height,
            &bit_depth,
            &color_type,
            &interlace_type,
            &compression_type,
            &filter_method) == 0 ||
        width == 0 || height == 0 ||
        width > BOXRIG_GRAPHICS_IMAGE_SIDE_MAX ||
        height > BOXRIG_GRAPHICS_IMAGE_SIDE_MAX) {
        png_error(context->png, "invalid PNG dimensions");
    }
    has_transparency =
        png_get_valid(context->png, context->info, PNG_INFO_tRNS) != 0;
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(context->png);
    }
    if (has_transparency) {
        png_set_tRNS_to_alpha(context->png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(context->png);
    }
    if (bit_depth == 16) {
        png_set_scale_16(context->png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(context->png);
    }
    if ((color_type & PNG_COLOR_MASK_ALPHA) == 0 && !has_transparency) {
        png_set_add_alpha(context->png, 255, PNG_FILLER_AFTER);
    }
    (void)png_set_interlace_handling(context->png);
    png_read_update_info(context->png, context->info);
    if (png_get_bit_depth(context->png, context->info) != 8 ||
        png_get_channels(context->png, context->info) != 4 ||
        width > UINT32_MAX / 4) {
        png_error(context->png, "PNG did not normalize to RGBA8");
    }
    stride = (size_t)width * 4;
    if (png_get_rowbytes(context->png, context->info) != stride ||
        height > UINT32_MAX / (uint32_t)stride ||
        !multiply_size(stride, height, &rgba_length) ||
        !multiply_size(height, sizeof(*context->rows), &context->rows_size)) {
        png_error(context->png, "invalid normalized PNG size");
    }
    upload->rgba_length = rgba_length;
    upload->rgba = allocate_decode_buffer(upload, upload->rgba_length);
    context->rows = allocate_decode_buffer(upload, context->rows_size);
    if (upload->rgba == NULL || context->rows == NULL) {
        upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
        destroy_png_decode_context(context);
        return false;
    }
    for (row_index = 0; row_index < height; row_index++) {
        context->rows[row_index] = upload->rgba + row_index * stride;
    }
    png_read_image(context->png, context->rows);
    png_read_end(context->png, NULL);
    if (context->input_offset != upload->png_length) {
        upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_BAD_PNG;
        destroy_png_decode_context(context);
        return false;
    }
    upload->width = width;
    upload->height = height;
    upload->stride = (uint32_t)stride;
    upload->rgba_offset = upload->rgba_length;
    destroy_png_decode_context(context);
    return true;
}

static boxrig_image_resource_state *find_image_by_id(
    const boxrig_kitty_graphics_protocol_state *protocol,
    uint32_t image_id)
{
    size_t index;

    if (image_id == 0) {
        return NULL;
    }
    for (index = 0; index < protocol->image_count; index++) {
        if (protocol->images[index]->view.image_id == image_id) {
            return protocol->images[index];
        }
    }
    return NULL;
}

static boxrig_image_resource_state *find_image_by_handle(
    const boxrig_kitty_graphics_protocol_state *protocol,
    boxrig_image_handle handle)
{
    size_t index;

    if (handle == 0) {
        return NULL;
    }
    for (index = 0; index < protocol->image_count; index++) {
        if (protocol->images[index]->view.handle == handle) {
            return protocol->images[index];
        }
    }
    return NULL;
}

static boxrig_image_resource_state *find_latest_image_by_number(
    const boxrig_kitty_graphics_protocol_state *protocol,
    uint32_t image_number)
{
    size_t index = protocol->image_count;

    while (index != 0) {
        boxrig_image_resource_state *resource;

        index--;
        resource = protocol->images[index];
        if (resource->has_image_number &&
            resource->view.image_number == image_number) {
            return resource;
        }
    }
    return NULL;
}

static uint32_t increment_image_id(uint32_t image_id)
{
    image_id++;
    return image_id == 0 ? 1 : image_id;
}

static uint64_t increment_nonzero_u64(uint64_t value)
{
    value++;
    return value == 0 ? 1 : value;
}

static bool image_handle_in_use(
    const boxrig_kitty_graphics_protocol_state *protocol,
    boxrig_image_handle handle)
{
    size_t index;

    for (index = 0; index < protocol->image_count; index++) {
        if (protocol->images[index]->view.handle == handle) {
            return true;
        }
    }
    return false;
}

static bool select_image_handle(
    const boxrig_kitty_graphics_protocol_state *protocol,
    boxrig_image_handle *handle_out,
    boxrig_image_handle *next_handle_out)
{
    boxrig_image_handle candidate = protocol->next_image_handle;
    size_t attempt;

    if (candidate == 0) {
        candidate = 1;
    }
    for (attempt = 0; attempt <= protocol->image_count; attempt++) {
        if (!image_handle_in_use(protocol, candidate)) {
            *handle_out = candidate;
            *next_handle_out = increment_nonzero_u64(candidate);
            return true;
        }
        candidate = increment_nonzero_u64(candidate);
    }
    return false;
}

static bool select_generated_image_id(
    const boxrig_kitty_graphics_protocol_state *protocol,
    uint32_t *image_id_out,
    uint32_t *next_image_id_out)
{
    uint32_t candidate = protocol->next_image_id;
    size_t attempt;

    if (candidate == 0) {
        candidate = 1;
    }
    for (attempt = 0; attempt <= protocol->image_count; attempt++) {
        if (find_image_by_id(protocol, candidate) == NULL) {
            *image_id_out = candidate;
            *next_image_id_out = increment_image_id(candidate);
            return true;
        }
        candidate = increment_image_id(candidate);
    }
    return false;
}

static size_t resource_placement_count(
    const boxrig_kitty_graphics_protocol_state *protocol,
    const boxrig_image_resource_state *resource)
{
    size_t count = 0;
    size_t index;

    for (index = 0; index < protocol->placement_count; index++) {
        if (protocol->placements[index]->resource == resource) {
            count++;
        }
    }
    return count;
}

static unsigned int eviction_priority(
    const boxrig_terminal *terminal,
    const boxrig_image_resource_state *resource)
{
    const boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    bool placed = resource_placement_count(protocol, resource) != 0;
    bool visible = false;
    size_t index;

    if (!placed) {
        return resource->transient ? 0u : 1u;
    }
    for (index = 0; index < protocol->placement_count; index++) {
        if (protocol->placements[index]->resource == resource &&
            placement_is_active(terminal, protocol->placements[index])) {
            visible = true;
            break;
        }
    }
    if (!visible) {
        for (index = 0; index < protocol->placeholder_cell_capacity; index++) {
            const boxrig_placeholder_instance_state *instance =
                &protocol->placeholder_instances[index];

            if (instance->matched && instance->view.clip_width != 0 &&
                instance->view.clip_height != 0 &&
                instance->view.image_handle == resource->view.handle) {
                visible = true;
                break;
            }
        }
    }
    if (!visible) {
        return resource->transient ? 2u : 3u;
    }
    return resource->transient ? 4u : 5u;
}

static bool placement_chain_uses_resource(
    const boxrig_image_placement_state *placement,
    const boxrig_image_resource_state *resource)
{
    while (placement != NULL) {
        if (placement->resource == resource) {
            return true;
        }
        placement = placement->parent;
    }
    return false;
}

static size_t image_resource_index(
    const boxrig_kitty_graphics_protocol_state *protocol,
    const boxrig_image_resource_state *resource)
{
    size_t index;

    for (index = 0; index < protocol->image_count; index++) {
        if (protocol->images[index] == resource) {
            return index;
        }
    }
    return SIZE_MAX;
}

static void mark_evicted_placement_subtree(
    const boxrig_kitty_graphics_protocol_state *protocol,
    const boxrig_image_placement_state *root,
    bool *placement_marks)
{
    size_t root_index;
    size_t index;

    for (root_index = 0; root_index < protocol->placement_count;
         root_index++) {
        if (protocol->placements[root_index] == root) {
            break;
        }
    }
    if (root_index == protocol->placement_count ||
        placement_marks[root_index]) {
        return;
    }
    placement_marks[root_index] = true;
    for (index = 0; index < protocol->placement_count; index++) {
        if (protocol->placements[index]->parent == root) {
            mark_evicted_placement_subtree(
                protocol,
                protocol->placements[index],
                placement_marks);
        }
    }
}

static void mark_evicted_resource_closure(
    const boxrig_kitty_graphics_protocol_state *protocol,
    const boxrig_image_resource_state *resource,
    bool *image_marks,
    bool *placement_marks)
{
    size_t resource_index = image_resource_index(protocol, resource);
    size_t index;

    if (resource_index == SIZE_MAX || image_marks[resource_index]) {
        return;
    }
    image_marks[resource_index] = true;
    for (index = 0; index < protocol->placement_count; index++) {
        if (protocol->placements[index]->resource == resource) {
            mark_evicted_placement_subtree(
                protocol,
                protocol->placements[index],
                placement_marks);
        }
    }
    for (index = 0; index < protocol->image_count; index++) {
        boxrig_image_resource_state *candidate = protocol->images[index];
        size_t placement_index;
        bool has_placement = false;
        bool all_removed = true;

        if (image_marks[index]) {
            continue;
        }
        for (placement_index = 0;
             placement_index < protocol->placement_count;
             placement_index++) {
            if (protocol->placements[placement_index]->resource !=
                candidate) {
                continue;
            }
            has_placement = true;
            if (!placement_marks[placement_index]) {
                all_removed = false;
                break;
            }
        }
        if (has_placement && all_removed) {
            mark_evicted_resource_closure(
                protocol,
                candidate,
                image_marks,
                placement_marks);
        }
    }
}

static bool plan_storage_evictions(
    const boxrig_terminal *terminal,
    const boxrig_image_resource_state *replacement,
    const boxrig_image_placement_state *protected_parent,
    size_t new_length,
    boxrig_image_resource_state ***eviction_resources_out,
    size_t *eviction_count_out,
    size_t *removed_image_count_out,
    size_t *removed_placement_count_out)
{
    const boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_image_resource_state **eviction_resources = NULL;
    bool *image_marks = NULL;
    bool *placement_marks = NULL;
    size_t required_free;
    size_t freed = 0;
    size_t eviction_count = 0;
    size_t removed_image_count = 0;
    size_t removed_placement_count = 0;
    size_t index;
    unsigned int priority;

    *eviction_resources_out = NULL;
    *eviction_count_out = 0;
    *removed_image_count_out = 0;
    *removed_placement_count_out = 0;
    if (protocol->storage_usage > protocol->storage_capacity ||
        new_length > protocol->storage_capacity) {
        return false;
    }
    required_free = protocol->storage_usage >
                            protocol->storage_capacity - new_length
                        ? protocol->storage_usage -
                              (protocol->storage_capacity - new_length)
                        : 0;
    if (protocol->image_count != 0) {
        image_marks = calloc(protocol->image_count, sizeof(*image_marks));
    }
    if (protocol->placement_count != 0) {
        placement_marks = calloc(
            protocol->placement_count,
            sizeof(*placement_marks));
    }
    if ((protocol->image_count != 0 && image_marks == NULL) ||
        (protocol->placement_count != 0 && placement_marks == NULL)) {
        free(placement_marks);
        free(image_marks);
        return false;
    }
    if (replacement != NULL) {
        mark_evicted_resource_closure(
            protocol,
            replacement,
            image_marks,
            placement_marks);
    }
    for (index = 0; index < protocol->image_count; index++) {
        if (image_marks[index]) {
            freed += protocol->images[index]->view.rgba_length;
        }
    }
    /* The image array remains in creation-serial order, so priority scans
     * still choose the oldest candidate. Each choice is closed over relative
     * descendants before deciding whether another candidate is necessary. */
    for (priority = 0; priority <= 5 && freed < required_free; priority++) {
        for (index = 0;
             index < protocol->image_count && freed < required_free;
             index++) {
            size_t freed_before = freed;
            size_t counted_index;

            if (image_marks[index] ||
                placement_chain_uses_resource(
                    protected_parent,
                    protocol->images[index]) ||
                eviction_priority(terminal, protocol->images[index]) !=
                    priority) {
                continue;
            }
            mark_evicted_resource_closure(
                protocol,
                protocol->images[index],
                image_marks,
                placement_marks);
            freed = 0;
            for (counted_index = 0;
                 counted_index < protocol->image_count;
                 counted_index++) {
                if (image_marks[counted_index]) {
                    freed += protocol->images[counted_index]
                                 ->view.rgba_length;
                }
            }
            if (freed == freed_before) {
                free(placement_marks);
                free(image_marks);
                return false;
            }
        }
    }
    if (freed < required_free) {
        free(placement_marks);
        free(image_marks);
        return false;
    }
    for (index = 0; index < protocol->image_count; index++) {
        if (image_marks[index]) {
            removed_image_count++;
            if (protocol->images[index] != replacement) {
                eviction_count++;
            }
        }
    }
    for (index = 0; index < protocol->placement_count; index++) {
        if (placement_marks[index]) {
            removed_placement_count++;
        }
    }
    if (eviction_count != 0) {
        eviction_resources = malloc(
            eviction_count * sizeof(*eviction_resources));
        if (eviction_resources == NULL) {
            free(placement_marks);
            free(image_marks);
            return false;
        }
        eviction_count = 0;
        for (index = 0; index < protocol->image_count; index++) {
            if (image_marks[index] && protocol->images[index] != replacement) {
                eviction_resources[eviction_count] = protocol->images[index];
                eviction_count++;
            }
        }
    }
    free(placement_marks);
    free(image_marks);
    *eviction_resources_out = eviction_resources;
    *eviction_count_out = eviction_count;
    *removed_image_count_out = removed_image_count;
    *removed_placement_count_out = removed_placement_count;
    return true;
}

static bool ensure_image_slot(
    boxrig_kitty_graphics_protocol_state *protocol,
    size_t required_count)
{
    boxrig_image_resource_state **images;
    size_t capacity;

    if (required_count <= protocol->image_slot_capacity) {
        return true;
    }
    if (required_count > protocol->image_capacity) {
        return false;
    }
    capacity = protocol->image_slot_capacity == 0
                   ? 4
                   : protocol->image_slot_capacity * 2;
    if (capacity < protocol->image_slot_capacity ||
        capacity > protocol->image_capacity) {
        capacity = protocol->image_capacity;
    }
    if (capacity < required_count) {
        capacity = required_count;
    }
    images = realloc(protocol->images, capacity * sizeof(*images));
    if (images == NULL) {
        return false;
    }
    protocol->images = images;
    protocol->image_slot_capacity = capacity;
    return true;
}

static bool ensure_placement_slot(
    boxrig_kitty_graphics_protocol_state *protocol,
    size_t required_count)
{
    boxrig_image_placement_state **placements;
    size_t capacity;

    if (required_count <= protocol->placement_slot_capacity) {
        return true;
    }
    if (required_count > protocol->placement_capacity) {
        return false;
    }
    capacity = protocol->placement_slot_capacity == 0
                   ? 4
                   : protocol->placement_slot_capacity * 2;
    if (capacity < protocol->placement_slot_capacity ||
        capacity > protocol->placement_capacity) {
        capacity = protocol->placement_capacity;
    }
    if (capacity < required_count) {
        capacity = required_count;
    }
    placements = realloc(
        protocol->placements,
        capacity * sizeof(*placements));
    if (placements == NULL) {
        return false;
    }
    protocol->placements = placements;
    protocol->placement_slot_capacity = capacity;
    return true;
}

static bool ensure_image_dirty_storage(boxrig_terminal *terminal)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    size_t *dirty_start;
    size_t *dirty_end;
    size_t row;
    size_t preserved_rows;

    if (protocol->dirty_start != NULL &&
        protocol->dirty_end != NULL &&
        protocol->dirty_row_count == terminal->screen.rows) {
        return true;
    }
    if (terminal->screen.rows > SIZE_MAX / sizeof(*dirty_start)) {
        return false;
    }
    dirty_start = malloc(
        terminal->screen.rows * sizeof(*dirty_start));
    dirty_end = malloc(terminal->screen.rows * sizeof(*dirty_end));
    if (dirty_start == NULL || dirty_end == NULL) {
        free(dirty_start);
        free(dirty_end);
        return false;
    }
    for (row = 0; row < terminal->screen.rows; row++) {
        dirty_start[row] = terminal->screen.columns;
        dirty_end[row] = 0;
    }
    preserved_rows = protocol->dirty_row_count < terminal->screen.rows
                         ? protocol->dirty_row_count
                         : terminal->screen.rows;
    for (row = 0; row < preserved_rows; row++) {
        size_t start = protocol->dirty_start[row];
        size_t end = protocol->dirty_end[row];

        if (start < end && start < terminal->screen.columns) {
            dirty_start[row] = start;
            dirty_end[row] = end < terminal->screen.columns
                                  ? end
                                  : terminal->screen.columns;
        }
    }
    free(protocol->dirty_start);
    free(protocol->dirty_end);
    protocol->dirty_start = dirty_start;
    protocol->dirty_end = dirty_end;
    protocol->dirty_row_count = terminal->screen.rows;
    return true;
}

static void placeholder_text_row_changed(boxrig_terminal *terminal, size_t row)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    size_t offset = terminal->screen.alternate_active
                        ? 0 : terminal->screen.viewport_offset;

    if (protocol->placeholder_instances == NULL ||
        protocol->placeholder_cell_capacity !=
            terminal->screen.rows * terminal->screen.columns ||
        offset >= terminal->screen.rows ||
        row >= terminal->screen.rows - offset) {
        return;
    }
    protocol->placeholder_instances[(row + offset) * terminal->screen.columns]
        .row_refresh_pending = true;
}

static bool ensure_placeholder_storage(boxrig_terminal *terminal)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_placeholder_instance_state *instances;
    size_t cell_count;

    if (terminal->screen.rows > SIZE_MAX / terminal->screen.columns) {
        return false;
    }
    cell_count = terminal->screen.rows * terminal->screen.columns;
    if (protocol->placeholder_instances != NULL &&
        protocol->placeholder_cell_capacity == cell_count) {
        return true;
    }
    if (cell_count > SIZE_MAX / sizeof(*instances)) {
        return false;
    }
    instances = calloc(cell_count, sizeof(*instances));
    if (instances == NULL) {
        return false;
    }
    free(protocol->placeholder_instances);
    protocol->placeholder_instances = instances;
    protocol->placeholder_cell_capacity = cell_count;
    /* Ordinary sessions pay no callback cost before virtual placement is used. */
    terminal->screen.graphics_sink.text_row_changed =
        placeholder_text_row_changed;
    return true;
}

static void mark_image_interval(
    boxrig_terminal *terminal,
    size_t row,
    size_t start,
    size_t end)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;

    if (row >= protocol->dirty_row_count || start >= end ||
        start >= terminal->screen.columns) {
        return;
    }
    if (end > terminal->screen.columns) {
        end = terminal->screen.columns;
    }
    if (protocol->dirty_start[row] > start) {
        protocol->dirty_start[row] = start;
    }
    if (protocol->dirty_end[row] < end) {
        protocol->dirty_end[row] = end;
    }
}

static bool placement_is_active(
    const boxrig_terminal *terminal,
    const boxrig_image_placement_state *placement)
{
    return !placement->virtual_placement &&
           placement->alternate_buffer == terminal->screen.alternate_active &&
           placement->view.clip_width != 0 &&
           placement->view.clip_height != 0;
}

static size_t divide_rounding_up(size_t value, size_t divisor)
{
    return value / divisor + (value % divisor != 0 ? 1 : 0);
}

static bool multiply_divide_rounding_up(
    size_t left,
    size_t right,
    size_t divisor,
    size_t *result_out)
{
    size_t product;

    /* Reject an unrepresentable intermediate rather than allowing platform
     * integer wrap to choose a different placement geometry. */
    if (divisor == 0 || !multiply_size(left, right, &product)) {
        return false;
    }
    *result_out = divide_rounding_up(product, divisor);
    return *result_out != 0;
}

static bool calculate_virtual_fit(
    const boxrig_image_placement_state *prototype,
    uint32_t cell_width,
    uint32_t cell_height,
    size_t *box_width_out,
    size_t *box_height_out,
    size_t *fit_width_out,
    size_t *fit_height_out)
{
    size_t box_width;
    size_t box_height;
    size_t width_product;
    size_t height_product;
    size_t fit_width;
    size_t fit_height;

    if (prototype->requested_columns == 0 ||
        prototype->requested_rows == 0 ||
        prototype->view.source_width == 0 ||
        prototype->view.source_height == 0 ||
        !multiply_size(
            prototype->requested_columns,
            cell_width,
            &box_width) ||
        !multiply_size(
            prototype->requested_rows,
            cell_height,
            &box_height) ||
        !multiply_size(
            prototype->view.source_width,
            box_height,
            &width_product) ||
        !multiply_size(
            prototype->view.source_height,
            box_width,
            &height_product)) {
        return false;
    }
    if (width_product > height_product) {
        fit_width = box_width;
        fit_height = height_product / prototype->view.source_width;
        if (fit_height == 0) {
            fit_height = 1;
        }
    } else {
        fit_height = box_height;
        fit_width = width_product / prototype->view.source_height;
        if (fit_width == 0) {
            fit_width = 1;
        }
    }
    if (fit_width > box_width || fit_height > box_height) {
        return false;
    }
    *box_width_out = box_width;
    *box_height_out = box_height;
    *fit_width_out = fit_width;
    *fit_height_out = fit_height;
    return true;
}

static bool checked_signed_cell_pixel(
    int64_t cell,
    uint32_t cell_size,
    uint32_t offset,
    int64_t *pixel_out)
{
    int64_t pixel;

    if (cell_size == 0 ||
        cell > INT64_MAX / (int64_t)cell_size ||
        cell < INT64_MIN / (int64_t)cell_size) {
        return false;
    }
    pixel = cell * (int64_t)cell_size;
    if (pixel > INT64_MAX - (int64_t)offset) {
        return false;
    }
    *pixel_out = pixel + (int64_t)offset;
    return true;
}

static bool placement_projected_anchor(
    const boxrig_terminal *terminal,
    const boxrig_image_placement_state *placement,
    int64_t *row_out)
{
    int64_t row = placement->anchor_line;

    if (!placement->alternate_buffer && !placement->viewport_anchored) {
        if (terminal->screen.viewport_offset > (size_t)INT64_MAX ||
            row > INT64_MAX - (int64_t)terminal->screen.viewport_offset) {
            return false;
        }
        row += (int64_t)terminal->screen.viewport_offset;
    }
    *row_out = row;
    return true;
}

static int64_t maximum_i64(int64_t left, int64_t right)
{
    return left > right ? left : right;
}

static int64_t minimum_i64(int64_t left, int64_t right)
{
    return left < right ? left : right;
}

static bool resolve_placement_geometry(
    const boxrig_terminal *terminal,
    const boxrig_image_placement_state *placement,
    uint32_t cell_width,
    uint32_t cell_height,
    boxrig_image_placement_view *view_out)
{
    boxrig_image_placement_view view = placement->view;
    size_t destination_width;
    size_t destination_height;
    size_t columns;
    size_t rows;
    size_t screen_width_size;
    size_t screen_height_size;
    int64_t projected_anchor;
    int64_t drawing_x;
    int64_t drawing_y;
    int64_t drawing_end_x;
    int64_t drawing_end_y;
    int64_t visible_x;
    int64_t visible_y;
    int64_t visible_end_x;
    int64_t visible_end_y;
    int64_t screen_width;
    int64_t screen_height;

    if (cell_width == 0 || cell_height == 0 ||
        view.cell_x_offset >= cell_width ||
        view.cell_y_offset >= cell_height ||
        view.source_width == 0 || view.source_height == 0) {
        return false;
    }
    if (placement->columns_specified && placement->rows_specified) {
        if (!multiply_size(
                placement->requested_columns,
                cell_width,
                &destination_width) ||
            !multiply_size(
                placement->requested_rows,
                cell_height,
                &destination_height)) {
            return false;
        }
        columns = placement->requested_columns;
        rows = placement->requested_rows;
    } else if (placement->columns_specified) {
        if (!multiply_size(
                placement->requested_columns,
                cell_width,
                &destination_width) ||
            !multiply_divide_rounding_up(
                view.source_height,
                destination_width,
                view.source_width,
                &destination_height)) {
            return false;
        }
        columns = placement->requested_columns;
        rows = divide_rounding_up(destination_height, cell_height);
    } else if (placement->rows_specified) {
        if (!multiply_size(
                placement->requested_rows,
                cell_height,
                &destination_height) ||
            !multiply_divide_rounding_up(
                view.source_width,
                destination_height,
                view.source_height,
                &destination_width)) {
            return false;
        }
        rows = placement->requested_rows;
        columns = divide_rounding_up(destination_width, cell_width);
    } else {
        destination_width = view.source_width;
        destination_height = view.source_height;
        columns = divide_rounding_up(destination_width, cell_width);
        rows = divide_rounding_up(destination_height, cell_height);
    }
    if (columns == 0 || rows == 0 || destination_width == 0 ||
        destination_height == 0 ||
        destination_width > (size_t)INT64_MAX ||
        destination_height > (size_t)INT64_MAX ||
        !multiply_size(
            terminal->screen.columns,
            cell_width,
            &screen_width_size) ||
        !multiply_size(
            terminal->screen.rows,
            cell_height,
            &screen_height_size) ||
        screen_width_size > (size_t)INT64_MAX ||
        screen_height_size > (size_t)INT64_MAX ||
        !placement_projected_anchor(
            terminal,
            placement,
            &projected_anchor) ||
        !checked_signed_cell_pixel(
            placement->anchor_column,
            cell_width,
            view.cell_x_offset,
            &drawing_x) ||
        !checked_signed_cell_pixel(
            projected_anchor,
            cell_height,
            view.cell_y_offset,
            &drawing_y)) {
        return false;
    }
    if (drawing_x > INT64_MAX - (int64_t)destination_width ||
        drawing_y > INT64_MAX - (int64_t)destination_height) {
        return false;
    }
    drawing_end_x = drawing_x + (int64_t)destination_width;
    drawing_end_y = drawing_y + (int64_t)destination_height;
    screen_width = (int64_t)screen_width_size;
    screen_height = (int64_t)screen_height_size;
    visible_x = maximum_i64(drawing_x, 0);
    visible_y = maximum_i64(drawing_y, 0);
    visible_end_x = minimum_i64(drawing_end_x, screen_width);
    visible_end_y = minimum_i64(drawing_end_y, screen_height);
    if (placement->clip_cell_left != 0) {
        int64_t clip_x;

        if (placement->clip_cell_left > (size_t)INT64_MAX ||
            placement->anchor_column >
                INT64_MAX - (int64_t)placement->clip_cell_left ||
            !checked_signed_cell_pixel(
                placement->anchor_column +
                    (int64_t)placement->clip_cell_left,
                cell_width,
                0,
                &clip_x)) {
            return false;
        }
        visible_x = maximum_i64(visible_x, clip_x);
    }
    if (placement->clip_cell_right != SIZE_MAX) {
        int64_t clip_end_x;

        if (placement->clip_cell_right > (size_t)INT64_MAX ||
            placement->anchor_column >
                INT64_MAX - (int64_t)placement->clip_cell_right ||
            !checked_signed_cell_pixel(
                placement->anchor_column +
                    (int64_t)placement->clip_cell_right,
                cell_width,
                0,
                &clip_end_x)) {
            return false;
        }
        visible_end_x = minimum_i64(visible_end_x, clip_end_x);
    }
    if (placement->clip_cell_top != 0) {
        int64_t clip_y;

        if (placement->clip_cell_top > (size_t)INT64_MAX ||
            projected_anchor >
                INT64_MAX - (int64_t)placement->clip_cell_top ||
            !checked_signed_cell_pixel(
                projected_anchor + (int64_t)placement->clip_cell_top,
                cell_height,
                0,
                &clip_y)) {
            return false;
        }
        visible_y = maximum_i64(visible_y, clip_y);
    }
    if (placement->clip_cell_bottom != SIZE_MAX) {
        int64_t clip_end_y;

        if (placement->clip_cell_bottom > (size_t)INT64_MAX ||
            projected_anchor >
                INT64_MAX - (int64_t)placement->clip_cell_bottom ||
            !checked_signed_cell_pixel(
                projected_anchor + (int64_t)placement->clip_cell_bottom,
                cell_height,
                0,
                &clip_end_y)) {
            return false;
        }
        visible_end_y = minimum_i64(visible_end_y, clip_end_y);
    }
    view.columns = columns;
    view.rows = rows;
    view.destination_pixel_width = destination_width;
    view.destination_pixel_height = destination_height;
    if (projected_anchor > 0 && (uint64_t)projected_anchor > SIZE_MAX) {
        return false;
    }
    view.anchor_row = projected_anchor < 0 ? 0 : (size_t)projected_anchor;
    view.anchor_column = placement->anchor_column < 0
                             ? 0
                             : (size_t)placement->anchor_column;
    if (visible_x >= visible_end_x || visible_y >= visible_end_y) {
        view.visual_column = terminal->screen.columns;
        view.visual_row = terminal->screen.rows;
        view.clip_x = 0;
        view.clip_y = 0;
        view.clip_width = 0;
        view.clip_height = 0;
        *view_out = view;
        return true;
    }
    view.visual_column = (size_t)visible_x / cell_width;
    view.visual_row = (size_t)visible_y / cell_height;
    view.clip_x = (size_t)(visible_x - drawing_x);
    view.clip_y = (size_t)(visible_y - drawing_y);
    view.clip_width = (size_t)(visible_end_x - visible_x);
    view.clip_height = (size_t)(visible_end_y - visible_y);
    *view_out = view;
    return true;
}

static void mark_placement_dirty(
    boxrig_terminal *terminal,
    const boxrig_image_placement_state *placement,
    uint32_t cell_width,
    uint32_t cell_height)
{
    const boxrig_image_placement_view *view = &placement->view;
    int64_t drawing_x;
    int64_t projected_anchor;
    int64_t drawing_y;
    int64_t pixel_y_signed;
    int64_t pixel_x_signed;
    size_t pixel_x;
    size_t pixel_y;
    size_t pixel_end_x;
    size_t pixel_end_y;
    size_t start_column;
    size_t end_column;
    size_t start_row;
    size_t end_row;
    size_t row;

    if (!placement_is_active(terminal, placement) || cell_width == 0 ||
        cell_height == 0 ||
        !checked_signed_cell_pixel(
            placement->anchor_column,
            cell_width,
            view->cell_x_offset,
            &drawing_x) ||
        !placement_projected_anchor(
            terminal,
            placement,
            &projected_anchor) ||
        !checked_signed_cell_pixel(
            projected_anchor,
            cell_height,
            view->cell_y_offset,
            &drawing_y)) {
        return;
    }
    if (view->clip_x > (size_t)INT64_MAX ||
        drawing_x > INT64_MAX - (int64_t)view->clip_x ||
        drawing_y > INT64_MAX - (int64_t)view->clip_y) {
        return;
    }
    pixel_x_signed = drawing_x + (int64_t)view->clip_x;
    pixel_y_signed = drawing_y + (int64_t)view->clip_y;
    if (pixel_x_signed < 0 || pixel_y_signed < 0) {
        return;
    }
    pixel_x = (size_t)pixel_x_signed;
    pixel_y = (size_t)pixel_y_signed;
    if (view->clip_width > SIZE_MAX - pixel_x ||
        view->clip_height > SIZE_MAX - pixel_y) {
        return;
    }
    pixel_end_x = pixel_x + view->clip_width;
    pixel_end_y = pixel_y + view->clip_height;
    start_column = pixel_x / cell_width;
    end_column = divide_rounding_up(pixel_end_x, cell_width);
    start_row = pixel_y / cell_height;
    end_row = divide_rounding_up(pixel_end_y, cell_height);
    for (row = start_row; row < end_row; row++) {
        mark_image_interval(terminal, row, start_column, end_column);
    }
}

static boxrig_image_placement_state *find_placement_pair(
    const boxrig_kitty_graphics_protocol_state *protocol,
    uint32_t image_id,
    uint32_t placement_id)
{
    size_t index;

    if (image_id == 0 || placement_id == 0) {
        return NULL;
    }
    for (index = 0; index < protocol->placement_count; index++) {
        boxrig_image_placement_state *placement = protocol->placements[index];

        if (placement->view.image_id == image_id &&
            placement->view.placement_id == placement_id) {
            return placement;
        }
    }
    return NULL;
}

static bool placement_chain_contains(
    const boxrig_image_placement_state *placement,
    const boxrig_image_placement_state *candidate)
{
    size_t depth = 0;

    while (placement != NULL && depth <= BOXRIG_GRAPHICS_RELATIVE_DEPTH_MAX) {
        if (placement == candidate) {
            return true;
        }
        placement = placement->parent;
        depth++;
    }
    return false;
}

static size_t placement_depth(
    const boxrig_image_placement_state *placement)
{
    size_t depth = 0;

    while (placement != NULL && placement->parent != NULL) {
        depth++;
        placement = placement->parent;
    }
    return depth;
}

static bool replacement_subtree_fits_depth(
    const boxrig_kitty_graphics_protocol_state *protocol,
    const boxrig_image_placement_state *replacement,
    size_t replacement_depth)
{
    size_t index;

    if (replacement == NULL) {
        return replacement_depth <= BOXRIG_GRAPHICS_RELATIVE_DEPTH_MAX;
    }
    for (index = 0; index < protocol->placement_count; index++) {
        const boxrig_image_placement_state *placement =
            protocol->placements[index];
        size_t distance = 0;

        while (placement != NULL && placement != replacement) {
            placement = placement->parent;
            distance++;
        }
        if (placement == replacement &&
            (distance > BOXRIG_GRAPHICS_RELATIVE_DEPTH_MAX ||
             replacement_depth >
                 BOXRIG_GRAPHICS_RELATIVE_DEPTH_MAX - distance)) {
            return false;
        }
    }
    return true;
}

static bool virtual_placement_anchor(
    const boxrig_terminal *terminal,
    const boxrig_image_placement_state *prototype,
    int64_t *row_out,
    int64_t *column_out)
{
    const boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    size_t cell_count = terminal->screen.rows * terminal->screen.columns;
    size_t minimum_row = terminal->screen.rows;
    size_t minimum_column = terminal->screen.columns;
    size_t index;
    bool found = false;

    if (!prototype->virtual_placement ||
        prototype->alternate_buffer != terminal->screen.alternate_active) {
        return false;
    }
    for (index = 0; index < cell_count; index++) {
        const boxrig_placeholder_instance_state *instance =
            &protocol->placeholder_instances[index];

        if (!instance->matched ||
            instance->prototype_serial != prototype->creation_serial) {
            continue;
        }
        if (instance->view.visual_row < minimum_row) {
            minimum_row = instance->view.visual_row;
        }
        if (instance->view.visual_column < minimum_column) {
            minimum_column = instance->view.visual_column;
        }
        found = true;
    }
    if (!found || minimum_row > (size_t)INT64_MAX ||
        minimum_column > (size_t)INT64_MAX) {
        return false;
    }
    *row_out = (int64_t)minimum_row;
    *column_out = (int64_t)minimum_column;
    return true;
}

static bool resolve_relationship_anchor(
    const boxrig_terminal *terminal,
    const boxrig_image_placement_state *placement,
    size_t depth,
    int64_t *row_out,
    int64_t *column_out,
    bool *viewport_anchored_out)
{
    int64_t row;
    int64_t column;
    bool viewport_anchored;

    if (placement->virtual_placement) {
        if (!virtual_placement_anchor(terminal, placement, &row, &column)) {
            return false;
        }
        *row_out = row;
        *column_out = column;
        *viewport_anchored_out = true;
        return true;
    }
    if (!placement->relative_placement) {
        *row_out = placement->anchor_line;
        *column_out = placement->anchor_column;
        *viewport_anchored_out = false;
        return true;
    }
    if (placement->parent == NULL ||
        depth >= BOXRIG_GRAPHICS_RELATIVE_DEPTH_MAX ||
        !resolve_relationship_anchor(
            terminal,
            placement->parent,
            depth + 1,
            &row,
            &column,
            &viewport_anchored)) {
        return false;
    }
    if ((placement->relative_y_offset > 0 &&
         row > INT64_MAX - placement->relative_y_offset) ||
        (placement->relative_y_offset < 0 &&
         row < INT64_MIN - placement->relative_y_offset) ||
        (placement->relative_x_offset > 0 &&
         column > INT64_MAX - placement->relative_x_offset) ||
        (placement->relative_x_offset < 0 &&
         column < INT64_MIN - placement->relative_x_offset)) {
        return false;
    }
    *row_out = row + placement->relative_y_offset;
    *column_out = column + placement->relative_x_offset;
    *viewport_anchored_out = viewport_anchored;
    return true;
}

static bool resolve_relative_geometry(
    const boxrig_terminal *terminal,
    boxrig_image_placement_state *placement,
    uint32_t cell_width,
    uint32_t cell_height,
    boxrig_image_placement_view *view_out)
{
    boxrig_image_placement_view resolved;
    int64_t row = 0;
    int64_t column = 0;
    bool viewport_anchored = false;
    bool parent_visible = resolve_relationship_anchor(
        terminal,
        placement,
        0,
        &row,
        &column,
        &viewport_anchored);

    placement->anchor_line = row;
    placement->anchor_column = column;
    placement->viewport_anchored = viewport_anchored;
    placement->relative_parent_visible = parent_visible;
    if (!resolve_placement_geometry(
            terminal,
            placement,
            cell_width,
            cell_height,
            &resolved)) {
        return false;
    }
    if (!parent_visible) {
        resolved.visual_row = terminal->screen.rows;
        resolved.visual_column = terminal->screen.columns;
        resolved.clip_x = 0;
        resolved.clip_y = 0;
        resolved.clip_width = 0;
        resolved.clip_height = 0;
    }
    *view_out = resolved;
    return true;
}

typedef struct boxrig_placeholder_descriptor {
    uint32_t image_id;
    uint32_t image_id_low;
    uint32_t placement_id;
    uint32_t image_row;
    uint32_t image_column;
    uint32_t image_id_high;
    bool valid;
} boxrig_placeholder_descriptor;

static bool decode_cell_codepoint(
    const char *text,
    size_t text_length,
    size_t *offset,
    int32_t *codepoint_out)
{
    const uint8_t *bytes = (const uint8_t *)text;
    uint32_t codepoint;
    size_t length;
    size_t index;

    if (*offset >= text_length) {
        return false;
    }
    if (bytes[*offset] < 0x80) {
        *codepoint_out = bytes[*offset];
        (*offset)++;
        return true;
    }
    if (bytes[*offset] >= 0xc2 && bytes[*offset] <= 0xdf) {
        codepoint = bytes[*offset] & 0x1f;
        length = 2;
    } else if (bytes[*offset] >= 0xe0 && bytes[*offset] <= 0xef) {
        codepoint = bytes[*offset] & 0x0f;
        length = 3;
    } else if (bytes[*offset] >= 0xf0 && bytes[*offset] <= 0xf4) {
        codepoint = bytes[*offset] & 0x07;
        length = 4;
    } else {
        return false;
    }
    if (length > text_length - *offset) {
        return false;
    }
    for (index = 1; index < length; index++) {
        uint8_t byte = bytes[*offset + index];

        if ((byte & 0xc0) != 0x80) {
            return false;
        }
        codepoint = (codepoint << 6) | (byte & 0x3f);
    }
    if ((length == 3 && codepoint < 0x800) ||
        (length == 4 && codepoint < 0x10000) ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
        codepoint > 0x10ffff) {
        return false;
    }
    *offset += length;
    *codepoint_out = (int32_t)codepoint;
    return true;
}

static bool rowcolumn_diacritic_index(
    int32_t codepoint,
    uint32_t *index_out)
{
    size_t index;

    for (index = 0;
         index < sizeof(boxrig_kitty_rowcolumn_diacritics) /
                     sizeof(boxrig_kitty_rowcolumn_diacritics[0]);
         index++) {
        if (boxrig_kitty_rowcolumn_diacritics[index] == codepoint) {
            *index_out = (uint32_t)index;
            return true;
        }
    }
    return false;
}

static uint32_t color_protocol_value(boxrig_color color)
{
    if (color.kind == BOXRIG_COLOR_INDEXED) {
        return color.index;
    }
    if (color.kind == BOXRIG_COLOR_RGB) {
        return ((uint32_t)color.red << 16) |
               ((uint32_t)color.green << 8) | color.blue;
    }
    return 0;
}

static bool parse_placeholder_descriptor(
    const boxrig_cell *cell,
    const boxrig_placeholder_descriptor *previous,
    boxrig_placeholder_descriptor *descriptor_out)
{
    boxrig_placeholder_descriptor descriptor = {0};
    int32_t codepoint;
    uint32_t diacritics[3] = {0, 0, 0};
    bool specified[3] = {false, false, false};
    size_t offset = 0;
    size_t index = 0;
    bool continues;

    if (cell->continuation || boxrig_cell_text(cell) == NULL ||
        !decode_cell_codepoint(
            boxrig_cell_text(cell),
            cell->text_length,
            &offset,
            &codepoint) ||
        codepoint != 0x10eeee) {
        return false;
    }
    while (index < 3 && offset < cell->text_length) {
        if (!decode_cell_codepoint(
                boxrig_cell_text(cell),
                cell->text_length,
                &offset,
                &codepoint)) {
            break;
        }
        specified[index] = rowcolumn_diacritic_index(
            codepoint,
            &diacritics[index]);
        index++;
    }
    descriptor.image_id_low = color_protocol_value(cell->style.foreground);
    descriptor.placement_id = color_protocol_value(
        cell->style.underline_color);
    descriptor.image_row = specified[0] ? diacritics[0] : 0;
    descriptor.image_column = specified[1] ? diacritics[1] : 0;
    descriptor.image_id_high = specified[2] ? diacritics[2] : 0;
    continues = previous != NULL && previous->valid &&
                descriptor.image_id_low == previous->image_id_low &&
                descriptor.placement_id == previous->placement_id &&
                (!specified[0] ||
                 descriptor.image_row == previous->image_row) &&
                (!specified[1] ||
                 descriptor.image_column == previous->image_column + 1) &&
                (!specified[2] ||
                 descriptor.image_id_high == previous->image_id_high);
    if (continues) {
        if (!specified[0]) {
            descriptor.image_row = previous->image_row;
        }
        if (!specified[1]) {
            descriptor.image_column = previous->image_column + 1;
        }
        if (!specified[2]) {
            descriptor.image_id_high = previous->image_id_high;
        }
    }
    descriptor.valid = descriptor.image_id_high <= UINT8_MAX;
    descriptor.image_id = descriptor.image_id_low |
                          (descriptor.image_id_high << 24);
    *descriptor_out = descriptor;
    return true;
}

static const boxrig_image_placement_state *find_virtual_placement(
    const boxrig_terminal *terminal,
    uint32_t image_id,
    uint32_t placement_id)
{
    const boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    size_t index;

    for (index = 0; index < protocol->placement_count; index++) {
        const boxrig_image_placement_state *placement =
            protocol->placements[index];

        if (!placement->virtual_placement ||
            placement->alternate_buffer != terminal->screen.alternate_active ||
            placement->view.image_id != image_id ||
            (placement_id != 0 &&
             placement->view.placement_id != placement_id)) {
            continue;
        }
        return placement;
    }
    return NULL;
}

static bool build_placeholder_instance(
    const boxrig_terminal *terminal,
    const boxrig_placeholder_descriptor *descriptor,
    size_t screen_row,
    size_t screen_column,
    boxrig_placeholder_instance_state *instance_out)
{
    const boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    const boxrig_image_placement_state *prototype = find_virtual_placement(
        terminal,
        descriptor->image_id,
        descriptor->placement_id);
    boxrig_placeholder_instance_state instance = {0};
    boxrig_image_placement_view view = {0};
    size_t box_width;
    size_t box_height;
    size_t fit_width;
    size_t fit_height;
    size_t fit_left;
    size_t fit_top;
    size_t tile_left;
    size_t tile_top;
    size_t tile_right;
    size_t tile_bottom;
    size_t visible_left;
    size_t visible_top;
    size_t visible_right;
    size_t visible_bottom;

    if (!descriptor->valid || prototype == NULL ||
        descriptor->image_column >= prototype->requested_columns ||
        descriptor->image_row >= prototype->requested_rows ||
        !calculate_virtual_fit(
            prototype,
            protocol->cell_pixel_width,
            protocol->cell_pixel_height,
            &box_width,
            &box_height,
            &fit_width,
            &fit_height) ||
        !multiply_size(
            descriptor->image_column,
            protocol->cell_pixel_width,
            &tile_left) ||
        !multiply_size(
            descriptor->image_row,
            protocol->cell_pixel_height,
            &tile_top)) {
        return false;
    }
    fit_left = (box_width - fit_width) / 2;
    fit_top = (box_height - fit_height) / 2;
    tile_right = tile_left + protocol->cell_pixel_width;
    tile_bottom = tile_top + protocol->cell_pixel_height;
    visible_left = tile_left > fit_left ? tile_left : fit_left;
    visible_top = tile_top > fit_top ? tile_top : fit_top;
    visible_right = tile_right < fit_left + fit_width
                        ? tile_right
                        : fit_left + fit_width;
    visible_bottom = tile_bottom < fit_top + fit_height
                         ? tile_bottom
                         : fit_top + fit_height;
    view.image_handle = prototype->view.image_handle;
    view.resource_creation_serial = prototype->resource->creation_serial;
    view.placement_creation_serial = prototype->creation_serial;
    view.kind = BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER;
    view.image_id = prototype->view.image_id;
    view.image_number = prototype->view.image_number;
    view.placement_id = prototype->view.placement_id;
    view.anchor_row = screen_row;
    view.anchor_column = screen_column;
    view.visual_row = screen_row;
    view.visual_column = screen_column;
    view.source_x = prototype->view.source_x;
    view.source_y = prototype->view.source_y;
    view.source_width = prototype->view.source_width;
    view.source_height = prototype->view.source_height;
    view.columns = prototype->requested_columns;
    view.rows = prototype->requested_rows;
    view.destination_pixel_width = fit_width;
    view.destination_pixel_height = fit_height;
    view.z_index = prototype->view.z_index;
    if (visible_left < visible_right && visible_top < visible_bottom) {
        view.cell_x_offset = (uint32_t)(visible_left - tile_left);
        view.cell_y_offset = (uint32_t)(visible_top - tile_top);
        view.clip_x = visible_left - fit_left;
        view.clip_y = visible_top - fit_top;
        view.clip_width = visible_right - visible_left;
        view.clip_height = visible_bottom - visible_top;
    }
    instance.view = view;
    instance.prototype_serial = prototype->creation_serial;
    instance.matched = true;
    *instance_out = instance;
    return true;
}

static bool placeholder_instance_equal(
    const boxrig_placeholder_instance_state *left,
    const boxrig_placeholder_instance_state *right)
{
    const boxrig_image_placement_view *a = &left->view;
    const boxrig_image_placement_view *b = &right->view;

    return left->matched == right->matched &&
           left->prototype_serial == right->prototype_serial &&
           (!left->matched ||
            (a->image_handle == b->image_handle &&
             a->image_id == b->image_id &&
             a->image_number == b->image_number &&
             a->placement_id == b->placement_id &&
             a->visual_row == b->visual_row &&
             a->visual_column == b->visual_column &&
             a->source_x == b->source_x && a->source_y == b->source_y &&
             a->source_width == b->source_width &&
             a->source_height == b->source_height &&
             a->cell_x_offset == b->cell_x_offset &&
             a->cell_y_offset == b->cell_y_offset &&
             a->columns == b->columns && a->rows == b->rows &&
             a->destination_pixel_width == b->destination_pixel_width &&
             a->destination_pixel_height == b->destination_pixel_height &&
             a->clip_x == b->clip_x && a->clip_y == b->clip_y &&
             a->clip_width == b->clip_width &&
             a->clip_height == b->clip_height &&
             a->z_index == b->z_index));
}

static bool placement_view_geometry_equal(
    const boxrig_image_placement_view *left,
    const boxrig_image_placement_view *right)
{
    return left->image_handle == right->image_handle &&
           left->kind == right->kind && left->image_id == right->image_id &&
           left->image_number == right->image_number &&
           left->placement_id == right->placement_id &&
           left->anchor_row == right->anchor_row &&
           left->anchor_column == right->anchor_column &&
           left->visual_row == right->visual_row &&
           left->visual_column == right->visual_column &&
           left->source_x == right->source_x &&
           left->source_y == right->source_y &&
           left->source_width == right->source_width &&
           left->source_height == right->source_height &&
           left->cell_x_offset == right->cell_x_offset &&
           left->cell_y_offset == right->cell_y_offset &&
           left->columns == right->columns && left->rows == right->rows &&
           left->destination_pixel_width ==
               right->destination_pixel_width &&
           left->destination_pixel_height ==
               right->destination_pixel_height &&
           left->clip_x == right->clip_x && left->clip_y == right->clip_y &&
           left->clip_width == right->clip_width &&
           left->clip_height == right->clip_height &&
           left->z_index == right->z_index;
}

static void refresh_derived_placements(boxrig_terminal *terminal, bool all_rows)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_placeholder_descriptor previous = {0};
    size_t row;
    size_t column;
    size_t index;
    bool changed = false;

    if (protocol->placeholder_instances != NULL &&
        protocol->placeholder_cell_capacity ==
            terminal->screen.rows * terminal->screen.columns) {
        for (row = 0; row < terminal->screen.rows; row++) {
            boxrig_placeholder_instance_state *first =
                &protocol->placeholder_instances[row * terminal->screen.columns];
            if (!all_rows && !first->row_refresh_pending) {
                continue;
            }
            previous.valid = false;
            for (column = 0; column < terminal->screen.columns; column++) {
                boxrig_text_workload_observe_placeholder_cell();
                const boxrig_cell *cell = boxrig_screen_view_cell(
                    &terminal->screen,
                    row,
                    column);
                boxrig_placeholder_descriptor descriptor = {0};
                boxrig_placeholder_instance_state next = {0};
                boxrig_placeholder_instance_state *current =
                    &protocol->placeholder_instances[
                        row * terminal->screen.columns + column];

                if (parse_placeholder_descriptor(
                        cell,
                        previous.valid ? &previous : NULL,
                        &descriptor)) {
                    (void)build_placeholder_instance(
                        terminal,
                        &descriptor,
                        row,
                        column,
                        &next);
                    previous = descriptor;
                } else {
                    previous.valid = false;
                }
                if (!placeholder_instance_equal(current, &next)) {
                    mark_image_interval(terminal, row, column, column + 1);
                    *current = next;
                    changed = true;
                }
            }
            first->row_refresh_pending = false;
        }
    }
    for (index = 0; index < protocol->placement_count; index++) {
        boxrig_image_placement_state *placement = protocol->placements[index];
        boxrig_image_placement_state old_state;
        boxrig_image_placement_state new_state;
        boxrig_image_placement_view resolved;

        if (!placement->relative_placement) {
            continue;
        }
        old_state = *placement;
        if (!resolve_relative_geometry(
                terminal,
                placement,
                protocol->cell_pixel_width,
                protocol->cell_pixel_height,
                &resolved)) {
            continue;
        }
        placement->view = resolved;
        new_state = *placement;
        if (old_state.relative_parent_visible !=
                new_state.relative_parent_visible ||
            old_state.viewport_anchored != new_state.viewport_anchored ||
            old_state.anchor_line != new_state.anchor_line ||
            old_state.anchor_column != new_state.anchor_column ||
            !placement_view_geometry_equal(
                &old_state.view,
                &new_state.view)) {
            *placement = old_state;
            mark_placement_dirty(
                terminal,
                placement,
                protocol->cell_pixel_width,
                protocol->cell_pixel_height);
            *placement = new_state;
            mark_placement_dirty(
                terminal,
                placement,
                protocol->cell_pixel_width,
                protocol->cell_pixel_height);
            changed = true;
        }
    }
    if (changed) {
        increment_revision(protocol);
        for (index = 0; index < protocol->placement_count; index++) {
            if (protocol->placements[index]->relative_placement) {
                protocol->placements[index]->view.revision =
                    protocol->revision;
            }
        }
        for (index = 0;
             index < protocol->placeholder_cell_capacity;
             index++) {
            if (protocol->placeholder_instances[index].matched) {
                protocol->placeholder_instances[index].view.revision =
                    protocol->revision;
            }
        }
    }
}

static bool placement_visible_cell_bounds(
    const boxrig_terminal *terminal,
    const boxrig_image_placement_state *placement,
    size_t *top_out,
    size_t *bottom_out,
    size_t *left_out,
    size_t *right_out)
{
    const boxrig_image_placement_view *view = &placement->view;
    uint32_t cell_width = terminal->graphics_protocol.cell_pixel_width;
    uint32_t cell_height = terminal->graphics_protocol.cell_pixel_height;
    int64_t drawing_x;
    int64_t pixel_x;
    int64_t pixel_end_x;
    int64_t projected_anchor;
    int64_t drawing_y;
    int64_t pixel_y;
    int64_t pixel_end_y;

    if (!placement_is_active(terminal, placement) || cell_width == 0 ||
        cell_height == 0 ||
        !checked_signed_cell_pixel(
            placement->anchor_column,
            cell_width,
            view->cell_x_offset,
            &drawing_x) ||
        !placement_projected_anchor(
            terminal,
            placement,
            &projected_anchor) ||
        !checked_signed_cell_pixel(
            projected_anchor,
            cell_height,
            view->cell_y_offset,
            &drawing_y) ||
        view->clip_x > (size_t)INT64_MAX ||
        drawing_x > INT64_MAX - (int64_t)view->clip_x ||
        drawing_y > INT64_MAX - (int64_t)view->clip_y) {
        return false;
    }
    pixel_x = drawing_x + (int64_t)view->clip_x;
    pixel_y = drawing_y + (int64_t)view->clip_y;
    if (pixel_x < 0 || pixel_y < 0 ||
        view->clip_width > (size_t)(INT64_MAX - pixel_x) ||
        view->clip_height > (size_t)(INT64_MAX - pixel_y)) {
        return false;
    }
    pixel_end_x = pixel_x + view->clip_width;
    pixel_end_y = pixel_y + (int64_t)view->clip_height;
    *top_out = (size_t)pixel_y / cell_height;
    *bottom_out = divide_rounding_up((size_t)pixel_end_y, cell_height);
    *left_out = (size_t)pixel_x / cell_width;
    *right_out = divide_rounding_up((size_t)pixel_end_x, cell_width);
    return *top_out < *bottom_out && *left_out < *right_out;
}

static bool placement_intersects_cell(
    const boxrig_terminal *terminal,
    const boxrig_image_placement_state *placement,
    size_t row,
    size_t column)
{
    size_t top;
    size_t bottom;
    size_t left;
    size_t right;

    return placement_visible_cell_bounds(
               terminal,
               placement,
               &top,
               &bottom,
               &left,
               &right) &&
           row >= top && row < bottom && column >= left && column < right;
}

static bool delete_resource_is_selected(
    const boxrig_kitty_graphics_transaction_state *transaction,
    const boxrig_image_resource_state *resource)
{
    size_t index;

    for (index = 0; index < transaction->delete_resource_count; index++) {
        if (transaction->delete_resources[index] == resource) {
            return true;
        }
    }
    return false;
}

static void select_delete_resource(
    boxrig_kitty_graphics_transaction_state *transaction,
    boxrig_image_resource_state *resource)
{
    if (!delete_resource_is_selected(transaction, resource)) {
        transaction->delete_resources[transaction->delete_resource_count] =
            resource;
        transaction->delete_resource_count++;
    }
}

static bool placement_matches_delete_location(
    const boxrig_terminal *terminal,
    const boxrig_image_placement_state *placement,
    const boxrig_kitty_graphics_command *command,
    uint8_t selector)
{
    size_t row;
    size_t column;
    size_t top;
    size_t bottom;
    size_t left;
    size_t right;

    if (!placement_is_active(terminal, placement)) {
        return false;
    }
    if (selector == (uint8_t)'a') {
        return true;
    }
    if (selector == (uint8_t)'c') {
        return placement_intersects_cell(
            terminal,
            placement,
            terminal->screen.active->cursor.row,
            terminal->screen.active->cursor.column);
    }
    if (selector == (uint8_t)'p' || selector == (uint8_t)'q') {
        row = (size_t)command->source_y - 1;
        column = (size_t)command->source_x - 1;
        return (selector != (uint8_t)'q' ||
                placement->view.z_index == command->z_index) &&
               placement_intersects_cell(
                   terminal,
                   placement,
                   row,
                   column);
    }
    if (selector == (uint8_t)'z') {
        return placement->view.z_index == command->z_index;
    }
    if (!placement_visible_cell_bounds(
            terminal,
            placement,
            &top,
            &bottom,
            &left,
            &right)) {
        return false;
    }
    if (selector == (uint8_t)'x') {
        column = (size_t)command->source_x - 1;
        return column >= left && column < right;
    }
    if (selector == (uint8_t)'y') {
        row = (size_t)command->source_y - 1;
        return row >= top && row < bottom;
    }
    return false;
}

static void prepare_delete(
    boxrig_terminal *terminal,
    boxrig_kitty_graphics_transaction_state *transaction)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_kitty_graphics_command *command = &transaction->command;
    boxrig_image_resource_state *identity_resource = NULL;
    uint8_t selector = command->delete_action;
    size_t index;
    bool identity_selector;
    bool range_selector;

    if (selector >= (uint8_t)'A' && selector <= (uint8_t)'Z') {
        selector = (uint8_t)(selector - (uint8_t)'A' + (uint8_t)'a');
    }
    if (selector == (uint8_t)'f') {
        bool by_id = command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID);
        boxrig_image_resource_state *resource = by_id
            ? find_image_by_id(protocol, command->image_id)
            : find_latest_image_by_number(protocol, command->image_number);

        if (resource == NULL) {
            transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NOT_FOUND;
            return;
        }
        command->image_id = resource->view.image_id;
        if (resource->frame_count != 0 || command->delete_action == (uint8_t)'f') {
            size_t frame = command->rows == 0 ? 1 : command->rows;

            if (frame > resource->frame_count + 1) {
                frame = resource->frame_count + 1;
            }
            if (resource->frame_count != 0 && resource->current_frame == frame &&
                !ensure_image_dirty_storage(terminal)) {
                transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
                return;
            }
            transaction->animation_resource = resource;
            transaction->prepared_animation_frame = frame;
            return;
        }
        /* Uppercase F on the sole frame removes the entire image, including
         * placements on inactive screens and relative descendants. */
        selector = by_id ? (uint8_t)'i' : (uint8_t)'n';
    }
    identity_selector = selector == (uint8_t)'i' ||
                        selector == (uint8_t)'n';
    range_selector = selector == (uint8_t)'r';
    if (protocol->placement_count != 0) {
        transaction->delete_placements = malloc(
            protocol->placement_count *
            sizeof(*transaction->delete_placements));
        if (transaction->delete_placements == NULL) {
            transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
            return;
        }
    }
    if (protocol->image_count != 0) {
        transaction->delete_resources = malloc(
            protocol->image_count * sizeof(*transaction->delete_resources));
        if (transaction->delete_resources == NULL) {
            transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
            return;
        }
    }
    if (selector == (uint8_t)'i') {
        identity_resource = find_image_by_id(protocol, command->image_id);
    } else if (selector == (uint8_t)'n') {
        identity_resource = find_latest_image_by_number(
            protocol,
            command->image_number);
        if (identity_resource != NULL) {
            command->image_id = identity_resource->view.image_id;
        }
    }
    if (identity_selector && identity_resource == NULL) {
        transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NOT_FOUND;
        return;
    }
    if (identity_resource != NULL) {
        select_delete_resource(transaction, identity_resource);
    }
    if (range_selector) {
        for (index = 0; index < protocol->image_count; index++) {
            boxrig_image_resource_state *resource = protocol->images[index];

            if (resource->view.image_id >= command->source_x &&
                resource->view.image_id <= command->source_y) {
                select_delete_resource(transaction, resource);
            }
        }
        if (transaction->delete_resource_count == 0) {
            transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NOT_FOUND;
            return;
        }
    }
    for (index = 0; index < protocol->placement_count; index++) {
        boxrig_image_placement_state *placement = protocol->placements[index];
        bool selected;

        if (identity_selector) {
            selected = placement->resource == identity_resource &&
                       (!command_has(
                            command,
                            BOXRIG_KITTY_GRAPHICS_FIELD_PLACEMENT_ID) ||
                        placement->view.placement_id == command->placement_id);
        } else if (range_selector) {
            selected = delete_resource_is_selected(
                transaction,
                placement->resource);
        } else {
            selected = placement_matches_delete_location(
                terminal,
                placement,
                command,
                selector);
        }
        if (!selected) {
            continue;
        }
        transaction->delete_placements[
            transaction->delete_placement_count] = placement;
        transaction->delete_placement_count++;
        select_delete_resource(transaction, placement->resource);
    }
    if (transaction->delete_placement_count > 1) {
        size_t source_index;
        size_t destination_index = 0;

        for (source_index = 0;
             source_index < transaction->delete_placement_count;
             source_index++) {
            boxrig_image_placement_state *placement =
                transaction->delete_placements[source_index];
            boxrig_image_placement_state *ancestor = placement->parent;
            bool ancestor_selected = false;

            while (ancestor != NULL && !ancestor_selected) {
                size_t candidate_index;

                for (candidate_index = 0;
                     candidate_index < transaction->delete_placement_count;
                     candidate_index++) {
                    if (transaction->delete_placements[candidate_index] ==
                        ancestor) {
                        ancestor_selected = true;
                        break;
                    }
                }
                ancestor = ancestor->parent;
            }
            if (!ancestor_selected) {
                transaction->delete_placements[destination_index] = placement;
                destination_index++;
            }
        }
        transaction->delete_placement_count = destination_index;
    }
    if (identity_selector &&
        command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_PLACEMENT_ID) &&
        transaction->delete_placement_count == 0) {
        transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NOT_FOUND;
    }
}

static boxrig_kitty_graphics_result prepare_placement(
    boxrig_terminal *terminal,
    boxrig_kitty_graphics_transaction_state *transaction,
    boxrig_image_resource_state *resource,
    const boxrig_kitty_graphics_command *command,
    size_t planned_resource_removals,
    bool replace_pair)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_image_placement_state *placement;
    boxrig_image_placement_state *replacement = NULL;
    boxrig_image_placement_state *parent = NULL;
    size_t source_x = command->source_x;
    size_t source_y = command->source_y;
    size_t source_width;
    size_t source_height;
    size_t remaining_count;
    size_t new_depth = 0;
    size_t virtual_box_width;
    size_t virtual_box_height;
    size_t virtual_fit_width;
    size_t virtual_fit_height;

    if (protocol->cell_pixel_width == 0 ||
        protocol->cell_pixel_height == 0) {
        return BOXRIG_KITTY_GRAPHICS_RESULT_NO_CELL_GEOMETRY;
    }
    if (source_x >= resource->view.width ||
        source_y >= resource->view.height) {
        return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    source_width = resource->view.width - source_x;
    source_height = resource->view.height - source_y;
    if (command->source_width != 0 && command->source_width < source_width) {
        source_width = command->source_width;
    }
    if (command->source_height != 0 &&
        command->source_height < source_height) {
        source_height = command->source_height;
    }
    if (source_width == 0 || source_height == 0) {
        return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (replace_pair && resource->view.image_id != 0 &&
        command->placement_id != 0) {
        replacement = find_placement_pair(
            protocol,
            resource->view.image_id,
            command->placement_id);
    }
    if (command_has(
            command,
            BOXRIG_KITTY_GRAPHICS_FIELD_PARENT_IMAGE_ID)) {
        parent = find_placement_pair(
            protocol,
            command->parent_image_id,
            command->parent_placement_id);
        if (parent == NULL) {
            return BOXRIG_KITTY_GRAPHICS_RESULT_NO_PARENT;
        }
        if (replacement != NULL &&
            placement_chain_contains(parent, replacement)) {
            return BOXRIG_KITTY_GRAPHICS_RESULT_CYCLE;
        }
        new_depth = placement_depth(parent) + 1;
        if (!replacement_subtree_fits_depth(
                protocol,
                replacement,
                new_depth)) {
            return BOXRIG_KITTY_GRAPHICS_RESULT_TOO_DEEP;
        }
    } else if (!replacement_subtree_fits_depth(
                   protocol,
                   replacement,
                   0)) {
        return BOXRIG_KITTY_GRAPHICS_RESULT_TOO_DEEP;
    }
    placement = calloc(1, sizeof(*placement));
    if (placement == NULL) {
        return BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
    }
    placement->resource = resource;
    placement->view.image_handle = resource->view.handle;
    placement->view.kind = parent == NULL
                               ? BOXRIG_IMAGE_PLACEMENT_NORMAL
                               : BOXRIG_IMAGE_PLACEMENT_RELATIVE;
    placement->view.image_id = resource->view.image_id;
    placement->view.image_number = resource->view.image_number;
    placement->view.placement_id = resource->view.image_id == 0
                                       ? 0
                                       : command->placement_id;
    placement->view.anchor_row = terminal->screen.active->cursor.row;
    placement->view.anchor_column = terminal->screen.active->cursor.column;
    placement->anchor_line = (int64_t)terminal->screen.active->cursor.row;
    placement->anchor_column =
        (int64_t)terminal->screen.active->cursor.column;
    placement->parent = parent;
    placement->relative_x_offset = command->relative_x_offset;
    placement->relative_y_offset = command->relative_y_offset;
    placement->clip_cell_bottom = SIZE_MAX;
    placement->clip_cell_right = SIZE_MAX;
    placement->view.source_x = source_x;
    placement->view.source_y = source_y;
    placement->view.source_width = source_width;
    placement->view.source_height = source_height;
    placement->view.cell_x_offset = command->cell_x_offset;
    placement->view.cell_y_offset = command->cell_y_offset;
    placement->view.z_index = command->z_index;
    placement->requested_columns = command->columns;
    placement->requested_rows = command->rows;
    placement->columns_specified = command->columns != 0;
    placement->rows_specified = command->rows != 0;
    placement->virtual_placement = command->virtual_placement != 0;
    placement->relative_placement = parent != NULL;
    placement->relative_parent_visible = true;
    placement->move_cursor = !placement->virtual_placement && parent == NULL &&
                             command->cursor_movement == 0;
    placement->alternate_buffer = parent != NULL
                                      ? parent->alternate_buffer
                                      : terminal->screen.alternate_active;
    if (placement->virtual_placement) {
        if (!calculate_virtual_fit(
                placement,
                protocol->cell_pixel_width,
                protocol->cell_pixel_height,
                &virtual_box_width,
                &virtual_box_height,
                &virtual_fit_width,
                &virtual_fit_height)) {
            destroy_placement(placement);
            return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
        }
        if (!ensure_placeholder_storage(terminal)) {
            destroy_placement(placement);
            return BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
        }
        placement->view.anchor_row = 0;
        placement->view.anchor_column = 0;
        placement->anchor_line = 0;
        placement->anchor_column = 0;
        placement->view.columns = placement->requested_columns;
        placement->view.rows = placement->requested_rows;
        placement->view.destination_pixel_width = virtual_fit_width;
        placement->view.destination_pixel_height = virtual_fit_height;
        placement->view.clip_width = 0;
        placement->view.clip_height = 0;
    } else if (!(parent != NULL
                     ? resolve_relative_geometry(
                           terminal,
                           placement,
                           protocol->cell_pixel_width,
                           protocol->cell_pixel_height,
                           &placement->view)
                     : resolve_placement_geometry(
                           terminal,
                           placement,
                           protocol->cell_pixel_width,
                           protocol->cell_pixel_height,
                           &placement->view))) {
        destroy_placement(placement);
        return BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (planned_resource_removals > protocol->placement_count) {
        destroy_placement(placement);
        return BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
    }
    remaining_count = protocol->placement_count - planned_resource_removals;
    if (replacement != NULL) {
        if (remaining_count == 0) {
            destroy_placement(placement);
            return BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
        }
        remaining_count--;
    }
    if (remaining_count >= protocol->placement_capacity ||
        !ensure_placement_slot(protocol, remaining_count + 1) ||
        !ensure_image_dirty_storage(terminal)) {
        destroy_placement(placement);
        return BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
    }
    transaction->prepared_placement = placement;
    transaction->replacement_placement = replacement;
    return BOXRIG_KITTY_GRAPHICS_RESULT_OK;
}

static void prepare_resource(
    boxrig_terminal *terminal,
    boxrig_kitty_graphics_upload_state *upload)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_image_resource_state *replacement = NULL;
    boxrig_image_placement_state *protected_parent = NULL;
    boxrig_image_resource_state *resource;
    boxrig_image_resource_state **eviction_resources;
    size_t removed_count;
    size_t final_count;
    size_t eviction_count;
    size_t removed_placement_count;
    uint32_t assigned_image_id = upload->command.image_id;
    uint32_t next_image_id = protocol->next_image_id;
    boxrig_image_handle assigned_handle;
    boxrig_image_handle next_image_handle;
    bool generated_image_id = false;

    if (command_has(
            &upload->command,
            BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER)) {
        if (!select_generated_image_id(
                protocol,
                &assigned_image_id,
                &next_image_id)) {
            upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
            return;
        }
        generated_image_id = true;
    } else if (command_has(
                   &upload->command,
                   BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID) &&
               upload->command.image_id != 0) {
        replacement = find_image_by_id(protocol, upload->command.image_id);
    }
    if (command_has(
            &upload->command,
            BOXRIG_KITTY_GRAPHICS_FIELD_PARENT_IMAGE_ID)) {
        protected_parent = find_placement_pair(
            protocol,
            upload->command.parent_image_id,
            upload->command.parent_placement_id);
        if (protected_parent == NULL ||
            placement_chain_uses_resource(
                protected_parent,
                replacement)) {
            upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_PARENT;
            return;
        }
    }
    if (!plan_storage_evictions(
            terminal,
            replacement,
            protected_parent,
            upload->rgba_length,
            &eviction_resources,
            &eviction_count,
            &removed_count,
            &removed_placement_count)) {
        upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
        return;
    }
    upload->eviction_resources = eviction_resources;
    upload->eviction_count = eviction_count;
    upload->planned_removed_placement_count = removed_placement_count;
    if (removed_count > protocol->image_count) {
        upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
        return;
    }
    final_count = protocol->image_count - removed_count + 1;
    if (final_count > protocol->image_capacity ||
        !ensure_image_slot(protocol, final_count) ||
        !select_image_handle(
            protocol,
            &assigned_handle,
            &next_image_handle)) {
        upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
        return;
    }
    resource = calloc(1, sizeof(*resource));
    if (resource == NULL) {
        upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
        return;
    }
    resource->rgba = upload->rgba;
    upload->rgba = NULL;
    release_decode(upload, upload->rgba_length);
    upload->command.image_id = assigned_image_id;
    resource->view.handle = assigned_handle;
    resource->view.image_id = assigned_image_id;
    resource->view.image_number = upload->command.image_number;
    resource->view.width = upload->width;
    resource->view.height = upload->height;
    resource->view.stride = upload->stride;
    resource->view.rgba = resource->rgba;
    resource->view.rgba_length = upload->rgba_length;
    resource->root_gap_milliseconds = 40;
    resource->animation_loop_count = 1;
    resource->animation_remaining_wraps = UINT32_MAX;
    resource->animation_playback_state =
        BOXRIG_ANIMATION_PLAYBACK_STOPPED;
    resource->transient = (upload->command.usage_hints & UINT32_C(1)) != 0;
    resource->root_transient = resource->transient;
    resource->current_frame = 1;
    resource->animation_frame = 1;
    resource->has_image_number = command_has(
        &upload->command,
        BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER);
    upload->prepared_resource = resource;
    upload->replacement_resource = replacement;
    upload->generated_image_id = generated_image_id;
    upload->next_image_handle_after_commit = next_image_handle;
    upload->next_image_id_after_commit = next_image_id;
}

static uint8_t *frame_rgba(
    const boxrig_image_resource_state *resource,
    size_t frame_number)
{
    if (frame_number == 1) {
        return resource->rgba;
    }
    if (frame_number < 2 || frame_number - 2 >= resource->frame_count) {
        return NULL;
    }
    return resource->frames[frame_number - 2].rgba;
}

static int32_t frame_gap(
    const boxrig_image_resource_state *resource,
    size_t frame_number)
{
    return frame_number == 1
               ? resource->root_gap_milliseconds
               : resource->frames[frame_number - 2].gap_milliseconds;
}

static bool frame_is_transient(
    const boxrig_image_resource_state *resource,
    size_t frame_number)
{
    return frame_number == 1
               ? resource->root_transient
               : resource->frames[frame_number - 2].transient;
}

static bool animation_quota_satisfied(
    const boxrig_kitty_graphics_protocol_state *protocol,
    size_t new_length,
    size_t freed_length,
    size_t freed_frames)
{
    size_t retained_length;
    size_t retained_frames;

    if (freed_length > protocol->animation_storage_usage ||
        freed_frames > protocol->animation_frame_count ||
        new_length > protocol->animation_storage_capacity) {
        return false;
    }
    retained_length = protocol->animation_storage_usage - freed_length;
    retained_frames = protocol->animation_frame_count - freed_frames;
    return retained_length <=
               protocol->animation_storage_capacity - new_length &&
           retained_frames < protocol->frame_capacity;
}

static bool plan_animation_evictions(
    boxrig_terminal *terminal,
    const boxrig_image_resource_state *protected_resource,
    size_t new_length,
    boxrig_image_resource_state ***resources_out,
    size_t *resource_count_out)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_image_resource_state **resources = NULL;
    size_t resource_count = 0;
    size_t freed_length = 0;
    size_t freed_frames = 0;
    unsigned int priority;
    size_t index;

    *resources_out = NULL;
    *resource_count_out = 0;
    if (protocol->animation_storage_usage >
            protocol->animation_storage_capacity ||
        protocol->animation_frame_count > protocol->frame_capacity ||
        new_length > protocol->animation_storage_capacity ||
        protocol->frame_capacity == 0) {
        return false;
    }
    if (animation_quota_satisfied(
            protocol,
            new_length,
            freed_length,
            freed_frames)) {
        return true;
    }
    if (protocol->image_count != 0) {
        resources = malloc(protocol->image_count * sizeof(*resources));
        if (resources == NULL) {
            return false;
        }
    }
    /* Images remain creation-serial ordered, so each priority group evicts
     * its oldest complete additional-frame set first. */
    for (priority = 0;
         priority <= 5 &&
         !animation_quota_satisfied(
             protocol,
             new_length,
             freed_length,
             freed_frames);
         priority++) {
        for (index = 0;
             index < protocol->image_count &&
             !animation_quota_satisfied(
                 protocol,
                 new_length,
                 freed_length,
                 freed_frames);
             index++) {
            boxrig_image_resource_state *candidate = protocol->images[index];
            size_t candidate_length;

            if (candidate == protected_resource ||
                candidate->frame_count == 0 ||
                eviction_priority(terminal, candidate) != priority ||
                (candidate->view.rgba_length != 0 &&
                 candidate->frame_count >
                     SIZE_MAX / candidate->view.rgba_length)) {
                continue;
            }
            candidate_length =
                candidate->frame_count * candidate->view.rgba_length;
            if (candidate_length > SIZE_MAX - freed_length ||
                candidate->frame_count > SIZE_MAX - freed_frames) {
                free(resources);
                return false;
            }
            resources[resource_count] = candidate;
            resource_count++;
            freed_length += candidate_length;
            freed_frames += candidate->frame_count;
        }
    }
    if (!animation_quota_satisfied(
            protocol,
            new_length,
            freed_length,
            freed_frames)) {
        free(resources);
        return false;
    }
    for (index = 0; index < resource_count; index++) {
        if (resources[index]->current_frame > 1 &&
            !ensure_image_dirty_storage(terminal)) {
            free(resources);
            return false;
        }
    }
    *resources_out = resources;
    *resource_count_out = resource_count;
    return true;
}

static void blend_straight_alpha(uint8_t *destination, const uint8_t *source)
{
    uint32_t source_alpha = source[3];
    uint32_t destination_alpha = destination[3];
    uint32_t inverse_alpha = 255 - source_alpha;
    uint32_t output_alpha_numerator =
        source_alpha * 255 + destination_alpha * inverse_alpha;
    size_t channel;

    if (output_alpha_numerator == 0) {
        memset(destination, 0, 4);
        return;
    }
    for (channel = 0; channel < 3; channel++) {
        uint32_t numerator =
            (uint32_t)source[channel] * source_alpha * 255 +
            (uint32_t)destination[channel] * destination_alpha *
                inverse_alpha;

        destination[channel] = (uint8_t)(
            (numerator + output_alpha_numerator / 2) /
            output_alpha_numerator);
    }
    destination[3] = (uint8_t)(
        (output_alpha_numerator + 127) / 255);
}

static void compose_frame_rectangle(
    uint8_t *canvas,
    const boxrig_image_resource_state *resource,
    const boxrig_kitty_graphics_upload_state *upload)
{
    size_t row;

    for (row = 0; row < upload->height; row++) {
        uint8_t *destination =
            canvas + ((size_t)upload->command.source_y + row) *
                         resource->view.stride +
            (size_t)upload->command.source_x * 4;
        const uint8_t *source = upload->rgba + row * upload->stride;
        size_t column;

        if (upload->command.cell_x_offset == 1) {
            memcpy(destination, source, (size_t)upload->width * 4);
            continue;
        }
        for (column = 0; column < upload->width; column++) {
            blend_straight_alpha(
                destination + column * 4,
                source + column * 4);
        }
    }
}

static void fill_frame_color(
    uint8_t *canvas,
    size_t length,
    uint32_t color)
{
    uint8_t rgba[4];
    size_t offset;

    rgba[0] = (uint8_t)(color >> 24);
    rgba[1] = (uint8_t)(color >> 16);
    rgba[2] = (uint8_t)(color >> 8);
    rgba[3] = (uint8_t)color;
    for (offset = 0; offset < length; offset += 4) {
        memcpy(canvas + offset, rgba, sizeof(rgba));
    }
}

static void prepare_frame(
    boxrig_terminal *terminal,
    boxrig_kitty_graphics_upload_state *upload,
    boxrig_image_resource_state *selected_resource)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_kitty_graphics_command *command = &upload->command;
    boxrig_image_resource_state *resource = selected_resource;
    uint8_t *base;
    uint8_t *canvas;
    size_t frame_number;
    size_t frame_total;
    bool editing = command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_ROWS);
    bool has_gap = command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_Z_INDEX);
    bool command_transient = (command->usage_hints & UINT32_C(1)) != 0;
    bool inherited_transient = false;

    if (protocol->animation_storage_capacity == 0 ||
        protocol->frame_capacity == 0) {
        upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_UNSUPPORTED_FRAMES;
        return;
    }
    if (resource == NULL) {
        resource = command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID)
                       ? find_image_by_id(protocol, command->image_id)
                       : find_latest_image_by_number(
                             protocol,
                             command->image_number);
    }
    if (resource == NULL) {
        upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NOT_FOUND;
        return;
    }
    command->image_id = resource->view.image_id;
    frame_total = resource->frame_count + 1;
    frame_number = editing ? command->rows : frame_total + 1;
    if (editing && (frame_number == 0 || frame_number > frame_total)) {
        upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NOT_FOUND;
        return;
    }
    if ((size_t)command->source_x > resource->view.width ||
        (size_t)command->source_y > resource->view.height ||
        upload->width >
            resource->view.width - (size_t)command->source_x ||
        upload->height >
            resource->view.height - (size_t)command->source_y) {
        upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
        return;
    }
    canvas = malloc(resource->view.rgba_length);
    if (canvas == NULL) {
        upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
        return;
    }
    if (editing) {
        base = frame_rgba(resource, frame_number);
        memcpy(canvas, base, resource->view.rgba_length);
        inherited_transient = frame_is_transient(resource, frame_number);
        upload->prepared_frame_gap =
            has_gap && command->z_index != 0
                ? command->z_index
                : frame_gap(resource, frame_number);
    } else if (command_has(
                   command,
                   BOXRIG_KITTY_GRAPHICS_FIELD_COLUMNS)) {
        base = frame_rgba(resource, command->columns);
        if (base == NULL) {
            free(canvas);
            upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NOT_FOUND;
            return;
        }
        memcpy(canvas, base, resource->view.rgba_length);
        inherited_transient = frame_is_transient(resource, command->columns);
        upload->prepared_frame_gap =
            has_gap && command->z_index != 0 ? command->z_index : 40;
    } else {
        fill_frame_color(
            canvas,
            resource->view.rgba_length,
            command->cell_y_offset);
        upload->prepared_frame_gap =
            has_gap && command->z_index != 0 ? command->z_index : 40;
    }
    compose_frame_rectangle(canvas, resource, upload);
    if (!editing) {
        if (!plan_animation_evictions(
                terminal,
                resource,
                resource->view.rgba_length,
                &upload->frame_eviction_resources,
                &upload->frame_eviction_count)) {
            free(canvas);
            upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
            return;
        }
        if (resource->frame_count == SIZE_MAX ||
            resource->frame_count + 1 >
                SIZE_MAX / sizeof(*upload->prepared_frames)) {
            free(canvas);
            upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
            return;
        }
        upload->prepared_frames = malloc(
            (resource->frame_count + 1) *
            sizeof(*upload->prepared_frames));
        if (upload->prepared_frames == NULL) {
            free(canvas);
            upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
            return;
        }
        if (resource->frame_count != 0) {
            memcpy(
                upload->prepared_frames,
                resource->frames,
                resource->frame_count * sizeof(*resource->frames));
        }
        upload->prepared_frames[resource->frame_count].rgba = canvas;
        upload->prepared_frames[resource->frame_count].gap_milliseconds =
            upload->prepared_frame_gap;
        upload->prepared_frames[resource->frame_count].transient =
            inherited_transient || command_transient;
    } else if (resource->current_frame == frame_number &&
               !ensure_image_dirty_storage(terminal)) {
        free(canvas);
        upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
        return;
    }
    upload->frame_resource = resource;
    upload->prepared_frame_rgba = canvas;
    upload->prepared_frame_number = frame_number;
    upload->prepared_frame_is_new = !editing;
    upload->prepared_frame_transient =
        inherited_transient || command_transient;
}

static void increment_revision(boxrig_kitty_graphics_protocol_state *protocol)
{
    protocol->revision++;
    if (protocol->revision == 0) {
        protocol->revision = 1;
    }
}

static void mark_resource_dirty(
    boxrig_terminal *terminal,
    const boxrig_image_resource_state *resource)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    size_t index;

    for (index = 0; index < protocol->placement_count; index++) {
        if (protocol->placements[index]->resource == resource) {
            mark_placement_dirty(
                terminal,
                protocol->placements[index],
                protocol->cell_pixel_width,
                protocol->cell_pixel_height);
        }
    }
    for (index = 0; index < protocol->placeholder_cell_capacity; index++) {
        const boxrig_placeholder_instance_state *instance =
            &protocol->placeholder_instances[index];

        if (instance->matched && instance->view.clip_width != 0 &&
            instance->view.clip_height != 0 &&
            instance->view.image_handle == resource->view.handle) {
            mark_image_interval(
                terminal,
                instance->view.visual_row,
                instance->view.visual_column,
                instance->view.visual_column + 1);
        }
    }
}

static uint32_t animation_initial_remaining_wraps(uint32_t loop_count)
{
    return loop_count <= 1 ? UINT32_MAX : loop_count - 1;
}

static bool animation_playback_running(
    const boxrig_image_resource_state *resource)
{
    return resource->animation_playback_state ==
               BOXRIG_ANIMATION_PLAYBACK_LOADING ||
           resource->animation_playback_state ==
               BOXRIG_ANIMATION_PLAYBACK_LOOPING;
}

static uint64_t animation_saturating_add(
    uint64_t milliseconds,
    uint64_t duration)
{
    return duration > UINT64_MAX - milliseconds
               ? UINT64_MAX
               : milliseconds + duration;
}

static void set_initial_animation_deadline(
    const boxrig_kitty_graphics_protocol_state *protocol,
    boxrig_image_resource_state *resource)
{
    int32_t gap = frame_gap(resource, resource->animation_frame);
    /* A newly started gapless cursor waits for the next increasing host tick,
     * avoiding a zero-timeout loop before normal same-deadline traversal. */
    uint64_t duration = gap > 0 ? (uint64_t)(uint32_t)gap : UINT64_C(1);

    resource->animation_deadline_milliseconds = animation_saturating_add(
        protocol->monotonic_time_milliseconds,
        duration);
    resource->animation_deadline_set = true;
}

static boxrig_image_resource_state *find_command_image(
    const boxrig_kitty_graphics_protocol_state *protocol,
    const boxrig_kitty_graphics_command *command)
{
    if (command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID)) {
        return find_image_by_id(protocol, command->image_id);
    }
    return find_latest_image_by_number(protocol, command->image_number);
}

static void prepare_animation_control(
    boxrig_terminal *terminal,
    boxrig_kitty_graphics_transaction_state *transaction)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_kitty_graphics_command *command = &transaction->command;
    boxrig_image_resource_state *resource =
        find_command_image(protocol, command);
    bool sets_current = command_has(
        command,
        BOXRIG_KITTY_GRAPHICS_FIELD_COLUMNS);
    bool sets_gap = command_has(
        command,
        BOXRIG_KITTY_GRAPHICS_FIELD_ROWS);

    if (resource == NULL) {
        transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NOT_FOUND;
        return;
    }
    if (sets_current && frame_rgba(resource, command->columns) == NULL) {
        transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NOT_FOUND;
        return;
    }
    if (sets_gap && frame_rgba(resource, command->rows) == NULL) {
        transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NOT_FOUND;
        return;
    }
    if (sets_current && resource->current_frame != command->columns &&
        !ensure_image_dirty_storage(terminal)) {
        transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
        return;
    }
    command->image_id = resource->view.image_id;
    transaction->animation_resource = resource;
    transaction->animation_sets_current = sets_current;
    transaction->animation_sets_gap = sets_gap;
    transaction->animation_sets_loops =
        command_has(command, BOXRIG_KITTY_GRAPHICS_FIELD_DATA_HEIGHT) &&
        command->data_height != 0;
    transaction->animation_sets_playback_state = command_has(
        command,
        BOXRIG_KITTY_GRAPHICS_FIELD_DATA_WIDTH);
    transaction->prepared_current_frame = command->columns;
    transaction->prepared_gap_frame = command->rows;
    transaction->prepared_gap_milliseconds = command->z_index;
    transaction->prepared_loop_count = command->data_height;
    transaction->prepared_playback_state = (uint8_t)command->data_width;
}

static bool frame_rectangles_overlap(
    size_t source_x,
    size_t source_y,
    size_t destination_x,
    size_t destination_y,
    size_t width,
    size_t height)
{
    return source_x < destination_x + width &&
           destination_x < source_x + width &&
           source_y < destination_y + height &&
           destination_y < source_y + height;
}

static void prepare_frame_composition(
    boxrig_terminal *terminal,
    boxrig_kitty_graphics_transaction_state *transaction)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_kitty_graphics_command *command = &transaction->command;
    boxrig_image_resource_state *resource =
        find_command_image(protocol, command);
    const uint8_t *source;
    const uint8_t *destination;
    uint8_t *canvas;
    size_t source_x = command->source_x;
    size_t source_y = command->source_y;
    size_t destination_x = command->cell_x_offset;
    size_t destination_y = command->cell_y_offset;
    size_t width;
    size_t height;
    size_t row;

    if (resource == NULL) {
        transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NOT_FOUND;
        return;
    }
    width = command->source_width == 0
                ? resource->view.width
                : command->source_width;
    height = command->source_height == 0
                 ? resource->view.height
                 : command->source_height;
    source = frame_rgba(resource, command->rows);
    destination = frame_rgba(resource, command->columns);
    if (source == NULL || destination == NULL) {
        transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NOT_FOUND;
        return;
    }
    if (source_x > resource->view.width ||
        source_y > resource->view.height ||
        destination_x > resource->view.width ||
        destination_y > resource->view.height ||
        width > resource->view.width - source_x ||
        height > resource->view.height - source_y ||
        width > resource->view.width - destination_x ||
        height > resource->view.height - destination_y ||
        (command->rows == command->columns &&
         frame_rectangles_overlap(
             source_x,
             source_y,
             destination_x,
             destination_y,
             width,
             height))) {
        transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
        return;
    }
    if (resource->current_frame == command->columns &&
        !ensure_image_dirty_storage(terminal)) {
        transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
        return;
    }
    canvas = malloc(resource->view.rgba_length);
    if (canvas == NULL) {
        transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
        return;
    }
    memcpy(canvas, destination, resource->view.rgba_length);
    for (row = 0; row < height; row++) {
        const uint8_t *source_row =
            source + (source_y + row) * resource->view.stride +
            source_x * 4;
        uint8_t *destination_row =
            canvas + (destination_y + row) * resource->view.stride +
            destination_x * 4;
        size_t column;

        if (command->cursor_movement == 1) {
            memcpy(destination_row, source_row, width * 4);
            continue;
        }
        for (column = 0; column < width; column++) {
            blend_straight_alpha(
                destination_row + column * 4,
                source_row + column * 4);
        }
    }
    command->image_id = resource->view.image_id;
    transaction->animation_resource = resource;
    transaction->prepared_animation_rgba = canvas;
    transaction->prepared_animation_frame = command->columns;
    transaction->prepared_animation_transient =
        frame_is_transient(resource, command->rows) ||
        frame_is_transient(resource, command->columns);
}

static void clear_resource_animation_frames(
    boxrig_terminal *terminal,
    boxrig_image_resource_state *resource)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    size_t released_length;
    size_t index;

    if (!multiply_size(
            resource->frame_count,
            resource->view.rgba_length,
            &released_length)) {
        released_length = protocol->animation_storage_usage;
    }
    if (resource->current_frame > 1) {
        mark_resource_dirty(terminal, resource);
        resource->current_frame = 1;
        resource->view.rgba = resource->rgba;
        increment_revision(protocol);
        resource->view.revision = protocol->revision;
    }
    for (index = 0; index < resource->frame_count; index++) {
        free(resource->frames[index].rgba);
    }
    free(resource->frames);
    resource->frames = NULL;
    if (released_length <= protocol->animation_storage_usage) {
        protocol->animation_storage_usage -= released_length;
    } else {
        protocol->animation_storage_usage = 0;
    }
    if (resource->frame_count <= protocol->animation_frame_count) {
        protocol->animation_frame_count -= resource->frame_count;
    } else {
        protocol->animation_frame_count = 0;
    }
    resource->frame_count = 0;
    resource->animation_frame = 1;
    resource->transient = resource->root_transient;
    resource->animation_playback_state =
        BOXRIG_ANIMATION_PLAYBACK_STOPPED;
    resource->animation_remaining_wraps = animation_initial_remaining_wraps(
        resource->animation_loop_count);
    resource->animation_deadline_set = false;
    resource->animation_waiting = false;
}

static void remove_placement_at_direct(
    boxrig_terminal *terminal,
    size_t index)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_image_placement_state *placement = protocol->placements[index];

    mark_placement_dirty(
        terminal,
        placement,
        protocol->cell_pixel_width,
        protocol->cell_pixel_height);
    if (index + 1 < protocol->placement_count) {
        memmove(
            protocol->placements + index,
            protocol->placements + index + 1,
            (protocol->placement_count - index - 1) *
                sizeof(*protocol->placements));
    }
    protocol->placement_count--;
    destroy_placement(placement);
}

static void remove_placement_pointer_direct(
    boxrig_terminal *terminal,
    const boxrig_image_placement_state *placement)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    size_t index;

    for (index = 0; index < protocol->placement_count; index++) {
        if (protocol->placements[index] == placement) {
            remove_placement_at_direct(terminal, index);
            return;
        }
    }
}

static void remove_placement_tree(
    boxrig_terminal *terminal,
    const boxrig_image_placement_state *root,
    bool descendant)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    size_t index = 0;

    while (index < protocol->placement_count) {
        boxrig_image_placement_state *placement = protocol->placements[index];

        if (placement->parent == root) {
            placement->resource->remove_if_orphaned = true;
            remove_placement_tree(terminal, placement, true);
            index = 0;
        } else {
            index++;
        }
    }
    if (descendant) {
        ((boxrig_image_placement_state *)root)
            ->resource->remove_if_orphaned = true;
    }
    remove_placement_pointer_direct(terminal, root);
}

static void remove_orphaned_descendant_resources(
    boxrig_terminal *terminal,
    const boxrig_image_resource_state *protected_resource)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    size_t index = 0;

    while (index < protocol->image_count) {
        boxrig_image_resource_state *resource = protocol->images[index];

        if (resource->remove_if_orphaned &&
            resource != protected_resource &&
            resource_placement_count(protocol, resource) == 0) {
            resource->remove_if_orphaned = false;
            remove_resource_at(terminal, index);
        } else {
            resource->remove_if_orphaned = false;
            index++;
        }
    }
}

static void remove_placement_pointer(
    boxrig_terminal *terminal,
    const boxrig_image_placement_state *placement)
{
    const boxrig_image_resource_state *root_resource = placement->resource;

    remove_placement_tree(terminal, placement, false);
    remove_orphaned_descendant_resources(terminal, root_resource);
}

static void remove_placement_at(
    boxrig_terminal *terminal,
    size_t index)
{
    boxrig_image_placement_state *placement =
        terminal->graphics_protocol.placements[index];

    remove_placement_pointer(terminal, placement);
}

static void remove_resource_placements(
    boxrig_terminal *terminal,
    const boxrig_image_resource_state *resource)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    size_t index = 0;

    while (index < protocol->placement_count) {
        if (protocol->placements[index]->resource == resource) {
            remove_placement_tree(
                terminal,
                protocol->placements[index],
                false);
            index = 0;
        } else {
            index++;
        }
    }
    remove_orphaned_descendant_resources(terminal, resource);
}

static void remove_resource_at(
    boxrig_terminal *terminal,
    size_t index)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_image_resource_state *resource = protocol->images[index];

    remove_resource_placements(terminal, resource);
    for (index = 0; index < protocol->image_count; index++) {
        if (protocol->images[index] == resource) {
            break;
        }
    }
    if (index == protocol->image_count) {
        return;
    }
    if (resource->view.rgba_length <= protocol->storage_usage) {
        protocol->storage_usage -= resource->view.rgba_length;
    } else {
        protocol->storage_usage = 0;
    }
    if (resource->frame_count != 0) {
        size_t animation_length;

        if (multiply_size(
                resource->frame_count,
                resource->view.rgba_length,
                &animation_length) &&
            animation_length <= protocol->animation_storage_usage) {
            protocol->animation_storage_usage -= animation_length;
        } else {
            protocol->animation_storage_usage = 0;
        }
        if (resource->frame_count <= protocol->animation_frame_count) {
            protocol->animation_frame_count -= resource->frame_count;
        } else {
            protocol->animation_frame_count = 0;
        }
    }
    if (index + 1 < protocol->image_count) {
        memmove(
            protocol->images + index,
            protocol->images + index + 1,
            (protocol->image_count - index - 1) * sizeof(*protocol->images));
    }
    protocol->image_count--;
    destroy_resource(resource);
}

static void remove_resource_pointer(
    boxrig_terminal *terminal,
    const boxrig_image_resource_state *resource)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    size_t index;

    for (index = 0; index < protocol->image_count; index++) {
        if (protocol->images[index] == resource) {
            remove_resource_at(terminal, index);
            return;
        }
    }
}

static bool resource_is_committed(
    const boxrig_kitty_graphics_protocol_state *protocol,
    const boxrig_image_resource_state *resource)
{
    size_t index;

    for (index = 0; index < protocol->image_count; index++) {
        if (protocol->images[index] == resource) {
            return true;
        }
    }
    return false;
}

static void commit_frame_delete(
    boxrig_terminal *terminal,
    const boxrig_kitty_graphics_transaction_state *transaction)
{
    boxrig_kitty_graphics_protocol_state *protocol = &terminal->graphics_protocol;
    boxrig_image_resource_state *resource = transaction->animation_resource;
    size_t frame = transaction->prepared_animation_frame;
    size_t index = frame == 1 ? 0 : frame - 2;
    bool visible_removed = resource->current_frame == frame;
    bool cursor_removed = resource->animation_frame == frame;

    if (resource->frame_count == 0) {
        return;
    }
    if (frame == 1) {
        free(resource->rgba);
        resource->rgba = resource->frames[0].rgba;
        resource->root_gap_milliseconds = resource->frames[0].gap_milliseconds;
        resource->root_transient = resource->frames[0].transient;
    } else {
        free(resource->frames[index].rgba);
    }
    memmove(resource->frames + index, resource->frames + index + 1,
        (resource->frame_count - index - 1) * sizeof(*resource->frames));
    resource->frame_count--;
    protocol->animation_storage_usage -= resource->view.rgba_length;
    protocol->animation_frame_count--;
    if (resource->frame_count == 0) {
        free(resource->frames);
        resource->frames = NULL;
    }
    resource->transient = resource->root_transient;
    for (size_t remaining = 0; remaining < resource->frame_count; remaining++) {
        resource->transient = resource->transient || resource->frames[remaining].transient;
    }
    if (resource->current_frame > frame ||
        resource->current_frame > resource->frame_count + 1) {
        resource->current_frame--;
    }
    if (resource->animation_frame > frame ||
        resource->animation_frame > resource->frame_count + 1) {
        resource->animation_frame--;
    }
    resource->view.rgba = frame_rgba(resource, resource->current_frame);
    if (visible_removed) {
        increment_revision(protocol);
        resource->view.revision = protocol->revision;
        mark_resource_dirty(terminal, resource);
    }
    if (cursor_removed) {
        resource->animation_waiting = false;
        resource->animation_deadline_set = false;
        /* Preserve an unaffected frame's deadline. A replacement receives its
         * own gap; exclusive runtimes anchor it at their next synchronization. */
        if (animation_playback_running(resource) &&
            terminal->animation_clock == NULL && protocol->monotonic_time_set) {
            set_initial_animation_deadline(protocol, resource);
        }
    }
}

static void commit_delete(boxrig_terminal *terminal)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_kitty_graphics_transaction_state *transaction =
        &protocol->transaction;
    bool release_data = transaction->command.delete_action >= (uint8_t)'A' &&
                        transaction->command.delete_action <= (uint8_t)'Z';
    bool changed = transaction->delete_placement_count != 0;
    size_t index;

    if (transaction->animation_resource != NULL) {
        commit_frame_delete(terminal, transaction);
        return;
    }
    for (index = 0; index < transaction->delete_placement_count; index++) {
        remove_placement_pointer(
            terminal,
            transaction->delete_placements[index]);
    }
    if (release_data) {
        for (index = 0; index < transaction->delete_resource_count; index++) {
            boxrig_image_resource_state *resource =
                transaction->delete_resources[index];

            if (resource_is_committed(protocol, resource) &&
                resource_placement_count(protocol, resource) == 0) {
                remove_resource_pointer(terminal, resource);
                changed = true;
            }
        }
    }
    if (changed) {
        increment_revision(protocol);
    }
    refresh_derived_placements(terminal, true);
}

static void commit_planned_evictions(
    boxrig_terminal *terminal,
    boxrig_image_resource_state *const *resources,
    size_t eviction_count)
{
    size_t index;

    for (index = 0; index < eviction_count; index++) {
        remove_resource_pointer(terminal, resources[index]);
    }
}

static void commit_resource(
    boxrig_terminal *terminal,
    boxrig_kitty_graphics_upload_state *upload,
    uint64_t reserved_creation_serial)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_image_resource_state *resource =
        upload->prepared_resource;
    size_t insertion_index;

    remove_resource_pointer(terminal, upload->replacement_resource);
    upload->replacement_resource = NULL;
    commit_planned_evictions(
        terminal,
        upload->eviction_resources,
        upload->eviction_count);
    free(upload->eviction_resources);
    upload->eviction_resources = NULL;
    upload->eviction_count = 0;
    protocol->next_image_handle = upload->next_image_handle_after_commit;
    if (reserved_creation_serial == 0) {
        resource->creation_serial = protocol->next_creation_serial;
        protocol->next_creation_serial = increment_nonzero_u64(
            protocol->next_creation_serial);
    } else {
        resource->creation_serial = reserved_creation_serial;
    }
    if (upload->generated_image_id) {
        protocol->next_image_id = upload->next_image_id_after_commit;
    }
    increment_revision(protocol);
    resource->view.revision = protocol->revision;
    insertion_index = protocol->image_count;
    while (insertion_index != 0 &&
           protocol->images[insertion_index - 1]->creation_serial >
               resource->creation_serial) {
        protocol->images[insertion_index] =
            protocol->images[insertion_index - 1];
        insertion_index--;
    }
    protocol->images[insertion_index] = resource;
    protocol->image_count++;
    protocol->storage_usage += resource->view.rgba_length;
    upload->prepared_resource = NULL;
}

static void commit_frame(
    boxrig_terminal *terminal,
    boxrig_kitty_graphics_upload_state *upload)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_image_resource_state *resource = upload->frame_resource;
    bool current_changed =
        resource->current_frame == upload->prepared_frame_number;
    size_t index;

    for (index = 0; index < upload->frame_eviction_count; index++) {
        clear_resource_animation_frames(
            terminal,
            upload->frame_eviction_resources[index]);
    }
    free(upload->frame_eviction_resources);
    upload->frame_eviction_resources = NULL;
    upload->frame_eviction_count = 0;
    if (upload->prepared_frame_is_new) {
        free(resource->frames);
        resource->frames = upload->prepared_frames;
        upload->prepared_frames = NULL;
        resource->frame_count++;
        protocol->animation_storage_usage += resource->view.rgba_length;
        protocol->animation_frame_count++;
        resource->transient =
            resource->transient || upload->prepared_frame_transient;
        upload->prepared_frame_rgba = NULL;
        return;
    }
    if (upload->prepared_frame_number == 1) {
        free(resource->rgba);
        resource->rgba = upload->prepared_frame_rgba;
        resource->root_gap_milliseconds = upload->prepared_frame_gap;
        resource->root_transient = upload->prepared_frame_transient;
    } else {
        boxrig_animation_frame_state *frame =
            &resource->frames[upload->prepared_frame_number - 2];

        free(frame->rgba);
        frame->rgba = upload->prepared_frame_rgba;
        frame->gap_milliseconds = upload->prepared_frame_gap;
        frame->transient = upload->prepared_frame_transient;
    }
    upload->prepared_frame_rgba = NULL;
    resource->transient =
        resource->transient || upload->prepared_frame_transient;
    if (current_changed) {
        resource->view.rgba = frame_rgba(
            resource,
            resource->current_frame);
        increment_revision(protocol);
        resource->view.revision = protocol->revision;
        mark_resource_dirty(terminal, resource);
    }
}

static void commit_animation_control(
    boxrig_terminal *terminal,
    const boxrig_kitty_graphics_transaction_state *transaction)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_image_resource_state *resource = transaction->animation_resource;
    bool restart_deadline = false;

    if (transaction->animation_sets_gap) {
        if (transaction->prepared_gap_frame == 1) {
            resource->root_gap_milliseconds =
                transaction->prepared_gap_milliseconds;
        } else {
            resource->frames[transaction->prepared_gap_frame - 2]
                .gap_milliseconds = transaction->prepared_gap_milliseconds;
        }
        if (transaction->prepared_gap_frame == resource->animation_frame &&
            animation_playback_running(resource) &&
            !resource->animation_waiting) {
            restart_deadline = true;
        }
    }
    if (transaction->animation_sets_loops) {
        resource->animation_loop_count = transaction->prepared_loop_count;
        resource->animation_remaining_wraps =
            animation_initial_remaining_wraps(
                resource->animation_loop_count);
    }
    if (transaction->animation_sets_playback_state) {
        resource->animation_playback_state =
            (boxrig_animation_playback_state)
                transaction->prepared_playback_state;
        resource->animation_remaining_wraps =
            animation_initial_remaining_wraps(
                resource->animation_loop_count);
        resource->animation_frame = resource->current_frame;
        resource->animation_deadline_set = false;
        resource->animation_waiting = false;
        restart_deadline = animation_playback_running(resource);
    }
    if (transaction->animation_sets_current) {
        bool current_changed =
            resource->current_frame != transaction->prepared_current_frame;

        resource->current_frame = transaction->prepared_current_frame;
        resource->animation_frame = transaction->prepared_current_frame;
        resource->animation_deadline_set = false;
        resource->animation_waiting = false;
        restart_deadline = animation_playback_running(resource);
        if (current_changed) {
            resource->view.rgba = frame_rgba(
                resource,
                resource->current_frame);
            increment_revision(protocol);
            resource->view.revision = protocol->revision;
            mark_resource_dirty(terminal, resource);
        }
    }
    if (!animation_playback_running(resource)) {
        resource->animation_frame = resource->current_frame;
        resource->animation_deadline_set = false;
        resource->animation_waiting = false;
    } else if (restart_deadline) {
        resource->animation_deadline_set = false;
        /* An exclusive runtime anchors new content at its next ordinary
         * synchronization. Feeding bytes never calls the host's clock. */
        if (terminal->animation_clock == NULL && protocol->monotonic_time_set) {
            set_initial_animation_deadline(protocol, resource);
        }
    }
}

static void commit_frame_composition(
    boxrig_terminal *terminal,
    boxrig_kitty_graphics_transaction_state *transaction)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_image_resource_state *resource = transaction->animation_resource;
    size_t frame_number = transaction->prepared_animation_frame;
    bool current = resource->current_frame == frame_number;

    if (frame_number == 1) {
        free(resource->rgba);
        resource->rgba = transaction->prepared_animation_rgba;
        resource->root_transient =
            transaction->prepared_animation_transient;
    } else {
        boxrig_animation_frame_state *frame =
            &resource->frames[frame_number - 2];

        free(frame->rgba);
        frame->rgba = transaction->prepared_animation_rgba;
        frame->transient = transaction->prepared_animation_transient;
    }
    transaction->prepared_animation_rgba = NULL;
    resource->transient =
        resource->transient || transaction->prepared_animation_transient;
    if (current) {
        resource->view.rgba = frame_rgba(resource, frame_number);
        increment_revision(protocol);
        resource->view.revision = protocol->revision;
        mark_resource_dirty(terminal, resource);
    }
}

static void commit_placement(
    boxrig_terminal *terminal,
    boxrig_kitty_graphics_transaction_state *transaction,
    bool increment_protocol_revision)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_image_placement_state *placement =
        transaction->prepared_placement;
    size_t index;

    if (transaction->replacement_placement != NULL) {
        for (index = 0; index < protocol->placement_count; index++) {
            boxrig_image_placement_state *candidate =
                protocol->placements[index];

            if (candidate->parent == transaction->replacement_placement) {
                candidate->parent = placement;
            }
        }
    }
    remove_placement_pointer_direct(
        terminal,
        transaction->replacement_placement);
    transaction->replacement_placement = NULL;
    if (increment_protocol_revision) {
        increment_revision(protocol);
    }
    placement->creation_serial = protocol->next_creation_serial;
    protocol->next_creation_serial = increment_nonzero_u64(
        protocol->next_creation_serial);
    placement->view.resource_creation_serial =
        placement->resource->creation_serial;
    placement->view.placement_creation_serial = placement->creation_serial;
    placement->view.revision = protocol->revision;
    protocol->placements[protocol->placement_count] = placement;
    protocol->placement_count++;
    transaction->prepared_placement = NULL;
    mark_placement_dirty(
        terminal,
        placement,
        protocol->cell_pixel_width,
        protocol->cell_pixel_height);
    if (placement->move_cursor) {
        boxrig_screen_move_cursor_for_image(
            &terminal->screen,
            placement->view.rows,
            placement->view.columns);
    }
    refresh_derived_placements(terminal, true);
}

static void finish_upload(
    boxrig_terminal *terminal,
    boxrig_kitty_graphics_transaction_state *transaction,
    boxrig_kitty_graphics_upload_state *upload,
    boxrig_image_resource_state *selected_frame_resource)
{
    if (upload->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
        !finish_base64(upload) &&
        upload->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK) {
        upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (upload->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
        upload->compressed) {
        (void)finish_zlib(upload);
    }
    if (upload->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
        upload->command.format == 100) {
        if (upload->png_fixed_capacity &&
            upload->png_length != upload->command.data_size) {
            upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_BAD_ZLIB;
        } else {
            (void)decode_png(upload);
        }
    } else if (upload->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
               (upload->raw_received != upload->raw_expected ||
                upload->rgba_offset != upload->rgba_length ||
                upload->channel_count != 0)) {
        upload->result = upload->compressed
                             ? BOXRIG_KITTY_GRAPHICS_RESULT_BAD_ZLIB
                             : BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
    }
    if (upload->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
        upload->command.action == (uint8_t)'f') {
        prepare_frame(terminal, upload, selected_frame_resource);
    } else if (upload->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
               upload->command.action != (uint8_t)'q') {
        prepare_resource(terminal, upload);
    }
    if (upload->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
        upload->command.action == (uint8_t)'T') {
        upload->result = prepare_placement(
            terminal,
            transaction,
            upload->prepared_resource,
            &upload->command,
            upload->planned_removed_placement_count,
            false);
    }
    transaction->command = upload->command;
    transaction->result = upload->result;
}

static void prepare_existing_placement(
    boxrig_terminal *terminal,
    boxrig_kitty_graphics_transaction_state *transaction)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_image_resource_state *resource;

    if (command_has(
            &transaction->command,
            BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID)) {
        resource = find_image_by_id(
            protocol,
            transaction->command.image_id);
    } else {
        resource = find_latest_image_by_number(
            protocol,
            transaction->command.image_number);
    }
    if (resource == NULL) {
        transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NOT_FOUND;
        return;
    }
    transaction->command.image_id = resource->view.image_id;
    transaction->result = prepare_placement(
        terminal,
        transaction,
        resource,
        &transaction->command,
        0,
        true);
}

static boxrig_graphics_external_request_state *find_external_request(
    boxrig_kitty_graphics_protocol_state *protocol,
    uint64_t request_id,
    boxrig_graphics_external_request_state **previous_out)
{
    boxrig_graphics_external_request_state *previous = NULL;
    boxrig_graphics_external_request_state *request;

    for (request = protocol->external_requests;
         request != NULL;
         request = request->next) {
        if (request->request_id == request_id) {
            if (previous_out != NULL) {
                *previous_out = previous;
            }
            return request;
        }
        previous = request;
    }
    return NULL;
}

static void remove_external_request(
    boxrig_kitty_graphics_protocol_state *protocol,
    boxrig_graphics_external_request_state *request,
    boxrig_graphics_external_request_state *previous)
{
    if (previous == NULL) {
        protocol->external_requests = request->next;
    } else {
        previous->next = request->next;
    }
    free(request);
}

static bool prepare_external_response(
    const boxrig_graphics_external_request_state *request,
    boxrig_kitty_graphics_result result,
    uint8_t *response,
    size_t *response_length_out)
{
    boxrig_kitty_graphics_transaction_state transaction;

    memset(&transaction, 0, sizeof(transaction));
    transaction.command = request->command;
    transaction.result = result;
    if (!prepare_response(&transaction)) {
        return false;
    }
    memcpy(response, transaction.response, transaction.response_length);
    *response_length_out = transaction.response_length;
    return true;
}

static boxrig_status queue_external_request(
    boxrig_terminal *terminal,
    boxrig_kitty_graphics_transaction_state *transaction)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_graphics_external_request_state *request;
    boxrig_graphics_transport_view transport;
    size_t response_reservation;
    uint64_t request_id;
    boxrig_status status;

    if (transaction->command.data_size > protocol->decode_capacity) {
        transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NO_SPACE;
        return BOXRIG_STATUS_OK;
    }
    request = calloc(1, sizeof(*request));
    if (request == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    if (transaction->command.action == (uint8_t)'f') {
        boxrig_image_resource_state *resource;

        if (command_has(
                &transaction->command,
                BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID)) {
            resource = find_image_by_id(
                protocol,
                transaction->command.image_id);
        } else {
            resource = find_latest_image_by_number(
                protocol,
                transaction->command.image_number);
        }

        if (resource == NULL) {
            free(request);
            transaction->result = BOXRIG_KITTY_GRAPHICS_RESULT_NOT_FOUND;
            return BOXRIG_STATUS_OK;
        }
        transaction->command.image_id = resource->view.image_id;
        request->target_image_handle = resource->view.handle;
    }
    transport.medium = transaction->command.transmission == (uint8_t)'f'
                           ? BOXRIG_GRAPHICS_TRANSPORT_FILE
                           : transaction->command.transmission == (uint8_t)'t'
                                 ? BOXRIG_GRAPHICS_TRANSPORT_TEMPORARY_FILE
                                 : BOXRIG_GRAPHICS_TRANSPORT_SHARED_MEMORY;
    transport.name = transaction->transport_name;
    transport.name_length = transaction->transport_name_length;
    transport.offset = transaction->command.data_offset;
    transport.size = transaction->command.data_size;
    transport.data_capacity = transaction->command.data_size == 0
                                  ? protocol->decode_capacity
                                  : transaction->command.data_size;
    transport.delete_after_read =
        transaction->command.transmission == (uint8_t)'t';
    response_reservation = should_respond(
                               &transaction->command,
                               BOXRIG_KITTY_GRAPHICS_RESULT_IO_ERROR)
                               ? BOXRIG_KITTY_GRAPHICS_RESPONSE_CAPACITY
                               : 0;
    status = boxrig_terminal_queue_graphics_request(
        terminal,
        &transport,
        response_reservation,
        &request_id);
    if (status != BOXRIG_STATUS_OK) {
        free(request);
        return status;
    }
    request->command = transaction->command;
    request->request_id = request_id;
    request->input_serial = protocol->next_external_serial;
    if (transaction->command.action != (uint8_t)'q' &&
        transaction->command.action != (uint8_t)'f') {
        request->creation_serial = protocol->next_creation_serial;
        protocol->next_creation_serial = increment_nonzero_u64(
            protocol->next_creation_serial);
    }
    request->data_capacity = transport.data_capacity;
    request->next = protocol->external_requests;
    protocol->external_requests = request;
    protocol->next_external_serial = increment_nonzero_u64(
        protocol->next_external_serial);
    return BOXRIG_STATUS_OK;
}

static boxrig_status protocol_begin(
    void *context,
    boxrig_kitty_graphics_command command)
{
    boxrig_terminal *terminal = context;
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_kitty_graphics_transaction_state *state = &protocol->transaction;
    bool continuation = protocol->upload != NULL &&
                        is_continuation_command(
                            &command,
                            protocol->upload);

    clear_transaction(state);
    if (continuation) {
        if (command_has(&command, BOXRIG_KITTY_GRAPHICS_FIELD_QUIET)) {
            protocol->upload->command.quiet = command.quiet;
            protocol->upload->command.presence |=
                FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_QUIET);
        }
        protocol->upload->chunk_encoded_length = 0;
        state->command = protocol->upload->command;
        state->more = command.more;
        state->active = true;
        state->uses_upload = true;
        return BOXRIG_STATUS_OK;
    }
    clear_upload(protocol);
    apply_defaults(&command);
    state->command = command;
    state->more = command.more;
    state->active = true;
    state->result = validate_protocol_command(protocol, &command, 1);
    if (protocol->enabled &&
        state->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
        is_upload_action(&command)) {
        if (transmission_uses_host(&command)) {
            state->uses_external_transport = true;
            state->transport_valid = protocol->transport_name_capacity != 0;
        } else {
            protocol->upload = create_upload(protocol, &command);
            if (protocol->upload == NULL) {
                clear_transaction(state);
                return BOXRIG_STATUS_OUT_OF_MEMORY;
            }
            state->uses_upload = true;
        }
    }
    return BOXRIG_STATUS_OK;
}

static boxrig_status protocol_put(void *context, uint8_t byte)
{
    boxrig_terminal *terminal = context;
    boxrig_kitty_graphics_transaction_state *state =
        &terminal->graphics_protocol.transaction;

    if (!state->active) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    state->payload_length++;
    if (state->uses_upload) {
        boxrig_kitty_graphics_upload_state *upload =
            terminal->graphics_protocol.upload;

        upload->chunk_encoded_length++;
        if (upload->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
            !put_base64_byte(upload, byte)) {
            if (upload->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK) {
                upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
            }
        }
    } else if (state->uses_external_transport && state->transport_valid &&
               !put_transport_base64_byte(
                   &terminal->graphics_protocol,
                   state,
                   byte)) {
        state->transport_valid = false;
    }
    return BOXRIG_STATUS_OK;
}

static boxrig_status protocol_commit(void *context)
{
    boxrig_terminal *terminal = context;
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_kitty_graphics_transaction_state *state = &protocol->transaction;
    boxrig_status status;

    if (!state->active) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (!protocol->enabled) {
        clear_transaction(state);
        return BOXRIG_STATUS_OK;
    }
    if (!state->prepared) {
        if (state->uses_external_transport) {
            state->result = validate_protocol_command(
                protocol,
                &state->command,
                state->payload_length);
            if (state->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
                protocol->transport_name_capacity == 0) {
                state->result =
                    BOXRIG_KITTY_GRAPHICS_RESULT_UNSUPPORTED_TRANSMISSION;
            } else if (state->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
                       (!state->transport_valid ||
                        !finish_transport_base64(protocol, state) ||
                        state->transport_name_length == 0)) {
                state->result = BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
            }
            if (state->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK) {
                status = queue_external_request(terminal, state);
                if (status != BOXRIG_STATUS_OK) {
                    return status;
                }
                if (state->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK) {
                    clear_transaction(state);
                    return BOXRIG_STATUS_OK;
                }
            }
        } else if (state->uses_upload) {
            boxrig_kitty_graphics_upload_state *upload = protocol->upload;

            if (state->more == 1 &&
                upload->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
                upload->chunk_encoded_length % 4 == 0 &&
                upload->quartet_length == 0 && !upload->base64_ended) {
                clear_transaction(state);
                return BOXRIG_STATUS_OK;
            }
            if (state->more == 1 &&
                upload->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK) {
                upload->result = BOXRIG_KITTY_GRAPHICS_RESULT_INVALID;
            }
            if (state->more == 0) {
                finish_upload(terminal, state, upload, NULL);
            } else {
                state->command = upload->command;
                state->result = upload->result;
            }
        } else {
            state->result = validate_protocol_command(
                protocol,
                &state->command,
                state->payload_length);
            if (state->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
                state->command.action == (uint8_t)'p') {
                prepare_existing_placement(terminal, state);
            } else if (state->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
                       state->command.action == (uint8_t)'d') {
                prepare_delete(terminal, state);
            } else if (state->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
                       state->command.action == (uint8_t)'a') {
                prepare_animation_control(terminal, state);
            } else if (state->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
                       state->command.action == (uint8_t)'c') {
                prepare_frame_composition(terminal, state);
            }
        }
        if (!prepare_response(state)) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        state->prepared = true;
    }
    status = boxrig_terminal_append_output(
        terminal,
        state->response,
        state->response_length);
    if (status == BOXRIG_STATUS_OK) {
        /* Every fallible decode, allocation, quota, and outbound operation is
         * complete before the prepared resource becomes publicly visible. */
        if (state->uses_upload &&
            state->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
            protocol->upload->command.action != (uint8_t)'q') {
            bool places_resource =
                protocol->upload->command.action == (uint8_t)'T';

            if (protocol->upload->command.action == (uint8_t)'f') {
                commit_frame(terminal, protocol->upload);
            } else {
                commit_resource(terminal, protocol->upload, 0);
                if (places_resource) {
                    commit_placement(terminal, state, false);
                }
            }
            refresh_derived_placements(terminal, true);
        } else if (!state->uses_upload &&
                   state->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
                   state->command.action == (uint8_t)'p') {
            commit_placement(terminal, state, true);
        } else if (!state->uses_upload &&
                   state->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
                   state->command.action == (uint8_t)'d') {
            commit_delete(terminal);
        } else if (!state->uses_upload &&
                   state->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
                   state->command.action == (uint8_t)'a') {
            commit_animation_control(terminal, state);
        } else if (!state->uses_upload &&
                   state->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
                   state->command.action == (uint8_t)'c') {
            commit_frame_composition(terminal, state);
        }
        if (state->uses_upload) {
            clear_upload(protocol);
        }
        clear_transaction(state);
    }
    return status;
}

static void protocol_abort(void *context)
{
    boxrig_terminal *terminal = context;

    if (terminal->graphics_protocol.transaction.uses_upload) {
        clear_upload(&terminal->graphics_protocol);
    }
    clear_transaction(&terminal->graphics_protocol.transaction);
}

void boxrig_kitty_graphics_protocol_initialize(
    boxrig_terminal *terminal,
    const boxrig_terminal_config *config)
{
    boxrig_kitty_graphics_consumer consumer;

    memset(&terminal->graphics_protocol, 0, sizeof(terminal->graphics_protocol));
    terminal->graphics_protocol.storage_capacity =
        config->graphics_storage_capacity;
    terminal->graphics_protocol.decode_capacity =
        config->graphics_decode_capacity;
    terminal->graphics_protocol.image_capacity =
        config->graphics_image_capacity;
    terminal->graphics_protocol.placement_capacity =
        config->graphics_placement_capacity;
    terminal->graphics_protocol.transport_name_capacity =
        config->graphics_transport_name_capacity;
    terminal->graphics_protocol.animation_storage_capacity =
        config->graphics_animation_storage_capacity;
    terminal->graphics_protocol.frame_capacity =
        config->graphics_frame_capacity;
    terminal->graphics_protocol.revision = 1;
    terminal->graphics_protocol.next_image_handle = 1;
    terminal->graphics_protocol.next_creation_serial = 1;
    terminal->graphics_protocol.next_external_serial = 1;
    terminal->graphics_protocol.next_image_id = 1;
    terminal->graphics_protocol.enabled =
        config->graphics_storage_capacity != 0;
    consumer.context = terminal;
    consumer.begin = protocol_begin;
    consumer.put = protocol_put;
    consumer.commit = protocol_commit;
    consumer.abort = protocol_abort;
    boxrig_kitty_graphics_set_consumer(&terminal->graphics, &consumer);
}

void boxrig_kitty_graphics_protocol_host_item_consumed(
    boxrig_terminal *terminal,
    uint64_t request_id)
{
    boxrig_graphics_external_request_state *request;

    if (terminal == NULL || request_id == 0) {
        return;
    }
    request = find_external_request(
        &terminal->graphics_protocol,
        request_id,
        NULL);
    if (request != NULL) {
        request->consumed = true;
    }
}

boxrig_status boxrig_kitty_graphics_protocol_complete_external(
    boxrig_terminal *terminal,
    uint64_t request_id,
    boxrig_host_request_result result,
    const uint8_t *payload,
    size_t payload_length)
{
    boxrig_kitty_graphics_protocol_state *protocol;
    boxrig_graphics_external_request_state *previous = NULL;
    boxrig_graphics_external_request_state *request;
    boxrig_kitty_graphics_upload_state *upload = NULL;
    boxrig_kitty_graphics_transaction_state transaction;
    boxrig_kitty_graphics_command decode_command;
    boxrig_kitty_graphics_result graphics_result;
    uint8_t response[BOXRIG_KITTY_GRAPHICS_RESPONSE_CAPACITY];
    size_t response_length;
    size_t index;
    bool stale = false;
    boxrig_status status;
    boxrig_image_resource_state *selected_frame_resource = NULL;

    if (terminal == NULL || request_id == 0 ||
        (payload == NULL && payload_length != 0)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    protocol = &terminal->graphics_protocol;
    request = find_external_request(protocol, request_id, &previous);
    if (request == NULL || !request->consumed) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (result == BOXRIG_HOST_REQUEST_SUCCESS &&
        payload_length > request->data_capacity) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (request->cancelled) {
        remove_external_request(protocol, request, previous);
        return BOXRIG_STATUS_OK;
    }
    memset(&transaction, 0, sizeof(transaction));
    if (result == BOXRIG_HOST_REQUEST_DENIED) {
        graphics_result = BOXRIG_KITTY_GRAPHICS_RESULT_ACCESS_DENIED;
    } else if (result == BOXRIG_HOST_REQUEST_UNAVAILABLE ||
               (request->command.data_size != 0 &&
                payload_length != request->command.data_size)) {
        graphics_result = BOXRIG_KITTY_GRAPHICS_RESULT_IO_ERROR;
    } else {
        if (request->command.action == (uint8_t)'f') {
            selected_frame_resource = find_image_by_handle(
                protocol,
                request->target_image_handle);
            if (selected_frame_resource == NULL) {
                graphics_result = BOXRIG_KITTY_GRAPHICS_RESULT_NOT_FOUND;
                goto prepare_completion;
            }
            stale = selected_frame_resource->last_external_frame_serial >
                    request->input_serial;
        }
        decode_command = request->command;
        decode_command.data_size = 0;
        decode_command.presence &=
            ~FIELD_BIT(BOXRIG_KITTY_GRAPHICS_FIELD_DATA_SIZE);
        upload = create_upload(protocol, &decode_command);
        if (upload == NULL) {
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
        for (index = 0;
             index < payload_length &&
             upload->result == BOXRIG_KITTY_GRAPHICS_RESULT_OK;
             index++) {
            (void)consume_decoded_byte(upload, payload[index]);
        }
        if (request->creation_serial != 0 &&
            command_has(
                &request->command,
                BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID) &&
            request->command.image_id != 0) {
            boxrig_image_resource_state *existing = find_image_by_id(
                protocol,
                request->command.image_id);

            stale = existing != NULL &&
                    existing->creation_serial > request->creation_serial;
        }
        finish_upload(
            terminal,
            &transaction,
            upload,
            selected_frame_resource);
        graphics_result = upload->result;
    }
prepare_completion:
    if (!prepare_external_response(
            request,
            graphics_result,
            response,
            &response_length)) {
        clear_transaction(&transaction);
        destroy_upload(upload);
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    status = boxrig_terminal_resolve_graphics_barrier(
        terminal,
        request_id,
        response,
        response_length);
    if (status != BOXRIG_STATUS_OK) {
        clear_transaction(&transaction);
        destroy_upload(upload);
        return status;
    }
    if (upload != NULL && graphics_result == BOXRIG_KITTY_GRAPHICS_RESULT_OK &&
        !stale && request->command.action != (uint8_t)'q') {
        bool places_resource = request->command.action == (uint8_t)'T';

        if (request->command.action == (uint8_t)'f') {
            commit_frame(terminal, upload);
            upload->frame_resource->last_external_frame_serial =
                request->input_serial;
        } else {
            commit_resource(terminal, upload, request->creation_serial);
            if (places_resource) {
                commit_placement(terminal, &transaction, false);
            }
        }
        refresh_derived_placements(terminal, true);
    }
    clear_transaction(&transaction);
    destroy_upload(upload);
    remove_external_request(protocol, request, previous);
    return BOXRIG_STATUS_OK;
}

static void cancel_external_requests(boxrig_terminal *terminal)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_graphics_external_request_state *previous = NULL;
    boxrig_graphics_external_request_state *request =
        protocol->external_requests;

    while (request != NULL) {
        boxrig_graphics_external_request_state *next = request->next;

        if (!request->cancelled) {
            uint8_t response[BOXRIG_KITTY_GRAPHICS_RESPONSE_CAPACITY];
            size_t response_length = 0;

            if (prepare_external_response(
                    request,
                    BOXRIG_KITTY_GRAPHICS_RESULT_IO_ERROR,
                    response,
                    &response_length)) {
                (void)boxrig_terminal_resolve_graphics_barrier(
                    terminal,
                    request->request_id,
                    response,
                    response_length);
            }
        }
        if (request->consumed) {
            request->cancelled = true;
            previous = request;
        } else {
            (void)boxrig_terminal_remove_queued_graphics_request(
                terminal,
                request->request_id);
            remove_external_request(protocol, request, previous);
        }
        request = next;
    }
}

void boxrig_kitty_graphics_protocol_reset(boxrig_terminal *terminal)
{
    boxrig_kitty_graphics_protocol_state *protocol;
    size_t index;

    if (terminal == NULL) {
        return;
    }
    protocol = &terminal->graphics_protocol;
    cancel_external_requests(terminal);
    clear_transaction(&protocol->transaction);
    clear_upload(protocol);
    mark_active_placements(terminal);
    for (index = 0; index < protocol->placement_count; index++) {
        destroy_placement(protocol->placements[index]);
    }
    for (index = 0; index < protocol->image_count; index++) {
        destroy_resource(protocol->images[index]);
    }
    if (protocol->placement_count != 0 || protocol->image_count != 0) {
        increment_revision(protocol);
    }
    protocol->placement_count = 0;
    protocol->image_count = 0;
    protocol->storage_usage = 0;
    protocol->animation_storage_usage = 0;
    protocol->animation_frame_count = 0;
    protocol->next_image_handle = 1;
    protocol->next_creation_serial = 1;
    protocol->next_image_id = 1;
    if (protocol->placeholder_instances != NULL) {
        memset(
            protocol->placeholder_instances,
            0,
            protocol->placeholder_cell_capacity *
                sizeof(*protocol->placeholder_instances));
    }
    protocol->screen_mutation_changed = false;
}

void boxrig_kitty_graphics_protocol_destroy(boxrig_terminal *terminal)
{
    boxrig_kitty_graphics_protocol_state *protocol;
    size_t index;

    if (terminal == NULL) {
        return;
    }
    protocol = &terminal->graphics_protocol;
    boxrig_kitty_graphics_protocol_reset(terminal);
    while (protocol->external_requests != NULL) {
        boxrig_graphics_external_request_state *request =
            protocol->external_requests;

        protocol->external_requests = request->next;
        free(request);
    }
    for (index = 0; index < protocol->placement_count; index++) {
        destroy_placement(protocol->placements[index]);
    }
    for (index = 0; index < protocol->image_count; index++) {
        destroy_resource(protocol->images[index]);
    }
    free(protocol->dirty_end);
    free(protocol->dirty_start);
    free(protocol->placeholder_instances);
    free(protocol->placements);
    free(protocol->images);
    protocol->dirty_end = NULL;
    protocol->dirty_start = NULL;
    protocol->placeholder_instances = NULL;
    protocol->placements = NULL;
    protocol->images = NULL;
    protocol->dirty_row_count = 0;
    protocol->placeholder_cell_capacity = 0;
    protocol->placement_count = 0;
    protocol->placement_slot_capacity = 0;
    protocol->image_count = 0;
    protocol->image_slot_capacity = 0;
    protocol->storage_usage = 0;
    protocol->animation_storage_usage = 0;
    protocol->animation_frame_count = 0;
}

static bool calculate_animation_cycle_duration(
    const boxrig_image_resource_state *resource,
    uint64_t *duration_out)
{
    uint64_t duration = 0;
    size_t frame_number;

    for (frame_number = 1;
         frame_number <= resource->frame_count + 1;
         frame_number++) {
        int32_t gap = frame_gap(resource, frame_number);
        uint64_t positive_gap;

        if (gap <= 0) {
            continue;
        }
        positive_gap = (uint64_t)(uint32_t)gap;
        if (positive_gap > UINT64_MAX - duration) {
            *duration_out = UINT64_MAX;
            return false;
        }
        duration += positive_gap;
    }
    *duration_out = duration;
    return true;
}

static size_t previous_visible_animation_frame(
    const boxrig_image_resource_state *resource)
{
    size_t frame_total = resource->frame_count + 1;
    size_t frame_number = resource->animation_frame;
    size_t count;

    for (count = 0; count < frame_total; count++) {
        frame_number = frame_number == 1 ? frame_total : frame_number - 1;
        if (frame_gap(resource, frame_number) > 0) {
            return frame_number;
        }
    }
    return resource->current_frame;
}

static void stop_resource_animation(boxrig_image_resource_state *resource)
{
    resource->animation_playback_state =
        BOXRIG_ANIMATION_PLAYBACK_STOPPED;
    resource->animation_deadline_set = false;
    resource->animation_waiting = false;
}

static void advance_resource_animation(
    boxrig_image_resource_state *resource,
    uint64_t milliseconds)
{
    size_t frame_total = resource->frame_count + 1;
    uint64_t cycle_duration = 0;
    bool cycle_duration_representable = true;
    size_t step;

    if (resource->animation_waiting) {
        if (resource->animation_playback_state !=
                BOXRIG_ANIMATION_PLAYBACK_LOADING ||
            resource->animation_frame >= frame_total) {
            return;
        }
        resource->animation_waiting = false;
        resource->animation_deadline_milliseconds = milliseconds;
        resource->animation_deadline_set = true;
    }
    if (!resource->animation_deadline_set ||
        resource->animation_deadline_milliseconds > milliseconds) {
        return;
    }
    if (resource->animation_playback_state ==
        BOXRIG_ANIMATION_PLAYBACK_LOOPING) {
        cycle_duration_representable =
            calculate_animation_cycle_duration(
                resource,
                &cycle_duration);
        if (cycle_duration == 0) {
            stop_resource_animation(resource);
            return;
        }
    }
    for (step = 0; step <= frame_total; step++) {
        uint64_t event_time;
        int32_t gap;

        if (!resource->animation_deadline_set ||
            resource->animation_deadline_milliseconds > milliseconds) {
            return;
        }
        if (resource->animation_playback_state ==
                BOXRIG_ANIMATION_PLAYBACK_LOOPING &&
            cycle_duration_representable &&
            resource->animation_deadline_milliseconds != UINT64_MAX) {
            uint64_t elapsed = milliseconds -
                               resource->animation_deadline_milliseconds;
            uint64_t cycles = elapsed / cycle_duration;

            if (resource->animation_loop_count != 1 &&
                cycles > resource->animation_remaining_wraps) {
                cycles = resource->animation_remaining_wraps;
            }
            if (cycles != 0) {
                /* The quotient makes the multiplication fit in elapsed time;
                 * a whole cycle returns to this logical cursor. */
                resource->animation_deadline_milliseconds +=
                    cycles * cycle_duration;
                if (resource->animation_loop_count != 1) {
                    resource->animation_remaining_wraps -=
                        (uint32_t)cycles;
                }
                resource->current_frame =
                    frame_gap(resource, resource->animation_frame) > 0
                        ? resource->animation_frame
                        : previous_visible_animation_frame(resource);
            }
        }
        if (resource->animation_deadline_milliseconds > milliseconds) {
            return;
        }
        event_time = resource->animation_deadline_milliseconds;
        if (resource->animation_playback_state ==
            BOXRIG_ANIMATION_PLAYBACK_LOADING) {
            if (resource->animation_frame >= frame_total) {
                resource->animation_waiting = true;
                resource->animation_deadline_set = false;
                return;
            }
            resource->animation_frame++;
        } else if (resource->animation_frame >= frame_total) {
            if (resource->animation_loop_count != 1 &&
                resource->animation_remaining_wraps == 0) {
                stop_resource_animation(resource);
                return;
            }
            if (resource->animation_loop_count != 1) {
                resource->animation_remaining_wraps--;
            }
            resource->animation_frame = 1;
        } else {
            resource->animation_frame++;
        }
        gap = frame_gap(resource, resource->animation_frame);
        if (gap > 0) {
            /* Negative-gap cursors advance without replacing the last visible
             * frame, including while loading waits at a gapless tail. */
            resource->current_frame = resource->animation_frame;
            resource->animation_deadline_milliseconds =
                animation_saturating_add(
                    event_time,
                    (uint64_t)(uint32_t)gap);
            if (event_time == UINT64_MAX ||
                resource->animation_deadline_milliseconds > milliseconds) {
                return;
            }
        }
    }
    stop_resource_animation(resource);
}

static bool resource_animation_can_advance(
    const boxrig_image_resource_state *resource,
    uint64_t milliseconds)
{
    if (!animation_playback_running(resource)) {
        return false;
    }
    if (resource->animation_waiting) {
        return resource->animation_playback_state ==
                   BOXRIG_ANIMATION_PLAYBACK_LOADING &&
               resource->animation_frame < resource->frame_count + 1;
    }
    return resource->animation_deadline_set &&
           resource->animation_deadline_milliseconds <= milliseconds;
}

static void initialize_pending_animation_deadlines(
    boxrig_kitty_graphics_protocol_state *protocol)
{
    for (size_t index = 0; index < protocol->image_count; index++) {
        boxrig_image_resource_state *resource = protocol->images[index];
        if (animation_playback_running(resource) &&
            !resource->animation_waiting && !resource->animation_deadline_set) {
            set_initial_animation_deadline(protocol, resource);
        }
    }
}

static boxrig_status update_monotonic_time(
    boxrig_terminal *terminal,
    uint64_t milliseconds)
{
    boxrig_kitty_graphics_protocol_state *protocol;
    size_t index;
    bool can_advance = false;

    if (terminal == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    protocol = &terminal->graphics_protocol;
    if (protocol->monotonic_time_set) {
        if (milliseconds < protocol->monotonic_time_milliseconds) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        if (milliseconds == protocol->monotonic_time_milliseconds) {
            boxrig_screen_update_synchronized_output_time(
                &terminal->screen, milliseconds);
            initialize_pending_animation_deadlines(protocol);
            return BOXRIG_STATUS_OK;
        }
    } else {
        protocol->monotonic_time_milliseconds = milliseconds;
        protocol->monotonic_time_set = true;
        boxrig_screen_update_synchronized_output_time(
            &terminal->screen, milliseconds);
        for (index = 0; index < protocol->image_count; index++) {
            boxrig_image_resource_state *resource = protocol->images[index];

            if (animation_playback_running(resource) &&
                !resource->animation_waiting) {
                set_initial_animation_deadline(protocol, resource);
            }
        }
        return BOXRIG_STATUS_OK;
    }
    for (index = 0; index < protocol->image_count; index++) {
        if (resource_animation_can_advance(
                protocol->images[index],
                milliseconds)) {
            can_advance = true;
            break;
        }
    }
    if (can_advance && !ensure_image_dirty_storage(terminal)) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    protocol->monotonic_time_milliseconds = milliseconds;
    initialize_pending_animation_deadlines(protocol);
    boxrig_screen_update_synchronized_output_time(
        &terminal->screen, milliseconds);
    for (index = 0; index < protocol->image_count; index++) {
        boxrig_image_resource_state *resource = protocol->images[index];
        size_t old_current_frame = resource->current_frame;

        if (!resource_animation_can_advance(resource, milliseconds)) {
            continue;
        }
        advance_resource_animation(resource, milliseconds);
        if (resource->current_frame == old_current_frame) {
            continue;
        }
        resource->view.rgba = frame_rgba(
            resource,
            resource->current_frame);
        increment_revision(protocol);
        resource->view.revision = protocol->revision;
        mark_resource_dirty(terminal, resource);
    }
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_update_monotonic_time(
    boxrig_terminal *terminal, uint64_t milliseconds)
{
    if (terminal != NULL && terminal->animation_clock != NULL) {
        return BOXRIG_STATUS_BUSY;
    }
    return update_monotonic_time(terminal, milliseconds);
}

boxrig_status boxrig_terminal_acquire_animation_clock(
    boxrig_terminal *terminal, boxrig_animation_clock **clock_out)
{
    if (clock_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *clock_out = NULL;
    if (terminal == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (terminal->animation_clock != NULL) {
        return BOXRIG_STATUS_BUSY;
    }
    boxrig_animation_clock *clock = calloc(1, sizeof(*clock));
    if (clock == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    clock->terminal = terminal;
    terminal->animation_clock = clock;
    *clock_out = clock;
    return BOXRIG_STATUS_OK;
}

void boxrig_animation_clock_release(boxrig_animation_clock *clock)
{
    if (clock == NULL) {
        return;
    }
    if (clock->terminal != NULL) {
        clock->terminal->animation_clock = NULL;
    }
    free(clock);
}

boxrig_status boxrig_animation_clock_update_monotonic_time(
    boxrig_animation_clock *clock, uint64_t milliseconds)
{
    if (clock == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (clock->terminal == NULL) {
        return BOXRIG_STATUS_STALE;
    }
    return update_monotonic_time(clock->terminal, milliseconds);
}

bool boxrig_terminal_next_animation_deadline(
    const boxrig_terminal *terminal,
    uint64_t *deadline_out)
{
    const boxrig_kitty_graphics_protocol_state *protocol;
    uint64_t deadline = UINT64_MAX;
    bool found = false;
    size_t index;

    if (terminal == NULL || deadline_out == NULL) {
        return false;
    }
    protocol = &terminal->graphics_protocol;
    if (terminal->screen.synchronized_output &&
        terminal->screen.synchronized_output_deadline_set) {
        deadline = terminal->screen.synchronized_output_deadline_milliseconds;
        found = true;
    }
    for (index = 0; index < protocol->image_count; index++) {
        const boxrig_image_resource_state *resource = protocol->images[index];

        if (!animation_playback_running(resource) ||
            resource->animation_waiting ||
            !resource->animation_deadline_set) {
            continue;
        }
        if (!found ||
            resource->animation_deadline_milliseconds < deadline) {
            deadline = resource->animation_deadline_milliseconds;
            found = true;
        }
    }
    if (found) {
        *deadline_out = deadline;
    }
    return found;
}

boxrig_status boxrig_terminal_validate_cell_pixel_geometry(
    const boxrig_terminal *terminal,
    uint32_t width,
    uint32_t height)
{
    const boxrig_kitty_graphics_protocol_state *state;
    size_t index;

    if (terminal == NULL || width == 0 || height == 0 ||
        terminal->screen.columns > SIZE_MAX / width ||
        terminal->screen.rows > SIZE_MAX / height) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    state = &terminal->graphics_protocol;
    for (index = 0; index < state->placement_count; index++) {
        boxrig_image_placement_state candidate =
            *state->placements[index];
        boxrig_image_placement_view resolved;
        size_t virtual_box_width;
        size_t virtual_box_height;
        size_t virtual_width;
        size_t virtual_height;

        if (candidate.virtual_placement) {
            if (!calculate_virtual_fit(
                    &candidate,
                    width,
                    height,
                    &virtual_box_width,
                    &virtual_box_height,
                    &virtual_width,
                    &virtual_height)) {
                return BOXRIG_STATUS_INVALID_ARGUMENT;
            }
        } else if (!(candidate.relative_placement
                         ? resolve_relative_geometry(
                               terminal,
                               &candidate,
                               width,
                               height,
                               &resolved)
                         : resolve_placement_geometry(
                               terminal,
                               &candidate,
                               width,
                               height,
                               &resolved))) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
    }
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_set_cell_pixel_geometry(
    boxrig_terminal *terminal,
    uint32_t width,
    uint32_t height)
{
    boxrig_kitty_graphics_protocol_state *state;
    size_t index;

    if (boxrig_terminal_validate_cell_pixel_geometry(
            terminal,
            width,
            height) != BOXRIG_STATUS_OK) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    state = &terminal->graphics_protocol;
    if (state->cell_pixel_width == width &&
        state->cell_pixel_height == height) {
        return BOXRIG_STATUS_OK;
    }
    if (state->placement_count != 0 &&
        !ensure_image_dirty_storage(terminal)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    /* All new views are known to be representable before old dirty or the
     * public metric revision changes. */
    mark_active_placements(terminal);
    state->cell_pixel_width = width;
    state->cell_pixel_height = height;
    increment_revision(state);
    for (index = 0; index < state->placement_count; index++) {
        boxrig_image_placement_state *placement = state->placements[index];
        boxrig_image_placement_view resolved;

        if (placement->virtual_placement) {
            size_t virtual_box_width;
            size_t virtual_box_height;

            (void)calculate_virtual_fit(
                placement,
                width,
                height,
                &virtual_box_width,
                &virtual_box_height,
                &placement->view.destination_pixel_width,
                &placement->view.destination_pixel_height);
            placement->view.revision = state->revision;
            continue;
        }
        if (placement->relative_placement) {
            (void)resolve_relative_geometry(
                terminal,
                placement,
                width,
                height,
                &resolved);
        } else {
            (void)resolve_placement_geometry(
                terminal,
                placement,
                width,
                height,
                &resolved);
        }
        resolved.revision = state->revision;
        placement->view = resolved;
        mark_placement_dirty(terminal, placement, width, height);
    }
    refresh_derived_placements(terminal, true);
    return BOXRIG_STATUS_OK;
}

uint64_t boxrig_terminal_image_revision(const boxrig_terminal *terminal)
{
    return terminal == NULL ? 0 : terminal->graphics_protocol.revision;
}

void boxrig_kitty_graphics_refresh_placeholders(boxrig_terminal *terminal)
{
    if (terminal != NULL && terminal->graphics_protocol.enabled) {
        refresh_derived_placements(terminal, false);
    }
}

size_t boxrig_terminal_image_count(const boxrig_terminal *terminal)
{
    return terminal == NULL ? 0 : terminal->graphics_protocol.image_count;
}

boxrig_status boxrig_terminal_image_handle_at(
    const boxrig_terminal *terminal,
    size_t index,
    boxrig_image_handle *handle_out)
{
    if (terminal == NULL || handle_out == NULL ||
        index >= terminal->graphics_protocol.image_count) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *handle_out = terminal->graphics_protocol.images[index]->view.handle;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_get_image(
    const boxrig_terminal *terminal,
    boxrig_image_handle handle,
    boxrig_image_view *image_out)
{
    size_t index;

    if (terminal == NULL || handle == 0 || image_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0; index < terminal->graphics_protocol.image_count; index++) {
        const boxrig_image_resource_state *resource =
            terminal->graphics_protocol.images[index];

        if (resource->view.handle == handle) {
            *image_out = resource->view;
            return BOXRIG_STATUS_OK;
        }
    }
    return BOXRIG_STATUS_INVALID_ARGUMENT;
}

size_t boxrig_terminal_image_placement_count(
    const boxrig_terminal *terminal)
{
    size_t count = 0;
    size_t index;

    if (terminal == NULL) {
        return 0;
    }
    for (index = 0;
         index < terminal->graphics_protocol.placement_count;
         index++) {
        if (placement_is_active(
                terminal,
                terminal->graphics_protocol.placements[index])) {
            count++;
        }
    }
    for (index = 0;
         index < terminal->graphics_protocol.placeholder_cell_capacity;
         index++) {
        const boxrig_placeholder_instance_state *instance =
            &terminal->graphics_protocol.placeholder_instances[index];

        if (instance->matched && instance->view.clip_width != 0 &&
            instance->view.clip_height != 0) {
            count++;
        }
    }
    return count;
}

boxrig_status boxrig_terminal_get_image_placement(
    const boxrig_terminal *terminal,
    size_t index,
    boxrig_image_placement_view *placement_out)
{
    size_t placement_index;

    if (terminal == NULL || placement_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    for (placement_index = 0;
         placement_index < terminal->graphics_protocol.placement_count;
         placement_index++) {
        const boxrig_image_placement_state *placement =
            terminal->graphics_protocol.placements[placement_index];

        if (!placement_is_active(terminal, placement)) {
            continue;
        }
        if (index == 0) {
            *placement_out = placement->view;
            return BOXRIG_STATUS_OK;
        }
        index--;
    }
    for (placement_index = 0;
         placement_index <
             terminal->graphics_protocol.placeholder_cell_capacity;
         placement_index++) {
        const boxrig_placeholder_instance_state *instance =
            &terminal->graphics_protocol
                 .placeholder_instances[placement_index];

        if (!instance->matched || instance->view.clip_width == 0 ||
            instance->view.clip_height == 0) {
            continue;
        }
        if (index == 0) {
            *placement_out = instance->view;
            return BOXRIG_STATUS_OK;
        }
        index--;
    }
    return BOXRIG_STATUS_INVALID_ARGUMENT;
}

bool boxrig_terminal_take_image_dirty(
    boxrig_terminal *terminal,
    boxrig_dirty *dirty_out)
{
    boxrig_kitty_graphics_protocol_state *protocol;
    size_t row;

    if (terminal == NULL || dirty_out == NULL ||
        terminal->screen.synchronized_output) {
        return false;
    }
    protocol = &terminal->graphics_protocol;
    for (row = 0; row < protocol->dirty_row_count; row++) {
        if (protocol->dirty_start[row] < protocol->dirty_end[row]) {
            dirty_out->row = row;
            dirty_out->column = protocol->dirty_start[row];
            dirty_out->column_count = protocol->dirty_end[row] -
                                       protocol->dirty_start[row];
            protocol->dirty_start[row] = terminal->screen.columns;
            protocol->dirty_end[row] = 0;
            return true;
        }
    }
    return false;
}

static bool placement_relative_cell_extents(
    const boxrig_kitty_graphics_protocol_state *protocol,
    const boxrig_image_placement_state *placement,
    size_t *top_out,
    size_t *bottom_out,
    size_t *left_out,
    size_t *right_out)
{
    const boxrig_image_placement_view *view = &placement->view;
    size_t pixel_bottom;
    size_t pixel_right;
    size_t bottom;
    size_t right;

    if (protocol->cell_pixel_width == 0 ||
        protocol->cell_pixel_height == 0 ||
        view->cell_y_offset > SIZE_MAX - view->destination_pixel_height ||
        view->cell_x_offset > SIZE_MAX - view->destination_pixel_width) {
        return false;
    }
    pixel_bottom = view->cell_y_offset + view->destination_pixel_height;
    pixel_right = view->cell_x_offset + view->destination_pixel_width;
    bottom = divide_rounding_up(pixel_bottom, protocol->cell_pixel_height);
    right = divide_rounding_up(pixel_right, protocol->cell_pixel_width);
    if (placement->clip_cell_bottom < bottom) {
        bottom = placement->clip_cell_bottom;
    }
    if (placement->clip_cell_right < right) {
        right = placement->clip_cell_right;
    }
    *top_out = placement->clip_cell_top;
    *bottom_out = bottom;
    *left_out = placement->clip_cell_left;
    *right_out = right;
    return *top_out < *bottom_out && *left_out < *right_out;
}

static bool placement_absolute_cell_extents(
    const boxrig_kitty_graphics_protocol_state *protocol,
    const boxrig_image_placement_state *placement,
    int64_t *top_out,
    int64_t *bottom_out,
    size_t *left_out,
    size_t *right_out)
{
    size_t relative_top;
    size_t relative_bottom;
    size_t relative_left;
    size_t relative_right;

    if (!placement_relative_cell_extents(
            protocol,
            placement,
            &relative_top,
            &relative_bottom,
            &relative_left,
            &relative_right) ||
        relative_top > (size_t)INT64_MAX ||
        relative_bottom > (size_t)INT64_MAX ||
        placement->anchor_line > INT64_MAX - (int64_t)relative_bottom ||
        placement->anchor_column < 0 ||
        (uint64_t)placement->anchor_column > SIZE_MAX ||
        (size_t)placement->anchor_column > SIZE_MAX - relative_right) {
        return false;
    }
    *top_out = placement->anchor_line + (int64_t)relative_top;
    *bottom_out = placement->anchor_line + (int64_t)relative_bottom;
    *left_out = (size_t)placement->anchor_column + relative_left;
    *right_out = (size_t)placement->anchor_column + relative_right;
    return *top_out < *bottom_out && *left_out < *right_out;
}

static bool placement_is_retained(
    const boxrig_terminal *terminal,
    const boxrig_image_placement_state *placement)
{
    int64_t top;
    int64_t bottom;
    int64_t retained_top;
    size_t left;
    size_t right;

    if (!placement_absolute_cell_extents(
            &terminal->graphics_protocol,
            placement,
            &top,
            &bottom,
            &left,
            &right)) {
        return false;
    }
    retained_top = placement->alternate_buffer
                       ? 0
                       : -(int64_t)terminal->screen.history_count;
    return bottom > retained_top && top < (int64_t)terminal->screen.rows &&
           left < terminal->screen.columns && right != 0;
}

static void mark_active_placements(boxrig_terminal *terminal)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    size_t index;

    for (index = 0; index < protocol->placement_count; index++) {
        mark_placement_dirty(
            terminal,
            protocol->placements[index],
            protocol->cell_pixel_width,
            protocol->cell_pixel_height);
    }
    for (index = 0; index < protocol->placeholder_cell_capacity; index++) {
        const boxrig_placeholder_instance_state *instance =
            &protocol->placeholder_instances[index];

        if (instance->matched && instance->view.clip_width != 0 &&
            instance->view.clip_height != 0) {
            mark_image_interval(
                terminal,
                instance->view.visual_row,
                instance->view.visual_column,
                instance->view.visual_column + 1);
        }
    }
}

static void finish_screen_mutation(boxrig_terminal *terminal)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    size_t index = 0;

    while (index < protocol->placement_count) {
        boxrig_image_placement_state *placement = protocol->placements[index];
        boxrig_image_placement_view resolved;

        if (placement->virtual_placement) {
            index++;
            continue;
        }
        if (placement->relative_placement) {
            if (resolve_relative_geometry(
                    terminal,
                    placement,
                    protocol->cell_pixel_width,
                    protocol->cell_pixel_height,
                    &resolved)) {
                placement->view = resolved;
            }
            index++;
            continue;
        }
        if (!resolve_placement_geometry(
                terminal,
                placement,
                protocol->cell_pixel_width,
                protocol->cell_pixel_height,
                &resolved) ||
            !placement_is_retained(terminal, placement)) {
            remove_placement_at(terminal, index);
            protocol->screen_mutation_changed = true;
            continue;
        }
        placement->view = resolved;
        index++;
    }
    if (protocol->screen_mutation_changed) {
        increment_revision(protocol);
        for (index = 0; index < protocol->placement_count; index++) {
            protocol->placements[index]->view.revision = protocol->revision;
        }
    }
    mark_active_placements(terminal);
    refresh_derived_placements(terminal, true);
    protocol->screen_mutation_changed = false;
}

void boxrig_kitty_graphics_viewport_will_change(boxrig_terminal *terminal)
{
    boxrig_kitty_graphics_protocol_state *protocol;
    size_t index;

    if (terminal == NULL) {
        return;
    }
    protocol = &terminal->graphics_protocol;
    mark_active_placements(terminal);
    for (index = 0; index < protocol->placement_count; index++) {
        if (!protocol->placements[index]->alternate_buffer) {
            protocol->screen_mutation_changed = true;
            break;
        }
    }
}

void boxrig_kitty_graphics_viewport_did_change(boxrig_terminal *terminal)
{
    if (terminal != NULL) {
        finish_screen_mutation(terminal);
    }
}

void boxrig_kitty_graphics_screen_scroll_will_change(
    boxrig_terminal *terminal,
    size_t top,
    size_t bottom,
    size_t count,
    bool downward)
{
    boxrig_kitty_graphics_protocol_state *protocol;
    size_t page_left;
    size_t page_right;
    size_t index;
    bool full_primary_history_scroll;

    if (terminal == NULL || count == 0) {
        return;
    }
    protocol = &terminal->graphics_protocol;
    page_left = terminal->screen.left_right_margin_mode
                    ? terminal->screen.active->horizontal_margin_left
                    : 0;
    page_right = terminal->screen.left_right_margin_mode
                     ? terminal->screen.active->horizontal_margin_right + 1
                     : terminal->screen.columns;
    full_primary_history_scroll =
        !terminal->screen.alternate_active && !downward && top == 0 &&
        bottom + 1 == terminal->screen.rows && page_left == 0 &&
        page_right == terminal->screen.columns;
    mark_active_placements(terminal);
    for (index = 0; index < protocol->placement_count; index++) {
        boxrig_image_placement_state *placement = protocol->placements[index];
        int64_t placement_top;
        int64_t placement_bottom;
        size_t placement_left;
        size_t placement_right;
        bool moves;

        if (placement->alternate_buffer !=
                terminal->screen.alternate_active ||
            placement->virtual_placement ||
            placement->relative_placement ||
            !placement_absolute_cell_extents(
                protocol,
                placement,
                &placement_top,
                &placement_bottom,
                &placement_left,
                &placement_right)) {
            continue;
        }
        moves = full_primary_history_scroll ||
                (placement_top >= (int64_t)top &&
                 placement_bottom <= (int64_t)bottom + 1 &&
                 placement_left >= page_left &&
                 placement_right <= page_right);
        if (!moves) {
            continue;
        }
        if (downward) {
            if (placement->anchor_line > INT64_MAX - (int64_t)count) {
                continue;
            }
            placement->anchor_line += (int64_t)count;
        } else {
            if (placement->anchor_line < INT64_MIN + (int64_t)count) {
                continue;
            }
            placement->anchor_line -= (int64_t)count;
        }
        if (!full_primary_history_scroll) {
            if (placement->anchor_line < (int64_t)top) {
                size_t clip_top = (size_t)((int64_t)top -
                                           placement->anchor_line);

                if (placement->clip_cell_top < clip_top) {
                    placement->clip_cell_top = clip_top;
                }
            }
            if (placement->anchor_line < (int64_t)bottom + 1) {
                size_t clip_bottom = (size_t)((int64_t)bottom + 1 -
                                              placement->anchor_line);

                if (placement->clip_cell_bottom > clip_bottom) {
                    placement->clip_cell_bottom = clip_bottom;
                }
            } else {
                placement->clip_cell_bottom = 0;
            }
        }
        protocol->screen_mutation_changed = true;
    }
}

void boxrig_kitty_graphics_screen_scroll_did_change(boxrig_terminal *terminal)
{
    if (terminal != NULL) {
        finish_screen_mutation(terminal);
    }
}

void boxrig_kitty_graphics_screen_erase_display(
    boxrig_terminal *terminal,
    uint32_t mode)
{
    boxrig_kitty_graphics_protocol_state *protocol;
    size_t index = 0;

    if (terminal == NULL || (mode != 2 && mode != 3)) {
        return;
    }
    protocol = &terminal->graphics_protocol;
    if (mode == 2) {
        for (;;) {
            boxrig_image_placement_state *candidate = NULL;

            for (index = 0; index < protocol->placement_count; index++) {
                boxrig_image_placement_state *placement =
                    protocol->placements[index];
                boxrig_image_placement_state *ancestor = placement->parent;
                bool active_ancestor = false;

                if (!placement_is_active(terminal, placement)) {
                    continue;
                }
                while (ancestor != NULL) {
                    if (placement_is_active(terminal, ancestor)) {
                        active_ancestor = true;
                        break;
                    }
                    ancestor = ancestor->parent;
                }
                if (!active_ancestor) {
                    candidate = placement;
                    break;
                }
            }
            if (candidate == NULL) {
                break;
            }
            remove_placement_pointer(terminal, candidate);
            protocol->screen_mutation_changed = true;
        }
        finish_screen_mutation(terminal);
        return;
    }
    if (mode == 3) {
        mark_active_placements(terminal);
    }
    while (index < protocol->placement_count) {
        boxrig_image_placement_state *placement = protocol->placements[index];
        bool remove = false;

        if (placement->virtual_placement || placement->relative_placement) {
            index++;
            continue;
        }
        if (!placement->alternate_buffer) {
            int64_t top;
            int64_t bottom;
            size_t left;
            size_t right;

            remove = placement_absolute_cell_extents(
                         protocol,
                         placement,
                         &top,
                         &bottom,
                         &left,
                         &right) &&
                     bottom <= 0;
            protocol->screen_mutation_changed = true;
        }
        if (remove) {
            remove_placement_at(terminal, index);
            protocol->screen_mutation_changed = true;
        } else {
            index++;
        }
    }
}

void boxrig_kitty_graphics_screen_buffer_will_switch(
    boxrig_terminal *terminal)
{
    if (terminal == NULL) {
        return;
    }
    mark_active_placements(terminal);
    terminal->graphics_protocol.screen_mutation_changed =
        terminal->graphics_protocol.placement_count != 0;
}

void boxrig_kitty_graphics_screen_buffer_did_switch(
    boxrig_terminal *terminal)
{
    if (terminal != NULL) {
        finish_screen_mutation(terminal);
    }
}

void boxrig_kitty_graphics_screen_buffer_reset(
    boxrig_terminal *terminal,
    bool alternate)
{
    boxrig_kitty_graphics_protocol_state *protocol;
    size_t index;

    if (terminal == NULL) {
        return;
    }
    protocol = &terminal->graphics_protocol;
    for (;;) {
        boxrig_image_placement_state *candidate = NULL;

        for (index = 0; index < protocol->placement_count; index++) {
            boxrig_image_placement_state *placement =
                protocol->placements[index];

            if (placement->alternate_buffer == alternate &&
                (placement->parent == NULL ||
                 placement->parent->alternate_buffer != alternate)) {
                candidate = placement;
                break;
            }
        }
        if (candidate == NULL) {
            break;
        }
        remove_placement_pointer(terminal, candidate);
        protocol->screen_mutation_changed = true;
    }
    finish_screen_mutation(terminal);
}

static void mark_cached_placement_view(
    boxrig_terminal *terminal,
    const boxrig_image_placement_state *placement)
{
    const boxrig_image_placement_view *view = &placement->view;
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    size_t row_count;
    size_t column_count;
    size_t end_column;
    size_t row;

    if (placement->alternate_buffer != terminal->screen.alternate_active ||
        view->clip_width == 0 || view->clip_height == 0 ||
        protocol->cell_pixel_width == 0 ||
        protocol->cell_pixel_height == 0) {
        return;
    }
    row_count = divide_rounding_up(
        view->clip_height,
        protocol->cell_pixel_height);
    column_count = divide_rounding_up(
        view->clip_width,
        protocol->cell_pixel_width);
    if (row_count < SIZE_MAX) {
        row_count++;
    }
    if (column_count < SIZE_MAX) {
        column_count++;
    }
    if (view->visual_column >= terminal->screen.columns) {
        return;
    }
    end_column = column_count >
                         terminal->screen.columns - view->visual_column
                     ? terminal->screen.columns
                     : view->visual_column + column_count;
    for (row = view->visual_row;
         row < terminal->screen.rows && row - view->visual_row < row_count;
         row++) {
        mark_image_interval(
            terminal,
            row,
            view->visual_column,
            end_column);
    }
}

boxrig_status boxrig_kitty_graphics_protocol_resize(
    boxrig_terminal *terminal,
    size_t rows,
    size_t columns)
{
    boxrig_kitty_graphics_protocol_state *protocol =
        &terminal->graphics_protocol;
    boxrig_screen_reflow_marker *markers = NULL;
    boxrig_placeholder_instance_state *placeholder_instances = NULL;
    size_t *dirty_start;
    size_t *dirty_end;
    size_t marker_capacity;
    size_t marker_count = 0;
    size_t index;
    size_t preserved_rows;
    bool preserve_viewport;
    boxrig_status status;

    if (!protocol->enabled) {
        return boxrig_screen_resize(&terminal->screen, rows, columns);
    }
    if (rows == terminal->screen.rows && columns == terminal->screen.columns) {
        return boxrig_screen_resize(&terminal->screen, rows, columns);
    }
    preserve_viewport = terminal->screen.viewport_offset != 0;
    if (protocol->placement_count > SIZE_MAX - (preserve_viewport ? 1u : 0u)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    marker_capacity = protocol->placement_count +
                      (preserve_viewport ? 1u : 0u);
    if ((protocol->cell_pixel_width != 0 &&
         columns > SIZE_MAX / protocol->cell_pixel_width) ||
        (protocol->cell_pixel_height != 0 &&
         rows > SIZE_MAX / protocol->cell_pixel_height) ||
        rows > SIZE_MAX / columns ||
        rows > SIZE_MAX / sizeof(*dirty_start) ||
        marker_capacity > SIZE_MAX / sizeof(*markers)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    dirty_start = malloc(rows * sizeof(*dirty_start));
    dirty_end = malloc(rows * sizeof(*dirty_end));
    if (marker_capacity != 0) {
        markers = malloc(marker_capacity * sizeof(*markers));
    }
    if (protocol->placeholder_instances != NULL) {
        size_t cell_count = rows * columns;

        if (cell_count > SIZE_MAX / sizeof(*placeholder_instances)) {
            free(markers);
            free(dirty_end);
            free(dirty_start);
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        placeholder_instances = calloc(
            cell_count,
            sizeof(*placeholder_instances));
    }
    if (dirty_start == NULL || dirty_end == NULL ||
        (marker_capacity != 0 && markers == NULL) ||
        (protocol->placeholder_instances != NULL &&
         placeholder_instances == NULL)) {
        free(placeholder_instances);
        free(markers);
        free(dirty_end);
        free(dirty_start);
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    for (index = 0; index < rows; index++) {
        dirty_start[index] = columns;
        dirty_end[index] = 0;
    }
    if (preserve_viewport) {
        boxrig_screen_prepare_viewport_reflow_marker(
            &terminal->screen,
            &markers[marker_count]);
        markers[marker_count].context = NULL;
        marker_count++;
    }
    for (index = 0; index < protocol->placement_count; index++) {
        boxrig_image_placement_state *placement = protocol->placements[index];
        int64_t marker_line;

        if (placement->alternate_buffer || placement->virtual_placement ||
            placement->relative_placement) {
            continue;
        }
        marker_line = placement->anchor_line;
        if (marker_line < -(int64_t)terminal->screen.history_count) {
            marker_line = -(int64_t)terminal->screen.history_count;
        }
        if (marker_line >= (int64_t)terminal->screen.rows ||
            placement->anchor_column < 0 ||
            (size_t)placement->anchor_column >= terminal->screen.columns) {
            continue;
        }
        markers[marker_count].context = placement;
        markers[marker_count].source_row =
            (size_t)(marker_line +
                     (int64_t)terminal->screen.history_count);
        markers[marker_count].source_column =
            (size_t)placement->anchor_column;
        markers[marker_count].logical_offset = 0;
        markers[marker_count].anchor_delta = marker_line -
                                             placement->anchor_line;
        markers[marker_count].destination_row = 0;
        markers[marker_count].destination_column = 0;
        marker_count++;
    }
    status = boxrig_screen_resize_with_markers(
        &terminal->screen,
        rows,
        columns,
        markers,
        marker_count);
    if (status != BOXRIG_STATUS_OK) {
        free(placeholder_instances);
        free(markers);
        free(dirty_end);
        free(dirty_start);
        return status;
    }
    if (preserve_viewport) {
        for (index = 0; index < marker_count; index++) {
            if (markers[index].context == NULL) {
                boxrig_screen_apply_viewport_reflow_marker(
                    &terminal->screen,
                    &markers[index]);
                break;
            }
        }
    }
    preserved_rows = protocol->dirty_row_count < rows
                         ? protocol->dirty_row_count
                         : rows;
    for (index = 0; index < preserved_rows; index++) {
        size_t start = protocol->dirty_start[index];
        size_t end = protocol->dirty_end[index];

        if (start < end && start < columns) {
            dirty_start[index] = start;
            dirty_end[index] = end < columns ? end : columns;
        }
    }
    free(protocol->dirty_start);
    free(protocol->dirty_end);
    protocol->dirty_start = dirty_start;
    protocol->dirty_end = dirty_end;
    protocol->dirty_row_count = rows;
    for (index = 0; index < protocol->placeholder_cell_capacity; index++) {
        const boxrig_placeholder_instance_state *instance =
            &protocol->placeholder_instances[index];

        if (instance->matched && instance->view.clip_width != 0 &&
            instance->view.clip_height != 0) {
            mark_image_interval(
                terminal,
                instance->view.visual_row,
                instance->view.visual_column,
                instance->view.visual_column + 1);
        }
    }
    if (protocol->placeholder_instances != NULL) {
        free(protocol->placeholder_instances);
        protocol->placeholder_instances = placeholder_instances;
        protocol->placeholder_cell_capacity = rows * columns;
        placeholder_instances = NULL;
    }
    for (index = 0; index < protocol->placement_count; index++) {
        mark_cached_placement_view(terminal, protocol->placements[index]);
    }
    for (index = 0; index < marker_count; index++) {
        boxrig_image_placement_state *placement = markers[index].context;

        if (placement == NULL) {
            continue;
        }
        placement->anchor_line = markers[index].destination_row -
                                 markers[index].anchor_delta;
        placement->anchor_column = (int64_t)markers[index].destination_column;
        protocol->screen_mutation_changed = true;
    }
    for (index = 0; index < protocol->placement_count; index++) {
        boxrig_image_placement_state *placement = protocol->placements[index];

        if (!placement->alternate_buffer || placement->virtual_placement ||
            placement->relative_placement) {
            continue;
        }
        if (placement->anchor_line >= 0 &&
            placement->anchor_line < (int64_t)rows) {
            size_t bottom = rows - (size_t)placement->anchor_line;

            if (placement->clip_cell_bottom > bottom) {
                placement->clip_cell_bottom = bottom;
            }
        } else {
            placement->clip_cell_bottom = 0;
        }
        if (placement->anchor_column >= 0 &&
            (size_t)placement->anchor_column < columns) {
            size_t right = columns - (size_t)placement->anchor_column;

            if (placement->clip_cell_right > right) {
                placement->clip_cell_right = right;
            }
        } else {
            placement->clip_cell_right = 0;
        }
        protocol->screen_mutation_changed = true;
    }
    free(markers);
    free(placeholder_instances);
    finish_screen_mutation(terminal);
    return BOXRIG_STATUS_OK;
}
