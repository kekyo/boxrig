/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "terminal_internal.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

typedef enum consumer_block {
    CONSUMER_BLOCK_NONE = 0,
    CONSUMER_BLOCK_BEGIN,
    CONSUMER_BLOCK_PUT,
    CONSUMER_BLOCK_COMMIT
} consumer_block;

typedef struct consumer_log {
    boxrig_kitty_graphics_command command;
    consumer_block block;
    bool blocked;
    size_t begin_attempts;
    size_t put_attempts;
    size_t commit_attempts;
    size_t begin_count;
    size_t put_count;
    size_t commit_count;
    size_t abort_count;
    uint64_t digest;
} consumer_log;

static boxrig_status consumer_begin(
    void *context,
    boxrig_kitty_graphics_command command)
{
    consumer_log *log = context;

    log->begin_attempts++;
    if (log->block == CONSUMER_BLOCK_BEGIN && !log->blocked) {
        log->blocked = true;
        return BOXRIG_STATUS_WOULD_BLOCK;
    }
    log->command = command;
    log->begin_count++;
    return BOXRIG_STATUS_OK;
}

static boxrig_status consumer_put(void *context, uint8_t byte)
{
    consumer_log *log = context;

    log->put_attempts++;
    if (log->block == CONSUMER_BLOCK_PUT && !log->blocked) {
        log->blocked = true;
        return BOXRIG_STATUS_WOULD_BLOCK;
    }
    log->digest ^= byte;
    log->digest *= UINT64_C(1099511628211);
    log->put_count++;
    return BOXRIG_STATUS_OK;
}

static boxrig_status consumer_commit(void *context)
{
    consumer_log *log = context;

    log->commit_attempts++;
    if (log->block == CONSUMER_BLOCK_COMMIT && !log->blocked) {
        log->blocked = true;
        return BOXRIG_STATUS_WOULD_BLOCK;
    }
    log->commit_count++;
    return BOXRIG_STATUS_OK;
}

static void consumer_abort(void *context)
{
    consumer_log *log = context;

    log->abort_count++;
}

static boxrig_kitty_graphics_consumer make_consumer(consumer_log *log)
{
    boxrig_kitty_graphics_consumer consumer;

    consumer.context = log;
    consumer.begin = consumer_begin;
    consumer.put = consumer_put;
    consumer.commit = consumer_commit;
    consumer.abort = consumer_abort;
    return consumer;
}

static bool parse_header(
    const uint8_t *bytes,
    size_t length,
    boxrig_kitty_graphics_command *command_out)
{
    boxrig_kitty_graphics_header state;
    size_t index;

    boxrig_kitty_graphics_header_initialize(&state);
    for (index = 0; index < length; index++) {
        if (!boxrig_kitty_graphics_header_put(&state, bytes[index])) {
            return false;
        }
    }
    return boxrig_kitty_graphics_header_finish(&state, command_out);
}

static size_t count_present(const boxrig_kitty_graphics_command *command)
{
    size_t count = 0;
    boxrig_kitty_graphics_field field;

    for (field = BOXRIG_KITTY_GRAPHICS_FIELD_ACTION;
         field < BOXRIG_KITTY_GRAPHICS_FIELD_COUNT;
         field++) {
        if (boxrig_kitty_graphics_command_has(command, field)) {
            count++;
        }
    }
    return count;
}

static bool test_empty_and_delimiterless_headers(void)
{
    static const uint8_t delimiterless[] = "a=t";
    boxrig_kitty_graphics_command command;

    CHECK(parse_header(NULL, 0, &command));
    CHECK(count_present(&command) == 0);
    CHECK(parse_header(
        delimiterless,
        sizeof(delimiterless) - 1,
        &command));
    CHECK(count_present(&command) == 1);
    CHECK(boxrig_kitty_graphics_command_has(
        &command,
        BOXRIG_KITTY_GRAPHICS_FIELD_ACTION));
    CHECK(command.action == (uint8_t)'t');
    return true;
}

static bool test_all_known_keys_and_integer_boundaries(void)
{
    static const uint8_t header[] =
        "a=T,t=f,o=z,d=a,"
        "q=0,f=32,s=640,v=480,S=123,O=4,i=5,I=6,p=7,m=1,N=8,"
        "x=9,y=10,w=11,h=12,X=13,Y=14,c=15,r=16,C=1,U=1,"
        "P=17,Q=4294967295,z=-2147483648,H=2147483647,V=+18";
    boxrig_kitty_graphics_command command;

    CHECK(parse_header(header, sizeof(header) - 1, &command));
    CHECK(count_present(&command) == BOXRIG_KITTY_GRAPHICS_FIELD_COUNT);
    CHECK(command.action == (uint8_t)'T');
    CHECK(command.transmission == (uint8_t)'f');
    CHECK(command.compression == (uint8_t)'z');
    CHECK(command.delete_action == (uint8_t)'a');
    CHECK(command.quiet == 0);
    CHECK(command.format == 32);
    CHECK(command.data_width == 640);
    CHECK(command.data_height == 480);
    CHECK(command.data_size == 123);
    CHECK(command.data_offset == 4);
    CHECK(command.image_id == 5);
    CHECK(command.image_number == 6);
    CHECK(command.placement_id == 7);
    CHECK(command.more == 1);
    CHECK(command.usage_hints == 8);
    CHECK(command.source_x == 9);
    CHECK(command.source_y == 10);
    CHECK(command.source_width == 11);
    CHECK(command.source_height == 12);
    CHECK(command.cell_x_offset == 13);
    CHECK(command.cell_y_offset == 14);
    CHECK(command.columns == 15);
    CHECK(command.rows == 16);
    CHECK(command.cursor_movement == 1);
    CHECK(command.virtual_placement == 1);
    CHECK(command.parent_image_id == 17);
    CHECK(command.parent_placement_id == UINT32_MAX);
    CHECK(command.z_index == INT32_MIN);
    CHECK(command.relative_x_offset == INT32_MAX);
    CHECK(command.relative_y_offset == 18);
    return true;
}

static bool test_unknown_keys_and_duplicates(void)
{
    static const uint8_t header[] =
        "Z=future,a=t,x=1,Z=still-printable,a=p,x=2";
    boxrig_kitty_graphics_command command;

    CHECK(parse_header(header, sizeof(header) - 1, &command));
    CHECK(count_present(&command) == 2);
    CHECK(command.action == (uint8_t)'p');
    CHECK(command.source_x == 2);
    return true;
}

static bool test_malformed_headers_are_rejected(void)
{
    static const char *invalid[] = {
        "=1",
        "aa=1",
        "a",
        "a=",
        "a=tt",
        "q=x",
        "q=-1",
        "q=+1",
        "q=4294967296",
        "z=-",
        "z=+",
        "z=2147483648",
        "z=-2147483649",
        "a=t,",
        "a=t,,q=1",
        "Z=",
        "Z=bad\x1f"
    };
    boxrig_kitty_graphics_command command;
    size_t index;

    for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
        CHECK(!parse_header(
            (const uint8_t *)invalid[index],
            strlen(invalid[index]),
            &command));
    }
    return true;
}

static bool test_header_capacity_boundary(void)
{
    uint8_t exact[BOXRIG_KITTY_GRAPHICS_HEADER_CAPACITY];
    uint8_t overflow[BOXRIG_KITTY_GRAPHICS_HEADER_CAPACITY + 1];
    boxrig_kitty_graphics_command command;

    exact[0] = (uint8_t)'Z';
    exact[1] = (uint8_t)'=';
    memset(exact + 2, 'x', sizeof(exact) - 2);
    memcpy(overflow, exact, sizeof(exact));
    overflow[sizeof(exact)] = (uint8_t)'x';
    CHECK(parse_header(exact, sizeof(exact), &command));
    CHECK(count_present(&command) == 0);
    CHECK(!parse_header(overflow, sizeof(overflow), &command));
    return true;
}

