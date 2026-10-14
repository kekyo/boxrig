/* boxrig - Under MIT. */
#ifndef BOXRIG_GTK_INPUT_H
#define BOXRIG_GTK_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "boxrig/ime.h"
#include "boxrig/ui/candidates.h"

/** Input-method selection shared by the GTK 3 and GTK 4 widgets. */
typedef enum boxrig_gtk_ime_policy {
    /** Select by actual display, runtime capabilities and explicit GTK module. */
    BOXRIG_GTK_IME_AUTO = 0,
    /** Use the ordinary GTK input context and its configured module. */
    BOXRIG_GTK_IME_GTK,
    /** Use direct IBus on Wayland or GTK 3/X11, and the GTK IBus module on GTK 4/X11. */
    BOXRIG_GTK_IME_IBUS,
    /** Use direct fcitx 5 on Wayland or GTK 3/X11, and the GTK fcitx module on GTK 4/X11. */
    BOXRIG_GTK_IME_FCITX5,
    /** Use direct fcitx 4 on Wayland or GTK 3/X11, and the GTK fcitx module on GTK 4/X11. */
    BOXRIG_GTK_IME_FCITX4
} boxrig_gtk_ime_policy;

/** Active input owner; pending means that native keys are retained in order. */
typedef enum boxrig_gtk_ime_route {
    BOXRIG_GTK_IME_ROUTE_PENDING = 0, /**< Waiting for capability discovery. */
    BOXRIG_GTK_IME_ROUTE_GTK, /**< The configured GtkIMContext. */
    BOXRIG_GTK_IME_ROUTE_IBUS, /**< Direct asynchronous IBus. */
    BOXRIG_GTK_IME_ROUTE_FCITX5, /**< Direct asynchronous fcitx 5. */
    BOXRIG_GTK_IME_ROUTE_FCITX4 /**< Direct asynchronous fcitx 4. */
} boxrig_gtk_ime_route;

/** Reason for selecting or retaining an input owner. */
typedef enum boxrig_gtk_ime_reason {
    BOXRIG_GTK_IME_REASON_PENDING = 0, /**< No actual display or query still pending. */
    BOXRIG_GTK_IME_REASON_NON_WAYLAND, /**< This non-Wayland display retains GTK. */
    BOXRIG_GTK_IME_REASON_EXPLICIT_GTK, /**< Application explicitly requested GTK. */
    BOXRIG_GTK_IME_REASON_EXPLICIT_MODULE, /**< Nonempty GTK_IM_MODULE takes precedence. */
    BOXRIG_GTK_IME_REASON_EXPLICIT_IBUS, /**< Application explicitly requested direct IBus. */
    BOXRIG_GTK_IME_REASON_WAYLAND_CLAUSES, /**< Runtime GTK and server support clause hints. */
    BOXRIG_GTK_IME_REASON_LEGACY_DIRECT, /**< Incomplete standard support requires direct discovery. */
    BOXRIG_GTK_IME_REASON_GTK_FALLBACK, /**< The configured GTK delegate retains input ownership. */
    BOXRIG_GTK_IME_REASON_DIRECT_UNAVAILABLE, /**< Direct connection or context creation failed. */
    BOXRIG_GTK_IME_REASON_DISPLAY_CLOSED, /**< Capability query ended with display closure. */
    BOXRIG_GTK_IME_REASON_X11_DIRECT, /**< Direct discovery selected for an X11 terminal. */
    BOXRIG_GTK_IME_REASON_CLIENT_CANDIDATES, /**< Explicit client UI requests direct discovery. */
    BOXRIG_GTK_IME_REASON_EXPLICIT_FCITX5, /**< Application explicitly requested fcitx 5. */
    BOXRIG_GTK_IME_REASON_EXPLICIT_FCITX4 /**< Application explicitly requested fcitx 4. */
} boxrig_gtk_ime_reason;

/** Snapshot of runtime input selection. No field owns external storage. */
typedef struct boxrig_gtk_ime_state {
    boxrig_gtk_ime_policy requested_policy; /**< Configuration supplied at creation. */
    boxrig_gtk_ime_route active_route; /**< The single current input owner. */
    boxrig_gtk_ime_reason reason; /**< Selection reason or current fallback condition. */
    unsigned int gtk_major; /**< Loaded GTK major version. */
    unsigned int gtk_minor; /**< Loaded GTK minor version. */
    unsigned int gtk_micro; /**< Loaded GTK micro version. */
    char display_type[64]; /**< Actual GdkDisplay type, empty before attachment. */
    char gtk_module[64]; /**< Resolved standard GTK delegate ID, empty if unknown. */
    bool registry_queried; /**< This actual display's registry was queried. */
    bool registry_complete; /**< The initial sync reply was received. */
    uint32_t text_input_version; /**< Advertised interface version, not GTK's binding. */
    boxrig_ime_diagnostic direct; /**< Common direct IME selection and transport diagnostics. */
    bool candidate_valid; /**< A direct candidate rectangle has been submitted. */
    /** True when the direct rectangle uses surface logical units; false means X11 root pixels. */
    bool candidate_surface_relative;
    /** Direct x, y, width, height, in the units specified by candidate_surface_relative. */
    int32_t candidate[4];
    boxrig_ui_candidate_policy candidate_policy; /**< Requested candidate presentation. */
    boxrig_ui_candidate_owner candidate_owner; /**< Actual display owner, independent of input route. */
    boxrig_ui_candidate_reason candidate_reason; /**< Candidate availability or selection reason. */
    uint64_t candidate_revision; /**< Latest retained client candidate revision. */
    size_t candidate_count; /**< Retained entries in the engine's table or received page. */
    size_t candidate_cursor; /**< Selected entry within the retained snapshot. */
    bool candidate_visible; /**< The lookup table is requested visible. */
    bool auxiliary_visible; /**< Supplementary text is requested visible. */
} boxrig_gtk_ime_state;

#endif
