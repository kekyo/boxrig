/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "parser.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

typedef enum test_action {
    TEST_ACTION_NONE = 0,
    TEST_ACTION_PRINT,
    TEST_ACTION_FLUSH_PRINT,
    TEST_ACTION_EXECUTE,
    TEST_ACTION_ESCAPE,
    TEST_ACTION_CSI,
    TEST_ACTION_STRING_START,
    TEST_ACTION_STRING_PUT,
    TEST_ACTION_STRING_END,
    TEST_ACTION_DCS_HOOK,
    TEST_ACTION_DCS_PUT,
    TEST_ACTION_DCS_UNHOOK
} test_action;

typedef struct action_log {
    test_action blocked_action;
    bool blocked;
    char bytes[1024];
    size_t length;
    uint64_t hash;
    size_t action_count;
    uint8_t accepted_dcs_final;
    size_t dcs_hook_count;
    size_t dcs_put_count;
    size_t dcs_unhook_count;
    size_t dcs_cancel_count;
    size_t print_count;
} action_log;

static void log_byte(action_log *log, uint8_t byte)
{
    if (log->length < sizeof(log->bytes)) {
        log->bytes[log->length] = (char)byte;
        log->length++;
    }
    log->hash ^= byte;
    log->hash *= UINT64_C(1099511628211);
    log->action_count++;
}

static boxrig_status block_once(action_log *log, test_action action)
{
    if (log->blocked_action == action && !log->blocked) {
        log->blocked = true;
        return BOXRIG_STATUS_WOULD_BLOCK;
    }
    return BOXRIG_STATUS_OK;
}

static boxrig_status record_print(void *context, uint8_t byte)
{
    action_log *log = context;
    boxrig_status status = block_once(log, TEST_ACTION_PRINT);

    if (status == BOXRIG_STATUS_OK) {
        log_byte(log, 'p');
        log_byte(log, byte);
        log->print_count++;
    }
    return status;
}

static boxrig_status record_flush_print(void *context)
{
    action_log *log = context;
    boxrig_status status = block_once(log, TEST_ACTION_FLUSH_PRINT);

    if (status == BOXRIG_STATUS_OK) {
        log_byte(log, 'f');
    }
    return status;
}

static boxrig_status record_execute(void *context, uint8_t control)
{
    action_log *log = context;
    boxrig_status status = block_once(log, TEST_ACTION_EXECUTE);

    if (status == BOXRIG_STATUS_OK) {
        log_byte(log, 'x');
        log_byte(log, control);
    }
    return status;
}

static boxrig_status record_escape(
    void *context,
    const uint8_t *intermediates,
    size_t intermediate_count,
    uint8_t final_byte)
{
    action_log *log = context;
    boxrig_status status = block_once(log, TEST_ACTION_ESCAPE);
    size_t index;

    if (status == BOXRIG_STATUS_OK) {
        log_byte(log, 'e');
        for (index = 0; index < intermediate_count; index++) {
            log_byte(log, intermediates[index]);
        }
        log_byte(log, final_byte);
    }
    return status;
}

static boxrig_status record_csi(
    void *context,
    uint8_t private_marker,
    const boxrig_parser_parameter *parameters,
    size_t parameter_count,
    const uint8_t *intermediates,
    size_t intermediate_count,
    uint8_t final_byte)
{
    action_log *log = context;
    boxrig_status status = block_once(log, TEST_ACTION_CSI);
    size_t index;

    if (status == BOXRIG_STATUS_OK) {
        log_byte(log, 'c');
        log_byte(log, private_marker);
        for (index = 0; index < parameter_count; index++) {
            log_byte(log, parameters[index].has_value ? 1 : 0);
            log_byte(log, parameters[index].subparameter ? 1 : 0);
            log_byte(log, (uint8_t)parameters[index].value);
        }
        for (index = 0; index < intermediate_count; index++) {
            log_byte(log, intermediates[index]);
        }
        log_byte(log, final_byte);
    }
    return status;
}