static bool test_apc_streams_multiple_maximum_payloads(void)
{
    static const uint8_t first_prefix[] = "\x1b_Ga=t,i=1;";
    static const uint8_t second_prefix[] = "\x1b_Ga=t,i=2;";
    static const uint8_t suffix[] = "\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_kitty_graphics_consumer consumer;
    consumer_log log;
    uint8_t payload[BOXRIG_KITTY_GRAPHICS_PAYLOAD_CAPACITY];
    uint64_t expected_digest = UINT64_C(1469598103934665603);
    size_t consumed = 0;
    size_t index;

    memset(&log, 0, sizeof(log));
    log.digest = expected_digest;
    consumer = make_consumer(&log);
    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_kitty_graphics_set_consumer(&terminal->graphics, &consumer);

    memset(payload, 'A', sizeof(payload));
    CHECK(boxrig_terminal_feed(
              terminal,
              first_prefix,
              sizeof(first_prefix) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              payload,
              sizeof(payload),
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(payload));
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              suffix,
              sizeof(suffix) - 1,
              &consumed) == BOXRIG_STATUS_OK);

    memset(payload, 'B', sizeof(payload));
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              second_prefix,
              sizeof(second_prefix) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              payload,
              sizeof(payload),
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(payload));
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              suffix,
              sizeof(suffix) - 1,
              &consumed) == BOXRIG_STATUS_OK);

    for (index = 0; index < BOXRIG_KITTY_GRAPHICS_PAYLOAD_CAPACITY; index++) {
        expected_digest ^= (uint8_t)'A';
        expected_digest *= UINT64_C(1099511628211);
    }
    for (index = 0; index < BOXRIG_KITTY_GRAPHICS_PAYLOAD_CAPACITY; index++) {
        expected_digest ^= (uint8_t)'B';
        expected_digest *= UINT64_C(1099511628211);
    }
    CHECK(log.begin_count == 2);
    CHECK(log.put_count == 2 * BOXRIG_KITTY_GRAPHICS_PAYLOAD_CAPACITY);
    CHECK(log.commit_count == 2);
    CHECK(log.abort_count == 0);
    CHECK(log.digest == expected_digest);
    CHECK(boxrig_kitty_graphics_command_has(
        &log.command,
        BOXRIG_KITTY_GRAPHICS_FIELD_IMAGE_ID));
    CHECK(log.command.image_id == 2);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_apc_every_split_preserves_stream(void)
{
    static const uint8_t input[] =
        "\x1b_Ga=t,i=7;AB\x07" "C\x1b\\Z";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    uint64_t expected_digest = UINT64_C(1469598103934665603);
    size_t split;
    size_t index;

    for (index = 0; index < 4; index++) {
        static const uint8_t payload[] = {'A', 'B', 0x07, 'C'};

        expected_digest ^= payload[index];
        expected_digest *= UINT64_C(1099511628211);
    }
    config.rows = 1;
    config.columns = 1;
    for (split = 0; split <= sizeof(input) - 1; split++) {
        boxrig_terminal *terminal = NULL;
        boxrig_kitty_graphics_consumer consumer;
        consumer_log log;
        boxrig_cell_view cell;
        size_t consumed = 0;

        memset(&log, 0, sizeof(log));
        log.digest = UINT64_C(1469598103934665603);
        consumer = make_consumer(&log);
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        boxrig_kitty_graphics_set_consumer(&terminal->graphics, &consumer);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  input,
                  split,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == split);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  input + split,
                  sizeof(input) - 1 - split,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(input) - 1 - split);
        CHECK(log.begin_count == 1);
        CHECK(log.put_count == 4);
        CHECK(log.commit_count == 1);
        CHECK(log.abort_count == 0);
        CHECK(log.digest == expected_digest);
        CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) ==
              BOXRIG_STATUS_OK);
        CHECK(cell.text_length == 1 && memcmp(cell.text, "Z", 1) == 0);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_apc_consumer_retries_are_atomic(void)
{
    static const uint8_t input[] = "\x1b_Ga=t,i=3;ABC\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    consumer_block block;

    config.rows = 1;
    config.columns = 1;
    for (block = CONSUMER_BLOCK_BEGIN;
         block <= CONSUMER_BLOCK_COMMIT;
         block++) {
        boxrig_terminal *terminal = NULL;
        boxrig_kitty_graphics_consumer consumer;
        consumer_log log;
        size_t consumed = 0;
        size_t remaining;

        memset(&log, 0, sizeof(log));
        log.block = block;
        log.digest = UINT64_C(1469598103934665603);
        consumer = make_consumer(&log);
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        boxrig_kitty_graphics_set_consumer(&terminal->graphics, &consumer);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  input,
                  sizeof(input) - 1,
                  &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
        CHECK(consumed < sizeof(input) - 1);
        if (block == CONSUMER_BLOCK_BEGIN) {
            CHECK(input[consumed] == (uint8_t)';');
        } else if (block == CONSUMER_BLOCK_PUT) {
            CHECK(input[consumed] == (uint8_t)'A');
        } else {
            CHECK(input[consumed] == (uint8_t)'\\');
        }
        remaining = sizeof(input) - 1 - consumed;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  input + consumed,
                  remaining,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == remaining);
        CHECK(log.begin_count == 1);
        CHECK(log.put_count == 3);
        CHECK(log.commit_count == 1);
        CHECK(log.abort_count == 0);
        CHECK(log.begin_attempts ==
              (block == CONSUMER_BLOCK_BEGIN ? 2u : 1u));
        CHECK(log.put_attempts ==
              (block == CONSUMER_BLOCK_PUT ? 4u : 3u));
        CHECK(log.commit_attempts ==
              (block == CONSUMER_BLOCK_COMMIT ? 2u : 1u));
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_apc_cancel_and_limits_abort_once(void)
{
    static const uint8_t cancelled[] =
        "\x1b_Ga=t;abc\x18Z"
        "\x1b_Ga=t;de\x1aY";
    static const uint8_t overflow_prefix[] = "\x1b_Ga=t;";
    static const uint8_t suffix[] = "\x1b\\Z";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_kitty_graphics_consumer consumer;
    consumer_log log;
    uint8_t payload[BOXRIG_KITTY_GRAPHICS_PAYLOAD_CAPACITY + 1];
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 4;
    memset(&log, 0, sizeof(log));
    consumer = make_consumer(&log);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_kitty_graphics_set_consumer(&terminal->graphics, &consumer);
    CHECK(boxrig_terminal_feed(
              terminal,
              cancelled,
              sizeof(cancelled) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(log.begin_count == 2);
    CHECK(log.put_count == 5);
    CHECK(log.commit_count == 0);
    CHECK(log.abort_count == 2);
    boxrig_terminal_destroy(terminal);

    memset(&log, 0, sizeof(log));
    consumer = make_consumer(&log);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_kitty_graphics_set_consumer(&terminal->graphics, &consumer);
    memset(payload, 'A', sizeof(payload));
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              overflow_prefix,
              sizeof(overflow_prefix) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              payload,
              sizeof(payload),
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(payload));
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              suffix,
              sizeof(suffix) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(log.begin_count == 1);
    CHECK(log.put_count == BOXRIG_KITTY_GRAPHICS_PAYLOAD_CAPACITY);
    CHECK(log.commit_count == 0);
    CHECK(log.abort_count == 1);
    boxrig_terminal_destroy(terminal);

    config.control_string_capacity = 4;
    memset(&log, 0, sizeof(log));
    consumer = make_consumer(&log);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_kitty_graphics_set_consumer(&terminal->graphics, &consumer);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b_G;abc\x1b\\Z",
              sizeof("\x1b_G;abc\x1b\\Z") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof("\x1b_G;abc\x1b\\Z") - 1);
    CHECK(log.begin_count == 1);
    CHECK(log.put_count == 2);
    CHECK(log.commit_count == 0);
    CHECK(log.abort_count == 1);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_apc_invalid_headers_never_begin(void)
{
    static const uint8_t invalid[] =
        "\x1b_Gaa=1;payload\x1b\\"
        "\x1b_Xa=t;payload\x1b\\Z";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_kitty_graphics_consumer consumer;
    consumer_log log;
    uint8_t header[BOXRIG_KITTY_GRAPHICS_HEADER_CAPACITY + 1];
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 2;
    memset(&log, 0, sizeof(log));
    consumer = make_consumer(&log);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_kitty_graphics_set_consumer(&terminal->graphics, &consumer);
    CHECK(boxrig_terminal_feed(
              terminal,
              invalid,
              sizeof(invalid) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(log.begin_count == 0);
    CHECK(log.put_count == 0);
    CHECK(log.commit_count == 0);
    CHECK(log.abort_count == 0);

    header[0] = (uint8_t)'Z';
    header[1] = (uint8_t)'=';
    memset(header + 2, 'x', sizeof(header) - 2);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b_G",
              3,
              &consumed) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              header,
              sizeof(header),
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(header));
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)";payload\x1b\\",
              10,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(log.begin_count == 0);
    CHECK(log.abort_count == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_apc_empty_and_delimiterless_commands(void)
{
    static const uint8_t input[] =
        "\x1b_G;\x1b\\"
        "\x1b_Ga=p\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_kitty_graphics_consumer consumer;
    consumer_log log;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 1;
    memset(&log, 0, sizeof(log));
    consumer = make_consumer(&log);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_kitty_graphics_set_consumer(&terminal->graphics, &consumer);
    CHECK(boxrig_terminal_feed(
              terminal,
              input,
              sizeof(input) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(log.begin_count == 2);
    CHECK(log.put_count == 0);
    CHECK(log.commit_count == 2);
    CHECK(log.abort_count == 0);
    CHECK(log.command.action == (uint8_t)'p');
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_apc_ris_and_destroy_reset_transactions(void)
{
    static const uint8_t embedded_ris[] =
        "\x1b_G;A\x1b" "cB\x18"
        "\x1b" "c"
        "\x1b_G;X\x1b\\";
    static const uint8_t unfinished[] = "\x1b_G;abc";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_kitty_graphics_consumer consumer;
    consumer_log log;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 1;
    memset(&log, 0, sizeof(log));
    consumer = make_consumer(&log);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_kitty_graphics_set_consumer(&terminal->graphics, &consumer);
    CHECK(boxrig_terminal_feed(
              terminal,
              embedded_ris,
              sizeof(embedded_ris) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(embedded_ris) - 1);
    CHECK(log.begin_count == 2);
    CHECK(log.put_count == 5);
    CHECK(log.commit_count == 1);
    CHECK(log.abort_count == 1);
    boxrig_terminal_destroy(terminal);
    CHECK(log.abort_count == 1);

    memset(&log, 0, sizeof(log));
    consumer = make_consumer(&log);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_kitty_graphics_set_consumer(&terminal->graphics, &consumer);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              unfinished,
              sizeof(unfinished) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(unfinished) - 1);
    CHECK(log.begin_count == 1);
    CHECK(log.put_count == 3);
    CHECK(log.commit_count == 0);
    CHECK(log.abort_count == 0);
    boxrig_terminal_destroy(terminal);
    CHECK(log.abort_count == 1);
    return true;
}

static bool test_graphics_command_profile_semantics_and_quiet_responses(void)
{
    struct fixture {
        const char *input;
        const char *expected;
    } fixtures[] = {
        {
            "\x1b_Ga=q,i=31,s=1,v=1,t=d,f=24;AAAA\x1b\\",
            "\x1b_Gi=31;OK\x1b\\"
        },
        {
            "\x1b_Gi=32,s=1,v=1;AAAAAA==\x1b\\",
            "\x1b_Gi=32;OK\x1b\\"
        },
        {
            "\x1b_Ga=p,i=33,p=7\x1b\\",
            "\x1b_Gi=33,p=7;ENOENT:image not found\x1b\\"
        },
        {
            "\x1b_Ga=f,i=34,f=24,s=1,v=1;AAAA\x1b\\",
            "\x1b_Gi=34;ENOENT:image not found\x1b\\"
        },
        {
            "\x1b_Ga=q,i=35,t=f,f=24,s=1,v=1,S=3,O=2;L3RtcC94\x1b\\",
            "\x1b_Gi=35;ENOTSUP:unsupported transmission\x1b\\"
        },
        {
            "\x1b_Ga=q,i=36,I=2,f=24,s=1,v=1;AAAA\x1b\\",
            "\x1b_Gi=36,I=2;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=q,i=37,q=3,f=24,s=1,v=1;AAAA\x1b\\",
            "\x1b_Gi=37;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=p,i=38;AAAA\x1b\\",
            "\x1b_Gi=38;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=q,i=39,f=24,s=0,v=1;AAAA\x1b\\",
            "\x1b_Gi=39;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=q,i=40,q=1,f=24,s=1,v=1;AAAA\x1b\\",
            ""
        },
        {
            "\x1b_Ga=q,i=41,q=1,f=24,s=0,v=1;AAAA\x1b\\",
            "\x1b_Gi=41;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=q,i=42,q=2,f=24,s=0,v=1;AAAA\x1b\\",
            ""
        },
        {
            "\x1b_Ga=q,i=43,t=s,f=24;L2JveHJpZw==\x1b\\",
            "\x1b_Gi=43;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=q,i=44,t=t,m=1,f=24,s=1,v=1;L3RtcC94\x1b\\",
            "\x1b_Gi=44;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=f,f=24,s=1,v=1;AAAA\x1b\\",
            ""
        },
        {
            "\x1b_Ga=f,i=45,f=24,s=1,v=1,r=1,c=2;AAAA\x1b\\",
            "\x1b_Gi=45;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=f,i=46,f=24,s=1,v=1,X=2;AAAA\x1b\\",
            "\x1b_Gi=46;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=a,i=47,s=3,v=2\x1b\\",
            "\x1b_Gi=47;ENOENT:image not found\x1b\\"
        },
        {
            "\x1b_Ga=a,i=48\x1b\\",
            "\x1b_Gi=48;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=a,i=49,r=2,z=0\x1b\\",
            "\x1b_Gi=49;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=a,i=50,c=1;QQ==\x1b\\",
            "\x1b_Gi=50;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=c,i=51,r=1,c=2,w=1,h=1,C=1\x1b\\",
            "\x1b_Gi=51;ENOENT:image not found\x1b\\"
        },
        {
            "\x1b_Ga=c,i=52,r=1,w=1,h=1\x1b\\",
            "\x1b_Gi=52;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=c,i=53,r=1,c=2,C=2\x1b\\",
            "\x1b_Gi=53;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=c,i=54,r=1,c=2,q=2\x1b\\",
            ""
        }
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    size_t index;

    config.rows = 1;
    config.columns = 1;
    config.graphics_transport_name_capacity = 0;
    for (index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]); index++) {
        boxrig_terminal *terminal = NULL;
        const uint8_t *output = NULL;
        size_t consumed = 0;
        size_t output_length;

        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)fixtures[index].input,
                  strlen(fixtures[index].input),
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == strlen(fixtures[index].input));
        output_length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(output_length == strlen(fixtures[index].expected));
        CHECK(output_length == 0 ||
              memcmp(output, fixtures[index].expected, output_length) == 0);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_static_delete_selector_semantics(void)
{
    struct fixture {
        const char *input;
        const char *expected;
    } fixtures[] = {
        {"\x1b_Ga=d,d=a,i=51\x1b\\", "\x1b_Gi=51;OK\x1b\\"},
        {"\x1b_Ga=d,d=A,i=52\x1b\\", "\x1b_Gi=52;OK\x1b\\"},
        {
            "\x1b_Ga=d,d=i,i=53\x1b\\",
            "\x1b_Gi=53;ENOENT:image not found\x1b\\"
        },
        {
            "\x1b_Ga=d,d=I,i=54\x1b\\",
            "\x1b_Gi=54;ENOENT:image not found\x1b\\"
        },
        {
            "\x1b_Ga=d,d=n,I=55\x1b\\",
            "\x1b_Gi=0,I=55;ENOENT:image not found\x1b\\"
        },
        {
            "\x1b_Ga=d,d=R,i=56,x=1,y=10\x1b\\",
            "\x1b_Gi=56;ENOENT:image not found\x1b\\"
        },
        {"\x1b_Ga=d,d=c,i=57\x1b\\", "\x1b_Gi=57;OK\x1b\\"},
        {"\x1b_Ga=d,d=p,i=58,x=1,y=1\x1b\\", "\x1b_Gi=58;OK\x1b\\"},
        {
            "\x1b_Ga=d,d=q,i=59,x=1,y=1,z=-2\x1b\\",
            "\x1b_Gi=59;OK\x1b\\"
        },
        {"\x1b_Ga=d,d=x,i=60,x=1\x1b\\", "\x1b_Gi=60;OK\x1b\\"},
        {"\x1b_Ga=d,d=y,i=61,y=1\x1b\\", "\x1b_Gi=61;OK\x1b\\"},
        {"\x1b_Ga=d,d=z,i=62,z=0\x1b\\", "\x1b_Gi=62;OK\x1b\\"},
        {
            "\x1b_Ga=d,d=f,i=63\x1b\\",
            "\x1b_Gi=63;ENOENT:image not found\x1b\\"
        },
        {
            "\x1b_Ga=d,d=F,i=63,r=2\x1b\\",
            "\x1b_Gi=63;ENOENT:image not found\x1b\\"
        },
        {
            "\x1b_Ga=d,d=f,i=63,p=1\x1b\\",
            "\x1b_Gi=63,p=1;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=d,d=f,i=63,x=1\x1b\\",
            "\x1b_Gi=63;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=d,d=i,i=63,r=2\x1b\\",
            "\x1b_Gi=63;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=d,d=p,i=64,x=0,y=1\x1b\\",
            "\x1b_Gi=64;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=d,d=q,i=65,x=1,y=1\x1b\\",
            "\x1b_Gi=65;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=d,d=b,i=66\x1b\\",
            "\x1b_Gi=66;EINVAL:invalid command\x1b\\"
        }
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    size_t index;

    config.rows = 1;
    config.columns = 1;
    for (index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]); index++) {
        boxrig_terminal *terminal = NULL;
        const uint8_t *output = NULL;
        size_t consumed = 0;
        size_t output_length;

        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)fixtures[index].input,
                  strlen(fixtures[index].input),
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == strlen(fixtures[index].input));
        output_length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(output_length == strlen(fixtures[index].expected));
        CHECK(memcmp(output, fixtures[index].expected, output_length) == 0);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_static_query_response_order_and_retry(void)
{
    static const uint8_t input[] =
        "\x1b_Ga=q,i=71,f=24,s=1,v=1;AAAA\x1b\\\x1b[c";
    static const uint8_t expected[] =
        "\x1b_Gi=71;OK\x1b\\\x1b[?62;6;22c";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    const uint8_t *output = NULL;
    uint8_t fill[64];
    size_t consumed = 0;
    size_t remaining;
    size_t output_length;

    config.rows = 1;
    config.columns = 1;
    config.output_capacity = sizeof(fill);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    memset(fill, 'x', sizeof(fill));
    CHECK(boxrig_terminal_send_text(
              terminal,
              (const char *)fill,
              sizeof(fill)) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              input,
              sizeof(input) - 1,
              &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed < sizeof(input) - 1);
    CHECK(input[consumed] == (uint8_t)'\\');
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(fill)) ==
          BOXRIG_STATUS_OK);
    remaining = sizeof(input) - 1 - consumed;
    CHECK(boxrig_terminal_feed(
              terminal,
              input + consumed,
              remaining,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == remaining);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, sizeof(expected) - 1) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_static_query_preserves_existing_barrier_order(void)
{
    static const uint8_t clipboard_query[] = "\x1b]52;c;?\x07";
    static const uint8_t query_and_da[] =
        "\x1b_Ga=q,i=72,f=24,s=1,v=1;AAAA\x1b\\\x1b[c";
    static const uint8_t clipboard_response[] = "\x1b]52;c;\x07";
    static const uint8_t query_response[] =
        "\x1b_Gi=72;OK\x1b\\\x1b[?62;6;22c";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              clipboard_query,
              sizeof(clipboard_query) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_CLIPBOARD_READ);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              query_and_da,
              sizeof(query_and_da) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(query_and_da) - 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              item.request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              NULL,
              0) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(clipboard_response) - 1);
    CHECK(memcmp(output, clipboard_response, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(query_response) - 1);
    CHECK(memcmp(output, query_response, output_length) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_graphics_config_disable_and_empty_views(void)
{
    static const uint8_t query_and_da[] =
        "\x1b_Ga=q,i=81,f=24,s=1,v=1;AAAA\x1b\\\x1b[c";
    static const uint8_t da_response[] = "\x1b[?62;6;22c";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_handle handle = 0;
    boxrig_image_view image;
    boxrig_image_placement_view placement;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;

    CHECK(config.graphics_storage_capacity == 128u * 1024u * 1024u);
    CHECK(config.graphics_decode_capacity == 192u * 1024u * 1024u);
    CHECK(config.graphics_image_capacity == 4096);
    CHECK(config.graphics_placement_capacity == 8192);
    CHECK(config.graphics_transport_name_capacity == 4u * 1024u);
    CHECK(config.graphics_animation_storage_capacity ==
          640u * 1024u * 1024u);
    CHECK(config.graphics_frame_capacity == 16384);
    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == 1);
    CHECK(boxrig_terminal_image_count(terminal) == 0);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_get_image(terminal, 1, &image) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_destroy(terminal);

    config = boxrig_terminal_config_default();
    config.rows = 1;
    config.columns = 1;
    config.graphics_storage_capacity = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    config.graphics_storage_capacity = 128u * 1024u * 1024u;
    config.graphics_decode_capacity = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    config.graphics_decode_capacity = 192u * 1024u * 1024u;
    config.graphics_image_capacity = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    config.graphics_image_capacity = 4096;
    config.graphics_placement_capacity = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    config.graphics_placement_capacity = 8192;
    config.graphics_image_capacity = SIZE_MAX;
    CHECK(boxrig_terminal_create(&config, &terminal) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    config.graphics_image_capacity = 4096;
    config.graphics_placement_capacity = SIZE_MAX;
    CHECK(boxrig_terminal_create(&config, &terminal) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);

    config.graphics_storage_capacity = 0;
    config.graphics_decode_capacity = 0;
    config.graphics_image_capacity = 0;
    config.graphics_placement_capacity = 0;
    config.graphics_transport_name_capacity = 0;
    config.graphics_animation_storage_capacity = 0;
    config.graphics_frame_capacity = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              query_and_da,
              sizeof(query_and_da) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(query_and_da) - 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(da_response) - 1);
    CHECK(memcmp(output, da_response, output_length) == 0);
    CHECK(boxrig_terminal_image_count(terminal) == 0);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    boxrig_terminal_destroy(terminal);

    config = boxrig_terminal_config_default();
    config.rows = 1;
    config.columns = 1;
    config.graphics_animation_storage_capacity = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    config.graphics_animation_storage_capacity = 640u * 1024u * 1024u;
    config.graphics_frame_capacity = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    config.graphics_animation_storage_capacity = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_external_transport_exposes_owned_host_requests(void)
{
    static const uint8_t file_request[] =
        "\x1b_Ga=t,i=91,t=f,f=32,s=1,v=1,S=4,O=2;L3RtcC9vd25lZA==\x1b\\";
    static const uint8_t temporary_request[] =
        "\x1b_Ga=q,i=92,t=t,f=24,s=1,v=1;L3RtcC90dHktZ3JhcGhpY3MtcHJvdG9jb2wteA==\x1b\\";
    static const uint8_t shared_request[] =
        "\x1b_Ga=q,i=93,t=s,f=24,s=1,v=1,S=8;L2JveHJpZw==\x1b\\";
    static const uint8_t denied[] =
        "\x1b_Gi=91;EACCES:external graphics data denied\x1b\\";
    static const uint8_t unavailable[] =
        "\x1b_Gi=92;EIO:external graphics data unavailable\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    const boxrig_graphics_transport_view *transport;
    const uint8_t *output = NULL;
    uint8_t input[sizeof(file_request)];
    uint64_t request_id;
    size_t consumed = 0;
    size_t output_length;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    memcpy(input, file_request, sizeof(file_request));
    CHECK(boxrig_terminal_feed(
              terminal,
              input,
              sizeof(input) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    memset(input, 'x', sizeof(input));
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_GRAPHICS_DATA);
    CHECK(item.request_id != 0);
    CHECK(item.payload == NULL);
    CHECK(item.payload_length == 0);
    CHECK(item.notification == NULL);
    CHECK(item.graphics_transport != NULL);
    transport = item.graphics_transport;
    CHECK(transport->medium == BOXRIG_GRAPHICS_TRANSPORT_FILE);
    CHECK(transport->name_length == 10);
    CHECK(memcmp(transport->name, "/tmp/owned", 10) == 0);
    CHECK(transport->offset == 2);
    CHECK(transport->size == 4);
    CHECK(transport->data_capacity == 4);
    CHECK(!transport->delete_after_read);
    request_id = item.request_id;
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_DENIED,
              NULL,
              0) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_DENIED,
              (const uint8_t *)"x",
              1) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_DENIED,
              NULL,
              0) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_DENIED,
              NULL,
              0) == BOXRIG_STATUS_INVALID_ARGUMENT);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(denied) - 1);
    CHECK(memcmp(output, denied, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              temporary_request,
              sizeof(temporary_request) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.graphics_transport->medium ==
          BOXRIG_GRAPHICS_TRANSPORT_TEMPORARY_FILE);
    CHECK(item.graphics_transport->name_length == 28);
    CHECK(memcmp(
              item.graphics_transport->name,
              "/tmp/tty-graphics-protocol-x",
              28) == 0);
    CHECK(item.graphics_transport->offset == 0);
    CHECK(item.graphics_transport->size == 0);
    CHECK(item.graphics_transport->data_capacity ==
          config.graphics_decode_capacity);
    CHECK(item.graphics_transport->delete_after_read);
    request_id = item.request_id;
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_UNAVAILABLE,
              NULL,
              0) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(unavailable) - 1);
    CHECK(memcmp(output, unavailable, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              shared_request,
              sizeof(shared_request) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.graphics_transport->medium ==
          BOXRIG_GRAPHICS_TRANSPORT_SHARED_MEMORY);
    CHECK(item.graphics_transport->name_length == 7);
    CHECK(memcmp(item.graphics_transport->name, "/boxrig", 7) == 0);
    CHECK(item.graphics_transport->data_capacity == 8);
    CHECK(!item.graphics_transport->delete_after_read);
    request_id = item.request_id;
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              (const uint8_t *)"123456789",
              9) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_UNAVAILABLE,
              NULL,
              0) == BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_external_transport_validation_retry_order_and_reset(void)
{
    static const uint8_t invalid[] =
        "\x1b_Ga=q,i=101,t=f,f=24,s=1,v=1;L3RtcC94=\x1b\\"
        "\x1b_Ga=q,i=102,t=f,f=24,s=1,v=1;L3RtcAA=\x1b\\";
    static const uint8_t invalid_responses[] =
        "\x1b_Gi=101;EINVAL:invalid command\x1b\\"
        "\x1b_Gi=102;EINVAL:invalid command\x1b\\";
    static const uint8_t request[] =
        "\x1b_Ga=q,i=103,t=f,f=24,s=1,v=1;L3RtcC94\x1b\\";
    static const uint8_t reset_pending[] =
        "\x1b_Ga=q,i=104,t=f,f=24,s=1,v=1;L3RtcC95\x1b\\";
    static const uint8_t retry_error[] =
        "\x1b_Gi=103;EIO:external graphics data unavailable\x1b\\";
    static const uint8_t reset_error[] =
        "\x1b_Gi=104;EIO:external graphics data unavailable\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    const uint8_t *output = NULL;
    uint64_t request_id;
    size_t consumed = 0;
    size_t offset;
    size_t output_length;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              invalid,
              sizeof(invalid) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_host_item_peek(terminal, &item));
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(invalid_responses) - 1);
    CHECK(memcmp(output, invalid_responses, output_length) == 0);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    consumed = 0;
    config.host_item_capacity = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\a",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              request,
              sizeof(request) - 1,
              &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed == sizeof(request) - 2);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_BELL);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    offset = consumed;
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              request + offset,
              sizeof(request) - 1 - offset,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 1);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_GRAPHICS_DATA);
    request_id = item.request_id;
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "B", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_UNAVAILABLE,
              NULL,
              0) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(retry_error) - 1);
    CHECK(memcmp(output, retry_error, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(output[0] == (uint8_t)'B');
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              reset_pending,
              sizeof(reset_pending) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    request_id = item.request_id;
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b" "c",
              2,
              &consumed) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(reset_error) - 1);
    CHECK(memcmp(output, reset_error, output_length) == 0);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              (const uint8_t *)"abc",
              3) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              NULL,
              0) == BOXRIG_STATUS_INVALID_ARGUMENT);

    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(reset_error) - 1) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              request,
              sizeof(request) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b" "c",
              2,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_host_item_peek(terminal, &item));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_external_completion_decodes_raw_png_zlib_and_places(void)
{
    static const uint8_t rgba[] = {
        255, 0, 0, 255, 0, 255, 0, 128,
        0, 0, 255, 0, 255, 255, 255, 127
    };
    static const uint8_t zlib[] = {
        0x78, 0x9c, 0xfb, 0xcf, 0xc0, 0xf0, 0x9f, 0xe1, 0x3f, 0x43, 0x03,
        0x98, 0xfa, 0xff, 0xbf, 0x1e, 0x00, 0x3b, 0xd8, 0x07, 0xf9
    };
    static const uint8_t png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00,
        0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
        0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24,
        0x00, 0x00, 0x00, 0x17, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63,
        0xf8, 0xcf, 0xc0, 0xf0, 0x9f, 0xe1, 0x3f, 0x43, 0x03, 0x03, 0x98,
        0xfe, 0xff, 0xbf, 0x1e, 0x00, 0x3f, 0x57, 0x07, 0xf9, 0x33, 0x67,
        0x48, 0x86, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
        0x42, 0x60, 0x82
    };
    struct fixture {
        const uint8_t *input;
        size_t input_length;
        const uint8_t *data;
        size_t data_length;
        uint32_t image_id;
        bool query;
        bool placement;
    } fixtures[] = {
        {
            (const uint8_t *)
                "\x1b_Ga=t,i=401,t=f,f=32,s=2,v=2,S=16;L2ZpbGU=\x1b\\",
            sizeof("\x1b_Ga=t,i=401,t=f,f=32,s=2,v=2,S=16;L2ZpbGU=\x1b\\") -
                1,
            rgba,
            sizeof(rgba),
            401,
            false,
            false
        },
        {
            (const uint8_t *)
                "\x1b_Ga=t,i=402,t=t,f=32,s=2,v=2,o=z,S=21;L3RlbXA=\x1b\\",
            sizeof("\x1b_Ga=t,i=402,t=t,f=32,s=2,v=2,o=z,S=21;L3RlbXA=\x1b\\") -
                1,
            zlib,
            sizeof(zlib),
            402,
            false,
            false
        },
        {
            (const uint8_t *)
                "\x1b_Ga=t,i=403,t=s,f=100,S=80;L3NobQ==\x1b\\",
            sizeof("\x1b_Ga=t,i=403,t=s,f=100,S=80;L3NobQ==\x1b\\") - 1,
            png,
            sizeof(png),
            403,
            false,
            false
        },
        {
            (const uint8_t *)
                "\x1b_Ga=q,i=404,t=f,f=32,s=2,v=2,S=16;L2ZpbGU=\x1b\\",
            sizeof("\x1b_Ga=q,i=404,t=f,f=32,s=2,v=2,S=16;L2ZpbGU=\x1b\\") -
                1,
            rgba,
            sizeof(rgba),
            404,
            true,
            false
        },
        {
            (const uint8_t *)
                "\x1b_Ga=T,i=405,t=f,f=32,s=1,v=1,S=4;L2ZpbGU=\x1b\\",
            sizeof("\x1b_Ga=T,i=405,t=f,f=32,s=1,v=1,S=4;L2ZpbGU=\x1b\\") -
                1,
            rgba,
            4,
            405,
            false,
            true
        }
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    size_t index;

    config.rows = 2;
    config.columns = 2;
    for (index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]); index++) {
        boxrig_terminal *terminal = NULL;
        boxrig_host_item item;
        boxrig_image_handle handle = 0;
        boxrig_image_view image;
        const uint8_t *output = NULL;
        char expected[80];
        size_t consumed = 0;
        size_t output_length;
        int expected_length;

        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  fixtures[index].input,
                  fixtures[index].input_length,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == fixtures[index].input_length);
        CHECK(boxrig_terminal_host_item_peek(terminal, &item));
        CHECK(item.type == BOXRIG_HOST_ITEM_GRAPHICS_DATA);
        CHECK(item.graphics_transport->data_capacity ==
              fixtures[index].data_length);
        CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_complete_host_request(
                  terminal,
                  item.request_id,
                  BOXRIG_HOST_REQUEST_SUCCESS,
                  fixtures[index].data,
                  fixtures[index].data_length) == BOXRIG_STATUS_OK);
        expected_length = snprintf(
            expected,
            sizeof(expected),
            "\x1b_Gi=%lu;OK\x1b\\",
            (unsigned long)fixtures[index].image_id);
        CHECK(expected_length > 0 && (size_t)expected_length < sizeof(expected));
        output_length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(output_length == (size_t)expected_length);
        CHECK(memcmp(output, expected, output_length) == 0);
        if (fixtures[index].query) {
            CHECK(boxrig_terminal_image_count(terminal) == 0);
        } else {
            CHECK(boxrig_terminal_image_count(terminal) == 1);
            CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
                  BOXRIG_STATUS_OK);
            CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
                  BOXRIG_STATUS_OK);
            CHECK(image.image_id == fixtures[index].image_id);
            CHECK(image.rgba_length ==
                  (fixtures[index].placement ? 4 : sizeof(rgba)));
            CHECK(memcmp(
                      image.rgba,
                      rgba,
                      image.rgba_length) == 0);
        }
        CHECK(boxrig_terminal_image_placement_count(terminal) ==
              (fixtures[index].placement ? 1 : 0));
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_external_completion_errors_and_recovery_are_atomic(void)
{
    struct fixture {
        const char *input;
        const uint8_t *data;
        size_t data_length;
        const char *expected;
    } fixtures[] = {
        {
            "\x1b_Ga=t,i=411,t=f,f=32,s=1,v=1,S=4;L2ZpbGU=\x1b\\",
            (const uint8_t *)"abc",
            3,
            "\x1b_Gi=411;EIO:external graphics data unavailable\x1b\\"
        },
        {
            "\x1b_Ga=t,i=412,t=f,f=32,s=1,v=1,S=3;L2ZpbGU=\x1b\\",
            (const uint8_t *)"abc",
            3,
            "\x1b_Gi=412;EINVAL:invalid command\x1b\\"
        },
        {
            "\x1b_Ga=t,i=413,t=f,f=32,s=1,v=1,o=z,S=4;L2ZpbGU=\x1b\\",
            (const uint8_t *)"nope",
            4,
            "\x1b_Gi=413;EBADZLIB:invalid stream\x1b\\"
        },
        {
            "\x1b_Ga=t,i=414,t=f,f=100,S=4;L2ZpbGU=\x1b\\",
            (const uint8_t *)"nope",
            4,
            "\x1b_Gi=414;EBADPNG:invalid image\x1b\\"
        }
    };
    static const uint8_t valid_request[] =
        "\x1b_Ga=t,i=415,t=f,f=32,s=1,v=1,S=4;L2ZpbGU=\x1b\\";
    static const uint8_t valid_response[] = "\x1b_Gi=415;OK\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    boxrig_image_handle handle = 0;
    boxrig_image_view image;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t index;
    size_t output_length;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]); index++) {
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)fixtures[index].input,
                  strlen(fixtures[index].input),
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_host_item_peek(terminal, &item));
        CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_complete_host_request(
                  terminal,
                  item.request_id,
                  BOXRIG_HOST_REQUEST_SUCCESS,
                  fixtures[index].data,
                  fixtures[index].data_length) == BOXRIG_STATUS_OK);
        output_length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(output_length == strlen(fixtures[index].expected));
        CHECK(memcmp(output, fixtures[index].expected, output_length) == 0);
        CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_image_count(terminal) == 0);
    }
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              valid_request,
              sizeof(valid_request) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              item.request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              (const uint8_t *)"\x01\x02\x03\x04",
              4) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(valid_response) - 1);
    CHECK(memcmp(output, valid_response, output_length) == 0);
    CHECK(boxrig_terminal_image_count(terminal) == 1);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(image.rgba_length == 4);
    CHECK(memcmp(image.rgba, "\x01\x02\x03\x04", 4) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_external_completions_preserve_identity_and_input_order(void)
{
    static const uint8_t requests[] =
        "\x1b_Ga=t,i=421,t=f,f=32,s=1,v=1,S=4;L29sZA==\x1b\\"
        "\x1b_Ga=t,i=421,t=f,f=32,s=1,v=1,S=4;L25ldw==\x1b\\"
        "\x1b_Ga=t,i=422,t=f,f=32,s=1,v=1,S=4;L2JsdWU=\x1b\\";
    static const char *ordered_responses[] = {
        "\x1b_Gi=421;OK\x1b\\",
        "\x1b_Gi=421;OK\x1b\\",
        "\x1b_Gi=422;OK\x1b\\"
    };
    static const uint8_t direct[] =
        "\x1b_Ga=t,i=423,f=32,s=1,v=1;/////w==\x1b\\";
    static const uint8_t red[] = {255, 0, 0, 255};
    static const uint8_t green[] = {0, 255, 0, 255};
    static const uint8_t blue[] = {0, 0, 255, 255};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    boxrig_image_handle handle;
    boxrig_image_view image;
    const uint8_t *output = NULL;
    uint64_t request_ids[3];
    bool found_421 = false;
    bool found_422 = false;
    bool found_423 = false;
    size_t consumed = 0;
    size_t index;
    size_t output_length;

    config.rows = 1;
    config.columns = 1;
    config.graphics_storage_capacity = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              requests,
              sizeof(requests) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    for (index = 0; index < 3; index++) {
        CHECK(boxrig_terminal_host_item_peek(terminal, &item));
        request_ids[index] = item.request_id;
        CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    }
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_ids[2],
              BOXRIG_HOST_REQUEST_SUCCESS,
              blue,
              sizeof(blue)) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_ids[1],
              BOXRIG_HOST_REQUEST_SUCCESS,
              green,
              sizeof(green)) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_ids[0],
              BOXRIG_HOST_REQUEST_SUCCESS,
              red,
              sizeof(red)) == BOXRIG_STATUS_OK);
    for (index = 0; index < 3; index++) {
        output_length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(output_length == strlen(ordered_responses[index]));
        CHECK(memcmp(output, ordered_responses[index], output_length) == 0);
        CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
              BOXRIG_STATUS_OK);
    }
    CHECK(boxrig_terminal_image_count(terminal) == 2);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              direct,
              sizeof(direct) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_count(terminal) == 2);
    for (index = 0; index < boxrig_terminal_image_count(terminal); index++) {
        CHECK(boxrig_terminal_image_handle_at(terminal, index, &handle) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
              BOXRIG_STATUS_OK);
        if (image.image_id == 421) {
            found_421 = true;
            CHECK(memcmp(image.rgba, green, sizeof(green)) == 0);
        } else if (image.image_id == 422) {
            found_422 = true;
            CHECK(memcmp(image.rgba, blue, sizeof(blue)) == 0);
        } else if (image.image_id == 423) {
            found_423 = true;
        }
    }
    CHECK(!found_421);
    CHECK(found_422);
    CHECK(found_423);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_frame_transfer_add_edit_and_chunks(void)
{
    static const uint8_t root[] =
        "\x1b_Ga=T,i=501,f=32,s=2,v=2,c=2,r=2,C=1;"
        "AAD//wAA//8AAP//AAD//w==\x1b\\";
    static const uint8_t add_from_root[] =
        "\x1b_Ga=f,i=501,f=32,s=1,v=1,x=1,c=1,X=1,z=75;"
        "AP8A/w==\x1b\\";
    static const uint8_t add_from_color[] =
        "\x1b_Ga=f,i=501,f=32,s=1,v=1,y=1,Y=4278190335,X=1,N=1;"
        "AP8A/w==\x1b\\";
    static const uint8_t edit_second[] =
        "\x1b_Ga=f,i=501,f=32,s=1,v=1,r=2,x=0,y=1,z=-1;"
        "/wAAgA==\x1b\\";
    static const uint8_t edit_root[] =
        "\x1b_Ga=f,i=501,f=32,s=1,v=1,r=1;/wAAgA==\x1b\\";
    static const uint8_t chunk_first[] =
        "\x1b_Ga=f,i=501,f=32,s=1,v=1,r=1,x=1,y=1,X=1,m=1;AP8A\x1b\\";
    static const uint8_t chunk_last[] =
        "\x1b_Ga=f,m=0;/w==\x1b\\";
    static const uint8_t zlib_edit[] =
        "\x1b_Ga=f,i=501,f=32,s=1,v=1,r=1,x=1,X=1,o=z;"
        "eJxj+M/wHwAEAAH/\x1b\\";
    static const uint8_t png_frame[] =
        "\x1b_Ga=f,i=501,f=100,s=77,v=88,c=1;"
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAF0lEQVR4nGP4z8Dw"
        "n+E/QwMDmP7/vx4AP1cH+TNnSIYAAAAASUVORK5CYII=\x1b\\";
    static const uint8_t invalid_rectangle[] =
        "\x1b_Ga=f,i=501,f=32,s=1,v=1,r=1,x=2,X=1;AP8A/w==\x1b\\";
    static const uint8_t ok[] = "\x1b_Gi=501;OK\x1b\\";
    static const uint8_t invalid[] =
        "\x1b_Gi=501;EINVAL:invalid command\x1b\\";
    static const uint8_t expected[] = {
        128, 0, 127, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 0, 255, 0, 255
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_handle handle = 0;
    boxrig_image_view image;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    uint64_t revision;
    size_t consumed = 0;
    size_t output_length;

    config.rows = 2;
    config.columns = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              root,
              sizeof(root) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    revision = boxrig_terminal_image_revision(terminal);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              add_from_root,
              sizeof(add_from_root) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(add_from_root) - 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(ok) - 1);
    CHECK(memcmp(output, ok, sizeof(ok) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              add_from_color,
              sizeof(add_from_color) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              edit_second,
              sizeof(edit_second) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              edit_root,
              sizeof(edit_root) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) != revision);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              chunk_first,
              sizeof(chunk_first) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(chunk_first) - 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              chunk_last,
              sizeof(chunk_last) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(chunk_last) - 1);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              zlib_edit,
              sizeof(zlib_edit) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              png_frame,
              sizeof(png_frame) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(image.rgba_length == sizeof(expected));
    CHECK(memcmp(image.rgba, expected, sizeof(expected)) == 0);

    revision = boxrig_terminal_image_revision(terminal);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              invalid_rectangle,
              sizeof(invalid_rectangle) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(invalid) - 1);
    CHECK(memcmp(output, invalid, output_length) == 0);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, expected, sizeof(expected)) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_external_frame_transfer_uses_the_same_transaction(void)
{
    static const uint8_t root[] =
        "\x1b_Ga=t,i=502,f=32,s=1,v=1;AAAA/w==\x1b\\";
    static const uint8_t edit[] =
        "\x1b_Ga=f,i=502,t=f,f=32,s=1,v=1,r=1,X=1,S=4;"
        "L2ZyYW1l\x1b\\";
    static const uint8_t add[] =
        "\x1b_Ga=f,i=502,t=f,f=32,s=1,v=1,c=1,S=4;"
        "L2ZyYW1l\x1b\\";
    static const uint8_t ok[] = "\x1b_Gi=502;OK\x1b\\";
    static const uint8_t replacement[] = {7, 8, 9, 255};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    boxrig_image_handle handle = 0;
    boxrig_image_view image;
    const uint8_t *output = NULL;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              root,
              sizeof(root) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_OK);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              edit,
              sizeof(edit) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_GRAPHICS_DATA);
    CHECK(item.graphics_transport->data_capacity == sizeof(replacement));
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              item.request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              replacement,
              sizeof(replacement)) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(ok) - 1);
    CHECK(memcmp(output, ok, sizeof(ok) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, replacement, sizeof(replacement)) == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              add,
              sizeof(add) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              item.request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              replacement,
              sizeof(replacement)) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(ok) - 1);
    CHECK(memcmp(output, ok, sizeof(ok) - 1) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_external_frame_completion_keeps_the_newest_success(void)
{
    static const uint8_t root[] =
        "\x1b_Ga=t,i=503,f=32,s=1,v=1;AAAA/w==\x1b\\";
    static const uint8_t requests[] =
        "\x1b_Ga=f,i=503,t=f,f=32,s=1,v=1,r=1,X=1,S=4;"
        "L29sZA==\x1b\\"
        "\x1b_Ga=f,i=503,t=f,f=32,s=1,v=1,r=1,X=1,S=4;"
        "L25ldw==\x1b\\";
    static const uint8_t response[] = "\x1b_Gi=503;OK\x1b\\";
    static const uint8_t red[] = {255, 0, 0, 255};
    static const uint8_t green[] = {0, 255, 0, 255};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    boxrig_image_handle handle = 0;
    boxrig_image_view image;
    const uint8_t *output = NULL;
    uint64_t request_ids[2];
    size_t consumed = 0;
    size_t index;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              root,
              sizeof(root) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(response) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              requests,
              sizeof(requests) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    for (index = 0; index < 2; index++) {
        CHECK(boxrig_terminal_host_item_peek(terminal, &item));
        request_ids[index] = item.request_id;
        CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    }
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_ids[1],
              BOXRIG_HOST_REQUEST_SUCCESS,
              green,
              sizeof(green)) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, green, sizeof(green)) == 0);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_ids[0],
              BOXRIG_HOST_REQUEST_SUCCESS,
              red,
              sizeof(red)) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, green, sizeof(green)) == 0);
    for (index = 0; index < 2; index++) {
        CHECK(boxrig_terminal_output_peek(terminal, &output) ==
              sizeof(response) - 1);
        CHECK(memcmp(output, response, sizeof(response) - 1) == 0);
        CHECK(boxrig_terminal_output_consume(
                  terminal,
                  sizeof(response) - 1) == BOXRIG_STATUS_OK);
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_frame_quota_eviction_disable_and_retry_are_atomic(void)
{
    static const uint8_t roots[] =
        "\x1b_Ga=t,i=511,f=32,s=1,v=1;AQID/w==\x1b\\"
        "\x1b_Ga=t,i=512,f=32,s=1,v=1;BAUG/w==\x1b\\";
    static const uint8_t roots_response[] =
        "\x1b_Gi=511;OK\x1b\\\x1b_Gi=512;OK\x1b\\";
    static const uint8_t add_first[] =
        "\x1b_Ga=f,i=511,N=1,f=32,s=1,v=1;BwgJ/w==\x1b\\";
    static const uint8_t add_second[] =
        "\x1b_Ga=f,i=512,f=32,s=1,v=1;CgsM/w==\x1b\\";
    static const uint8_t edit_evicted[] =
        "\x1b_Ga=f,i=511,f=32,s=1,v=1,r=2;DQ4P/w==\x1b\\";
    static const uint8_t edit_retained[] =
        "\x1b_Ga=f,i=512,f=32,s=1,v=1,r=2;EBES/w==\x1b\\";
    static const uint8_t over_quota[] =
        "\x1b_Ga=f,i=512,f=32,s=1,v=1;ExQV/w==\x1b\\";
    static const uint8_t edit_root[] =
        "\x1b_Ga=f,i=512,f=32,s=1,v=1,r=1,X=1;FhcY/w==\x1b\\";
    static const uint8_t ok_511[] = "\x1b_Gi=511;OK\x1b\\";
    static const uint8_t ok_512[] = "\x1b_Gi=512;OK\x1b\\";
    static const uint8_t missing_511[] =
        "\x1b_Gi=511;ENOENT:image not found\x1b\\";
    static const uint8_t no_space_512[] =
        "\x1b_Gi=512;ENOSPC:graphics capacity exceeded\x1b\\";
    static const uint8_t unsupported_512[] =
        "\x1b_Gi=512;ENOTSUP:animation frames unsupported\x1b\\";
    static const uint8_t replacement[] = {22, 23, 24, 255};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_handle handle = 0;
    boxrig_image_view image;
    const uint8_t *output = NULL;
    uint8_t fill[sizeof(ok_512) - 1];
    uint64_t revision;
    size_t consumed = 0;
    size_t remaining;

    config.rows = 1;
    config.columns = 1;
    config.graphics_animation_storage_capacity = 4;
    config.graphics_frame_capacity = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              roots,
              sizeof(roots) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(roots_response) - 1);
    CHECK(memcmp(output, roots_response, sizeof(roots_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(roots_response) - 1) == BOXRIG_STATUS_OK);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              add_first,
              sizeof(add_first) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok_511) - 1) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              add_second,
              sizeof(add_second) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok_512) - 1) ==
          BOXRIG_STATUS_OK);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              edit_evicted,
              sizeof(edit_evicted) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(missing_511) - 1);
    CHECK(memcmp(output, missing_511, sizeof(missing_511) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(missing_511) - 1) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              edit_retained,
              sizeof(edit_retained) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok_512) - 1) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              over_quota,
              sizeof(over_quota) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(no_space_512) - 1);
    CHECK(memcmp(output, no_space_512, sizeof(no_space_512) - 1) == 0);
    boxrig_terminal_destroy(terminal);

    config = boxrig_terminal_config_default();
    config.rows = 1;
    config.columns = 1;
    config.graphics_animation_storage_capacity = 0;
    config.graphics_frame_capacity = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)
                  "\x1b_Ga=t,i=512,f=32,s=1,v=1;BAUG/w==\x1b\\",
              sizeof("\x1b_Ga=t,i=512,f=32,s=1,v=1;BAUG/w==\x1b\\") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok_512) - 1) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              add_second,
              sizeof(add_second) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(unsupported_512) - 1);
    CHECK(memcmp(output, unsupported_512, sizeof(unsupported_512) - 1) == 0);
    boxrig_terminal_destroy(terminal);

    config = boxrig_terminal_config_default();
    config.rows = 1;
    config.columns = 1;
    config.output_capacity = sizeof(fill);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)
                  "\x1b_Ga=t,i=512,f=32,s=1,v=1;BAUG/w==\x1b\\",
              sizeof("\x1b_Ga=t,i=512,f=32,s=1,v=1;BAUG/w==\x1b\\") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok_512) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    revision = boxrig_terminal_image_revision(terminal);
    memset(fill, 'x', sizeof(fill));
    CHECK(boxrig_terminal_send_text(
              terminal,
              (const char *)fill,
              sizeof(fill)) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              edit_root,
              sizeof(edit_root) - 1,
              &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed < sizeof(edit_root) - 1);
    CHECK(edit_root[consumed] == (uint8_t)'\\');
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, "\x04\x05\x06\xff", 4) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(fill)) ==
          BOXRIG_STATUS_OK);
    remaining = sizeof(edit_root) - 1 - consumed;
    CHECK(boxrig_terminal_feed(
              terminal,
              edit_root + consumed,
              remaining,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == remaining);
    CHECK(boxrig_terminal_image_revision(terminal) != revision);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, replacement, sizeof(replacement)) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_animation_control_selects_frames_and_validates_controls(void)
{
    static const uint8_t root[] =
        "\x1b_Ga=T,i=521,f=32,s=2,v=2,c=2,r=2,C=1;"
        "/wAA/wD/AP8AAP///////w==\x1b\\";
    static const uint8_t add_second[] =
        "\x1b_Ga=f,i=521,f=32,s=1,v=1,x=1,c=1,X=1,z=70;"
        "//8A/w==\x1b\\";
    static const uint8_t add_third[] =
        "\x1b_Ga=f,i=521,f=32,s=1,v=1,y=1,Y=16909311,X=1,z=-1;"
        "AP///w==\x1b\\";
    static const uint8_t select_second[] =
        "\x1b_Ga=a,i=521,s=2,v=3,c=2,r=2,z=25\x1b\\";
    static const uint8_t select_third[] =
        "\x1b_Ga=a,i=521,s=3,v=1,c=3\x1b\\";
    static const uint8_t select_root[] =
        "\x1b_Ga=a,i=521,s=1,c=1\x1b\\";
    static const uint8_t select_same[] =
        "\x1b_Ga=a,i=521,c=1,v=0\x1b\\";
    static const uint8_t missing_current[] =
        "\x1b_Ga=a,i=521,c=4\x1b\\";
    static const uint8_t missing_gap[] =
        "\x1b_Ga=a,i=521,r=4,z=10\x1b\\";
    static const uint8_t ok[] = "\x1b_Gi=521;OK\x1b\\";
    static const uint8_t missing[] =
        "\x1b_Gi=521;ENOENT:image not found\x1b\\";
    static const uint8_t unsupported[] =
        "\x1b_Gi=521;ENOTSUP:animation frames unsupported\x1b\\";
    static const uint8_t expected_root[] = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 255
    };
    static const uint8_t expected_second[] = {
        255, 0, 0, 255, 255, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 255
    };
    static const uint8_t expected_third[] = {
        1, 2, 3, 255, 1, 2, 3, 255,
        0, 255, 255, 255, 1, 2, 3, 255
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_handle handle = 0;
    boxrig_image_view image;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    uint64_t revision;
    size_t consumed = 0;

    config.rows = 2;
    config.columns = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              root,
              sizeof(root) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              add_second,
              sizeof(add_second) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              add_third,
              sizeof(add_third) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    revision = boxrig_terminal_image_revision(terminal);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              select_second,
              sizeof(select_second) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(ok) - 1);
    CHECK(memcmp(output, ok, sizeof(ok) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) != revision);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, expected_second, sizeof(expected_second)) == 0);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              select_third,
              sizeof(select_third) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, expected_third, sizeof(expected_third)) == 0);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              select_root,
              sizeof(select_root) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, expected_root, sizeof(expected_root)) == 0);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    revision = boxrig_terminal_image_revision(terminal);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              select_same,
              sizeof(select_same) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              missing_current,
              sizeof(missing_current) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(missing) - 1);
    CHECK(memcmp(output, missing, sizeof(missing) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(missing) - 1) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              missing_gap,
              sizeof(missing_gap) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(missing) - 1);
    CHECK(memcmp(output, missing, sizeof(missing) - 1) == 0);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    boxrig_terminal_destroy(terminal);

    config = boxrig_terminal_config_default();
    config.rows = 2;
    config.columns = 2;
    config.graphics_animation_storage_capacity = 0;
    config.graphics_frame_capacity = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)
                  "\x1b_Ga=t,i=521,f=32,s=2,v=2;"
                  "/wAA/wD/AP8AAP///////w==\x1b\\",
              sizeof("\x1b_Ga=t,i=521,f=32,s=2,v=2;"
                     "/wAA/wD/AP8AAP///////w==\x1b\\") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b_Ga=a,i=521,c=1\x1b\\",
              sizeof("\x1b_Ga=a,i=521,c=1\x1b\\") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(unsupported) - 1);
    CHECK(memcmp(output, unsupported, sizeof(unsupported) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(unsupported) - 1) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b_Ga=c,i=521,r=1,c=1,C=1\x1b\\",
              sizeof("\x1b_Ga=c,i=521,r=1,c=1,C=1\x1b\\") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(unsupported) - 1);
    CHECK(memcmp(output, unsupported, sizeof(unsupported) - 1) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_frame_composition_coordinates_alpha_and_overlap(void)
{
    static const uint8_t root[] =
        "\x1b_Ga=T,i=522,f=32,s=2,v=2,c=2,r=2,C=1;"
        "/wAA/wD/AP8AAP///////w==\x1b\\";
    static const uint8_t add_destination[] =
        "\x1b_Ga=f,i=522,f=32,s=1,v=1,Y=255,X=1;AAAA/w==\x1b\\";
    static const uint8_t add_source[] =
        "\x1b_Ga=f,i=522,f=32,s=1,v=1,Y=0,X=1;/wAAgA==\x1b\\";
    static const uint8_t replace[] =
        "\x1b_Ga=c,i=522,r=1,c=2,x=1,y=0,X=0,Y=1,w=1,h=1,C=1"
        "\x1b\\";
    static const uint8_t select_destination[] =
        "\x1b_Ga=a,i=522,c=2\x1b\\";
    static const uint8_t alpha[] =
        "\x1b_Ga=c,i=522,r=3,c=2,x=0,y=0,X=1,Y=1,w=1,h=1"
        "\x1b\\";
    static const uint8_t overlap[] =
        "\x1b_Ga=c,i=522,r=2,c=2,x=0,y=0,X=0,Y=0,w=1,h=1,C=1"
        "\x1b\\";
    static const uint8_t nonoverlap[] =
        "\x1b_Ga=c,i=522,r=2,c=2,x=0,y=1,X=1,Y=0,w=1,h=1,C=1"
        "\x1b\\";
    static const uint8_t out_of_bounds[] =
        "\x1b_Ga=c,i=522,r=1,c=2,x=2,y=0,X=0,Y=0,w=1,h=1,C=1"
        "\x1b\\";
    static const uint8_t missing_frame[] =
        "\x1b_Ga=c,i=522,r=4,c=2,C=1\x1b\\";
    static const uint8_t ok[] = "\x1b_Gi=522;OK\x1b\\";
    static const uint8_t invalid[] =
        "\x1b_Gi=522;EINVAL:invalid command\x1b\\";
    static const uint8_t missing[] =
        "\x1b_Gi=522;ENOENT:image not found\x1b\\";
    static const uint8_t after_replace[] = {
        0, 0, 0, 255, 0, 0, 0, 255,
        0, 255, 0, 255, 0, 0, 0, 255
    };
    static const uint8_t after_alpha[] = {
        0, 0, 0, 255, 0, 0, 0, 255,
        0, 255, 0, 255, 128, 0, 0, 255
    };
    static const uint8_t after_nonoverlap[] = {
        0, 0, 0, 255, 0, 255, 0, 255,
        0, 255, 0, 255, 128, 0, 0, 255
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_handle handle = 0;
    boxrig_image_view image;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    uint64_t revision;
    size_t consumed = 0;

    config.rows = 2;
    config.columns = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              root,
              sizeof(root) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              add_destination,
              sizeof(add_destination) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              add_source,
              sizeof(add_source) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    revision = boxrig_terminal_image_revision(terminal);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              replace,
              sizeof(replace) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(ok) - 1);
    CHECK(memcmp(output, ok, sizeof(ok) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              select_destination,
              sizeof(select_destination) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, after_replace, sizeof(after_replace)) == 0);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    revision = boxrig_terminal_image_revision(terminal);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              alpha,
              sizeof(alpha) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) != revision);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, after_alpha, sizeof(after_alpha)) == 0);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    revision = boxrig_terminal_image_revision(terminal);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              overlap,
              sizeof(overlap) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(invalid) - 1);
    CHECK(memcmp(output, invalid, sizeof(invalid) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(invalid) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, after_alpha, sizeof(after_alpha)) == 0);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              nonoverlap,
              sizeof(nonoverlap) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(
              image.rgba,
              after_nonoverlap,
              sizeof(after_nonoverlap)) == 0);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    revision = boxrig_terminal_image_revision(terminal);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              out_of_bounds,
              sizeof(out_of_bounds) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(invalid) - 1);
    CHECK(memcmp(output, invalid, sizeof(invalid) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(invalid) - 1) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              missing_frame,
              sizeof(missing_frame) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(missing) - 1);
    CHECK(memcmp(output, missing, sizeof(missing) - 1) == 0);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(
              image.rgba,
              after_nonoverlap,
              sizeof(after_nonoverlap)) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_animation_control_and_composition_retry_are_atomic(void)
{
    static const uint8_t root[] =
        "\x1b_Ga=T,i=523,f=32,s=1,v=1,c=1,r=1,C=1;/wAA/w==\x1b\\";
    static const uint8_t add_second[] =
        "\x1b_Ga=f,i=523,f=32,s=1,v=1,Y=255,X=1;AAAA/w==\x1b\\";
    static const uint8_t select_second[] =
        "\x1b_Ga=a,i=523,c=2,s=3,v=2\x1b\\";
    static const uint8_t compose[] =
        "\x1b_Ga=c,i=523,r=1,c=2,C=1\x1b\\";
    static const uint8_t ok[] = "\x1b_Gi=523;OK\x1b\\";
    static const uint8_t red[] = {255, 0, 0, 255};
    static const uint8_t black[] = {0, 0, 0, 255};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_handle handle = 0;
    boxrig_image_view image;
    boxrig_dirty dirty;
    uint8_t fill[sizeof(ok) - 1];
    uint64_t revision;
    size_t consumed = 0;
    size_t remaining;

    config.rows = 1;
    config.columns = 1;
    config.output_capacity = sizeof(fill);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              root,
              sizeof(root) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              add_second,
              sizeof(add_second) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    revision = boxrig_terminal_image_revision(terminal);
    memset(fill, 'x', sizeof(fill));
    CHECK(boxrig_terminal_send_text(
              terminal,
              (const char *)fill,
              sizeof(fill)) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              select_second,
              sizeof(select_second) - 1,
              &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed < sizeof(select_second) - 1);
    CHECK(select_second[consumed] == (uint8_t)'\\');
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, red, sizeof(red)) == 0);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(fill)) ==
          BOXRIG_STATUS_OK);
    remaining = sizeof(select_second) - 1 - consumed;
    CHECK(boxrig_terminal_feed(
              terminal,
              select_second + consumed,
              remaining,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == remaining);
    CHECK(boxrig_terminal_image_revision(terminal) != revision);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, black, sizeof(black)) == 0);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);

    revision = boxrig_terminal_image_revision(terminal);
    CHECK(boxrig_terminal_send_text(
              terminal,
              (const char *)fill,
              sizeof(fill)) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              compose,
              sizeof(compose) - 1,
              &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed < sizeof(compose) - 1);
    CHECK(compose[consumed] == (uint8_t)'\\');
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, black, sizeof(black)) == 0);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(fill)) ==
          BOXRIG_STATUS_OK);
    remaining = sizeof(compose) - 1 - consumed;
    CHECK(boxrig_terminal_feed(
              terminal,
              compose + consumed,
              remaining,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == remaining);
    CHECK(boxrig_terminal_image_revision(terminal) != revision);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, red, sizeof(red)) == 0);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_animation_monotonic_deadlines_and_rollback(void)
{
    static const uint8_t setup[] =
        "\x1b_Ga=T,i=531,f=32,s=1,v=1,c=1,r=1,C=1;/wAA/w==\x1b\\"
        "\x1b_Ga=f,i=531,f=32,s=1,v=1,X=1,z=20;AP8A/w==\x1b\\"
        "\x1b_Ga=f,i=531,f=32,s=1,v=1,X=1,z=30;AAD//w==\x1b\\"
        "\x1b_Ga=a,i=531,r=1,z=10,s=3,v=1,c=1\x1b\\";
    static const uint8_t responses[] =
        "\x1b_Gi=531;OK\x1b\\"
        "\x1b_Gi=531;OK\x1b\\"
        "\x1b_Gi=531;OK\x1b\\"
        "\x1b_Gi=531;OK\x1b\\";
    static const uint8_t red[] = {255, 0, 0, 255};
    static const uint8_t green[] = {0, 255, 0, 255};
    static const uint8_t blue[] = {0, 0, 255, 255};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_handle handle = 0;
    boxrig_image_view image;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    uint64_t deadline = UINT64_C(777);
    uint64_t revision;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              setup,
              sizeof(setup) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(setup) - 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(responses) - 1);
    CHECK(memcmp(output, responses, sizeof(responses) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(responses) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    CHECK(boxrig_terminal_update_monotonic_time(NULL, 0) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(!boxrig_terminal_next_animation_deadline(NULL, &deadline));
    CHECK(!boxrig_terminal_next_animation_deadline(terminal, NULL));
    CHECK(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == UINT64_C(777));
    revision = boxrig_terminal_image_revision(terminal);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 100) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 110);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 100) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 99) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 110);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 109) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));

    CHECK(boxrig_terminal_update_monotonic_time(terminal, 110) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision + 1);
    revision = boxrig_terminal_image_revision(terminal);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, green, sizeof(green)) == 0);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 130);

    CHECK(boxrig_terminal_update_monotonic_time(terminal, 129) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 130) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision + 1);
    revision = boxrig_terminal_image_revision(terminal);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, blue, sizeof(blue)) == 0);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 160);

    CHECK(boxrig_terminal_update_monotonic_time(terminal, 160) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision + 1);
    revision = boxrig_terminal_image_revision(terminal);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, red, sizeof(red)) == 0);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 170);

    CHECK(boxrig_terminal_update_monotonic_time(terminal, 250) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision + 1);
    revision = boxrig_terminal_image_revision(terminal);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, blue, sizeof(blue)) == 0);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 280);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 250) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_animation_finite_stop_and_loading_modes(void)
{
    static const uint8_t setup[] =
        "\x1b_Ga=T,i=532,f=32,s=1,v=1,c=1,r=1,C=1;/wAA/w==\x1b\\"
        "\x1b_Ga=f,i=532,f=32,s=1,v=1,X=1,z=10;AP8A/w==\x1b\\"
        "\x1b_Ga=a,i=532,r=1,z=10,s=3,v=2,c=1\x1b\\";
    static const uint8_t stop[] =
        "\x1b_Ga=a,i=532,s=1,c=1\x1b\\";
    static const uint8_t load[] =
        "\x1b_Ga=a,i=532,s=2,c=1\x1b\\";
    static const uint8_t add_third[] =
        "\x1b_Ga=f,i=532,f=32,s=1,v=1,X=1,z=10;AAD//w==\x1b\\";
    static const uint8_t ok[] = "\x1b_Gi=532;OK\x1b\\";
    static const uint8_t green[] = {0, 255, 0, 255};
    static const uint8_t blue[] = {0, 0, 255, 255};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_handle handle = 0;
    boxrig_image_view image;
    boxrig_dirty dirty;
    uint64_t deadline;
    uint64_t revision;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              setup,
              sizeof(setup) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              (sizeof(ok) - 1) * 3) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 0) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 10);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 10) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, green, sizeof(green)) == 0);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 20) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 30) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, green, sizeof(green)) == 0);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    revision = boxrig_terminal_image_revision(terminal);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 40) ==
          BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              stop,
              sizeof(stop) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    revision = boxrig_terminal_image_revision(terminal);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 50) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(!boxrig_terminal_next_animation_deadline(terminal, &deadline));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              load,
              sizeof(load) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 60);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 60) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, green, sizeof(green)) == 0);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 70);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 70) ==
          BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              add_third,
              sizeof(add_third) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    revision = boxrig_terminal_image_revision(terminal);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 70) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 71) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision + 1);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, blue, sizeof(blue)) == 0);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 81);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 81) ==
          BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_animation_gapless_frames_and_loading_tail(void)
{
    static const uint8_t setup[] =
        "\x1b_Ga=T,i=533,f=32,s=1,v=1,c=1,r=1,C=1;/wAA/w==\x1b\\"
        "\x1b_Ga=f,i=533,f=32,s=1,v=1,X=1,z=-1;AP8A/w==\x1b\\"
        "\x1b_Ga=f,i=533,f=32,s=1,v=1,X=1,z=10;AAD//w==\x1b\\"
        "\x1b_Ga=a,i=533,r=1,z=10,s=3,v=1,c=1\x1b\\";
    static const uint8_t make_gapless[] =
        "\x1b_Ga=a,i=533,s=1,c=1,r=1,z=-1\x1b\\"
        "\x1b_Ga=a,i=533,r=2,z=-1\x1b\\"
        "\x1b_Ga=a,i=533,r=3,z=-1\x1b\\"
        "\x1b_Ga=a,i=533,s=3,c=1\x1b\\";
    static const uint8_t loading_setup[] =
        "\x1b_Ga=T,i=534,f=32,s=1,v=1,c=1,r=1,C=1;/wAA/w==\x1b\\"
        "\x1b_Ga=f,i=534,f=32,s=1,v=1,X=1,z=-1;AP8A/w==\x1b\\"
        "\x1b_Ga=a,i=534,r=1,z=10,s=2,c=1\x1b\\";
    static const uint8_t add_loading_frame[] =
        "\x1b_Ga=f,i=534,f=32,s=1,v=1,X=1,z=10;AAD//w==\x1b\\";
    static const uint8_t ok_533[] = "\x1b_Gi=533;OK\x1b\\";
    static const uint8_t ok_534[] = "\x1b_Gi=534;OK\x1b\\";
    static const uint8_t red[] = {255, 0, 0, 255};
    static const uint8_t blue[] = {0, 0, 255, 255};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_handle first_handle = 0;
    boxrig_image_handle second_handle = 0;
    boxrig_image_view image;
    boxrig_dirty dirty;
    uint64_t deadline;
    uint64_t revision;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              setup,
              sizeof(setup) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              (sizeof(ok_533) - 1) * 4) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &first_handle) ==
          BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 100) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 110);
    revision = boxrig_terminal_image_revision(terminal);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 110) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision + 1);
    CHECK(boxrig_terminal_get_image(terminal, first_handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, blue, sizeof(blue)) == 0);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 120);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 120) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, first_handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, red, sizeof(red)) == 0);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              make_gapless,
              sizeof(make_gapless) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              (sizeof(ok_533) - 1) * 4) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 121);
    revision = boxrig_terminal_image_revision(terminal);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 121) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(!boxrig_terminal_next_animation_deadline(terminal, &deadline));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              loading_setup,
              sizeof(loading_setup) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              (sizeof(ok_534) - 1) * 3) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_handle_at(terminal, 1, &second_handle) ==
          BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 131);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 130) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 131) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, second_handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, red, sizeof(red)) == 0);
    CHECK(!boxrig_terminal_next_animation_deadline(terminal, &deadline));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              add_loading_frame,
              sizeof(add_loading_frame) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok_534) - 1) ==
          BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    revision = boxrig_terminal_image_revision(terminal);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 131) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 132) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision + 1);
    CHECK(boxrig_terminal_get_image(terminal, second_handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, blue, sizeof(blue)) == 0);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 142);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_animation_large_time_skip_and_deadline_overflow(void)
{
    static const uint8_t skip_setup[] =
        "\x1b_Ga=T,i=535,f=32,s=1,v=1,c=1,r=1,C=1;/wAA/w==\x1b\\"
        "\x1b_Ga=f,i=535,f=32,s=1,v=1,X=1,z=11;AP8A/w==\x1b\\"
        "\x1b_Ga=f,i=535,f=32,s=1,v=1,X=1,z=13;AAD//w==\x1b\\"
        "\x1b_Ga=a,i=535,r=1,z=7,s=3,v=1,c=1\x1b\\";
    static const uint8_t overflow_setup[] =
        "\x1b_Ga=T,i=536,f=32,s=1,v=1,c=1,r=1,C=1;/wAA/w==\x1b\\"
        "\x1b_Ga=f,i=536,f=32,s=1,v=1,X=1,z=2147483647;"
        "AP8A/w==\x1b\\"
        "\x1b_Ga=a,i=536,r=1,z=2147483647,s=3,v=1,c=1\x1b\\";
    static const uint8_t ok_535[] = "\x1b_Gi=535;OK\x1b\\";
    static const uint8_t ok_536[] = "\x1b_Gi=536;OK\x1b\\";
    static const uint8_t red[] = {255, 0, 0, 255};
    static const uint8_t green[] = {0, 255, 0, 255};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_handle handle = 0;
    boxrig_image_view image;
    boxrig_dirty dirty;
    uint64_t deadline;
    uint64_t revision;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              skip_setup,
              sizeof(skip_setup) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              (sizeof(ok_535) - 1) * 4) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 0) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 7);
    revision = boxrig_terminal_image_revision(terminal);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 31) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, red, sizeof(red)) == 0);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 38);
    CHECK(boxrig_terminal_update_monotonic_time(
              terminal,
              UINT64_C(1000000000)) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision + 1);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, green, sizeof(green)) == 0);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == UINT64_C(1000000002));
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    config = boxrig_terminal_config_default();
    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              overflow_setup,
              sizeof(overflow_setup) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              (sizeof(ok_536) - 1) * 3) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_update_monotonic_time(
              terminal,
              UINT64_MAX - UINT64_C(5)) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == UINT64_MAX);
    revision = boxrig_terminal_image_revision(terminal);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, UINT64_MAX) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision + 1);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, green, sizeof(green)) == 0);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == UINT64_MAX);
    revision = boxrig_terminal_image_revision(terminal);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, UINT64_MAX) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(boxrig_terminal_update_monotonic_time(
              terminal,
              UINT64_MAX - UINT64_C(1)) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_frame_delete_renumbers_and_reclaims_storage(void)
{
    static const uint8_t setup[] =
        "\x1b_Ga=T,i=541,f=32,s=1,v=1,c=1,r=1,C=1,q=2;/wAA/w==\x1b\\"
        "\x1b_Ga=f,i=541,f=32,s=1,v=1,X=1,q=2;AP8A/w==\x1b\\"
        "\x1b_Ga=f,i=541,f=32,s=1,v=1,X=1,q=2;AAD//w==\x1b\\"
        "\x1b_Ga=f,i=541,f=32,s=1,v=1,X=1,q=2;/////w==\x1b\\"
        "\x1b_Ga=a,i=541,c=2,q=2\x1b\\";
    static const struct {
        const char *command;
        uint8_t pixel[4];
        size_t extra_frames;
        bool changed;
    } steps[] = {
        {"\x1b_Ga=d,d=f,i=541,r=1\x1b\\", {0,255,0,255}, 2, false},
        {"\x1b_Ga=a,i=541,c=3\x1b\\", {255,255,255,255}, 2, true},
        {"\x1b_Ga=d,d=f,i=541,r=2\x1b\\", {255,255,255,255}, 1, false},
        {"\x1b_Ga=a,i=541,c=1\x1b\\", {0,255,0,255}, 1, true},
        {"\x1b_Ga=a,i=541,c=2\x1b\\", {255,255,255,255}, 1, true},
        {"\x1b_Ga=d,d=f,i=541\x1b\\", {255,255,255,255}, 0, false},
        {"\x1b_Ga=d,d=f,i=541\x1b\\", {255,255,255,255}, 0, false}
    };
    static const uint8_t ok[] = "\x1b_Gi=541;OK\x1b\\";
    static const uint8_t release[] = "\x1b_Ga=d,d=F,i=541\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 1;
    config.columns = 1;
    boxrig_terminal *terminal = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) == BOXRIG_STATUS_OK);
    size_t consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, setup, sizeof(setup) - 1, &consumed) == BOXRIG_STATUS_OK);
    boxrig_image_handle handle = 0;
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) == BOXRIG_STATUS_OK);
    boxrig_dirty dirty;
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {}
    for (size_t index = 0; index < sizeof(steps) / sizeof(steps[0]); index++) {
        uint64_t revision = boxrig_terminal_image_revision(terminal);
        const uint8_t *output = NULL;
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)steps[index].command,
                  strlen(steps[index].command), &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(ok) - 1);
        CHECK(memcmp(output, ok, sizeof(ok) - 1) == 0);
        CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) == BOXRIG_STATUS_OK);
        CHECK(terminal->graphics_protocol.storage_usage == 4);
        CHECK(terminal->graphics_protocol.animation_storage_usage == steps[index].extra_frames * 4);
        CHECK(terminal->graphics_protocol.animation_frame_count == steps[index].extra_frames);
        CHECK(boxrig_terminal_image_count(terminal) == 1);
        CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
        boxrig_image_view image;
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) == BOXRIG_STATUS_OK);
        CHECK(memcmp(image.rgba, steps[index].pixel, 4) == 0);
        CHECK((boxrig_terminal_image_revision(terminal) != revision) == steps[index].changed);
        CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty) == steps[index].changed);
        CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    }
    CHECK(boxrig_terminal_feed(terminal, release, sizeof(release) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_count(terminal) == 0);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(terminal->graphics_protocol.storage_usage == 0);
    CHECK(terminal->graphics_protocol.animation_storage_usage == 0);
    CHECK(terminal->graphics_protocol.animation_frame_count == 0);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_frame_delete_current_pixels_and_deadlines(void)
{
    static const uint8_t setup[] =
        "\x1b_Ga=T,i=542,f=32,s=1,v=1,c=1,r=1,C=1,q=2;/wAA/w==\x1b\\"
        "\x1b_Ga=f,i=542,f=32,s=1,v=1,X=1,z=20,q=2;AP8A/w==\x1b\\"
        "\x1b_Ga=f,i=542,f=32,s=1,v=1,X=1,z=30,q=2;AAD//w==\x1b\\"
        "\x1b_Ga=a,i=542,r=1,z=10,s=3,v=1,q=2\x1b\\";
    static const struct {
        unsigned int current;
        const char *deletion;
        uint8_t pixel[4];
        uint64_t deadline;
        uint8_t next[4];
        bool changed;
    } fixtures[] = {
        {1, "d=f,r=1", {0,255,0,255}, 125, {0,0,255,255}, true},
        {2, "d=f,r=2", {0,0,255,255}, 135, {255,0,0,255}, true},
        {3, "d=F,r=3", {0,255,0,255}, 125, {255,0,0,255}, true},
        {3, "d=f,r=999", {0,255,0,255}, 125, {255,0,0,255}, true},
        {2, "d=f,r=0", {0,255,0,255}, 120, {0,0,255,255}, false},
        {2, "d=F", {0,255,0,255}, 120, {0,0,255,255}, false},
        {1, "d=f,r=2", {255,0,0,255}, 110, {0,0,255,255}, false}
    };
    for (size_t index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]); index++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        config.rows = 1;
        config.columns = 1;
        boxrig_terminal *terminal = NULL;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) == BOXRIG_STATUS_OK);
        size_t consumed = 0;
        CHECK(boxrig_terminal_feed(terminal, setup, sizeof(setup) - 1, &consumed) == BOXRIG_STATUS_OK);
        char command[128];
        int length = snprintf(command, sizeof(command), "\x1b_Ga=a,i=542,c=%u,q=2\x1b\\", fixtures[index].current);
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)command, (size_t)length, &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_update_monotonic_time(terminal, 100) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_update_monotonic_time(terminal, 105) == BOXRIG_STATUS_OK);
        boxrig_image_handle handle = 0;
        CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) == BOXRIG_STATUS_OK);
        boxrig_dirty dirty;
        while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {}
        uint64_t revision = boxrig_terminal_image_revision(terminal);
        length = snprintf(command, sizeof(command), "\x1b_Ga=d,i=542,%s,q=2\x1b\\", fixtures[index].deletion);
        for (int byte = 0; byte < length; byte++) {
            CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)command + byte, 1, &consumed) == BOXRIG_STATUS_OK);
            CHECK(consumed == 1);
        }
        CHECK(terminal->graphics_protocol.animation_storage_usage == 4);
        boxrig_image_view image;
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) == BOXRIG_STATUS_OK);
        CHECK(memcmp(image.rgba, fixtures[index].pixel, 4) == 0);
        CHECK((boxrig_terminal_image_revision(terminal) != revision) == fixtures[index].changed);
        CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty) == fixtures[index].changed);
        uint64_t deadline = 0;
        CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
        CHECK(deadline == fixtures[index].deadline);
        CHECK(boxrig_terminal_update_monotonic_time(terminal, deadline) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) == BOXRIG_STATUS_OK);
        CHECK(memcmp(image.rgba, fixtures[index].next, 4) == 0);
        CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_frame_delete_retry_cancel_and_image_number(void)
{
    static const uint8_t setup[] =
        "\x1b_Ga=t,I=7,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\"
        "\x1b_Ga=T,I=7,f=32,s=1,v=1,c=1,r=1,C=1,q=2;AP8A/w==\x1b\\"
        "\x1b_Ga=f,I=7,f=32,s=1,v=1,X=1,q=2;AAD//w==\x1b\\";
    static const uint8_t cancelled[] = "\x1b_Ga=d,d=F,I=7,r=1\x18";
    static const uint8_t partial[] = "\x1b_Ga=f,I=7,f=32,s=1,v=1,m=1,q=2;AAAA\x1b\\";
    static const uint8_t deletion[] = "\x1b_Ga=d,d=f,I=7,r=1\x1b\\";
    static const uint8_t ok[] = "\x1b_Gi=2,I=7;OK\x1b\\";
    static const uint8_t blue[] = {0,0,255,255};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 1;
    config.columns = 1;
    config.output_capacity = sizeof(ok) - 1;
    config.graphics_animation_storage_capacity = 4;
    config.graphics_frame_capacity = 1;
    boxrig_terminal *terminal = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) == BOXRIG_STATUS_OK);
    size_t consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, setup, sizeof(setup) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, cancelled, sizeof(cancelled) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(terminal->graphics_protocol.animation_storage_usage == 4);
    CHECK(boxrig_terminal_feed(terminal, partial, sizeof(partial) - 1, &consumed) == BOXRIG_STATUS_OK);
    boxrig_dirty dirty;
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {}
    uint64_t revision = boxrig_terminal_image_revision(terminal);
    char fill[sizeof(ok) - 1];
    memset(fill, 'x', sizeof(fill));
    CHECK(boxrig_terminal_send_text(terminal, fill, sizeof(fill)) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, deletion, sizeof(deletion) - 1, &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed < sizeof(deletion) - 1 && deletion[consumed] == '\\');
    CHECK(terminal->graphics_protocol.animation_storage_usage == 4);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(fill)) == BOXRIG_STATUS_OK);
    size_t remaining = sizeof(deletion) - 1 - consumed;
    CHECK(boxrig_terminal_feed(terminal, deletion + consumed, remaining, &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == remaining);
    const uint8_t *output = NULL;
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(ok) - 1);
    CHECK(memcmp(output, ok, sizeof(ok) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(ok) - 1) == BOXRIG_STATUS_OK);
    CHECK(terminal->graphics_protocol.animation_storage_usage == 0);
    CHECK(boxrig_terminal_image_count(terminal) == 2);
    boxrig_image_handle handle = 0;
    boxrig_image_view image;
    CHECK(boxrig_terminal_image_handle_at(terminal, 1, &handle) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) == BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, blue, 4) == 0);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    static const uint8_t refill[] =
        "\x1b_Ga=f,I=7,f=32,s=1,v=1,X=1,q=1;/////w==\x1b\\";
    CHECK(boxrig_terminal_feed(terminal, refill, sizeof(refill) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(terminal->graphics_protocol.animation_storage_usage == 4);
    CHECK(terminal->graphics_protocol.animation_frame_count == 1);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_frame_delete_loading_gapless_tail_and_final_placements(void)
{
    static const uint8_t setup[] =
        "\x1b_Ga=T,i=543,p=1,f=32,s=1,v=1,c=1,r=1,C=1,q=2;/wAA/w==\x1b\\"
        "\x1b_Ga=f,i=543,f=32,s=1,v=1,X=1,z=20,q=2;AP8A/w==\x1b\\"
        "\x1b_Ga=f,i=543,f=32,s=1,v=1,X=1,z=-1,q=2;AAD//w==\x1b\\"
        "\x1b_Ga=a,i=543,r=1,z=10,s=2,q=2\x1b\\";
    static const uint8_t delete_tail[] = "\x1b_Ga=d,d=f,i=543,r=3,q=1\x1b\\";
    static const uint8_t append[] =
        "\x1b_Ga=f,i=543,f=32,s=1,v=1,X=1,z=40,q=2;/////w==\x1b\\";
    static const uint8_t green[] = {0,255,0,255};
    static const uint8_t white[] = {255,255,255,255};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2;
    config.columns = 2;
    boxrig_terminal *terminal = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) == BOXRIG_STATUS_OK);
    size_t consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, setup, sizeof(setup) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 100) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 130) == BOXRIG_STATUS_OK);
    uint64_t deadline = 0;
    CHECK(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    boxrig_image_handle handle = 0;
    boxrig_image_view image;
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, delete_tail, sizeof(delete_tail) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) == BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, green, 4) == 0);
    const uint8_t *output = NULL;
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline) && deadline == 150);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 150) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(boxrig_terminal_feed(terminal, append, sizeof(append) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 151) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) == BOXRIG_STATUS_OK);
    CHECK(memcmp(image.rgba, white, 4) == 0);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline) && deadline == 191);
    static const uint8_t finish[] =
        "\x1b_Ga=d,d=F,i=543,r=1,q=2\x1b\\"
        "\x1b_Ga=d,d=F,i=543,r=1,q=2\x1b\\"
        "\x1b_Ga=T,i=544,p=1,P=543,Q=1,f=32,s=1,v=1,c=1,r=1,C=1,q=2;AP8A/w==\x1b\\"
        "\x1b[?47h\x1b_Ga=p,i=543,p=2,c=1,r=1,C=1,q=2\x1b\\"
        "\x1b_Ga=d,d=F,i=543,q=2\x1b\\\x1b[?47l";
    CHECK(boxrig_terminal_feed(terminal, finish, sizeof(finish) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_count(terminal) == 0);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(terminal->graphics_protocol.placement_count == 0);
    CHECK(terminal->graphics_protocol.storage_usage == 0);
    CHECK(terminal->graphics_protocol.animation_storage_usage == 0);
    CHECK(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) == BOXRIG_STATUS_INVALID_ARGUMENT);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_cell_pixel_geometry_and_window_reports(void)
{
    static const uint8_t reports[] = "\x1b[14t\x1b[16t";
    static const uint8_t expected[] = "\x1b[4;48;40t\x1b[6;16;8t";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    const uint8_t *output = NULL;
    boxrig_dirty dirty;
    uint64_t revision;
    size_t consumed = 0;
    size_t output_length;

    config.rows = 3;
    config.columns = 5;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    revision = boxrig_terminal_image_revision(terminal);
    CHECK(revision != 0);
    CHECK(boxrig_terminal_feed(
              terminal,
              reports,
              sizeof(reports) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(reports) - 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 0, 16) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 8, 0) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 8, 16) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) != revision);
    revision = boxrig_terminal_image_revision(terminal);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 8, 16) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              reports,
              sizeof(reports) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(reports) - 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 9, 18) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) != revision);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_raw_rgb_and_rgba_normalize_at_every_input_split(void)
{
    static const uint8_t rgb_input[] =
        "\x1b_Ga=t,i=101,f=24,s=2,v=2;/wAAAP8AAAD/////\x1b\\";
    static const uint8_t rgb_expected[] = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 255
    };
    static const uint8_t rgb_response[] = "\x1b_Gi=101;OK\x1b\\";
    static const uint8_t rgba_input[] =
        "\x1b_Ga=t,i=102,f=32,s=2,v=2;/wAA/wD/AIAAAP8A////fw==\x1b\\";
    static const uint8_t rgba_expected[] = {
        255, 0, 0, 255, 0, 255, 0, 128,
        0, 0, 255, 0, 255, 255, 255, 127
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    size_t split;

    config.rows = 1;
    config.columns = 1;
    for (split = 0; split <= sizeof(rgb_input) - 1; split++) {
        boxrig_terminal *terminal = NULL;
        boxrig_image_handle handle = 0;
        boxrig_image_view image;
        const uint8_t *output = NULL;
        size_t consumed = 0;

        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  rgb_input,
                  split,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == split);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  rgb_input + split,
                  sizeof(rgb_input) - 1 - split,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(rgb_input) - 1 - split);
        CHECK(boxrig_terminal_image_count(terminal) == 1);
        CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
              BOXRIG_STATUS_OK);
        CHECK(handle != 0);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
              BOXRIG_STATUS_OK);
        CHECK(image.handle == handle);
        CHECK(image.revision != 0);
        CHECK(image.image_id == 101);
        CHECK(image.image_number == 0);
        CHECK(image.width == 2 && image.height == 2);
        CHECK(image.stride == 8);
        CHECK(image.rgba_length == sizeof(rgb_expected));
        CHECK(memcmp(image.rgba, rgb_expected, sizeof(rgb_expected)) == 0);
        CHECK(boxrig_terminal_output_peek(terminal, &output) ==
              sizeof(rgb_response) - 1);
        CHECK(memcmp(output, rgb_response, sizeof(rgb_response) - 1) == 0);
        boxrig_terminal_destroy(terminal);
    }

    {
        boxrig_terminal *terminal = NULL;
        boxrig_image_handle handle = 0;
        boxrig_image_view image;
        size_t consumed = 0;

        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  rgba_input,
                  sizeof(rgba_input) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(rgba_input) - 1);
        CHECK(boxrig_terminal_image_count(terminal) == 1);
        CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
              BOXRIG_STATUS_OK);
        CHECK(image.image_id == 102);
        CHECK(image.width == 2 && image.height == 2 && image.stride == 8);
        CHECK(image.rgba_length == sizeof(rgba_expected));
        CHECK(memcmp(image.rgba, rgba_expected, sizeof(rgba_expected)) == 0);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_raw_base64_validation_and_query_is_non_mutating(void)
{
    struct fixture {
        const char *payload;
        const char *expected_message;
    } invalid[] = {
        {"AAAA AA==", "EINVAL:invalid command"},
        {"AAAA_A==", "EINVAL:invalid command"},
        {"AAAAAA=A", "EINVAL:invalid command"},
        {"AAAAA===", "EINVAL:invalid command"},
        {"AAAAAB==", "EINVAL:invalid command"},
        {"AAAAAA==A", "EINVAL:invalid command"}
    };
    static const uint8_t query[] =
        "\x1b_Ga=q,i=111,f=32,s=1,v=1;AAAAAA\x1b\\";
    static const uint8_t query_response[] = "\x1b_Gi=111;OK\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    size_t index;

    config.rows = 1;
    config.columns = 1;
    for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
        boxrig_terminal *terminal = NULL;
        const uint8_t *output = NULL;
        char input[160];
        char expected[160];
        size_t consumed = 0;
        int input_length;
        int expected_length;

        input_length = snprintf(
            input,
            sizeof(input),
            "\x1b_Ga=t,i=%zu,f=32,s=1,v=1;%s\x1b\\",
            112 + index,
            invalid[index].payload);
        expected_length = snprintf(
            expected,
            sizeof(expected),
            "\x1b_Gi=%zu;%s\x1b\\",
            112 + index,
            invalid[index].expected_message);
        CHECK(input_length > 0 && (size_t)input_length < sizeof(input));
        CHECK(expected_length > 0 &&
              (size_t)expected_length < sizeof(expected));
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)input,
                  (size_t)input_length,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == (size_t)input_length);
        CHECK(boxrig_terminal_image_count(terminal) == 0);
        CHECK(boxrig_terminal_output_peek(terminal, &output) ==
              (size_t)expected_length);
        CHECK(memcmp(output, expected, (size_t)expected_length) == 0);
        boxrig_terminal_destroy(terminal);
    }

    {
        boxrig_terminal *terminal = NULL;
        const uint8_t *output = NULL;
        uint64_t revision;
        size_t consumed = 0;

        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        revision = boxrig_terminal_image_revision(terminal);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  query,
                  sizeof(query) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(query) - 1);
        CHECK(boxrig_terminal_image_count(terminal) == 0);
        CHECK(boxrig_terminal_image_revision(terminal) == revision);
        CHECK(boxrig_terminal_output_peek(terminal, &output) ==
              sizeof(query_response) - 1);
        CHECK(memcmp(output, query_response, sizeof(query_response) - 1) == 0);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_raw_apc_chunks_and_empty_final_chunk(void)
{
    static const char base64[] = "/wAAAP8AAAD/////";
    static const uint8_t expected_pixels[] = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 255
    };
    static const uint8_t expected_response[] = "\x1b_Gi=131;OK\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    size_t split;

    config.rows = 1;
    config.columns = 1;
    for (split = 0; split <= sizeof(base64) - 1; split += 4) {
        boxrig_terminal *terminal = NULL;
        boxrig_image_handle handle = 0;
        boxrig_image_view image;
        const uint8_t *output = NULL;
        char first[160];
        char final[160];
        size_t consumed = 0;
        int first_length;
        int final_length;

        first_length = snprintf(
            first,
            sizeof(first),
            "\x1b_Ga=t,i=131,f=24,s=2,v=2,m=1;%.*s\x1b\\",
            (int)split,
            base64);
        final_length = snprintf(
            final,
            sizeof(final),
            "\x1b_Gm=0;%s\x1b\\",
            base64 + split);
        CHECK(first_length > 0 && (size_t)first_length < sizeof(first));
        CHECK(final_length > 0 && (size_t)final_length < sizeof(final));
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)first,
                  (size_t)first_length,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == (size_t)first_length);
        CHECK(boxrig_terminal_image_count(terminal) == 0);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)final,
                  (size_t)final_length,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == (size_t)final_length);
        CHECK(boxrig_terminal_image_count(terminal) == 1);
        CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
              BOXRIG_STATUS_OK);
        CHECK(image.image_id == 131);
        CHECK(image.rgba_length == sizeof(expected_pixels));
        CHECK(memcmp(image.rgba, expected_pixels, sizeof(expected_pixels)) ==
              0);
        CHECK(boxrig_terminal_output_peek(terminal, &output) ==
              sizeof(expected_response) - 1);
        CHECK(memcmp(
                  output,
                  expected_response,
                  sizeof(expected_response) - 1) == 0);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_raw_chunk_errors_and_new_command_abort_old_upload(void)
{
    static const uint8_t input[] =
        "\x1b_Ga=t,i=161,f=24,s=1,v=1,m=1;AAA\x1b\\"
        "\x1b_Ga=t,i=162,f=24,s=1,v=1;AQID\x1b\\"
        "\x1b_Ga=t,i=163,f=24,s=1,v=1,m=1;AAAA\x1b\\"
        "\x1b_Ga=t,i=164,f=24,s=1,v=1;BAUG\x1b\\"
        "\x1b_Ga=t,i=165,f=24,s=1,v=1,m=1;AAAA\x1b\\"
        "\x1b_Gm=0,s=1;AAAA\x1b\\"
        "\x1b_Ga=t,i=166,f=24,s=1,v=1;BwgJ\x1b\\"
        "\x1b_Ga=t,i=167,f=32,s=1,v=1,m=1;AAAAAA==\x1b\\";
    static const uint8_t expected_output[] =
        "\x1b_Gi=161;EINVAL:invalid command\x1b\\"
        "\x1b_Gi=162;OK\x1b\\"
        "\x1b_Gi=164;OK\x1b\\"
        "\x1b_Gi=166;OK\x1b\\"
        "\x1b_Gi=167;EINVAL:invalid command\x1b\\";
    static const uint32_t expected_ids[] = {162, 164, 166};
    static const uint8_t expected_red[] = {1, 4, 7};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t index;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              input,
              sizeof(input) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(boxrig_terminal_image_count(terminal) ==
          sizeof(expected_ids) / sizeof(expected_ids[0]));
    for (index = 0; index < sizeof(expected_ids) / sizeof(expected_ids[0]);
         index++) {
        boxrig_image_handle handle = 0;
        boxrig_image_view image;

        CHECK(boxrig_terminal_image_handle_at(terminal, index, &handle) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
              BOXRIG_STATUS_OK);
        CHECK(image.image_id == expected_ids[index]);
        CHECK(image.rgba_length == 4);
        CHECK(image.rgba[0] == expected_red[index]);
        CHECK(image.rgba[3] == 255);
    }
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(expected_output) - 1);
    CHECK(memcmp(output, expected_output, sizeof(expected_output) - 1) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_raw_dimensions_and_decode_capacity_are_bounded(void)
{
    struct fixture {
        const char *header;
        const char *message;
    } fixtures[] = {
        {
            "a=t,i=171,f=24,s=1000001,v=1",
            "EINVAL:invalid command"
        },
        {
            "a=t,i=172,f=32,s=1000000,v=1000000",
            "EINVAL:invalid command"
        },
        {
            "a=t,i=173,f=24,s=1,v=1",
            "EINVAL:invalid command"
        }
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    size_t index;

    config.rows = 1;
    config.columns = 1;
    for (index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]); index++) {
        boxrig_terminal *terminal = NULL;
        const uint8_t *output = NULL;
        char input[160];
        char expected[160];
        size_t consumed = 0;
        int input_length = snprintf(
            input,
            sizeof(input),
            "\x1b_G%s;AA==\x1b\\",
            fixtures[index].header);
        int expected_length = snprintf(
            expected,
            sizeof(expected),
            "\x1b_Gi=%zu;%s\x1b\\",
            171 + index,
            fixtures[index].message);

        CHECK(input_length > 0 && (size_t)input_length < sizeof(input));
        CHECK(expected_length > 0 &&
              (size_t)expected_length < sizeof(expected));
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)input,
                  (size_t)input_length,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == (size_t)input_length);
        CHECK(boxrig_terminal_image_count(terminal) == 0);
        CHECK(boxrig_terminal_output_peek(terminal, &output) ==
              (size_t)expected_length);
        CHECK(memcmp(output, expected, (size_t)expected_length) == 0);
        boxrig_terminal_destroy(terminal);
    }

    {
        static const uint8_t input[] =
            "\x1b_Ga=t,i=174,f=32,s=1,v=1;AQIDBA==\x1b\\";
        static const uint8_t expected[] =
            "\x1b_Gi=174;ENOSPC:graphics capacity exceeded\x1b\\";
        boxrig_terminal *terminal = NULL;
        const uint8_t *output = NULL;
        size_t consumed = 0;

        config.graphics_decode_capacity = 3;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  input,
                  sizeof(input) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(input) - 1);
        CHECK(boxrig_terminal_image_count(terminal) == 0);
        CHECK(boxrig_terminal_output_peek(terminal, &output) ==
              sizeof(expected) - 1);
        CHECK(memcmp(output, expected, sizeof(expected) - 1) == 0);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_zlib_and_png_normalize_at_every_input_split(void)
{
    static const uint8_t zlib_rgb[] =
        "\x1b_Ga=t,i=201,f=24,s=2,v=2,o=z;"
        "eJz7z8DA8B+E////DwAd7gX7\x1b\\";
    static const uint8_t zlib_rgba[] =
        "\x1b_Ga=t,i=202,f=32,s=2,v=2,o=z;"
        "eJz7z8Dwn+E/QwOY+v+/HgA72Af5\x1b\\";
    static const uint8_t palette_png[] =
        "\x1b_Ga=t,i=203,f=100,s=77,v=88;"
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAMAAABFaP0WAAAADFBMVEX/AAAA/wAA"
        "AP/////7AGD2AAAABHRSTlP/gAB/TBkXRgAAAA5JREFUeJxjYGBkYGIGAAARAAee"
        "oioSAAAAAElFTkSuQmCC\x1b\\";
    static const uint8_t rgba_png[] =
        "\x1b_Ga=t,i=204,f=100;"
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAF0lEQVR4nGP4z8Dw"
        "n+E/QwMDmP7/vx4AP1cH+TNnSIYAAAAASUVORK5CYII=\x1b\\";
    static const uint8_t outer_zlib_png[] =
        "\x1b_Ga=t,i=205,f=100,o=z,S=111;"
        "eJzrDPBz5+WS4mJgYOD19HAJYmBgYAJhDmYGBgbXjL9iDAwMPAE+Ia7/GRgYwPj/"
        "//+/GRK+MTAwsJQE+QX/b2Co95EUd2NgYODzdHEMqZiTnJCQkpDExsAgyMA+b5GW"
        "EAMDA4Onq5/LOqeEJgB2oht9\x1b\\";
    static const uint8_t rgb_expected[] = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 255
    };
    static const uint8_t rgba_expected[] = {
        255, 0, 0, 255, 0, 255, 0, 128,
        0, 0, 255, 0, 255, 255, 255, 127
    };
    struct fixture {
        const uint8_t *input;
        size_t length;
        uint32_t image_id;
        const uint8_t *expected;
    } fixtures[] = {
        {zlib_rgb, sizeof(zlib_rgb) - 1, 201, rgb_expected},
        {zlib_rgba, sizeof(zlib_rgba) - 1, 202, rgba_expected},
        {palette_png, sizeof(palette_png) - 1, 203, rgba_expected},
        {rgba_png, sizeof(rgba_png) - 1, 204, rgba_expected},
        {outer_zlib_png, sizeof(outer_zlib_png) - 1, 205, rgba_expected}
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    size_t fixture_index;

    config.rows = 1;
    config.columns = 1;
    for (fixture_index = 0;
         fixture_index < sizeof(fixtures) / sizeof(fixtures[0]);
         fixture_index++) {
        size_t split;

        for (split = 0; split <= fixtures[fixture_index].length; split++) {
            boxrig_terminal *terminal = NULL;
            boxrig_image_handle handle = 0;
            boxrig_image_view image;
            const uint8_t *output = NULL;
            char expected_response[64];
            size_t consumed = 0;
            int response_length;

            response_length = snprintf(
                expected_response,
                sizeof(expected_response),
                "\x1b_Gi=%lu;OK\x1b\\",
                (unsigned long)fixtures[fixture_index].image_id);
            CHECK(response_length > 0 &&
                  (size_t)response_length < sizeof(expected_response));
            CHECK(boxrig_terminal_create(&config, &terminal) ==
                  BOXRIG_STATUS_OK);
            CHECK(boxrig_terminal_feed(
                      terminal,
                      fixtures[fixture_index].input,
                      split,
                      &consumed) == BOXRIG_STATUS_OK);
            CHECK(consumed == split);
            consumed = 0;
            CHECK(boxrig_terminal_feed(
                      terminal,
                      fixtures[fixture_index].input + split,
                      fixtures[fixture_index].length - split,
                      &consumed) == BOXRIG_STATUS_OK);
            CHECK(consumed == fixtures[fixture_index].length - split);
            CHECK(boxrig_terminal_image_count(terminal) == 1);
            CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
                  BOXRIG_STATUS_OK);
            CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
                  BOXRIG_STATUS_OK);
            CHECK(image.image_id == fixtures[fixture_index].image_id);
            CHECK(image.width == 2 && image.height == 2 && image.stride == 8);
            CHECK(image.rgba_length == sizeof(rgba_expected));
            CHECK(memcmp(
                      image.rgba,
                      fixtures[fixture_index].expected,
                      sizeof(rgba_expected)) == 0);
            CHECK(boxrig_terminal_output_peek(terminal, &output) ==
                  (size_t)response_length);
            CHECK(memcmp(
                      output,
                      expected_response,
                      (size_t)response_length) == 0);
            boxrig_terminal_destroy(terminal);
        }
    }
    return true;
}

