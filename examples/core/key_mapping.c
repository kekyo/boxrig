/* boxrig - Under MIT. */
#include "boxrig/terminal.h"
#include <stdio.h>

static bool show_key(boxrig_terminal *terminal, boxrig_key identity, const char *label)
{
    boxrig_key_event key = {.key = identity, .action = BOXRIG_KEY_PRESS};
    if (boxrig_terminal_send_key(terminal, &key) != BOXRIG_STATUS_OK) return false;
    const uint8_t *bytes;
    size_t length = boxrig_terminal_output_peek(terminal, &bytes);
    printf("%s:", label);
    for (size_t index = 0; index < length; index++) printf(" %02x", bytes[index]);
    putchar('\n');
    return boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK;
}

int main(void)
{
    const uint8_t sequence[] = {'U', 0};
    const boxrig_key_mapping mapping = {BOXRIG_KEY_UP, 0, sequence, sizeof(sequence)};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *standard = NULL, *configured = NULL;
    int result = 1;
    if (boxrig_terminal_create(&config, &standard) != BOXRIG_STATUS_OK) goto cleanup;
    config.input.key_mappings = &mapping;
    config.input.key_mapping_count = 1;
    if (boxrig_terminal_create(&config, &configured) != BOXRIG_STATUS_OK) goto cleanup;
    if (!show_key(standard, BOXRIG_KEY_UP, "default") || !show_key(configured, BOXRIG_KEY_UP, "configured")) goto cleanup;
    const uint8_t application_mode[] = "\033[?1h";
    size_t consumed;
    if (boxrig_terminal_feed(standard, application_mode, sizeof(application_mode) - 1,
            &consumed) != BOXRIG_STATUS_OK || consumed != sizeof(application_mode) - 1 ||
        boxrig_terminal_feed(configured, application_mode, sizeof(application_mode) - 1,
            &consumed) != BOXRIG_STATUS_OK || consumed != sizeof(application_mode) - 1)
        goto cleanup;
    if (!show_key(standard, BOXRIG_KEY_UP, "application default") ||
        !show_key(configured, BOXRIG_KEY_UP, "application configured")) goto cleanup;
    config.input.enter_newline = BOXRIG_NEWLINE_LF;
    config.input.backspace_binding = BOXRIG_ERASE_DELETE_SEQUENCE;
    config.input.delete_binding = BOXRIG_ERASE_ASCII_BACKSPACE;
    if (boxrig_terminal_set_input_config(standard, &config.input) != BOXRIG_STATUS_OK ||
        !show_key(standard, BOXRIG_KEY_UP, "runtime up") ||
        !show_key(standard, BOXRIG_KEY_ENTER, "runtime enter") ||
        !show_key(standard, BOXRIG_KEY_BACKSPACE, "runtime backspace") ||
        !show_key(standard, BOXRIG_KEY_DELETE, "runtime delete")) goto cleanup;
    result = 0;
cleanup:
    boxrig_terminal_destroy(standard);
    boxrig_terminal_destroy(configured);
    return result;
}