static boxrig_status record_string_start(
    void *context,
    boxrig_parser_string_kind kind)
{
    action_log *log = context;
    boxrig_status status = block_once(log, TEST_ACTION_STRING_START);

    if (status == BOXRIG_STATUS_OK) {
        log_byte(log, '[');
        log_byte(log, (uint8_t)kind);
    }
    return status;
}

static boxrig_status record_string_put(void *context, uint8_t byte)
{
    action_log *log = context;
    boxrig_status status = block_once(log, TEST_ACTION_STRING_PUT);

    if (status == BOXRIG_STATUS_OK) {
        log_byte(log, byte);
    }
    return status;
}

static boxrig_status record_string_end(
    void *context,
    bool cancelled,
    boxrig_parser_string_terminator terminator)
{
    action_log *log = context;
    boxrig_status status = block_once(log, TEST_ACTION_STRING_END);

    if (status == BOXRIG_STATUS_OK) {
        log_byte(log, cancelled ? '!' : ']');
        log_byte(log, (uint8_t)terminator);
    }
    return status;
}

static boxrig_status record_dcs_hook(
    void *context,
    uint8_t private_marker,
    const boxrig_parser_parameter *parameters,
    size_t parameter_count,
    const uint8_t *intermediates,
    size_t intermediate_count,
    uint8_t final_byte,
    bool *passthrough_out)
{
    action_log *log = context;
    boxrig_status status = block_once(log, TEST_ACTION_DCS_HOOK);
    size_t index;

    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    log_byte(log, 'h');
    log_byte(log, private_marker);
    log_byte(log, (uint8_t)parameter_count);
    for (index = 0; index < parameter_count; index++) {
        uint32_t value = parameters[index].value;

        log_byte(log, parameters[index].has_value ? 1 : 0);
        log_byte(log, parameters[index].subparameter ? 1 : 0);
        log_byte(log, (uint8_t)(value >> 24));
        log_byte(log, (uint8_t)(value >> 16));
        log_byte(log, (uint8_t)(value >> 8));
        log_byte(log, (uint8_t)value);
    }
    log_byte(log, (uint8_t)intermediate_count);
    for (index = 0; index < intermediate_count; index++) {
        log_byte(log, intermediates[index]);
    }
    log_byte(log, final_byte);
    log->dcs_hook_count++;
    *passthrough_out = final_byte == log->accepted_dcs_final;
    return BOXRIG_STATUS_OK;
}

static boxrig_status record_dcs_put(void *context, uint8_t byte)
{
    action_log *log = context;
    boxrig_status status = block_once(log, TEST_ACTION_DCS_PUT);

    if (status == BOXRIG_STATUS_OK) {
        log_byte(log, 'd');
        log_byte(log, byte);
        log->dcs_put_count++;
    }
    return status;
}

static boxrig_status record_dcs_unhook(
    void *context,
    bool cancelled,
    boxrig_parser_string_terminator terminator)
{
    action_log *log = context;
    boxrig_status status = block_once(log, TEST_ACTION_DCS_UNHOOK);

    if (status == BOXRIG_STATUS_OK) {
        log_byte(log, 'u');
        log_byte(log, cancelled ? 1 : 0);
        log_byte(log, (uint8_t)terminator);
        log->dcs_unhook_count++;
        if (cancelled) {
            log->dcs_cancel_count++;
        }
    }
    return status;
}

static boxrig_parser_actions actions_for(action_log *log)
{
    boxrig_parser_actions actions;

    actions.context = log;
    actions.print = record_print;
    actions.flush_print = record_flush_print;
    actions.execute = record_execute;
    actions.dispatch_escape = record_escape;
    actions.dispatch_csi = record_csi;
    actions.string_start = record_string_start;
    actions.string_put = record_string_put;
    actions.string_end = record_string_end;
    actions.dcs_hook = record_dcs_hook;
    actions.dcs_put = record_dcs_put;
    actions.dcs_unhook = record_dcs_unhook;
    return actions;
}

static bool logs_equal(const action_log *left, const action_log *right)
{
    return left->length == right->length &&
           memcmp(left->bytes, right->bytes, left->length) == 0;
}

