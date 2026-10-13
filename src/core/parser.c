/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "parser.h"

#include <limits.h>
#include <string.h>

static void clear_sequence(boxrig_parser *parser)
{
    parser->parameter_count = 0;
    parser->intermediate_count = 0;
    parser->private_marker = 0;
}

static bool is_c0(uint8_t byte)
{
    return byte <= 0x1f;
}

static bool is_final(uint8_t byte)
{
    return byte >= 0x40 && byte <= 0x7e;
}

static bool append_intermediate(boxrig_parser *parser, uint8_t byte)
{
    if (parser->intermediate_count >= BOXRIG_PARSER_MAX_INTERMEDIATES) {
        return false;
    }
    parser->intermediates[parser->intermediate_count] = byte;
    parser->intermediate_count++;
    return true;
}

static bool append_parameter(boxrig_parser *parser, bool subparameter)
{
    boxrig_parser_parameter *parameter;

    if (parser->parameter_count >= BOXRIG_PARSER_MAX_PARAMETERS) {
        return false;
    }
    parameter = &parser->parameters[parser->parameter_count];
    parameter->value = 0;
    parameter->has_value = false;
    parameter->subparameter = subparameter;
    parser->parameter_count++;
    return true;
}

static bool append_digit(boxrig_parser *parser, uint8_t byte)
{
    boxrig_parser_parameter *parameter;
    uint32_t digit = (uint32_t)(byte - (uint8_t)'0');

    if (parser->parameter_count == 0 && !append_parameter(parser, false)) {
        return false;
    }
    parameter = &parser->parameters[parser->parameter_count - 1];
    if (parameter->value > (UINT32_MAX - digit) / 10u) {
        return false;
    }
    parameter->value = parameter->value * 10u + digit;
    parameter->has_value = true;
    return true;
}

static boxrig_status begin_string(
    boxrig_parser *parser,
    boxrig_parser_string_kind kind)
{
    boxrig_status status = parser->actions.string_start(
        parser->actions.context,
        kind);

    if (status == BOXRIG_STATUS_OK) {
        parser->string_kind = kind;
        parser->state = BOXRIG_PARSER_STRING;
        parser->string_length = 0;
        parser->string_escape_delivered = false;
        parser->string_discarding = false;
    }
    return status;
}

static void begin_dcs(boxrig_parser *parser)
{
    clear_sequence(parser);
    parser->state = BOXRIG_PARSER_DCS_ENTRY;
    parser->dcs_length = 0;
    parser->dcs_escape_delivered = false;
}

static boxrig_status finish_string(
    boxrig_parser *parser,
    bool cancelled,
    boxrig_parser_string_terminator terminator)
{
    boxrig_status status = parser->string_discarding
                               ? BOXRIG_STATUS_OK
                               : parser->actions.string_end(
                                     parser->actions.context,
                                     cancelled,
                                     terminator);

    if (status == BOXRIG_STATUS_OK) {
        parser->state = BOXRIG_PARSER_GROUND;
        parser->string_length = 0;
        parser->string_escape_delivered = false;
        parser->string_discarding = false;
    }
    return status;
}

static boxrig_status put_string_byte(boxrig_parser *parser, uint8_t byte)
{
    boxrig_status status;

    if (parser->string_discarding) {
        return BOXRIG_STATUS_OK;
    }
    if (parser->string_length >= parser->string_capacity) {
        /*
         * Cancellation belongs to the byte that first crosses the limit.
         * The discarded terminator must not deliver a second end action.
         */
        status = parser->actions.string_end(
            parser->actions.context,
            true,
            BOXRIG_PARSER_STRING_TERMINATOR_CANCEL);
        if (status == BOXRIG_STATUS_OK) {
            parser->string_discarding = true;
        }
        return status;
    }
    status = parser->actions.string_put(parser->actions.context, byte);
    if (status == BOXRIG_STATUS_OK) {
        parser->string_length++;
    }
    return status;
}

