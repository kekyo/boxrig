/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "application_internal.h"
#include "event_source_policy.h"

typedef struct boxrig_term_gtk4_dispatch_source {
    GSource source;
    gpointer descriptor_tag;
    GIOCondition descriptor_events;
    boxrig_term_gtk4_application *application;
} boxrig_term_gtk4_dispatch_source;

static bool widget_can_frame(
    const boxrig_term_gtk4_application *application)
{
    return application->widget != NULL && !application->failed &&
           gtk_widget_get_mapped(GTK_WIDGET(application->widget)) &&
           gtk_widget_get_width(GTK_WIDGET(application->widget)) > 0 &&
           gtk_widget_get_height(GTK_WIDGET(application->widget)) > 0;
}

static bool widget_frame_ready(
    const boxrig_term_gtk4_application *application)
{
    return widget_can_frame(application) &&
           boxrig_gtk4_terminal_widget_frame_ready(application->widget);
}

static void update_descriptor(
    boxrig_term_gtk4_dispatch_source *source)
{
    boxrig_term_gtk4_application *application = source->application;
    uint32_t interests = BOXRIG_SESSION_INTEREST_NONE;
    GIOCondition events = G_IO_HUP | G_IO_ERR | G_IO_NVAL;

    if (source->descriptor_tag == NULL) {
        return;
    }
    if (application->session == NULL || application->peer_closed ||
        application->failed) {
        g_source_remove_unix_fd(
            (GSource *)source,
            source->descriptor_tag);
        source->descriptor_tag = NULL;
        return;
    }
    interests = boxrig_session_interests(application->session);
    if ((interests & BOXRIG_SESSION_INTEREST_READ) != 0) {
        events |= G_IO_IN;
    }
    if ((interests & BOXRIG_SESSION_INTEREST_WRITE) != 0) {
        events |= G_IO_OUT;
    }
    if (boxrig_term_gtk4_descriptor_events_need_update(
            (unsigned int)source->descriptor_events,
            (unsigned int)events)) {
        g_source_modify_unix_fd(
            (GSource *)source,
            source->descriptor_tag,
            events);
        source->descriptor_events = events;
    }
}

static gboolean dispatch_source_prepare(
    GSource *base,
    gint *timeout_milliseconds)
{
    boxrig_term_gtk4_dispatch_source *source =
        (boxrig_term_gtk4_dispatch_source *)base;
    boxrig_term_gtk4_application *application = source->application;
    uint32_t interests = BOXRIG_SESSION_INTEREST_NONE;

    update_descriptor(source);
    *timeout_milliseconds = -1;
    if (application->failed) {
        return FALSE;
    }
    if (application->session != NULL && !application->peer_closed) {
        interests = boxrig_session_interests(application->session);
    }
    if (application->host_items_pending ||
        (widget_frame_ready(application) && application->frame_requested) ||
        ((interests & BOXRIG_SESSION_INTEREST_DISPATCH) != 0 &&
         (interests & BOXRIG_SESSION_INTEREST_WRITE) == 0)) {
        return TRUE;
    }
    return FALSE;
}

static gboolean dispatch_source_check(GSource *base)
{
    boxrig_term_gtk4_dispatch_source *source =
        (boxrig_term_gtk4_dispatch_source *)base;

    return source->descriptor_tag != NULL &&
           g_source_query_unix_fd(base, source->descriptor_tag) != 0;
}

