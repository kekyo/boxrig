/* boxrig - Under MIT. */
#include "link_gesture.h"
#include "mouse_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x); return false; } } while (0)

static const char fixture_text[] =
    "\033]8;;https://example.test/a?x=1&y=%22z%22\033\\alpha beta\033]8;;\033\\\r\n"
    "https://example.test/regex\r\nplain text\r\n"
    "\033]8;;file:///tmp/rejected\033\\https://example.test/decoy\033]8;;\033\\";

typedef struct fixture {
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_view *view;
    boxrig_term_text_policy *policy;
    boxrig_term_link_gesture *gesture;
    bool selecting;
    bool completed;
    boxrig_input_mouse_route link_route;
    size_t opened;
    uint32_t multi_click_interval;
    char urls[8][BOXRIG_TERM_LINK_URI_CAPACITY + 1];
} fixture;

static void record_open(void *context, const char *uri, size_t length)
{
    fixture *f = context;
    if (f->opened < 8 && length <= BOXRIG_TERM_LINK_URI_CAPACITY) {
        memcpy(f->urls[f->opened], uri, length);
        f->urls[f->opened][length] = 0;
    }
    f->opened++;
}

static bool synchronize(fixture *f)
{
    CHECK(boxrig_ui_model_synchronize(f->model) == BOXRIG_UI_OK);
    bool more;
    do {
        size_t used;
        CHECK(boxrig_term_text_policy_resume(f->policy, 4096, &used, &more) == 0);
    } while (more);
    return true;
}

static bool feed(fixture *f, const char *bytes)
{
    size_t consumed;
    CHECK(boxrig_terminal_feed(f->terminal, (const uint8_t *)bytes,
        strlen(bytes), &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == strlen(bytes));
    return synchronize(f);
}

static bool create(fixture *f)
{
    f->multi_click_interval = 500;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 4; config.columns = 64;
    config.text_recognizer_event_capacity = 32;
    CHECK(boxrig_terminal_create(&config, &f->terminal) == BOXRIG_STATUS_OK);
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    CHECK(boxrig_ui_model_create(f->terminal, &f->model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(f->model, &view_config, &f->view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(f->view) == BOXRIG_UI_OK);
    boxrig_ui_allocation_input allocation = {0};
    allocation.allocation_width = 512 * BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = 64 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = 8 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = 16 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    boxrig_ui_layout_result layout;
    CHECK(boxrig_ui_view_accept_allocation(f->view, &allocation, &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_term_text_policy_create(f->terminal, f->view, &f->policy) == 0);
    CHECK(boxrig_term_link_gesture_create(f->terminal, f->view, f->policy,
        record_open, f, &f->gesture) == 0);
    return feed(f, fixture_text);
}

static void destroy(fixture *f)
{
    boxrig_term_link_gesture_destroy(f->gesture);
    boxrig_term_text_policy_destroy(f->policy);
    boxrig_ui_view_destroy(f->view);
    boxrig_ui_model_destroy(f->model);
    boxrig_terminal_destroy(f->terminal);
}

static bool mouse_at(fixture *f, boxrig_mouse_action action, int x, int y,
    uint8_t clicks, uint32_t modifiers, uint64_t now)
{
    boxrig_ui_hit_test_result hit;
    CHECK(boxrig_ui_view_hit_test(f->view, x * BOXRIG_UI_FIXED_ONE,
        y * BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    boxrig_mouse_event event = {0};
    event.action = action;
    event.button = action == BOXRIG_MOUSE_MOTION ? BOXRIG_MOUSE_BUTTON_NONE : BOXRIG_MOUSE_BUTTON_LEFT;
    event.pressed_buttons = action == BOXRIG_MOUSE_RELEASE ? 0 : BOXRIG_MOUSE_BUTTON_MASK_LEFT;
    event.modifiers = modifiers; event.click_count = clicks;
    event.x = x; event.y = y; event.row = hit.row; event.column = hit.logical_column;
    f->link_route = boxrig_term_link_gesture_route(f->gesture, &f->selecting,
        &event, &hit, now, f->multi_click_interval,
        4 * BOXRIG_UI_FIXED_ONE, 4 * BOXRIG_UI_FIXED_ONE);
    CHECK(f->link_route != BOXRIG_INPUT_MOUSE_ROUTE_ERROR);
    f->completed = false;
    if (f->link_route == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL) {
        CHECK(boxrig_term_mouse_policy_route_selection(f->terminal, f->view,
            &f->selecting, &event, &hit, &f->completed) != BOXRIG_INPUT_MOUSE_ROUTE_ERROR);
    }
    return true;
}

static bool mouse(fixture *f, boxrig_mouse_action action, size_t row, size_t col,
    uint8_t clicks, uint32_t modifiers, uint64_t now)
{
    return mouse_at(f, action, (int)col * 8 + 2, (int)row * 16 + 8, clicks, modifiers, now);
}

static bool snapshot_is(fixture *f, const char *expected)
{
    boxrig_text_snapshot *snapshot = NULL;
    CHECK(boxrig_ui_view_create_selection_snapshot(f->view, &snapshot) == BOXRIG_UI_OK);
    size_t length;
    const uint8_t *bytes = boxrig_text_snapshot_data(snapshot, &length);
    bool equal = length == strlen(expected) && memcmp(bytes, expected, length) == 0;
    if (!equal) fprintf(stderr, "snapshot expected %zu bytes, got %zu: <%.*s>\n", strlen(expected), length, (int)length, bytes);
    boxrig_text_snapshot_destroy(snapshot);
    CHECK(equal);
    return true;
}

static bool single_click_deadline(fixture *f)
{
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 2, 0, 1, 0, 1));
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 2, 4, 0, 0, 2));
    CHECK(snapshot_is(f, "plain"));
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 1, 0, 100));
    CHECK(f->link_route == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION && !f->selecting);
    CHECK(snapshot_is(f, "plain"));
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 1, 0, 0, 120));
    uint64_t deadline;
    CHECK(boxrig_term_link_gesture_deadline(f->gesture, &deadline) && deadline == 601);
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 600) == 0 && f->opened == 0);
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 601) == 0 && f->opened == 1);
    CHECK(strcmp(f->urls[0], "https://example.test/a?x=1&y=%22z%22") == 0);
    CHECK(!boxrig_term_link_gesture_deadline(f->gesture, &deadline));
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 1000) == 0 && f->opened == 1);
    return snapshot_is(f, "plain");
}

