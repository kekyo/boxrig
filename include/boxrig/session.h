/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_SESSION_H
#define BOXRIG_SESSION_H

#include "boxrig/terminal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Result of one nonblocking transport operation. */
typedef enum boxrig_transport_status {
    /** The operation transferred a positive number of bytes. */
    BOXRIG_TRANSPORT_OK = 0,
    /** No bytes can be transferred until the transport is ready again. */
    BOXRIG_TRANSPORT_WOULD_BLOCK,
    /** The peer closed this transport direction. */
    BOXRIG_TRANSPORT_CLOSED,
    /** A permanent platform transport error occurred. */
    BOXRIG_TRANSPORT_ERROR
} boxrig_transport_status;

/**
 * Reads one nonblocking byte range from a transport.
 *
 * @param context Transport-specific state.
 * @param bytes Destination storage.
 * @param capacity Available destination bytes.
 * @param length_out Receives transferred bytes and must be zero unless OK.
 * @return A portable transport status.
 */
typedef boxrig_transport_status (*boxrig_transport_read_callback)(
    void *context,
    uint8_t *bytes,
    size_t capacity,
    size_t *length_out);

/**
 * Writes one nonblocking byte range to a transport.
 *
 * @param context Transport-specific state.
 * @param bytes Source bytes.
 * @param length Number of available source bytes.
 * @param length_out Receives transferred bytes and must be zero unless OK.
 * @return A portable transport status.
 */
typedef boxrig_transport_status (*boxrig_transport_write_callback)(
    void *context,
    const uint8_t *bytes,
    size_t length,
    size_t *length_out);

/**
 * Applies an effective terminal grid to a transport.
 *
 * @param context Transport-specific state.
 * @param rows New nonzero row count.
 * @param columns New nonzero column count.
 * @return BOXRIG_TRANSPORT_OK on success or a transport error.
 */
typedef boxrig_transport_status (*boxrig_transport_resize_callback)(
    void *context,
    size_t rows,
    size_t columns);

/**
 * Releases transport-specific state owned by a session.
 *
 * @param context Transport-specific state.
 */
typedef void (*boxrig_transport_destroy_callback)(void *context);

/** Platform-independent transport operations copied by one session. */
typedef struct boxrig_session_transport {
    /** Opaque state passed to every transport callback. */
    void *context;
    /** Required nonblocking read operation. */
    boxrig_transport_read_callback read;
    /** Required nonblocking write operation. */
    boxrig_transport_write_callback write;
    /** Optional synchronous grid resize operation. */
    boxrig_transport_resize_callback resize;
    /** Optional state destructor called once by session destruction. */
    boxrig_transport_destroy_callback destroy;
} boxrig_session_transport;

/** Bounded buffering and fairness limits for one transport session. */
typedef struct boxrig_session_config {
    /** Bytes retained across parser backpressure. */
    size_t input_capacity;
    /** Maximum transport bytes read by one dispatch call. */
    size_t maximum_read_bytes_per_dispatch;
} boxrig_session_config;

/** Result codes returned by reusable session operations. */
typedef enum boxrig_session_status {
    BOXRIG_SESSION_OK = 0,
    BOXRIG_SESSION_INVALID_ARGUMENT,
    BOXRIG_SESSION_OUT_OF_MEMORY,
    BOXRIG_SESSION_CORE_ERROR,
    BOXRIG_SESSION_TRANSPORT_ERROR
} boxrig_session_status;

/** Wait interests requested from an application-owned event loop. */
typedef enum boxrig_session_interest {
    /** No native wait interest is currently needed. */
    BOXRIG_SESSION_INTEREST_NONE = 0,
    /** Wait for transport input readiness. */
    BOXRIG_SESSION_INTEREST_READ = 1u << 0,
    /** Wait for transport output readiness. */
    BOXRIG_SESSION_INTEREST_WRITE = 1u << 1,
    /** Dispatch retained input before blocking in the event loop. */
    BOXRIG_SESSION_INTEREST_DISPATCH = 1u << 2
} boxrig_session_interest;

