/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

int main(void)
{
    static const uint8_t input[] =
        "\x1b]99;i=second:d=0:e=1;U2Vj\x07"
        "\x1b]99;i=second:e=1;b25k\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    size_t consumed = 0;

    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_NOTIFICATION);
    CHECK(item.notification != NULL);
    CHECK(item.notification->title.length == 6);
    CHECK(memcmp(item.notification->title.bytes, "Second", 6) == 0);
    CHECK(item.notification->body.length == 0);
    boxrig_terminal_destroy(terminal);
    puts("empty notification body tests passed");
    return 0;
}
