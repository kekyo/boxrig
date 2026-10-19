/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_PARSER_H
#define BOXRIG_PARSER_H

#include "boxrig/terminal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOXRIG_PARSER_MAX_PARAMETERS 32
#define BOXRIG_PARSER_MAX_INTERMEDIATES 4

typedef enum boxrig_parser_string_kind {
    BOXRIG_PARSER_STRING_OSC = 0,
    BOXRIG_PARSER_STRING_APC,
    BOXRIG_PARSER_STRING_PM,
    BOXRIG_PARSER_STRING_SOS
} boxrig_parser_string_kind;

typedef enum boxrig_parser_string_terminator {
    BOXRIG_PARSER_STRING_TERMINATOR_BEL = 0,
    BOXRIG_PARSER_STRING_TERMINATOR_ST,
    BOXRIG_PARSER_STRING_TERMINATOR_CANCEL
} boxrig_parser_string_terminator;

typedef struct boxrig_parser_parameter {
    uint32_t value;
    bool has_value;
    bool subparameter;
} boxrig_parser_parameter;

typedef struct boxrig_parser_actions {
    void *context;
    boxrig_status (*print)(void *context, uint8_t byte);
    /* Optional printable-ASCII run. Success consumes the entire run; on
     * failure consumed_out excludes the byte to retry. */
    boxrig_status (*print_run)(
        void *context,
        const uint8_t *bytes,
        size_t length,
        size_t *consumed_out);
    boxrig_status (*flush_print)(void *context);
    boxrig_status (*execute)(void *context, uint8_t control);
    boxrig_status (*dispatch_escape)(
        void *context,
        const uint8_t *intermediates,
        size_t intermediate_count,
        uint8_t final_byte);
    boxrig_status (*dispatch_csi)(
        void *context,
        uint8_t private_marker,
        const boxrig_parser_parameter *parameters,
        size_t parameter_count,
        const uint8_t *intermediates,
        size_t intermediate_count,
        uint8_t final_byte);
    boxrig_status (*dcs_hook)(
        void *context,
        uint8_t private_marker,
        const boxrig_parser_parameter *parameters,
        size_t parameter_count,
        const uint8_t *intermediates,
        size_t intermediate_count,
        uint8_t final_byte,
        bool *passthrough_out);
    boxrig_status (*dcs_put)(void *context, uint8_t byte);
    boxrig_status (*dcs_unhook)(
        void *context,
        bool cancelled,
        boxrig_parser_string_terminator terminator);
    boxrig_status (*string_start)(
        void *context,
        boxrig_parser_string_kind kind);
    boxrig_status (*string_put)(void *context, uint8_t byte);
    boxrig_status (*string_end)(
        void *context,
        bool cancelled,
        boxrig_parser_string_terminator terminator);
} boxrig_parser_actions;

typedef enum boxrig_parser_state {
    BOXRIG_PARSER_GROUND = 0,
    BOXRIG_PARSER_ESCAPE,
    BOXRIG_PARSER_ESCAPE_INTERMEDIATE,
    BOXRIG_PARSER_CSI_ENTRY,
    BOXRIG_PARSER_CSI_PARAMETER,
    BOXRIG_PARSER_CSI_INTERMEDIATE,
    BOXRIG_PARSER_CSI_IGNORE,
    BOXRIG_PARSER_DCS_ENTRY,
    BOXRIG_PARSER_DCS_PARAMETER,
    BOXRIG_PARSER_DCS_INTERMEDIATE,
    BOXRIG_PARSER_DCS_IGNORE,
    BOXRIG_PARSER_DCS_IGNORE_ESCAPE,
    BOXRIG_PARSER_DCS_PASSTHROUGH,
    BOXRIG_PARSER_DCS_PASSTHROUGH_ESCAPE,
    BOXRIG_PARSER_STRING,
    BOXRIG_PARSER_STRING_ESCAPE
} boxrig_parser_state;

typedef struct boxrig_parser {
    boxrig_parser_state state;
    boxrig_parser_actions actions;
    boxrig_parser_parameter parameters[BOXRIG_PARSER_MAX_PARAMETERS];
    size_t parameter_count;
    uint8_t intermediates[BOXRIG_PARSER_MAX_INTERMEDIATES];
    size_t intermediate_count;
    uint8_t private_marker;
    boxrig_parser_string_kind string_kind;
    size_t string_capacity;
    size_t string_length;
    bool string_escape_delivered;
    bool string_discarding;
    size_t dcs_length;
    bool dcs_escape_delivered;
    bool ground_print_flushed;
    /* Updated only after successful input consumption, including ignored
     * bytes, so display CRLF pairing survives chunking and backpressure. */
    bool last_input_was_cr;
} boxrig_parser;


void boxrig_parser_initialize(
    boxrig_parser *parser,
    const boxrig_parser_actions *actions,
    size_t string_capacity);

/* Finalizes pending text and cancels incomplete syntax; safe to retry. */
boxrig_status boxrig_parser_finish(boxrig_parser *parser);

boxrig_status boxrig_parser_feed(
    boxrig_parser *parser,
    const uint8_t *bytes,
    size_t length,
    size_t *consumed_out);

#endif
