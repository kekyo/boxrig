/* boxrig - Under MIT. */
#include "boxrig/ui/animation.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fake_scheduler {
    uint64_t now;
    uint64_t deadline;
    uint64_t generation;
    unsigned reservations;
    unsigned cancellations;
    unsigned clock_reads;
    bool armed;
    bool fail;
} fake_scheduler;

static boxrig_ui_status read_time(void *context, uint64_t *time_out)
{
    ((fake_scheduler *)context)->clock_reads++;
    *time_out = ((fake_scheduler *)context)->now;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status reserve(
    void *context, uint64_t deadline, uint64_t generation)
{
    fake_scheduler *scheduler = context;
    if (scheduler->fail) return BOXRIG_UI_UNAVAILABLE;
    scheduler->deadline = deadline;
    scheduler->generation = generation;
    scheduler->armed = true;
    scheduler->reservations++;
    return BOXRIG_UI_OK;
}

static void cancel(void *context)
{
    fake_scheduler *scheduler = context;
    scheduler->armed = false;
    scheduler->cancellations++;
}

static boxrig_ui_status deliver(
    boxrig_ui_animation_runtime *runtime, fake_scheduler *timer,
    uint64_t generation)
{
    /* A matching one-shot is no longer armed when the host delivers it. */
    if (timer->generation == generation) timer->armed = false;
    return boxrig_ui_animation_runtime_dispatch(runtime, generation);
}

static void request_frame(void *context)
{
    (*(unsigned *)context)++;
}

static void clear_dirty(boxrig_ui_view *view)
{
    for (int layer = 0; layer < BOXRIG_UI_DIRTY_LAYER_COUNT; layer++) {
        boxrig_ui_dirty dirty;
        while (boxrig_ui_view_peek_dirty(view, (boxrig_ui_dirty_layer)layer,
                                          &dirty)) {
            assert(boxrig_ui_view_consume_dirty(view, (boxrig_ui_dirty_layer)layer) == BOXRIG_UI_OK);
        }
    }
}

static void feed(boxrig_terminal *terminal, const char *text)
{
    size_t consumed = 0;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text),
                                &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == strlen(text));
}

static void test_synchronized_frame_resume(void)
{
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *first = NULL, *second = NULL;
    boxrig_ui_animation_runtime *runtime = NULL;
    fake_scheduler timer = {0};
    boxrig_ui_animation_scheduler scheduler = {
        &timer, read_time, reserve, cancel
    };
    unsigned first_requests = 0, second_requests = 0;
    tc.rows = 2;
    tc.columns = 8;
    vc.blink.cursor_enabled = false;
    vc.blink.text_enabled = false;
    assert(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &vc, &first) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &vc, &second) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_runtime_create(&scheduler, &runtime) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_runtime_attach(runtime, first) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_runtime_attach(runtime, second) == BOXRIG_UI_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    clear_dirty(first);
    clear_dirty(second);
    assert(boxrig_ui_view_set_frame_request_callback(first, request_frame,
        &first_requests) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_frame_request_callback(second, request_frame,
        &second_requests) == BOXRIG_UI_OK);

    feed(terminal, "\033[?2026hheld");
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(first_requests == 1 && second_requests == 1);
    assert(boxrig_terminal_synchronized_output(terminal));
    assert(timer.armed && timer.deadline == 1000);
    /* The renderer retains dirty while presentation is held. Expiry must
     * request a frame even though the view never became clean in between. */
    first_requests = second_requests = 0;
    timer.now = 1000;
    uint64_t expired_generation = timer.generation;
    assert(deliver(runtime, &timer, expired_generation) == BOXRIG_UI_OK);
    assert(!boxrig_terminal_synchronized_output(terminal));
    assert(first_requests == 1 && second_requests == 1);
    assert(boxrig_ui_view_frame_requested(first));
    assert(!timer.armed);
    assert(deliver(runtime, &timer, expired_generation) == BOXRIG_UI_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(first_requests == 1 && second_requests == 1);

    clear_dirty(first);
    clear_dirty(second);
    assert(boxrig_ui_view_set_visible(second, false) == BOXRIG_UI_OK);
    feed(terminal, "\033[?2026hmore");
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    first_requests = second_requests = 0;
    feed(terminal, "\033[?2026l");
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(first_requests == 1 && second_requests == 0);
    assert(!timer.armed);
    assert(boxrig_ui_view_set_visible(second, true) == BOXRIG_UI_OK);
    assert(second_requests == 1);

    boxrig_ui_view_destroy(first);
    boxrig_ui_view_destroy(second);
    boxrig_ui_animation_runtime_destroy(runtime);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS synchronized output resumes pending frames once per visible view");
}

