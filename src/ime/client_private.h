/* boxrig - Under MIT. */
#ifndef BOXRIG_IME_CLIENT_PRIVATE_H
#define BOXRIG_IME_CLIENT_PRIVATE_H
#include "client.h"
#include "boxrig/ui/editable.h"
#include <gio/gio.h>

/* Protocol operations run on the client's private main context. A start or
 * key operation completes exactly once, including cancellation. The common
 * layer owns sessions and requests until those completions have returned. */
typedef struct boxrig_ime_backend {
    boxrig_ime_backend_kind kind;
    void (*initialize)(boxrig_ime_client *client);
    void (*connect)(boxrig_ime_client *client);
    void (*stop)(boxrig_ime_client *client);
    void (*start)(boxrig_ime_session *session);
    void (*drop)(boxrig_ime_session *session);
    void (*reset)(boxrig_ime_session *session);
    void (*surrounding)(boxrig_ime_session *session, const char *text,
        uint32_t cursor, uint32_t anchor, bool available);
    void (*key)(boxrig_ime_session *session, const boxrig_ime_key *key, boxrig_ime_request *request);
    void (*cursor)(boxrig_ime_session *session, const int32_t rectangle[4], bool relative);
} boxrig_ime_backend;
extern const boxrig_ime_backend boxrig_ime_ibus_backend;
extern const boxrig_ime_backend boxrig_ime_fcitx5_backend;

/* Only this private main context dispatches client callbacks. The host polls
 * its sources alongside Wayland and the PTY; no library worker runs input. */
struct boxrig_ime_client {
    size_t maximum_sessions;
    const boxrig_ime_backend *backend;
    const char *name;
    void (*selection_changed)(void *, bool);
    void *data;
    GMainContext *loop;
    GDBusConnection *connection;
    /* Name subscriptions and owner identity belong to this bus instance. */
    guint watcher;
    char *connection_owner;
    uint64_t connection_generation;
    bool wayland, shared_connection;
    GCancellable *cancel;
    GFileMonitor *monitor;
    GPollFD *glib_fds;
    struct pollfd *fds;
    size_t capacity, count, pending, errors, session_count, subscriptions;
    gint priority;
    bool prepared, connecting, closing;
    boxrig_ime_state state;
    boxrig_ime_session *sessions;
};

/* The list owns each session until all of its requests have gone away. A
 * detached seat/component is never retained by an outstanding D-Bus call. */
struct boxrig_ime_session {
    boxrig_ime_session *next;
    boxrig_ime_client *owner;
    void *seat;
    boxrig_ime_target *component;
    /* Owned protocol context, interpreted only by the selected backend. */
    void *proxy;
    GDBusConnection *setup_connection;
    GCancellable *cancel;
    char *path, *setup_owner;
    uint64_t setup_connection_generation;
    uint64_t generation, setup_generation, document_generation;
    size_t references;
    bool pending, cursor_valid, cursor_relative, preedit_visible;
    int32_t rectangle[4];
    uint64_t capabilities;
    bool preedit_commit;
    /* Optional commit text excludes protocol fragments marked DontCommit. */
    char *preedit_commit_text;
    boxrig_ime_effect preedit;
    boxrig_ime_candidates candidates;
    boxrig_ime_request *request, *signals_head, *signals_tail;
};

/* A key owns one reply transaction, including all signals emitted before its
 * reply. Backpressure retries this transaction without sending the key twice. */
struct boxrig_ime_request {
    boxrig_ime_session *session;
    boxrig_ime_target *component;
    size_t references, count, capacity, head;
    boxrig_ime_effect *effects;
    uint64_t generation, connection_generation;
    bool ready, handled, discarded, invalid, unsolicited, signal_linked, committed;
    boxrig_ime_request *signal_previous, *signal_next;
    bool document_attached, document_available, document_changed, edit_applied;
    /* Independent document signals are interpreted after earlier signals
     * have committed their edits, against the resulting document context. */
    bool deferred_delete, deferred_prepared;
    gint delete_offset;
    guint delete_characters;
    char *deferred_commit;
    boxrig_ui_editable_document_snapshot snapshot;
    char *original, *edited;
    size_t edited_length, cursor, anchor;
};

void boxrig_ime_free_candidates(boxrig_ime_candidates *table);
size_t boxrig_ime_candidate_budget(const boxrig_ime_target *target);
boxrig_ui_status boxrig_ime_copy_candidate_text(const char *source, char **target, size_t *budget);
boxrig_ui_status boxrig_ime_allocate_candidates(boxrig_ime_candidates *table);
boxrig_ui_status boxrig_ime_copy_candidates(const boxrig_ime_candidates *source,
    boxrig_ime_candidates *target, size_t budget);
void boxrig_ime_clear_preedit(boxrig_ime_session *session);
bool boxrig_ime_owns_candidates(const boxrig_ime_session *session);
void boxrig_ime_publish_candidates(boxrig_ime_session *session);
void boxrig_ime_reject_candidates(boxrig_ime_session *session, boxrig_ui_status status);
void boxrig_ime_publish_preedit(boxrig_ime_session *session);
void boxrig_ime_failed_connection(boxrig_ime_client *bus);
void boxrig_ime_connection_closed(GDBusConnection *connection, gboolean remote, GError *error, gpointer data);
void boxrig_ime_update_surrounding(boxrig_ime_session *session, bool reset);
bool boxrig_ime_current_setup(boxrig_ime_session *session);
void boxrig_ime_finish_setup(boxrig_ime_session *session);
void boxrig_ime_start_session(boxrig_ime_session *session);

void boxrig_ime_commit(boxrig_ime_session *session, const char *text);
void boxrig_ime_delete(boxrig_ime_session *session, int32_t offset, uint32_t characters);
void boxrig_ime_forward(boxrig_ime_session *session, uint32_t symbol, uint32_t code, uint32_t modifiers);
void boxrig_ime_key_complete(boxrig_ime_request *request, bool handled, bool failed);
#endif
