/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "root_backing_policy.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

int main(void)
{
    boxrig_term_wayland_root_slot slots[2] = {{0}};
    boxrig_term_wayland_root_candidate candidate;

    CHECK(BOXRIG_TERM_WAYLAND_ROOT_BUFFER_COUNT == 2);
    CHECK(BOXRIG_TERM_WAYLAND_ROOT_BYTES_DEFAULT ==
          (size_t)(256u * 1024u * 1024u));
    CHECK(BOXRIG_TERM_WAYLAND_ROOT_BYTES_MAXIMUM ==
          (size_t)(1024u * 1024u * 1024u));
    CHECK(boxrig_term_wayland_root_clear_pixel(true) == UINT32_C(0));
    CHECK(boxrig_term_wayland_root_clear_pixel(false) ==
          UINT32_C(0xff202430));
    CHECK(boxrig_term_wayland_root_candidate_action(
              BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_OK) ==
          BOXRIG_TERM_WAYLAND_ROOT_UPDATE_APPLY);
    CHECK(boxrig_term_wayland_root_candidate_action(
              BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_BUSY) ==
          BOXRIG_TERM_WAYLAND_ROOT_UPDATE_RETRY);
    CHECK(boxrig_term_wayland_root_candidate_action(
              BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_LIMIT) ==
          BOXRIG_TERM_WAYLAND_ROOT_UPDATE_REJECT);
    CHECK(boxrig_term_wayland_root_candidate_action(
              BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_INVALID) ==
          BOXRIG_TERM_WAYLAND_ROOT_UPDATE_FAIL);
    CHECK(boxrig_term_wayland_root_choose_candidate(
              slots,
              BOXRIG_TERM_WAYLAND_ROOT_BUFFER_COUNT,
              BOXRIG_TERM_WAYLAND_ROOT_BUFFER_COUNT,
              BOXRIG_TERM_WAYLAND_ROOT_BYTES_DEFAULT,
              4096,
              &candidate) == BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_OK);
    CHECK(candidate.index == 0 && !candidate.recycle_released);

    slots[0].allocated = true;
    slots[0].bytes = 4096;
    slots[1].allocated = true;
    slots[1].bytes = 4096;
    CHECK(boxrig_term_wayland_root_choose_candidate(
              slots,
              2,
              0,
              BOXRIG_TERM_WAYLAND_ROOT_BYTES_DEFAULT,
              4096,
              &candidate) == BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_BUSY);

    slots[1].released = true;
    CHECK(boxrig_term_wayland_root_choose_candidate(
              slots,
              2,
              0,
              8192,
              4096,
              &candidate) == BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_OK);
    CHECK(candidate.index == 1 && candidate.recycle_released);
    CHECK(boxrig_term_wayland_root_choose_candidate(
              slots,
              2,
              0,
              8191,
              4096,
              &candidate) == BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_LIMIT);
    CHECK(boxrig_term_wayland_root_choose_candidate(
              slots,
              2,
              0,
              BOXRIG_TERM_WAYLAND_ROOT_BYTES_MAXIMUM + 1,
              4096,
              &candidate) == BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_INVALID);

    printf("PASS native Wayland root backing bounds and candidate retention\n");
    return 0;
}
