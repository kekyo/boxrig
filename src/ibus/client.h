/* boxrig - Under MIT. */
#ifndef BOXRIG_IBUS_CLIENT_PRIVATE_H
#define BOXRIG_IBUS_CLIENT_PRIVATE_H
#include "boxrig/ui/view.h"
#include "boxrig/ui/input.h"
#include <poll.h>

typedef struct boxrig_ibus_client boxrig_ibus_client;
typedef struct boxrig_ibus_session boxrig_ibus_session;
typedef struct boxrig_ibus_request boxrig_ibus_request;

typedef enum boxrig_ibus_state {
    BOXRIG_IBUS_DISABLED = 0,
    BOXRIG_IBUS_CONNECTING,
    BOXRIG_IBUS_READY,
    BOXRIG_IBUS_UNAVAILABLE
} boxrig_ibus_state;

typedef enum boxrig_ibus_effect_type {
    BOXRIG_IBUS_COMPOSITION,
    BOXRIG_IBUS_KEY,
    BOXRIG_IBUS_CANDIDATES
} boxrig_ibus_effect_type;

/* An owned, bounded copy of engine data. Labels address positions within a
 * page, not absolute candidate indices. No IBus/GObject storage is borrowed. */
#define BOXRIG_IBUS_CANDIDATES_MAXIMUM 1024u
#define BOXRIG_IBUS_CANDIDATE_BYTES_MAXIMUM 65536u
typedef struct boxrig_ibus_candidates {
    size_t count, page_size, cursor;
    int orientation;
    bool visible, cursor_visible, round, auxiliary_visible;
    char **items, **labels;
    char *auxiliary;
} boxrig_ibus_candidates;

/* A transaction owns its effects, including all text and segment storage. */
typedef struct boxrig_ibus_effect {
    boxrig_ibus_effect_type type;
    union {
        boxrig_composition_event composition;
        boxrig_key_event key;
        boxrig_ibus_candidates candidates;
    } value;
    char *text;
    boxrig_composition_segment *segments;
} boxrig_ibus_effect;

/* Borrowed host endpoint. Detach all sessions and release queued requests
 * before destroying it. enqueue takes a request reference even on failure;
 * apply borrows an effect and reports acceptance independently of blocking. */
typedef struct boxrig_ibus_target {
    boxrig_ui_view *view;
    size_t maximum_events, maximum_bytes;
    void *data;
    void (*wake)(void *data, bool waiting);
    void (*error)(void *data, bool resource);
    void (*enqueue)(void *data, void *seat, boxrig_ibus_request *request);
    bool (*forward)(void *data, void *seat, uint32_t symbol, uint32_t code,
        uint32_t modifiers, boxrig_ibus_effect *effect);
    boxrig_ui_status (*apply)(void *data, boxrig_ibus_effect *effect,
        bool *blocked, bool *accepted);
    void (*cursor)(void *data);
    /* Non-NULL opts into lookup/auxiliary ownership. apply must handle owned
     * candidate effects; this immediate reset must never block or commit text.
     * Set only after a renderer is ready, and keep fixed until focus detaches. */
    void (*clear_candidates)(void *data);
} boxrig_ibus_target;

typedef struct boxrig_ibus_diagnostic {
    boxrig_ibus_state state;
    size_t pending, errors, contexts;
} boxrig_ibus_diagnostic;

/* The client owns its private GMainContext, connection, sessions and callbacks.
 * No callback dispatches the GTK/default main context. Host owns this client. */
boxrig_ibus_client *boxrig_ibus_create(size_t maximum_sessions,
    const char *name, void (*selection_changed)(void *, bool), void *data);
void boxrig_ibus_destroy(boxrig_ibus_client *client);
bool boxrig_ibus_selected(const boxrig_ibus_client *client);
boxrig_ibus_session *boxrig_ibus_add_session(boxrig_ibus_client *client, void *seat);
void boxrig_ibus_remove_session(boxrig_ibus_session *session);
bool boxrig_ibus_is_focused(const boxrig_ibus_session *session, const boxrig_ibus_target *target);
void boxrig_ibus_focus(boxrig_ibus_session *session, boxrig_ibus_target *target,
    bool commit, uint64_t generation);
void boxrig_ibus_update(boxrig_ibus_client *client);
/* Refresh document/cursor state without dispatching pending callbacks. */
void boxrig_ibus_refresh(boxrig_ibus_client *client);
/* The caller selects surface-local logical or absolute root coordinates. */
void boxrig_ibus_cursor(boxrig_ibus_client *client, boxrig_ibus_target *target,
    const int32_t rectangle[4], bool relative);
void boxrig_ibus_get_diagnostic(const boxrig_ibus_client *client,
    boxrig_ibus_diagnostic *diagnostic);
boxrig_ui_status boxrig_ibus_filter_key(boxrig_ibus_session *session,
    boxrig_ibus_target *target, uint64_t generation, uint32_t symbol,
    uint32_t code, uint32_t modifiers, boxrig_ibus_request **request,
    bool *blocked, bool *handled);
boxrig_ui_status boxrig_ibus_apply_reply(boxrig_ibus_target *target,
    boxrig_ibus_request *request, bool *blocked, bool *accepted);
void boxrig_ibus_release_request(boxrig_ibus_request *request);
boxrig_ui_status boxrig_ibus_prepare_poll(boxrig_ibus_client *client,
    const struct pollfd **descriptors, size_t *count, int *timeout);
boxrig_ui_status boxrig_ibus_dispatch_poll(boxrig_ibus_client *client,
    const struct pollfd *descriptors, size_t count);
#endif
