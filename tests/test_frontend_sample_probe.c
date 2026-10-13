/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "boxrig/session.h"
#include "boxrig/ui/view.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            boxrig_terminal_destroy(terminal);                                \
            return 1;                                                          \
        }                                                                      \
    } while (0)

typedef struct transport_state {
    bool read;
    bool written;
    bool resized;
    bool destroyed;
} transport_state;

static boxrig_transport_status read_transport(
    void *context, uint8_t *bytes, size_t capacity, size_t *length_out)
{
    transport_state *state = context;
    *length_out = 0u;
    if (state->destroyed || capacity == 0u) {
        return BOXRIG_TRANSPORT_ERROR;
    }
    if (state->read) {
        return BOXRIG_TRANSPORT_WOULD_BLOCK;
    }
    bytes[0] = 'R';
    *length_out = 1u;
    state->read = true;
    return BOXRIG_TRANSPORT_OK;
}

static boxrig_transport_status write_transport(
    void *context, const uint8_t *bytes, size_t length, size_t *length_out)
{
    transport_state *state = context;
    *length_out = 0u;
    if (state->destroyed || state->written || length != 1u || bytes[0] != 'K') {
        return BOXRIG_TRANSPORT_ERROR;
    }
    *length_out = 1u;
    state->written = true;
    return BOXRIG_TRANSPORT_OK;
}

static boxrig_transport_status resize_transport(
    void *context, size_t rows, size_t columns)
{
    transport_state *state = context;
    if (state->destroyed || rows != 28u || columns != 88u) {
        return BOXRIG_TRANSPORT_ERROR;
    }
    state->resized = true;
    return BOXRIG_TRANSPORT_OK;
}

static void destroy_transport(void *context)
{
    transport_state *state = context;
    state->destroyed = true;
}

static int lifetime_contract(void)
{
    static const uint8_t copied_text[] = "copy";
    unsigned int scenario;
    boxrig_terminal *terminal = NULL;

    for (scenario = 0u; scenario < 9u; scenario++) {
        pid_t child = fork();
        pid_t waited;
        int status;

        CHECK(child != -1);
        if (child == 0) {
            boxrig_terminal_config config = boxrig_terminal_config_default();
            boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
            boxrig_ui_model *model = NULL;
            boxrig_ui_view *view = NULL;
            boxrig_ui_host_item_consumer *consumer = NULL;
            boxrig_composition *composition = NULL;
            boxrig_session *session = NULL;
            boxrig_text_snapshot *snapshot = NULL;
            boxrig_session_config session_config = boxrig_session_config_default();
            transport_state state = {false, false, false, false};
            boxrig_session_transport transport = {
                &state, read_transport, write_transport,
                resize_transport, destroy_transport};

            if (scenario >= 7u) {
                config.rows = 1u;
                config.columns = 4u;
            }
            if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK) {
                _Exit(98);
            }
            if (scenario >= 7u) {
                size_t consumed = 0u;
                if (boxrig_terminal_feed(terminal, copied_text, 4u, &consumed) !=
                        BOXRIG_STATUS_OK || consumed != 4u) {
                    _Exit(98);
                }
            }
            if (boxrig_ui_model_create(terminal, &model) != BOXRIG_UI_OK ||
                boxrig_ui_view_create(model, &view_config, &view) != BOXRIG_UI_OK ||
                boxrig_ui_host_item_consumer_create(model, &consumer) != BOXRIG_UI_OK ||
                boxrig_composition_create(terminal,
                    boxrig_composition_limits_default(), &composition) !=
                    BOXRIG_COMPOSITION_OK ||
                boxrig_session_create(terminal, &transport, &session_config,
                                      &session) != BOXRIG_SESSION_OK) {
                /* Setup failure must never look like an expected audit failure. */
                _Exit(98);
            }
            if (scenario >= 7u) {
                boxrig_ui_allocation_input allocation = {0};
                boxrig_ui_layout_result layout;
                boxrig_ui_hit_test_result hit;
                allocation.allocation_width = 4 * BOXRIG_UI_FIXED_ONE;
                allocation.allocation_height = BOXRIG_UI_FIXED_ONE;
                allocation.cell_width = BOXRIG_UI_FIXED_ONE;
                allocation.cell_height = BOXRIG_UI_FIXED_ONE;
                allocation.cell_metrics_revision = 1u;
                allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
                if (boxrig_ui_view_acquire_layout_authority(view) != BOXRIG_UI_OK ||
                    boxrig_ui_view_accept_allocation(view, &allocation, &layout) != BOXRIG_UI_OK ||
                    boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE / 4,
                        BOXRIG_UI_FIXED_ONE / 2, &hit) != BOXRIG_UI_OK ||
                    boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_WORD,
                                                  &hit) != BOXRIG_UI_OK ||
                    boxrig_ui_view_create_selection_snapshot(view, &snapshot) != BOXRIG_UI_OK) {
                    _Exit(98);
                }
            }
            if (scenario == 1u) {
                /* Normal process exit must not disguise retained borrowers. */
                exit(0);
            }
            if (scenario == 2u) {
                boxrig_terminal_destroy(terminal);
                exit(0);
            }
            if (scenario == 3u) {
                boxrig_ui_model_destroy(model);
                exit(0);
            }
            boxrig_ui_view_destroy(view);
            if (scenario == 4u) {
                boxrig_ui_model_destroy(model);
                exit(0);
            }
            boxrig_ui_host_item_consumer_destroy(consumer);
            boxrig_ui_model_destroy(model);
            if (scenario != 6u) {
                boxrig_session_destroy(session);
                if (!state.destroyed) {
                    _Exit(98);
                }
            }
            if (scenario == 5u) {
                boxrig_terminal_destroy(terminal);
                exit(0);
            }
            boxrig_composition_destroy(composition);
            if (scenario == 6u) {
                boxrig_terminal_destroy(terminal);
                exit(0);
            }
            boxrig_terminal_destroy(terminal);
            if (scenario == 8u) {
                size_t length;
                const uint8_t *bytes = boxrig_text_snapshot_data(snapshot, &length);
                /* Owned copy data may legitimately outlive its source model. */
                if (bytes == NULL || length != 4u || memcmp(bytes, "copy", 4u) != 0) {
                    _Exit(98);
                }
                boxrig_text_snapshot_destroy(snapshot);
            }
            exit(0);
        }
        do {
            waited = waitpid(child, &status, 0);
        } while (waited == -1 && errno == EINTR);
        CHECK(waited == child);
        CHECK(WIFEXITED(status));
        if (WEXITSTATUS(status) != (scenario == 0u || scenario == 8u ? 0 : 97)) {
            fprintf(stderr, "lifetime scenario %u exited with %d\n",
                    scenario, WEXITSTATUS(status));
        }
        CHECK(WEXITSTATUS(status) == (scenario == 0u || scenario == 8u ? 0 : 97));
    }
    printf("PASS sample probe rejects live borrowers and invalid destruction order\n");
    printf("PASS sample probe audits owned copy snapshots independently of core lifetime\n");
    return 0;
}

