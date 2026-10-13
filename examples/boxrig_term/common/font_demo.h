/* boxrig - Under MIT. */
#ifndef BOXRIG_TERM_FONT_DEMO_H
#define BOXRIG_TERM_FONT_DEMO_H

#include "boxrig/ui.h"

/** Applies only the color-glyph setting, preserving the current font and DPI.
 * @param context Borrowed native widget or component.
 * @param enabled Whether color glyphs are permitted.
 * @return Native policy-application status, including BUSY for a deferred retry.
 */
typedef boxrig_ui_status (*boxrig_term_font_demo_setter)(void *context, bool enabled);

/** Optional color-glyph control, whose borrowed objects outlive this state. */
typedef struct boxrig_term_font_demo {
    boxrig_ui_view *view; /**< View to invalidate when a setting is queued. */
    boxrig_term_font_demo_setter setter; /**< Native policy setter. */
    void *context; /**< Borrowed setter context. */
    bool color_glyphs; /**< Latest requested value. */
    bool enabled; /**< Explicit environment setting enables the shortcut. */
    bool pending; /**< Work to apply outside input dispatch. */
} boxrig_term_font_demo;

/** Reads BOXRIG_COLOR_GLYPHS=0|1 and applies the initial setting.
 * @param demo Caller-owned state.
 * @param view Borrowed view.
 * @param setter Native policy setter.
 * @param context Borrowed setter context.
 * @return Zero on success or deferred work, -1 on error.
 * @remarks An unset variable leaves the frontend default and disables F11.
 */
int boxrig_term_font_demo_initialize(boxrig_term_font_demo *demo,
    boxrig_ui_view *view, boxrig_term_font_demo_setter setter, void *context);

/** Applies a pending value outside input dispatch.
 * @param demo Initialized state.
 * @return Zero on success or BUSY, -1 on failure. Failed native updates preserve
 * the previously applied font policy.
 */
int boxrig_term_font_demo_apply(boxrig_term_font_demo *demo);

/** Queues a color-glyph toggle on Ctrl+Shift+F11.
 * @param demo Initialized state.
 * @param event Normalized event; repeats and releases do not toggle.
 * @return APPLICATION for the enabled shortcut, TERMINAL otherwise.
 */
boxrig_input_key_route boxrig_term_font_demo_route_key(
    boxrig_term_font_demo *demo, const boxrig_key_event *event);

#endif