static boxrig_status feed_string_byte(boxrig_parser *parser, uint8_t byte)
{
    boxrig_status status;

    if (parser->state == BOXRIG_PARSER_STRING) {
        if (parser->string_kind == BOXRIG_PARSER_STRING_OSC && byte == 0x07) {
            return finish_string(
                parser,
                false,
                BOXRIG_PARSER_STRING_TERMINATOR_BEL);
        }
        if (byte == 0x18 || byte == 0x1a) {
            return finish_string(
                parser,
                true,
                BOXRIG_PARSER_STRING_TERMINATOR_CANCEL);
        }
        if (byte == 0x1b) {
            parser->state = BOXRIG_PARSER_STRING_ESCAPE;
            parser->string_escape_delivered = false;
            return BOXRIG_STATUS_OK;
        }
        return put_string_byte(parser, byte);
    }

    if (byte == (uint8_t)'\\') {
        return finish_string(
            parser,
            false,
            BOXRIG_PARSER_STRING_TERMINATOR_ST);
    }
    if (byte == 0x18 || byte == 0x1a) {
        return finish_string(
            parser,
            true,
            BOXRIG_PARSER_STRING_TERMINATOR_CANCEL);
    }
    if (!parser->string_escape_delivered) {
        status = put_string_byte(parser, 0x1b);
        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
        parser->string_escape_delivered = true;
    }
    if (byte == 0x1b) {
        parser->string_escape_delivered = false;
        return BOXRIG_STATUS_OK;
    }
    status = put_string_byte(parser, byte);
    if (status == BOXRIG_STATUS_OK) {
        parser->state = BOXRIG_PARSER_STRING;
        parser->string_escape_delivered = false;
    }
    return status;
}

static bool is_dcs_state(boxrig_parser_state state)
{
    return state >= BOXRIG_PARSER_DCS_ENTRY &&
           state <= BOXRIG_PARSER_DCS_PASSTHROUGH_ESCAPE;
}

static void finish_ignored_dcs(boxrig_parser *parser)
{
    parser->state = BOXRIG_PARSER_GROUND;
    parser->dcs_length = 0;
    parser->dcs_escape_delivered = false;
    clear_sequence(parser);
}

static void discard_dcs(boxrig_parser *parser)
{
    parser->state = BOXRIG_PARSER_DCS_IGNORE;
    parser->dcs_escape_delivered = false;
    clear_sequence(parser);
}

static boxrig_status finish_active_dcs(
    boxrig_parser *parser,
    bool cancelled,
    boxrig_parser_string_terminator terminator)
{
    boxrig_status status = parser->actions.dcs_unhook(
        parser->actions.context,
        cancelled,
        terminator);

    if (status == BOXRIG_STATUS_OK) {
        finish_ignored_dcs(parser);
    }
    return status;
}

static boxrig_status dispatch_dcs(boxrig_parser *parser, uint8_t byte)
{
    bool passthrough = false;
    boxrig_status status = parser->actions.dcs_hook(
        parser->actions.context,
        parser->private_marker,
        parser->parameters,
        parser->parameter_count,
        parser->intermediates,
        parser->intermediate_count,
        byte,
        &passthrough);

    if (status == BOXRIG_STATUS_OK) {
        parser->dcs_length++;
        parser->state = passthrough ? BOXRIG_PARSER_DCS_PASSTHROUGH
                                    : BOXRIG_PARSER_DCS_IGNORE;
        clear_sequence(parser);
    }
    return status;
}

