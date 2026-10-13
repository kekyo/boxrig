/* boxrig - Under MIT. */
#ifndef BOXRIG_GTK_ANIMATION_H
#define BOXRIG_GTK_ANIMATION_H

#include "boxrig/ui/animation.h"
#include <glib.h>
#include <stdlib.h>

/* GTK 3 and GTK 4 use this private adapter in their widget implementation.
 * A registry belongs to the widget's owning thread; entries share one source
 * per main context and never redirect work to the global default context. */
typedef struct boxrig_gtk_animation boxrig_gtk_animation;
typedef struct boxrig_gtk_animation_source {
    GSource source;
    boxrig_gtk_animation *target;
} boxrig_gtk_animation_source;

struct boxrig_gtk_animation {
    GMainContext *context;
    boxrig_ui_animation_runtime *runtime;
    GSource *source;
    uint64_t generation;
    size_t references;
    boxrig_gtk_animation *next;
};

static GPrivate boxrig_gtk_animation_registry = G_PRIVATE_INIT(NULL);

static boxrig_ui_status gtk_animation_read_time(void *context, uint64_t *time_out)
{
    (void)context;
    gint64 microseconds = g_get_monotonic_time();
    if (microseconds < 0) return BOXRIG_UI_UNAVAILABLE;
    *time_out = (uint64_t)microseconds / 1000;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status gtk_animation_reserve(
    void *context, uint64_t deadline, uint64_t generation)
{
    boxrig_gtk_animation *animation = context;
    if (deadline > (uint64_t)G_MAXINT64 / 1000) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    animation->generation = generation;
    g_source_set_ready_time(animation->source, (gint64)(deadline * 1000));
    return BOXRIG_UI_OK;
}

static void gtk_animation_cancel(void *context)
{
    boxrig_gtk_animation *animation = context;
    g_source_set_ready_time(animation->source, -1);
}

static gboolean gtk_animation_dispatch(
    GSource *source, GSourceFunc callback, gpointer data)
{
    (void)callback;
    (void)data;
    boxrig_gtk_animation *animation =
        ((boxrig_gtk_animation_source *)source)->target;
    if (animation == NULL) return G_SOURCE_REMOVE;
    uint64_t generation = animation->generation;
    g_source_set_ready_time(source, -1);
    /* Dispatch only requests frames. Native widget frame processing reports
     * the runtime's retained status through its ordinary error return. */
    (void)boxrig_ui_animation_runtime_dispatch(animation->runtime, generation);
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs gtk_animation_source_functions = {
    NULL, NULL, gtk_animation_dispatch, NULL, NULL, NULL
};

static boxrig_ui_status boxrig_gtk_animation_acquire(
    GMainContext *context, boxrig_gtk_animation **animation_out)
{
    if (animation_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *animation_out = NULL;
    if (context == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    boxrig_gtk_animation *first = g_private_get(&boxrig_gtk_animation_registry);
    for (boxrig_gtk_animation *entry = first; entry != NULL; entry = entry->next) {
        if (entry->context == context) {
            if (entry->references == SIZE_MAX) return BOXRIG_UI_LIMIT_EXCEEDED;
            entry->references++;
            *animation_out = entry;
            return BOXRIG_UI_OK;
        }
    }
    boxrig_gtk_animation *animation = calloc(1, sizeof(*animation));
    if (animation == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    animation->context = g_main_context_ref(context);
    animation->source = g_source_new(&gtk_animation_source_functions,
        sizeof(boxrig_gtk_animation_source));
    ((boxrig_gtk_animation_source *)animation->source)->target = animation;
    boxrig_ui_animation_scheduler scheduler = {
        animation, gtk_animation_read_time, gtk_animation_reserve, gtk_animation_cancel
    };
    boxrig_ui_status status = boxrig_ui_animation_runtime_create(
        &scheduler, &animation->runtime);
    if (status == BOXRIG_UI_OK && g_source_attach(animation->source, context) == 0) {
        status = BOXRIG_UI_UNAVAILABLE;
    }
    if (status != BOXRIG_UI_OK) {
        ((boxrig_gtk_animation_source *)animation->source)->target = NULL;
        boxrig_ui_animation_runtime_destroy(animation->runtime);
        g_source_destroy(animation->source);
        g_source_unref(animation->source);
        g_main_context_unref(animation->context);
        free(animation);
        return status;
    }
    animation->references = 1;
    animation->next = first;
    g_private_set(&boxrig_gtk_animation_registry, animation);
    *animation_out = animation;
    return BOXRIG_UI_OK;
}

static void boxrig_gtk_animation_release(boxrig_gtk_animation *animation)
{
    if (animation == NULL || --animation->references != 0) return;
    boxrig_gtk_animation *first = g_private_get(&boxrig_gtk_animation_registry);
    boxrig_gtk_animation **entry = &first;
    while (*entry != animation) entry = &(*entry)->next;
    *entry = animation->next;
    g_private_set(&boxrig_gtk_animation_registry, first);
    /* Disconnect first: a prepared GLib dispatch must not dereference a freed
     * runtime. Destroying the source also removes its pending dispatch. */
    ((boxrig_gtk_animation_source *)animation->source)->target = NULL;
    boxrig_ui_animation_runtime_destroy(animation->runtime);
    g_source_destroy(animation->source);
    g_source_unref(animation->source);
    g_main_context_unref(animation->context);
    free(animation);
}

static boxrig_ui_animation_runtime *boxrig_gtk_animation_runtime(
    boxrig_gtk_animation *animation)
{
    return animation == NULL ? NULL : animation->runtime;
}

#endif
