/* boxrig - Under MIT. */
#include "boxrig/terminal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void feed(boxrig_terminal *terminal, const char *text)
{
    size_t consumed = 0;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text),
                                &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == strlen(text));
}

static void round_trip(const char *prefix, const char *tail)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *original = NULL;
    const size_t limit = 32 * 1024 * 1024;
    config.rows = 3;
    config.columns = 10;
    assert(boxrig_terminal_create(&config, &original) == BOXRIG_STATUS_OK);
    feed(original, prefix);
    uint8_t *wire = NULL;
    size_t length = 0;
    assert(boxrig_terminal_export_checkpoint(original, limit, &wire, &length)
           == BOXRIG_STATUS_OK);
    /* Only the exported bytes may be used to recreate the terminal. */
    feed(original, tail);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        boxrig_terminal *copy = NULL;
        assert(boxrig_terminal_import_checkpoint(wire, length - 1, limit, &copy)
               != BOXRIG_STATUS_OK && copy == NULL);
        assert(boxrig_terminal_import_checkpoint(wire, length, length - 1, &copy)
               == BOXRIG_STATUS_LIMIT_EXCEEDED && copy == NULL);
        assert(boxrig_terminal_import_checkpoint(wire, length, limit, &copy)
               == BOXRIG_STATUS_OK);
        feed(copy, tail);
        for (size_t row = 0; row < config.rows; ++row) {
            for (size_t col = 0; col < config.columns; ++col) {
                boxrig_cell_view a, b;
                assert(!boxrig_terminal_get_cell(original, row, col, &a));
                assert(!boxrig_terminal_get_cell(copy, row, col, &b));
                assert(a.text_length == b.text_length && a.attributes == b.attributes);
                assert(!a.text_length || !memcmp(a.text, b.text, a.text_length));
            }
        }
        const uint8_t *first, *second;
        size_t count;
        while ((count = boxrig_terminal_output_peek(original, &first))) {
            assert(boxrig_terminal_output_peek(copy, &second) == count);
            assert(!memcmp(first, second, count));
            assert(!boxrig_terminal_output_consume(original, count));
            assert(!boxrig_terminal_output_consume(copy, count));
        }
        assert(!boxrig_terminal_output_peek(copy, &second));
        boxrig_terminal_destroy(copy);
        boxrig_terminal_destroy(original);
        boxrig_checkpoint_free(wire);
        exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    boxrig_checkpoint_free(wire);
    boxrig_terminal_destroy(original);
}

int main(void)
{
    round_trip("test\xe3\x81", "\x82");
    round_trip("line\r\n\x1b[?1049hALT\x1b[3", "1mred\x1b[?1049l\x1b[6n");
    round_trip("\x1b[6n\x1b]8;;https://example.", "org\x1b\\link\x1b]8;;\x1b\\");
    round_trip("\x1bP$q", "m\x1b\\");
    return 0;
}
