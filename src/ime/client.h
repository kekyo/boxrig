/* boxrig - Under MIT. */
#ifndef BOXRIG_IME_CLIENT_H
#define BOXRIG_IME_CLIENT_H
#include "boxrig/ime.h"
#include "boxrig/ui/view.h"
#include "boxrig/ui/input.h"
#include <poll.h>

typedef struct boxrig_ime_client boxrig_ime_client;
typedef struct boxrig_ime_session boxrig_ime_session;
typedef struct boxrig_ime_request boxrig_ime_request;

/* Native timestamps are milliseconds; code is evdev (XKB hardware minus 8).
 * Modifiers use the X11/XKB masks, with release and repeat kept separately. */
typedef struct boxrig_ime_key {
    uint32_t symbol, code, modifiers, time;
    bool release, repeat;
} boxrig_ime_key;

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
    boxrig_ui_candidate_format format;
    size_t count, page_size, cursor;
    boxrig_ui_candidate_orientation orientation;
    bool visible, cursor_visible, round, auxiliary_visible;
    bool auxiliary_above_visible, has_previous_page, has_next_page;
    char **items, **labels;
    char *auxiliary, *auxiliary_above;
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

/* The client owns its private GMainContext, connection, sessions and callbacks.
 * No callback dispatches the GTK/default main context. Host owns this client. */
boxrig_ime_client *boxrig_ime_create(boxrig_ime_backend_kind backend, bool wayland, size_t maximum_sessions,
    const char *name, void (*selection_changed)(void *, bool), void *data);
/* module is an applicable GTK delegate, or NULL for non-GTK hosts. Framework
 * delegation must already have been resolved before starting discovery. */
boxrig_ime_client *boxrig_ime_create_auto(bool wayland, size_t maximum_sessions,
    const char *name, const char *module, void (*selection_changed)(void *, bool), void *data);
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
/* Surface-relative rectangles use logical coordinates and the surface scale.
 * Absolute root rectangles are already physical pixels; their scale is ignored. */
void boxrig_ime_cursor(boxrig_ime_client *client, boxrig_ime_target *target,
    const int32_t rectangle[4], bool relative, double scale);
void boxrig_ime_get_diagnostic(const boxrig_ime_client *client,
    boxrig_ime_diagnostic *diagnostic);
/* Fcitx may suppress the requested client panel. Until a notification arrives,
 * NONE distinguishes an unconfirmed request from a confirmed client owner. */
boxrig_ui_candidate_owner boxrig_ime_candidate_owner(const boxrig_ime_client *client,
    const boxrig_ime_target *target);
boxrig_ui_status boxrig_ime_filter_key(boxrig_ime_session *session,
    boxrig_ime_target *target, uint64_t generation, const boxrig_ime_key *key,
    boxrig_ime_request **request,
    bool *blocked, bool *handled);
boxrig_ui_status boxrig_ime_apply_reply(boxrig_ime_target *target,
    boxrig_ime_request *request, bool *blocked, bool *accepted);
void boxrig_ime_release_request(boxrig_ime_request *request);
boxrig_ui_status boxrig_ime_prepare_poll(boxrig_ime_client *client,
    const struct pollfd **descriptors, size_t *count, int *timeout);
boxrig_ui_status boxrig_ime_dispatch_poll(boxrig_ime_client *client,
    const struct pollfd *descriptors, size_t count);
#endif
