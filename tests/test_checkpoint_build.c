/* boxrig - Under MIT. Exchange a checkpoint between two public SDK builds. */
#include <boxrig/terminal.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    assert(argc == 3);
    const size_t budget = 32u << 20;
    boxrig_terminal *terminal = NULL;
    if (!strcmp(argv[1], "write")) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        config.scrollback_capacity = 0;
        assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        uint8_t *bytes;
        size_t length;
        assert(boxrig_terminal_export_checkpoint(terminal, budget, &bytes, &length) == BOXRIG_STATUS_OK);
        FILE *file = fopen(argv[2], "wb");
        assert(file && fwrite(bytes, 1, length, file) == length && fclose(file) == 0);
        boxrig_checkpoint_free(bytes);
    } else {
        FILE *file = fopen(argv[2], "rb");
        assert(file && !fseek(file, 0, SEEK_END));
        long length = ftell(file);
        assert(length > 0 && (unsigned long)length <= budget);
        rewind(file);
        uint8_t *bytes = malloc((size_t)length);
        assert(bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length && fclose(file) == 0);
        boxrig_status status = boxrig_terminal_import_checkpoint(bytes, (size_t)length, budget, &terminal);
        assert(status == (!strcmp(argv[1], "read") ? BOXRIG_STATUS_OK : BOXRIG_STATUS_INVALID_ARGUMENT));
        free(bytes);
    }
    boxrig_terminal_destroy(terminal);
}
