/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "terminal_internal.h"

#include <string.h>

typedef struct response_builder {
    uint8_t *bytes;
    size_t capacity;
    size_t length;
    bool valid;
} response_builder;

static void builder_append_bytes(
    response_builder *builder,
    const uint8_t *bytes,
    size_t length)
{
    if (!builder->valid || length > builder->capacity - builder->length) {
        builder->valid = false;
        return;
    }
    memcpy(builder->bytes + builder->length, bytes, length);
    builder->length += length;
}

static void builder_append_byte(response_builder *builder, uint8_t byte)
{
    builder_append_bytes(builder, &byte, 1);
}

static void builder_append_size(response_builder *builder, size_t value)
{
    uint8_t digits[sizeof(size_t) * 3];
    size_t length = 0;

    do {
        digits[length] = (uint8_t)('0' + value % 10);
        length++;
        value /= 10;
    } while (value != 0);
    while (length != 0) {
        length--;
        builder_append_byte(builder, digits[length]);
    }
}

static void builder_append_parameter(
    response_builder *builder,
    size_t value)
{
    builder_append_byte(builder, (uint8_t)';');
    builder_append_size(builder, value);
}

static void builder_append_color(
    response_builder *builder,
    boxrig_color color,
    size_t basic_base,
    size_t bright_base,
    size_t extended_selector,
    bool extended_only)
{
    if (color.kind == BOXRIG_COLOR_DEFAULT) {
        return;
    }
    if (color.kind == BOXRIG_COLOR_INDEXED) {
        if (!extended_only && color.index < 8) {
            builder_append_parameter(builder, basic_base + color.index);
        } else if (!extended_only && color.index < 16) {
            builder_append_parameter(
                builder,
                bright_base + (size_t)color.index - 8);
        } else {
            builder_append_parameter(builder, extended_selector);
            builder_append_parameter(builder, 5);
            builder_append_parameter(builder, color.index);
        }
        return;
    }
    if (color.kind == BOXRIG_COLOR_RGB) {
        builder_append_parameter(builder, extended_selector);
        builder_append_parameter(builder, 2);
        builder_append_parameter(builder, color.red);
        builder_append_parameter(builder, color.green);
        builder_append_parameter(builder, color.blue);
        return;
    }
    builder->valid = false;
}

static void builder_start_success(response_builder *builder)
{
    static const uint8_t prefix[] = "\x1bP1$r";

    builder_append_bytes(builder, prefix, sizeof(prefix) - 1);
}

static void builder_finish_success(
    response_builder *builder,
    const uint8_t *request,
    size_t request_length)
{
    static const uint8_t suffix[] = "\x1b\\";

    builder_append_bytes(builder, request, request_length);
    builder_append_bytes(builder, suffix, sizeof(suffix) - 1);
}

static void build_rendition_response(
    const boxrig_screen *screen,
    response_builder *builder)
{
    const boxrig_cell_style *style = &screen->style;

    builder_start_success(builder);
    builder_append_byte(builder, (uint8_t)'0');
    if ((style->attributes & BOXRIG_CELL_BOLD) != 0) {
        builder_append_parameter(builder, 1);
    }
    if ((style->attributes & BOXRIG_CELL_FAINT) != 0) {
        builder_append_parameter(builder, 2);
    }
    if ((style->attributes & BOXRIG_CELL_ITALIC) != 0) {
        builder_append_parameter(builder, 3);
    }
    if (style->underline_style != BOXRIG_UNDERLINE_NONE) {
        builder_append_byte(builder, (uint8_t)';');
        builder_append_byte(builder, (uint8_t)'4');
        builder_append_byte(builder, (uint8_t)':');
        builder_append_size(builder, (size_t)style->underline_style);
    }
    if ((style->attributes & BOXRIG_CELL_BLINK) != 0) {
        builder_append_parameter(builder,
            (style->attributes & BOXRIG_CELL_RAPID_BLINK) != 0 ? 6 : 5);
    }
    if ((style->attributes & BOXRIG_CELL_INVERSE) != 0) {
        builder_append_parameter(builder, 7);
    }
    if ((style->attributes & BOXRIG_CELL_CONCEALED) != 0) {
        builder_append_parameter(builder, 8);
    }
    if ((style->attributes & BOXRIG_CELL_STRIKETHROUGH) != 0) {
        builder_append_parameter(builder, 9);
    }
    builder_append_color(builder, style->foreground, 30, 90, 38, false);
    builder_append_color(builder, style->background, 40, 100, 48, false);
    builder_append_color(builder, style->underline_color, 0, 0, 58, true);
    builder_finish_success(builder, (const uint8_t *)"m", 1);
}

static void build_vertical_margin_response(
    const boxrig_screen *screen,
    response_builder *builder)
{
    builder_start_success(builder);
    builder_append_size(builder, screen->active->scroll_region_top + 1);
    builder_append_parameter(
        builder,
        screen->active->scroll_region_bottom + 1);
    builder_finish_success(builder, (const uint8_t *)"r", 1);
}