static bool test_png_grayscale_alpha_16_bit_and_interlace_normalize(void)
{
    static const uint8_t grayscale_png[] =
        "\x1b_Ga=t,i=211,f=100;"
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACAgAAAAAdbUpZAAAADElEQVR4nGMQYNgA"
        "AADkAMEnqOhXAAAAAElFTkSuQmCC\x1b\\";
    static const uint8_t gray_alpha_16_png[] =
        "\x1b_Ga=t,i=212,f=100;"
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACEAQAAACILxnsAAAAGklEQVR4nGNgYPzP"
        "EMrUwMywioWB9T9bPTsAI7IEGZV3LWEAAAAASUVORK5CYII=\x1b\\";
    static const uint8_t interlaced_png[] =
        "\x1b_Ga=t,i=213,f=100;"
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAAEFsT2yAAAAFUlEQVR4nGP4z8AA"
        "Rg0MIPL////1AEFWB/mayCNYAAAAAElFTkSuQmCC\x1b\\";
    static const uint8_t grayscale_expected[] = {
        0, 0, 0, 255, 85, 85, 85, 255,
        170, 170, 170, 255, 255, 255, 255, 255
    };
    static const uint8_t alpha_expected[] = {
        0, 0, 0, 254, 85, 85, 85, 128,
        169, 169, 169, 0, 254, 254, 254, 127
    };
    static const uint8_t rgba_expected[] = {
        255, 0, 0, 255, 0, 255, 0, 128,
        0, 0, 255, 0, 255, 255, 255, 127
    };
    struct fixture {
        const uint8_t *input;
        size_t length;
        uint32_t image_id;
        const uint8_t *expected;
    } fixtures[] = {
        {grayscale_png, sizeof(grayscale_png) - 1, 211, grayscale_expected},
        {gray_alpha_16_png,
         sizeof(gray_alpha_16_png) - 1,
         212,
         alpha_expected},
        {interlaced_png, sizeof(interlaced_png) - 1, 213, rgba_expected}
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    size_t index;

    config.rows = 1;
    config.columns = 1;
    for (index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]); index++) {
        boxrig_terminal *terminal = NULL;
        boxrig_image_handle handle = 0;
        boxrig_image_view image;
        size_t consumed = 0;

        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  fixtures[index].input,
                  fixtures[index].length,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == fixtures[index].length);
        CHECK(boxrig_terminal_image_count(terminal) == 1);
        CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
              BOXRIG_STATUS_OK);
        CHECK(image.image_id == fixtures[index].image_id);
        CHECK(image.width == 2 && image.height == 2 && image.stride == 8);
        CHECK(image.rgba_length == sizeof(rgba_expected));
        CHECK(memcmp(image.rgba, fixtures[index].expected, sizeof(rgba_expected)) ==
              0);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_zlib_and_png_apc_chunk_boundaries(void)
{
    static const char zlib_rgba[] = "eJz7z8Dwn+E/QwOY+v+/HgA72Af5";
    static const char direct_png[] =
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAF0lEQVR4nGP4z8Dw"
        "n+E/QwMDmP7/vx4AP1cH+TNnSIYAAAAASUVORK5CYII=";
    static const char outer_zlib_png[] =
        "eJzrDPBz5+WS4mJgYOD19HAJYmBgYAJhDmYGBgbXjL9iDAwMPAE+Ia7/GRgYwPj/"
        "//+/GRK+MTAwsJQE+QX/b2Co95EUd2NgYODzdHEMqZiTnJCQkpDExsAgyMA+b5GW"
        "EAMDA4Onq5/LOqeEJgB2oht9";
    static const uint8_t rgba_expected[] = {
        255, 0, 0, 255, 0, 255, 0, 128,
        0, 0, 255, 0, 255, 255, 255, 127
    };
    struct fixture {
        const char *header;
        const char *payload;
        uint32_t image_id;
    } fixtures[] = {
        {"f=32,s=2,v=2,o=z", zlib_rgba, 221},
        {"f=100", direct_png, 222},
        {"f=100,o=z,S=111", outer_zlib_png, 223}
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    size_t fixture_index;

    config.rows = 1;
    config.columns = 1;
    for (fixture_index = 0;
         fixture_index < sizeof(fixtures) / sizeof(fixtures[0]);
         fixture_index++) {
        size_t payload_length = strlen(fixtures[fixture_index].payload);
        size_t last_split = payload_length;
        size_t split;

        if (last_split != 0 &&
            fixtures[fixture_index].payload[last_split - 1] == '=') {
            last_split -= 4;
        }
        for (split = 0; split <= last_split; split += 4) {
            boxrig_terminal *terminal = NULL;
            boxrig_image_handle handle = 0;
            boxrig_image_view image;
            char first[320];
            char final[320];
            size_t consumed = 0;
            int first_length;
            int final_length;

            first_length = snprintf(
                first,
                sizeof(first),
                "\x1b_Ga=t,i=%lu,%s,m=1;%.*s\x1b\\",
                (unsigned long)fixtures[fixture_index].image_id,
                fixtures[fixture_index].header,
                (int)split,
                fixtures[fixture_index].payload);
            final_length = snprintf(
                final,
                sizeof(final),
                "\x1b_Gm=0;%s\x1b\\",
                fixtures[fixture_index].payload + split);
            CHECK(first_length > 0 && (size_t)first_length < sizeof(first));
            CHECK(final_length > 0 && (size_t)final_length < sizeof(final));
            CHECK(boxrig_terminal_create(&config, &terminal) ==
                  BOXRIG_STATUS_OK);
            CHECK(boxrig_terminal_feed(
                      terminal,
                      (const uint8_t *)first,
                      (size_t)first_length,
                      &consumed) == BOXRIG_STATUS_OK);
            CHECK(consumed == (size_t)first_length);
            CHECK(boxrig_terminal_image_count(terminal) == 0);
            consumed = 0;
            CHECK(boxrig_terminal_feed(
                      terminal,
                      (const uint8_t *)final,
                      (size_t)final_length,
                      &consumed) == BOXRIG_STATUS_OK);
            CHECK(consumed == (size_t)final_length);
            CHECK(boxrig_terminal_image_count(terminal) == 1);
            CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
                  BOXRIG_STATUS_OK);
            CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
                  BOXRIG_STATUS_OK);
            CHECK(image.image_id == fixtures[fixture_index].image_id);
            CHECK(image.rgba_length == sizeof(rgba_expected));
            CHECK(memcmp(image.rgba, rgba_expected, sizeof(rgba_expected)) ==
                  0);
            boxrig_terminal_destroy(terminal);
        }
    }
    return true;
}

