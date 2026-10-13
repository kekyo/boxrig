/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_DCS_H
#define BOXRIG_DCS_H

#include "boxrig/terminal.h"
#include "parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    BOXRIG_DECRQSS_REQUEST_CAPACITY = 2,
    BOXRIG_DECRQSS_RESPONSE_CAPACITY = 256
};

typedef struct boxrig_dcs_state {
    uint8_t request[BOXRIG_DECRQSS_REQUEST_CAPACITY];
    size_t request_length;
    bool request_overflow;
    /* A prepared response remains owned here until its atomic append succeeds. */
    uint8_t response[BOXRIG_DECRQSS_RESPONSE_CAPACITY];
    size_t response_length;
    bool response_ready;
    bool active;
} boxrig_dcs_state;

void boxrig_dcs_reset(boxrig_dcs_state *state);
boxrig_status boxrig_dcs_hook(
    boxrig_terminal *terminal,
    uint8_t private_marker,
    const boxrig_parser_parameter *parameters,
    size_t parameter_count,
    const uint8_t *intermediates,
    size_t intermediate_count,
    uint8_t final_byte,
    bool *passthrough_out);
boxrig_status boxrig_dcs_put(boxrig_terminal *terminal, uint8_t byte);
boxrig_status boxrig_dcs_unhook(
    boxrig_terminal *terminal,
    bool cancelled,
    boxrig_parser_string_terminator terminator);

#endif
