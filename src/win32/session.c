/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/win32/session.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    BOXRIG_WIN32_SESSION_CHUNK_CAPACITY = 4096
};

struct boxrig_win32_pty_session {
    boxrig_win32_pty *pty;
    boxrig_session *portable;
    CRITICAL_SECTION lock;
    HANDLE activity_event;
    HANDLE stop_event;
    HANDLE read_consumed_event;
    HANDLE write_requested_event;
    HANDLE reader_thread;
    HANDLE writer_thread;
    uint8_t read_bytes[BOXRIG_WIN32_SESSION_CHUNK_CAPACITY];
    size_t read_length;
    size_t read_offset;
    DWORD read_error;
    bool read_closed;
    bool reader_in_io;
    uint8_t write_bytes[BOXRIG_WIN32_SESSION_CHUNK_CAPACITY];
    size_t write_length;
    size_t write_completed_length;
    DWORD write_error;
    bool write_pending;
    bool write_complete;
    bool writer_in_io;
    bool stopping;
};

static void close_handle(HANDLE *handle)
{
    if (*handle != NULL) {
        (void)CloseHandle(*handle);
        *handle = NULL;
    }
}

static void publish_read_end(
    boxrig_win32_pty_session *session,
    DWORD error)
{
    EnterCriticalSection(&session->lock);
    if (!session->stopping) {
        if (error == ERROR_SUCCESS || error == ERROR_BROKEN_PIPE) {
            session->read_closed = true;
        } else {
            session->read_error = error;
        }
    }
    LeaveCriticalSection(&session->lock);
    (void)SetEvent(session->activity_event);
}

static DWORD WINAPI reader_thread_main(LPVOID parameter)
{
    boxrig_win32_pty_session *session = parameter;

    for (;;) {
        DWORD received = 0;
        DWORD error = ERROR_SUCCESS;
        bool read_succeeded;
        bool stopping;

        /* Mark the synchronous call while holding the same lock used by
         * stop_workers. The stopper can therefore either cancel this call or
         * prevent the worker from starting another call after shutdown. */
        EnterCriticalSection(&session->lock);
        if (session->stopping) {
            LeaveCriticalSection(&session->lock);
            return 0;
        }
        session->reader_in_io = true;
        LeaveCriticalSection(&session->lock);
        read_succeeded = ReadFile(
            boxrig_win32_pty_output_handle(session->pty),
            session->read_bytes,
            sizeof(session->read_bytes),
            &received,
            NULL) != 0;
        if (!read_succeeded) {
            error = GetLastError();
        }
        EnterCriticalSection(&session->lock);
        session->reader_in_io = false;
        stopping = session->stopping;
        if (!stopping && read_succeeded && received != 0) {
            session->read_length = (size_t)received;
            session->read_offset = 0;
        }
        LeaveCriticalSection(&session->lock);
        if (stopping) {
            return 0;
        }
        if (!read_succeeded) {
            publish_read_end(session, error);
            return 0;
        }
        if (received == 0) {
            publish_read_end(session, ERROR_SUCCESS);
            return 0;
        }
        if (!SetEvent(session->activity_event)) {
            publish_read_end(session, GetLastError());
            return 1;
        }
        {
            HANDLE events[2] = {
                session->stop_event,
                session->read_consumed_event,
            };
            DWORD wait_result = WaitForMultipleObjects(
                2,
                events,
                FALSE,
                INFINITE);

            if (wait_result == WAIT_OBJECT_0) {
                return 0;
            }
            if (wait_result != WAIT_OBJECT_0 + 1) {
                publish_read_end(session, GetLastError());
                return 1;
            }
        }
    }
}

