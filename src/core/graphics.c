/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "graphics.h"

#include <string.h>

static bool lookup_key(
    uint8_t key,
    boxrig_kitty_graphics_field *field_out,
    boxrig_kitty_graphics_value_kind *kind_out)
{
    switch (key) {
    case 'a':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_ACTION;
        *kind_out = BOXRIG_KITTY_GRAPHICS_VALUE_CHARACTER;
        return true;
    case 't':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_TRANSMISSION;
        *kind_out = BOXRIG_KITTY_GRAPHICS_VALUE_CHARACTER;
        return true;
    case 'o':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_COMPRESSION;
        *kind_out = BOXRIG_KITTY_GRAPHICS_VALUE_CHARACTER;
        return true;
    case 'd':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_DELETE_ACTION;
        *kind_out = BOXRIG_KITTY_GRAPHICS_VALUE_CHARACTER;
        return true;
    case 'q':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_QUIET;
        break;
    case 'f':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_FORMAT;
        break;
    case 's':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_DATA_WIDTH;
        break;
    case 'v':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_DATA_HEIGHT;
        break;
    case 'S':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_DATA_SIZE;
        break;
    case 'O':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_DATA_OFFSET;
        break;
    case 'i':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID;
        break;
    case 'I':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER;
        break;
    case 'p':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_PLACEMENT_ID;
        break;
    case 'm':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_MORE;
        break;
    case 'N':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_USAGE_HINTS;
        break;
    case 'x':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_X;
        break;
    case 'y':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_Y;
        break;
    case 'w':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_WIDTH;
        break;
    case 'h':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_HEIGHT;
        break;
    case 'X':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_CELL_X_OFFSET;
        break;
    case 'Y':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_CELL_Y_OFFSET;
        break;
    case 'c':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_COLUMNS;
        break;
    case 'r':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_ROWS;
        break;
    case 'C':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_CURSOR_MOVEMENT;
        break;
    case 'U':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_VIRTUAL_PLACEMENT;
        break;
    case 'P':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_PARENT_IMAGE_ID;
        break;
    case 'Q':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_PARENT_PLACEMENT_ID;
        break;
    case 'z':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_Z_INDEX;
        *kind_out = BOXRIG_KITTY_GRAPHICS_VALUE_SIGNED;
        return true;
    case 'H':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_RELATIVE_X_OFFSET;
        *kind_out = BOXRIG_KITTY_GRAPHICS_VALUE_SIGNED;
        return true;
    case 'V':
        *field_out = BOXRIG_KITTY_GRAPHICS_FIELD_RELATIVE_Y_OFFSET;
        *kind_out = BOXRIG_KITTY_GRAPHICS_VALUE_SIGNED;
        return true;
    default:
        *kind_out = BOXRIG_KITTY_GRAPHICS_VALUE_UNKNOWN;
        return false;
    }
    *kind_out = BOXRIG_KITTY_GRAPHICS_VALUE_UNSIGNED;
    return true;
}

static void set_character_value(
    boxrig_kitty_graphics_command *command,
    boxrig_kitty_graphics_field field,
    uint8_t value)
{
    if (field == BOXRIG_KITTY_GRAPHICS_FIELD_ACTION) {
        command->action = value;
    } else if (field == BOXRIG_KITTY_GRAPHICS_FIELD_TRANSMISSION) {
        command->transmission = value;
    } else if (field == BOXRIG_KITTY_GRAPHICS_FIELD_COMPRESSION) {
        command->compression = value;
    } else if (field == BOXRIG_KITTY_GRAPHICS_FIELD_DELETE_ACTION) {
        command->delete_action = value;
    }
}

static void set_unsigned_value(
    boxrig_kitty_graphics_command *command,
    boxrig_kitty_graphics_field field,
    uint32_t value)
{
    switch (field) {
    case BOXRIG_KITTY_GRAPHICS_FIELD_QUIET:
        command->quiet = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_FORMAT:
        command->format = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_DATA_WIDTH:
        command->data_width = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_DATA_HEIGHT:
        command->data_height = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_DATA_SIZE:
        command->data_size = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_DATA_OFFSET:
        command->data_offset = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID:
        command->image_id = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_NUMBER:
        command->image_number = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_PLACEMENT_ID:
        command->placement_id = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_MORE:
        command->more = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_USAGE_HINTS:
        command->usage_hints = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_X:
        command->source_x = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_Y:
        command->source_y = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_WIDTH:
        command->source_width = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_SOURCE_HEIGHT:
        command->source_height = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_CELL_X_OFFSET:
        command->cell_x_offset = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_CELL_Y_OFFSET:
        command->cell_y_offset = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_COLUMNS:
        command->columns = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_ROWS:
        command->rows = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_CURSOR_MOVEMENT:
        command->cursor_movement = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_VIRTUAL_PLACEMENT:
        command->virtual_placement = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_PARENT_IMAGE_ID:
        command->parent_image_id = value;
        break;
    case BOXRIG_KITTY_GRAPHICS_FIELD_PARENT_PLACEMENT_ID:
        command->parent_placement_id = value;
        break;
    default:
        break;
    }
}

