/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "terminal_internal.h"

#include "utf8proc.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t first_parameter(
    const boxrig_parser_parameter *parameters,
    size_t parameter_count,
    uint32_t default_value)
{
    if (parameter_count == 0 || parameters[0].subparameter ||
        !parameters[0].has_value) {
        return default_value;
    }
    return parameters[0].value;
}

static uint32_t parameter_at(
    const boxrig_parser_parameter *parameters,
    size_t parameter_count,
    size_t index,
    uint32_t default_value)
{
    if (index >= parameter_count || parameters[index].subparameter ||
        !parameters[index].has_value) {
        return default_value;
    }
    return parameters[index].value;
}

static boxrig_status emit_codepoint(void *context, int32_t codepoint)
{
    boxrig_terminal *terminal = context;

    if (codepoint >= 0 && codepoint <= 0x7f) {
        int32_t translated;
        if (boxrig_screen_translate_byte(&terminal->screen, (uint8_t)codepoint, &translated))
            codepoint = translated;
    }
    return boxrig_screen_put_codepoint(&terminal->screen, codepoint);
}

static boxrig_status interpreter_print(void *context, uint8_t byte)
{
    boxrig_terminal *terminal = context;
    return boxrig_codec_decoder_put(&terminal->decoder, terminal->encoding,
        byte, emit_codepoint, terminal);
}

static boxrig_status interpreter_flush_print(void *context)
{
    boxrig_terminal *terminal = context;
    boxrig_status status = boxrig_codec_decoder_flush(&terminal->decoder,
        terminal->encoding, emit_codepoint, terminal);
    if (status == BOXRIG_STATUS_OK) boxrig_screen_end_text(&terminal->screen);
    return status;
}

static boxrig_status interpreter_execute(void *context, uint8_t control)
{
    boxrig_terminal *terminal = context;

    if (control == 0x07) {
        return boxrig_terminal_push_host_item(
            terminal,
            BOXRIG_HOST_ITEM_BELL,
            0,
            0,
            NULL,
            0);
    }
    boxrig_screen_end_text(&terminal->screen);
    boxrig_text_monitor_begin_mutation(terminal);
    boxrig_receive_newline policy = terminal->receive_newline;
    if (control == 0x0a && policy == BOXRIG_RECEIVE_NEWLINE_CRLF &&
        terminal->parser.last_input_was_cr) {
        /* The preceding CR already performed the complete newline. */
    } else if ((control == 0x0d &&
                (policy == BOXRIG_RECEIVE_NEWLINE_CR ||
                 policy == BOXRIG_RECEIVE_NEWLINE_CRLF)) ||
               (control == 0x0a &&
                (policy == BOXRIG_RECEIVE_NEWLINE_LF ||
                 policy == BOXRIG_RECEIVE_NEWLINE_CRLF))) {
        boxrig_screen_linefeed(&terminal->screen, true);
    } else if (control == 0x0a &&
               (policy == BOXRIG_RECEIVE_NEWLINE_STANDARD ||
                policy == BOXRIG_RECEIVE_NEWLINE_CR)) {
        boxrig_screen_linefeed(&terminal->screen, false);
    } else {
        boxrig_screen_execute(&terminal->screen, control);
    }
    boxrig_text_monitor_end_mutation(terminal);
    return BOXRIG_STATUS_OK;
}

static boxrig_status interpreter_dispatch_escape(
    void *context,
    const uint8_t *intermediates,
    size_t intermediate_count,
    uint8_t final_byte)
{
    boxrig_terminal *terminal = context;

    boxrig_screen_end_text(&terminal->screen);
    boxrig_text_monitor_begin_mutation(terminal);

    if (intermediate_count == 0 && final_byte == (uint8_t)'c') {
        boxrig_notification_reset(terminal);
        boxrig_dcs_reset(&terminal->dcs);
        boxrig_kitty_graphics_reset(&terminal->graphics);
        boxrig_kitty_graphics_protocol_reset(terminal);
    }
    boxrig_input_dispatch_escape(
        &terminal->input,
        intermediates,
        intermediate_count,
        final_byte);
    boxrig_screen_dispatch_escape(
        &terminal->screen,
        intermediates,
        intermediate_count,
        final_byte);
    boxrig_text_monitor_end_mutation(terminal);
    return BOXRIG_STATUS_OK;
}

