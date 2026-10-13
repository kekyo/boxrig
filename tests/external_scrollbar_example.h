/* Exercise each frontend's application-owned bar recipe with a native component. */
#ifndef BOXRIG_TEST_EXTERNAL_SCROLLBAR_EXAMPLE_H
#define BOXRIG_TEST_EXTERNAL_SCROLLBAR_EXAMPLE_H

typedef struct external_bar_observation {
    example_external_scrollbar *bar;
    boxrig_scrollbar_state state;
    unsigned int notifications;
    bool feedback_suppressed;
} external_bar_observation;

static void observe_external_bar(void *context, const boxrig_scrollbar_state *state)
{
    external_bar_observation *observation = context;
    observation->state = *state;
    observation->notifications++;
    bool changed = true;
    /* A native adjustment can clamp and notify while its range is being set.
     * Such a notification must not scroll the terminal to a temporary value. */
    observation->feedback_suppressed = example_external_scrollbar_action(
        observation->bar, BOXRIG_SCROLLBAR_TOP, 0, &changed) == BOXRIG_UI_OK && !changed;
}

static bool check_external_scrollbar_example(
    example_external_scrollbar *bar, boxrig_scrollbar_state *state_out)
{
    external_bar_observation observation = {.bar = bar};
    if (example_external_scrollbar_refresh(bar, observe_external_bar, &observation) != BOXRIG_UI_OK ||
        observation.notifications != 1 || !observation.feedback_suppressed ||
        example_external_scrollbar_refresh(bar, observe_external_bar, &observation) != BOXRIG_UI_OK ||
        observation.notifications != 1) return false;
    bool changed = true;
    if (example_external_scrollbar_action(bar, BOXRIG_SCROLLBAR_POSITION,
            observation.state.model.position, &changed) != BOXRIG_UI_OK || changed) return false;
    *state_out = observation.state;
    return true;
}
#endif
