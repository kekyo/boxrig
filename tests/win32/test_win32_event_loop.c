/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "boxrig/win32.h"

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool test_animation_tick_selects_message_wait_deadline(void)
{
    static const uint8_t setup[] =
        "\x1b_Ga=T,i=902,f=32,s=1,v=1,c=1,r=1,C=1;/wAA/w==\x1b\\"
        "\x1b_Ga=f,i=902,f=32,s=1,v=1,X=1,z=20;AP8A/w==\x1b\\"
        "\x1b_Ga=a,i=902,r=1,z=10,s=3,v=1,c=1\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    uint64_t first_sample;
    uint64_t second_sample;
    DWORD timeout = 0;
    size_t consumed = 0;

    CHECK(boxrig_win32_sample_monotonic_time(&first_sample) == 0);
    CHECK(boxrig_win32_sample_monotonic_time(&second_sample) == 0);
    CHECK(second_sample >= first_sample);
    CHECK(boxrig_win32_sample_monotonic_time(NULL) == -1);

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_animation_wait_timeout(
              terminal,
              100,
              &timeout) == 0);
    CHECK(timeout == INFINITE);
    CHECK(boxrig_terminal_feed(
              terminal,
              setup,
              sizeof(setup) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(setup) - 1);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 100) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_animation_wait_timeout(
              terminal,
              100,
              &timeout) == 0);
    CHECK(timeout == 10);
    CHECK(boxrig_win32_animation_wait_timeout(
              terminal,
              109,
              &timeout) == 0);
    CHECK(timeout == 1);
    CHECK(boxrig_win32_animation_wait_timeout(
              terminal,
              110,
              &timeout) == 0);
    CHECK(timeout == 0);
    CHECK(boxrig_win32_animation_wait_timeout(
              terminal,
              100,
              NULL) == -1);
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    if (!test_animation_tick_selects_message_wait_deadline()) {
        return 1;
    }
    printf("PASS Win32 animation tick selects message wait deadline\n");
    return 0;
}
