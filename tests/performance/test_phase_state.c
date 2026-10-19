/* Functional completion checks for the test-only GTK3 phase probe. */
#include "phase_state.h"

#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "phase state failed at line %d: %s\n", \
            __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int test_completion_requires_both_phases_and_clean_frame(void)
{
    boxrig_phase_state state = {0};

    boxrig_phase_mark(&state, 9, 100);
    CHECK(!boxrig_phase_try_settle(&state, 9, false, 101));
    boxrig_phase_observe_recognition(&state, 9, 110);
    boxrig_phase_observe_recognition(&state, 9, 115);
    CHECK(state.recognition_ns == 110);
    CHECK(!boxrig_phase_try_settle(&state, 9, false, 111));
    boxrig_phase_observe_cairo(&state, 9, 120);
    CHECK(!boxrig_phase_try_settle(&state, 9, false, 121));
    CHECK(!boxrig_phase_try_settle(&state, 9, true, 122));
    boxrig_phase_observe_frame(&state, 9, 130);
    CHECK(!boxrig_phase_try_settle(&state, 10, false, 132));
    CHECK(boxrig_phase_try_settle(&state, 9, false, 133));
    CHECK(state.settled_ns == 133);
    CHECK(!boxrig_phase_try_settle(&state, 9, false, 134));
    return 0;
}

static int test_pre_mark_completions_and_new_dirty(void)
{
    boxrig_phase_state state = {0};

    boxrig_phase_observe_recognition(&state, 9, 80);
    boxrig_phase_observe_recognition(&state, 9, 81);
    boxrig_phase_observe_cairo(&state, 9, 85);
    boxrig_phase_observe_frame(&state, 9, 90);
    boxrig_phase_mark(&state, 9, 100);
    CHECK(state.recognition_ns == 80);
    CHECK(state.cairo_ns == 85);
    CHECK(state.frame_ns == 90);
    CHECK(boxrig_phase_try_settle(&state, 9, false, 101));

    state = (boxrig_phase_state){0};
    boxrig_phase_observe_cairo(&state, 9, 80);
    boxrig_phase_observe_frame(&state, 9, 85);
    boxrig_phase_mark(&state, 9, 100);
    boxrig_phase_observe_recognition(&state, 9, 110);
    CHECK(!boxrig_phase_try_settle(&state, 9, true, 111));
    CHECK(!boxrig_phase_try_settle(&state, 9, false, 112));
    boxrig_phase_observe_cairo(&state, 9, 120);
    boxrig_phase_observe_frame(&state, 9, 130);
    CHECK(boxrig_phase_try_settle(&state, 9, false, 131));
    return 0;
}

static int test_revision_mismatch_and_frame_order(void)
{
    boxrig_phase_state state = {0};

    boxrig_phase_observe_recognition(&state, 8, 80);
    boxrig_phase_observe_cairo(&state, 8, 85);
    boxrig_phase_observe_frame(&state, 8, 90);
    boxrig_phase_mark(&state, 9, 100);
    CHECK(!boxrig_phase_try_settle(&state, 9, false, 101));
    boxrig_phase_observe_recognition(&state, 9, 110);
    boxrig_phase_observe_frame(&state, 9, 115);
    boxrig_phase_observe_cairo(&state, 9, 120);
    CHECK(!boxrig_phase_try_settle(&state, 9, false, 121));
    boxrig_phase_observe_frame(&state, 9, 130);
    CHECK(boxrig_phase_try_settle(&state, 9, false, 131));
    return 0;
}

int main(void)
{
    if (test_completion_requires_both_phases_and_clean_frame() != 0 ||
        test_pre_mark_completions_and_new_dirty() != 0 ||
        test_revision_mismatch_and_frame_order() != 0) {
        return 1;
    }
    puts("phase state: PASS");
    return 0;
}