static bool regex_and_adjacent_osc_cell(fixture *f)
{
    CHECK(mouse_at(f, BOXRIG_MOUSE_PRESS, 15, 8, 1, BOXRIG_MODIFIER_CAPS_LOCK, 100));
    CHECK(mouse_at(f, BOXRIG_MOUSE_RELEASE, 17, 8, 0, 0, 110));
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 601) == 0 && f->opened == 1);
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 1, 2, 1, 0, 1000));
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 1, 2, 0, 0, 1001));
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 1501) == 0 && f->opened == 2);
    CHECK(strcmp(f->urls[1], "https://example.test/regex") == 0);
    return true;
}

static bool drag_uses_original_anchor(fixture *f)
{
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 1, 0, 100));
    CHECK(mouse(f, BOXRIG_MOUSE_MOTION, 0, 5, 0, 0, 110));
    CHECK(f->selecting);
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 8, 0, 0, 120));
    CHECK(!f->selecting && f->completed);
    CHECK(snapshot_is(f, "lpha bet"));
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 1000) == 0 && f->opened == 0);
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 0, 1, 0, 1100));
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 4, 0, 0, 1110));
    CHECK(f->completed && snapshot_is(f, "alpha"));
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 2000) == 0 && f->opened == 0);
    return true;
}

static bool multiclick_keeps_selection(fixture *f)
{
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 1, 0, 100));
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 1, 0, 0, 101));
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 2, 0, 600));
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 1, 0, 0, 610));
    CHECK(snapshot_is(f, "alpha"));
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 3, 0, 650));
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 1, 0, 0, 651));
    CHECK(snapshot_is(f, "alpha beta\n"));
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 2000) == 0 && f->opened == 0);
    return true;
}

static bool native_multiclick_promotion(fixture *f)
{
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 1, 0, 100));
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 1, 0, 0, 101));
    /* GDK 3 delivers an ordinary press before its double-click event. */
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 1, 0, 200));
    CHECK(f->opened == 0);
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 2, 0, 200));
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 1, 0, 0, 201));
    CHECK(snapshot_is(f, "alpha"));
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 1, 0, 300));
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 3, 0, 300));
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 1, 0, 0, 301));
    CHECK(snapshot_is(f, "alpha beta\n"));
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 1000) == 0 && f->opened == 0);
    return true;
}

static bool modifiers_reporting_and_rejected_uri(fixture *f)
{
    const uint32_t modifiers[] = {BOXRIG_MODIFIER_SHIFT, BOXRIG_MODIFIER_CONTROL, BOXRIG_MODIFIER_ALT};
    for (size_t i = 0; i < sizeof(modifiers) / sizeof(modifiers[0]); i++) {
        CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 1, modifiers[i], 100));
        CHECK(f->link_route == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL);
        CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 1, 0, modifiers[i], 101));
    }
    CHECK(feed(f, "\033[?1003;1006h"));
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 1, 0, 1000));
    CHECK(f->link_route == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL && !f->selecting);
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 1, 0, 0, 1001));
    CHECK(feed(f, "\033[?1003;1006l"));
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 3, 2, 1, 0, 1100));
    CHECK(f->link_route == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL);
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 3, 2, 0, 0, 1101));
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 2000) == 0 && f->opened == 0);
    return true;
}

