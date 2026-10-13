/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/notification_dispatch.h"

#include <stdlib.h>
#include <string.h>

typedef struct pending_notification {
    struct pending_notification *next;
    uint64_t id;
    uint64_t timestamp;
    bool started;
    bool finished;
    boxrig_host_request_result result;
    boxrig_notification_view notification;
    boxrig_notification_text_view types[16];
    uint8_t content[];
} pending_notification;

struct boxrig_notification_dispatcher {
    boxrig_ui_host_item_consumer *consumer;
    boxrig_notification_policy_state *policy;
    boxrig_notification_provider provider;
    pending_notification *pending;
    bool focused;
    bool visible;
    bool processing;
    bool destroying;
    bool failed;
};

static bool add_text_size(size_t *size, boxrig_notification_text_view text)
{
    if ((text.length != 0 && text.bytes == NULL) ||
        text.length > SIZE_MAX - *size) {
        return false;
    }
    *size += text.length;
    return true;
}

static boxrig_notification_text_view copy_text(
    uint8_t *content, size_t *offset, boxrig_notification_text_view source)
{
    boxrig_notification_text_view result = {NULL, source.length};
    if (source.length != 0) {
        result.bytes = content + *offset;
        memcpy(content + *offset, source.bytes, source.length);
        *offset += source.length;
    }
    return result;
}

static pending_notification *copy_item(const boxrig_host_item *item)
{
    const boxrig_notification_view *n = item->notification;
    size_t size = sizeof(pending_notification);
    if (n == NULL || item->request_id == 0 || n->identifier.length > 128 ||
        n->type_count > 16 || (n->type_count != 0 && n->types == NULL) ||
        n->occasion < BOXRIG_NOTIFICATION_ALWAYS ||
        n->occasion > BOXRIG_NOTIFICATION_INVISIBLE ||
        n->urgency < BOXRIG_NOTIFICATION_URGENCY_LOW ||
        n->urgency > BOXRIG_NOTIFICATION_URGENCY_CRITICAL ||
        !add_text_size(&size, n->identifier) ||
        !add_text_size(&size, n->title) || !add_text_size(&size, n->body) ||
        !add_text_size(&size, n->application_name)) {
        return NULL;
    }
    for (size_t i = 0; i < n->type_count; i++) {
        if (!add_text_size(&size, n->types[i])) {
            return NULL;
        }
    }
    pending_notification *p = calloc(1, size);
    if (p == NULL) {
        return NULL;
    }
    p->id = item->request_id;
    p->notification = *n;
    size_t offset = 0;
    p->notification.identifier = copy_text(p->content, &offset, n->identifier);
    p->notification.title = copy_text(p->content, &offset, n->title);
    p->notification.body = copy_text(p->content, &offset, n->body);
    p->notification.application_name =
        copy_text(p->content, &offset, n->application_name);
    p->notification.types = n->type_count == 0 ? NULL : p->types;
    for (size_t i = 0; i < n->type_count; i++) {
        p->types[i] = copy_text(p->content, &offset, n->types[i]);
    }
    return p;
}

static bool blocked(
    const boxrig_notification_dispatcher *d, const pending_notification *p)
{
    if (!p->notification.has_identifier) {
        return false;
    }
    for (const pending_notification *q = d->pending; q != p; q = q->next) {
        if (q->notification.has_identifier &&
            q->notification.identifier.length == p->notification.identifier.length &&
            (p->notification.identifier.length == 0 ||
             memcmp(q->notification.identifier.bytes,
                 p->notification.identifier.bytes,
                 p->notification.identifier.length) == 0)) {
            return true;
        }
    }
    return false;
}

static void finish(boxrig_notification_dispatcher *d, pending_notification *p)
{
    if (p->started && p->result == BOXRIG_HOST_REQUEST_SUCCESS) {
        (void)boxrig_notification_policy_record_success(
            d->policy, &p->notification, p->timestamp);
    }
    if (boxrig_ui_host_item_consumer_complete(d->consumer, p->id,
            p->result, NULL, 0) != BOXRIG_UI_OK) {
        d->failed = true;
    }
    free(p);
}