static void set_signed_value(
    boxrig_kitty_graphics_command *command,
    boxrig_kitty_graphics_field field,
    int32_t value)
{
    if (field == BOXRIG_KITTY_GRAPHICS_FIELD_Z_INDEX) {
        command->z_index = value;
    } else if (field == BOXRIG_KITTY_GRAPHICS_FIELD_RELATIVE_X_OFFSET) {
        command->relative_x_offset = value;
    } else if (field == BOXRIG_KITTY_GRAPHICS_FIELD_RELATIVE_Y_OFFSET) {
        command->relative_y_offset = value;
    }
}

static void mark_present(
    boxrig_kitty_graphics_command *command,
    boxrig_kitty_graphics_field field)
{
    command->presence |= UINT64_C(1) << (unsigned int)field;
}

static bool finish_value(boxrig_kitty_graphics_header *state)
{
    int32_t signed_value;

    if (state->value_length == 0) {
        return false;
    }
    if (state->value_kind == BOXRIG_KITTY_GRAPHICS_VALUE_UNKNOWN) {
        return true;
    }
    if (state->value_kind == BOXRIG_KITTY_GRAPHICS_VALUE_CHARACTER) {
        if (state->value_length != 1) {
            return false;
        }
        set_character_value(
            &state->command,
            state->field,
            state->character_value);
    } else if (state->value_kind == BOXRIG_KITTY_GRAPHICS_VALUE_UNSIGNED) {
        if (state->digit_count == 0) {
            return false;
        }
        set_unsigned_value(&state->command, state->field, state->magnitude);
    } else {
        if (state->digit_count == 0) {
            return false;
        }
        if (state->negative) {
            uint32_t minimum_magnitude = (uint32_t)INT32_MAX + UINT32_C(1);

            signed_value = state->magnitude == minimum_magnitude
                               ? INT32_MIN
                               : -(int32_t)state->magnitude;
        } else {
            signed_value = (int32_t)state->magnitude;
        }
        set_signed_value(&state->command, state->field, signed_value);
    }
    mark_present(&state->command, state->field);
    return true;
}

static void begin_key(boxrig_kitty_graphics_header *state)
{
    state->key_length = 0;
    state->value_length = 0;
    state->digit_count = 0;
    state->magnitude = 0;
    state->key = 0;
    state->character_value = 0;
    state->negative = false;
    state->value_kind = BOXRIG_KITTY_GRAPHICS_VALUE_UNKNOWN;
    state->stage = BOXRIG_KITTY_GRAPHICS_HEADER_KEY;
}

static bool put_digit(
    boxrig_kitty_graphics_header *state,
    uint8_t byte,
    uint32_t limit)
{
    uint32_t digit = (uint32_t)(byte - (uint8_t)'0');

    if (state->magnitude > (limit - digit) / 10) {
        return false;
    }
    state->magnitude = state->magnitude * 10 + digit;
    state->digit_count++;
    return true;
}

void boxrig_kitty_graphics_header_initialize(
    boxrig_kitty_graphics_header *state)
{
    memset(state, 0, sizeof(*state));
    state->valid = true;
}

