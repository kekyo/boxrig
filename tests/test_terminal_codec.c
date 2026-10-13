/* boxrig - Under MIT. */
#include "boxrig/terminal.h"
#include "boxrig/session.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #expression); \
    goto fail; } } while (0)

static bool feed(boxrig_terminal *terminal, const void *bytes, size_t length)
{
    size_t consumed = 0;
    return boxrig_terminal_feed(terminal, bytes, length, &consumed) ==
        BOXRIG_STATUS_OK && consumed == length;
}

static bool cell_is(boxrig_terminal *terminal, size_t row, size_t column, const char *text)
{
    boxrig_cell_view cell;
    return boxrig_terminal_get_cell(terminal, row, column, &cell) == BOXRIG_STATUS_OK &&
        cell.text_length == strlen(text) && memcmp(cell.text, text, cell.text_length) == 0;
}

static bool output_is(boxrig_terminal *terminal, const void *expected, size_t length)
{
    const uint8_t *bytes = NULL;
    size_t actual = boxrig_terminal_output_peek(terminal, &bytes);
    if (actual != length || (length != 0 && memcmp(bytes, expected, length) != 0)) {
        fprintf(stderr, "expected %zu bytes, got %zu\n", length, actual);
        return false;
    }
    return length == 0 || boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK;
}

