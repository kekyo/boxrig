/* boxrig - Under MIT. */
#include "boxrig/terminal.h"
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

static bool output_is(boxrig_terminal *terminal, const char *expected, size_t length)
{
    const uint8_t *bytes = NULL;
    size_t actual = boxrig_terminal_output_peek(terminal, &bytes);
    if (actual != length || (length != 0 && memcmp(bytes, expected, length) != 0)) {
        fprintf(stderr, "output: expected %zu bytes, got %zu\n", length, actual);
        return false;
    }
    return length == 0 || boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK;
}

static bool enter_policies(void)
{
    boxrig_terminal *terminal = NULL;
    const boxrig_newline policies[] = {BOXRIG_NEWLINE_TERMINAL, BOXRIG_NEWLINE_CR,
        BOXRIG_NEWLINE_LF, BOXRIG_NEWLINE_CRLF};
    for (size_t p = 0; p < 4; p++) for (unsigned mode = 0; mode < 2; mode++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        config.enter_newline = policies[p];
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        if (mode) CHECK(feed(terminal, "\033[20h", 5));
        for (unsigned keypad = 0; keypad < 2; keypad++)
        for (unsigned modifiers = 0; modifiers < 4; modifiers++)
        for (unsigned action = 0; action < 3; action++) {
            boxrig_key_event event = {0};
            event.key = keypad ? BOXRIG_KEY_KP_ENTER : BOXRIG_KEY_ENTER;
            event.action = action == 0 ? BOXRIG_KEY_PRESS : action == 1 ?
                BOXRIG_KEY_REPEAT : BOXRIG_KEY_RELEASE;
            event.modifiers = (modifiers & 1 ? BOXRIG_MODIFIER_ALT : 0) |
                (modifiers & 2 ? BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT : 0);
            CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
            char expected[3]; size_t length = 0;
            if (action != 2) {
                if (modifiers & 1) expected[length++] = '\033';
                boxrig_newline effective = policies[p] == BOXRIG_NEWLINE_TERMINAL
                    ? (mode ? BOXRIG_NEWLINE_CRLF : BOXRIG_NEWLINE_CR) : policies[p];
                expected[length++] = effective == BOXRIG_NEWLINE_LF ? '\n' : '\r';
                if (effective == BOXRIG_NEWLINE_CRLF) expected[length++] = '\n';
            }
            CHECK(output_is(terminal, expected, length));
        }
        boxrig_terminal_destroy(terminal); terminal = NULL;
    }
    return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool protocol_priority_and_reset(void)
{
    boxrig_terminal *terminal = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.enter_newline = BOXRIG_NEWLINE_LF;
    config.paste_newline = BOXRIG_NEWLINE_CRLF;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_key_event key = {.key = BOXRIG_KEY_KP_ENTER, .action = BOXRIG_KEY_PRESS};
    CHECK(feed(terminal, "\033=", 2));
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\033OM", 3));
    CHECK(feed(terminal, "\033>", 2));
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\n", 1));
    CHECK(feed(terminal, "\033[>8u", 5));
    key.key = BOXRIG_KEY_ENTER;
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\033[13u", 5));
    CHECK(feed(terminal, "\033[<u\033[20h\033[20$p", 15));
    CHECK(output_is(terminal, "\033[20;1$y", 8));
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\n", 1));
    CHECK(feed(terminal, "\033c\033[20$p", 8));
    CHECK(output_is(terminal, "\033[20;2$y", 8));
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\n", 1));
    CHECK(feed(terminal, "\033[20h\033[!p", 9));
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\n", 1));
    boxrig_paste_event paste = {.phase = BOXRIG_PASTE_BEGIN};
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    paste.phase = BOXRIG_PASTE_CHUNK; paste.text = "\n"; paste.text_length = 1;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\r\n", 2));
    boxrig_terminal_destroy(terminal); return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool received_controls(void)
{
    boxrig_terminal *terminal = NULL;
    /* Each triple is the expected row after CR, LF, and CRLF. Columns are
     * observed after a final X, following an initial 'ab'. */
    const size_t rows[5][3] = {{0,1,1},{0,1,1},{0,1,1},{1,1,2},{1,1,1}};
    const size_t columns[5][3] = {{1,3,1},{1,3,1},{1,1,1},{1,3,1},{1,1,1}};
    const char *controls[] = {"\r", "\n", "\r\n"};
    for (unsigned p = 0; p < 5; p++) for (unsigned mode = 0; mode < 2; mode++)
    for (unsigned c = 0; c < 3; c++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        config.receive_newline = (boxrig_receive_newline)p;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        if (mode) CHECK(feed(terminal, "\033[20h", 5));
        CHECK(feed(terminal, "ab", 2));
        CHECK(feed(terminal, controls[c], strlen(controls[c])));
        CHECK(feed(terminal, "X", 1));
        boxrig_cursor_view cursor;
        boxrig_terminal_get_cursor(terminal, &cursor);
        CHECK(cursor.row == rows[p][c]);
        CHECK(cursor.column == (p == 0 && mode && c == 1 ? 1 : columns[p][c]));
        boxrig_cell_view cell;
        CHECK(boxrig_terminal_get_cell(terminal, cursor.row, cursor.column - 1, &cell) == BOXRIG_STATUS_OK);
        CHECK(cell.text_length == 1 && cell.text[0] == 'X');
        boxrig_terminal_destroy(terminal); terminal = NULL;
    }
    boxrig_terminal_config config = boxrig_terminal_config_default();
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "abc\rX", 5));
    boxrig_cell_view cell;
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == 'b');
    boxrig_terminal_destroy(terminal); return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool split_crlf_and_intervening_bytes(void)
{
    boxrig_terminal *terminal = NULL;
    const struct {const char *bytes; size_t length; size_t row;} cases[] = {
        {"A\r\nB", 4, 1}, {"A\r\r\nB", 5, 2}, {"A\r\0\nB", 5, 2},
        {"A\r\177\nB", 5, 2}, {"A\r\033[0m\nB", 8, 2},
        {"A\r\030\nB", 5, 2}, {"A\r\007\nB", 5, 2},
        {"A\r\033c\nB", 6, 1}, {"A\r\033[!p\nB", 8, 1}
    };
    for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)
    for (size_t split = 0; split <= cases[c].length + 1; split++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        config.receive_newline = BOXRIG_RECEIVE_NEWLINE_CRLF;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        if (split <= cases[c].length) {
            CHECK(feed(terminal, cases[c].bytes, split));
            CHECK(feed(terminal, cases[c].bytes + split, cases[c].length - split));
        } else for (size_t i = 0; i < cases[c].length; i++)
            CHECK(feed(terminal, cases[c].bytes + i, 1));
        boxrig_cursor_view cursor;
        boxrig_terminal_get_cursor(terminal, &cursor);
        if (cursor.row != cases[c].row || cursor.column != 1)
            fprintf(stderr, "split case=%zu split=%zu cursor=%zu,%zu expected=%zu,1\n", c, split, cursor.row, cursor.column, cases[c].row);
        CHECK(cursor.row == cases[c].row && cursor.column == 1);
        boxrig_cell_view cell;
        CHECK(boxrig_terminal_get_cell(terminal, cursor.row, 0, &cell) == BOXRIG_STATUS_OK);
        CHECK(cell.text_length == 1 && cell.text[0] == 'B');
        boxrig_terminal_destroy(terminal); terminal = NULL;
    }
    return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool paste_encoding_and_literal_text(void)
{
    boxrig_terminal *terminal = NULL;
    const char *expected[] = {"A\rB\rC\r", "A\nB\rC\n", "A\r\nB\rC\r\n"};
    for (unsigned p = 0; p < 3; p++) for (unsigned bracketed = 0; bracketed < 2; bracketed++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        config.paste_newline = (boxrig_newline)p;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        if (bracketed) CHECK(feed(terminal, "\033[?2004h", 8));
        boxrig_paste_event event = {.phase = BOXRIG_PASTE_BEGIN};
        CHECK(boxrig_terminal_send_paste(terminal, &event) == BOXRIG_STATUS_OK);
        CHECK(output_is(terminal, bracketed ? "\033[200~" : "", bracketed ? 6 : 0));
        CHECK(feed(terminal, "\033[?2004l", 8));
        event.phase = BOXRIG_PASTE_CHUNK; event.text = "A\nB\rC\n"; event.text_length = 6;
        CHECK(boxrig_terminal_send_paste(terminal, &event) == BOXRIG_STATUS_OK);
        CHECK(output_is(terminal, expected[p], strlen(expected[p])));
        event.text = "\033"; event.text_length = 1;
        CHECK(boxrig_terminal_send_paste(terminal, &event) == BOXRIG_STATUS_INVALID_ARGUMENT);
        CHECK(output_is(terminal, "", 0));
        event.phase = BOXRIG_PASTE_END; event.text = NULL; event.text_length = 0;
        CHECK(boxrig_terminal_send_paste(terminal, &event) == BOXRIG_STATUS_OK);
        CHECK(output_is(terminal, bracketed ? "\033[201~" : "", bracketed ? 6 : 0));
        CHECK(boxrig_terminal_send_text(terminal, "a\nb\r", 4) == BOXRIG_STATUS_OK);
        CHECK(output_is(terminal, "a\nb\r", 4));
        boxrig_terminal_destroy(terminal); terminal = NULL;
    }
    return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool atomic_output_retry(void)
{
    boxrig_terminal *terminal = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.output_capacity = 5;
    config.enter_newline = BOXRIG_NEWLINE_CRLF;
    config.paste_newline = BOXRIG_NEWLINE_CRLF;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "abcd", 4) == BOXRIG_STATUS_OK);
    boxrig_key_event key = {.key = BOXRIG_KEY_ENTER, .action = BOXRIG_KEY_PRESS};
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(output_is(terminal, "abcd", 4));
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\r\n", 2));
    boxrig_paste_event paste = {.phase = BOXRIG_PASTE_BEGIN};
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    paste.phase = BOXRIG_PASTE_CHUNK; paste.text = "\n\n\n"; paste.text_length = 3;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(output_is(terminal, "", 0));
    CHECK(boxrig_terminal_send_text(terminal, "ab", 2) == BOXRIG_STATUS_OK);
    paste.text_length = 2;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(output_is(terminal, "ab", 2));
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\r\n\r\n", 4));
    paste.phase = BOXRIG_PASTE_END; paste.text = NULL; paste.text_length = 0;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal); terminal = NULL;
    config.output_capacity = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    key.modifiers = BOXRIG_MODIFIER_ALT;
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(output_is(terminal, "", 0));
    boxrig_terminal_destroy(terminal); return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool receive_backpressure_and_barrier(void)
{
    boxrig_terminal *terminal = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.receive_newline = BOXRIG_RECEIVE_NEWLINE_CRLF;
    config.enter_newline = BOXRIG_NEWLINE_LF;
    config.paste_newline = BOXRIG_NEWLINE_CRLF;
    config.host_item_capacity = 1;
    config.clipboard_capacity = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "\007A\r", 3));
    size_t consumed = 0;
    const uint8_t blocked[] = "\007\nB";
    CHECK(boxrig_terminal_feed(terminal, blocked, 3, &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, blocked, 3));
    boxrig_cursor_view cursor;
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 2 && cursor.column == 1);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "\033]52;c;?\007", 9));
    boxrig_host_item item;
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_CLIPBOARD_READ);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    boxrig_key_event key = {.key = BOXRIG_KEY_ENTER, .action = BOXRIG_KEY_PRESS};
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    boxrig_paste_event paste = {.phase = BOXRIG_PASTE_BEGIN};
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    paste.phase = BOXRIG_PASTE_CHUNK; paste.text = "\n"; paste.text_length = 1;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "", 0));
    CHECK(boxrig_terminal_complete_host_request(terminal, item.request_id,
        BOXRIG_HOST_REQUEST_SUCCESS, (const uint8_t *)"X", 1) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\033]52;c;WA==\007", 12));
    CHECK(output_is(terminal, "\n\r\n", 3));
    boxrig_terminal_destroy(terminal); return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool invalid_configuration(void)
{
    boxrig_terminal *terminal = NULL;
    for (unsigned p = 0; p < 4; p++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        if (p == 0) config.enter_newline = (boxrig_newline)-1;
        if (p == 1) config.paste_newline = BOXRIG_NEWLINE_TERMINAL;
        if (p == 2) config.receive_newline = (boxrig_receive_newline)5;
        if (p == 3) config.receive_newline = (boxrig_receive_newline)-1;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_INVALID_ARGUMENT);
        CHECK(terminal == NULL);
    }
    return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool settings_are_owned_per_terminal(void)
{
    boxrig_terminal *first = NULL, *second = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.enter_newline = BOXRIG_NEWLINE_LF;
    config.receive_newline = BOXRIG_RECEIVE_NEWLINE_CR;
    CHECK(boxrig_terminal_create(&config, &first) == BOXRIG_STATUS_OK);
    config.enter_newline = BOXRIG_NEWLINE_CRLF;
    config.receive_newline = BOXRIG_RECEIVE_NEWLINE_STANDARD;
    CHECK(boxrig_terminal_create(&config, &second) == BOXRIG_STATUS_OK);
    config = boxrig_terminal_config_default();
    boxrig_key_event key = {.key = BOXRIG_KEY_ENTER, .action = BOXRIG_KEY_PRESS};
    CHECK(boxrig_terminal_send_key(first, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(first, "\n", 1));
    CHECK(boxrig_terminal_send_key(second, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(second, "\r\n", 2));
    CHECK(feed(first, "ab\rX", 4));
    CHECK(feed(second, "ab\rX", 4));
    boxrig_cursor_view cursor;
    boxrig_terminal_get_cursor(first, &cursor);
    CHECK(cursor.row == 1 && cursor.column == 1);
    boxrig_terminal_get_cursor(second, &cursor);
    CHECK(cursor.row == 0 && cursor.column == 1);
    boxrig_terminal_destroy(first);
    boxrig_terminal_destroy(second);
    return true;
fail:
    boxrig_terminal_destroy(first);
    boxrig_terminal_destroy(second);
    return false;
}

static bool unrelated_controls_keep_their_meaning(void)
{
    boxrig_terminal *terminal = NULL;
    const char *controls[] = {"\013", "\014", "\033D", "\033E",
        "\033]2;title\r\ntext\007", "\033Pignored\r\ntext\033\\"};
    for (unsigned p = 0; p < 5; p++) for (unsigned mode = 0; mode < 2; mode++)
    for (size_t c = 0; c < sizeof(controls) / sizeof(controls[0]); c++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        config.receive_newline = (boxrig_receive_newline)p;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        if (mode) CHECK(feed(terminal, "\033[20h", 5));
        CHECK(feed(terminal, "ab", 2));
        CHECK(feed(terminal, controls[c], strlen(controls[c])));
        boxrig_cursor_view cursor;
        boxrig_terminal_get_cursor(terminal, &cursor);
        CHECK(cursor.row == (c < 4 ? 1 : 0));
        CHECK(cursor.column == (c == 3 || (mode && c < 2) ? 0 : 2));
        boxrig_terminal_destroy(terminal); terminal = NULL;
    }
    return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

int main(void)
{
    const struct { const char *name; bool (*run)(void); } tests[] = {
        {"Enter modes, keypad, modifiers and actions", enter_policies},
        {"Protocol priority and persistent reset settings", protocol_priority_and_reset},
        {"Independent received controls and CR overwrite", received_controls},
        {"All CRLF splits and intervening bytes", split_crlf_and_intervening_bytes},
        {"Paste markers, encoding and literal text", paste_encoding_and_literal_text},
        {"Atomic expanded output and retry", atomic_output_retry},
        {"Receive backpressure and outbound barrier", receive_backpressure_and_barrier},
        {"Invalid construction settings", invalid_configuration},
        {"Owned settings remain independent across terminals", settings_are_owned_per_terminal},
        {"Other controls and protocol payloads retain their meaning", unrelated_controls_keep_their_meaning}
    };
    bool passed = true;
    for (size_t i = 0; i < sizeof(tests)/sizeof(tests[0]); i++) {
        bool result = tests[i].run();
        printf("%s %s\n", result ? "PASS" : "FAIL", tests[i].name);
        passed = passed && result;
    }
    return passed ? 0 : 1;
}