static gboolean dispatch_source_dispatch(
    GSource *base,
    GSourceFunc callback,
    gpointer user_data)
{
    boxrig_term_gtk4_dispatch_source *source =
        (boxrig_term_gtk4_dispatch_source *)base;
    boxrig_term_gtk4_application *application = source->application;
    boxrig_session_dispatch_result session_result;
    boxrig_ui_component_frame_result frame_result;
    uint32_t ready = BOXRIG_SESSION_READY_NONE;
    GIOCondition native_ready =
        source->descriptor_tag == NULL
            ? 0
            : g_source_query_unix_fd(base, source->descriptor_tag);
    size_t recognition_work;
    bool recognition_more = false;
    bool should_frame;

#ifdef BOXRIG_TERM_ENABLE_BACKGROUND_EFFECT
    if (application->background_failed) {
        application->background_failed = false;
        boxrig_term_gtk4_fail(application);
        return G_SOURCE_CONTINUE;
    }
#endif
    (void)callback;
    (void)user_data;
    g_source_set_ready_time(base, -1);
    if (application->failed) {
        return G_SOURCE_REMOVE;
    }
    if ((native_ready & G_IO_NVAL) != 0 ||
        (native_ready & G_IO_ERR) != 0) {
        boxrig_term_gtk4_fail(application);
        return G_SOURCE_REMOVE;
    }
    if ((native_ready & (G_IO_IN | G_IO_HUP)) != 0) {
        ready |= BOXRIG_SESSION_READY_READ;
    }
    if ((native_ready & G_IO_OUT) != 0) {
        ready |= BOXRIG_SESSION_READY_WRITE;
    }
    if (boxrig_session_dispatch(
            application->session,
            ready,
            &session_result) != BOXRIG_SESSION_OK ||
        boxrig_term_gtk4_process_host_items(
            application,
            &application->host_items_pending) == -1 ||
        boxrig_term_gtk4_native_host_process_layout_action(application) ==
            -1 ||
        boxrig_ui_model_synchronize(application->model) != BOXRIG_UI_OK ||
        boxrig_term_font_demo_apply(&application->font_demo) == -1 ||
        boxrig_term_scrollbar_demo_apply(&application->scrollbar_demo) == -1 ||
        boxrig_term_text_policy_resume(
            application->text_policy,
            BOXRIG_TERM_TEXT_POLICY_WORK_BUDGET_DEFAULT,
            &recognition_work,
            &recognition_more) == -1) {
        boxrig_term_gtk4_fail(application);
        return G_SOURCE_REMOVE;
    }
    application->frame_requested =
        application->frame_requested || recognition_more;
    application->peer_closed =
        application->peer_closed || session_result.peer_closed;
    should_frame = widget_frame_ready(application) &&
                   (application->frame_requested ||
                    !boxrig_gtk4_terminal_widget_has_presentation(
                        application->widget));
    if (should_frame) {
        application->frame_requested = false;
        if (boxrig_gtk4_terminal_widget_frame(
                application->widget,
                &frame_result) != BOXRIG_UI_OK) {
            boxrig_term_gtk4_report_render_state(application);
            boxrig_term_gtk4_fail(application);
            return G_SOURCE_REMOVE;
        }
        boxrig_term_gtk4_report_render_state(application);
        if (boxrig_term_gtk4_apply_pending_zoom_shortcuts(application) ==
            -1) {
            boxrig_term_gtk4_fail(application);
            return G_SOURCE_REMOVE;
        }
        /* frame_pending can represent a native tick or configure wait. Only
         * component work that can advance now requests an immediate pass;
         * the native completion callback wakes the source otherwise. */
        if (application->render_policy_pending) {
            application->render_policy_pending = false;
            boxrig_ui_status status = boxrig_gtk4_terminal_widget_set_render_policy(
                application->widget, application->pending_render_policy);
            boxrig_term_gtk4_report_render_state(application);
            if (status != BOXRIG_UI_OK && status != BOXRIG_UI_UNAVAILABLE) {
                boxrig_term_gtk4_fail(application);
                return G_SOURCE_REMOVE;
            }
        }
        if (frame_result.additional_dispatch) {
            application->frame_requested = true;
        }
    }
    /* Hover feedback may request another frame after every text revision.
     * Keep it at idle priority so continuous output still yields to GTK's
     * allocation and paint; only existing component work needs priority 0. */
    bool immediate_frame_requested = application->frame_requested;
    int pointer_changed = boxrig_term_gtk4_update_pointer(application);
    if (pointer_changed < 0) { boxrig_term_gtk4_fail(application); return G_SOURCE_REMOVE; }
    if (pointer_changed != 0) application->frame_requested = true;
    boxrig_term_gtk4_finish_if_ready(application);
    if (application->quitting) {
        return G_SOURCE_CONTINUE;
    }
    /* Finish native layout transactions before GTK reallocates the widget.
     * A grid resize waiting for GTK's tick cannot advance here: continuous
     * PTY readiness must yield to that tick as well as to paint. */
    g_source_set_priority(base,
        immediate_frame_requested && widget_frame_ready(application) &&
                !boxrig_terminal_synchronized_output(application->terminal)
            ? G_PRIORITY_DEFAULT : G_PRIORITY_DEFAULT_IDLE);
    if (boxrig_term_gtk4_dispatch_needs_immediate_wake(
            application->host_items_pending,
            widget_frame_ready(application),
            application->frame_requested)) {
        g_source_set_ready_time(base, 0);
    }
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs dispatch_source_functions = {
    dispatch_source_prepare,
    dispatch_source_check,
    dispatch_source_dispatch,
    NULL,
    NULL,
    NULL};

static void child_exited(
    GPid child,
    gint child_status,
    gpointer user_data)
{
    boxrig_term_gtk4_application *application = user_data;
    int child_result = 1;

    (void)child;
    if (application->child_completed) {
        boxrig_term_gtk4_fail(application);
        return;
    }
    if (boxrig_term_posix_runtime_record_child_status(
            application->runtime,
            child_status,
            application->frontend_closed,
            &child_result) == -1) {
        application->child_completed = true;
        boxrig_term_gtk4_fail(application);
        boxrig_term_gtk4_finish_if_ready(application);
        return;
    }
    application->child_completed = true;
    if (!application->failed) {
        application->result = child_result;
    }
    boxrig_term_gtk4_request_dispatch(application);
    boxrig_term_gtk4_finish_if_ready(application);
}

void boxrig_term_gtk4_request_dispatch(void *context)
{
    boxrig_term_gtk4_application *application = context;

    if (application == NULL || application->failed) {
        return;
    }
    application->frame_requested = true;
    if (application->dispatch_source != NULL &&
        !g_source_is_destroyed(application->dispatch_source)) {
        g_source_set_priority(application->dispatch_source, G_PRIORITY_DEFAULT);
        g_source_set_ready_time(application->dispatch_source, 0);
    }
}

static void notification_failed(void *context)
{
    boxrig_term_gtk4_fail(context);
}

int boxrig_term_gtk4_create_event_sources(
    boxrig_term_gtk4_application *application)
{
    boxrig_term_gtk4_dispatch_source *dispatch;

    if (application == NULL || application->session == NULL ||
        application->pty == NULL || application->dispatch_source != NULL ||
        application->child_source != NULL) {
        return -1;
    }
    if (boxrig_freedesktop_notifications_create(
            application->host_consumer,
            &application->config->host.notification,
            &application->notifications) == -1) {
        return -1;
    }
    application->notification_source =
        boxrig_freedesktop_notifications_source_new(
            application->notifications, notification_failed, application);
    if (application->notification_source == NULL ||
        g_source_attach(application->notification_source, NULL) == 0) {
        return -1;
    }
    application->child_source = g_child_watch_source_new(
        boxrig_posix_pty_child(application->pty));
    if (application->child_source == NULL) {
        return -1;
    }
    g_source_set_callback(
        application->child_source,
        G_SOURCE_FUNC(child_exited),
        application,
        NULL);
    g_source_set_name(application->child_source, "boxrig child process");
    if (g_source_attach(application->child_source, NULL) == 0) {
        return -1;
    }
    application->dispatch_source = g_source_new(
        &dispatch_source_functions,
        sizeof(*dispatch));
    if (application->dispatch_source == NULL) {
        return -1;
    }
    dispatch =
        (boxrig_term_gtk4_dispatch_source *)application->dispatch_source;
    dispatch->application = application;
    dispatch->descriptor_events =
        G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL;
    dispatch->descriptor_tag = g_source_add_unix_fd(
        application->dispatch_source,
        boxrig_posix_pty_descriptor(application->pty),
        dispatch->descriptor_events);
    g_source_set_name(application->dispatch_source, "boxrig terminal dispatch");
    /* A continuously readable PTY must yield to GTK layout, paint and native
     * event delivery; otherwise prepared terminal images never reach GTK. */
    g_source_set_priority(application->dispatch_source, G_PRIORITY_DEFAULT_IDLE);
    if (g_source_attach(application->dispatch_source, NULL) == 0) {
        return -1;
    }
    return 0;
}
