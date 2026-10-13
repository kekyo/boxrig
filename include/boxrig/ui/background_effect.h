/* boxrig - Under MIT. */
#ifndef BOXRIG_UI_BACKGROUND_EFFECT_H
#define BOXRIG_UI_BACKGROUND_EFFECT_H
#include "boxrig/ui/view.h"

/** Host-owned request for compositor processing of pixels behind the window. */
typedef enum boxrig_ui_background_effect_request {
    /** Withdraw this host's effect request. */
    BOXRIG_UI_BACKGROUND_EFFECT_NONE,
    /** Use frosted glass when supported; preserve ordinary transparency otherwise. */
    BOXRIG_UI_BACKGROUND_EFFECT_PREFER,
    /** Fail without replacing the previous request when unsupported. */
    BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE
} boxrig_ui_background_effect_request;

/** Result of the latest request or capability update, never a pixel observation. */
typedef enum boxrig_ui_background_effect_reason {
    /** This host has no enabled request. */
    BOXRIG_UI_BACKGROUND_EFFECT_DISABLED,
    /** Native protocol or API accepted the request; OS policy controls appearance. */
    BOXRIG_UI_BACKGROUND_EFFECT_ACCEPTED,
    /** Required native support is absent. */
    BOXRIG_UI_BACKGROUND_EFFECT_UNSUPPORTED,
    /** Native support discovery is still pending. */
    BOXRIG_UI_BACKGROUND_EFFECT_PENDING,
    /** A native operation failed. */
    BOXRIG_UI_BACKGROUND_EFFECT_FAILED
} boxrig_ui_background_effect_reason;

/** Capability, committed request, and native acceptance reported separately. */
typedef struct boxrig_ui_background_effect_state {
    /** Last committed host request. Failed required changes preserve this value. */
    boxrig_ui_background_effect_request request;
    /** Native API or advertised protocol capability is available. */
    bool supported;
    /** Last committed enabled request was accepted; does not guarantee visible blur. */
    bool accepted;
    /** Result of the latest operation. */
    boxrig_ui_background_effect_reason reason;
    /** Platform error value, zero when unavailable or not applicable. */
    int64_t native_result;
} boxrig_ui_background_effect_state;

/** One host-owned effect rectangle in native surface coordinates. */
typedef struct boxrig_ui_background_effect_rectangle {
    /** Left coordinate. */
    int32_t x;
    /** Top coordinate. */
    int32_t y;
    /** Positive width. */
    int32_t width;
    /** Positive height. */
    int32_t height;
} boxrig_ui_background_effect_rectangle;
#endif