static boxrig_status append_cursor_report(
    boxrig_terminal *terminal,
    bool private_report)
{
    char response[64];
    int length;
    size_t row = terminal->screen.active->cursor.row;
    size_t column = terminal->screen.active->cursor.column;

    if (terminal->screen.origin_mode) {
        if (row >= terminal->screen.active->scroll_region_top) {
            row -= terminal->screen.active->scroll_region_top;
        } else {
            row = 0;
        }
        if (terminal->screen.left_right_margin_mode) {
            if (column >= terminal->screen.active->horizontal_margin_left) {
                column -= terminal->screen.active->horizontal_margin_left;
            } else {
                column = 0;
            }
        }
    }

    length = snprintf(
        response,
        sizeof(response),
        private_report ? "\x1b[?%zu;%zuR" : "\x1b[%zu;%zuR",
        row + 1,
        column + 1);
    if (length < 0 || (size_t)length >= sizeof(response)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    return boxrig_terminal_append_output(
        terminal,
        (const uint8_t *)response,
        (size_t)length);
}

static boxrig_status append_mode_report(
    boxrig_terminal *terminal,
    bool private_mode,
    uint32_t mode)
{
    char response[64];
    bool enabled = false;
    bool recognized;
    unsigned int status;
    int length;

    recognized = boxrig_input_query_mode(
        &terminal->input,
        private_mode,
        mode,
        &enabled);
    if (!recognized) {
        recognized = boxrig_screen_query_mode(
            &terminal->screen,
            private_mode,
            mode,
            &enabled);
    }
    status = recognized ? (enabled ? 1u : 2u) : 0u;
    length = snprintf(
        response,
        sizeof(response),
        private_mode ? "\x1b[?%" PRIu32 ";%u$y"
                     : "\x1b[%" PRIu32 ";%u$y",
        mode,
        status);
    if (length < 0 || (size_t)length >= sizeof(response)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    return boxrig_terminal_append_output(
        terminal,
        (const uint8_t *)response,
        (size_t)length);
}

static boxrig_status append_kitty_keyboard_report(boxrig_terminal *terminal)
{
    char response[32];
    uint32_t flags = boxrig_input_kitty_flags(
        &terminal->input,
        terminal->screen.alternate_active);
    int length = snprintf(
        response,
        sizeof(response),
        "\x1b[?%" PRIu32 "u",
        flags);

    if (length < 0 || (size_t)length >= sizeof(response)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    return boxrig_terminal_append_output(
        terminal,
        (const uint8_t *)response,
        (size_t)length);
}

static boxrig_status append_pixel_geometry_report(
    boxrig_terminal *terminal,
    uint32_t operation)
{
    const boxrig_kitty_graphics_protocol_state *graphics =
        &terminal->graphics_protocol;
    char response[96];
    size_t height;
    size_t width;
    unsigned int response_kind;
    int length;

    if (graphics->cell_pixel_width == 0 ||
        graphics->cell_pixel_height == 0) {
        return BOXRIG_STATUS_OK;
    }
    if (operation == 14) {
        if (terminal->screen.rows >
                SIZE_MAX / graphics->cell_pixel_height ||
            terminal->screen.columns >
                SIZE_MAX / graphics->cell_pixel_width) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        response_kind = 4;
        height = terminal->screen.rows * graphics->cell_pixel_height;
        width = terminal->screen.columns * graphics->cell_pixel_width;
    } else {
        response_kind = 6;
        height = graphics->cell_pixel_height;
        width = graphics->cell_pixel_width;
    }
    length = snprintf(
        response,
        sizeof(response),
        "\x1b[%u;%zu;%zut",
        response_kind,
        height,
        width);
    if (length < 0 || (size_t)length >= sizeof(response)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    return boxrig_terminal_append_output(
        terminal,
        (const uint8_t *)response,
        (size_t)length);
}

static boxrig_status interpreter_dispatch_csi(
    void *context,
    uint8_t private_marker,
    const boxrig_parser_parameter *parameters,
    size_t parameter_count,
    const uint8_t *intermediates,
    size_t intermediate_count,
    uint8_t final_byte)
{
    boxrig_terminal *terminal = context;
    uint32_t parameter;

    if (private_marker == 0 && intermediate_count == 1 &&
        intermediates[0] == (uint8_t)'!' && final_byte == (uint8_t)'p' &&
        first_parameter(parameters, parameter_count, 0) == 0) {
        boxrig_screen_end_text(&terminal->screen);
        boxrig_text_monitor_begin_mutation(terminal);
        boxrig_input_soft_reset(&terminal->input);
        boxrig_screen_soft_reset(&terminal->screen);
        boxrig_text_monitor_end_mutation(terminal);
        return BOXRIG_STATUS_OK;
    }
    if (private_marker == (uint8_t)'>' && intermediate_count == 0 &&
        final_byte == (uint8_t)'q' &&
        first_parameter(parameters, parameter_count, 0) == 0) {
        static const uint8_t response[] = "\x1bP>|boxrig\x1b\\";

        return boxrig_terminal_append_output(
            terminal,
            response,
            sizeof(response) - 1);
    }
    if (private_marker == 0 && intermediate_count == 0 &&
        final_byte == (uint8_t)'t' && parameter_count >= 1 &&
        (first_parameter(parameters, parameter_count, 0) == 14 ||
         first_parameter(parameters, parameter_count, 0) == 16)) {
        return append_pixel_geometry_report(
            terminal,
            first_parameter(parameters, parameter_count, 0));
    }
    if (private_marker == 0 && intermediate_count == 0 &&
        final_byte == (uint8_t)'t' && parameter_count >= 1 &&
        parameter_at(parameters, parameter_count, 2, 0) == 0) {
        uint32_t operation = first_parameter(parameters, parameter_count, 0);
        uint32_t selection = parameter_at(parameters, parameter_count, 1, 0);
        uint32_t flags = 0;

        if (selection == 0) {
            flags = BOXRIG_TITLE_ICON | BOXRIG_TITLE_WINDOW;
        } else if (selection == 1) {
            flags = BOXRIG_TITLE_ICON;
        } else if (selection == 2) {
            flags = BOXRIG_TITLE_WINDOW;
        }
        if (flags != 0 && (operation == 22 || operation == 23)) {
            flags |= operation == 22 ? BOXRIG_TITLE_STACK_PUSH
                                     : BOXRIG_TITLE_STACK_POP;
            return boxrig_terminal_push_host_item(
                terminal,
                BOXRIG_HOST_ITEM_TITLE,
                0,
                flags,
                NULL,
                0);
        }
    }

    if (intermediate_count == 0 && final_byte == (uint8_t)'u') {
        if (private_marker == (uint8_t)'?' && parameter_count == 0) {
            return append_kitty_keyboard_report(terminal);
        }
        if (private_marker == (uint8_t)'=') {
            boxrig_input_set_kitty_flags(
                &terminal->input,
                terminal->screen.alternate_active,
                parameter_at(parameters, parameter_count, 0, 0),
                parameter_at(parameters, parameter_count, 1, 1));
            return BOXRIG_STATUS_OK;
        }
        if (private_marker == (uint8_t)'>') {
            boxrig_input_push_kitty_flags(
                &terminal->input,
                terminal->screen.alternate_active,
                parameter_at(parameters, parameter_count, 0, 0));
            return BOXRIG_STATUS_OK;
        }
        if (private_marker == (uint8_t)'<') {
            boxrig_input_pop_kitty_flags(
                &terminal->input,
                terminal->screen.alternate_active,
                parameter_at(parameters, parameter_count, 0, 1));
            return BOXRIG_STATUS_OK;
        }
    }

    if (intermediate_count == 1 &&
        intermediates[0] == (uint8_t)'$' &&
        final_byte == (uint8_t)'p' &&
        (private_marker == 0 || private_marker == (uint8_t)'?')) {
        return append_mode_report(
            terminal,
            private_marker == (uint8_t)'?',
            first_parameter(parameters, parameter_count, 0));
    }
    if (intermediate_count == 0 && final_byte == (uint8_t)'n') {
        parameter = first_parameter(parameters, parameter_count, 0);
        if (private_marker == 0 && parameter == 5) {
            static const uint8_t response[] = "\x1b[0n";

            return boxrig_terminal_append_output(
                terminal,
                response,
                sizeof(response) - 1);
        }
        if (parameter == 6 &&
            (private_marker == 0 || private_marker == (uint8_t)'?')) {
            return append_cursor_report(
                terminal,
                private_marker == (uint8_t)'?');
        }
        if (private_marker == (uint8_t)'?') {
            static const uint8_t printer_status[] = "\x1b[?10n";
            static const uint8_t udk_status[] = "\x1b[?20n";
            static const uint8_t keyboard_status[] = "\x1b[?27;1;0;0n";
            static const uint8_t locator_status[] = "\x1b[?70n";
            static const uint8_t multibutton_status[] = "\x1b[?83n";
            const uint8_t *response = NULL;
            size_t response_length = 0;

            if (parameter == 15) {
                response = printer_status;
                response_length = sizeof(printer_status) - 1;
            } else if (parameter == 25) {
                response = udk_status;
                response_length = sizeof(udk_status) - 1;
            } else if (parameter == 26) {
                response = keyboard_status;
                response_length = sizeof(keyboard_status) - 1;
            } else if (parameter == 75) {
                response = locator_status;
                response_length = sizeof(locator_status) - 1;
            } else if (parameter == 85) {
                response = multibutton_status;
                response_length = sizeof(multibutton_status) - 1;
            }
            if (response != NULL) {
                return boxrig_terminal_append_output(
                    terminal,
                    response,
                    response_length);
            }
        }
    }
    if (intermediate_count == 0 && final_byte == (uint8_t)'c' &&
        first_parameter(parameters, parameter_count, 0) == 0) {
        static const uint8_t primary_response[] = "\x1b[?62;6;22c";
        static const uint8_t secondary_response[] = "\x1b[>1;0;0c";

        if (private_marker == 0) {
            return boxrig_terminal_append_output(
                terminal,
                primary_response,
                sizeof(primary_response) - 1);
        }
        if (private_marker == (uint8_t)'>') {
            return boxrig_terminal_append_output(
                terminal,
                secondary_response,
                sizeof(secondary_response) - 1);
        }
    }
    boxrig_screen_end_text(&terminal->screen);
    boxrig_text_monitor_begin_mutation(terminal);
    if (private_marker == (uint8_t)'?' && intermediate_count == 0 &&
        (final_byte == (uint8_t)'h' || final_byte == (uint8_t)'l')) {
        boxrig_input_set_private_modes(
            &terminal->input,
            parameters,
            parameter_count,
            final_byte == (uint8_t)'h');
    }
    boxrig_screen_dispatch_csi(
        &terminal->screen,
        private_marker,
        parameters,
        parameter_count,
        intermediates,
        intermediate_count,
        final_byte);
    boxrig_text_monitor_end_mutation(terminal);
    return BOXRIG_STATUS_OK;
}

static boxrig_status interpreter_dcs_hook(
    void *context,
    uint8_t private_marker,
    const boxrig_parser_parameter *parameters,
    size_t parameter_count,
    const uint8_t *intermediates,
    size_t intermediate_count,
    uint8_t final_byte,
    bool *passthrough_out)
{
    return boxrig_dcs_hook(
        context,
        private_marker,
        parameters,
        parameter_count,
        intermediates,
        intermediate_count,
        final_byte,
        passthrough_out);
}

static boxrig_status interpreter_dcs_put(void *context, uint8_t byte)
{
    return boxrig_dcs_put(context, byte);
}

static boxrig_status interpreter_dcs_unhook(
    void *context,
    bool cancelled,
    boxrig_parser_string_terminator terminator)
{
    return boxrig_dcs_unhook(context, cancelled, terminator);
}

static boxrig_status interpreter_string_start(
    void *context,
    boxrig_parser_string_kind kind)
{
    boxrig_terminal *terminal = context;

    terminal->active_string_kind = kind;
    if (kind == BOXRIG_PARSER_STRING_APC) {
        boxrig_kitty_graphics_start(&terminal->graphics);
    } else if (kind == BOXRIG_PARSER_STRING_OSC) {
        terminal->osc_buffer_length = 0;
    }
    return BOXRIG_STATUS_OK;
}

static boxrig_status interpreter_string_put(void *context, uint8_t byte)
{
    boxrig_terminal *terminal = context;

    if (terminal->active_string_kind == BOXRIG_PARSER_STRING_APC) {
        return boxrig_kitty_graphics_put(&terminal->graphics, byte);
    }
    if (terminal->active_string_kind != BOXRIG_PARSER_STRING_OSC) {
        return BOXRIG_STATUS_OK;
    }
    if (terminal->osc_buffer_length >= terminal->osc_buffer_capacity) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    terminal->osc_buffer[terminal->osc_buffer_length] = byte;
    terminal->osc_buffer_length++;
    return BOXRIG_STATUS_OK;
}

static bool is_host_text_control(utf8proc_int32_t codepoint)
{
    return (codepoint >= 0 && codepoint <= 0x1f) ||
           (codepoint >= 0x7f && codepoint <= 0x9f);
}

static boxrig_status sanitize_host_text(
    const uint8_t *input,
    size_t input_length,
    uint8_t **output_out,
    size_t *output_length_out)
{
    static const uint8_t replacement[] = {0xef, 0xbf, 0xbd};
    uint8_t *output;
    size_t output_capacity;
    size_t input_offset = 0;
    size_t output_length = 0;

    *output_out = NULL;
    *output_length_out = 0;
    if (input_length == 0) {
        return BOXRIG_STATUS_OK;
    }
    if (input_length > SIZE_MAX / sizeof(replacement) ||
        input_length > (size_t)PTRDIFF_MAX) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    output_capacity = input_length * sizeof(replacement);
    output = malloc(output_capacity);
    if (output == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    while (input_offset < input_length) {
        utf8proc_int32_t codepoint = -1;
        utf8proc_ssize_t decoded = utf8proc_iterate(
            input + input_offset,
            (utf8proc_ssize_t)(input_length - input_offset),
            &codepoint);

        if (decoded <= 0) {
            memcpy(output + output_length, replacement, sizeof(replacement));
            output_length += sizeof(replacement);
            input_offset++;
            continue;
        }
        if (!is_host_text_control(codepoint)) {
            memcpy(
                output + output_length,
                input + input_offset,
                (size_t)decoded);
            output_length += (size_t)decoded;
        }
        input_offset += (size_t)decoded;
    }
    *output_out = output;
    *output_length_out = output_length;
    return BOXRIG_STATUS_OK;
}

typedef struct boxrig_title_decoder_output {
    uint8_t *bytes;
    size_t length;
} boxrig_title_decoder_output;

static boxrig_status append_title_scalar(void *context, int32_t codepoint)
{
    boxrig_title_decoder_output *output = context;
    if (!is_host_text_control(codepoint)) {
        uint8_t encoded[4];
        size_t length = boxrig_unicode_encode(codepoint, encoded);
        memcpy(output->bytes + output->length, encoded, length);
        output->length += length;
    }
    return BOXRIG_STATUS_OK;
}

static boxrig_status decode_title_text(
    const uint8_t *input, size_t length, uint8_t **output_out, size_t *length_out)
{
    *output_out = NULL;
    *length_out = 0;
    if (length == 0) return BOXRIG_STATUS_OK;
    if (length > SIZE_MAX / 3) return BOXRIG_STATUS_LIMIT_EXCEEDED;
    boxrig_title_decoder_output output = {malloc(length * 3), 0};
    if (output.bytes == NULL) return BOXRIG_STATUS_OUT_OF_MEMORY;
    boxrig_codec_decoder decoder = {0};
    /* Shift_JIS maps into the BMP, so three UTF-8 bytes per source byte also
     * bound malformed prefixes followed by an independently restored ASCII. */
    for (size_t index = 0; index < length; index++)
        (void)boxrig_codec_decoder_put(&decoder, BOXRIG_ENCODING_SHIFT_JIS,
            input[index], append_title_scalar, &output);
    (void)boxrig_codec_decoder_flush(&decoder, BOXRIG_ENCODING_SHIFT_JIS,
        append_title_scalar, &output);
    *output_out = output.bytes;
    *length_out = output.length;
    return BOXRIG_STATUS_OK;
}

static boxrig_status push_text_host_item(
    boxrig_terminal *terminal,
    boxrig_host_item_type type,
    uint32_t flags,
    const uint8_t *payload,
    size_t payload_length)
{
    uint8_t *sanitized = NULL;
    size_t sanitized_length = 0;
    boxrig_status status = type == BOXRIG_HOST_ITEM_TITLE &&
        terminal->encoding == BOXRIG_ENCODING_SHIFT_JIS ? decode_title_text(
            payload, payload_length, &sanitized, &sanitized_length) : sanitize_host_text(
        payload,
        payload_length,
        &sanitized,
        &sanitized_length);

    if (status == BOXRIG_STATUS_OK) {
        status = boxrig_terminal_push_host_item(
            terminal,
            type,
            0,
            flags,
            sanitized,
            sanitized_length);
    }
    free(sanitized);
    if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) {
        return BOXRIG_STATUS_OK;
    }
    return status;
}

static bool parse_decimal_field(
    const uint8_t *field,
    size_t field_length,
    unsigned int maximum,
    unsigned int *value_out)
{
    unsigned int value = 0;
    size_t index;

    if (field_length == 0) {
        return false;
    }
    for (index = 0; index < field_length; index++) {
        unsigned int digit;

        if (field[index] < (uint8_t)'0' || field[index] > (uint8_t)'9') {
            return false;
        }
        digit = (unsigned int)(field[index] - (uint8_t)'0');
        if (value > (maximum - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }
    *value_out = value;
    return true;
}

static bool next_osc_field(
    const uint8_t *payload,
    size_t payload_length,
    size_t *offset,
    const uint8_t **field_out,
    size_t *field_length_out)
{
    size_t end;

    if (*offset > payload_length) {
        return false;
    }
    end = *offset;
    while (end < payload_length && payload[end] != (uint8_t)';') {
        end++;
    }
    *field_out = payload + *offset;
    *field_length_out = end - *offset;
    *offset = end < payload_length ? end + 1 : payload_length + 1;
    return true;
}

static int hexadecimal_digit(uint8_t byte)
{
    if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') {
        return byte - (uint8_t)'0';
    }
    if (byte >= (uint8_t)'a' && byte <= (uint8_t)'f') {
        return byte - (uint8_t)'a' + 10;
    }
    if (byte >= (uint8_t)'A' && byte <= (uint8_t)'F') {
        return byte - (uint8_t)'A' + 10;
    }
    return -1;
}

static bool parse_color_component(
    const uint8_t *text,
    size_t length,
    uint8_t *component_out)
{
    unsigned int value = 0;
    unsigned int maximum;
    size_t index;

    if (length == 0 || length > 4) {
        return false;
    }
    for (index = 0; index < length; index++) {
        int digit = hexadecimal_digit(text[index]);

        if (digit < 0) {
            return false;
        }
        value = value * 16u + (unsigned int)digit;
    }
    maximum = (1u << (unsigned int)(length * 4)) - 1u;
    *component_out = (uint8_t)((value * 255u + maximum / 2u) / maximum);
    return true;
}

static bool parse_rgb_specification(
    const uint8_t *text,
    size_t length,
    boxrig_rgb *color_out)
{
    if (length >= 4 && text[0] == (uint8_t)'#') {
        size_t component_length = (length - 1) / 3;

        if ((length - 1) % 3 != 0 || component_length == 0 ||
            component_length > 4) {
            return false;
        }
        return parse_color_component(
                   text + 1,
                   component_length,
                   &color_out->red) &&
               parse_color_component(
                   text + 1 + component_length,
                   component_length,
                   &color_out->green) &&
               parse_color_component(
                   text + 1 + component_length * 2,
                   component_length,
                   &color_out->blue);
    }
    if (length >= 9 && memcmp(text, "rgb:", 4) == 0) {
        const uint8_t *components[3];
        size_t component_lengths[3];
        size_t component = 0;
        size_t start = 4;
        size_t index;

        for (index = 4; index <= length; index++) {
            if (index == length || text[index] == (uint8_t)'/') {
                if (component >= 3) {
                    return false;
                }
                components[component] = text + start;
                component_lengths[component] = index - start;
                component++;
                start = index + 1;
            }
        }
        return component == 3 &&
               parse_color_component(
                   components[0],
                   component_lengths[0],
                   &color_out->red) &&
               parse_color_component(
                   components[1],
                   component_lengths[1],
                   &color_out->green) &&
               parse_color_component(
                   components[2],
                   component_lengths[2],
                   &color_out->blue);
    }
    return false;
}

static bool rgb_equal(boxrig_rgb left, boxrig_rgb right)
{
    return left.red == right.red && left.green == right.green &&
           left.blue == right.blue;
}

static bool append_color_response(
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length,
    unsigned int command,
    bool has_index,
    unsigned int index,
    boxrig_rgb color,
    boxrig_parser_string_terminator terminator)
{
    char response[64];
    int length;
    size_t terminator_length;

    if (has_index) {
        length = snprintf(
            response,
            sizeof(response),
            "\x1b]%u;%u;rgb:%04x/%04x/%04x",
            command,
            index,
            (unsigned int)color.red * 257u,
            (unsigned int)color.green * 257u,
            (unsigned int)color.blue * 257u);
    } else {
        length = snprintf(
            response,
            sizeof(response),
            "\x1b]%u;rgb:%04x/%04x/%04x",
            command,
            (unsigned int)color.red * 257u,
            (unsigned int)color.green * 257u,
            (unsigned int)color.blue * 257u);
    }
    terminator_length = terminator == BOXRIG_PARSER_STRING_TERMINATOR_BEL
                            ? 1
                            : 2;
    if (length < 0 || (size_t)length >= sizeof(response) ||
        (size_t)length > output_capacity - *output_length ||
        terminator_length >
            output_capacity - *output_length - (size_t)length) {
        return false;
    }
    memcpy(output + *output_length, response, (size_t)length);
    *output_length += (size_t)length;
    if (terminator == BOXRIG_PARSER_STRING_TERMINATOR_BEL) {
        output[*output_length] = 0x07;
        (*output_length)++;
    } else {
        output[*output_length] = 0x1b;
        output[*output_length + 1] = (uint8_t)'\\';
        *output_length += 2;
    }
    return true;
}

static uint64_t next_color_revision(uint64_t revision)
{
    return revision == UINT64_MAX ? 1 : revision + 1;
}

static void finish_color_change(boxrig_terminal *terminal, bool changed)
{
    if (changed) {
        terminal->color_revision = next_color_revision(
            terminal->color_revision);
        boxrig_screen_mark_all(&terminal->screen);
    }
}

static boxrig_status dispatch_palette_osc(
    boxrig_terminal *terminal,
    const uint8_t *payload,
    size_t payload_length,
    boxrig_parser_string_terminator terminator)
{
    boxrig_rgb palette[256];
    uint8_t *response = NULL;
    size_t response_capacity;
    size_t response_length = 0;
    size_t query_count = 0;
    size_t offset = 0;
    const uint8_t *field;
    size_t field_length;
    boxrig_status status;
    bool changed = false;

    while (next_osc_field(
        payload, payload_length, &offset, &field, &field_length)) {
        const uint8_t *specification;
        size_t specification_length;

        if (!next_osc_field(
                payload,
                payload_length,
                &offset,
                &specification,
                &specification_length)) {
            return BOXRIG_STATUS_OK;
        }
        (void)field;
        (void)field_length;
        if (specification_length == 1 &&
            specification[0] == (uint8_t)'?') {
            query_count++;
        }
    }
    if (query_count > SIZE_MAX / 32u) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    response_capacity = query_count * 32u;
    if (response_capacity != 0) {
        response = malloc(response_capacity);
        if (response == NULL) {
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
    }

    memcpy(palette, terminal->palette, sizeof(palette));
    offset = 0;
    while (next_osc_field(
        payload, payload_length, &offset, &field, &field_length)) {
        const uint8_t *specification;
        size_t specification_length;
        unsigned int palette_index;
        boxrig_rgb color;

        if (!next_osc_field(
                payload,
                payload_length,
                &offset,
                &specification,
                &specification_length) ||
            !parse_decimal_field(
                field,
                field_length,
                255,
                &palette_index)) {
            free(response);
            return BOXRIG_STATUS_OK;
        }
        if (specification_length == 1 &&
            specification[0] == (uint8_t)'?') {
            if (!append_color_response(
                    response,
                    response_capacity,
                    &response_length,
                    4,
                    true,
                    palette_index,
                    palette[palette_index],
                    terminator)) {
                free(response);
                return BOXRIG_STATUS_LIMIT_EXCEEDED;
            }
        } else if (parse_rgb_specification(
                       specification,
                       specification_length,
                       &color)) {
            palette[palette_index] = color;
        } else {
            free(response);
            return BOXRIG_STATUS_OK;
        }
    }

    status = boxrig_terminal_append_output(
        terminal,
        response,
        response_length);
    if (status != BOXRIG_STATUS_OK) {
        free(response);
        return status;
    }
    free(response);
    for (offset = 0; offset < 256; offset++) {
        if (!rgb_equal(terminal->palette[offset], palette[offset])) {
            terminal->palette[offset] = palette[offset];
            changed = true;
        }
    }
    finish_color_change(terminal, changed);
    return BOXRIG_STATUS_OK;
}

static boxrig_status dispatch_palette_reset_osc(
    boxrig_terminal *terminal,
    const uint8_t *payload,
    size_t payload_length)
{
    bool reset[256] = {false};
    size_t offset = 0;
    const uint8_t *field;
    size_t field_length;
    size_t index;
    bool changed = false;

    if (payload_length == 0) {
        memset(reset, 1, sizeof(reset));
    } else {
        while (next_osc_field(
            payload, payload_length, &offset, &field, &field_length)) {
            unsigned int palette_index;

            if (!parse_decimal_field(
                    field,
                    field_length,
                    255,
                    &palette_index)) {
                return BOXRIG_STATUS_OK;
            }
            reset[palette_index] = true;
        }
    }
    for (index = 0; index < sizeof(reset) / sizeof(reset[0]); index++) {
        if (reset[index] && boxrig_terminal_reset_palette_color(
                                terminal, (uint8_t)index)) {
            changed = true;
        }
    }
    finish_color_change(terminal, changed);
    return BOXRIG_STATUS_OK;
}

static boxrig_status dispatch_dynamic_color_reset_osc(
    boxrig_terminal *terminal,
    unsigned int command,
    size_t payload_length)
{
    boxrig_dynamic_color color =
        (boxrig_dynamic_color)(command - 110u);
    bool changed;

    if (payload_length != 0) {
        return BOXRIG_STATUS_OK;
    }
    changed = boxrig_terminal_reset_dynamic_color(terminal, color);
    finish_color_change(terminal, changed);
    return BOXRIG_STATUS_OK;
}

static boxrig_status dispatch_dynamic_color_osc(
    boxrig_terminal *terminal,
    unsigned int command,
    const uint8_t *payload,
    size_t payload_length,
    boxrig_parser_string_terminator terminator)
{
    boxrig_rgb colors[3];
    uint8_t response[96];
    size_t response_length = 0;
    size_t offset = 0;
    const uint8_t *field;
    size_t field_length;
    unsigned int role = command - 10u;
    boxrig_status status;
    bool changed = false;

    memcpy(colors, terminal->dynamic_colors, sizeof(colors));
    while (next_osc_field(
        payload, payload_length, &offset, &field, &field_length)) {
        boxrig_rgb color;

        if (role > BOXRIG_DYNAMIC_COLOR_CURSOR) {
            return BOXRIG_STATUS_OK;
        }
        if (field_length == 1 && field[0] == (uint8_t)'?') {
            if (!append_color_response(
                    response,
                    sizeof(response),
                    &response_length,
                    role + 10u,
                    false,
                    0,
                    colors[role],
                    terminator)) {
                return BOXRIG_STATUS_LIMIT_EXCEEDED;
            }
        } else if (parse_rgb_specification(field, field_length, &color)) {
            colors[role] = color;
        } else {
            return BOXRIG_STATUS_OK;
        }
        role++;
    }
    status = boxrig_terminal_append_output(
        terminal,
        response,
        response_length);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    for (role = 0; role < 3; role++) {
        if (!rgb_equal(terminal->dynamic_colors[role], colors[role])) {
            terminal->dynamic_colors[role] = colors[role];
            changed = true;
        }
    }
    finish_color_change(terminal, changed);
    return BOXRIG_STATUS_OK;
}

static boxrig_status dispatch_hyperlink_osc(
    boxrig_terminal *terminal,
    const uint8_t *payload,
    size_t payload_length)
{
    const uint8_t *id = NULL;
    size_t id_length = 0;
    const uint8_t *uri;
    size_t uri_length;
    size_t separator = 0;
    size_t parameter_start = 0;
    uint8_t *sanitized_id = NULL;
    size_t sanitized_id_length = 0;
    uint8_t *sanitized_uri = NULL;
    size_t sanitized_uri_length = 0;
    boxrig_status status;

    while (separator < payload_length &&
           payload[separator] != (uint8_t)';') {
        separator++;
    }
    if (separator == payload_length) {
        return BOXRIG_STATUS_OK;
    }
    while (parameter_start <= separator) {
        size_t parameter_end = parameter_start;

        while (parameter_end < separator &&
               payload[parameter_end] != (uint8_t)':') {
            parameter_end++;
        }
        if (parameter_end - parameter_start >= 3 &&
            memcmp(payload + parameter_start, "id=", 3) == 0) {
            id = payload + parameter_start + 3;
            id_length = parameter_end - parameter_start - 3;
            break;
        }
        parameter_start = parameter_end + 1;
    }
    uri = payload + separator + 1;
    uri_length = payload_length - separator - 1;
    status = sanitize_host_text(
        id,
        id_length,
        &sanitized_id,
        &sanitized_id_length);
    if (status == BOXRIG_STATUS_OK) {
        status = sanitize_host_text(
            uri,
            uri_length,
            &sanitized_uri,
            &sanitized_uri_length);
    }
    if (status == BOXRIG_STATUS_OK) {
        status = boxrig_terminal_set_active_hyperlink(
            terminal,
            sanitized_id,
            sanitized_id_length,
            sanitized_uri,
            sanitized_uri_length);
    }
    free(sanitized_uri);
    free(sanitized_id);
    if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) {
        terminal->screen.active_hyperlink = 0;
        return BOXRIG_STATUS_OK;
    }
    return status;
}

static bool parse_clipboard_selectors(
    const uint8_t *text,
    size_t length,
    uint8_t selectors[12],
    size_t *selector_count_out,
    uint32_t *flags_out)
{
    size_t count = 0;
    uint32_t flags = 0;
    size_t index;

    if (length == 0) {
        selectors[0] = (uint8_t)'s';
        selectors[1] = (uint8_t)'0';
        *selector_count_out = 2;
        *flags_out = BOXRIG_CLIPBOARD_SELECTION_SELECT |
                     BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_0;
        return true;
    }
    for (index = 0; index < length; index++) {
        uint32_t flag;
        size_t existing;

        if (text[index] == (uint8_t)'c') {
            flag = BOXRIG_CLIPBOARD_SELECTION_CLIPBOARD;
        } else if (text[index] == (uint8_t)'p') {
            flag = BOXRIG_CLIPBOARD_SELECTION_PRIMARY;
        } else if (text[index] == (uint8_t)'q') {
            flag = BOXRIG_CLIPBOARD_SELECTION_SECONDARY;
        } else if (text[index] == (uint8_t)'s') {
            flag = BOXRIG_CLIPBOARD_SELECTION_SELECT;
        } else if (text[index] >= (uint8_t)'0' &&
                   text[index] <= (uint8_t)'7') {
            flag = (uint32_t)BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_0 <<
                   (unsigned int)(text[index] - (uint8_t)'0');
        } else {
            return false;
        }
        for (existing = 0; existing < count; existing++) {
            if (selectors[existing] == text[index]) {
                break;
            }
        }
        if (existing == count) {
            if (count == 12) {
                return false;
            }
            selectors[count] = text[index];
            count++;
        }
        flags |= flag;
    }
    *selector_count_out = count;
    *flags_out = flags;
    return true;
}

static int base64_digit(uint8_t byte)
{
    if (byte >= (uint8_t)'A' && byte <= (uint8_t)'Z') {
        return byte - (uint8_t)'A';
    }
    if (byte >= (uint8_t)'a' && byte <= (uint8_t)'z') {
        return byte - (uint8_t)'a' + 26;
    }
    if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') {
        return byte - (uint8_t)'0' + 52;
    }
    if (byte == (uint8_t)'+') {
        return 62;
    }
    if (byte == (uint8_t)'/') {
        return 63;
    }
    return -1;
}

static boxrig_status decode_base64(
    const uint8_t *input,
    size_t input_length,
    size_t maximum_length,
    uint8_t **output_out,
    size_t *output_length_out,
    bool *valid_out)
{
    size_t padding = 0;
    size_t output_length;
    uint8_t *output = NULL;
    size_t input_offset;
    size_t output_offset = 0;

    *output_out = NULL;
    *output_length_out = 0;
    *valid_out = false;
    if (input_length == 0) {
        *valid_out = true;
        return BOXRIG_STATUS_OK;
    }
    if (input_length % 4 != 0) {
        return BOXRIG_STATUS_OK;
    }
    if (input[input_length - 1] == (uint8_t)'=') {
        padding++;
    }
    if (input_length >= 2 && input[input_length - 2] == (uint8_t)'=') {
        padding++;
    }
    if (input_length / 4 > SIZE_MAX / 3) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    output_length = input_length / 4 * 3 - padding;
    if (output_length > maximum_length) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (output_length != 0) {
        output = malloc(output_length);
        if (output == NULL) {
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
    }
    for (input_offset = 0;
         input_offset < input_length;
         input_offset += 4) {
        int digits[4];
        bool last = input_offset + 4 == input_length;
        size_t digit_index;
        uint32_t value;

        for (digit_index = 0; digit_index < 4; digit_index++) {
            uint8_t byte = input[input_offset + digit_index];

            if (byte == (uint8_t)'=') {
                if (!last || digit_index < 4 - padding) {
                    free(output);
                    return BOXRIG_STATUS_OK;
                }
                digits[digit_index] = 0;
            } else {
                digits[digit_index] = base64_digit(byte);
                if (digits[digit_index] < 0 ||
                    (last && digit_index >= 4 - padding)) {
                    free(output);
                    return BOXRIG_STATUS_OK;
                }
            }
        }
        if (last &&
            ((padding == 2 && (digits[1] & 0x0f) != 0) ||
             (padding == 1 && (digits[2] & 0x03) != 0))) {
            free(output);
            return BOXRIG_STATUS_OK;
        }
        value = ((uint32_t)digits[0] << 18) |
                ((uint32_t)digits[1] << 12) |
                ((uint32_t)digits[2] << 6) |
                (uint32_t)digits[3];
        if (output_offset < output_length) {
            output[output_offset] = (uint8_t)(value >> 16);
            output_offset++;
        }
        if (output_offset < output_length) {
            output[output_offset] = (uint8_t)(value >> 8);
            output_offset++;
        }
        if (output_offset < output_length) {
            output[output_offset] = (uint8_t)value;
            output_offset++;
        }
    }
    *output_out = output;
    *output_length_out = output_length;
    *valid_out = true;
    return BOXRIG_STATUS_OK;
}

static boxrig_status append_empty_clipboard_response(
    boxrig_terminal *terminal,
    const uint8_t *selectors,
    size_t selector_count,
    boxrig_parser_string_terminator terminator)
{
    uint8_t response[20];
    size_t length = 0;
    boxrig_status status;

    memcpy(response, "\x1b]52;", 5);
    length = 5;
    memcpy(response + length, selectors, selector_count);
    length += selector_count;
    response[length] = (uint8_t)';';
    length++;
    if (terminator == BOXRIG_PARSER_STRING_TERMINATOR_BEL) {
        response[length] = 0x07;
        length++;
    } else {
        response[length] = 0x1b;
        response[length + 1] = (uint8_t)'\\';
        length += 2;
    }
    status = boxrig_terminal_append_output(terminal, response, length);
    return status == BOXRIG_STATUS_LIMIT_EXCEEDED
               ? BOXRIG_STATUS_OK
               : status;
}

static boxrig_status dispatch_clipboard_osc(
    boxrig_terminal *terminal,
    const uint8_t *payload,
    size_t payload_length,
    boxrig_parser_string_terminator terminator)
{
    uint8_t selectors[12];
    size_t selector_count;
    uint32_t flags;
    size_t separator = 0;
    const uint8_t *data;
    size_t data_length;

    while (separator < payload_length &&
           payload[separator] != (uint8_t)';') {
        separator++;
    }
    if (separator == payload_length ||
        !parse_clipboard_selectors(
            payload,
            separator,
            selectors,
            &selector_count,
            &flags)) {
        return BOXRIG_STATUS_OK;
    }
    data = payload + separator + 1;
    data_length = payload_length - separator - 1;
    if (data_length == 1 && data[0] == (uint8_t)'?') {
        size_t encoded_capacity;
        size_t terminator_length =
            terminator == BOXRIG_PARSER_STRING_TERMINATOR_BEL ? 1 : 2;
        size_t response_reservation;
        boxrig_status status;

        if (terminal->clipboard_capacity > SIZE_MAX - 2) {
            return append_empty_clipboard_response(
                terminal,
                selectors,
                selector_count,
                terminator);
        }
        encoded_capacity = (terminal->clipboard_capacity + 2) / 3;
        if (encoded_capacity > SIZE_MAX / 4) {
            return append_empty_clipboard_response(
                terminal,
                selectors,
                selector_count,
                terminator);
        }
        encoded_capacity *= 4;
        if (encoded_capacity >
            SIZE_MAX - 6 - selector_count - terminator_length) {
            return append_empty_clipboard_response(
                terminal,
                selectors,
                selector_count,
                terminator);
        }
        response_reservation = 6 + selector_count + encoded_capacity +
                               terminator_length;
        if (terminal->clipboard_capacity == 0 ||
            response_reservation > terminal->output_capacity) {
            return append_empty_clipboard_response(
                terminal,
                selectors,
                selector_count,
                terminator);
        }
        status = boxrig_terminal_queue_clipboard_request(
            terminal,
            true,
            flags,
            NULL,
            0,
            selectors,
            selector_count,
            terminator,
            response_reservation);
        if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) {
            return append_empty_clipboard_response(
                terminal,
                selectors,
                selector_count,
                terminator);
        }
        return status;
    }
    if (terminal->clipboard_capacity != 0) {
        uint8_t *decoded = NULL;
        size_t decoded_length = 0;
        bool valid = false;
        boxrig_status status = decode_base64(
            data,
            data_length,
            terminal->clipboard_capacity,
            &decoded,
            &decoded_length,
            &valid);

        if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) {
            return BOXRIG_STATUS_OK;
        }
        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
        if (!valid) {
            free(decoded);
            decoded = NULL;
            decoded_length = 0;
        }
        status = boxrig_terminal_queue_clipboard_request(
            terminal,
            false,
            flags,
            decoded,
            decoded_length,
            selectors,
            selector_count,
            terminator,
            0);
        free(decoded);
        return status == BOXRIG_STATUS_LIMIT_EXCEEDED
                   ? BOXRIG_STATUS_OK
                   : status;
    }
    return BOXRIG_STATUS_OK;
}

static boxrig_status dispatch_osc(
    boxrig_terminal *terminal,
    boxrig_parser_string_terminator terminator)
{
    const uint8_t *payload;
    size_t payload_length;
    size_t separator = 0;
    unsigned int command;

    while (separator < terminal->osc_buffer_length &&
           terminal->osc_buffer[separator] != (uint8_t)';') {
        separator++;
    }
    if (!parse_decimal_field(
            terminal->osc_buffer,
            separator,
            UINT_MAX,
            &command)) {
        return BOXRIG_STATUS_OK;
    }
    if (separator < terminal->osc_buffer_length) {
        payload = terminal->osc_buffer + separator + 1;
        payload_length = terminal->osc_buffer_length - separator - 1;
    } else {
        payload = terminal->osc_buffer + separator;
        payload_length = 0;
    }
    if (command == 0) {
        return push_text_host_item(
            terminal,
            BOXRIG_HOST_ITEM_TITLE,
            BOXRIG_TITLE_ICON | BOXRIG_TITLE_WINDOW,
            payload,
            payload_length);
    }
    if (command == 1) {
        return push_text_host_item(
            terminal,
            BOXRIG_HOST_ITEM_TITLE,
            BOXRIG_TITLE_ICON,
            payload,
            payload_length);
    }
    if (command == 2) {
        return push_text_host_item(
            terminal,
            BOXRIG_HOST_ITEM_TITLE,
            BOXRIG_TITLE_WINDOW,
            payload,
            payload_length);
    }
    if (command == 4) {
        return dispatch_palette_osc(
            terminal,
            payload,
            payload_length,
            terminator);
    }
    if (command == 104) {
        return dispatch_palette_reset_osc(
            terminal,
            payload,
            payload_length);
    }
    if (command == 7) {
        return push_text_host_item(
            terminal,
            BOXRIG_HOST_ITEM_WORKING_DIRECTORY,
            0,
            payload,
            payload_length);
    }
    if (command == 8) {
        return dispatch_hyperlink_osc(
            terminal,
            payload,
            payload_length);
    }
    if (command >= 10 && command <= 12) {
        return dispatch_dynamic_color_osc(
            terminal,
            command,
            payload,
            payload_length,
            terminator);
    }
    if (command >= 110 && command <= 112) {
        return dispatch_dynamic_color_reset_osc(
            terminal,
            command,
            payload_length);
    }
    if (command == 52) {
        return dispatch_clipboard_osc(
            terminal,
            payload,
            payload_length,
            terminator);
    }
    if (command == 99) {
        return boxrig_notification_dispatch(
            terminal,
            payload,
            payload_length,
            terminator);
    }
    return BOXRIG_STATUS_OK;
}

static boxrig_status interpreter_string_end(
    void *context,
    bool cancelled,
    boxrig_parser_string_terminator terminator)
{
    boxrig_terminal *terminal = context;
    boxrig_status status = BOXRIG_STATUS_OK;

    if (terminal->active_string_kind == BOXRIG_PARSER_STRING_APC) {
        status = boxrig_kitty_graphics_end(&terminal->graphics, cancelled);
    } else if (!cancelled &&
               terminal->active_string_kind == BOXRIG_PARSER_STRING_OSC) {
        status = dispatch_osc(terminal, terminator);
    }
    if (status == BOXRIG_STATUS_OK &&
        terminal->active_string_kind == BOXRIG_PARSER_STRING_OSC) {
        terminal->osc_buffer_length = 0;
    }
    return status;
}

boxrig_parser_actions boxrig_interpreter_actions(boxrig_terminal *terminal)
{
    boxrig_parser_actions actions;

    actions.context = terminal;
    actions.print = interpreter_print;
    actions.flush_print = interpreter_flush_print;
    actions.execute = interpreter_execute;
    actions.dispatch_escape = interpreter_dispatch_escape;
    actions.dispatch_csi = interpreter_dispatch_csi;
    actions.dcs_hook = interpreter_dcs_hook;
    actions.dcs_put = interpreter_dcs_put;
    actions.dcs_unhook = interpreter_dcs_unhook;
    actions.string_start = interpreter_string_start;
    actions.string_put = interpreter_string_put;
    actions.string_end = interpreter_string_end;
    return actions;
}