static boxrig_status feed_dcs_selector_byte(
    boxrig_parser *parser,
    uint8_t byte)
{
    if (byte == 0x18 || byte == 0x1a) {
        finish_ignored_dcs(parser);
        return BOXRIG_STATUS_OK;
    }
    if (byte == 0x1b) {
        parser->state = BOXRIG_PARSER_DCS_IGNORE_ESCAPE;
        return BOXRIG_STATUS_OK;
    }
    if (parser->dcs_length >= parser->string_capacity) {
        discard_dcs(parser);
        return BOXRIG_STATUS_OK;
    }
    if (is_final(byte)) {
        return dispatch_dcs(parser, byte);
    }
    parser->dcs_length++;
    if (is_c0(byte) || byte == 0x7f) {
        return BOXRIG_STATUS_OK;
    }
    if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9' &&
        (parser->state == BOXRIG_PARSER_DCS_ENTRY ||
         parser->state == BOXRIG_PARSER_DCS_PARAMETER)) {
        parser->state = BOXRIG_PARSER_DCS_PARAMETER;
        if (!append_digit(parser, byte)) {
            parser->state = BOXRIG_PARSER_DCS_IGNORE;
        }
        return BOXRIG_STATUS_OK;
    }
    if ((byte == (uint8_t)';' || byte == (uint8_t)':') &&
        (parser->state == BOXRIG_PARSER_DCS_ENTRY ||
         parser->state == BOXRIG_PARSER_DCS_PARAMETER)) {
        if (parser->parameter_count == 0 &&
            !append_parameter(parser, false)) {
            parser->state = BOXRIG_PARSER_DCS_IGNORE;
            return BOXRIG_STATUS_OK;
        }
        if (!append_parameter(parser, byte == (uint8_t)':')) {
            parser->state = BOXRIG_PARSER_DCS_IGNORE;
            return BOXRIG_STATUS_OK;
        }
        parser->state = BOXRIG_PARSER_DCS_PARAMETER;
        return BOXRIG_STATUS_OK;
    }
    if (byte >= 0x3c && byte <= 0x3f &&
        parser->state == BOXRIG_PARSER_DCS_ENTRY &&
        parser->private_marker == 0) {
        parser->private_marker = byte;
        return BOXRIG_STATUS_OK;
    }
    if (byte >= 0x20 && byte <= 0x2f &&
        (parser->state == BOXRIG_PARSER_DCS_ENTRY ||
         parser->state == BOXRIG_PARSER_DCS_PARAMETER ||
         parser->state == BOXRIG_PARSER_DCS_INTERMEDIATE)) {
        parser->state = BOXRIG_PARSER_DCS_INTERMEDIATE;
        if (!append_intermediate(parser, byte)) {
            parser->state = BOXRIG_PARSER_DCS_IGNORE;
        }
        return BOXRIG_STATUS_OK;
    }
    parser->state = BOXRIG_PARSER_DCS_IGNORE;
    return BOXRIG_STATUS_OK;
}

static boxrig_status abort_dcs_for_limit(boxrig_parser *parser)
{
    /* Preserve the active state until abort succeeds so the overflow byte can
     * be retried without delivering data or aborting twice. */
    boxrig_status status = parser->actions.dcs_unhook(
        parser->actions.context,
        true,
        BOXRIG_PARSER_STRING_TERMINATOR_CANCEL);

    if (status == BOXRIG_STATUS_OK) {
        discard_dcs(parser);
    }
    return status;
}

static boxrig_status put_dcs_content(boxrig_parser *parser, uint8_t byte)
{
    boxrig_status status;

    if (parser->dcs_length >= parser->string_capacity) {
        return abort_dcs_for_limit(parser);
    }
    status = parser->actions.dcs_put(parser->actions.context, byte);
    if (status == BOXRIG_STATUS_OK) {
        parser->dcs_length++;
    }
    return status;
}

static boxrig_status feed_dcs_passthrough_byte(
    boxrig_parser *parser,
    uint8_t byte)
{
    boxrig_status status;

    if (parser->state == BOXRIG_PARSER_DCS_PASSTHROUGH) {
        if (byte == 0x18 || byte == 0x1a) {
            return finish_active_dcs(
                parser,
                true,
                BOXRIG_PARSER_STRING_TERMINATOR_CANCEL);
        }
        if (byte == 0x1b) {
            parser->state = BOXRIG_PARSER_DCS_PASSTHROUGH_ESCAPE;
            parser->dcs_escape_delivered = false;
            return BOXRIG_STATUS_OK;
        }
        return put_dcs_content(parser, byte);
    }

    if (byte == (uint8_t)'\\') {
        return finish_active_dcs(
            parser,
            false,
            BOXRIG_PARSER_STRING_TERMINATOR_ST);
    }
    if (byte == 0x18 || byte == 0x1a) {
        return finish_active_dcs(
            parser,
            true,
            BOXRIG_PARSER_STRING_TERMINATOR_CANCEL);
    }
    if (!parser->dcs_escape_delivered) {
        status = put_dcs_content(parser, 0x1b);
        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
        if (parser->state != BOXRIG_PARSER_DCS_PASSTHROUGH_ESCAPE) {
            return BOXRIG_STATUS_OK;
        }
        parser->dcs_escape_delivered = true;
    }
    if (byte == 0x1b) {
        parser->dcs_escape_delivered = false;
        return BOXRIG_STATUS_OK;
    }
    status = put_dcs_content(parser, byte);
    if (status == BOXRIG_STATUS_OK &&
        parser->state == BOXRIG_PARSER_DCS_PASSTHROUGH_ESCAPE) {
        parser->state = BOXRIG_PARSER_DCS_PASSTHROUGH;
        parser->dcs_escape_delivered = false;
    }
    return status;
}

