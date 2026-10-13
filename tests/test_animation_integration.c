/* boxrig - Under MIT. */
#include "boxrig/ui/animation.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned int model_clock_updates;
boxrig_status __real_boxrig_animation_clock_update_monotonic_time(
    boxrig_animation_clock *clock, uint64_t now);
boxrig_status __wrap_boxrig_animation_clock_update_monotonic_time(
    boxrig_animation_clock *clock, uint64_t now)
{
    model_clock_updates++;
    return __real_boxrig_animation_clock_update_monotonic_time(clock, now);
}

typedef struct scheduler_state {
    uint64_t now, deadline, generation;
    unsigned int reservations;
    bool armed;
} scheduler_state;

static boxrig_ui_status read_time(void *context, uint64_t *now_out)
{
    *now_out = ((scheduler_state *)context)->now;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status reserve(void *context, uint64_t deadline,
    uint64_t generation)
{
    scheduler_state *state = context;
    assert(deadline > state->now && generation != 0);
    state->deadline = deadline;
    state->generation = generation;
    state->armed = true;
    state->reservations++;
    return BOXRIG_UI_OK;
}

static void cancel(void *context)
{
    ((scheduler_state *)context)->armed = false;
}

static void request_frame(void *context)
{
    (*(unsigned int *)context)++;
}

static void feed(boxrig_terminal *terminal, const char *text)
{
    size_t consumed = 0;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text),
        &consumed) == BOXRIG_STATUS_OK && consumed == strlen(text));
}

static void accept_frame(boxrig_ui_view *view)
{
    for (int layer = 0; layer < BOXRIG_UI_DIRTY_LAYER_COUNT; layer++) {
        boxrig_ui_dirty dirty;
        while (boxrig_ui_view_peek_dirty(view, (boxrig_ui_dirty_layer)layer,
                &dirty))
            assert(boxrig_ui_view_consume_dirty(view,
                (boxrig_ui_dirty_layer)layer) == BOXRIG_UI_OK);
    }
}

static void deliver(boxrig_ui_animation_runtime *runtime,
    scheduler_state *timer, uint64_t now)
{
    timer->now = now;
    timer->armed = false;
    model_clock_updates = 0;
    assert(boxrig_ui_animation_runtime_dispatch(runtime, timer->generation) == BOXRIG_UI_OK);
    assert(model_clock_updates == 1);
}

static void test_frame_deletion_reschedules_shared_clock(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 1;
    config.columns = 1;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_animation_runtime *runtime = NULL;
    scheduler_state timer = {.now = 100};
    boxrig_ui_animation_scheduler scheduler = {&timer, read_time, reserve, cancel};
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) == BOXRIG_STATUS_OK);
    feed(terminal,
        "\033[?25l\033_Ga=T,i=1,f=32,s=1,v=1,c=1,r=1,C=1,q=2;/wAA/w==\033\\"
        "\033_Ga=f,i=1,f=32,s=1,v=1,X=1,z=20,q=2;AP8A/w==\033\\"
        "\033_Ga=f,i=1,f=32,s=1,v=1,X=1,z=30,q=2;AAD//w==\033\\"
        "\033_Ga=a,i=1,r=1,z=10,c=2,s=3,v=1,q=2\033\\");
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_runtime_create(&scheduler, &runtime) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_runtime_attach(runtime, view) == BOXRIG_UI_OK);
    assert(timer.armed && timer.deadline == 120);
    accept_frame(view);
    timer.now = 105;
    model_clock_updates = 0;
    feed(terminal, "\033_Ga=d,d=f,i=1,r=2,q=2\033\\");
    assert(model_clock_updates == 0);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(timer.armed && timer.deadline == 135);
    boxrig_ui_dirty dirty;
    assert(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_IMAGE, &dirty));
    accept_frame(view);
    deliver(runtime, &timer, 135);
    assert(timer.armed && timer.deadline == 145);
    timer.now = 137;
    feed(terminal, "\033_Ga=d,d=F,i=1,r=2,q=2\033\\");
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(timer.armed && timer.deadline == 145);
    feed(terminal, "\033_Ga=d,d=F,i=1,q=2\033\\");
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(!timer.armed && boxrig_terminal_image_count(terminal) == 0);
    boxrig_ui_view_destroy(view);
    boxrig_ui_animation_runtime_destroy(runtime);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS frame deletion dirty and shared clock rescheduling");
}

