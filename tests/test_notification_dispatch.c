/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/notification_dispatch.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t completions;
static boxrig_host_request_result last_result;

boxrig_status __real_boxrig_terminal_complete_host_request(
    boxrig_terminal *, uint64_t, boxrig_host_request_result,
    const uint8_t *, size_t);

boxrig_status __wrap_boxrig_terminal_complete_host_request(
    boxrig_terminal *terminal, uint64_t id, boxrig_host_request_result result,
    const uint8_t *payload, size_t length)
{
    boxrig_status status = __real_boxrig_terminal_complete_host_request(
        terminal, id, result, payload, length);
    assert(status == BOXRIG_STATUS_OK);
    completions++;
    last_result = result;
    return status;
}

typedef struct fixture {
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_host_item_consumer *consumer;
    boxrig_notification_dispatcher *dispatcher;
    uint64_t now;
    uint64_t id;
    const boxrig_notification_view *notification;
    size_t submissions;
    size_t cancellations;
    size_t availability_checks;
    bool available;
    bool synchronous;
    bool fail;
} fixture;

static uint64_t clock_read(void *context)
{
    return ((fixture *)context)->now;
}

static bool available(void *context)
{
    fixture *f = context;
    f->availability_checks++;
    return f->available;
}

static int submit(
    void *context, boxrig_notification_dispatcher *dispatcher, uint64_t id,
    const boxrig_notification_view *notification)
{
    fixture *f = context;
    f->submissions++;
    f->id = id;
    f->notification = notification;
    if (f->fail) {
        return -1;
    }
    if (f->synchronous) {
        assert(boxrig_notification_dispatcher_complete(
            dispatcher, id, BOXRIG_HOST_REQUEST_SUCCESS) == 0);
    }
    return 0;
}

static void cancel(void *context, uint64_t id)
{
    fixture *f = context;
    assert(id == f->id);
    /* Borrowed data must remain valid until cancellation returns. */
    assert(f->notification->title.length > 0);
    f->cancellations++;
}

static void create(fixture *f, bool allow)
{
    memset(f, 0, sizeof(*f));
    completions = 0;
    f->available = true;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    assert(boxrig_terminal_create(&config, &f->terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(f->terminal, &f->model) == BOXRIG_UI_OK);
    assert(boxrig_ui_host_item_consumer_create(f->model, &f->consumer) ==
           BOXRIG_UI_OK);
    boxrig_notification_policy_config policy =
        boxrig_notification_policy_config_default(clock_read, f);
    policy.allow_notifications = allow;
    boxrig_notification_provider provider = {available, submit, cancel, f};
    assert(boxrig_notification_dispatcher_create(
        f->consumer, &policy, &provider, &f->dispatcher) == 0);
}

static void destroy(fixture *f)
{
    boxrig_notification_dispatcher_destroy(f->dispatcher);
    boxrig_ui_host_item_consumer_destroy(f->consumer);
    boxrig_ui_model_destroy(f->model);
    boxrig_terminal_destroy(f->terminal);
}

static uint64_t send(fixture *f, const char *sequence, bool focused, bool visible)
{
    size_t consumed = 0;
    size_t length = strlen(sequence);
    assert(boxrig_terminal_feed(f->terminal, (const uint8_t *)sequence,
        length, &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == length);
    boxrig_host_item item;
    assert(boxrig_ui_host_item_consumer_peek(f->consumer, &item));
    uint64_t id = item.request_id;
    assert(boxrig_notification_dispatcher_process(
        f->dispatcher, focused, visible) == 0);
    assert(!boxrig_ui_host_item_consumer_peek(f->consumer, &item));
    return id;
}

static void test_pending_and_duplicate(void)
{
    fixture f;
    create(&f, true);
    uint64_t first = send(&f, "\033]99;i=a;first\033\\", false, true);
    assert(completions == 0 && f.submissions == 1);
    uint64_t second = send(&f, "\033]99;i=a;second\033\\", false, true);
    assert(f.submissions == 1);
    assert(f.notification->title.length == 5);
    assert(memcmp(f.notification->title.bytes, "first", 5) == 0);
    assert(boxrig_notification_dispatcher_complete(
        f.dispatcher, first, BOXRIG_HOST_REQUEST_SUCCESS) == 0);
    assert(f.id == second && f.submissions == 2 && completions == 1);
    assert(memcmp(f.notification->title.bytes, "second", 6) == 0);
    assert(boxrig_notification_dispatcher_complete(
        f.dispatcher, first, BOXRIG_HOST_REQUEST_SUCCESS) == -1);
    assert(completions == 1);
    assert(boxrig_notification_dispatcher_complete(
        f.dispatcher, second, BOXRIG_HOST_REQUEST_SUCCESS) == 0);
    send(&f, "\033]99;i=a;second\033\\", false, true);
    assert(completions == 3 && f.submissions == 2);
    destroy(&f);
    assert(f.cancellations == 0);
}

static void test_policy_failure_and_cancel(void)
{
    fixture f;
    create(&f, false);
    send(&f, "\033]99;;denied\033\\", false, true);
    assert(last_result == BOXRIG_HOST_REQUEST_DENIED);
    assert(f.availability_checks == 0 && f.submissions == 0);
    destroy(&f);

    create(&f, true);
    send(&f, "\033]99;o=invisible;unknown visibility\033\\", false, true);
    assert(last_result == BOXRIG_HOST_REQUEST_DENIED);
    f.available = false;
    send(&f, "\033]99;;unavailable\033\\", false, true);
    assert(last_result == BOXRIG_HOST_REQUEST_UNAVAILABLE);
    f.available = true;
    f.fail = true;
    send(&f, "\033]99;;failed\033\\", false, true);
    assert(last_result == BOXRIG_HOST_REQUEST_UNAVAILABLE);
    f.fail = false;
    send(&f, "\033]99;i=a;pending\033\\", false, true);
    send(&f, "\033]99;i=a;queued\033\\", false, true);
    assert(completions == 3);
    destroy(&f);
    assert(f.cancellations == 1 && completions == 5);
    assert(last_result == BOXRIG_HOST_REQUEST_UNAVAILABLE);
}

static void test_inline_completion_and_rate_limit(void)
{
    fixture f;
    create(&f, true);
    f.synchronous = true;
    for (size_t i = 0; i < 6; i++) {
        send(&f, "\033]99;;anonymous\033\\", false, true);
    }
    assert(completions == 6 && f.submissions == 5);
    assert(last_result == BOXRIG_HOST_REQUEST_DENIED);
    f.now = 2000;
    send(&f, "\033]99;;refilled\033\\", false, true);
    assert(completions == 7 && f.submissions == 6);
    assert(last_result == BOXRIG_HOST_REQUEST_SUCCESS);
    destroy(&f);
}

int main(void)
{
    test_pending_and_duplicate();
    test_policy_failure_and_cancel();
    test_inline_completion_and_rate_limit();
    puts("PASS notification provider ownership, ordering, policy and completion");
    return 0;
}