static bool test_zlib_png_errors_and_decode_quota_recover(void)
{
    struct fixture {
        const char *header;
        const char *payload;
        const char *message;
    } fixtures[] = {
        {"f=24,s=2,v=2,o=z", "+8/AwPAfhP///w8A", "EBADZLIB:invalid stream"},
        {"f=24,s=2,v=2,o=z",
         "H4sIAAAAAAAAA/vPwMDwH4T///8PAA+RPLUMAAAA",
         "EBADZLIB:invalid stream"},
        {"f=24,s=2,v=2,o=z",
         "eJz7z8DA8B+E////DwAd7gX6",
         "EBADZLIB:invalid stream"},
        {"f=24,s=2,v=2,o=z",
         "eJz7z8DA8B+E////DwAd7gX7AA==",
         "EBADZLIB:invalid stream"},
        {"f=24,s=2,v=2,o=z",
         "eJz7z8DA8B+E////DwAd7gU=",
         "EBADZLIB:invalid stream"},
        {"f=24,s=1,v=1,o=z",
         "eJz7z8DA8B+E////DwAd7gX7",
         "EBADZLIB:invalid stream"},
        {"f=100",
         "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAMAAABFaP0WAAAADFBMVEX/AAAA/wAA"
         "AP/////7AGD2AAAABHRSTlP/gAB/TBkXRgAAAA5JREFUeJxjYGBkYGIGAAARAAee"
         "oioTAAAAAElFTkSuQmCC",
         "EBADPNG:invalid image"},
        {"f=100",
         "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAMAAABFaP0WAAAADFBMVEX/AAAA/wAA"
         "AP/////7AGD2AAAABHRSTlP/gAB/TBkXRgAAAA5JREFUeJxjYGBkYGIGAAARAAee"
         "oioSAAAAAElFTkSuQmCCAA==",
         "EBADPNG:invalid image"},
        {"f=100",
         "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAMAAABFaP0WAAAADFBMVEX/AAAA/wAA"
         "AP/////7AGD2AAAABHRSTlP/gAB/TBkXRgAAAA5JREFUeJxjYGBkYGIGAAARAAee"
         "oioSAAAAAElFTkSuQmA=",
         "EBADPNG:invalid image"},
        {"f=100",
         "iVBORw0KGgoAAAANSUhEUgAPQkEAAAABCAYAAAB9H/x2AAAAAElFTkSuQmCC",
         "EBADPNG:invalid image"},
        {"f=100,o=z,S=110",
         "eJzrDPBz5+WS4mJgYOD19HAJYmBgYAJhDmYGBgbXjL9iDAwMPAE+Ia7/GRgYwPj/"
         "//+/GRK+MTAwsJQE+QX/b2Co95EUd2NgYODzdHEMqZiTnJCQkpDExsAgyMA+b5GW"
         "EAMDA4Onq5/LOqeEJgB2oht9",
         "EBADZLIB:invalid stream"},
        {"f=24,s=1,v=1,S=0", "AQID", "EINVAL:invalid command"},
        {"f=100,S=0",
         "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAF0lEQVR4nGP4z8Dw"
         "n+E/QwMDmP7/vx4AP1cH+TNnSIYAAAAASUVORK5CYII=",
         "EINVAL:invalid command"}
    };
    static const uint8_t valid[] =
        "\x1b_Ga=t,i=239,f=24,s=1,v=1;AQID\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    size_t index;

    config.rows = 1;
    config.columns = 1;
    for (index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]); index++) {
        boxrig_terminal *terminal = NULL;
        boxrig_image_handle handle = 0;
        boxrig_image_view image;
        const uint8_t *output = NULL;
        char input[384];
        char expected[160];
        size_t consumed = 0;
        int input_length;
        int expected_length;

        input_length = snprintf(
            input,
            sizeof(input),
            "\x1b_Ga=t,i=%lu,%s;%s\x1b\\",
            (unsigned long)(230 + index),
            fixtures[index].header,
            fixtures[index].payload);
        expected_length = snprintf(
            expected,
            sizeof(expected),
            "\x1b_Gi=%lu;%s\x1b\\",
            (unsigned long)(230 + index),
            fixtures[index].message);
        CHECK(input_length > 0 && (size_t)input_length < sizeof(input));
        CHECK(expected_length > 0 && (size_t)expected_length < sizeof(expected));
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)input,
                  (size_t)input_length,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == (size_t)input_length);
        CHECK(boxrig_terminal_image_count(terminal) == 0);
        CHECK(boxrig_terminal_output_peek(terminal, &output) ==
              (size_t)expected_length);
        CHECK(memcmp(output, expected, (size_t)expected_length) == 0);
        CHECK(boxrig_terminal_output_consume(terminal, (size_t)expected_length) ==
              BOXRIG_STATUS_OK);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  valid,
                  sizeof(valid) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(valid) - 1);
        CHECK(boxrig_terminal_image_count(terminal) == 1);
        CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
              BOXRIG_STATUS_OK);
        CHECK(image.image_id == 239);
        boxrig_terminal_destroy(terminal);
    }

    {
        static const uint8_t png[] =
            "\x1b_Ga=t,i=241,f=100;"
            "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAF0lEQVR4nGP4z8Dw"
            "n+E/QwMDmP7/vx4AP1cH+TNnSIYAAAAASUVORK5CYII=\x1b\\";
        static const uint8_t expected[] =
            "\x1b_Gi=241;ENOSPC:graphics capacity exceeded\x1b\\";
        boxrig_terminal *terminal = NULL;
        const uint8_t *output = NULL;
        size_t consumed = 0;

        config.graphics_decode_capacity = 256;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  png,
                  sizeof(png) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(png) - 1);
        CHECK(boxrig_terminal_image_count(terminal) == 0);
        CHECK(boxrig_terminal_output_peek(terminal, &output) ==
              sizeof(expected) - 1);
        CHECK(memcmp(output, expected, sizeof(expected) - 1) == 0);
        boxrig_terminal_destroy(terminal);
    }

    {
        static const uint8_t zlib[] =
            "\x1b_Ga=t,i=242,f=32,s=1,v=1,o=z;eJxjZGJmAQAAGAAL\x1b\\";
        static const uint8_t expected[] =
            "\x1b_Gi=242;ENOSPC:graphics capacity exceeded\x1b\\";
        boxrig_terminal *terminal = NULL;
        const uint8_t *output = NULL;
        size_t consumed = 0;

        config.graphics_decode_capacity = 4;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  zlib,
                  sizeof(zlib) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(zlib) - 1);
        CHECK(boxrig_terminal_image_count(terminal) == 0);
        CHECK(boxrig_terminal_output_peek(terminal, &output) ==
              sizeof(expected) - 1);
        CHECK(memcmp(output, expected, sizeof(expected) - 1) == 0);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_raw_upload_abort_reset_and_destroy(void)
{
    static const uint8_t first[] =
        "\x1b_Ga=t,i=141,f=24,s=1,v=1,m=1;AAAA\x1b\\";
    static const uint8_t cancelled[] = "\x1b_Gm=0;AA";
    static const uint8_t valid[] =
        "\x1b_Ga=t,i=142,f=24,s=1,v=1;AAAA\x1b\\";
    static const uint8_t ris_and_valid[] =
        "\x1b" "c"
        "\x1b_Gm=0;\x1b\\"
        "\x1b_Ga=t,i=143,f=24,s=1,v=1;AAAA\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    const uint8_t cancellations[] = {0x18, 0x1a};
    size_t index;

    config.rows = 1;
    config.columns = 1;
    for (index = 0; index < sizeof(cancellations); index++) {
        boxrig_terminal *terminal = NULL;
        boxrig_image_handle handle = 0;
        boxrig_image_view image;
        size_t consumed = 0;

        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  first,
                  sizeof(first) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  cancelled,
                  sizeof(cancelled) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(cancelled) - 1);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  &cancellations[index],
                  1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == 1);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  valid,
                  sizeof(valid) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(valid) - 1);
        CHECK(boxrig_terminal_image_count(terminal) == 1);
        CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
              BOXRIG_STATUS_OK);
        CHECK(image.image_id == 142);
        boxrig_terminal_destroy(terminal);
    }

    {
        boxrig_terminal *terminal = NULL;
        boxrig_image_handle handle = 0;
        boxrig_image_view image;
        size_t consumed = 0;

        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  first,
                  sizeof(first) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  ris_and_valid,
                  sizeof(ris_and_valid) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(ris_and_valid) - 1);
        CHECK(boxrig_terminal_image_count(terminal) == 1);
        CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
              BOXRIG_STATUS_OK);
        CHECK(image.image_id == 143);
        boxrig_terminal_destroy(terminal);
    }

    {
        boxrig_terminal *terminal = NULL;
        size_t consumed = 0;

        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  first,
                  sizeof(first) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_raw_upload_limits_discard_logical_staging(void)
{
    static const uint8_t first[] =
        "\x1b_Ga=t,i=181,f=24,s=1,v=1,m=1;AAAA\x1b\\";
    static const uint8_t continuation[] = "\x1b_Gm=0;";
    static const uint8_t suffix[] = "\x1b\\";
    static const uint8_t valid[] =
        "\x1b_Ga=t,i=182,f=24,s=1,v=1;AQID\x1b\\";
    boxrig_terminal_config config;
    uint8_t payload[BOXRIG_KITTY_GRAPHICS_PAYLOAD_CAPACITY + 1];
    size_t mode;

    memset(payload, 'A', sizeof(payload));
    for (mode = 0; mode < 2; mode++) {
        boxrig_terminal *terminal = NULL;
        boxrig_image_handle handle = 0;
        boxrig_image_view image;
        const uint8_t *output = NULL;
        size_t consumed = 0;
        size_t overflow_length = mode == 0 ? 65 : sizeof(payload);

        config = boxrig_terminal_config_default();
        config.rows = 1;
        config.columns = 1;
        if (mode == 0) {
            config.control_string_capacity = 64;
        }
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  first,
                  sizeof(first) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  continuation,
                  sizeof(continuation) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(continuation) - 1);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  payload,
                  overflow_length,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == overflow_length);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  suffix,
                  sizeof(suffix) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(suffix) - 1);
        CHECK(boxrig_terminal_image_count(terminal) == 0);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  valid,
                  sizeof(valid) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(valid) - 1);
        CHECK(boxrig_terminal_image_count(terminal) == 1);
        CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
              BOXRIG_STATUS_OK);
        CHECK(image.image_id == 182);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_raw_final_commit_backpressure_is_atomic(void)
{
    static const uint8_t input[] =
        "\x1b_Ga=t,i=151,f=24,s=1,v=1;AQID\x1b\\";
    static const uint8_t response[] = "\x1b_Gi=151;OK\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_handle handle = 0;
    boxrig_image_view image;
    const uint8_t *output = NULL;
    uint8_t fill[sizeof(response) - 1];
    uint64_t revision;
    size_t consumed = 0;
    size_t remaining;

    config.rows = 1;
    config.columns = 1;
    config.output_capacity = sizeof(fill);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    revision = boxrig_terminal_image_revision(terminal);
    memset(fill, 'x', sizeof(fill));
    CHECK(boxrig_terminal_send_text(
              terminal,
              (const char *)fill,
              sizeof(fill)) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              input,
              sizeof(input) - 1,
              &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed < sizeof(input) - 1);
    CHECK(input[consumed] == (uint8_t)'\\');
    CHECK(boxrig_terminal_image_count(terminal) == 0);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(fill));
    CHECK(memcmp(output, fill, sizeof(fill)) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(fill)) ==
          BOXRIG_STATUS_OK);
    remaining = sizeof(input) - 1 - consumed;
    CHECK(boxrig_terminal_feed(
              terminal,
              input + consumed,
              remaining,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == remaining);
    CHECK(boxrig_terminal_image_count(terminal) == 1);
    CHECK(boxrig_terminal_image_revision(terminal) != revision);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(image.image_id == 151);
    CHECK(image.rgba_length == 4);
    CHECK(image.rgba[0] == 1 && image.rgba[1] == 2 &&
          image.rgba[2] == 3 && image.rgba[3] == 255);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(response) - 1);
    CHECK(memcmp(output, response, sizeof(response) - 1) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_image_identity_replacement_and_anonymous_resources(void)
{
    static const uint8_t first[] =
        "\x1b_Ga=t,i=301,f=24,s=1,v=1;AQID\x1b\\";
    static const uint8_t first_response[] = "\x1b_Gi=301;OK\x1b\\";
    static const uint8_t remaining[] =
        "\x1b_Ga=t,f=24,s=1,v=1;BAUG\x1b\\"
        "\x1b_Ga=t,i=0,f=24,s=1,v=1;BwgJ\x1b\\"
        "\x1b_Ga=t,i=301,f=24,s=1,v=1;CgsM\x1b\\";
    static const uint8_t replacement_response[] = "\x1b_Gi=301;OK\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_handle old_handle = 0;
    boxrig_image_handle anonymous_handles[2] = {0, 0};
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t anonymous_count = 0;
    size_t explicit_count = 0;
    size_t index;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              first,
              sizeof(first) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(first) - 1);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &old_handle) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(first_response) - 1);
    CHECK(memcmp(output, first_response, sizeof(first_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(first_response) - 1) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              remaining,
              sizeof(remaining) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(remaining) - 1);
    CHECK(boxrig_terminal_image_count(terminal) == 3);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(replacement_response) - 1);
    CHECK(memcmp(
              output,
              replacement_response,
              sizeof(replacement_response) - 1) == 0);
    for (index = 0; index < boxrig_terminal_image_count(terminal); index++) {
        boxrig_image_handle handle = 0;
        boxrig_image_view image;

        CHECK(boxrig_terminal_image_handle_at(terminal, index, &handle) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
              BOXRIG_STATUS_OK);
        CHECK(image.rgba_length == 4 && image.rgba[3] == 255);
        if (image.image_id == 0) {
            CHECK(anonymous_count < 2);
            CHECK(image.rgba[0] == 4 || image.rgba[0] == 7);
            anonymous_handles[anonymous_count] = handle;
            anonymous_count++;
        } else {
            CHECK(image.image_id == 301);
            CHECK(image.rgba[0] == 10);
            CHECK(handle != old_handle);
            explicit_count++;
        }
    }
    CHECK(anonymous_count == 2 && explicit_count == 1);
    CHECK(anonymous_handles[0] != anonymous_handles[1]);
    {
        boxrig_image_view image;

        CHECK(boxrig_terminal_get_image(terminal, old_handle, &image) ==
              BOXRIG_STATUS_INVALID_ARGUMENT);
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_image_numbers_assign_monotonic_ids(void)
{
    static const uint8_t input[] =
        "\x1b_Ga=t,i=1,f=24,s=1,v=1;AQID\x1b\\"
        "\x1b_Ga=t,I=7,f=24,s=1,v=1;BAUG\x1b\\"
        "\x1b_Ga=t,I=7,f=24,s=1,v=1;BwgJ\x1b\\"
        "\x1b_Ga=t,I=0,f=24,s=1,v=1;CgsM\x1b\\";
    static const uint8_t expected[] =
        "\x1b_Gi=1;OK\x1b\\"
        "\x1b_Gi=2,I=7;OK\x1b\\"
        "\x1b_Gi=3,I=7;OK\x1b\\"
        "\x1b_Gi=4,I=0;OK\x1b\\";
    static const uint8_t expected_red[] = {1, 4, 7, 10};
    static const uint32_t expected_number[] = {0, 7, 7, 0};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    const uint8_t *output = NULL;
    bool found[4] = {false, false, false, false};
    size_t consumed = 0;
    size_t index;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              input,
              sizeof(input) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(boxrig_terminal_image_count(terminal) == 4);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, sizeof(expected) - 1) == 0);
    for (index = 0; index < boxrig_terminal_image_count(terminal); index++) {
        boxrig_image_handle handle = 0;
        boxrig_image_view image;
        size_t assigned_index;

        CHECK(boxrig_terminal_image_handle_at(terminal, index, &handle) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
              BOXRIG_STATUS_OK);
        CHECK(image.image_id >= 1 && image.image_id <= 4);
        assigned_index = image.image_id - 1;
        CHECK(!found[assigned_index]);
        CHECK(image.image_number == expected_number[assigned_index]);
        CHECK(image.rgba_length == 4);
        CHECK(image.rgba[0] == expected_red[assigned_index]);
        found[assigned_index] = true;
    }
    for (index = 0; index < sizeof(found) / sizeof(found[0]); index++) {
        CHECK(found[index]);
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_image_capacity_and_replacement_are_atomic(void)
{
    static const uint8_t initial[] =
        "\x1b_Ga=t,i=10,f=24,s=1,v=1;AQID\x1b\\"
        "\x1b_Ga=t,i=11,f=24,s=1,v=1;BAUG\x1b\\";
    static const uint8_t initial_response[] =
        "\x1b_Gi=10;OK\x1b\\"
        "\x1b_Gi=11;OK\x1b\\";
    static const uint8_t rejected[] =
        "\x1b_Ga=t,i=12,f=24,s=1,v=1;BwgJ\x1b\\";
    static const uint8_t rejected_response[] =
        "\x1b_Gi=12;ENOSPC:graphics capacity exceeded\x1b\\";
    static const uint8_t replacement[] =
        "\x1b_Ga=t,i=10,f=24,s=1,v=1;CgsM\x1b\\";
    static const uint8_t replacement_response[] = "\x1b_Gi=10;OK\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_handle old_ten = 0;
    boxrig_image_handle old_eleven = 0;
    const uint8_t *output = NULL;
    uint64_t revision;
    size_t consumed = 0;
    size_t index;

    config.rows = 1;
    config.columns = 1;
    config.graphics_storage_capacity = 12;
    config.graphics_image_capacity = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              initial,
              sizeof(initial) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(initial) - 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(initial_response) - 1);
    CHECK(memcmp(output, initial_response, sizeof(initial_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(initial_response) - 1) == BOXRIG_STATUS_OK);
    for (index = 0; index < 2; index++) {
        boxrig_image_handle handle = 0;
        boxrig_image_view image;

        CHECK(boxrig_terminal_image_handle_at(terminal, index, &handle) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
              BOXRIG_STATUS_OK);
        if (image.image_id == 10) {
            old_ten = handle;
        } else if (image.image_id == 11) {
            old_eleven = handle;
        }
    }
    CHECK(old_ten != 0 && old_eleven != 0);
    revision = boxrig_terminal_image_revision(terminal);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              rejected,
              sizeof(rejected) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(rejected) - 1);
    CHECK(boxrig_terminal_image_count(terminal) == 2);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(rejected_response) - 1);
    CHECK(memcmp(output, rejected_response, sizeof(rejected_response) - 1) == 0);
    {
        boxrig_image_view image;

        CHECK(boxrig_terminal_get_image(terminal, old_ten, &image) ==
              BOXRIG_STATUS_OK);
        CHECK(image.rgba[0] == 1);
        CHECK(boxrig_terminal_get_image(terminal, old_eleven, &image) ==
              BOXRIG_STATUS_OK);
        CHECK(image.rgba[0] == 4);
    }
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(rejected_response) - 1) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              replacement,
              sizeof(replacement) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(replacement) - 1);
    CHECK(boxrig_terminal_image_count(terminal) == 2);
    CHECK(boxrig_terminal_image_revision(terminal) != revision);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(replacement_response) - 1);
    CHECK(memcmp(
              output,
              replacement_response,
              sizeof(replacement_response) - 1) == 0);
    {
        boxrig_image_view image;
        bool found_replacement = false;

        CHECK(boxrig_terminal_get_image(terminal, old_ten, &image) ==
              BOXRIG_STATUS_INVALID_ARGUMENT);
        CHECK(boxrig_terminal_get_image(terminal, old_eleven, &image) ==
              BOXRIG_STATUS_OK);
        CHECK(image.rgba[0] == 4);
        for (index = 0; index < 2; index++) {
            boxrig_image_handle handle = 0;

            CHECK(boxrig_terminal_image_handle_at(terminal, index, &handle) ==
                  BOXRIG_STATUS_OK);
            CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
                  BOXRIG_STATUS_OK);
            if (image.image_id == 10) {
                CHECK(handle != old_ten && image.rgba[0] == 10);
                found_replacement = true;
            }
        }
        CHECK(found_replacement);
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_storage_eviction_and_backpressure_are_atomic(void)
{
    static const uint8_t initial[] =
        "\x1b_Ga=t,i=21,f=24,s=1,v=1;AQID\x1b\\"
        "\x1b_Ga=t,i=22,N=1,f=24,s=1,v=1;BAUG\x1b\\"
        "\x1b_Ga=t,i=23,N=2,f=24,s=1,v=1;BwgJ\x1b\\";
    static const uint8_t initial_response[] =
        "\x1b_Gi=21;OK\x1b\\"
        "\x1b_Gi=22;OK\x1b\\"
        "\x1b_Gi=23;OK\x1b\\";
    static const uint8_t evicting[] =
        "\x1b_Ga=t,i=24,f=32,s=2,v=1;CQoLDA0ODxA=\x1b\\";
    static const uint8_t evicting_response[] = "\x1b_Gi=24;OK\x1b\\";
    static const uint8_t same_size_replacement[] =
        "\x1b_Ga=t,i=23,f=24,s=1,v=1;DQ4P\x1b\\";
    static const uint8_t same_size_response[] = "\x1b_Gi=23;OK\x1b\\";
    static const uint8_t oversized[] =
        "\x1b_Ga=t,i=23,f=32,s=4,v=1;"
        "AQEBAQICAgIDAwMDBAQEBA==\x1b\\";
    static const uint8_t oversized_response[] =
        "\x1b_Gi=23;ENOSPC:graphics capacity exceeded\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_handle handles[3] = {0, 0, 0};
    const uint8_t *output = NULL;
    uint64_t revision;
    size_t consumed = 0;
    size_t index;

    config.rows = 1;
    config.columns = 1;
    config.graphics_storage_capacity = 12;
    config.graphics_image_capacity = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              initial,
              sizeof(initial) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(initial) - 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(initial_response) - 1);
    CHECK(memcmp(output, initial_response, sizeof(initial_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(initial_response) - 1) == BOXRIG_STATUS_OK);
    for (index = 0; index < 3; index++) {
        boxrig_image_handle handle = 0;
        boxrig_image_view image;

        CHECK(boxrig_terminal_image_handle_at(terminal, index, &handle) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
              BOXRIG_STATUS_OK);
        CHECK(image.image_id >= 21 && image.image_id <= 23);
        handles[image.image_id - 21] = handle;
    }
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              evicting,
              sizeof(evicting) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(evicting) - 1);
    CHECK(boxrig_terminal_image_count(terminal) == 2);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(evicting_response) - 1);
    CHECK(memcmp(output, evicting_response, sizeof(evicting_response) - 1) == 0);
    {
        boxrig_image_view image;

        CHECK(boxrig_terminal_get_image(terminal, handles[0], &image) ==
              BOXRIG_STATUS_INVALID_ARGUMENT);
        CHECK(boxrig_terminal_get_image(terminal, handles[1], &image) ==
              BOXRIG_STATUS_INVALID_ARGUMENT);
        CHECK(boxrig_terminal_get_image(terminal, handles[2], &image) ==
              BOXRIG_STATUS_OK);
        CHECK(image.image_id == 23 && image.rgba[0] == 7);
    }
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(evicting_response) - 1) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              same_size_replacement,
              sizeof(same_size_replacement) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(same_size_replacement) - 1);
    CHECK(boxrig_terminal_image_count(terminal) == 2);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(same_size_response) - 1);
    CHECK(memcmp(output, same_size_response, sizeof(same_size_response) - 1) ==
          0);
    {
        boxrig_image_view image;
        bool found_replacement = false;

        CHECK(boxrig_terminal_get_image(terminal, handles[2], &image) ==
              BOXRIG_STATUS_INVALID_ARGUMENT);
        for (index = 0; index < 2; index++) {
            boxrig_image_handle handle = 0;

            CHECK(boxrig_terminal_image_handle_at(terminal, index, &handle) ==
                  BOXRIG_STATUS_OK);
            CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
                  BOXRIG_STATUS_OK);
            if (image.image_id == 23) {
                CHECK(image.rgba[0] == 13);
                handles[2] = handle;
                found_replacement = true;
            } else {
                CHECK(image.image_id == 24);
            }
        }
        CHECK(found_replacement);
    }
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(same_size_response) - 1) == BOXRIG_STATUS_OK);
    revision = boxrig_terminal_image_revision(terminal);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              oversized,
              sizeof(oversized) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(oversized) - 1);
    CHECK(boxrig_terminal_image_count(terminal) == 2);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(oversized_response) - 1);
    CHECK(memcmp(output, oversized_response, sizeof(oversized_response) - 1) ==
          0);
    {
        boxrig_image_view image;

        CHECK(boxrig_terminal_get_image(terminal, handles[2], &image) ==
              BOXRIG_STATUS_OK);
        CHECK(image.image_id == 23 && image.rgba[0] == 13);
    }
    boxrig_terminal_destroy(terminal);

    {
        static const uint8_t first[] =
            "\x1b_Ga=t,i=31,f=24,s=1,v=1;AQID\x1b\\";
        static const uint8_t second[] =
            "\x1b_Ga=t,i=32,N=1,f=24,s=1,v=1;BAUG\x1b\\";
        static const uint8_t candidate[] =
            "\x1b_Ga=t,i=33,f=24,s=1,v=1;BwgJ\x1b\\";
        static const uint8_t response[] = "\x1b_Gi=33;OK\x1b\\";
        boxrig_image_handle first_handle = 0;
        boxrig_image_handle second_handle = 0;
        uint8_t fill[sizeof(response) - 1];
        size_t remaining;

        terminal = NULL;
        config.graphics_storage_capacity = 8;
        config.output_capacity = sizeof(fill);
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  first,
                  sizeof(first) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_output_consume(terminal, sizeof(response) - 1) ==
              BOXRIG_STATUS_OK);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  second,
                  sizeof(second) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_output_consume(terminal, sizeof(response) - 1) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_image_handle_at(terminal, 0, &first_handle) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_image_handle_at(terminal, 1, &second_handle) ==
              BOXRIG_STATUS_OK);
        revision = boxrig_terminal_image_revision(terminal);
        memset(fill, 'x', sizeof(fill));
        CHECK(boxrig_terminal_send_text(
                  terminal,
                  (const char *)fill,
                  sizeof(fill)) == BOXRIG_STATUS_OK);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  candidate,
                  sizeof(candidate) - 1,
                  &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
        CHECK(consumed < sizeof(candidate) - 1);
        CHECK(candidate[consumed] == (uint8_t)'\\');
        CHECK(boxrig_terminal_image_count(terminal) == 2);
        CHECK(boxrig_terminal_image_revision(terminal) == revision);
        {
            boxrig_image_view image;

            CHECK(boxrig_terminal_get_image(terminal, first_handle, &image) ==
                  BOXRIG_STATUS_OK);
            CHECK(image.image_id == 31);
            CHECK(boxrig_terminal_get_image(terminal, second_handle, &image) ==
                  BOXRIG_STATUS_OK);
            CHECK(image.image_id == 32);
        }
        CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(fill));
        CHECK(memcmp(output, fill, sizeof(fill)) == 0);
        CHECK(boxrig_terminal_output_consume(terminal, sizeof(fill)) ==
              BOXRIG_STATUS_OK);
        remaining = sizeof(candidate) - 1 - consumed;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  candidate + consumed,
                  remaining,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == remaining);
        CHECK(boxrig_terminal_image_count(terminal) == 2);
        CHECK(boxrig_terminal_image_revision(terminal) != revision);
        {
            boxrig_image_view image;
            bool found_thirty_three = false;

            CHECK(boxrig_terminal_get_image(terminal, first_handle, &image) ==
                  BOXRIG_STATUS_OK);
            CHECK(image.image_id == 31);
            CHECK(boxrig_terminal_get_image(terminal, second_handle, &image) ==
                  BOXRIG_STATUS_INVALID_ARGUMENT);
            for (index = 0; index < 2; index++) {
                boxrig_image_handle handle = 0;

                CHECK(boxrig_terminal_image_handle_at(terminal, index, &handle) ==
                      BOXRIG_STATUS_OK);
                CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
                      BOXRIG_STATUS_OK);
                if (image.image_id == 33) {
                    CHECK(image.rgba[0] == 7);
                    found_thirty_three = true;
                }
            }
            CHECK(found_thirty_three);
        }
        CHECK(boxrig_terminal_output_peek(terminal, &output) ==
              sizeof(response) - 1);
        CHECK(memcmp(output, response, sizeof(response) - 1) == 0);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_normal_placement_crop_scale_offset_and_dirty(void)
{
    static const uint8_t upload[] =
        "\x1b_Ga=t,i=501,f=24,s=4,v=3;"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\x1b\\";
    static const uint8_t upload_response[] = "\x1b_Gi=501;OK\x1b\\";
    static const uint8_t place[] =
        "\x1b_Ga=p,i=501,p=7,x=1,y=1,w=10,h=2,X=3,Y=4,c=2,"
        "C=1,z=-5\x1b\\";
    static const uint8_t place_response[] =
        "\x1b_Gi=501,p=7;OK\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_placement_view placement;
    boxrig_cursor_view cursor;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    size_t consumed = 0;

    config.rows = 6;
    config.columns = 10;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 8, 10) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[2;3H",
              sizeof("\x1b[2;3H") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
    }
    CHECK(boxrig_terminal_feed(
              terminal,
              upload,
              sizeof(upload) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(upload_response) - 1);
    CHECK(memcmp(output, upload_response, sizeof(upload_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(upload_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              place,
              sizeof(place) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(place) - 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(place_response) - 1);
    CHECK(memcmp(output, place_response, sizeof(place_response) - 1) == 0);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.image_handle != 0 && placement.revision != 0);
    CHECK(placement.resource_creation_serial != 0 &&
          placement.placement_creation_serial != 0);
    CHECK(placement.kind == BOXRIG_IMAGE_PLACEMENT_NORMAL);
    CHECK(placement.image_id == 501 && placement.image_number == 0);
    CHECK(placement.placement_id == 7);
    CHECK(placement.anchor_row == 1 && placement.anchor_column == 2);
    CHECK(placement.visual_row == 1 && placement.visual_column == 2);
    CHECK(placement.source_x == 1 && placement.source_y == 1);
    CHECK(placement.source_width == 3 && placement.source_height == 2);
    CHECK(placement.cell_x_offset == 3 && placement.cell_y_offset == 4);
    CHECK(placement.columns == 2 && placement.rows == 2);
    CHECK(placement.destination_pixel_width == 16);
    CHECK(placement.destination_pixel_height == 11);
    CHECK(placement.clip_x == 0 && placement.clip_y == 0);
    CHECK(placement.clip_width == 16 && placement.clip_height == 11);
    CHECK(placement.z_index == -5);
    CHECK(boxrig_terminal_get_image_placement(terminal, 1, &placement) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 1 && cursor.column == 2);
    CHECK(!boxrig_terminal_take_dirty(terminal, &dirty));
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(dirty.row == 1 && dirty.column == 2 && dirty.column_count == 3);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(dirty.row == 2 && dirty.column == 2 && dirty.column_count == 3);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_normal_placement_scaling_validation_and_geometry_change(void)
{
    static const uint8_t upload[] =
        "\x1b_Ga=t,i=510,f=24,s=5,v=3;"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "\x1b\\";
    static const uint8_t upload_response[] = "\x1b_Gi=510;OK\x1b\\";
    static const uint8_t placements[] =
        "\x1b_Ga=p,i=510,p=1,c=2,r=3,C=1\x1b\\"
        "\x1b_Ga=p,i=510,p=2,c=2,C=1\x1b\\"
        "\x1b_Ga=p,i=510,p=3,r=2,C=1\x1b\\"
        "\x1b_Ga=p,i=510,p=4,C=1\x1b\\";
    static const uint8_t placement_responses[] =
        "\x1b_Gi=510,p=1;OK\x1b\\"
        "\x1b_Gi=510,p=2;OK\x1b\\"
        "\x1b_Gi=510,p=3;OK\x1b\\"
        "\x1b_Gi=510,p=4;OK\x1b\\";
    static const uint8_t invalid_offset[] =
        "\x1b_Ga=p,i=510,p=5,X=4,C=1\x1b\\";
    static const uint8_t invalid_offset_response[] =
        "\x1b_Gi=510,p=5;EINVAL:invalid command\x1b\\";
    static const uint8_t empty_crop[] =
        "\x1b_Ga=p,i=510,p=6,x=5,C=1\x1b\\";
    static const uint8_t empty_crop_response[] =
        "\x1b_Gi=510,p=6;EINVAL:invalid command\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_placement_view placement;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    uint64_t revision;
    size_t consumed = 0;

    config.rows = 10;
    config.columns = 10;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 4, 6) ==
          BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
    }
    CHECK(boxrig_terminal_feed(
              terminal,
              upload,
              sizeof(upload) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(upload_response) - 1);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(upload_response) - 1) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              placements,
              sizeof(placements) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(placements) - 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(placement_responses) - 1);
    CHECK(memcmp(
              output,
              placement_responses,
              sizeof(placement_responses) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(placement_responses) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 4);

    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.columns == 2 && placement.rows == 3);
    CHECK(placement.destination_pixel_width == 8 &&
          placement.destination_pixel_height == 18);
    CHECK(boxrig_terminal_get_image_placement(terminal, 1, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.columns == 2 && placement.rows == 1);
    CHECK(placement.destination_pixel_width == 8 &&
          placement.destination_pixel_height == 5);
    CHECK(boxrig_terminal_get_image_placement(terminal, 2, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.columns == 5 && placement.rows == 2);
    CHECK(placement.destination_pixel_width == 20 &&
          placement.destination_pixel_height == 12);
    CHECK(boxrig_terminal_get_image_placement(terminal, 3, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.columns == 2 && placement.rows == 1);
    CHECK(placement.destination_pixel_width == 5 &&
          placement.destination_pixel_height == 3);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    revision = boxrig_terminal_image_revision(terminal);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              invalid_offset,
              sizeof(invalid_offset) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(invalid_offset_response) - 1);
    CHECK(memcmp(
              output,
              invalid_offset_response,
              sizeof(invalid_offset_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(invalid_offset_response) - 1) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              empty_crop,
              sizeof(empty_crop) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(empty_crop_response) - 1);
    CHECK(memcmp(
              output,
              empty_crop_response,
              sizeof(empty_crop_response) - 1) == 0);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 4);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));

    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 8, 12) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_revision(terminal) != revision);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.destination_pixel_width == 16 &&
          placement.destination_pixel_height == 36);
    CHECK(boxrig_terminal_get_image_placement(terminal, 1, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.destination_pixel_width == 16 &&
          placement.destination_pixel_height == 10);
    CHECK(boxrig_terminal_get_image_placement(terminal, 2, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.destination_pixel_width == 40 &&
          placement.destination_pixel_height == 24);
    CHECK(boxrig_terminal_get_image_placement(terminal, 3, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.columns == 1 && placement.rows == 1);
    CHECK(placement.destination_pixel_width == 5 &&
          placement.destination_pixel_height == 3);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_transmit_and_place_geometry_cursor_and_atomic_commit(void)
{
    static const uint8_t unavailable[] =
        "\x1b_Ga=T,i=601,p=3,f=24,s=1,v=1,c=2,r=1;AQID\x1b\\";
    static const uint8_t unavailable_response[] =
        "\x1b_Gi=601,p=3;ENOTSUP:cell geometry unavailable\x1b\\";
    static const uint8_t combined[] =
        "\x1b_Ga=T,i=601,p=3,f=24,s=1,v=1,c=2,r=1;AQID\x1b\\";
    static const uint8_t combined_response[] =
        "\x1b_Gi=601,p=3;OK\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_placement_view placement;
    boxrig_cursor_view cursor;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    size_t consumed = 0;

    config.rows = 4;
    config.columns = 6;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
    }
    CHECK(boxrig_terminal_feed(
              terminal,
              unavailable,
              sizeof(unavailable) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(unavailable_response) - 1);
    CHECK(memcmp(
              output,
              unavailable_response,
              sizeof(unavailable_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(unavailable_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_count(terminal) == 0);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0 && cursor.column == 0);

    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 8, 16) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[2;2H",
              sizeof("\x1b[2;2H") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
    }
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              combined,
              sizeof(combined) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(combined) - 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(combined_response) - 1);
    CHECK(memcmp(output, combined_response, sizeof(combined_response) - 1) ==
          0);
    CHECK(boxrig_terminal_image_count(terminal) == 1);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.anchor_row == 1 && placement.anchor_column == 1);
    CHECK(placement.columns == 2 && placement.rows == 1);
    CHECK(placement.destination_pixel_width == 16 &&
          placement.destination_pixel_height == 16);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 2 && cursor.column == 3);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(dirty.row == 1 && dirty.column == 1 && dirty.column_count == 2);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(boxrig_terminal_take_dirty(terminal, &dirty));
    CHECK(dirty.row == 1 && dirty.column == 1 && dirty.column_count == 1);
    CHECK(boxrig_terminal_take_dirty(terminal, &dirty));
    CHECK(dirty.row == 2 && dirty.column == 3 && dirty.column_count == 1);
    CHECK(!boxrig_terminal_take_dirty(terminal, &dirty));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_placement_number_replacement_and_capacity(void)
{
    static const uint8_t uploads[] =
        "\x1b_Ga=t,I=77,f=24,s=1,v=1;AQID\x1b\\"
        "\x1b_Ga=t,I=77,f=24,s=1,v=1;BAUG\x1b\\";
    static const uint8_t upload_responses[] =
        "\x1b_Gi=1,I=77;OK\x1b\\"
        "\x1b_Gi=2,I=77;OK\x1b\\";
    static const uint8_t first[] =
        "\x1b_Ga=p,I=77,p=9,c=1,r=1,C=1\x1b\\";
    static const uint8_t first_response[] =
        "\x1b_Gi=2,I=77,p=9;OK\x1b\\";
    static const uint8_t replacement[] =
        "\x1b_Ga=p,I=77,p=9,X=1,c=2,r=1,C=1\x1b\\";
    static const uint8_t replacement_response[] =
        "\x1b_Gi=2,I=77,p=9;OK\x1b\\";
    static const uint8_t anonymous_placements[] =
        "\x1b_Ga=p,I=77,p=0,C=1\x1b\\"
        "\x1b_Ga=p,I=77,p=0,C=1\x1b\\";
    static const uint8_t anonymous_responses[] =
        "\x1b_Gi=2,I=77,p=0;OK\x1b\\"
        "\x1b_Gi=2,I=77,p=0;OK\x1b\\";
    static const uint8_t over_capacity[] =
        "\x1b_Ga=p,I=77,p=10,C=1\x1b\\";
    static const uint8_t over_capacity_response[] =
        "\x1b_Gi=2,I=77,p=10;ENOSPC:graphics capacity exceeded\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_placement_view placement;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    uint64_t revision;
    size_t consumed = 0;

    config.rows = 4;
    config.columns = 6;
    config.graphics_placement_capacity = 3;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 8, 8) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              uploads,
              sizeof(uploads) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(upload_responses) - 1);
    CHECK(memcmp(output, upload_responses, sizeof(upload_responses) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(upload_responses) - 1) == BOXRIG_STATUS_OK);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              first,
              sizeof(first) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(first_response) - 1);
    CHECK(memcmp(output, first_response, sizeof(first_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(first_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.image_id == 2 && placement.image_number == 77 &&
          placement.placement_id == 9);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    revision = boxrig_terminal_image_revision(terminal);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              replacement,
              sizeof(replacement) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(replacement_response) - 1);
    CHECK(memcmp(
              output,
              replacement_response,
              sizeof(replacement_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(replacement_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_image_revision(terminal) != revision);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.placement_id == 9 && placement.cell_x_offset == 1);
    CHECK(placement.columns == 2 && placement.rows == 1);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(dirty.row == 0 && dirty.column == 0 && dirty.column_count == 3);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              anonymous_placements,
              sizeof(anonymous_placements) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(anonymous_responses) - 1);
    CHECK(memcmp(
              output,
              anonymous_responses,
              sizeof(anonymous_responses) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(anonymous_responses) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 3);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    revision = boxrig_terminal_image_revision(terminal);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              over_capacity,
              sizeof(over_capacity) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(over_capacity_response) - 1);
    CHECK(memcmp(
              output,
              over_capacity_response,
              sizeof(over_capacity_response) - 1) == 0);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 3);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_placement_backpressure_retry_is_atomic(void)
{
    static const uint8_t upload[] =
        "\x1b_Ga=t,i=801,f=24,s=1,v=1;AQID\x1b\\";
    static const uint8_t upload_response[] = "\x1b_Gi=801;OK\x1b\\";
    static const uint8_t place[] = "\x1b_Ga=p,i=801,p=5\x1b\\";
    static const uint8_t place_response[] = "\x1b_Gi=801,p=5;OK\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cursor_view cursor;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    uint8_t fill[sizeof(place_response) - 1];
    uint64_t revision;
    size_t consumed = 0;
    size_t remaining;

    config.rows = 5;
    config.columns = 5;
    config.output_capacity = sizeof(fill);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 8, 8) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              upload,
              sizeof(upload) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(upload_response) - 1);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(upload_response) - 1) == BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
    }
    memset(fill, 'x', sizeof(fill));
    CHECK(boxrig_terminal_send_text(
              terminal,
              (const char *)fill,
              sizeof(fill)) == BOXRIG_STATUS_OK);
    revision = boxrig_terminal_image_revision(terminal);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              place,
              sizeof(place) - 1,
              &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed < sizeof(place) - 1);
    CHECK(place[consumed] == (uint8_t)'\\');
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0 && cursor.column == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(fill));
    CHECK(memcmp(output, fill, sizeof(fill)) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(fill)) ==
          BOXRIG_STATUS_OK);

    remaining = sizeof(place) - 1 - consumed;
    CHECK(boxrig_terminal_feed(
              terminal,
              place + consumed,
              remaining,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == remaining);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_image_revision(terminal) != revision);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 1 && cursor.column == 1);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(dirty.row == 0 && dirty.column == 0 && dirty.column_count == 1);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(place_response) - 1);
    CHECK(memcmp(output, place_response, sizeof(place_response) - 1) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_placement_resource_replacement_and_eviction_dirty(void)
{
    static const uint8_t initial[] =
        "\x1b_Ga=t,i=901,f=24,s=1,v=1;AQID\x1b\\"
        "\x1b_Ga=t,i=902,N=1,f=24,s=1,v=1;BAUG\x1b\\";
    static const uint8_t initial_responses[] =
        "\x1b_Gi=901;OK\x1b\\"
        "\x1b_Gi=902;OK\x1b\\";
    static const uint8_t place_901[] =
        "\x1b_Ga=p,i=901,p=1,C=1\x1b\\";
    static const uint8_t place_901_response[] =
        "\x1b_Gi=901,p=1;OK\x1b\\";
    static const uint8_t replace_901[] =
        "\x1b_Ga=t,i=901,f=24,s=1,v=1;BwgJ\x1b\\";
    static const uint8_t replace_901_response[] = "\x1b_Gi=901;OK\x1b\\";
    static const uint8_t place_902[] =
        "\x1b_Ga=p,i=902,p=2,C=1\x1b\\";
    static const uint8_t place_902_response[] =
        "\x1b_Gi=902,p=2;OK\x1b\\";
    static const uint8_t upload_903[] =
        "\x1b_Ga=t,i=903,f=24,s=1,v=1;CgsM\x1b\\";
    static const uint8_t upload_903_response[] = "\x1b_Gi=903;OK\x1b\\";
    static const uint8_t upload_904[] =
        "\x1b_Ga=t,i=904,f=32,s=2,v=1;AQIDBAUGBwg=\x1b\\";
    static const uint8_t upload_904_response[] = "\x1b_Gi=904;OK\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_handle handle;
    boxrig_image_view image;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t index;
    bool found_901;
    bool found_902;

    config.rows = 3;
    config.columns = 4;
    config.graphics_storage_capacity = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 8, 8) ==
          BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
    }
    CHECK(boxrig_terminal_feed(
              terminal,
              initial,
              sizeof(initial) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(initial_responses) - 1);
    CHECK(memcmp(output, initial_responses, sizeof(initial_responses) - 1) ==
          0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(initial_responses) - 1) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              place_901,
              sizeof(place_901) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(place_901_response) - 1);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(place_901_response) - 1) == BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              replace_901,
              sizeof(replace_901) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(replace_901_response) - 1);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(replace_901_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(dirty.row == 0 && dirty.column == 0 && dirty.column_count == 1);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(!boxrig_terminal_take_dirty(terminal, &dirty));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              place_902,
              sizeof(place_902) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(place_902_response) - 1);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(place_902_response) - 1) == BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              upload_903,
              sizeof(upload_903) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(upload_903_response) - 1);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(upload_903_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    found_901 = false;
    found_902 = false;
    for (index = 0; index < boxrig_terminal_image_count(terminal); index++) {
        CHECK(boxrig_terminal_image_handle_at(terminal, index, &handle) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
              BOXRIG_STATUS_OK);
        found_901 = found_901 || image.image_id == 901;
        found_902 = found_902 || image.image_id == 902;
    }
    CHECK(!found_901 && found_902);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              upload_904,
              sizeof(upload_904) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(upload_904_response) - 1);
    CHECK(memcmp(output, upload_904_response, sizeof(upload_904_response) - 1) ==
          0);
    CHECK(boxrig_terminal_image_count(terminal) == 1);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(image.image_id == 904);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(dirty.row == 0 && dirty.column == 0 && dirty.column_count == 1);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_delete_identity_lifetime_and_backpressure(void)
{
    static const uint8_t initialize[] =
        "\x1b_Ga=t,i=101,f=24,s=1,v=1;AQID\x1b\\"
        "\x1b_Ga=t,i=102,f=24,s=1,v=1;BAUG\x1b\\"
        "\x1b_Ga=p,i=101,p=1,C=1\x1b\\"
        "\x1b[1;3H\x1b_Ga=p,i=101,p=2,C=1\x1b\\"
        "\x1b[2;1H\x1b_Ga=p,i=102,p=1,C=1\x1b\\";
    static const uint8_t initialize_responses[] =
        "\x1b_Gi=101;OK\x1b\\"
        "\x1b_Gi=102;OK\x1b\\"
        "\x1b_Gi=101,p=1;OK\x1b\\"
        "\x1b_Gi=101,p=2;OK\x1b\\"
        "\x1b_Gi=102,p=1;OK\x1b\\";
    static const uint8_t delete_pair[] =
        "\x1b_Ga=d,d=i,i=101,p=1\x1b\\";
    static const uint8_t delete_pair_response[] =
        "\x1b_Gi=101,p=1;OK\x1b\\";
    static const uint8_t delete_remaining[] =
        "\x1b_Ga=d,d=i,i=101\x1b\\";
    static const uint8_t delete_remaining_response[] =
        "\x1b_Gi=101;OK\x1b\\";
    static const uint8_t free_101[] =
        "\x1b_Ga=d,d=I,i=101\x1b\\";
    static const uint8_t free_101_response[] =
        "\x1b_Gi=101;OK\x1b\\";
    static const uint8_t numbered[] =
        "\x1b_Ga=t,I=77,f=24,s=1,v=1;BwgJ\x1b\\"
        "\x1b_Ga=t,I=77,f=24,s=1,v=1;CgsM\x1b\\"
        "\x1b_Ga=p,I=77,p=5,C=1\x1b\\";
    static const uint8_t numbered_responses[] =
        "\x1b_Gi=1,I=77;OK\x1b\\"
        "\x1b_Gi=2,I=77;OK\x1b\\"
        "\x1b_Gi=2,I=77,p=5;OK\x1b\\";
    static const uint8_t delete_number_pair[] =
        "\x1b_Ga=d,d=n,I=77,p=5\x1b\\";
    static const uint8_t delete_number_pair_response[] =
        "\x1b_Gi=2,I=77,p=5;OK\x1b\\";
    static const uint8_t free_latest_number[] =
        "\x1b_Ga=d,d=N,I=77\x1b\\";
    static const uint8_t free_latest_number_response[] =
        "\x1b_Gi=2,I=77;OK\x1b\\";
    static const uint8_t place_older_number[] =
        "\x1b_Ga=p,I=77,p=6,C=1\x1b\\";
    static const uint8_t place_older_number_response[] =
        "\x1b_Gi=1,I=77,p=6;OK\x1b\\";
    static const uint8_t free_range[] =
        "\x1b_Ga=d,d=R,x=1,y=102\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    uint8_t fill[256];
    uint64_t revision;
    size_t consumed = 0;
    size_t remaining;

    config.rows = 4;
    config.columns = 6;
    config.output_capacity = sizeof(fill);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              initialize,
              sizeof(initialize) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(initialize_responses) - 1);
    CHECK(memcmp(
              output,
              initialize_responses,
              sizeof(initialize_responses) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(initialize_responses) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_count(terminal) == 2);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 3);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    memset(fill, 'x', sizeof(fill));
    CHECK(boxrig_terminal_send_text(
              terminal,
              (const char *)fill,
              sizeof(fill)) == BOXRIG_STATUS_OK);
    revision = boxrig_terminal_image_revision(terminal);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              delete_pair,
              sizeof(delete_pair) - 1,
              &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed < sizeof(delete_pair) - 1);
    CHECK(delete_pair[consumed] == (uint8_t)'\\');
    CHECK(boxrig_terminal_image_count(terminal) == 2);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 3);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(fill)) ==
          BOXRIG_STATUS_OK);
    remaining = sizeof(delete_pair) - 1 - consumed;
    CHECK(boxrig_terminal_feed(
              terminal,
              delete_pair + consumed,
              remaining,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == remaining);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(delete_pair_response) - 1);
    CHECK(memcmp(
              output,
              delete_pair_response,
              sizeof(delete_pair_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(delete_pair_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_count(terminal) == 2);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 2);
    CHECK(boxrig_terminal_image_revision(terminal) != revision);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(dirty.row == 0 && dirty.column == 0 && dirty.column_count == 1);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              delete_remaining,
              sizeof(delete_remaining) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(delete_remaining_response) - 1);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(delete_remaining_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_count(terminal) == 2);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              free_101,
              sizeof(free_101) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(free_101_response) - 1);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(free_101_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_count(terminal) == 1);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              numbered,
              sizeof(numbered) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(numbered_responses) - 1);
    CHECK(memcmp(
              output,
              numbered_responses,
              sizeof(numbered_responses) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(numbered_responses) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_count(terminal) == 3);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 2);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              delete_number_pair,
              sizeof(delete_number_pair) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(delete_number_pair_response) - 1);
    CHECK(memcmp(
              output,
              delete_number_pair_response,
              sizeof(delete_number_pair_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(delete_number_pair_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_count(terminal) == 3);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              free_latest_number,
              sizeof(free_latest_number) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(free_latest_number_response) - 1);
    CHECK(memcmp(
              output,
              free_latest_number_response,
              sizeof(free_latest_number_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(free_latest_number_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_count(terminal) == 2);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              place_older_number,
              sizeof(place_older_number) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(place_older_number_response) - 1);
    CHECK(memcmp(
              output,
              place_older_number_response,
              sizeof(place_older_number_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(place_older_number_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 2);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              free_range,
              sizeof(free_range) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_terminal_image_count(terminal) == 0);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_delete_location_selectors(void)
{
    struct fixture {
        const char *setup;
        const char *command;
        size_t placement_count;
        size_t image_count;
    } fixtures[] = {
        {"", "\x1b_Ga=d,d=a,i=900\x1b\\", 0, 2},
        {"", "\x1b_Ga=d,d=A,i=900\x1b\\", 0, 0},
        {"\x1b[2;2H", "\x1b_Ga=d,d=c,i=900\x1b\\", 1, 2},
        {"", "\x1b_Ga=d,d=p,i=900,x=2,y=2\x1b\\", 1, 2},
        {"", "\x1b_Ga=d,d=q,i=900,x=3,y=3,z=5\x1b\\", 1, 2},
        {"", "\x1b_Ga=d,d=x,i=900,x=3\x1b\\", 1, 2},
        {"", "\x1b_Ga=d,d=y,i=900,y=3\x1b\\", 1, 2},
        {"", "\x1b_Ga=d,d=z,i=900,z=5\x1b\\", 1, 2},
        {"", "\x1b_Ga=d,d=P,i=900,x=2,y=2\x1b\\", 1, 1}
    };
    static const uint8_t initialize[] =
        "\x1b[2;2H"
        "\x1b_Ga=T,i=301,p=1,f=24,s=1,v=1,c=2,r=2,z=5,C=1;AQID\x1b\\"
        "\x1b[5;5H"
        "\x1b_Ga=T,i=302,p=1,f=24,s=1,v=1,c=1,r=1,z=-1,C=1;BAUG\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    size_t index;

    config.rows = 6;
    config.columns = 6;
    for (index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]); index++) {
        boxrig_terminal *terminal = NULL;
        boxrig_dirty dirty;
        const uint8_t *output = NULL;
        size_t consumed = 0;
        size_t output_length;

        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  initialize,
                  sizeof(initialize) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        output_length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(output_length != 0);
        CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
              BOXRIG_STATUS_OK);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)fixtures[index].setup,
                  strlen(fixtures[index].setup),
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == strlen(fixtures[index].setup));
        while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
        }
        while (boxrig_terminal_take_dirty(terminal, &dirty)) {
        }
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)fixtures[index].command,
                  strlen(fixtures[index].command),
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == strlen(fixtures[index].command));
        CHECK(boxrig_terminal_image_placement_count(terminal) ==
              fixtures[index].placement_count);
        CHECK(boxrig_terminal_image_count(terminal) ==
              fixtures[index].image_count);
        CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
        CHECK(!boxrig_terminal_take_dirty(terminal, &dirty));
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_screen_history_viewport_and_erase_lifecycle(void)
{
    static const uint8_t upload_and_place[] =
        "\x1b_Ga=T,i=401,p=1,f=24,s=1,v=1,C=1;AQID\x1b\\";
    static const uint8_t place_live[] =
        "\x1b[2;2H\x1b_Ga=p,i=401,p=2,C=1\x1b\\";
    static const uint8_t place_for_eviction[] =
        "\x1b[1;1H\x1b_Ga=p,i=401,p=3,C=1\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_placement_view placement;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;

    config.rows = 3;
    config.columns = 4;
    config.scrollback_capacity = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              upload_and_place,
              sizeof(upload_and_place) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[3;1H\n",
              sizeof("\x1b[3;1H\n") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.anchor_row == 0 && placement.visual_row == 0);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              place_live,
              sizeof(place_live) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[3J",
              sizeof("\x1b[3J") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 0);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 0);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.placement_id == 2);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[2;2H\x1b[J\x1b[1J\x1b[2X\x1b[K",
              sizeof("\x1b[2;2H\x1b[J\x1b[1J\x1b[2X\x1b[K") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[2J",
              sizeof("\x1b[2J") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(boxrig_terminal_image_count(terminal) == 1);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              place_for_eviction,
              sizeof(place_for_eviction) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[3;1H\n\x1b[3;1H\n\x1b[3;1H\n",
              sizeof("\x1b[3;1H\n\x1b[3;1H\n\x1b[3;1H\n") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 2);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 2) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(boxrig_terminal_image_count(terminal) == 1);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_scrollback_capacity_updates_graphics_transactionally(void)
{
    static const uint8_t upload_and_place[] =
        "\x1b_Ga=T,i=450,p=1,f=24,s=1,v=1,C=1;AQID\x1b\\";
    static const uint8_t scroll_twice[] =
        "\x1b[2;1H\n\x1b[2;1H\n";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_placement_view before;
    boxrig_image_placement_view after;
    boxrig_dirty dirty;
    uint64_t image_revision;
    size_t consumed = 0;

    config.rows = 2;
    config.columns = 2;
    config.scrollback_capacity = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              upload_and_place,
              sizeof(upload_and_place) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              scroll_twice,
              sizeof(scroll_twice) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 2);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 2) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &before) ==
          BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
    }
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    image_revision = boxrig_terminal_image_revision(terminal);

    boxrig_terminal_test_fail_scrollback_allocation(terminal, 1);
    CHECK(boxrig_terminal_set_scrollback_capacity(terminal, 1) ==
          BOXRIG_STATUS_OUT_OF_MEMORY);
    CHECK(boxrig_terminal_scrollback_capacity(terminal) == 2);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 2);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 2);
    CHECK(boxrig_terminal_image_revision(terminal) == image_revision);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &after) ==
          BOXRIG_STATUS_OK);
    CHECK(after.image_handle == before.image_handle);
    CHECK(after.placement_id == before.placement_id);
    CHECK(after.anchor_row == before.anchor_row);
    CHECK(after.anchor_column == before.anchor_column);
    CHECK(after.visual_row == before.visual_row);
    CHECK(after.visual_column == before.visual_column);
    CHECK(!boxrig_terminal_take_dirty(terminal, &dirty));
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));

    CHECK(boxrig_terminal_set_scrollback_capacity(terminal, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 1);
    CHECK(boxrig_terminal_image_count(terminal) == 1);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_margin_scroll_primitives_and_clipping(void)
{
    struct fixture {
        const char *placement_cursor;
        const char *operation;
        size_t expected_row;
    } fixtures[] = {
        {"\x1b[3;2H", "\x1b[2;4r\x1b[S", 1},
        {"\x1b[3;2H", "\x1b[2;4r\x1b[T", 3},
        {"\x1b[3;2H", "\x1b[2;4r\x1b[4;1H\x1b" "D", 1},
        {"\x1b[3;2H", "\x1b[2;4r\x1b[4;1H\x1b" "E", 1},
        {"\x1b[3;2H", "\x1b[2;4r\x1b[2;1H\x1bM", 3},
        {"\x1b[3;2H", "\x1b[2;4r\x1b[3;1H\x1b[L", 3},
        {"\x1b[4;2H", "\x1b[2;4r\x1b[3;1H\x1b[M", 2}
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    size_t index;

    config.rows = 5;
    config.columns = 5;
    config.scrollback_capacity = 0;
    for (index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]); index++) {
        static const uint8_t place[] =
            "\x1b_Ga=T,i=501,p=1,f=24,s=1,v=1,c=1,r=1,C=1;AQID\x1b\\";
        boxrig_terminal *terminal = NULL;
        boxrig_image_placement_view placement;
        const uint8_t *output = NULL;
        size_t consumed = 0;
        size_t output_length;

        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)fixtures[index].placement_cursor,
                  strlen(fixtures[index].placement_cursor),
                  &consumed) == BOXRIG_STATUS_OK);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  place,
                  sizeof(place) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        output_length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
              BOXRIG_STATUS_OK);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)fixtures[index].operation,
                  strlen(fixtures[index].operation),
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
        CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
              BOXRIG_STATUS_OK);
        CHECK(placement.anchor_row == fixtures[index].expected_row);
        boxrig_terminal_destroy(terminal);
    }

    {
        static const uint8_t placements[] =
            "\x1b[2;2H"
            "\x1b_Ga=T,i=511,p=1,f=24,s=1,v=1,c=1,r=2,C=1;AQID\x1b\\"
            "\x1b[1;4H"
            "\x1b_Ga=T,i=512,p=1,f=24,s=1,v=1,c=1,r=2,C=1;BAUG\x1b\\";
        boxrig_terminal *terminal = NULL;
        boxrig_image_placement_view placement;
        const uint8_t *output = NULL;
        size_t consumed = 0;
        size_t output_length;
        size_t placement_index;
        bool found_clipped = false;
        bool found_crossing = false;

        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  placements,
                  sizeof(placements) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        output_length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
              BOXRIG_STATUS_OK);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)"\x1b[2;5r\x1b[S",
                  sizeof("\x1b[2;5r\x1b[S") - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_image_placement_count(terminal) == 2);
        for (placement_index = 0;
             placement_index < boxrig_terminal_image_placement_count(terminal);
             placement_index++) {
            CHECK(boxrig_terminal_get_image_placement(
                      terminal,
                      placement_index,
                      &placement) == BOXRIG_STATUS_OK);
            if (placement.image_id == 511) {
                found_clipped = placement.anchor_row == 0 &&
                                placement.visual_row == 1 &&
                                placement.clip_y == 1 &&
                                placement.clip_height == 1;
            } else if (placement.image_id == 512) {
                found_crossing = placement.anchor_row == 0 &&
                                 placement.visual_row == 0 &&
                                 placement.clip_y == 0 &&
                                 placement.clip_height == 2;
            }
        }
        CHECK(found_clipped && found_crossing);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_alternate_screen_and_ris_lifecycle(void)
{
    static const uint8_t primary[] =
        "\x1b_Ga=T,i=601,p=1,f=24,s=1,v=1,C=1;AQID\x1b\\";
    static const uint8_t alternate[] =
        "\x1b_Ga=T,i=602,p=1,f=24,s=1,v=1,C=1;BAUG\x1b\\";
    static const uint8_t alternate_1049[] =
        "\x1b_Ga=T,i=603,p=1,f=24,s=1,v=1,C=1;BwgJ\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    uint64_t revision;
    size_t consumed = 0;
    size_t output_length;

    config.rows = 3;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              primary,
              sizeof(primary) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?47h",
              sizeof("\x1b[?47h") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              alternate,
              sizeof(alternate) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?47l",
              sizeof("\x1b[?47l") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?47h",
              sizeof("\x1b[?47h") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?1047l\x1b[?47h",
              sizeof("\x1b[?1047l\x1b[?47h") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?47l\x1b[?1049h",
              sizeof("\x1b[?47l\x1b[?1049h") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              alternate_1049,
              sizeof(alternate_1049) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?1049l",
              sizeof("\x1b[?1049l") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_image_count(terminal) == 3);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    revision = boxrig_terminal_image_revision(terminal);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b" "c",
              sizeof("\x1b" "c") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_count(terminal) == 0);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(boxrig_terminal_image_revision(terminal) != revision);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(!boxrig_terminal_take_dirty(terminal, &dirty) ||
          dirty.column_count != 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_resize_reflow_and_visibility_eviction(void)
{
    {
        static const uint8_t blank_anchor[] =
            "\x1b[3;4H"
            "\x1b_Ga=T,i=701,p=1,f=24,s=1,v=1,C=1;AQID\x1b\\"
            "\x1b[1;1H";
        boxrig_terminal_config config = boxrig_terminal_config_default();
        boxrig_terminal *terminal = NULL;
        boxrig_image_placement_view placement;
        boxrig_dirty dirty;
        const uint8_t *output = NULL;
        size_t consumed = 0;
        size_t output_length;

        config.rows = 3;
        config.columns = 4;
        config.scrollback_capacity = 4;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  blank_anchor,
                  sizeof(blank_anchor) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        output_length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
              BOXRIG_STATUS_OK);
        while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
        }
        CHECK(boxrig_terminal_resize(terminal, 3, 2) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
        CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
              BOXRIG_STATUS_OK);
        CHECK(placement.anchor_row == 2 && placement.anchor_column == 1);
        CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
        CHECK(boxrig_terminal_resize(terminal, 0, 2) ==
              BOXRIG_STATUS_INVALID_ARGUMENT);
        CHECK(boxrig_terminal_image_placement_count(terminal) == 1);

        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)"\x1b[?47h\x1b[2;2H"
                                   "\x1b_Ga=p,i=701,p=2,c=3,r=2,C=1\x1b\\",
                  sizeof("\x1b[?47h\x1b[2;2H"
                         "\x1b_Ga=p,i=701,p=2,c=3,r=2,C=1\x1b\\") - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        output_length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_resize(terminal, 2, 2) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
        CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
              BOXRIG_STATUS_OK);
        CHECK(placement.clip_width == 1 && placement.clip_height == 1);
        CHECK(boxrig_terminal_resize(terminal, 4, 5) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
              BOXRIG_STATUS_OK);
        CHECK(placement.clip_width == 1 && placement.clip_height == 1);
        boxrig_terminal_destroy(terminal);
    }

    {
        static const uint8_t offscreen[] =
            "\x1b_Ga=T,i=801,p=1,f=32,s=1,v=1,C=1;AQIDBA==\x1b\\"
            "\x1b[2;1H\n";
        static const uint8_t visible[] =
            "\x1b[1;1H"
            "\x1b_Ga=T,i=802,p=1,N=1,f=32,s=1,v=1,C=1;BAUGBw==\x1b\\";
        static const uint8_t trigger[] =
            "\x1b_Ga=t,i=803,f=32,s=1,v=1;CAkKCw==\x1b\\";
        boxrig_terminal_config config = boxrig_terminal_config_default();
        boxrig_terminal *terminal = NULL;
        boxrig_image_handle handle;
        boxrig_image_view image;
        boxrig_dirty dirty;
        const uint8_t *output = NULL;
        size_t consumed = 0;
        size_t output_length;
        size_t image_index;
        bool found_801 = false;
        bool found_802 = false;
        bool found_803 = false;

        config.rows = 2;
        config.columns = 2;
        config.scrollback_capacity = 2;
        config.graphics_storage_capacity = 8;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  offscreen,
                  sizeof(offscreen) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        output_length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  visible,
                  sizeof(visible) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        output_length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
        while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
        }
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  trigger,
                  sizeof(trigger) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        output_length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(output_length != 0);
        CHECK(boxrig_terminal_image_count(terminal) == 2);
        CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
        CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
        for (image_index = 0;
             image_index < boxrig_terminal_image_count(terminal);
             image_index++) {
            CHECK(boxrig_terminal_image_handle_at(
                      terminal,
                      image_index,
                      &handle) == BOXRIG_STATUS_OK);
            CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
                  BOXRIG_STATUS_OK);
            found_801 = found_801 || image.image_id == 801;
            found_802 = found_802 || image.image_id == 802;
            found_803 = found_803 || image.image_id == 803;
        }
        CHECK(!found_801 && found_802 && found_803);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_virtual_placements_and_unicode_placeholders(void)
{
    static const uint8_t upload[] =
        "\x1b_Ga=t,i=33554474,f=24,s=4,v=1;AAAAAAAAAAAAAAAA\x1b\\";
    static const uint8_t upload_response[] = "\x1b_Gi=33554474;OK\x1b\\";
    static const uint8_t virtual_placement[] =
        "\x1b_Ga=p,i=33554474,p=7,c=4,r=1,U=1,z=5,C=1\x1b\\";
    static const uint8_t virtual_response[] =
        "\x1b_Gi=33554474,p=7;OK\x1b\\";
    static const uint8_t placeholders[] =
        "\x1b[38;2;0;0;42;48;5;9;58;2;0;0;7m"
        "\xf4\x8e\xbb\xae\xcc\x85\xcc\x85\xcc\x8e"
        "\xf4\x8e\xbb\xae"
        "\xf4\x8e\xbb\xae\xcc\x85"
        "\xf4\x8e\xbb\xae\xcc\x85\xcc\x90";
    static const uint8_t delete_active[] =
        "\x1b_Ga=d,d=a,i=33554474\x1b\\";
    static const uint8_t delete_active_response[] =
        "\x1b_Gi=33554474;OK\x1b\\";
    static const uint8_t delete_virtual[] =
        "\x1b_Ga=d,d=i,i=33554474,p=7\x1b\\";
    static const uint8_t delete_virtual_response[] =
        "\x1b_Gi=33554474,p=7;OK\x1b\\";
    static const uint8_t boundary_upload_and_place[] =
        "\x1b_Ga=t,i=4278190123,f=24,s=1,v=1;AQID\x1b\\"
        "\x1b_Ga=p,i=4278190123,p=9,c=297,r=297,U=1,C=1\x1b\\";
    static const uint8_t boundary_responses[] =
        "\x1b_Gi=4278190123;OK\x1b\\"
        "\x1b_Gi=4278190123,p=9;OK\x1b\\";
    static const uint8_t boundary_placeholder[] =
        "\x1b[2;1H\x1b[38;2;0;0;43;48;5;10;58;2;0;0;9m"
        "\xf4\x8e\xbb\xae\xf0\x9d\x89\x84\xf0\x9d\x89\x84"
        "\xea\xa3\xa5";
    static const uint8_t indexed_upload_and_place[] =
        "\x1b_Ga=t,i=42,f=24,s=1,v=1;BAUG\x1b\\"
        "\x1b_Ga=p,i=42,p=0,c=1,r=1,U=1,C=1\x1b\\";
    static const uint8_t indexed_responses[] =
        "\x1b_Gi=42;OK\x1b\\"
        "\x1b_Gi=42,p=0;OK\x1b\\";
    static const uint8_t indexed_and_unresolved[] =
        "\x1b[3;1H\x1b[38;5;42;48;5;12;59m"
        "\xf4\x8e\xbb\xae\xcc\x85\xcc\x85"
        "\x1b[58;5;8m\xf4\x8e\xbb\xae\xcc\x85\xcc\x85";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_placement_view placement;
    boxrig_cursor_view cursor;
    boxrig_cell_view cell;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t index;

    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
    }
    CHECK(boxrig_terminal_feed(
              terminal,
              upload,
              sizeof(upload) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(upload_response) - 1);
    CHECK(memcmp(output, upload_response, sizeof(upload_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(upload_response) - 1) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              virtual_placement,
              sizeof(virtual_placement) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(virtual_placement) - 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(virtual_response) - 1);
    CHECK(memcmp(output, virtual_response, sizeof(virtual_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(virtual_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0 && cursor.column == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              placeholders,
              sizeof(placeholders) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(placeholders) - 1);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 4);
    for (index = 0; index < 4; index++) {
        CHECK(boxrig_terminal_get_image_placement(
                  terminal,
                  index,
                  &placement) == BOXRIG_STATUS_OK);
        CHECK(placement.kind == BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER);
        CHECK(placement.resource_creation_serial != 0 &&
              placement.placement_creation_serial != 0);
        CHECK(placement.image_id == 33554474 && placement.placement_id == 7);
        CHECK(placement.visual_row == 0 &&
              placement.visual_column == index);
        CHECK(placement.source_x == 0 && placement.source_y == 0);
        CHECK(placement.source_width == 4 && placement.source_height == 1);
        CHECK(placement.columns == 4 && placement.rows == 1);
        CHECK(placement.destination_pixel_width == 4 &&
              placement.destination_pixel_height == 1);
        CHECK(placement.clip_x == index && placement.clip_y == 0);
        CHECK(placement.clip_width == 1 && placement.clip_height == 1);
        CHECK(placement.z_index == 5);
        CHECK(boxrig_terminal_get_cell(terminal, 0, index, &cell) ==
              BOXRIG_STATUS_OK);
        CHECK(cell.text_length == 1 && cell.text[0] == ' ');
        CHECK(cell.foreground.kind == BOXRIG_COLOR_RGB);
        CHECK(cell.foreground.red == 0 && cell.foreground.green == 0 &&
              cell.foreground.blue == 42);
        CHECK(cell.underline_color.kind == BOXRIG_COLOR_RGB);
        CHECK(cell.underline_color.red == 0 &&
              cell.underline_color.green == 0 &&
              cell.underline_color.blue == 7);
        CHECK(cell.background.kind == BOXRIG_COLOR_INDEXED &&
              cell.background.index == 9);
    }
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(dirty.row == 0 && dirty.column == 0 && dirty.column_count == 4);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              delete_active,
              sizeof(delete_active) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(delete_active_response) - 1);
    CHECK(memcmp(
              output,
              delete_active_response,
              sizeof(delete_active_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(delete_active_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 4);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              delete_virtual,
              sizeof(delete_virtual) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(delete_virtual_response) - 1);
    CHECK(memcmp(
              output,
              delete_virtual_response,
              sizeof(delete_virtual_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(delete_virtual_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_count(terminal) == 1);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(dirty.row == 0 && dirty.column == 0 && dirty.column_count == 4);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == ' ');
    CHECK(cell.background.kind == BOXRIG_COLOR_INDEXED &&
          cell.background.index == 9);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              boundary_upload_and_place,
              sizeof(boundary_upload_and_place) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(boundary_responses) - 1);
    CHECK(memcmp(
              output,
              boundary_responses,
              sizeof(boundary_responses) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(boundary_responses) - 1) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              boundary_placeholder,
              sizeof(boundary_placeholder) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.kind == BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER);
    CHECK(placement.image_id == UINT32_C(4278190123));
    CHECK(placement.placement_id == 9);
    CHECK(placement.visual_row == 1 && placement.visual_column == 0);
    CHECK(placement.destination_pixel_width == 297 &&
          placement.destination_pixel_height == 297);
    CHECK(placement.clip_x == 296 && placement.clip_y == 296);
    CHECK(placement.clip_width == 1 && placement.clip_height == 1);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              indexed_upload_and_place,
              sizeof(indexed_upload_and_place) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(indexed_responses) - 1);
    CHECK(memcmp(output, indexed_responses, sizeof(indexed_responses) - 1) ==
          0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(indexed_responses) - 1) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              indexed_and_unresolved,
              sizeof(indexed_and_unresolved) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 2);
    CHECK(boxrig_terminal_get_image_placement(terminal, 1, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.image_id == 42 && placement.placement_id == 0);
    CHECK(placement.visual_row == 2 && placement.visual_column == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 2, 1, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == ' ');
    CHECK(cell.background.kind == BOXRIG_COLOR_INDEXED &&
          cell.background.index == 12);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_placeholder_row_inheritance_and_buffer_switch(void)
{
    static const uint8_t setup[] =
        "\x1b_Ga=t,i=33554474,f=24,s=4,v=1;AAAAAAAAAAAAAAAA\x1b\\"
        "\x1b_Ga=p,i=33554474,p=7,c=4,r=1,U=1,C=1\x1b\\"
        "\x1b[38;2;0;0;42m"
        "\xf4\x8e\xbb\xae\xcc\x85\xcc\x85\xcc\x8e"
        "\xf4\x8e\xbb\xae"
        "\x1b[2;1H"
        "\xf4\x8e\xbb\xae\xcc\x85\xcc\x85\xcc\x8e";
    static const uint8_t remove_row_start[] = "\x1b[1;1H ";
    static const uint8_t enter_alternate[] = "\x1b[?47h";
    static const uint8_t leave_alternate[] = "\x1b[?47l";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_placement_view placement;
    size_t consumed = 0;

    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal, setup, sizeof(setup) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(setup) - 1);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 3);

    CHECK(boxrig_terminal_feed(
              terminal, remove_row_start, sizeof(remove_row_start) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.visual_row == 1 && placement.visual_column == 0);

    CHECK(boxrig_terminal_feed(
              terminal, enter_alternate, sizeof(enter_alternate) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(boxrig_terminal_feed(
              terminal, leave_alternate, sizeof(leave_alternate) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_relative_placement_validation_and_resolution(void)
{
    static const uint8_t uploads[] =
        "\x1b_Ga=t,i=1001,f=24,s=1,v=1;AQID\x1b\\"
        "\x1b_Ga=t,i=1002,f=24,s=1,v=1;BAUG\x1b\\"
        "\x1b_Ga=t,i=1003,f=24,s=1,v=1;BwgJ\x1b\\"
        "\x1b_Ga=t,i=1004,f=24,s=1,v=1;CgsM\x1b\\";
    static const uint8_t upload_responses[] =
        "\x1b_Gi=1001;OK\x1b\\"
        "\x1b_Gi=1002;OK\x1b\\"
        "\x1b_Gi=1003;OK\x1b\\"
        "\x1b_Gi=1004;OK\x1b\\";
    static const uint8_t placements[] =
        "\x1b[2;2H\x1b_Ga=p,i=1001,p=1,C=1\x1b\\"
        "\x1b_Ga=p,i=1002,p=2,P=1001,Q=1,H=2,V=-1\x1b\\"
        "\x1b_Ga=p,i=1003,p=3,P=1002,Q=2,H=-1,V=2\x1b\\";
    static const uint8_t placement_responses[] =
        "\x1b_Gi=1001,p=1;OK\x1b\\"
        "\x1b_Gi=1002,p=2;OK\x1b\\"
        "\x1b_Gi=1003,p=3;OK\x1b\\";
    static const uint8_t missing_parent[] =
        "\x1b_Ga=p,i=1004,p=4,P=9999,Q=1\x1b\\";
    static const uint8_t missing_parent_response[] =
        "\x1b_Gi=1004,p=4;ENOPARENT:parent placement not found\x1b\\";
    static const uint8_t invalid_virtual_child[] =
        "\x1b_Ga=p,i=1004,p=4,c=1,r=1,U=1,P=1001,Q=1\x1b\\";
    static const uint8_t invalid_virtual_child_response[] =
        "\x1b_Gi=1004,p=4;EINVAL:invalid command\x1b\\";
    static const uint8_t cycle[] =
        "\x1b_Ga=p,i=1001,p=1,P=1003,Q=3\x1b\\";
    static const uint8_t cycle_response[] =
        "\x1b_Gi=1001,p=1;ECYCLE:relative placement cycle\x1b\\";
    static const uint8_t replace_parent[] =
        "\x1b[3;1H\x1b_Ga=p,i=1001,p=1,C=1\x1b\\";
    static const uint8_t replace_parent_response[] =
        "\x1b_Gi=1001,p=1;OK\x1b\\";
    static const uint8_t delete_parent[] =
        "\x1b_Ga=d,d=i,i=1001,p=1\x1b\\";
    static const uint8_t delete_parent_response[] =
        "\x1b_Gi=1001,p=1;OK\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_placement_view placement;
    boxrig_cursor_view cursor;
    boxrig_dirty dirty;
    boxrig_image_view image;
    const uint8_t *output = NULL;
    uint8_t fill[128];
    uint64_t revision;
    size_t consumed = 0;
    size_t remaining;
    size_t index;
    bool found_1001 = false;
    bool found_1004 = false;

    config.rows = 6;
    config.columns = 8;
    config.output_capacity = sizeof(fill);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              uploads,
              sizeof(uploads) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(upload_responses) - 1);
    CHECK(memcmp(output, upload_responses, sizeof(upload_responses) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(upload_responses) - 1) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              placements,
              sizeof(placements) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(placement_responses) - 1);
    CHECK(memcmp(
              output,
              placement_responses,
              sizeof(placement_responses) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(placement_responses) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 3);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.kind == BOXRIG_IMAGE_PLACEMENT_NORMAL);
    CHECK(placement.visual_row == 1 && placement.visual_column == 1);
    CHECK(boxrig_terminal_get_image_placement(terminal, 1, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.kind == BOXRIG_IMAGE_PLACEMENT_RELATIVE);
    CHECK(placement.visual_row == 0 && placement.visual_column == 3);
    CHECK(boxrig_terminal_get_image_placement(terminal, 2, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.kind == BOXRIG_IMAGE_PLACEMENT_RELATIVE);
    CHECK(placement.visual_row == 2 && placement.visual_column == 2);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 1 && cursor.column == 1);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    revision = boxrig_terminal_image_revision(terminal);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              missing_parent,
              sizeof(missing_parent) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(missing_parent_response) - 1);
    CHECK(memcmp(
              output,
              missing_parent_response,
              sizeof(missing_parent_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(missing_parent_response) - 1) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              invalid_virtual_child,
              sizeof(invalid_virtual_child) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(invalid_virtual_child_response) - 1);
    CHECK(memcmp(
              output,
              invalid_virtual_child_response,
              sizeof(invalid_virtual_child_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(invalid_virtual_child_response) - 1) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              cycle,
              sizeof(cycle) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(cycle_response) - 1);
    CHECK(memcmp(output, cycle_response, sizeof(cycle_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(cycle_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 3);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              replace_parent,
              sizeof(replace_parent) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(replace_parent_response) - 1);
    CHECK(memcmp(
              output,
              replace_parent_response,
              sizeof(replace_parent_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(replace_parent_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 3);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.image_id == 1002 && placement.visual_row == 1 &&
          placement.visual_column == 2);
    CHECK(boxrig_terminal_get_image_placement(terminal, 1, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.image_id == 1003 && placement.visual_row == 3 &&
          placement.visual_column == 1);
    CHECK(boxrig_terminal_get_image_placement(terminal, 2, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.image_id == 1001 && placement.visual_row == 2 &&
          placement.visual_column == 0);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    memset(fill, 'x', sizeof(fill));
    CHECK(boxrig_terminal_send_text(
              terminal,
              (const char *)fill,
              sizeof(fill)) == BOXRIG_STATUS_OK);
    revision = boxrig_terminal_image_revision(terminal);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              delete_parent,
              sizeof(delete_parent) - 1,
              &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed < sizeof(delete_parent) - 1);
    CHECK(delete_parent[consumed] == (uint8_t)'\\');
    CHECK(boxrig_terminal_image_count(terminal) == 4);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 3);
    CHECK(boxrig_terminal_image_revision(terminal) == revision);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(fill)) ==
          BOXRIG_STATUS_OK);
    remaining = sizeof(delete_parent) - 1 - consumed;
    CHECK(boxrig_terminal_feed(
              terminal,
              delete_parent + consumed,
              remaining,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == remaining);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(delete_parent_response) - 1);
    CHECK(memcmp(
              output,
              delete_parent_response,
              sizeof(delete_parent_response) - 1) == 0);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(boxrig_terminal_image_count(terminal) == 2);
    for (index = 0; index < boxrig_terminal_image_count(terminal); index++) {
        boxrig_image_handle handle = 0;

        CHECK(boxrig_terminal_image_handle_at(terminal, index, &handle) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
              BOXRIG_STATUS_OK);
        found_1001 = found_1001 || image.image_id == 1001;
        found_1004 = found_1004 || image.image_id == 1004;
    }
    CHECK(found_1001 && found_1004);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_relative_placement_depth_and_cascade(void)
{
    static const uint8_t upload[] =
        "\x1b_Ga=t,i=5000,f=24,s=1,v=1;AQID\x1b\\";
    static const uint8_t upload_response[] = "\x1b_Gi=5000;OK\x1b\\";
    static const uint8_t parent[] =
        "\x1b_Ga=p,i=5000,p=1,C=1\x1b\\";
    static const uint8_t parent_response[] =
        "\x1b_Gi=5000,p=1;OK\x1b\\";
    static const uint8_t delete_parent[] =
        "\x1b_Ga=d,d=i,i=5000,p=1\x1b\\";
    static const uint8_t delete_parent_response[] =
        "\x1b_Gi=5000,p=1;OK\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    char command[128];
    char response[128];
    size_t command_length;
    size_t response_length;
    size_t consumed = 0;
    uint32_t placement_id;

    config.rows = 2;
    config.columns = 32;
    config.graphics_placement_capacity = 32;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              upload,
              sizeof(upload) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(upload_response) - 1);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(upload_response) - 1) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              parent,
              sizeof(parent) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(parent_response) - 1);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(parent_response) - 1) == BOXRIG_STATUS_OK);
    for (placement_id = 2; placement_id <= 17; placement_id++) {
        command_length = (size_t)snprintf(
            command,
            sizeof(command),
            "\x1b_Ga=p,i=5000,p=%u,P=5000,Q=%u,H=1,V=0\x1b\\",
            placement_id,
            placement_id - 1);
        response_length = (size_t)snprintf(
            response,
            sizeof(response),
            "\x1b_Gi=5000,p=%u;OK\x1b\\",
            placement_id);
        CHECK(command_length < sizeof(command));
        CHECK(response_length < sizeof(response));
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)command,
                  command_length,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_output_peek(terminal, &output) ==
              response_length);
        CHECK(memcmp(output, response, response_length) == 0);
        CHECK(boxrig_terminal_output_consume(terminal, response_length) ==
              BOXRIG_STATUS_OK);
    }
    CHECK(boxrig_terminal_image_placement_count(terminal) == 17);

    command_length = (size_t)snprintf(
        command,
        sizeof(command),
        "\x1b_Ga=p,i=5000,p=18,P=5000,Q=17,H=1,V=0\x1b\\");
    response_length = (size_t)snprintf(
        response,
        sizeof(response),
        "\x1b_Gi=5000,p=18;ETOODEEP:relative placement depth exceeded\x1b\\");
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)command,
              command_length,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == response_length);
    CHECK(memcmp(output, response, response_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, response_length) ==
          BOXRIG_STATUS_OK);
    command_length = (size_t)snprintf(
        command,
        sizeof(command),
        "\x1b_Ga=p,i=5000,p=1,P=5000,Q=17,H=0,V=0\x1b\\");
    response_length = (size_t)snprintf(
        response,
        sizeof(response),
        "\x1b_Gi=5000,p=1;ECYCLE:relative placement cycle\x1b\\");
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)command,
              command_length,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == response_length);
    CHECK(memcmp(output, response, response_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, response_length) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 17);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              delete_parent,
              sizeof(delete_parent) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(delete_parent_response) - 1);
    CHECK(memcmp(
              output,
              delete_parent_response,
              sizeof(delete_parent_response) - 1) == 0);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(boxrig_terminal_image_count(terminal) == 1);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_virtual_parent_relative_lifecycle(void)
{
    static const uint8_t uploads_and_placements[] =
        "\x1b_Ga=t,i=3001,f=24,s=1,v=1;AQID\x1b\\"
        "\x1b_Ga=t,i=3002,f=24,s=1,v=1;BAUG\x1b\\"
        "\x1b_Ga=p,i=3001,p=1,c=1,r=1,U=1,C=1\x1b\\"
        "\x1b_Ga=p,i=3002,p=2,P=3001,Q=1,H=1,V=1\x1b\\";
    static const uint8_t setup_responses[] =
        "\x1b_Gi=3001;OK\x1b\\"
        "\x1b_Gi=3002;OK\x1b\\"
        "\x1b_Gi=3001,p=1;OK\x1b\\"
        "\x1b_Gi=3002,p=2;OK\x1b\\";
    static const uint8_t first_placeholder[] =
        "\x1b[4;5H\x1b[38;2;0;11;185;48;5;13;58;2;0;0;1m"
        "\xf4\x8e\xbb\xae\xcc\x85\xcc\x85";
    static const uint8_t second_placeholder[] =
        "\x1b[2;6H\xf4\x8e\xbb\xae\xcc\x85\xcc\x85";
    static const uint8_t delete_active[] =
        "\x1b_Ga=d,d=a,i=3001\x1b\\";
    static const uint8_t delete_active_response[] =
        "\x1b_Gi=3001;OK\x1b\\";
    static const uint8_t recreate_child[] =
        "\x1b_Ga=p,i=3002,p=2,P=3001,Q=1,H=1,V=1\x1b\\";
    static const uint8_t recreate_child_response[] =
        "\x1b_Gi=3002,p=2;OK\x1b\\";
    static const uint8_t delete_virtual[] =
        "\x1b_Ga=d,d=i,i=3001,p=1\x1b\\";
    static const uint8_t delete_virtual_response[] =
        "\x1b_Gi=3001,p=1;OK\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_placement_view placement;
    boxrig_cell_view cell;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t index;
    bool found_child;
    bool found_placeholder;

    config.rows = 6;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              uploads_and_placements,
              sizeof(uploads_and_placements) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(setup_responses) - 1);
    CHECK(memcmp(output, setup_responses, sizeof(setup_responses) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(setup_responses) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              first_placeholder,
              sizeof(first_placeholder) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 2);
    found_child = false;
    found_placeholder = false;
    for (index = 0; index < 2; index++) {
        CHECK(boxrig_terminal_get_image_placement(
                  terminal,
                  index,
                  &placement) == BOXRIG_STATUS_OK);
        if (placement.kind == BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER) {
            CHECK(placement.visual_row == 3 && placement.visual_column == 4);
            found_placeholder = true;
        } else if (placement.kind == BOXRIG_IMAGE_PLACEMENT_RELATIVE) {
            CHECK(placement.visual_row == 4 && placement.visual_column == 5);
            found_child = true;
        }
    }
    CHECK(found_child && found_placeholder);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              second_placeholder,
              sizeof(second_placeholder) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 3);
    found_child = false;
    for (index = 0; index < 3; index++) {
        CHECK(boxrig_terminal_get_image_placement(
                  terminal,
                  index,
                  &placement) == BOXRIG_STATUS_OK);
        if (placement.kind == BOXRIG_IMAGE_PLACEMENT_RELATIVE) {
            CHECK(placement.visual_row == 2 && placement.visual_column == 5);
            found_child = true;
        }
    }
    CHECK(found_child);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[4;5HX\x1b[2;6HY",
              sizeof("\x1b[4;5HX\x1b[2;6HY") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              first_placeholder,
              sizeof(first_placeholder) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 2);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              delete_active,
              sizeof(delete_active) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(delete_active_response) - 1);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(delete_active_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_image_count(terminal) == 2);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              recreate_child,
              sizeof(recreate_child) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(recreate_child_response) - 1);
    CHECK(memcmp(
              output,
              recreate_child_response,
              sizeof(recreate_child_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(recreate_child_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 2);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[2J",
              sizeof("\x1b[2J") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(boxrig_terminal_image_count(terminal) == 2);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              first_placeholder,
              sizeof(first_placeholder) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              recreate_child,
              sizeof(recreate_child) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(recreate_child_response) - 1);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(recreate_child_response) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 2);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              delete_virtual,
              sizeof(delete_virtual) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(delete_virtual_response) - 1);
    CHECK(memcmp(
              output,
              delete_virtual_response,
              sizeof(delete_virtual_response) - 1) == 0);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(boxrig_terminal_image_count(terminal) == 1);
    CHECK(boxrig_terminal_get_cell(terminal, 3, 4, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == ' ');
    CHECK(cell.background.kind == BOXRIG_COLOR_INDEXED &&
          cell.background.index == 13);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_relative_cascade_eviction_is_minimal(void)
{
    static const uint8_t setup[] =
        "\x1b_Ga=t,i=6001,f=24,s=1,v=1;AQID\x1b\\"
        "\x1b_Ga=p,i=6001,p=1,C=1\x1b\\"
        "\x1b_Ga=t,i=6002,f=24,s=1,v=1;BAUG\x1b\\"
        "\x1b_Ga=p,i=6002,p=2,P=6001,Q=1,H=1,V=0\x1b\\"
        "\x1b_Ga=t,i=6003,f=24,s=1,v=1;BwgJ\x1b\\"
        "\x1b[2;1H\x1b_Ga=p,i=6003,p=3,C=1\x1b\\";
    static const uint8_t setup_responses[] =
        "\x1b_Gi=6001;OK\x1b\\"
        "\x1b_Gi=6001,p=1;OK\x1b\\"
        "\x1b_Gi=6002;OK\x1b\\"
        "\x1b_Gi=6002,p=2;OK\x1b\\"
        "\x1b_Gi=6003;OK\x1b\\"
        "\x1b_Gi=6003,p=3;OK\x1b\\";
    static const uint8_t replacement[] =
        "\x1b_Ga=t,i=6004,f=24,s=2,v=1;AAAAAAAA\x1b\\";
    static const uint8_t replacement_response[] =
        "\x1b_Gi=6004;OK\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_view image;
    boxrig_image_placement_view placement;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t index;
    bool found_6003 = false;
    bool found_6004 = false;

    config.rows = 2;
    config.columns = 6;
    config.graphics_storage_capacity = 12;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              setup,
              sizeof(setup) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(setup_responses) - 1);
    CHECK(memcmp(output, setup_responses, sizeof(setup_responses) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(setup_responses) - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_count(terminal) == 3);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 3);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              replacement,
              sizeof(replacement) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(replacement_response) - 1);
    CHECK(memcmp(
              output,
              replacement_response,
              sizeof(replacement_response) - 1) == 0);
    CHECK(boxrig_terminal_image_count(terminal) == 2);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.image_id == 6003);
    for (index = 0; index < boxrig_terminal_image_count(terminal); index++) {
        boxrig_image_handle handle = 0;

        CHECK(boxrig_terminal_image_handle_at(terminal, index, &handle) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
              BOXRIG_STATUS_OK);
        found_6003 = found_6003 || image.image_id == 6003;
        found_6004 = found_6004 || image.image_id == 6004;
    }
    CHECK(found_6003 && found_6004);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_relative_parent_is_protected_from_eviction(void)
{
    static const uint8_t parent[] =
        "\x1b_Ga=T,i=7001,p=1,f=24,s=1,v=1,C=1;AQID\x1b\\";
    static const uint8_t parent_response[] =
        "\x1b_Gi=7001,p=1;OK\x1b\\";
    static const uint8_t child[] =
        "\x1b_Ga=T,i=7002,p=2,P=7001,Q=1,H=1,V=0,"
        "f=24,s=1,v=1,C=1;BAUG\x1b\\";
    static const uint8_t child_response[] =
        "\x1b_Gi=7002,p=2;ENOSPC:graphics capacity exceeded\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_handle handle = 0;
    boxrig_image_view image;
    boxrig_image_placement_view placement;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 4;
    config.graphics_storage_capacity = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              parent,
              sizeof(parent) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(parent_response) - 1);
    CHECK(memcmp(output, parent_response, sizeof(parent_response) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(parent_response) - 1) == BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              child,
              sizeof(child) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(child) - 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(child_response) - 1);
    CHECK(memcmp(output, child_response, sizeof(child_response) - 1) == 0);
    CHECK(boxrig_terminal_image_count(terminal) == 1);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image) ==
          BOXRIG_STATUS_OK);
    CHECK(image.image_id == 7001);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.image_id == 7001 && placement.placement_id == 1);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_virtual_placement_overflow_is_atomic(void)
{
    static const uint8_t upload_and_place[] =
        "\x1b_Ga=t,i=7101,f=32,s=1,v=2;AQIDBAUGBwg=\x1b\\"
        "\x1b_Ga=p,i=7101,p=1,c=4294967295,r=1,U=1,C=1\x1b\\";
    static const uint8_t responses[] =
        "\x1b_Gi=7101;OK\x1b\\"
        "\x1b_Gi=7101,p=1;EINVAL:invalid command\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(
              terminal,
              UINT32_MAX,
              UINT32_MAX) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              upload_and_place,
              sizeof(upload_and_place) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(upload_and_place) - 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(responses) - 1);
    CHECK(memcmp(output, responses, sizeof(responses) - 1) == 0);
    CHECK(boxrig_terminal_image_count(terminal) == 1);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(!boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    struct test_case {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"empty and delimiterless headers",
         test_empty_and_delimiterless_headers},
        {"all known keys and integer boundaries",
         test_all_known_keys_and_integer_boundaries},
        {"unknown keys and duplicates", test_unknown_keys_and_duplicates},
        {"malformed headers are rejected",
         test_malformed_headers_are_rejected},
        {"header capacity boundary", test_header_capacity_boundary},
        {"APC streams multiple maximum payloads",
         test_apc_streams_multiple_maximum_payloads},
        {"APC every split preserves stream",
         test_apc_every_split_preserves_stream},
        {"APC consumer retries are atomic",
         test_apc_consumer_retries_are_atomic},
        {"APC cancel and limits abort once",
         test_apc_cancel_and_limits_abort_once},
        {"APC invalid headers never begin",
         test_apc_invalid_headers_never_begin},
        {"APC empty and delimiterless commands",
         test_apc_empty_and_delimiterless_commands},
        {"APC RIS and destroy reset transactions",
         test_apc_ris_and_destroy_reset_transactions},
        {"graphics command profile semantics and quiet responses",
         test_graphics_command_profile_semantics_and_quiet_responses},
        {"frame deletion renumbers and reclaims storage",
         test_frame_delete_renumbers_and_reclaims_storage},
        {"frame deletion current pixels and deadlines",
         test_frame_delete_current_pixels_and_deadlines},
        {"frame deletion retry cancel and image number",
         test_frame_delete_retry_cancel_and_image_number},
        {"frame deletion loading gapless tail and final placements",
         test_frame_delete_loading_gapless_tail_and_final_placements},
        {"static graphics delete selector semantics",
         test_static_delete_selector_semantics},
        {"static graphics query response order and retry",
         test_static_query_response_order_and_retry},
        {"static graphics query preserves existing barrier order",
         test_static_query_preserves_existing_barrier_order},
        {"graphics config disable and empty public views",
         test_graphics_config_disable_and_empty_views},
        {"external transport exposes owned host requests",
         test_external_transport_exposes_owned_host_requests},
        {"external transport validation retry order and reset",
         test_external_transport_validation_retry_order_and_reset},
        {"external completion decodes raw PNG zlib and places",
         test_external_completion_decodes_raw_png_zlib_and_places},
        {"external completion errors and recovery are atomic",
         test_external_completion_errors_and_recovery_are_atomic},
        {"external completions preserve identity and input order",
         test_external_completions_preserve_identity_and_input_order},
        {"frame transfer add edit and chunks",
         test_frame_transfer_add_edit_and_chunks},
        {"external frame transfer uses the same transaction",
         test_external_frame_transfer_uses_the_same_transaction},
        {"external frame completion keeps the newest success",
         test_external_frame_completion_keeps_the_newest_success},
        {"frame quota eviction disable and retry are atomic",
         test_frame_quota_eviction_disable_and_retry_are_atomic},
        {"animation control selects frames and validates controls",
         test_animation_control_selects_frames_and_validates_controls},
        {"frame composition coordinates alpha and overlap",
         test_frame_composition_coordinates_alpha_and_overlap},
        {"animation control and composition retry are atomic",
         test_animation_control_and_composition_retry_are_atomic},
        {"animation monotonic deadlines and rollback",
         test_animation_monotonic_deadlines_and_rollback},
        {"animation finite stop and loading modes",
         test_animation_finite_stop_and_loading_modes},
        {"animation gapless frames and loading tail",
         test_animation_gapless_frames_and_loading_tail},
        {"animation large time skip and deadline overflow",
         test_animation_large_time_skip_and_deadline_overflow},
        {"cell pixel geometry and window reports",
         test_cell_pixel_geometry_and_window_reports},
        {"raw RGB and RGBA normalize at every input split",
         test_raw_rgb_and_rgba_normalize_at_every_input_split},
        {"raw Base64 validation and query is non-mutating",
         test_raw_base64_validation_and_query_is_non_mutating},
        {"raw APC chunks and empty final chunk",
         test_raw_apc_chunks_and_empty_final_chunk},
        {"raw chunk errors and new command abort old upload",
         test_raw_chunk_errors_and_new_command_abort_old_upload},
        {"raw dimensions and decode capacity are bounded",
         test_raw_dimensions_and_decode_capacity_are_bounded},
        {"zlib and PNG normalize at every input split",
         test_zlib_and_png_normalize_at_every_input_split},
        {"PNG grayscale alpha 16-bit and interlace normalize",
         test_png_grayscale_alpha_16_bit_and_interlace_normalize},
        {"zlib and PNG APC chunk boundaries",
         test_zlib_and_png_apc_chunk_boundaries},
        {"zlib PNG errors and decode quota recover",
         test_zlib_png_errors_and_decode_quota_recover},
        {"raw upload abort reset and destroy",
         test_raw_upload_abort_reset_and_destroy},
        {"raw upload limits discard logical staging",
         test_raw_upload_limits_discard_logical_staging},
        {"raw final commit backpressure is atomic",
         test_raw_final_commit_backpressure_is_atomic},
        {"image identity replacement and anonymous resources",
         test_image_identity_replacement_and_anonymous_resources},
        {"image numbers assign monotonic IDs",
         test_image_numbers_assign_monotonic_ids},
        {"image capacity and replacement are atomic",
         test_image_capacity_and_replacement_are_atomic},
        {"storage eviction and backpressure are atomic",
         test_storage_eviction_and_backpressure_are_atomic},
        {"normal placement crop scale offset and dirty",
         test_normal_placement_crop_scale_offset_and_dirty},
        {"normal placement scaling validation and geometry change",
         test_normal_placement_scaling_validation_and_geometry_change},
        {"transmit and place geometry cursor and atomic commit",
         test_transmit_and_place_geometry_cursor_and_atomic_commit},
        {"placement number replacement and capacity",
         test_placement_number_replacement_and_capacity},
        {"placement backpressure retry is atomic",
         test_placement_backpressure_retry_is_atomic},
        {"placement resource replacement and eviction dirty",
         test_placement_resource_replacement_and_eviction_dirty},
        {"delete identity lifetime and backpressure",
         test_delete_identity_lifetime_and_backpressure},
        {"delete location selectors", test_delete_location_selectors},
        {"screen history viewport and erase lifecycle",
         test_screen_history_viewport_and_erase_lifecycle},
        {"scrollback capacity updates graphics transactionally",
         test_scrollback_capacity_updates_graphics_transactionally},
        {"margin scroll primitives and clipping",
         test_margin_scroll_primitives_and_clipping},
        {"alternate screen and RIS lifecycle",
         test_alternate_screen_and_ris_lifecycle},
        {"resize reflow and visibility eviction",
         test_resize_reflow_and_visibility_eviction},
        {"virtual placements and Unicode placeholders",
         test_virtual_placements_and_unicode_placeholders},
        {"placeholder row inheritance and buffer switch",
         test_placeholder_row_inheritance_and_buffer_switch},
        {"relative placement validation and resolution",
         test_relative_placement_validation_and_resolution},
        {"relative placement depth and cascade",
         test_relative_placement_depth_and_cascade},
        {"virtual parent relative lifecycle",
         test_virtual_parent_relative_lifecycle},
        {"relative cascade eviction is minimal",
         test_relative_cascade_eviction_is_minimal},
        {"relative parent is protected from eviction",
         test_relative_parent_is_protected_from_eviction},
        {"virtual placement overflow is atomic",
         test_virtual_placement_overflow_is_atomic}
    };
    size_t index;

    for (index = 0; index < sizeof(tests) / sizeof(tests[0]); index++) {
        if (!tests[index].run()) {
            return 1;
        }
        printf("PASS %s\n", tests[index].name);
    }
    return 0;
}