static boxrig_status feed_dcs_byte(boxrig_parser *parser, uint8_t byte)
{
    if (parser->state == BOXRIG_PARSER_DCS_PASSTHROUGH ||
        parser->state == BOXRIG_PARSER_DCS_PASSTHROUGH_ESCAPE) {
        return feed_dcs_passthrough_byte(parser, byte);
    }
    if (parser->state == BOXRIG_PARSER_DCS_IGNORE_ESCAPE) {
        if (byte == (uint8_t)'\\' || byte == 0x18 || byte == 0x1a) {
            finish_ignored_dcs(parser);
        } else {
            if (parser->dcs_length < parser->string_capacity) {
                parser->dcs_length++;
            }
            if (byte != 0x1b &&
                parser->dcs_length < parser->string_capacity) {
                parser->dcs_length++;
            }
        }
        if (parser->state == BOXRIG_PARSER_DCS_IGNORE_ESCAPE &&
            byte != 0x1b) {
            parser->state = BOXRIG_PARSER_DCS_IGNORE;
        }
        return BOXRIG_STATUS_OK;
    }
    if (parser->state == BOXRIG_PARSER_DCS_IGNORE) {
        if (byte == 0x18 || byte == 0x1a) {
            finish_ignored_dcs(parser);
        } else if (byte == 0x1b) {
            parser->state = BOXRIG_PARSER_DCS_IGNORE_ESCAPE;
        } else if (parser->dcs_length < parser->string_capacity) {
            parser->dcs_length++;
        }
        return BOXRIG_STATUS_OK;
    }
    return feed_dcs_selector_byte(parser, byte);
}

static boxrig_status feed_escape_byte(boxrig_parser *parser, uint8_t byte)
{
    boxrig_status status;

    if (byte == (uint8_t)'[' && parser->intermediate_count == 0) {
        clear_sequence(parser);
        parser->state = BOXRIG_PARSER_CSI_ENTRY;
        return BOXRIG_STATUS_OK;
    }
    if (parser->intermediate_count == 0) {
        if (byte == (uint8_t)']') {
            return begin_string(parser, BOXRIG_PARSER_STRING_OSC);
        }
        if (byte == (uint8_t)'P') {
            begin_dcs(parser);
            return BOXRIG_STATUS_OK;
        }
        if (byte == (uint8_t)'_') {
            return begin_string(parser, BOXRIG_PARSER_STRING_APC);
        }
        if (byte == (uint8_t)'^') {
            return begin_string(parser, BOXRIG_PARSER_STRING_PM);
        }
        if (byte == (uint8_t)'X') {
            return begin_string(parser, BOXRIG_PARSER_STRING_SOS);
        }
    }
    if (byte >= 0x20 && byte <= 0x2f) {
        if (!append_intermediate(parser, byte)) {
            parser->state = BOXRIG_PARSER_GROUND;
        } else {
            parser->state = BOXRIG_PARSER_ESCAPE_INTERMEDIATE;
        }
        return BOXRIG_STATUS_OK;
    }
    if (byte >= 0x30 && byte <= 0x7e) {
        status = parser->actions.dispatch_escape(
            parser->actions.context,
            parser->intermediates,
            parser->intermediate_count,
            byte);
        if (status == BOXRIG_STATUS_OK) {
            parser->state = BOXRIG_PARSER_GROUND;
            clear_sequence(parser);
        }
        return status;
    }
    parser->state = BOXRIG_PARSER_GROUND;
    clear_sequence(parser);
    return BOXRIG_STATUS_OK;
}