static int schedule(boxrig_notification_dispatcher *d)
{
    if (d->processing || d->destroying) {
        return d->failed ? -1 : 0;
    }
    d->processing = true;
    pending_notification **link = &d->pending;
    while (*link != NULL) {
        pending_notification *p = *link;
        if (p->finished) {
            *link = p->next;
            finish(d, p);
            link = &d->pending;
            continue;
        }
        if (p->started || blocked(d, p)) {
            link = &p->next;
            continue;
        }
        boxrig_notification_policy_evaluation e =
            boxrig_notification_policy_evaluate(d->policy, &p->notification,
                d->focused, d->visible, false);
        if (e.decision == BOXRIG_NOTIFICATION_POLICY_UNAVAILABLE &&
            d->provider.available != NULL &&
            d->provider.available(d->provider.context)) {
            e = boxrig_notification_policy_evaluate(d->policy, &p->notification,
                d->focused, d->visible, true);
        }
        if (e.decision == BOXRIG_NOTIFICATION_POLICY_DISPLAY) {
            p->started = true;
            p->timestamp = e.timestamp_milliseconds;
            /* Inline completion marks the request; this frame owns reaping it. */
            int status = d->provider.submit(
                d->provider.context, d, p->id, &p->notification);
            if (status != 0 && !p->finished) {
                p->finished = true;
                p->result = BOXRIG_HOST_REQUEST_UNAVAILABLE;
            }
        } else {
            p->finished = true;
            p->result = e.decision == BOXRIG_NOTIFICATION_POLICY_DENIED
                ? BOXRIG_HOST_REQUEST_DENIED
                : e.decision == BOXRIG_NOTIFICATION_POLICY_DUPLICATE
                    ? BOXRIG_HOST_REQUEST_SUCCESS : BOXRIG_HOST_REQUEST_UNAVAILABLE;
        }
        link = &d->pending;
    }
    d->processing = false;
    return d->failed ? -1 : 0;
}

int boxrig_notification_dispatcher_create(
    boxrig_ui_host_item_consumer *consumer,
    const boxrig_notification_policy_config *policy,
    const boxrig_notification_provider *provider,
    boxrig_notification_dispatcher **dispatcher_out)
{
    if (dispatcher_out == NULL) {
        return -1;
    }
    *dispatcher_out = NULL;
    if (boxrig_ui_host_item_consumer_terminal(consumer) == NULL ||
        (provider != NULL && (provider->available == NULL ||
            provider->submit == NULL || provider->cancel == NULL))) {
        return -1;
    }
    boxrig_notification_dispatcher *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        return -1;
    }
    if (boxrig_notification_policy_create(policy, &d->policy) != 0) {
        free(d);
        return -1;
    }
    d->consumer = consumer;
    d->visible = true;
    if (provider != NULL) {
        d->provider = *provider;
    }
    *dispatcher_out = d;
    return 0;
}

void boxrig_notification_dispatcher_set_window_state(
    boxrig_notification_dispatcher *d, bool focused, bool visible)
{
    if (d != NULL) {
        d->focused = focused;
        d->visible = visible;
    }
}

int boxrig_notification_dispatcher_process(
    boxrig_notification_dispatcher *d, bool focused, bool visible)
{
    boxrig_host_item item;
    if (d == NULL || d->processing || d->destroying || d->failed ||
        !boxrig_ui_host_item_consumer_peek(d->consumer, &item) ||
        item.type != BOXRIG_HOST_ITEM_NOTIFICATION) {
        return -1;
    }
    pending_notification *p = copy_item(&item);
    if (p == NULL) {
        return -1;
    }
    if (boxrig_ui_host_item_consumer_consume(d->consumer) != BOXRIG_UI_OK) {
        free(p);
        return -1;
    }
    pending_notification **link = &d->pending;
    while (*link != NULL) {
        link = &(*link)->next;
    }
    *link = p;
    boxrig_notification_dispatcher_set_window_state(d, focused, visible);
    return schedule(d);
}

int boxrig_notification_dispatcher_complete(
    boxrig_notification_dispatcher *d, uint64_t id,
    boxrig_host_request_result result)
{
    if (d == NULL || d->destroying ||
        (result != BOXRIG_HOST_REQUEST_SUCCESS &&
         result != BOXRIG_HOST_REQUEST_DENIED &&
         result != BOXRIG_HOST_REQUEST_UNAVAILABLE)) {
        return -1;
    }
    for (pending_notification *p = d->pending; p != NULL; p = p->next) {
        if (p->id == id && p->started && !p->finished) {
            p->finished = true;
            p->result = result;
            return schedule(d);
        }
    }
    return -1;
}

void boxrig_notification_dispatcher_destroy(boxrig_notification_dispatcher *d)
{
    if (d == NULL) {
        return;
    }
    d->destroying = true;
    while (d->pending != NULL) {
        pending_notification *p = d->pending;
        if (p->started && !p->finished) {
            d->provider.cancel(d->provider.context, p->id);
        }
        d->pending = p->next;
        p->result = BOXRIG_HOST_REQUEST_UNAVAILABLE;
        finish(d, p);
    }
    boxrig_notification_policy_destroy(d->policy);
    free(d);
}
