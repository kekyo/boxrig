/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/gtk4.h"

#include <stdio.h>

int main(void)
{
    boxrig_gtk4_terminal_widget_config config =
        boxrig_gtk4_terminal_widget_config_default();

    if (config.attachment != BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY ||
        config.paste_capacity == 0 || config.paste_chunk_size == 0 ||
        config.maximum_native_input_events == 0 ||
        config.maximum_frame_items == 0 ||
        config.maximum_clipboard_transfers == 0 ||
        config.maximum_clipboard_bytes == 0) {
        return 1;
    }
    printf("PASS installed GTK 4 package compiles links and runs\n");
    return 0;
}