static boxrig_status dispatch_csi(boxrig_parser *parser, uint8_t byte)
{
    boxrig_status status = parser->actions.dispatch_csi(
        parser->actions.context,
        parser->private_marker,
        parser->parameters,
        parser->parameter_count,
        parser->intermediates,
        parser->intermediate_count,
        byte);

    if (status == BOXRIG_STATUS_OK) {
        parser->state = BOXRIG_PARSER_GROUND;
        clear_sequence(parser);
    }
    return status;
}

static boxrig_status feed_csi_byte(boxrig_parser *parser, uint8_t byte)
{
    if (parser->state == BOXRIG_PARSER_CSI_IGNORE) {
        if (is_final(byte)) {
            parser->state = BOXRIG_PARSER_GROUND;
            clear_sequence(parser);
        }
        return BOXRIG_STATUS_OK;
    }

    if (is_final(byte)) {
        return dispatch_csi(parser, byte);
    }
    if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9' &&
        (parser->state == BOXRIG_PARSER_CSI_ENTRY ||
         parser->state == BOXRIG_PARSER_CSI_PARAMETER)) {
        parser->state = BOXRIG_PARSER_CSI_PARAMETER;
        if (!append_digit(parser, byte)) {
            parser->state = BOXRIG_PARSER_CSI_IGNORE;
        }
        return BOXRIG_STATUS_OK;
    }
    if ((byte == (uint8_t)';' || byte == (uint8_t)':') &&
        (parser->state == BOXRIG_PARSER_CSI_ENTRY ||
         parser->state == BOXRIG_PARSER_CSI_PARAMETER)) {
        if (parser->parameter_count == 0 &&
            !append_parameter(parser, false)) {
            parser->state = BOXRIG_PARSER_CSI_IGNORE;
            return BOXRIG_STATUS_OK;
        }
        if (!append_parameter(parser, byte == (uint8_t)':')) {
            parser->state = BOXRIG_PARSER_CSI_IGNORE;
            return BOXRIG_STATUS_OK;
        }
        parser->state = BOXRIG_PARSER_CSI_PARAMETER;
        return BOXRIG_STATUS_OK;
    }
    if (byte >= 0x3c && byte <= 0x3f &&
        parser->state == BOXRIG_PARSER_CSI_ENTRY &&
        parser->private_marker == 0) {
        parser->private_marker = byte;
        return BOXRIG_STATUS_OK;
    }
    if (byte >= 0x20 && byte <= 0x2f &&
        parser->state != BOXRIG_PARSER_CSI_INTERMEDIATE) {
        parser->state = BOXRIG_PARSER_CSI_INTERMEDIATE;
        if (!append_intermediate(parser, byte)) {
            parser->state = BOXRIG_PARSER_CSI_IGNORE;
        }
        return BOXRIG_STATUS_OK;
    }
    if (byte >= 0x20 && byte <= 0x2f &&
        parser->state == BOXRIG_PARSER_CSI_INTERMEDIATE) {
        if (!append_intermediate(parser, byte)) {
            parser->state = BOXRIG_PARSER_CSI_IGNORE;
        }
        return BOXRIG_STATUS_OK;
    }
    parser->state = BOXRIG_PARSER_CSI_IGNORE;
    return BOXRIG_STATUS_OK;
}

void boxrig_parser_initialize(
    boxrig_parser *parser,
    const boxrig_parser_actions *actions,
    size_t string_capacity)
{
    memset(parser, 0, sizeof(*parser));
    parser->state = BOXRIG_PARSER_GROUND;
    parser->actions = *actions;
    parser->string_capacity = string_capacity;
}