static DWORD WINAPI writer_thread_main(LPVOID parameter)
{
    boxrig_win32_pty_session *session = parameter;
    HANDLE events[2] = {
        session->stop_event,
        session->write_requested_event,
    };

    for (;;) {
        DWORD wait_result = WaitForMultipleObjects(
            2,
            events,
            FALSE,
            INFINITE);
        size_t offset = 0;
        DWORD error = ERROR_SUCCESS;
        size_t length;

        if (wait_result == WAIT_OBJECT_0) {
            return 0;
        }
        if (wait_result != WAIT_OBJECT_0 + 1) {
            return 1;
        }
        EnterCriticalSection(&session->lock);
        if (session->stopping) {
            LeaveCriticalSection(&session->lock);
            return 0;
        }
        length = session->write_length;
        LeaveCriticalSection(&session->lock);
        while (offset < length) {
            DWORD written = 0;
            bool write_succeeded;
            bool stopping;

            /* Keep the stop decision and in-I/O marker atomic with respect to
             * stop_workers, matching the reader's cancellation protocol. */
            EnterCriticalSection(&session->lock);
            if (session->stopping) {
                LeaveCriticalSection(&session->lock);
                return 0;
            }
            session->writer_in_io = true;
            LeaveCriticalSection(&session->lock);
            write_succeeded = WriteFile(
                boxrig_win32_pty_input_handle(session->pty),
                session->write_bytes + offset,
                (DWORD)(length - offset),
                &written,
                NULL) != 0;
            if (!write_succeeded) {
                error = GetLastError();
            }
            EnterCriticalSection(&session->lock);
            session->writer_in_io = false;
            stopping = session->stopping;
            LeaveCriticalSection(&session->lock);
            if (stopping) {
                return 0;
            }
            if (!write_succeeded || written == 0) {
                if (error == ERROR_SUCCESS) {
                    error = ERROR_WRITE_FAULT;
                }
                break;
            }
            offset += (size_t)written;
        }
        EnterCriticalSection(&session->lock);
        if (session->stopping) {
            LeaveCriticalSection(&session->lock);
            return 0;
        }
        session->write_pending = false;
        session->write_complete = true;
        session->write_completed_length = offset;
        session->write_error = error;
        LeaveCriticalSection(&session->lock);
        if (!SetEvent(session->activity_event)) {
            return 1;
        }
    }
}

static boxrig_transport_status read_transport(
    void *opaque_session,
    uint8_t *bytes,
    size_t capacity,
    size_t *length_out)
{
    boxrig_win32_pty_session *session = opaque_session;
    boxrig_transport_status status;
    bool consumed = false;

    *length_out = 0;
    EnterCriticalSection(&session->lock);
    if (session->read_offset < session->read_length) {
        size_t length = session->read_length - session->read_offset;

        if (length > capacity) {
            length = capacity;
        }
        memcpy(bytes, session->read_bytes + session->read_offset, length);
        session->read_offset += length;
        *length_out = length;
        if (session->read_offset == session->read_length) {
            session->read_offset = 0;
            session->read_length = 0;
            consumed = true;
        }
        status = BOXRIG_TRANSPORT_OK;
    } else if (session->read_error != ERROR_SUCCESS) {
        SetLastError(session->read_error);
        status = BOXRIG_TRANSPORT_ERROR;
    } else if (session->read_closed) {
        status = BOXRIG_TRANSPORT_CLOSED;
    } else {
        status = BOXRIG_TRANSPORT_WOULD_BLOCK;
    }
    LeaveCriticalSection(&session->lock);
    if (consumed && !SetEvent(session->read_consumed_event)) {
        *length_out = 0;
        return BOXRIG_TRANSPORT_ERROR;
    }
    return status;
}