static bool test_dcs_selector_and_lifecycle(void)
{
    static const uint8_t input[] =
        "A\x1bP?1;2:3 $qX\x07Y\x1b\\Z";
    action_log expected;
    boxrig_parser parser;
    boxrig_parser_actions actions;
    size_t consumed = 0;
    size_t split;

    memset(&expected, 0, sizeof(expected));
    expected.hash = UINT64_C(1469598103934665603);
    expected.accepted_dcs_final = (uint8_t)'q';
    actions = actions_for(&expected);
    boxrig_parser_initialize(&parser, &actions, 64);
    CHECK(boxrig_parser_feed(&parser, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(parser.state == BOXRIG_PARSER_GROUND);
    CHECK(expected.dcs_hook_count == 1);
    CHECK(expected.dcs_put_count == 3);
    CHECK(expected.dcs_unhook_count == 1);

    for (split = 0; split <= sizeof(input) - 1; split++) {
        action_log actual;
        size_t first_consumed = 0;
        size_t second_consumed = 0;

        memset(&actual, 0, sizeof(actual));
        actual.hash = UINT64_C(1469598103934665603);
        actual.accepted_dcs_final = (uint8_t)'q';
        actions = actions_for(&actual);
        boxrig_parser_initialize(&parser, &actions, 64);
        CHECK(boxrig_parser_feed(&parser, input, split, &first_consumed) ==
              BOXRIG_STATUS_OK);
        CHECK(first_consumed == split);
        CHECK(boxrig_parser_feed(
                  &parser,
                  input + split,
                  sizeof(input) - 1 - split,
                  &second_consumed) == BOXRIG_STATUS_OK);
        CHECK(second_consumed == sizeof(input) - 1 - split);
        CHECK(parser.state == BOXRIG_PARSER_GROUND);
        CHECK(logs_equal(&actual, &expected));
    }
    return true;
}

static bool test_dcs_cancel_and_sub_are_unhooked(void)
{
    static const uint8_t input[] =
        "\x1bP$qone\x18"
        "A\x1bP$qtwo\x1a"
        "B";
    action_log log;
    boxrig_parser parser;
    boxrig_parser_actions actions;
    size_t consumed = 0;

    memset(&log, 0, sizeof(log));
    log.hash = UINT64_C(1469598103934665603);
    log.accepted_dcs_final = (uint8_t)'q';
    actions = actions_for(&log);
    boxrig_parser_initialize(&parser, &actions, 64);
    CHECK(boxrig_parser_feed(&parser, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(parser.state == BOXRIG_PARSER_GROUND);
    CHECK(log.dcs_hook_count == 2);
    CHECK(log.dcs_put_count == 6);
    CHECK(log.dcs_unhook_count == 2);
    CHECK(log.dcs_cancel_count == 2);
    CHECK(log.print_count == 2);
    return true;
}

static bool test_dcs_unsupported_and_malformed_recover(void)
{
    static const uint8_t input[] =
        "\x1bP$zignored\x1b\\"
        "A\x1bP1$2qdiscarded\x1b\\B"
        "\x1bP42949672960qdiscarded\x1b\\C";
    action_log log;
    boxrig_parser parser;
    boxrig_parser_actions actions;
    size_t consumed = 0;

    memset(&log, 0, sizeof(log));
    log.hash = UINT64_C(1469598103934665603);
    log.accepted_dcs_final = (uint8_t)'q';
    actions = actions_for(&log);
    boxrig_parser_initialize(&parser, &actions, 128);
    CHECK(boxrig_parser_feed(&parser, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(parser.state == BOXRIG_PARSER_GROUND);
    CHECK(log.dcs_hook_count == 1);
    CHECK(log.dcs_put_count == 0);
    CHECK(log.dcs_unhook_count == 0);
    CHECK(log.print_count == 3);
    return true;
}

static bool test_would_block_retries_every_action(void)
{
    static const uint8_t input[] =
        "A\x07\x1b" "7\x1b[1;2H\x1b]x\x07\x1bP$qz\x1b\\";
    action_log expected;
    test_action blocked_action;
    boxrig_parser parser;
    boxrig_parser_actions actions;
    size_t consumed = 0;

    memset(&expected, 0, sizeof(expected));
    expected.hash = UINT64_C(1469598103934665603);
    expected.accepted_dcs_final = (uint8_t)'q';
    actions = actions_for(&expected);
    boxrig_parser_initialize(&parser, &actions, 64);
    CHECK(boxrig_parser_feed(&parser, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);

    for (blocked_action = TEST_ACTION_PRINT;
         blocked_action <= TEST_ACTION_DCS_UNHOOK;
         blocked_action++) {
        action_log actual;
        size_t offset = 0;

        memset(&actual, 0, sizeof(actual));
        actual.blocked_action = blocked_action;
        actual.hash = UINT64_C(1469598103934665603);
        actual.accepted_dcs_final = (uint8_t)'q';
        actions = actions_for(&actual);
        boxrig_parser_initialize(&parser, &actions, 64);
        while (offset < sizeof(input) - 1) {
            boxrig_status status;

            consumed = 0;
            status = boxrig_parser_feed(
                &parser,
                input + offset,
                sizeof(input) - 1 - offset,
                &consumed);
            offset += consumed;
            CHECK(status == BOXRIG_STATUS_OK ||
                  status == BOXRIG_STATUS_WOULD_BLOCK);
            CHECK(status != BOXRIG_STATUS_WOULD_BLOCK || consumed == 0 ||
                  offset < sizeof(input) - 1);
        }
        CHECK(actual.blocked);
        CHECK(actual.length == expected.length);
        CHECK(memcmp(actual.bytes, expected.bytes, expected.length) == 0);
    }
    return true;
}

static bool test_dcs_limit_aborts_once(void)
{
    static const uint8_t input[] = "\x1bP$qAB\x1b\\Z";
    action_log boundary;
    action_log overflow;
    action_log blocked_overflow;
    action_log selector_overflow;
    boxrig_parser parser;
    boxrig_parser_actions actions;
    size_t consumed = 0;
    size_t offset = 0;

    memset(&boundary, 0, sizeof(boundary));
    boundary.hash = UINT64_C(1469598103934665603);
    boundary.accepted_dcs_final = (uint8_t)'q';
    actions = actions_for(&boundary);
    boxrig_parser_initialize(&parser, &actions, 4);
    CHECK(boxrig_parser_feed(&parser, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(boundary.dcs_hook_count == 1);
    CHECK(boundary.dcs_put_count == 2);
    CHECK(boundary.dcs_unhook_count == 1);
    CHECK(boundary.dcs_cancel_count == 0);
    CHECK(boundary.print_count == 1);

    memset(&overflow, 0, sizeof(overflow));
    overflow.hash = UINT64_C(1469598103934665603);
    overflow.accepted_dcs_final = (uint8_t)'q';
    actions = actions_for(&overflow);
    boxrig_parser_initialize(&parser, &actions, 3);
    CHECK(boxrig_parser_feed(&parser, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(parser.state == BOXRIG_PARSER_GROUND);
    CHECK(overflow.dcs_hook_count == 1);
    CHECK(overflow.dcs_put_count == 1);
    CHECK(overflow.dcs_unhook_count == 1);
    CHECK(overflow.dcs_cancel_count == 1);
    CHECK(overflow.print_count == 1);

    memset(&blocked_overflow, 0, sizeof(blocked_overflow));
    blocked_overflow.blocked_action = TEST_ACTION_DCS_UNHOOK;
    blocked_overflow.hash = UINT64_C(1469598103934665603);
    blocked_overflow.accepted_dcs_final = (uint8_t)'q';
    actions = actions_for(&blocked_overflow);
    boxrig_parser_initialize(&parser, &actions, 3);
    while (offset < sizeof(input) - 1) {
        boxrig_status status;

        consumed = 0;
        status = boxrig_parser_feed(
            &parser,
            input + offset,
            sizeof(input) - 1 - offset,
            &consumed);
        offset += consumed;
        CHECK(status == BOXRIG_STATUS_OK ||
              status == BOXRIG_STATUS_WOULD_BLOCK);
    }
    CHECK(blocked_overflow.blocked);
    CHECK(blocked_overflow.dcs_put_count == 1);
    CHECK(blocked_overflow.dcs_unhook_count == 1);
    CHECK(blocked_overflow.dcs_cancel_count == 1);
    CHECK(blocked_overflow.print_count == 1);

    memset(&selector_overflow, 0, sizeof(selector_overflow));
    selector_overflow.hash = UINT64_C(1469598103934665603);
    selector_overflow.accepted_dcs_final = (uint8_t)'q';
    actions = actions_for(&selector_overflow);
    boxrig_parser_initialize(&parser, &actions, 1);
    CHECK(boxrig_parser_feed(&parser, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(selector_overflow.dcs_hook_count == 0);
    CHECK(selector_overflow.dcs_put_count == 0);
    CHECK(selector_overflow.dcs_unhook_count == 0);
    CHECK(selector_overflow.print_count == 1);
    return true;
}

static bool test_control_string_limit_is_cancelled(void)
{
    static const uint8_t input[] = "\x1b]abc\x1b\\Z";
    static const uint8_t repeated_escape_input[] =
        "\x1b]ab\x1b\x1b\\Z";
    action_log log;
    boxrig_parser parser;
    boxrig_parser_actions actions;
    size_t consumed = 0;

    memset(&log, 0, sizeof(log));
    log.hash = UINT64_C(1469598103934665603);
    actions = actions_for(&log);
    boxrig_parser_initialize(&parser, &actions, 2);
    CHECK(boxrig_parser_feed(&parser, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(log.length == 9);
    CHECK(log.bytes[0] == 'f');
    CHECK(log.bytes[1] == '[');
    CHECK(log.bytes[2] == BOXRIG_PARSER_STRING_OSC);
    CHECK(log.bytes[3] == 'a');
    CHECK(log.bytes[4] == 'b');
    CHECK(log.bytes[5] == '!');
    CHECK(log.bytes[6] == BOXRIG_PARSER_STRING_TERMINATOR_CANCEL);
    CHECK(log.bytes[7] == 'p');
    CHECK(log.bytes[8] == 'Z');

    memset(&log, 0, sizeof(log));
    log.hash = UINT64_C(1469598103934665603);
    actions = actions_for(&log);
    boxrig_parser_initialize(&parser, &actions, 2);
    consumed = 0;
    CHECK(boxrig_parser_feed(
              &parser,
              repeated_escape_input,
              sizeof(repeated_escape_input) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(repeated_escape_input) - 1);
    CHECK(parser.state == BOXRIG_PARSER_GROUND);
    CHECK(log.length == 9);
    CHECK(log.bytes[0] == 'f');
    CHECK(log.bytes[1] == '[');
    CHECK(log.bytes[2] == BOXRIG_PARSER_STRING_OSC);
    CHECK(log.bytes[3] == 'a');
    CHECK(log.bytes[4] == 'b');
    CHECK(log.bytes[5] == '!');
    CHECK(log.bytes[6] == BOXRIG_PARSER_STRING_TERMINATOR_CANCEL);
    CHECK(log.bytes[7] == 'p');
    CHECK(log.bytes[8] == 'Z');
    return true;
}

static bool test_control_string_terminators_are_reported(void)
{
    static const uint8_t input[] =
        "\x1b]a\x07"
        "\x1b]b\x1b\\"
        "\x1b]c\x18";
    action_log log;
    boxrig_parser parser;
    boxrig_parser_actions actions;
    size_t consumed = 0;
    size_t index = 0;

    memset(&log, 0, sizeof(log));
    log.hash = UINT64_C(1469598103934665603);
    actions = actions_for(&log);
    boxrig_parser_initialize(&parser, &actions, 64);
    CHECK(boxrig_parser_feed(&parser, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);

    while (index < log.length && log.bytes[index] != ']') {
        index++;
    }
    CHECK(index + 1 < log.length);
    CHECK(log.bytes[index + 1] == BOXRIG_PARSER_STRING_TERMINATOR_BEL);
    index += 2;
    while (index < log.length && log.bytes[index] != ']') {
        index++;
    }
    CHECK(index + 1 < log.length);
    CHECK(log.bytes[index + 1] == BOXRIG_PARSER_STRING_TERMINATOR_ST);
    index += 2;
    while (index < log.length && log.bytes[index] != '!') {
        index++;
    }
    CHECK(index + 1 < log.length);
    CHECK(log.bytes[index + 1] == BOXRIG_PARSER_STRING_TERMINATOR_CANCEL);
    return true;
}

static bool test_every_split_matches_one_shot(void)
{
    static const uint8_t input[] =
        "A\x1b[?25l\x1b[2:3;4mB\x1b(0\x18"
        "C\x1b]xy\x1b\\D\x1bPz\x1a"
        "E\x1b[42949672960mF\x1b[1;2";
    action_log expected;
    boxrig_parser parser;
    boxrig_parser_actions actions;
    size_t consumed = 0;
    size_t split;

    memset(&expected, 0, sizeof(expected));
    expected.hash = UINT64_C(1469598103934665603);
    actions = actions_for(&expected);
    boxrig_parser_initialize(&parser, &actions, 8);
    CHECK(boxrig_parser_feed(&parser, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);

    for (split = 0; split <= sizeof(input) - 1; split++) {
        action_log actual;
        size_t first_consumed = 0;
        size_t second_consumed = 0;

        memset(&actual, 0, sizeof(actual));
        actual.hash = UINT64_C(1469598103934665603);
        actions = actions_for(&actual);
        boxrig_parser_initialize(&parser, &actions, 8);
        CHECK(boxrig_parser_feed(&parser, input, split, &first_consumed) ==
              BOXRIG_STATUS_OK);
        CHECK(first_consumed == split);
        CHECK(boxrig_parser_feed(
                  &parser,
                  input + split,
                  sizeof(input) - 1 - split,
                  &second_consumed) == BOXRIG_STATUS_OK);
        CHECK(second_consumed == sizeof(input) - 1 - split);
        CHECK(actual.length == expected.length);
        CHECK(memcmp(actual.bytes, expected.bytes, expected.length) == 0);
    }
    return true;
}

static uint32_t next_random(uint32_t *state)
{
    uint32_t value = *state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static bool test_bounded_fragmentation_fuzz(void)
{
    uint8_t input[256];
    uint32_t random_state = UINT32_C(0x6d2b79f5);
    size_t iteration;

    for (iteration = 0; iteration < 512; iteration++) {
        action_log whole;
        action_log fragmented;
        boxrig_parser whole_parser;
        boxrig_parser fragmented_parser;
        boxrig_parser_actions whole_actions;
        boxrig_parser_actions fragmented_actions;
        size_t length = next_random(&random_state) % (sizeof(input) + 1);
        size_t index;
        size_t consumed = 0;

        for (index = 0; index < length; index++) {
            input[index] = (uint8_t)next_random(&random_state);
        }
        memset(&whole, 0, sizeof(whole));
        memset(&fragmented, 0, sizeof(fragmented));
        whole.hash = UINT64_C(1469598103934665603);
        fragmented.hash = UINT64_C(1469598103934665603);
        whole_actions = actions_for(&whole);
        fragmented_actions = actions_for(&fragmented);
        boxrig_parser_initialize(&whole_parser, &whole_actions, 32);
        boxrig_parser_initialize(&fragmented_parser, &fragmented_actions, 32);
        CHECK(boxrig_parser_feed(&whole_parser, input, length, &consumed) ==
              BOXRIG_STATUS_OK);
        CHECK(consumed == length);
        index = 0;
        while (index < length) {
            size_t chunk = next_random(&random_state) % 17 + 1;

            if (chunk > length - index) {
                chunk = length - index;
            }
            consumed = 0;
            CHECK(boxrig_parser_feed(
                      &fragmented_parser,
                      input + index,
                      chunk,
                      &consumed) == BOXRIG_STATUS_OK);
            CHECK(consumed == chunk);
            index += chunk;
        }
        CHECK(fragmented.hash == whole.hash);
        CHECK(fragmented.action_count == whole.action_count);
        CHECK(fragmented_parser.state == whole_parser.state);
        CHECK(fragmented_parser.parameter_count == whole_parser.parameter_count);
        CHECK(fragmented_parser.intermediate_count ==
              whole_parser.intermediate_count);
        CHECK(fragmented_parser.string_length == whole_parser.string_length);
        CHECK(fragmented_parser.string_discarding ==
              whole_parser.string_discarding);
    }
    return true;
}

static bool test_bounded_dcs_fragmentation_fuzz(void)
{
    static const uint8_t payload_alphabet[] = {
        'A', 'B', 'C', '0', '9', ';', 0x07
    };
    uint8_t input[80];
    uint32_t random_state = UINT32_C(0x1f123bb5);
    size_t iteration;

    for (iteration = 0; iteration < 512; iteration++) {
        action_log whole;
        action_log fragmented;
        boxrig_parser whole_parser;
        boxrig_parser fragmented_parser;
        boxrig_parser_actions whole_actions;
        boxrig_parser_actions fragmented_actions;
        size_t payload_length = next_random(&random_state) % 65;
        size_t length = payload_length + 7;
        size_t index;
        size_t consumed = 0;

        input[0] = 0x1b;
        input[1] = (uint8_t)'P';
        input[2] = (uint8_t)'$';
        input[3] = (uint8_t)'q';
        for (index = 0; index < payload_length; index++) {
            input[4 + index] = payload_alphabet[
                next_random(&random_state) % sizeof(payload_alphabet)];
        }
        input[4 + payload_length] = 0x1b;
        input[5 + payload_length] = (uint8_t)'\\';
        input[6 + payload_length] = (uint8_t)'Z';

        memset(&whole, 0, sizeof(whole));
        memset(&fragmented, 0, sizeof(fragmented));
        whole.hash = UINT64_C(1469598103934665603);
        fragmented.hash = UINT64_C(1469598103934665603);
        whole.accepted_dcs_final = (uint8_t)'q';
        fragmented.accepted_dcs_final = (uint8_t)'q';
        whole_actions = actions_for(&whole);
        fragmented_actions = actions_for(&fragmented);
        boxrig_parser_initialize(&whole_parser, &whole_actions, 16);
        boxrig_parser_initialize(&fragmented_parser, &fragmented_actions, 16);
        CHECK(boxrig_parser_feed(&whole_parser, input, length, &consumed) ==
              BOXRIG_STATUS_OK);
        CHECK(consumed == length);

        index = 0;
        while (index < length) {
            size_t chunk = next_random(&random_state) % 7 + 1;

            if (chunk > length - index) {
                chunk = length - index;
            }
            consumed = 0;
            CHECK(boxrig_parser_feed(
                      &fragmented_parser,
                      input + index,
                      chunk,
                      &consumed) == BOXRIG_STATUS_OK);
            CHECK(consumed == chunk);
            index += chunk;
        }
        CHECK(fragmented.hash == whole.hash);
        CHECK(fragmented.action_count == whole.action_count);
        CHECK(fragmented_parser.state == whole_parser.state);
        CHECK(fragmented.dcs_put_count == whole.dcs_put_count);
        CHECK(fragmented.dcs_unhook_count == whole.dcs_unhook_count);
    }
    return true;
}

int main(void)
{
    static const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"DCS selector and lifecycle", test_dcs_selector_and_lifecycle},
        {"DCS CAN and SUB unhook active commands",
         test_dcs_cancel_and_sub_are_unhooked},
        {"unsupported and malformed DCS recover",
         test_dcs_unsupported_and_malformed_recover},
        {"WOULD_BLOCK retries every action",
         test_would_block_retries_every_action},
        {"DCS limit aborts once", test_dcs_limit_aborts_once},
        {"control string limit is cancelled",
         test_control_string_limit_is_cancelled},
        {"control string terminators are reported",
         test_control_string_terminators_are_reported},
        {"every split matches one shot", test_every_split_matches_one_shot},
        {"bounded fragmentation fuzz", test_bounded_fragmentation_fuzz},
        {"bounded DCS fragmentation fuzz",
         test_bounded_dcs_fragmentation_fuzz},
    };
    size_t index;

    for (index = 0; index < sizeof(tests) / sizeof(tests[0]); index++) {
        if (!tests[index].run()) {
            fprintf(stderr, "Test failed: %s\n", tests[index].name);
            return 1;
        }
        printf("PASS %s\n", tests[index].name);
    }
    return 0;
}
