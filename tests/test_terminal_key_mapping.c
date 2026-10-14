/* boxrig - Under MIT. */
#include "boxrig/terminal.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #expression); \
    goto fail; } } while (0)

static bool feed(boxrig_terminal *terminal, const char *text)
{
    size_t consumed = 0;
    return boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text),
        &consumed) == BOXRIG_STATUS_OK && consumed == strlen(text);
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

static bool modes_modifiers_and_actions(void)
{
    boxrig_terminal *terminal = NULL;
    const boxrig_key keys[] = {BOXRIG_KEY_UP, BOXRIG_KEY_DOWN, BOXRIG_KEY_RIGHT, BOXRIG_KEY_LEFT};
    const uint8_t bytes[][2] = {{'U', 0}, {'D', 0}, {'R', 0}, {'L', 0}};
    boxrig_key_mapping mappings[4];
    for (unsigned index = 0; index < 4; index++)
        mappings[index] = (boxrig_key_mapping){keys[index], 0, bytes[index], 2};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    CHECK(config.input.key_mappings == NULL && config.input.key_mapping_count == 0);
    config.input.key_mappings = mappings;
    config.input.key_mapping_count = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (unsigned mode = 0; mode < 2; mode++) {
        CHECK(feed(terminal, mode ? "\033[?1h" : "\033[?1l"));
        for (unsigned index = 0; index < 4; index++)
        for (unsigned locks = 0; locks < 4; locks++)
        for (unsigned action = 0; action < 3; action++) {
            boxrig_key_event key = {.key = keys[index], .action = (boxrig_key_action)action,
                .modifiers = (locks & 1 ? BOXRIG_MODIFIER_CAPS_LOCK : 0) |
                    (locks & 2 ? BOXRIG_MODIFIER_NUM_LOCK : 0)};
            CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
            CHECK(output_is(terminal, bytes[index], action == 2 ? 0 : 2));
        }
        boxrig_key_event key = {.key = BOXRIG_KEY_UP, .modifiers = BOXRIG_MODIFIER_SHIFT};
        CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
        CHECK(output_is(terminal, "\033[1;2A", 6));
        key.key = BOXRIG_KEY_HOME; key.modifiers = 0;
        CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
        CHECK(output_is(terminal, mode ? "\033OH" : "\033[H", 3));
        key.key = BOXRIG_KEY_KP_UP;
        CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
        CHECK(output_is(terminal, bytes[0], 2));
    }
    boxrig_terminal_destroy(terminal); return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool complete_bytes_empty_and_exact_modifiers(void)
{
    boxrig_terminal *terminal = NULL;
    const uint8_t raw[] = {0, 0xff, 0x80, 0x1b, '[', 'A'};
    const uint32_t modifiers = BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_ALT |
        BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SUPER | BOXRIG_MODIFIER_HYPER | BOXRIG_MODIFIER_META;
    const boxrig_key_mapping mappings[] = {
        {BOXRIG_KEY_UP, modifiers, raw, sizeof(raw)},
        {BOXRIG_KEY_DOWN, 0, NULL, 0}
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.input.key_mappings = mappings; config.input.key_mapping_count = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_key_event key = {.key = BOXRIG_KEY_UP, .modifiers = modifiers};
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, raw, sizeof(raw)));
    key.modifiers = modifiers | BOXRIG_MODIFIER_CAPS_LOCK | BOXRIG_MODIFIER_NUM_LOCK;
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, raw, sizeof(raw)));
    key.modifiers = BOXRIG_MODIFIER_ALT;
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\033[1;3A", 6));
    key.key = BOXRIG_KEY_DOWN; key.modifiers = 0;
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, NULL, 0));
    key.modifiers = BOXRIG_MODIFIER_CONTROL;
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\033[1;5B", 6));
    boxrig_terminal_destroy(terminal); return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool kitty_precedence(void)
{
    boxrig_terminal *terminal = NULL;
    const boxrig_key_mapping mapping = {BOXRIG_KEY_UP, 0, (const uint8_t *)"U", 1};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.input.key_mappings = &mapping; config.input.key_mapping_count = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    /* The protocol encodes arrows as CSI finals, with event subparameters
     * only when report-events is enabled. Other flags alone leave them legacy. */
    for (unsigned flags = 0; flags < 32; flags++) {
        char enable[24];
        int length = snprintf(enable, sizeof(enable), "\033[=%uu", flags);
        CHECK(length > 0 && (size_t)length < sizeof(enable));
        CHECK(feed(terminal, enable));
        for (unsigned action = 0; action < 3; action++) {
            boxrig_key_event key = {.key = BOXRIG_KEY_UP, .action = (boxrig_key_action)action};
            const char *expected;
            if (action == 2 && !(flags & 2)) expected = "";
            else if (action != 0 && (flags & 2))
                expected = action == 1 ? "\033[1;1:2A" : "\033[1;1:3A";
            else expected = flags & (1 | 8) ? "\033[A" : "U";
            CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
            CHECK(output_is(terminal, expected, strlen(expected)));
        }
    }
    CHECK(feed(terminal, "\033[=1u"));
    boxrig_key_event key = {.key = BOXRIG_KEY_KP_UP};
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\033[57419u", 8));
    CHECK(feed(terminal, "\033[=0u\033="));
    key.key = BOXRIG_KEY_KP_8;
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\033Ox", 3));
    key.key = BOXRIG_KEY_KP_UP;
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "U", 1));
    boxrig_terminal_destroy(terminal); return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool ownership_reset_and_isolation(void)
{
    boxrig_terminal *first = NULL, *second = NULL;
    uint8_t bytes[] = {'A', 0, 'B'};
    boxrig_key_mapping mapping = {BOXRIG_KEY_UP, 0, bytes, sizeof(bytes)};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.input.key_mappings = &mapping; config.input.key_mapping_count = 1;
    config.input.enter_newline = BOXRIG_NEWLINE_LF;
    CHECK(boxrig_terminal_create(&config, &first) == BOXRIG_STATUS_OK);
    memset(bytes, 'Z', sizeof(bytes));
    mapping.key = BOXRIG_KEY_DOWN;
    CHECK(boxrig_terminal_create(&config, &second) == BOXRIG_STATUS_OK);
    mapping.bytes = NULL; mapping.length = 0;
    config = boxrig_terminal_config_default();
    const char *resets[] = {"", "\033c", "\033[!p", "\033[?1049h", "\033[?1049l"};
    for (size_t i = 0; i < sizeof(resets) / sizeof(resets[0]); i++) {
        CHECK(feed(first, resets[i]));
        CHECK(feed(second, resets[i]));
        boxrig_key_event key = {.key = BOXRIG_KEY_UP};
        CHECK(boxrig_terminal_send_key(first, &key) == BOXRIG_STATUS_OK);
        CHECK(output_is(first, "A\0B", 3));
        CHECK(boxrig_terminal_send_key(second, &key) == BOXRIG_STATUS_OK);
        CHECK(output_is(second, "\033[A", 3));
        key.key = BOXRIG_KEY_DOWN;
        CHECK(boxrig_terminal_send_key(second, &key) == BOXRIG_STATUS_OK);
        CHECK(output_is(second, "ZZZ", 3));
        key.key = BOXRIG_KEY_ENTER;
        CHECK(boxrig_terminal_send_key(first, &key) == BOXRIG_STATUS_OK);
        CHECK(output_is(first, "\n", 1));
    }
    boxrig_terminal_destroy(first); boxrig_terminal_destroy(second); return true;
fail:
    boxrig_terminal_destroy(first); boxrig_terminal_destroy(second); return false;
}

static bool invalid_settings_and_limits(void)
{
    boxrig_terminal *terminal = NULL;
    boxrig_key_mapping mappings[65];
    uint8_t bytes[4097] = {0};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.input.key_mappings = mappings;
    for (unsigned i = 0; i < 65; i++)
        mappings[i] = (boxrig_key_mapping){BOXRIG_KEY_UP, i, bytes, 0};
    config.input.key_mapping_count = 64;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal); terminal = NULL;
    config.input.key_mapping_count = 65;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(terminal == NULL);
    config.input.key_mapping_count = SIZE_MAX;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    config.input.key_mapping_count = 1;
    for (unsigned test = 0; test < 7; test++) {
        mappings[0] = (boxrig_key_mapping){BOXRIG_KEY_UP, 0, bytes, 1};
        config.input.key_mappings = mappings;
        if (test == 0) mappings[0].key = BOXRIG_KEY_ENTER;
        if (test == 1) mappings[0].key = BOXRIG_KEY_NONE;
        if (test == 2) mappings[0].key = BOXRIG_KEY_KP_UP;
        if (test == 3) mappings[0].modifiers = BOXRIG_MODIFIER_CAPS_LOCK;
        if (test == 4) mappings[0].modifiers = 1u << 20;
        if (test == 5) mappings[0].bytes = NULL;
        if (test == 6) config.input.key_mappings = NULL;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_INVALID_ARGUMENT);
        CHECK(terminal == NULL);
    }
    config.input.key_mappings = mappings;
    mappings[0] = (boxrig_key_mapping){BOXRIG_KEY_UP, 0, bytes, 4097};
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    mappings[0].length = SIZE_MAX;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    mappings[0].length = 1;
    mappings[1] = mappings[0]; config.input.key_mapping_count = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_INVALID_ARGUMENT);
    for (unsigned i = 0; i < 17; i++)
        mappings[i] = (boxrig_key_mapping){BOXRIG_KEY_UP, i, bytes, 4096};
    config.input.key_mapping_count = 16;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal); terminal = NULL;
    config.input.key_mapping_count = 17;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(terminal == NULL);
    return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool retry_preserves_atomic_bytes(void)
{
    boxrig_terminal *terminal = NULL;
    const boxrig_key_mapping mapping = {BOXRIG_KEY_UP, 0, (const uint8_t *)"A\0B", 3};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.input.key_mappings = &mapping; config.input.key_mapping_count = 1;
    config.output_capacity = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "xy", 2) == BOXRIG_STATUS_OK);
    boxrig_key_event key = {.key = BOXRIG_KEY_UP};
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(output_is(terminal, "xy", 2));
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    const uint8_t *bytes;
    CHECK(boxrig_terminal_output_peek(terminal, &bytes) == 3 && bytes[0] == 'A');
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\0B", 2));
    key.action = BOXRIG_KEY_REPEAT;
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "A\0B", 3));
    boxrig_terminal_destroy(terminal); terminal = NULL;
    config.output_capacity = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(output_is(terminal, NULL, 0));
    boxrig_terminal_destroy(terminal); return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

