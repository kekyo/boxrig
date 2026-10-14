/* boxrig - Under MIT. */
#ifndef BOXRIG_IME_H
#define BOXRIG_IME_H
#include <stdbool.h>
#include <stddef.h>

/** Direct input protocol, independent of the GUI framework's own IM module. */
typedef enum boxrig_ime_backend_kind {
    BOXRIG_IME_BACKEND_NONE = 0, /**< No direct protocol has been selected. */
    BOXRIG_IME_BACKEND_IBUS, /**< IBus input-context protocol. */
    BOXRIG_IME_BACKEND_FCITX5 /**< Native fcitx 5 input-context protocol. */
} boxrig_ime_backend_kind;

/** Lifecycle of an optional direct IME connection. */
typedef enum boxrig_ime_state {
    BOXRIG_IME_DISABLED = 0, /**< The host delegates input to its framework. */
    BOXRIG_IME_CONNECTING, /**< Discovery, connection, or input-context setup is pending. */
    BOXRIG_IME_READY, /**< The selected service has a verified owner. */
    BOXRIG_IME_UNAVAILABLE /**< No eligible service is available; framework input remains usable. */
} boxrig_ime_state;

/** Evidence used to select a direct protocol; connection state is separate. */
typedef enum boxrig_ime_selection_reason {
    BOXRIG_IME_SELECTION_EXPLICIT = 0, /**< Application explicitly selected a protocol. */
    BOXRIG_IME_SELECTION_DISCOVERY, /**< No configuration hint; discovering actual owners. */
    BOXRIG_IME_SELECTION_MODULE, /**< An applicable GTK delegate supplies the hint. */
    BOXRIG_IME_SELECTION_XMODIFIERS, /**< XMODIFIERS supplies the hint. */
    BOXRIG_IME_SELECTION_ADDRESS, /**< An explicit IME address supplies the hint. */
    BOXRIG_IME_SELECTION_SINGLE, /**< Exactly one independent service was found. */
    BOXRIG_IME_SELECTION_PRIORITY, /**< Multiple services were found; initial priority selected one. */
    BOXRIG_IME_SELECTION_RETAINED /**< A later service appeared; the valid current selection was retained. */
} boxrig_ime_selection_reason;

/** Owned-value snapshot of direct IME discovery and transport activity. */
typedef struct boxrig_ime_diagnostic {
    boxrig_ime_backend_kind backend; /**< Selected protocol, or NONE during automatic discovery/failure. */
    boxrig_ime_state state; /**< Connection and discovery lifecycle. */
    boxrig_ime_selection_reason selection_reason; /**< Highest-priority selection evidence. */
    bool ambiguous; /**< Multiple independent running services were found. */
    bool conflicting_hints; /**< Applicable environment hints disagree. */
    bool compatibility_alias; /**< An IBus service is provided by the same fcitx process. */
    size_t pending; /**< Outstanding asynchronous discovery, setup, and key operations. */
    size_t errors; /**< Recoverable discovery, transport, and adapter errors observed. */
    size_t contexts; /**< Live input contexts on the selected protocol. */
} boxrig_ime_diagnostic;
#endif