bool boxrig_kitty_graphics_header_put(
    boxrig_kitty_graphics_header *state,
    uint8_t byte)
{
    uint32_t limit;

    if (state == NULL || !state->valid || state->finished ||
        state->length >= BOXRIG_KITTY_GRAPHICS_HEADER_CAPACITY ||
        byte < 0x20 || byte > 0x7e || byte == (uint8_t)';') {
        if (state != NULL) {
            state->valid = false;
        }
        return false;
    }
    state->length++;
    if (state->stage == BOXRIG_KITTY_GRAPHICS_HEADER_KEY) {
        if (byte == (uint8_t)'=') {
            if (state->key_length != 1) {
                state->valid = false;
                return false;
            }
            (void)lookup_key(state->key, &state->field, &state->value_kind);
            state->stage = BOXRIG_KITTY_GRAPHICS_HEADER_VALUE;
            return true;
        }
        if (byte == (uint8_t)',' || state->key_length != 0) {
            state->valid = false;
            return false;
        }
        state->key = byte;
        state->key_length = 1;
        return true;
    }
    if (byte == (uint8_t)',') {
        if (!finish_value(state)) {
            state->valid = false;
            return false;
        }
        begin_key(state);
        return true;
    }
    if (state->value_kind == BOXRIG_KITTY_GRAPHICS_VALUE_CHARACTER) {
        if (state->value_length != 0) {
            state->valid = false;
            return false;
        }
        state->character_value = byte;
    } else if (state->value_kind == BOXRIG_KITTY_GRAPHICS_VALUE_UNSIGNED) {
        if (byte < (uint8_t)'0' || byte > (uint8_t)'9' ||
            !put_digit(state, byte, UINT32_MAX)) {
            state->valid = false;
            return false;
        }
    } else if (state->value_kind == BOXRIG_KITTY_GRAPHICS_VALUE_SIGNED) {
        if (state->value_length == 0 &&
            (byte == (uint8_t)'+' || byte == (uint8_t)'-')) {
            state->negative = byte == (uint8_t)'-';
        } else {
            limit = state->negative
                        ? (uint32_t)INT32_MAX + UINT32_C(1)
                        : (uint32_t)INT32_MAX;
            if (byte < (uint8_t)'0' || byte > (uint8_t)'9' ||
                !put_digit(state, byte, limit)) {
                state->valid = false;
                return false;
            }
        }
    }
    state->value_length++;
    return true;
}

bool boxrig_kitty_graphics_header_finish(
    boxrig_kitty_graphics_header *state,
    boxrig_kitty_graphics_command *command_out)
{
    if (state == NULL || command_out == NULL || !state->valid) {
        return false;
    }
    if (!state->finished) {
        if (state->length != 0 &&
            (state->stage != BOXRIG_KITTY_GRAPHICS_HEADER_VALUE ||
             !finish_value(state))) {
            state->valid = false;
            return false;
        }
        state->finished = true;
    }
    *command_out = state->command;
    return true;
}

bool boxrig_kitty_graphics_command_has(
    const boxrig_kitty_graphics_command *command,
    boxrig_kitty_graphics_field field)
{
    return command != NULL && field >= BOXRIG_KITTY_GRAPHICS_FIELD_ACTION &&
           field < BOXRIG_KITTY_GRAPHICS_FIELD_COUNT &&
           (command->presence &
            (UINT64_C(1) << (unsigned int)field)) != 0;
}

static boxrig_status unsupported_begin(
    void *context,
    boxrig_kitty_graphics_command command)
{
    (void)context;
    (void)command;
    return BOXRIG_STATUS_OK;
}

static boxrig_status unsupported_put(void *context, uint8_t byte)
{
    (void)context;
    (void)byte;
    return BOXRIG_STATUS_OK;
}

static boxrig_status unsupported_commit(void *context)
{
    (void)context;
    return BOXRIG_STATUS_OK;
}

static void unsupported_abort(void *context)
{
    (void)context;
}

static boxrig_kitty_graphics_consumer unsupported_consumer(void)
{
    boxrig_kitty_graphics_consumer consumer;

    consumer.context = NULL;
    consumer.begin = unsupported_begin;
    consumer.put = unsupported_put;
    consumer.commit = unsupported_commit;
    consumer.abort = unsupported_abort;
    return consumer;
}

static void clear_transaction(boxrig_kitty_graphics_state *state)
{
    boxrig_kitty_graphics_consumer consumer = state->consumer;

    memset(state, 0, sizeof(*state));
    state->consumer = consumer;
}

static void abort_consumer(boxrig_kitty_graphics_state *state)
{
    if (state->consumer_active) {
        state->consumer.abort(state->consumer.context);
        state->consumer_active = false;
    }
}

static boxrig_status begin_consumer(
    boxrig_kitty_graphics_state *state,
    boxrig_kitty_graphics_stage next_stage)
{
    boxrig_status status = state->consumer.begin(
        state->consumer.context,
        state->command);

    if (status == BOXRIG_STATUS_OK) {
        state->consumer_active = true;
        state->stage = next_stage;
    }
    return status;
}

static boxrig_status commit_consumer(boxrig_kitty_graphics_state *state)
{
    boxrig_status status = state->consumer.commit(state->consumer.context);

    if (status == BOXRIG_STATUS_OK) {
        state->consumer_active = false;
        clear_transaction(state);
    }
    return status;
}

void boxrig_kitty_graphics_initialize(boxrig_kitty_graphics_state *state)
{
    memset(state, 0, sizeof(*state));
    state->consumer = unsupported_consumer();
}