int main(void)
{
    test_synchronized_frame_resume();
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *first = NULL, *second = NULL;
    boxrig_ui_animation_runtime *runtime = NULL, *other = NULL;
    fake_scheduler timer = {0}, other_timer = {0};
    boxrig_ui_animation_scheduler scheduler = {
        &timer, read_time, reserve, cancel
    };
    boxrig_ui_animation_scheduler other_scheduler = {
        &other_timer, read_time, reserve, cancel
    };
    boxrig_ui_blink_config blink = boxrig_ui_blink_config_default(), actual;
    uint64_t deadline = 0;
    unsigned frame_requests = 0;

    terminal_config.rows = 3;
    terminal_config.columns = 10;
    assert(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
    feed(terminal, "\033[1 q");
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &first) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_focused(first, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_frame_request_callback(first, request_frame,
                                                    &frame_requests) == BOXRIG_UI_OK);
    clear_dirty(first);
    assert(boxrig_ui_view_update_monotonic_time(first, 0) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_next_animation_deadline(first, &deadline) && deadline == 500);
    assert(boxrig_ui_view_update_monotonic_time(first, 499) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_cursor_blink_visible(first));
    assert(frame_requests == 0);
    assert(boxrig_ui_view_update_monotonic_time(first, 500) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_cursor_blink_visible(first));
    assert(frame_requests == 1);
    clear_dirty(first);
    assert(boxrig_ui_view_update_monotonic_time(first, 500) == BOXRIG_UI_OK);
    assert(frame_requests == 1);
    assert(boxrig_ui_view_update_monotonic_time(first, 499) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(!boxrig_ui_view_cursor_blink_visible(first));
    assert(boxrig_ui_view_update_monotonic_time(first, UINT64_C(1000000001000)) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_cursor_blink_visible(first));
    assert(boxrig_ui_view_update_monotonic_time(first, UINT64_C(1000000001500)) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_cursor_blink_visible(first));
    assert(boxrig_ui_view_notify_local_input(first) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_cursor_blink_visible(first));
    assert(boxrig_ui_view_set_focused(first, false) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_next_animation_deadline(first, &deadline));
    assert(boxrig_ui_view_set_focused(first, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_next_animation_deadline(first, &deadline));
    assert(boxrig_ui_view_set_composition_active(first, true) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_next_animation_deadline(first, &deadline));
    assert(boxrig_ui_view_set_composition_active(first, false) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_visible(first, false) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_next_animation_deadline(first, &deadline));
    assert(boxrig_ui_view_set_visible(first, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_cursor_blink_visible(first));
    boxrig_ui_view_destroy(first);
    assert(boxrig_ui_view_create(model, &view_config, &first) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &second) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_focused(first, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_focused(second, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_frame_request_callback(first, request_frame,
                                                    &frame_requests) == BOXRIG_UI_OK);
    timer.now = 10000;
    assert(boxrig_ui_animation_runtime_create(&scheduler, &runtime) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_runtime_create(&other_scheduler, &other) == BOXRIG_UI_OK);
    timer.fail = true;
    assert(boxrig_ui_animation_runtime_attach(runtime, first) == BOXRIG_UI_UNAVAILABLE);
    assert(!timer.armed);
    assert(boxrig_terminal_update_monotonic_time(terminal, 10000) == BOXRIG_STATUS_OK);
    timer.fail = false;
    assert(boxrig_ui_animation_runtime_attach(runtime, first) == BOXRIG_UI_OK);
    assert(timer.armed && timer.deadline == 10500);
    assert(boxrig_ui_animation_runtime_attach(runtime, second) == BOXRIG_UI_OK);
    assert(timer.reservations == 1);
    assert(boxrig_ui_animation_runtime_attach(other, second) == BOXRIG_UI_BUSY);
    assert(boxrig_terminal_update_monotonic_time(terminal, 10000) == BOXRIG_STATUS_BUSY);
    assert(boxrig_ui_view_update_monotonic_time(first, 10000) == BOXRIG_UI_BUSY);
    uint64_t old_generation = timer.generation;
    blink.cursor.visible_milliseconds = 100;
    blink.cursor.hidden_milliseconds = 200;
    timer.fail = true;
    assert(boxrig_ui_view_set_blink_config(first, &blink) == BOXRIG_UI_UNAVAILABLE);
    assert(boxrig_ui_view_get_blink_config(first, &actual) == BOXRIG_UI_OK);
    assert(actual.cursor.visible_milliseconds == 500);
    assert(timer.deadline == 10500 && timer.generation == old_generation);
    timer.fail = false;
    assert(boxrig_ui_view_set_blink_config(first, &blink) == BOXRIG_UI_OK);
    assert(timer.deadline == 10100);
    timer.now = 10100;
    clear_dirty(first);
    frame_requests = 0;
    assert(deliver(runtime, &timer, old_generation) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_cursor_blink_visible(first) && frame_requests == 0);
    assert(deliver(runtime, &timer, timer.generation) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_cursor_blink_visible(first) && frame_requests == 1);
    assert(boxrig_ui_view_cursor_blink_visible(second));
    assert(timer.deadline == 10300);
    old_generation = timer.generation;
    boxrig_ui_view_destroy(first);
    first = NULL;
    assert(timer.deadline == 10500);
    assert(deliver(runtime, &timer, old_generation) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_cursor_blink_visible(second));
    assert(boxrig_ui_view_set_focused(second, false) == BOXRIG_UI_OK);
    assert(!timer.armed);
    timer.fail = true;
    assert(boxrig_ui_view_set_focused(second, true) == BOXRIG_UI_UNAVAILABLE);
    assert(!boxrig_ui_view_focused(second));
    timer.fail = false;
    assert(boxrig_ui_view_set_focused(second, true) == BOXRIG_UI_OK);
    assert(timer.deadline == 10600);
    assert(boxrig_ui_view_set_visible(second, false) == BOXRIG_UI_OK);
    assert(!timer.armed);
    feed(terminal, "\033[?2026h");
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(timer.armed && timer.deadline == 11100);
    timer.now = 11100;
    assert(deliver(runtime, &timer, timer.generation) == BOXRIG_UI_OK);
    assert(!timer.armed);
    assert(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    /* Receiving data never invokes a host callback. Normal model synchronization
     * starts newly received timed content after an arbitrarily long idle wait. */
    timer.now = 50000;
    unsigned reads_before_feed = timer.clock_reads;
    feed(terminal, "\033[?2026h");
    assert(timer.clock_reads == reads_before_feed);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(timer.armed && timer.deadline == 51000);
    timer.now = 51000;
    assert(deliver(runtime, &timer, timer.generation) == BOXRIG_UI_OK);
    assert(!timer.armed);
    assert(boxrig_ui_view_set_visible(second, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_runtime_status(runtime) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_frame_request_callback(second, request_frame,
        &frame_requests) == BOXRIG_UI_OK);
    clear_dirty(second);
    frame_requests = 0;
    timer.now = timer.deadline;
    timer.fail = true;
    assert(deliver(runtime, &timer, timer.generation) == BOXRIG_UI_UNAVAILABLE);
    assert(boxrig_ui_animation_runtime_status(runtime) == BOXRIG_UI_UNAVAILABLE);
    assert(frame_requests == 1 && !boxrig_ui_view_cursor_blink_visible(second));
    timer.fail = false;
    old_generation = timer.generation;
    boxrig_ui_model_destroy(model);
    model = NULL;
    assert(!timer.armed);
    assert(deliver(runtime, &timer, old_generation) == BOXRIG_UI_OK);
    assert(boxrig_terminal_update_monotonic_time(terminal, timer.now + 1) == BOXRIG_STATUS_OK);
    boxrig_ui_view_destroy(second);
    boxrig_ui_animation_runtime_destroy(runtime);
    boxrig_ui_animation_runtime_destroy(other);
    boxrig_terminal_destroy(terminal);
    puts("PASS blink phases, shared scheduling, rollback and stale notifications");
    return 0;
}
