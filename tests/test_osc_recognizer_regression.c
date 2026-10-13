#include "boxrig/terminal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(                                                         \
                stderr,                                                       \
                "check failed at %s:%d: %s\n",                               \
                __FILE__,                                                     \
                __LINE__,                                                     \
                #condition);                                                  \
            return false;                                                     \
        }                                                                     \
    } while (0)

static bool register_url_recognizer(
    boxrig_terminal *terminal,
    uint64_t *recognizer_id_out)
{
    static const uint8_t pattern[] = "https://[a-z.]+";
    boxrig_text_recognizer_options options;
    boxrig_text_regex_diagnostic diagnostic;

    memset(&options, 0, sizeof(options));
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH;
    options.pattern = pattern;
    options.pattern_length = sizeof(pattern) - 1u;
    options.limits = boxrig_text_recognizer_limits_default();
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal,
              &options,
              &diagnostic,
              recognizer_id_out) == BOXRIG_STATUS_OK);
    CHECK(diagnostic.code == BOXRIG_TEXT_REGEX_DIAGNOSTIC_NONE);
    CHECK(*recognizer_id_out != 0u);
    return true;
}

static bool feed_all(
    boxrig_terminal *terminal,
    const uint8_t *input,
    size_t input_length)
{
    size_t consumed = 0u;

    CHECK(boxrig_terminal_feed(
              terminal,
              input,
              input_length,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == input_length);
    return true;
}

static bool resume_recognizers(boxrig_terminal *terminal)
{
    bool more_work;
    size_t dispatch_count = 0u;

    do {
        size_t work_used = 0u;

        more_work = false;
        CHECK(boxrig_terminal_resume_text_recognizers(
                  terminal,
                  7u,
                  &work_used,
                  &more_work) == BOXRIG_STATUS_OK);
        CHECK(work_used <= 7u);
        dispatch_count++;
        CHECK(dispatch_count < 1000000u);
    } while (more_work);
    return true;
}

static bool range_has_bytes(
    boxrig_terminal *terminal,
    const boxrig_text_range *range,
    const uint8_t *expected,
    size_t expected_length)
{
    boxrig_text_snapshot_options options;
    boxrig_text_snapshot *snapshot = NULL;
    const uint8_t *bytes;
    size_t byte_length = 0u;

    memset(&options, 0, sizeof(options));
    options.screen = range->start.screen;
    options.range = range;
    options.byte_capacity = expected_length;
    options.line_capacity = 1u;
    CHECK(boxrig_terminal_create_text_snapshot(
              terminal,
              &options,
              &snapshot) == BOXRIG_STATUS_OK);
    bytes = boxrig_text_snapshot_data(snapshot, &byte_length);
    CHECK(byte_length == expected_length);
    CHECK(bytes != NULL);
    CHECK(memcmp(bytes, expected, expected_length) == 0);
    boxrig_text_snapshot_destroy(snapshot);
    return true;
}

static bool known_unknown_osc_and_overlap_keep_separate_semantics(void)
{
    static const uint8_t input[] =
        "\x1b]0;recognizer boundary\x07"
        "\x1b]4;1;#123\x07"
        "\x1b]7;file:///tmp/boxrig\x1b\\"
        "\x1b]10;#abcdef\x1b\\"
        "\x1b]777;https://hidden.example\x07"
        "\x1b]8;id=explicit;https://semantic.example\x1b\\"
        "https://visible.example"
        "\x1b]8;;\x1b\\"
        "\x1b]52;c;SGk=\x07"
        "\x1b]99;i=notice;Ready\x07";
    static const uint8_t visible[] = "https://visible.example";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_viewport_cell_resolution cell;
    boxrig_text_recognition_result result;
    boxrig_host_item item;
    boxrig_rgb color;
    uint64_t recognizer_id = 0u;
    uint64_t request_id;
    size_t required = 0u;

    config.rows = 2u;
    config.columns = 80u;
    config.host_item_capacity = 8u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(register_url_recognizer(terminal, &recognizer_id));
    CHECK(feed_all(terminal, input, sizeof(input) - 1u));
    CHECK(resume_recognizers(terminal));

    CHECK(boxrig_terminal_resolve_viewport_cell(
              terminal,
              0u,
              0u,
              &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.cell.hyperlink_handle != 0u);
    CHECK(cell.cell.hyperlink_uri_length == 24u);
    CHECK(memcmp(
              cell.cell.hyperlink_uri,
              "https://semantic.example",
              24u) == 0);
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              recognizer_id,
              &result,
              1u,
              &required) == BOXRIG_STATUS_OK);
    CHECK(required == 1u);
    CHECK(range_has_bytes(
        terminal,
        &result.range,
        visible,
        sizeof(visible) - 1u));

    CHECK(boxrig_terminal_get_palette_color(terminal, 1u, &color) ==
          BOXRIG_STATUS_OK);
    CHECK(color.red == 0x11u && color.green == 0x22u && color.blue == 0x33u);
    CHECK(boxrig_terminal_get_dynamic_color(
              terminal,
              BOXRIG_DYNAMIC_COLOR_FOREGROUND,
              &color) == BOXRIG_STATUS_OK);
    CHECK(color.red == 0xabu && color.green == 0xcdu && color.blue == 0xefu);

    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_TITLE);
    CHECK(item.flags == (BOXRIG_TITLE_ICON | BOXRIG_TITLE_WINDOW));
    CHECK(item.payload_length == 19u);
    CHECK(memcmp(item.payload, "recognizer boundary", 19u) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);

    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_WORKING_DIRECTORY);
    CHECK(item.payload_length == 18u);
    CHECK(memcmp(item.payload, "file:///tmp/boxrig", 18u) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);

    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_CLIPBOARD_WRITE);
    CHECK(item.payload_length == 2u);
    CHECK(memcmp(item.payload, "Hi", 2u) == 0);
    request_id = item.request_id;
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              NULL,
              0u) == BOXRIG_STATUS_OK);

    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_NOTIFICATION);
    CHECK(item.notification != NULL);
    CHECK(item.notification->identifier.length == 6u);
    CHECK(memcmp(item.notification->identifier.bytes, "notice", 6u) == 0);
    CHECK(item.notification->title.length == 5u);
    CHECK(memcmp(item.notification->title.bytes, "Ready", 5u) == 0);
    request_id = item.request_id;
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              NULL,
              0u) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_host_item_peek(terminal, &item));

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool osc52_and_osc99_limits_remain_independent_from_recognition(void)
{
    static const uint8_t oversized_clipboard[] =
        "\x1b]52;c;MTIzNDU=\x07";
    static const uint8_t bounded_clipboard[] =
        "\x1b]52;c;MTIzNA==\x07";
    static const uint8_t oversized_notification[] =
        "\x1b]99;;12345\x07";
    static const uint8_t bounded_notification[] =
        "\x1b]99;;1234\x07";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    uint64_t recognizer_id = 0u;
    uint64_t request_id;

    config.rows = 1u;
    config.columns = 16u;
    config.host_item_capacity = 4u;
    config.clipboard_capacity = 4u;
    config.notification_capacity = 4u;
    config.notification_pending_capacity = 1u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(register_url_recognizer(terminal, &recognizer_id));

    CHECK(feed_all(
        terminal,
        oversized_clipboard,
        sizeof(oversized_clipboard) - 1u));
    CHECK(!boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(feed_all(
        terminal,
        bounded_clipboard,
        sizeof(bounded_clipboard) - 1u));
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_CLIPBOARD_WRITE);
    CHECK(item.payload_length == 4u);
    CHECK(memcmp(item.payload, "1234", 4u) == 0);
    request_id = item.request_id;
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              NULL,
              0u) == BOXRIG_STATUS_OK);

    CHECK(feed_all(
        terminal,
        oversized_notification,
        sizeof(oversized_notification) - 1u));
    CHECK(!boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(feed_all(
        terminal,
        bounded_notification,
        sizeof(bounded_notification) - 1u));
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_NOTIFICATION);
    CHECK(item.notification != NULL);
    CHECK(item.notification->title.length == 4u);
    CHECK(memcmp(item.notification->title.bytes, "1234", 4u) == 0);
    request_id = item.request_id;
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              NULL,
              0u) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(resume_recognizers(terminal));

    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    if (!known_unknown_osc_and_overlap_keep_separate_semantics() ||
        !osc52_and_osc99_limits_remain_independent_from_recognition()) {
        return 1;
    }
    puts("PASS OSC and text recognizer boundaries");
    return 0;
}