static void build_horizontal_margin_response(
    const boxrig_screen *screen,
    response_builder *builder)
{
    size_t left = screen->left_right_margin_mode
                      ? screen->active->horizontal_margin_left
                      : 0;
    size_t right = screen->left_right_margin_mode
                       ? screen->active->horizontal_margin_right
                       : screen->columns - 1;

    builder_start_success(builder);
    builder_append_size(builder, left + 1);
    builder_append_parameter(builder, right + 1);
    builder_finish_success(builder, (const uint8_t *)"s", 1);
}

static void build_cursor_response(
    const boxrig_screen *screen,
    response_builder *builder)
{
    size_t value;
    static const uint8_t request[] = {' ', 'q'};

    if (screen->cursor_shape == BOXRIG_CURSOR_BLOCK) {
        value = screen->cursor_blinking ? 1 : 2;
    } else if (screen->cursor_shape == BOXRIG_CURSOR_UNDERLINE) {
        value = screen->cursor_blinking ? 3 : 4;
    } else if (screen->cursor_shape == BOXRIG_CURSOR_BAR) {
        value = screen->cursor_blinking ? 5 : 6;
    } else {
        builder->valid = false;
        return;
    }
    builder_start_success(builder);
    builder_append_size(builder, value);
    builder_finish_success(builder, request, sizeof(request));
}

static void build_protection_response(
    const boxrig_screen *screen,
    response_builder *builder)
{
    static const uint8_t request[] = {'"', 'q'};

    builder_start_success(builder);
    builder_append_size(builder, screen->style.protected_cell ? 1 : 0);
    builder_finish_success(builder, request, sizeof(request));
}

static void build_failure_response(response_builder *builder)
{
    static const uint8_t response[] = "\x1bP0$r\x1b\\";

    builder_append_bytes(builder, response, sizeof(response) - 1);
}

static boxrig_status prepare_response(boxrig_terminal *terminal)
{
    boxrig_dcs_state *state = &terminal->dcs;
    response_builder builder;

    builder.bytes = state->response;
    builder.capacity = sizeof(state->response);
    builder.length = 0;
    builder.valid = true;
    if (!state->request_overflow && state->request_length == 1 &&
        state->request[0] == (uint8_t)'m') {
        build_rendition_response(&terminal->screen, &builder);
    } else if (!state->request_overflow && state->request_length == 1 &&
               state->request[0] == (uint8_t)'r') {
        build_vertical_margin_response(&terminal->screen, &builder);
    } else if (!state->request_overflow && state->request_length == 1 &&
               state->request[0] == (uint8_t)'s') {
        build_horizontal_margin_response(&terminal->screen, &builder);
    } else if (!state->request_overflow && state->request_length == 2 &&
               state->request[0] == (uint8_t)' ' &&
               state->request[1] == (uint8_t)'q') {
        build_cursor_response(&terminal->screen, &builder);
    } else if (!state->request_overflow && state->request_length == 2 &&
               state->request[0] == (uint8_t)'"' &&
               state->request[1] == (uint8_t)'q') {
        build_protection_response(&terminal->screen, &builder);
    } else {
        build_failure_response(&builder);
    }
    if (!builder.valid) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    state->response_length = builder.length;
    state->response_ready = true;
    return BOXRIG_STATUS_OK;
}

void boxrig_dcs_reset(boxrig_dcs_state *state)
{
    memset(state, 0, sizeof(*state));
}

boxrig_status boxrig_dcs_hook(
    boxrig_terminal *terminal,
    uint8_t private_marker,
    const boxrig_parser_parameter *parameters,
    size_t parameter_count,
    const uint8_t *intermediates,
    size_t intermediate_count,
    uint8_t final_byte,
    bool *passthrough_out)
{
    (void)parameters;
    *passthrough_out = false;
    if (private_marker != 0 || parameter_count != 0 ||
        intermediate_count != 1 || intermediates[0] != (uint8_t)'$' ||
        final_byte != (uint8_t)'q') {
        return BOXRIG_STATUS_OK;
    }
    boxrig_dcs_reset(&terminal->dcs);
    terminal->dcs.active = true;
    *passthrough_out = true;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_dcs_put(boxrig_terminal *terminal, uint8_t byte)
{
    boxrig_dcs_state *state = &terminal->dcs;

    if (!state->active) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (state->request_length < sizeof(state->request)) {
        state->request[state->request_length] = byte;
        state->request_length++;
    } else {
        state->request_overflow = true;
    }
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_dcs_unhook(
    boxrig_terminal *terminal,
    bool cancelled,
    boxrig_parser_string_terminator terminator)
{
    boxrig_dcs_state *state = &terminal->dcs;
    boxrig_status status;

    if (!state->active) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (cancelled || terminator != BOXRIG_PARSER_STRING_TERMINATOR_ST) {
        boxrig_dcs_reset(state);
        return BOXRIG_STATUS_OK;
    }
    if (!state->response_ready) {
        status = prepare_response(terminal);
        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
    }
    status = boxrig_terminal_append_output(
        terminal,
        state->response,
        state->response_length);
    if (status == BOXRIG_STATUS_OK) {
        boxrig_dcs_reset(state);
    }
    return status;
}