void boxrig_kitty_graphics_set_consumer(
    boxrig_kitty_graphics_state *state,
    const boxrig_kitty_graphics_consumer *consumer)
{
    boxrig_kitty_graphics_reset(state);
    state->consumer = *consumer;
}

void boxrig_kitty_graphics_start(boxrig_kitty_graphics_state *state)
{
    boxrig_kitty_graphics_reset(state);
    state->active = true;
    state->stage = BOXRIG_KITTY_GRAPHICS_SELECT;
}

boxrig_status boxrig_kitty_graphics_put(
    boxrig_kitty_graphics_state *state,
    uint8_t byte)
{
    boxrig_status status;

    if (state == NULL || !state->active) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (state->stage == BOXRIG_KITTY_GRAPHICS_SELECT) {
        if (byte == (uint8_t)'G') {
            boxrig_kitty_graphics_header_initialize(&state->header);
            state->stage = BOXRIG_KITTY_GRAPHICS_HEADER;
        } else {
            state->stage = BOXRIG_KITTY_GRAPHICS_IGNORE;
        }
        return BOXRIG_STATUS_OK;
    }
    if (state->stage == BOXRIG_KITTY_GRAPHICS_HEADER) {
        if (byte == (uint8_t)';') {
            if (!boxrig_kitty_graphics_header_finish(
                    &state->header,
                    &state->command)) {
                state->stage = BOXRIG_KITTY_GRAPHICS_IGNORE;
                return BOXRIG_STATUS_OK;
            }
            state->stage = BOXRIG_KITTY_GRAPHICS_BEGIN_PAYLOAD;
            return begin_consumer(state, BOXRIG_KITTY_GRAPHICS_PAYLOAD);
        }
        if (!boxrig_kitty_graphics_header_put(&state->header, byte)) {
            state->stage = BOXRIG_KITTY_GRAPHICS_IGNORE;
        }
        return BOXRIG_STATUS_OK;
    }
    if (state->stage == BOXRIG_KITTY_GRAPHICS_BEGIN_PAYLOAD) {
        if (byte != (uint8_t)';') {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        return begin_consumer(state, BOXRIG_KITTY_GRAPHICS_PAYLOAD);
    }
    if (state->stage == BOXRIG_KITTY_GRAPHICS_PAYLOAD) {
        if (state->payload_length >= BOXRIG_KITTY_GRAPHICS_PAYLOAD_CAPACITY) {
            abort_consumer(state);
            state->stage = BOXRIG_KITTY_GRAPHICS_IGNORE;
            return BOXRIG_STATUS_OK;
        }
        status = state->consumer.put(state->consumer.context, byte);
        if (status == BOXRIG_STATUS_OK) {
            state->payload_length++;
        }
        return status;
    }
    if (state->stage == BOXRIG_KITTY_GRAPHICS_IGNORE) {
        return BOXRIG_STATUS_OK;
    }
    return BOXRIG_STATUS_INVALID_ARGUMENT;
}

boxrig_status boxrig_kitty_graphics_end(
    boxrig_kitty_graphics_state *state,
    bool cancelled)
{
    boxrig_status status;

    if (state == NULL || !state->active) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (cancelled) {
        boxrig_kitty_graphics_reset(state);
        return BOXRIG_STATUS_OK;
    }
    if (state->stage == BOXRIG_KITTY_GRAPHICS_SELECT ||
        state->stage == BOXRIG_KITTY_GRAPHICS_IGNORE) {
        clear_transaction(state);
        return BOXRIG_STATUS_OK;
    }
    if (state->stage == BOXRIG_KITTY_GRAPHICS_HEADER) {
        if (!boxrig_kitty_graphics_header_finish(
                &state->header,
                &state->command)) {
            clear_transaction(state);
            return BOXRIG_STATUS_OK;
        }
        state->stage = BOXRIG_KITTY_GRAPHICS_BEGIN_EMPTY;
    }
    if (state->stage == BOXRIG_KITTY_GRAPHICS_BEGIN_EMPTY) {
        status = begin_consumer(state, BOXRIG_KITTY_GRAPHICS_COMMIT);
        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
    } else if (state->stage == BOXRIG_KITTY_GRAPHICS_PAYLOAD) {
        state->stage = BOXRIG_KITTY_GRAPHICS_COMMIT;
    }
    if (state->stage == BOXRIG_KITTY_GRAPHICS_COMMIT) {
        return commit_consumer(state);
    }
    return BOXRIG_STATUS_INVALID_ARGUMENT;
}

void boxrig_kitty_graphics_reset(boxrig_kitty_graphics_state *state)
{
    if (state == NULL) {
        return;
    }
    abort_consumer(state);
    clear_transaction(state);
}