static bool stale_and_cancellation(fixture *f)
{
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 1, 0, 100));
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 1, 0, 0, 101));
    CHECK(feed(f, "\033[H\033]8;;https://example.test/replaced\033\\alpha beta\033]8;;\033\\"));
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 1000) == 0 && f->opened == 0);
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 1, 0, 1100));
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 1, 0, 0, 1101));
    boxrig_term_link_gesture_cancel(f->gesture);
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 2000) == 0 && f->opened == 0);
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 1, 0, 2100));
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 1, 0, 0, 2101));
    CHECK(feed(f, "\033[?1003h"));
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 3000) == 0 && f->opened == 0);
    return true;
}

static bool other_line_update_and_next_click(fixture *f)
{
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 1, 0, 100));
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 1, 0, 0, 101));
    CHECK(feed(f, "\033[3;1Hchanged"));
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 1, 2, 1, 0, 200));
    CHECK(f->opened == 1);
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 1, 2, 0, 0, 201));
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 701) == 0 && f->opened == 2);
    CHECK(strcmp(f->urls[1], "https://example.test/regex") == 0);
    return true;
}

static bool press_without_release_never_launches(fixture *f)
{
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 1, 0, 100));
    uint64_t deadline;
    CHECK(!boxrig_term_link_gesture_deadline(f->gesture, &deadline));
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 1000) == 0 && f->opened == 0);
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 1, 1, 1, 0, 1100));
    CHECK(f->opened == 0);
    boxrig_term_link_gesture_cancel(f->gesture);
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 1, 1, 0, 0, 1101));
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 2000) == 0 && f->opened == 0);
    return true;
}

static bool stale_drag_still_consumes_release(fixture *f)
{
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 1, 0, 100));
    CHECK(feed(f, "\033[Hreplacement\033[?1003h"));
    CHECK(mouse(f, BOXRIG_MOUSE_MOTION, 0, 8, 0, 0, 110));
    CHECK(f->link_route == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION && !f->selecting);
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 8, 0, 0, 120));
    CHECK(f->link_route == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 1000) == 0 && f->opened == 0);
    return true;
}

static bool long_press_and_release_modifier(fixture *f)
{
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 1, 0, 100));
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 1, 0, 0, 1000));
    CHECK(f->opened == 1);
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 1, 0, 1100));
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 1, 0, BOXRIG_MODIFIER_SHIFT, 1200));
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 2000) == 0 && f->opened == 1);
    return true;
}

static bool zero_multi_click_interval(fixture *f)
{
    f->multi_click_interval = 0;
    CHECK(mouse(f, BOXRIG_MOUSE_PRESS, 0, 1, 1, 0, 100));
    CHECK(mouse(f, BOXRIG_MOUSE_RELEASE, 0, 1, 0, 0, 100));
    uint64_t deadline;
    CHECK(boxrig_term_link_gesture_deadline(f->gesture, &deadline) && deadline == 101);
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 100) == 0 && f->opened == 0);
    CHECK(boxrig_term_link_gesture_dispatch(f->gesture, 101) == 0 && f->opened == 1);
    return true;
}

int main(void)
{
    const struct { const char *name; bool (*run)(fixture *); } cases[] = {
        {"single click deadline and retained selection", single_click_deadline},
        {"regex and adjacent OSC cell", regex_and_adjacent_osc_cell},
        {"drag uses original anchor", drag_uses_original_anchor},
        {"multi-click preserves word and line selection", multiclick_keeps_selection},
        {"native double-click promotion never launches", native_multiclick_promotion},
        {"modifiers, reporting and rejected URI", modifiers_reporting_and_rejected_uri},
        {"stale and explicit cancellation", stale_and_cancellation},
        {"unrelated output and distinct clicks", other_line_update_and_next_click},
        {"press without release", press_without_release_never_launches},
        {"stale drag retains release ownership", stale_drag_still_consumes_release},
        {"long press and release modifier", long_press_and_release_modifier},
        {"zero native multi-click interval", zero_multi_click_interval}
    };
    bool passed = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fixture f = {0};
        bool ok = create(&f) && cases[i].run(&f);
        destroy(&f);
        printf("%s %s\n", ok ? "PASS" : "FAIL", cases[i].name);
        passed = passed && ok;
    }
    return passed ? 0 : 1;
}
