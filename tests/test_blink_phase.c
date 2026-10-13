/* Monotonic blink phase contracts - Under MIT. */
#include "blink_private.h"
#include <stdio.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); return 1; \
} } while (0)

int main(void)
{
    boxrig_ui_blink_config config = boxrig_ui_blink_config_default();
    CHECK(config.text_enabled && config.cursor_enabled);
    CHECK(config.slow_text.visible_milliseconds == 500 &&
        config.slow_text.hidden_milliseconds == 500 &&
        config.rapid_text.visible_milliseconds == 150 &&
        config.rapid_text.hidden_milliseconds == 150 &&
        config.cursor.visible_milliseconds == 500 &&
        config.cursor.hidden_milliseconds == 500);
    CHECK(boxrig_ui_blink_config_valid(&config));
    boxrig_ui_blink_phase phase;
    boxrig_ui_blink_interval interval = {30, 70};
    CHECK(boxrig_ui_blink_phase_at(interval, 1000, 1000, &phase));
    CHECK(phase.visible && phase.has_deadline && phase.deadline == 1030);
    CHECK(boxrig_ui_blink_phase_at(interval, 1000, 1029, &phase));
    CHECK(phase.visible && phase.deadline == 1030);
    CHECK(boxrig_ui_blink_phase_at(interval, 1000, 1030, &phase));
    CHECK(!phase.visible && phase.deadline == 1100);
    CHECK(boxrig_ui_blink_phase_at(interval, 1000, 1099, &phase));
    CHECK(!phase.visible && phase.deadline == 1100);
    CHECK(boxrig_ui_blink_phase_at(interval, 1000, 1100, &phase));
    CHECK(phase.visible && phase.deadline == 1130);
    CHECK(boxrig_ui_blink_phase_at(interval, 1000, UINT64_C(10000001030), &phase));
    CHECK(!phase.visible && phase.deadline == UINT64_C(10000001100));
    CHECK(boxrig_ui_blink_phase_at(interval, UINT64_MAX - 29, UINT64_MAX, &phase));
    CHECK(phase.visible && !phase.has_deadline);
    CHECK(boxrig_ui_blink_phase_at(interval, UINT64_MAX - 30, UINT64_MAX, &phase));
    CHECK(!phase.visible && !phase.has_deadline);
    phase = (boxrig_ui_blink_phase){true, true, 777};
    CHECK(!boxrig_ui_blink_phase_at(interval, 1000, 999, &phase));
    CHECK(phase.visible && phase.has_deadline && phase.deadline == 777);
    CHECK(!boxrig_ui_blink_phase_at((boxrig_ui_blink_interval){0, 1}, 0, 0, &phase));
    CHECK(!boxrig_ui_blink_phase_at((boxrig_ui_blink_interval){UINT64_MAX, 1}, 0, 0, &phase));
    CHECK(phase.deadline == 777);
    CHECK(!boxrig_ui_blink_phase_at(interval, 0, 0, NULL));
    config.cursor.hidden_milliseconds = 0;
    CHECK(!boxrig_ui_blink_config_valid(&config));
    config.cursor.hidden_milliseconds = UINT64_MAX;
    CHECK(!boxrig_ui_blink_config_valid(&config));
    CHECK(!boxrig_ui_blink_config_valid(NULL));
    puts("PASS blink phase boundaries, skipped cycles and overflow");
    return 0;
}
