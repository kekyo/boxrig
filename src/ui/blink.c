/* boxrig - Under MIT. */
#include "blink_private.h"
#include <stddef.h>

boxrig_ui_blink_config boxrig_ui_blink_config_default(void)
{
    return (boxrig_ui_blink_config){true, true, {500, 500}, {150, 150}, {500, 500}};
}

static bool interval_valid(boxrig_ui_blink_interval interval)
{
    return interval.visible_milliseconds != 0 && interval.hidden_milliseconds != 0 &&
        interval.visible_milliseconds <= UINT64_MAX - interval.hidden_milliseconds;
}

bool boxrig_ui_blink_config_valid(const boxrig_ui_blink_config *config)
{
    return config != NULL && interval_valid(config->slow_text) &&
        interval_valid(config->rapid_text) && interval_valid(config->cursor);
}

bool boxrig_ui_blink_phase_at(boxrig_ui_blink_interval interval,
    uint64_t origin, uint64_t now, boxrig_ui_blink_phase *phase)
{
    if (phase == NULL || now < origin || !interval_valid(interval)) return false;
    uint64_t period = interval.visible_milliseconds + interval.hidden_milliseconds;
    uint64_t offset = (now - origin) % period;
    bool visible = offset < interval.visible_milliseconds;
    uint64_t remaining = (visible ? interval.visible_milliseconds : period) - offset;
    boxrig_ui_blink_phase result = {visible, remaining <= UINT64_MAX - now, 0};
    if (result.has_deadline) result.deadline = now + remaining;
    *phase = result;
    return true;
}
