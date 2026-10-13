/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/gtk3.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    boxrig_gtk3_terminal_widget_config config =
        boxrig_gtk3_terminal_widget_config_default();
    const char *type_name = g_type_name(BOXRIG_GTK3_TYPE_TERMINAL_WIDGET);

    if (config.attachment != BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY ||
        config.paste_capacity == 0 || config.paste_chunk_size == 0 ||
        config.maximum_native_input_events == 0 ||
        config.maximum_frame_items == 0 ||
        config.maximum_clipboard_transfers == 0 ||
        config.maximum_clipboard_bytes == 0 || type_name == NULL ||
        strcmp(type_name, "BoxrigGtk3TerminalWidget") != 0) {
        return 1;
    }
    printf("PASS installed GTK 3 package compiles links and runs\n");
    return 0;
}
