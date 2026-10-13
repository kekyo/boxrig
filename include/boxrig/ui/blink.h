/* boxrig - Under MIT. */
#ifndef BOXRIG_UI_BLINK_H
#define BOXRIG_UI_BLINK_H

#include <stdbool.h>
#include <stdint.h>

/** Positive durations for one displayed/hidden cycle, in milliseconds. */
typedef struct boxrig_ui_blink_interval {
    /** Time for which the target is visible in each cycle. */
    uint64_t visible_milliseconds;
    /** Time for which the target is hidden in each cycle. */
    uint64_t hidden_milliseconds;
} boxrig_ui_blink_interval;

/** View-local policy for terminal-requested text and cursor blinking. */
typedef struct boxrig_ui_blink_config {
    /** Honor blinking text attributes; false leaves their ink steady. */
    bool text_enabled;
    /** Honor a blinking cursor style; false leaves the cursor steady. */
    bool cursor_enabled;
    /** Displayed/hidden durations for SGR 5 text. */
    boxrig_ui_blink_interval slow_text;
    /** Displayed/hidden durations for SGR 6 text. */
    boxrig_ui_blink_interval rapid_text;
    /** Displayed/hidden durations for the insertion cursor. */
    boxrig_ui_blink_interval cursor;
} boxrig_ui_blink_config;

/**
 * Returns enabled blinking with slow/cursor 500+500 ms and rapid 150+150 ms.
 * @return Independent configuration value that the caller may modify.
 * @remarks All durations must remain positive and each pair's sum must fit
 * uint64_t. Disabling blinking does not alter terminal rendition attributes.
 */
boxrig_ui_blink_config boxrig_ui_blink_config_default(void);

#endif