static bool barriers_and_other_output(void)
{
    boxrig_terminal *terminal = NULL;
    const boxrig_key_mapping mapping = {BOXRIG_KEY_UP, 0, (const uint8_t *)"U\0", 2};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.input.key_mappings = &mapping; config.input.key_mapping_count = 1;
    config.input.enter_newline = BOXRIG_NEWLINE_LF;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "\033]52;c;?\007"));
    boxrig_host_item item;
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_CLIPBOARD_READ);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    boxrig_key_event key = {.key = BOXRIG_KEY_UP};
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    key.key = BOXRIG_KEY_ENTER;
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    key = (boxrig_key_event){.codepoint = 'c', .modifiers = BOXRIG_MODIFIER_CONTROL,
        .text = "c", .text_length = 1};
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "\033[A", 3) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "\033[6n"));
    CHECK(output_is(terminal, NULL, 0));
    CHECK(boxrig_terminal_complete_host_request(terminal, item.request_id,
        BOXRIG_HOST_REQUEST_SUCCESS, (const uint8_t *)"X", 1) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\033]52;c;WA==\007", 12));
    CHECK(output_is(terminal, "U\0\n\003\033[A\033[1;1R", 13));
    boxrig_paste_event paste = {.phase = BOXRIG_PASTE_BEGIN};
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    paste.phase = BOXRIG_PASTE_CHUNK; paste.text = "\nU"; paste.text_length = 2;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    CHECK(output_is(terminal, "\nU", 2));
    boxrig_terminal_destroy(terminal); return true;
fail:
    boxrig_terminal_destroy(terminal); return false;
}

int main(void)
{
    const struct {const char *name; bool (*run)(void);} tests[] = {
        {"Configured arrows override legacy modes and preserve fallback", modes_modifiers_and_actions},
        {"Complete byte sequences, empty mappings and exact modifiers", complete_bytes_empty_and_exact_modifiers},
        {"Kitty flags and keypad precedence", kitty_precedence},
        {"Deep ownership, reset and terminal isolation", ownership_reset_and_isolation},
        {"Configuration validation and capacity boundaries", invalid_settings_and_limits},
        {"Atomic configured output and partial drain retry", retry_preserves_atomic_bytes},
        {"Host barrier ordering and unrelated output", barriers_and_other_output}
    };
    bool passed = true;
    for (size_t index = 0; index < sizeof(tests) / sizeof(tests[0]); index++) {
        bool result = tests[index].run();
        printf("%s %s\n", result ? "PASS" : "FAIL", tests[index].name);
        passed = result && passed;
    }
    return passed ? 0 : 1;
}