boxrig_status boxrig_parser_finish(boxrig_parser *parser)
{
    boxrig_status status = parser->actions.flush_print(parser->actions.context);
    if (status != BOXRIG_STATUS_OK) return status;
    if (parser->state == BOXRIG_PARSER_STRING || parser->state == BOXRIG_PARSER_STRING_ESCAPE)
        status = finish_string(parser, true, BOXRIG_PARSER_STRING_TERMINATOR_CANCEL);
    else if (parser->state == BOXRIG_PARSER_DCS_PASSTHROUGH ||
             parser->state == BOXRIG_PARSER_DCS_PASSTHROUGH_ESCAPE)
        status = finish_active_dcs(parser, true, BOXRIG_PARSER_STRING_TERMINATOR_CANCEL);
    if (status == BOXRIG_STATUS_OK) {
        boxrig_parser_actions actions = parser->actions;
        boxrig_parser_initialize(parser, &actions, parser->string_capacity);
    }
    return status;
}

boxrig_status boxrig_parser_feed(
    boxrig_parser *parser,
    const uint8_t *bytes,
    size_t length,
    size_t *consumed_out)
{
    size_t consumed = 0;

    while (consumed < length) {
        uint8_t byte = bytes[consumed];
        boxrig_status status;

        if (!parser->ground_print_flushed &&
            parser->state == BOXRIG_PARSER_GROUND &&
            (is_c0(byte) || byte == 0x7f)) {
            status = parser->actions.flush_print(parser->actions.context);
            if (status != BOXRIG_STATUS_OK) {
                *consumed_out = consumed;
                return status;
            }
            parser->ground_print_flushed = true;
        }

        if (parser->state == BOXRIG_PARSER_STRING ||
            parser->state == BOXRIG_PARSER_STRING_ESCAPE) {
            status = feed_string_byte(parser, byte);
        } else if (is_dcs_state(parser->state)) {
            status = feed_dcs_byte(parser, byte);
        } else if (byte == 0x18 || byte == 0x1a) {
            parser->state = BOXRIG_PARSER_GROUND;
            clear_sequence(parser);
            status = BOXRIG_STATUS_OK;
        } else if (byte == 0x1b) {
            clear_sequence(parser);
            parser->state = BOXRIG_PARSER_ESCAPE;
            status = BOXRIG_STATUS_OK;
        } else if (is_c0(byte)) {
            status = parser->actions.execute(parser->actions.context, byte);
        } else {
            switch (parser->state) {
            case BOXRIG_PARSER_GROUND:
                if (byte == 0x7f) {
                    status = BOXRIG_STATUS_OK;
                } else {
                    status = parser->actions.print(
                        parser->actions.context,
                        byte);
                }
                break;
            case BOXRIG_PARSER_ESCAPE:
            case BOXRIG_PARSER_ESCAPE_INTERMEDIATE:
                status = feed_escape_byte(parser, byte);
                break;
            case BOXRIG_PARSER_CSI_ENTRY:
            case BOXRIG_PARSER_CSI_PARAMETER:
            case BOXRIG_PARSER_CSI_INTERMEDIATE:
            case BOXRIG_PARSER_CSI_IGNORE:
                status = feed_csi_byte(parser, byte);
                break;
            case BOXRIG_PARSER_DCS_ENTRY:
            case BOXRIG_PARSER_DCS_PARAMETER:
            case BOXRIG_PARSER_DCS_INTERMEDIATE:
            case BOXRIG_PARSER_DCS_IGNORE:
            case BOXRIG_PARSER_DCS_IGNORE_ESCAPE:
            case BOXRIG_PARSER_DCS_PASSTHROUGH:
            case BOXRIG_PARSER_DCS_PASSTHROUGH_ESCAPE:
            case BOXRIG_PARSER_STRING:
            case BOXRIG_PARSER_STRING_ESCAPE:
                status = BOXRIG_STATUS_OK;
                break;
            default:
                status = BOXRIG_STATUS_INVALID_ARGUMENT;
                break;
            }
        }
        if (status != BOXRIG_STATUS_OK) {
            *consumed_out = consumed;
            return status;
        }
        parser->ground_print_flushed = false;
        parser->last_input_was_cr = byte == 0x0d;
        consumed++;
    }
    *consumed_out = consumed;
    return BOXRIG_STATUS_OK;
}
