/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "boxrig/ui/composition.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            goto cleanup;                                                       \
        }                                                                       \
    } while (0)

int main(void)
{
    static const uint8_t text[] = {
        'A',
        0xc2u,
        0xb7u,
        'e',
        0xccu,
        0x81u,
        0xe7u,
        0x95u,
        0x8cu,
        0xe2u,
        0x9du,
        0xa4u,
        0xefu,
        0xb8u,
        0x8fu,
    };
    static const size_t expected_lengths[] = {1u, 2u, 3u, 3u, 6u};
    static const size_t expected_widths[] = {1u, 2u, 1u, 2u, 2u};
    static const char ambiguous[] = "\302\267";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_composition *composition = NULL;
    boxrig_composition_event event;
    boxrig_composition_cluster_view cluster;
    boxrig_cell_view cell;
    size_t offset = 0;
    size_t index;
    size_t consumed = 0;
    int result = 1;

    config.rows = 2;
    config.columns = 8;
    config.ambiguous_width = BOXRIG_AMBIGUOUS_WIDTH_WIDE;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (index = 0;
         index < sizeof(expected_lengths) / sizeof(expected_lengths[0]);
         index++) {
        boxrig_text_cluster_measurement measurement;

        CHECK(boxrig_terminal_measure_text_cluster(
                  terminal,
                  text + offset,
                  sizeof(text) - offset,
                  &measurement) == BOXRIG_STATUS_OK);
        CHECK(measurement.text_length == expected_lengths[index]);
        CHECK(measurement.column_width == expected_widths[index]);
        CHECK(!measurement.split_by_limit);
        offset += measurement.text_length;
    }
    CHECK(offset == sizeof(text));

    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = ambiguous;
    event.text_length = sizeof(ambiguous) - 1;
    event.caret_offset = sizeof(ambiguous) - 1;
    event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_cluster(composition, 0, &cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(cluster.column_count == 2);

    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)ambiguous,
              sizeof(ambiguous) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(ambiguous) - 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == sizeof(ambiguous) - 1);
    CHECK(memcmp(cell.text, ambiguous, cell.text_length) == 0);
    CHECK(cell.column_width == 2);
    result = 0;

cleanup:
    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    if (result == 0) {
        printf("PASS installed custom UI cluster measurement\n");
    }
    return result;
}