static int transport_contract(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_session *session = NULL;
    boxrig_session_config session_config = boxrig_session_config_default();
    transport_state state = {false, false, false, false};
    boxrig_session_transport transport = {
        &state, read_transport, write_transport, resize_transport, destroy_transport};
    boxrig_key_event key;
    boxrig_session_dispatch_result result;
    boxrig_cell_view cell;
    size_t rows;
    size_t columns;

    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_session_create(terminal, &transport, &session_config, &session) ==
          BOXRIG_SESSION_OK);
    memset(&key, 0, sizeof(key));
    key.action = BOXRIG_KEY_PRESS;
    key.text = "K";
    key.text_length = 1u;
    CHECK(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    CHECK(boxrig_session_dispatch(session,
              BOXRIG_SESSION_READY_READ | BOXRIG_SESSION_READY_WRITE, &result) ==
          BOXRIG_SESSION_OK);
    CHECK(state.read && state.written && !state.destroyed);
    CHECK(result.terminal_bytes_fed == 1u && result.transport_bytes_written == 1u);
    CHECK(boxrig_terminal_get_cell(terminal, 0u, 0u, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1u && cell.text[0] == 'R');
    CHECK(boxrig_session_resize(session, 28u, 88u) == BOXRIG_SESSION_OK);
    boxrig_terminal_size(terminal, &rows, &columns);
    CHECK(state.resized && rows == 28u && columns == 88u);
    boxrig_session_destroy(session);
    CHECK(state.destroyed);
    boxrig_terminal_destroy(terminal);
    printf("PASS sample probe preserves transport bytes geometry and owned cleanup\n");
    return 0;
}

int main(void)
{
    static const char payload[] = "copy-target";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_paste_event event;
    const uint8_t *bytes;
    size_t length;
    size_t index;

    CHECK(lifetime_contract() == 0);
    CHECK(transport_contract() == 0);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.phase = BOXRIG_PASTE_BEGIN;
    CHECK(boxrig_terminal_send_paste(terminal, &event) == BOXRIG_STATUS_OK);
    event.phase = BOXRIG_PASTE_CHUNK;
    event.text = payload;
    event.text_length = sizeof(payload) - 1u;
    CHECK(boxrig_terminal_send_paste(terminal, &event) ==
          BOXRIG_STATUS_WOULD_BLOCK);
    length = boxrig_terminal_output_peek(terminal, &bytes);
    CHECK(length == 64u);
    for (index = 0; index < length; index++) {
        CHECK(bytes[index] == '!');
    }
    CHECK(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_paste(terminal, &event) == BOXRIG_STATUS_OK);
    length = boxrig_terminal_output_peek(terminal, &bytes);
    CHECK(length == sizeof(payload) - 1u);
    CHECK(memcmp(bytes, payload, length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.phase = BOXRIG_PASTE_END;
    CHECK(boxrig_terminal_send_paste(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &bytes) == 0u);
    boxrig_terminal_destroy(terminal);
    printf("PASS sample probe preserves real paste backpressure and retry bytes\n");
    return 0;
}
