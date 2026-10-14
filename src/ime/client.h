/* boxrig - Under MIT. */
#ifndef BOXRIG_IME_CLIENT_H
#define BOXRIG_IME_CLIENT_H
#include "boxrig/ui/view.h"
#include "boxrig/ui/input.h"
#include <poll.h>

typedef struct boxrig_ime_client boxrig_ime_client;
typedef struct boxrig_ime_session boxrig_ime_session;
typedef struct boxrig_ime_request boxrig_ime_request;

typedef enum boxrig_ime_backend_kind {
    BOXRIG_IME_BACKEND_IBUS = 0
} boxrig_ime_backend_kind;

typedef enum boxrig_ime_state {
    BOXRIG_IME_DISABLED = 0,
    BOXRIG_IME_CONNECTING,
    BOXRIG_IME_READY,
    BOXRIG_IME_UNAVAILABLE
} boxrig_ime_state;

typedef enum boxrig_ime_effect_type {
    BOXRIG_IME_COMPOSITION,
    BOXRIG_IME_KEY,
    BOXRIG_IME_CANDIDATES
} boxrig_ime_effect_type;

/* An owned, bounded copy of engine data. Labels address positions within a
 * page, not absolute candidate indices. No protocol-owned storage is borrowed. */
#define BOXRIG_IME_CANDIDATES_MAXIMUM 1024u
#define BOXRIG_IME_CANDIDATE_BYTES_MAXIMUM 65536u
typedef struct boxrig_ime_candidates {
    size_t count, page_size, cursor;
    boxrig_ui_candidate_orientation orientation;
    bool visible, cursor_visible, round, auxiliary_visible;
    char **items, **labels;
    char *auxiliary;
} boxrig_ime_candidates;

/* A transaction owns its effects, including all text and segment storage. */
typedef struct boxrig_ime_effect {
    boxrig_ime_effect_type type;
    union {
        boxrig_composition_event composition;
        boxrig_key_event key;
        boxrig_ime_candidates candidates;
    } value;
    char *text;
    boxrig_composition_segment *segments;
} boxrig_ime_effect;

/* Borrowed host endpoint. Detach all sessions and release queued requests
 * before destroying it. enqueue takes a request reference even on failure;
 * apply borrows an effect and reports acceptance independently of blocking. */
typedef struct boxrig_ime_target {
    boxrig_ui_view *view;
    size_t maximum_events, maximum_bytes;
    void *data;
    void (*wake)(void *data, bool waiting);
    void (*error)(void *data, bool resource);
    void (*enqueue)(void *data, void *seat, boxrig_ime_request *request);
    bool (*forward)(void *data, void *seat, uint32_t symbol, uint32_t code,
        uint32_t modifiers, boxrig_ime_effect *effect);
    boxrig_ui_status (*apply)(void *data, boxrig_ime_effect *effect,
        bool *blocked, bool *accepted);
    void (*cursor)(void *data);
    /* Non-NULL opts into lookup/auxiliary ownership. apply must handle owned
     * candidate effects; this immediate reset must never block or commit text.
     * Set only after a renderer is ready, and keep fixed until focus detaches. */
    void (*clear_candidates)(void *data);
} boxrig_ime_target;

typedef struct boxrig_ime_diagnostic {
    boxrig_ime_backend_kind backend;
    boxrig_ime_state state;
    size_t pending, errors, contexts;
} boxrig_ime_diagnostic;

/* The client owns its private GMainContext, connection, sessions and callbacks.
 * No callback dispatches the GTK/default main context. Host owns this client. */
boxrig_ime_client *boxrig_ime_create(boxrig_ime_backend_kind backend, size_t maximum_sessions,
    const char *name, void (*selection_changed)(void *, bool), void *data);
void boxrig_ime_destroy(boxrig_ime_client *client);
bool boxrig_ime_selected(const boxrig_ime_client *client);
boxrig_ime_session *boxrig_ime_add_session(boxrig_ime_client *client, void *seat);
void boxrig_ime_remove_session(boxrig_ime_session *session);
bool boxrig_ime_is_focused(const boxrig_ime_session *session, const boxrig_ime_target *target);
void boxrig_ime_focus(boxrig_ime_session *session, boxrig_ime_target *target,
    bool commit, uint64_t generation);
void boxrig_ime_update(boxrig_ime_client *client);
/* Refresh document/cursor state without dispatching pending callbacks. */
void boxrig_ime_refresh(boxrig_ime_client *client);
/* The caller selects surface-local logical or absolute root coordinates. */
void boxrig_ime_cursor(boxrig_ime_client *client, boxrig_ime_target *target,
    const int32_t rectangle[4], bool relative);
void boxrig_ime_get_diagnostic(const boxrig_ime_client *client,
    boxrig_ime_diagnostic *diagnostic);
boxrig_ui_status boxrig_ime_filter_key(boxrig_ime_session *session,
    boxrig_ime_target *target, uint64_t generation, uint32_t symbol,
    uint32_t code, uint32_t modifiers, boxrig_ime_request **request,
    bool *blocked, bool *handled);
boxrig_ui_status boxrig_ime_apply_reply(boxrig_ime_target *target,
    boxrig_ime_request *request, bool *blocked, bool *accepted);
void boxrig_ime_release_request(boxrig_ime_request *request);
boxrig_ui_status boxrig_ime_prepare_poll(boxrig_ime_client *client,
    const struct pollfd **descriptors, size_t *count, int *timeout);
boxrig_ui_status boxrig_ime_dispatch_poll(boxrig_ime_client *client,
    const struct pollfd *descriptors, size_t count);
#endif