static boxrig_transport_status write_transport(
    void *opaque_session,
    const uint8_t *bytes,
    size_t length,
    size_t *length_out)
{
    boxrig_win32_pty_session *session = opaque_session;
    size_t scheduled_length;

    *length_out = 0;
    EnterCriticalSection(&session->lock);
    if (session->write_complete) {
        DWORD error = session->write_error;

        session->write_complete = false;
        session->write_error = ERROR_SUCCESS;
        if (error != ERROR_SUCCESS) {
            SetLastError(error);
            LeaveCriticalSection(&session->lock);
            return error == ERROR_BROKEN_PIPE
                       ? BOXRIG_TRANSPORT_CLOSED
                       : BOXRIG_TRANSPORT_ERROR;
        }
        *length_out = session->write_completed_length;
        session->write_completed_length = 0;
        LeaveCriticalSection(&session->lock);
        return BOXRIG_TRANSPORT_OK;
    }
    if (session->write_pending) {
        LeaveCriticalSection(&session->lock);
        return BOXRIG_TRANSPORT_WOULD_BLOCK;
    }
    scheduled_length = length;
    if (scheduled_length > sizeof(session->write_bytes)) {
        scheduled_length = sizeof(session->write_bytes);
    }
    memcpy(session->write_bytes, bytes, scheduled_length);
    session->write_length = scheduled_length;
    session->write_pending = true;
    LeaveCriticalSection(&session->lock);
    if (!SetEvent(session->write_requested_event)) {
        EnterCriticalSection(&session->lock);
        session->write_pending = false;
        LeaveCriticalSection(&session->lock);
        return BOXRIG_TRANSPORT_ERROR;
    }
    return BOXRIG_TRANSPORT_WOULD_BLOCK;
}

static boxrig_transport_status resize_transport(
    void *opaque_session,
    size_t rows,
    size_t columns)
{
    boxrig_win32_pty_session *session = opaque_session;

    return boxrig_win32_pty_resize(session->pty, rows, columns) == 0
               ? BOXRIG_TRANSPORT_OK
               : BOXRIG_TRANSPORT_ERROR;
}

static void stop_workers(boxrig_win32_pty_session *session)
{
    bool cancel_reader;
    bool cancel_writer;

    if (session == NULL) {
        return;
    }
    EnterCriticalSection(&session->lock);
    session->stopping = true;
    cancel_reader = session->reader_in_io;
    cancel_writer = session->writer_in_io;
    LeaveCriticalSection(&session->lock);
    if (session->stop_event != NULL) {
        (void)SetEvent(session->stop_event);
    }
    if (session->read_consumed_event != NULL) {
        (void)SetEvent(session->read_consumed_event);
    }
    if (session->write_requested_event != NULL) {
        (void)SetEvent(session->write_requested_event);
    }
    /* Closing a Wine ConPTY does not always release a synchronous anonymous
     * pipe operation after Ctrl+C. Cancel only calls known to be active; an
     * ERROR_NOT_FOUND race means the call completed before cancellation and
     * the worker will observe stopping before issuing another one. */
    if (cancel_reader && session->reader_thread != NULL) {
        (void)CancelSynchronousIo(session->reader_thread);
    }
    if (cancel_writer && session->writer_thread != NULL) {
        (void)CancelSynchronousIo(session->writer_thread);
    }
    boxrig_win32_pty_close(session->pty);
    if (session->reader_thread != NULL) {
        (void)WaitForSingleObject(session->reader_thread, INFINITE);
    }
    if (session->writer_thread != NULL) {
        (void)WaitForSingleObject(session->writer_thread, INFINITE);
    }
}

