/* boxrig - Under MIT. */
#include "boxrig/terminal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail_allocation;
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size)
{
    return fail_allocation ? NULL : __real_calloc(count, size);
}

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_animation_clock *clock = NULL;
    boxrig_animation_clock *other = NULL;
    const uint8_t batch[] = "\033[?2026h";
    uint64_t deadline = 0;
    size_t consumed = 0;

    config.rows = 2;
    config.columns = 4;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_update_monotonic_time(terminal, 100) ==
           BOXRIG_STATUS_OK);
    fail_allocation = true;
    assert(boxrig_terminal_acquire_animation_clock(terminal, &clock) ==
           BOXRIG_STATUS_OUT_OF_MEMORY);
    fail_allocation = false;
    assert(clock == NULL);
    assert(boxrig_terminal_update_monotonic_time(terminal, 101) ==
           BOXRIG_STATUS_OK);
    assert(boxrig_terminal_acquire_animation_clock(terminal, &clock) ==
           BOXRIG_STATUS_OK);
    assert(clock != NULL);
    assert(boxrig_terminal_acquire_animation_clock(terminal, &other) ==
           BOXRIG_STATUS_BUSY);
    assert(other == NULL);
    assert(boxrig_terminal_update_monotonic_time(terminal, 9999) ==
           BOXRIG_STATUS_BUSY);
    assert(boxrig_animation_clock_update_monotonic_time(clock, 100) ==
           BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(boxrig_animation_clock_update_monotonic_time(clock, 200) ==
           BOXRIG_STATUS_OK);
    assert(boxrig_terminal_feed(terminal, batch, sizeof(batch) - 1,
                                &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == sizeof(batch) - 1);
    assert(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    assert(boxrig_animation_clock_update_monotonic_time(clock, 200) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    assert(deadline == 1200);
    assert(boxrig_animation_clock_update_monotonic_time(clock, 1199) ==
           BOXRIG_STATUS_OK);
    assert(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    assert(boxrig_animation_clock_update_monotonic_time(clock, 1200) ==
           BOXRIG_STATUS_OK);
    assert(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    boxrig_animation_clock_release(clock);
    assert(boxrig_terminal_update_monotonic_time(terminal, 1201) ==
           BOXRIG_STATUS_OK);
    assert(boxrig_terminal_acquire_animation_clock(terminal, &clock) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_feed(terminal, batch, sizeof(batch) - 1, &consumed) == BOXRIG_STATUS_OK);
    assert(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    assert(boxrig_animation_clock_update_monotonic_time(clock, 20000) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_next_animation_deadline(terminal, &deadline) && deadline == 21000);
    assert(boxrig_terminal_feed(terminal, batch, sizeof(batch) - 1, &consumed) == BOXRIG_STATUS_OK);
    assert(boxrig_animation_clock_update_monotonic_time(clock, 20500) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_next_animation_deadline(terminal, &deadline) && deadline == 21000);
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)"X", 1, &consumed) == BOXRIG_STATUS_OK);
    assert(boxrig_animation_clock_update_monotonic_time(clock, 21000) == BOXRIG_STATUS_OK);
    assert(consumed == 1 && !boxrig_terminal_next_animation_deadline(terminal, &deadline));
    boxrig_cell_view cell;
    assert(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    assert(cell.text_length == 1 && cell.text[0] == 'X');
    assert(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) == BOXRIG_STATUS_OK);
    const uint8_t animation[] =
        "\033_Ga=T,i=531,f=32,s=1,v=1,c=1,r=1,C=1;/wAA/w==\033\\"
        "\033_Ga=f,i=531,f=32,s=1,v=1,X=1,z=20;AP8A/w==\033\\"
        "\033_Ga=a,i=531,r=1,z=10,s=3,v=1,c=1\033\\";
    assert(boxrig_terminal_feed(terminal, animation, sizeof(animation) - 1,
        &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == sizeof(animation) - 1);
    assert(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    uint64_t image_revision = boxrig_terminal_image_revision(terminal);
    assert(boxrig_animation_clock_update_monotonic_time(clock, 90000) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_image_revision(terminal) == image_revision);
    assert(boxrig_terminal_next_animation_deadline(terminal, &deadline) && deadline == 90010);
    assert(boxrig_animation_clock_update_monotonic_time(clock, 90010) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_image_revision(terminal) == image_revision + 1);
    assert(boxrig_terminal_next_animation_deadline(terminal, &deadline) && deadline == 90030);
    boxrig_terminal_destroy(terminal);
    assert(boxrig_animation_clock_update_monotonic_time(clock, 1202) ==
           BOXRIG_STATUS_STALE);
    boxrig_animation_clock_release(clock);
    boxrig_animation_clock_release(NULL);
    assert(boxrig_animation_clock_update_monotonic_time(NULL, 0) ==
           BOXRIG_STATUS_INVALID_ARGUMENT);
    puts("PASS exclusive animation clock ownership and lifetime");
    return 0;
}