static bool receive_every_split(void)
{
    boxrig_terminal *terminal = NULL;
    /* Japanese text, then a DEC drawing designation surrounding characters
     * whose Shift_JIS trail bytes overlap ASCII/DEC drawing codes. */
    const uint8_t bytes[] = {0x93,0xfa,0x96,0x7b,0x8c,0xea,13,10,
        27,'(','0',0x82,0x60,0x83,0x5c,0x95,0x5c,'q',27,'(','B',0xb1};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.encoding = BOXRIG_ENCODING_SHIFT_JIS;
    config.rows = 4; config.columns = 30;
    for (size_t split = 0; split <= sizeof(bytes) + 1; split++) {
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        if (split <= sizeof(bytes)) {
            CHECK(feed(terminal, bytes, split));
            CHECK(feed(terminal, bytes + split, sizeof(bytes) - split));
        } else for (size_t offset = 0; offset < sizeof(bytes); offset++)
            CHECK(feed(terminal, bytes + offset, 1));
        CHECK(cell_is(terminal, 0, 0, "日"));
        CHECK(cell_is(terminal, 0, 2, "本"));
        CHECK(cell_is(terminal, 0, 4, "語"));
        CHECK(cell_is(terminal, 1, 0, "Ａ"));
        CHECK(cell_is(terminal, 1, 2, "ソ"));
        CHECK(cell_is(terminal, 1, 4, "表"));
        CHECK(cell_is(terminal, 1, 6, "─"));
        CHECK(cell_is(terminal, 1, 7, "ｱ"));
        boxrig_cursor_view cursor;
        boxrig_terminal_get_cursor(terminal, &cursor);
        CHECK(cursor.row == 1 && cursor.column == 8);
        boxrig_terminal_destroy(terminal); terminal = NULL;
    }
    return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool malformed_and_eof(void)
{
    boxrig_terminal *terminal = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.encoding = BOXRIG_ENCODING_SHIFT_JIS;
    config.receive_newline = BOXRIG_RECEIVE_NEWLINE_CRLF;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    const uint8_t invalid[] = {0x82,'"',0xff,0x82,' ',0x82};
    CHECK(feed(terminal, invalid, sizeof(invalid)));
    CHECK(cell_is(terminal, 0, 0, "�") && cell_is(terminal, 0, 1, "\""));
    CHECK(cell_is(terminal, 0, 2, "�") && cell_is(terminal, 0, 3, "�"));
    CHECK(cell_is(terminal, 0, 4, " ") && cell_is(terminal, 0, 5, " "));
    CHECK(boxrig_terminal_finish_input(terminal) == BOXRIG_STATUS_OK);
    CHECK(cell_is(terminal, 0, 5, "�"));
    CHECK(boxrig_terminal_finish_input(terminal) == BOXRIG_STATUS_OK);
    boxrig_cursor_view cursor;
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.column == 6);
    CHECK(feed(terminal, "\r", 1));
    CHECK(boxrig_terminal_finish_input(terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "\n", 1));
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 2);
    const char *partial[] = {"\033", "\033[12", "\033]2;discard", "\033P$qm", "\033_Ga=q;AAAA"};
    for (size_t i = 0; i < sizeof(partial) / sizeof(partial[0]); i++) {
        CHECK(feed(terminal, partial[i], strlen(partial[i])));
        CHECK(boxrig_terminal_finish_input(terminal) == BOXRIG_STATUS_OK);
        CHECK(feed(terminal, "X", 1));
        CHECK(cell_is(terminal, 2, i, "X"));
        boxrig_host_item item;
        CHECK(!boxrig_terminal_host_item_peek(terminal, &item));
        CHECK(output_is(terminal, NULL, 0));
    }
    boxrig_terminal_destroy(terminal); terminal = NULL;
    config.encoding = BOXRIG_ENCODING_UTF8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "\xe6\x97", 2));
    CHECK(cell_is(terminal, 0, 0, " "));
    CHECK(boxrig_terminal_finish_input(terminal) == BOXRIG_STATUS_OK);
    CHECK(cell_is(terminal, 0, 0, "�"));
    CHECK(feed(terminal, "\x82", 1));
    CHECK(cell_is(terminal, 0, 1, "�"));
    boxrig_terminal_destroy(terminal); return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool outgoing_text_and_raw(void)
{
    boxrig_terminal *terminal = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    CHECK(config.encoding == BOXRIG_ENCODING_UTF8);
    for (unsigned encoding = 0; encoding < 2; encoding++) {
        config.encoding = (boxrig_encoding)encoding;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        const char utf8[] = "日本語ｱ\0\n";
        const uint8_t sjis[] = {0x93,0xfa,0x96,0x7b,0x8c,0xea,0xb1,0,10};
        CHECK(boxrig_terminal_send_text(terminal, utf8, sizeof(utf8) - 1) == BOXRIG_STATUS_OK);
        CHECK(output_is(terminal, encoding ? (const void *)sjis : utf8,
            encoding ? sizeof(sjis) : sizeof(utf8) - 1));
        const uint8_t raw[] = {0xff,0x82,0,27,'[','A'};
        CHECK(boxrig_terminal_send_bytes(terminal, raw, sizeof(raw)) == BOXRIG_STATUS_OK);
        CHECK(output_is(terminal, raw, sizeof(raw)));
        const char *invalid[] = {"\x82", "\xe6\x97", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "A\xe6"};
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            CHECK(boxrig_terminal_send_text(terminal, invalid[i], strlen(invalid[i])) == BOXRIG_STATUS_INVALID_ARGUMENT);
            CHECK(output_is(terminal, NULL, 0));
        }
        CHECK(boxrig_terminal_send_text(terminal, NULL, 0) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_send_bytes(terminal, NULL, 1) == BOXRIG_STATUS_INVALID_ARGUMENT);
        boxrig_terminal_destroy(terminal); terminal = NULL;
    }
    config.encoding = BOXRIG_ENCODING_SHIFT_JIS;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "¥‾−😀\xee\x80\x80", 15) == BOXRIG_STATUS_OK);
    const uint8_t lossy[] = {0x5c,0x7e,0x81,0x7c,'?','?'};
    CHECK(output_is(terminal, lossy, sizeof(lossy)));
    boxrig_terminal_destroy(terminal); terminal = NULL;
    config.encoding = (boxrig_encoding)-1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(terminal == NULL);
    config.encoding = (boxrig_encoding)2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_INVALID_ARGUMENT);
    return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool keys_paste_and_protocols(void)
{
    boxrig_terminal *terminal = NULL;
    const uint8_t mapped[] = {0xff,0,0x82};
    const boxrig_key_mapping mapping = {BOXRIG_KEY_UP, 0, mapped, sizeof(mapped)};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.encoding = BOXRIG_ENCODING_SHIFT_JIS;
    config.paste_newline = BOXRIG_NEWLINE_CRLF;
    config.key_mappings = &mapping; config.key_mapping_count = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_key_event key = {.key = BOXRIG_KEY_NONE, .codepoint = 0x65e5,
        .text = "日", .text_length = 3, .modifiers = BOXRIG_MODIFIER_ALT};
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\033\x93\xfa", 3));
    key.modifiers = 0;
    CHECK(feed(terminal, "\033[=8u", 5));
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\033[26085u", 8));
    CHECK(feed(terminal, "\033[=0u", 5));
    key = (boxrig_key_event){.key = BOXRIG_KEY_UP};
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, mapped, sizeof(mapped)));
    key = (boxrig_key_event){.key = BOXRIG_KEY_NONE, .codepoint = 'c',
        .text = "c", .text_length = 1, .modifiers = BOXRIG_MODIFIER_CONTROL};
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\003", 1));
    CHECK(feed(terminal, "\033[?2004h", 8));
    boxrig_paste_event paste = {.phase = BOXRIG_PASTE_BEGIN};
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\033[200~", 6));
    paste.phase = BOXRIG_PASTE_CHUNK; paste.text = "日\n本"; paste.text_length = 7;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\x93\xfa\r\n\x96\x7b", 6));
    const char *bad[] = {"\xe6\x97", "\033[201~", "\xff"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        paste.text = bad[i]; paste.text_length = strlen(bad[i]);
        CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_INVALID_ARGUMENT);
        CHECK(output_is(terminal, NULL, 0));
    }
    paste = (boxrig_paste_event){.phase = BOXRIG_PASTE_END};
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\033[201~", 6));
    CHECK(feed(terminal, "\033[5n", 4));
    CHECK(output_is(terminal, "\033[0n", 4));
    boxrig_terminal_destroy(terminal); return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool atomic_encoded_capacity(void)
{
    boxrig_terminal *terminal = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.encoding = BOXRIG_ENCODING_SHIFT_JIS;
    config.output_capacity = 4; config.paste_newline = BOXRIG_NEWLINE_CRLF;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "ab", 2) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "日本", 6) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(output_is(terminal, "ab", 2));
    CHECK(boxrig_terminal_send_text(terminal, "日本", 6) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "日", 3) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(output_is(terminal, "\xfa\x96\x7b", 3));
    CHECK(boxrig_terminal_send_text(terminal, "日", 3) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\x93\xfa", 2));
    CHECK(boxrig_terminal_send_text(terminal, "日本語", 9) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    boxrig_paste_event paste = {.phase = BOXRIG_PASTE_BEGIN};
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    paste = (boxrig_paste_event){.phase = BOXRIG_PASTE_CHUNK, .text = "日\n", .text_length = 4};
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(output_is(terminal, "x", 1));
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\x93\xfa\r\n", 4));
    paste.text = "日\n\n"; paste.text_length = 5;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(output_is(terminal, NULL, 0));
    boxrig_terminal_destroy(terminal); return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool titles_and_receive_backpressure(void)
{
    boxrig_terminal *terminal = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.encoding = BOXRIG_ENCODING_SHIFT_JIS; config.host_item_capacity = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "\007\x82", 2));
    size_t consumed = 99;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\007X", 2, &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed == 0 && cell_is(terminal, 0, 0, "�"));
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\007X", 2, &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed == 0 && cell_is(terminal, 0, 1, " "));
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "\007X", 2));
    CHECK(cell_is(terminal, 0, 1, "X"));
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    for (unsigned command = 0; command < 3; command++) {
        uint8_t title[] = {27,']',(uint8_t)('0' + command),';',0x93,0xfa,0x82,7};
        for (size_t i = 0; i < sizeof(title); i++) CHECK(feed(terminal, title + i, 1));
        boxrig_host_item item;
        CHECK(boxrig_terminal_host_item_peek(terminal, &item));
        CHECK(item.type == BOXRIG_HOST_ITEM_TITLE && item.payload_length == 6);
        CHECK(memcmp(item.payload, "日�", 6) == 0);
        CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    }
    /* URI payloads keep UTF-8 even when ground text uses Shift_JIS. */
    const char uri[] = "\033]7;file:///日本\007";
    CHECK(feed(terminal, uri, sizeof(uri) - 1));
    boxrig_host_item item;
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.payload_length == strlen("file:///日本"));
    CHECK(memcmp(item.payload, "file:///日本", item.payload_length) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal); return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool reset_and_owned_settings(void)
{
    boxrig_terminal *first = NULL, *second = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.encoding = BOXRIG_ENCODING_SHIFT_JIS;
    CHECK(boxrig_terminal_create(&config, &first) == BOXRIG_STATUS_OK);
    config.encoding = BOXRIG_ENCODING_UTF8;
    CHECK(boxrig_terminal_create(&config, &second) == BOXRIG_STATUS_OK);
    const char *resets[] = {"", "\033c", "\033[!p", "\033[?1049h", "\033[?1049l"};
    for (size_t i = 0; i < sizeof(resets) / sizeof(resets[0]); i++) {
        CHECK(feed(first, resets[i], strlen(resets[i])));
        CHECK(feed(second, resets[i], strlen(resets[i])));
        CHECK(boxrig_terminal_send_text(first, "あ", 3) == BOXRIG_STATUS_OK);
        CHECK(output_is(first, "\x82\xa0", 2));
        CHECK(boxrig_terminal_send_text(second, "あ", 3) == BOXRIG_STATUS_OK);
        CHECK(output_is(second, "あ", 3));
    }
    CHECK(feed(first, "\x82\033c\xa0", 4));
    CHECK(cell_is(first, 0, 0, "�"));
    CHECK(boxrig_terminal_finish_input(NULL) == BOXRIG_STATUS_INVALID_ARGUMENT);
    boxrig_terminal_destroy(first); boxrig_terminal_destroy(second); return true;
fail:
    boxrig_terminal_destroy(first); boxrig_terminal_destroy(second); return false;
}

int main(void)
{
    const struct {const char *name; bool (*run)(void);} cases[] = {
        {"receive_every_split", receive_every_split},
        {"malformed_and_eof", malformed_and_eof},
        {"outgoing_text_and_raw", outgoing_text_and_raw},
        {"keys_paste_and_protocols", keys_paste_and_protocols},
        {"atomic_encoded_capacity", atomic_encoded_capacity},
        {"titles_and_receive_backpressure", titles_and_receive_backpressure},
        {"reset_and_owned_settings", reset_and_owned_settings}
    };
    unsigned failures = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bool passed = cases[i].run();
        printf("%s %s\n", passed ? "PASS" : "FAIL", cases[i].name);
        if (!passed) failures++;
    }
    return failures ? 1 : 0;
}