boxrig_session_status boxrig_win32_pty_session_create(
    boxrig_terminal *terminal,
    boxrig_win32_pty *pty,
    const boxrig_session_config *config,
    boxrig_win32_pty_session **session_out)
{
    boxrig_win32_pty_session *session;
    boxrig_session_transport transport;
    boxrig_session_status status;

    if (session_out != NULL) {
        *session_out = NULL;
    }
    if (terminal == NULL || pty == NULL || config == NULL ||
        session_out == NULL || boxrig_win32_pty_input_handle(pty) == NULL ||
        boxrig_win32_pty_output_handle(pty) == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return BOXRIG_SESSION_INVALID_ARGUMENT;
    }
    session = calloc(1, sizeof(*session));
    if (session == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return BOXRIG_SESSION_OUT_OF_MEMORY;
    }
    session->pty = pty;
    InitializeCriticalSection(&session->lock);
    session->activity_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    session->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    session->read_consumed_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    session->write_requested_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (session->activity_event == NULL || session->stop_event == NULL ||
        session->read_consumed_event == NULL ||
        session->write_requested_event == NULL) {
        status = BOXRIG_SESSION_TRANSPORT_ERROR;
        goto failure;
    }
    memset(&transport, 0, sizeof(transport));
    transport.context = session;
    transport.read = read_transport;
    transport.write = write_transport;
    transport.resize = resize_transport;
    status = boxrig_session_create(
        terminal,
        &transport,
        config,
        &session->portable);
    if (status != BOXRIG_SESSION_OK) {
        SetLastError(status == BOXRIG_SESSION_OUT_OF_MEMORY
                         ? ERROR_NOT_ENOUGH_MEMORY
                         : ERROR_INVALID_PARAMETER);
        goto failure;
    }
    session->reader_thread = CreateThread(
        NULL,
        0,
        reader_thread_main,
        session,
        0,
        NULL);
    if (session->reader_thread == NULL) {
        status = BOXRIG_SESSION_TRANSPORT_ERROR;
        goto failure;
    }
    session->writer_thread = CreateThread(
        NULL,
        0,
        writer_thread_main,
        session,
        0,
        NULL);
    if (session->writer_thread == NULL) {
        status = BOXRIG_SESSION_TRANSPORT_ERROR;
        goto failure;
    }
    *session_out = session;
    return BOXRIG_SESSION_OK;

failure:
    {
        DWORD saved_error = GetLastError();

        stop_workers(session);
        boxrig_session_destroy(session->portable);
        close_handle(&session->reader_thread);
        close_handle(&session->writer_thread);
        close_handle(&session->write_requested_event);
        close_handle(&session->read_consumed_event);
        close_handle(&session->stop_event);
        close_handle(&session->activity_event);
        DeleteCriticalSection(&session->lock);
        free(session);
        SetLastError(saved_error == ERROR_SUCCESS
                         ? ERROR_GEN_FAILURE
                         : saved_error);
    }
    return status;
}

void boxrig_win32_pty_session_destroy(
    boxrig_win32_pty_session *session)
{
    if (session == NULL) {
        return;
    }
    stop_workers(session);
    boxrig_session_destroy(session->portable);
    close_handle(&session->reader_thread);
    close_handle(&session->writer_thread);
    close_handle(&session->write_requested_event);
    close_handle(&session->read_consumed_event);
    close_handle(&session->stop_event);
    close_handle(&session->activity_event);
    DeleteCriticalSection(&session->lock);
    free(session);
}

HANDLE boxrig_win32_pty_session_activity_handle(
    const boxrig_win32_pty_session *session)
{
    return session == NULL ? NULL : session->activity_event;
}

uint32_t boxrig_win32_pty_session_interests(
    const boxrig_win32_pty_session *session)
{
    return session == NULL
               ? BOXRIG_SESSION_INTEREST_NONE
               : boxrig_session_interests(session->portable);
}

boxrig_session_status boxrig_win32_pty_session_dispatch(
    boxrig_win32_pty_session *session,
    boxrig_session_dispatch_result *result_out)
{
    if (session == NULL || result_out == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return BOXRIG_SESSION_INVALID_ARGUMENT;
    }
    if (!ResetEvent(session->activity_event)) {
        return BOXRIG_SESSION_TRANSPORT_ERROR;
    }
    return boxrig_session_dispatch(
        session->portable,
        BOXRIG_SESSION_READY_READ | BOXRIG_SESSION_READY_WRITE,
        result_out);
}

boxrig_session_status boxrig_win32_pty_session_resize(
    boxrig_win32_pty_session *session,
    size_t rows,
    size_t columns)
{
    if (session == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return BOXRIG_SESSION_INVALID_ARGUMENT;
    }
    return boxrig_session_resize(session->portable, rows, columns);
}