/** Native readiness supplied by an application-owned event loop. */
typedef enum boxrig_session_ready {
    BOXRIG_SESSION_READY_NONE = 0,
    BOXRIG_SESSION_READY_READ = 1u << 0,
    BOXRIG_SESSION_READY_WRITE = 1u << 1
} boxrig_session_ready;

/** Observable bounded work completed by one dispatch. */
typedef struct boxrig_session_dispatch_result {
    /** Bytes received from the platform transport. */
    size_t transport_bytes_read;
    /** Received bytes accepted by the core parser. */
    size_t terminal_bytes_fed;
    /** Core outbound bytes accepted by the platform transport. */
    size_t transport_bytes_written;
    /** Whether parser or host-queue backpressure retained input for retry. */
    bool core_blocked;
    /** Whether the peer closed and all retained input and EOF work completed. */
    bool peer_closed;
} boxrig_session_dispatch_result;

/** Opaque reusable bridge between one terminal and one transport. */
typedef struct boxrig_session boxrig_session;

/**
 * Returns bounded defaults for a reusable session.
 *
 * @return A 4 KiB input buffer and 64 KiB read-dispatch budget.
 */
boxrig_session_config boxrig_session_config_default(void);

/**
 * Creates a reusable session and copies the transport callback table.
 *
 * @param terminal Borrowed terminal that outlives the session.
 * @param transport Callback table copied by the session.
 * @param config Bounded input and dispatch configuration.
 * @param session_out Receives the allocated session.
 * @return A portable session status.
 *
 * @remarks A non-NULL destroy callback transfers context ownership to the
 * session. With a NULL destroy callback, context remains borrowed and must
 * outlive the session.
 */
boxrig_session_status boxrig_session_create(
    boxrig_terminal *terminal,
    const boxrig_session_transport *transport,
    const boxrig_session_config *config,
    boxrig_session **session_out);

/**
 * Releases pending bytes and invokes the optional transport destructor once.
 *
 * @param session Session to release, or NULL.
 */
void boxrig_session_destroy(boxrig_session *session);

/**
 * Returns the terminal borrowed by a session.
 *
 * @param session Session to inspect.
 * @return Borrowed terminal, or NULL.
 */
boxrig_terminal *boxrig_session_terminal(boxrig_session *session);

/**
 * Returns current read, write, and immediate-dispatch interests.
 *
 * @param session Session to inspect.
 * @return Bitwise boxrig_session_interest values.
 */
uint32_t boxrig_session_interests(const boxrig_session *session);

/**
 * Reports whether received bytes or input-end processing await parser capacity.
 *
 * @param session Session to inspect.
 * @return True while dispatch must retry retained bytes or an input end.
 */
bool boxrig_session_input_pending(const boxrig_session *session);

/**
 * Performs bounded nonblocking transport and parser work.
 *
 * @param session Session to service.
 * @param ready Bitwise boxrig_session_ready values from the event loop.
 * @param result_out Receives transferred counts and backpressure state.
 * @return A portable session status.
 *
 * @remarks Retained parser input is retried even when ready is NONE. One write
 * callback is made per dispatch, including when parser input remains blocked,
 * so outbound capacity can make progress without starving unrelated work.
 * After CLOSED, retained input is consumed and finish_input is retried before
 * peer_closed is published. No further reads occur after transport closure.
 */
boxrig_session_status boxrig_session_dispatch(
    boxrig_session *session,
    uint32_t ready,
    boxrig_session_dispatch_result *result_out);

/**
 * Applies one effective grid to the transport and terminal.
 *
 * @param session Session to resize.
 * @param rows New nonzero row count.
 * @param columns New nonzero column count.
 * @return A portable session status.
 *
 * @remarks External transport resize cannot be rolled back if subsequent core
 * allocation fails; callers should treat either error as session-fatal.
 */
boxrig_session_status boxrig_session_resize(
    boxrig_session *session,
    size_t rows,
    size_t columns);

#endif
