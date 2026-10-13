/* A relocated core-only installation needs no window-system SDK. */
#include <boxrig/terminal.h>
#include <stdio.h>

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    size_t consumed = 0, rows = 0, columns = 0;
    boxrig_cell_view cell;
    const uint8_t text[] = "installed core";
    config.rows = 4;
    config.columns = 20;
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK) return 1;
    if (boxrig_terminal_feed(terminal, text, sizeof(text) - 1, &consumed) !=
        BOXRIG_STATUS_OK || consumed != sizeof(text) - 1) return 1;
    if (boxrig_terminal_get_cell(terminal, 0, 0, &cell) != BOXRIG_STATUS_OK ||
        cell.text_length != 1 || cell.text[0] != 'i') return 1;
    boxrig_terminal_size(terminal, &rows, &columns);
    boxrig_terminal_destroy(terminal);
    if (rows != 4 || columns != 20) return 1;
    puts("PASS installed core-only terminal");
    return 0;
}