int main(void)
{
    test_frame_deletion_reschedules_shared_clock();
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    config.rows = 3;
    config.columns = 8;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *first = NULL, *second = NULL;
    boxrig_ui_animation_runtime *runtime = NULL;
    scheduler_state timer = {0};
    boxrig_ui_animation_scheduler scheduler = {&timer, read_time, reserve, cancel};
    unsigned int first_frames = 0, second_frames = 0;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) == BOXRIG_STATUS_OK);
    feed(terminal, "\033[5mS\033[6mR\033[0m\033[2 q\033[3;1H"
        "\033_Ga=T,i=531,f=32,s=1,v=1,c=1,r=1,C=1;/wAA/w==\033\\"
        "\033_Ga=f,i=531,f=32,s=1,v=1,X=1,z=20;AP8A/w==\033\\"
        "\033_Ga=a,i=531,r=1,z=10,s=3,v=1,c=1\033\\");
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &first) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &second) == BOXRIG_UI_OK);
    boxrig_ui_cursor_style style = boxrig_ui_cursor_style_default();
    style.blink = BOXRIG_UI_CURSOR_BLINK_ENABLED;
    assert(boxrig_ui_view_set_cursor_style(first, &style) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_focused(first, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_focused(second, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_frame_request_callback(first, request_frame, &first_frames) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_frame_request_callback(second, request_frame, &second_frames) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_runtime_create(&scheduler, &runtime) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_runtime_attach(runtime, first) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_runtime_attach(runtime, second) == BOXRIG_UI_OK);
    assert(timer.armed && timer.deadline == 10);
    accept_frame(first);
    accept_frame(second);
    first_frames = second_frames = 0;
    uint64_t image_revision = boxrig_terminal_image_revision(terminal);
    deliver(runtime, &timer, 10);
    assert(boxrig_terminal_image_revision(terminal) == image_revision + 1);
    assert(first_frames == 1 && second_frames == 1 && timer.deadline == 30);
    /* Pending frames coalesce; another image notification cannot render or
     * erase unpresented dirty. Both views still receive the latest state. */
    deliver(runtime, &timer, 30);
    assert(first_frames == 1 && second_frames == 1);
    boxrig_ui_dirty dirty;
    assert(boxrig_ui_view_peek_dirty(first, BOXRIG_UI_DIRTY_IMAGE, &dirty));
    assert(boxrig_ui_view_peek_dirty(second, BOXRIG_UI_DIRTY_IMAGE, &dirty));
    deliver(runtime, &timer, 150);
    assert(!boxrig_ui_view_text_blink_visible(first,
        BOXRIG_CELL_BLINK | BOXRIG_CELL_RAPID_BLINK));
    assert(!boxrig_ui_view_text_blink_visible(second,
        BOXRIG_CELL_BLINK | BOXRIG_CELL_RAPID_BLINK));
    deliver(runtime, &timer, 500);
    assert(!boxrig_ui_view_cursor_blink_visible(first));
    assert(boxrig_ui_view_cursor_blink_visible(second));
    assert(boxrig_ui_view_set_composition_active(first, true) == BOXRIG_UI_OK);
    uint64_t cursor_only_deadline = 0;
    assert(boxrig_ui_view_next_animation_deadline(first, &cursor_only_deadline));
    assert(cursor_only_deadline == 600);
    deliver(runtime, &timer, UINT64_C(1000000000500));
    assert(!boxrig_ui_view_text_blink_visible(first, BOXRIG_CELL_BLINK));
    assert(!boxrig_ui_view_text_blink_visible(second, BOXRIG_CELL_BLINK));
    /* Visible views may stop independently while the shared image clock is
     * retained. Removing the image removes the final shared deadline. */
    assert(boxrig_ui_view_set_visible(first, false) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_visible(second, false) == BOXRIG_UI_OK);
    assert(timer.armed);
    feed(terminal, "\033_Ga=d,d=I,i=531\033\\");
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(!timer.armed);
    uint64_t obsolete_generation = timer.generation;
    unsigned int reservations = timer.reservations;
    model_clock_updates = 0;
    assert(boxrig_ui_animation_runtime_dispatch(runtime, obsolete_generation) == BOXRIG_UI_OK);
    assert(model_clock_updates == 0 && timer.reservations == reservations);
    assert(boxrig_ui_view_set_visible(second, true) == BOXRIG_UI_OK);
    assert(timer.armed);
    boxrig_ui_view_destroy(first);
    boxrig_ui_view_destroy(second);
    assert(!timer.armed);
    model_clock_updates = 0;
    assert(boxrig_ui_animation_runtime_dispatch(runtime, timer.generation) == BOXRIG_UI_OK);
    assert(model_clock_updates == 0);
    boxrig_ui_animation_runtime_destroy(runtime);
    assert(boxrig_terminal_update_monotonic_time(terminal, timer.now + 1) == BOXRIG_STATUS_OK);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS shared image/text/style clocks, deferred frames and teardown");
    return 0;
}
