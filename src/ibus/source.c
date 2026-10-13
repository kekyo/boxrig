/* boxrig - Under MIT. */
#include "source.h"
#include <stdlib.h>

typedef struct ibus_source {
    GSource base;
    boxrig_ibus_client *client;
    gpointer *tags;
    struct pollfd *descriptors;
    size_t count, capacity;
    gint64 deadline;
    bool prepared, failed;
    void (*failure)(void *);
    void *data;
} ibus_source;

static gboolean prepare(GSource *base, gint *timeout)
{
    ibus_source *source = (ibus_source *)base;
    const struct pollfd *descriptors;
    size_t count;
    *timeout = -1;
    if (boxrig_ibus_prepare_poll(source->client, &descriptors, &count, timeout) != BOXRIG_UI_OK)
        goto failed;
    bool changed = count != source->count;
    for (size_t index = 0; !changed && index < count; index++)
        changed = descriptors[index].fd != source->descriptors[index].fd ||
            descriptors[index].events != source->descriptors[index].events;
    if (changed)
        for (size_t index = 0; index < source->count; index++)
            g_source_remove_unix_fd(base, source->tags[index]);
    if (count > source->capacity) {
        gpointer *tags = calloc(count, sizeof(*tags));
        struct pollfd *copy = calloc(count, sizeof(*copy));
        if (tags == NULL || copy == NULL) { free(tags); free(copy); goto failed; }
        free(source->tags);
        free(source->descriptors);
        source->tags = tags;
        source->descriptors = copy;
        source->capacity = count;
    }
    for (size_t index = 0; index < count; index++) {
        source->descriptors[index] = descriptors[index];
        if (changed) source->tags[index] = g_source_add_unix_fd(base, descriptors[index].fd,
            (GIOCondition)descriptors[index].events);
    }
    source->count = count;
    source->prepared = true;
    source->deadline = *timeout < 0 ? -1 : g_source_get_time(base) + (gint64)*timeout * 1000;
    return *timeout == 0;
failed:
    source->failed = true;
    *timeout = 0;
    return TRUE;
}

static gboolean check(GSource *base)
{
    ibus_source *source = (ibus_source *)base;
    if (source->failed || (source->deadline >= 0 && g_source_get_time(base) >= source->deadline)) return TRUE;
    for (size_t index = 0; index < source->count; index++)
        if (g_source_query_unix_fd(base, source->tags[index]) != 0) return TRUE;
    return FALSE;
}

static gboolean dispatch(GSource *base, GSourceFunc callback, gpointer data)
{
    (void)callback; (void)data;
    ibus_source *source = (ibus_source *)base;
    if (source->failed) { source->failure(source->data); return G_SOURCE_REMOVE; }
    if (source->prepared) {
        for (size_t index = 0; index < source->count; index++)
            source->descriptors[index].revents = (short)g_source_query_unix_fd(base, source->tags[index]);
        source->prepared = false;
        if (boxrig_ibus_dispatch_poll(source->client, source->descriptors, source->count) != BOXRIG_UI_OK) {
            source->failure(source->data);
            return G_SOURCE_REMOVE;
        }
    }
    boxrig_ibus_update(source->client);
    return G_SOURCE_CONTINUE;
}

static void finalize(GSource *base)
{
    ibus_source *source = (ibus_source *)base;
    free(source->tags);
    free(source->descriptors);
}

GSource *boxrig_ibus_source_new(boxrig_ibus_client *client, void (*failed)(void *), void *data)
{
    static GSourceFuncs functions = {.prepare = prepare, .check = check,
        .dispatch = dispatch, .finalize = finalize};
    GSource *base = g_source_new(&functions, sizeof(ibus_source));
    ibus_source *source = (ibus_source *)base;
    source->client = client;
    source->failure = failed;
    source->data = data;
    source->deadline = -1;
    g_source_set_name(base, "boxrig-ibus");
    return base;
}
