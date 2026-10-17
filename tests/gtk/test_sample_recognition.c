/* boxrig - Under MIT. */
#include <gtk/gtk.h>
#if GTK_MAJOR_VERSION == 3
#include "../../examples/boxrig_term/gtk3/event_source.c"
typedef boxrig_term_gtk3_application application;
typedef boxrig_term_gtk3_dispatch_source dispatch_source;
typedef boxrig_gtk3_terminal_widget_config widget_config;
#define APP(name) boxrig_term_gtk3_##name
#define WIDGET(name) boxrig_gtk3_terminal_widget_##name
#else
#include "../../examples/boxrig_term/gtk4/event_source.c"
typedef boxrig_term_gtk4_application application;
typedef boxrig_term_gtk4_dispatch_source dispatch_source;
typedef boxrig_gtk4_terminal_widget_config widget_config;
#define APP(name) boxrig_term_gtk4_##name
#define WIDGET(name) boxrig_gtk4_terminal_widget_##name
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL GTK%d %s:%d: %s\n", GTK_MAJOR_VERSION, \
        __FILE__, __LINE__, #condition); abort(); } } while (0)

static size_t resumes;
static size_t frames;
static size_t host_passes;
static bool last_more;

int __real_boxrig_term_text_policy_resume(boxrig_term_text_policy *policy,
    size_t budget, size_t *used, bool *more);
int __wrap_boxrig_term_text_policy_resume(boxrig_term_text_policy *policy,
    size_t budget, size_t *used, bool *more)
{
    int result = __real_boxrig_term_text_policy_resume(policy, budget, used, more);
    CHECK(result == 0 && *used <= budget);
    resumes++;
    last_more = *more;
    return result;
}

/* Native window commands are outside this fixture. The session, recognizer,
 * view, widget and production GSource dispatch all execute normally. */
int APP(process_host_items)(application *app, bool *pending)
{
    (void)app;
    host_passes++;
    *pending = false;
    return 0;
}

int APP(native_host_process_layout_action)(application *app)
{
    boxrig_ui_layout_action action;
    if (WIDGET(take_layout_action)(app->widget, &action)) {
        CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(boxrig_terminal_resize(app->terminal, action.value.grid_resize.rows,
            action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(WIDGET(complete_grid_resize)(app->widget, action.revision, true) == BOXRIG_UI_OK);
    }
    return 0;
}

void APP(report_render_state)(application *app) { (void)app; frames++; }
int APP(apply_pending_zoom_shortcuts)(application *app) { (void)app; return 0; }
int APP(update_pointer)(application *app) { (void)app; return 0; }
void APP(finish_if_ready)(application *app) { (void)app; }
void APP(fail)(application *app) { app->failed = true; }

static boxrig_transport_status read_input(void *context, uint8_t *bytes,
    size_t capacity, size_t *length)
{
    (void)context; (void)bytes; (void)capacity;
    *length = 0;
    return BOXRIG_TRANSPORT_WOULD_BLOCK;
}

static boxrig_transport_status write_output(void *context, const uint8_t *bytes,
    size_t length, size_t *written)
{
    (void)context; (void)bytes;
    *written = length;
    return BOXRIG_TRANSPORT_OK;
}

static boxrig_transport_status resize_transport(void *context, size_t rows, size_t columns)
{
    (void)context; (void)rows; (void)columns;
    return BOXRIG_TRANSPORT_OK;
}

static void feed(application *app, const char *text)
{
    size_t consumed;
    CHECK(boxrig_terminal_feed(app->terminal, (const uint8_t *)text,
        strlen(text), &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == strlen(text));
}

static void drain(application *app, GMainContext *context)
{
    /* This context contains only the real application source. Its transition
     * to not-ready is observable, without relying on an idle-time timeout. */
    size_t iterations = 0;
    while (g_main_context_pending(context)) {
        CHECK(++iterations < 100000);
        CHECK(g_main_context_iteration(context, false));
        CHECK(!app->failed);
    }
    CHECK(g_source_get_ready_time(app->dispatch_source) == -1);
}

static gboolean expired(gpointer data)
{
    (void)data;
    fprintf(stderr, "GTK sample recognition fixture initialization timed out\n");
    abort();
}

static void run(bool mapped, bool urls)
{
    application app = {0};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 66;
    config.columns = 190;
    config.text_recognizer_event_capacity = 1024;
    CHECK(boxrig_terminal_create(&config, &app.terminal) == BOXRIG_STATUS_OK);
    feed(&app, "\033[?1049h\033[?25l");
    CHECK(boxrig_ui_model_create(app.terminal, &app.model) == BOXRIG_UI_OK);
    boxrig_session_config session_config = boxrig_session_config_default();
    boxrig_session_transport transport = {0};
    transport.read = read_input;
    transport.write = write_output;
    transport.resize = resize_transport;
    CHECK(boxrig_session_create(app.terminal, &transport, &session_config,
        &app.session) == BOXRIG_SESSION_OK);
    widget_config wc = WIDGET(config_default)();
    wc.frame_request_callback = APP(request_dispatch);
    wc.frame_request_context = &app;
    CHECK(WIDGET(create)(app.model, &wc, &app.widget) == BOXRIG_UI_OK);
    g_object_ref_sink(app.widget);
#if GTK_MAJOR_VERSION == 3
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(app.widget));
#else
    GtkWidget *window = gtk_window_new();
    gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(app.widget));
#endif
    g_object_ref_sink(window);
    gtk_widget_set_size_request(GTK_WIDGET(app.widget), 900, 650);
    if (mapped) {
#if GTK_MAJOR_VERSION == 3
        gtk_widget_show_all(window);
#else
        gtk_window_present(GTK_WINDOW(window));
#endif
        CHECK(WIDGET(set_visible)(app.widget, true) == BOXRIG_UI_OK);
        guint timeout = g_timeout_add_seconds(30, expired, NULL);
        while (!gtk_widget_get_mapped(GTK_WIDGET(app.widget)))
            g_main_context_iteration(NULL, true);
        CHECK(g_source_remove(timeout));
        APP(native_host_process_layout_action)(&app);
    }

    CHECK(boxrig_term_text_policy_create(app.terminal, WIDGET(view)(app.widget),
        &app.text_policy) == 0);
    GMainContext *context = g_main_context_new();
    app.dispatch_source = g_source_new(&dispatch_source_functions, sizeof(dispatch_source));
    ((dispatch_source *)app.dispatch_source)->application = &app;
    g_source_set_priority(app.dispatch_source, G_PRIORITY_DEFAULT_IDLE);
    CHECK(g_source_attach(app.dispatch_source, context) != 0);

    if (mapped) {
        guint timeout = g_timeout_add_seconds(30, expired, NULL);
        /* Complete native allocation before isolating the production source
         * from GTK timers. No recognition workload has been fed yet. */
        bool initialized = false;
        while (!initialized) {
            app.frame_requested = false;
            boxrig_ui_layout_result layout;
            if (WIDGET(get_layout)(app.widget, &layout) == BOXRIG_UI_OK) {
                boxrig_ui_component_frame_result frame_result;
                CHECK(WIDGET(frame)(app.widget, &frame_result) == BOXRIG_UI_OK);
                APP(native_host_process_layout_action)(&app);
            }
            g_main_context_iteration(NULL, false);
            CHECK(!app.failed);
            initialized = WIDGET(has_presentation)(app.widget) &&
                !app.frame_requested && !boxrig_ui_view_frame_requested(WIDGET(view)(app.widget));
#if GTK_MAJOR_VERSION == 4
            initialized = initialized && WIDGET(frame_ready)(app.widget);
#endif
        }
        CHECK(g_source_remove(timeout));
        app.frame_requested = false;
        g_source_set_ready_time(app.dispatch_source, -1);
    }

    /* Enough text to require many budgets before the final URL. Alternate
     * screen avoids a scrollbar resize that would require another GTK tick;
     * blinking cannot accidentally resume the isolated recognition source. */
    for (size_t index = 0; index < 250; index++)
        feed(&app, "unchanged row without a link 0123456789012345678901234567890123456789\r\n");
    feed(&app, urls ? "https://example.org/final" : "finite output complete");
    resumes = frames = host_passes = 0;
    last_more = false;
    APP(request_dispatch)(&app);
    drain(&app, context);
    fprintf(stderr, "GTK%d mapped=%d urls=%d resumes=%zu frames=%zu host=%zu more=%d\n",
        GTK_MAJOR_VERSION, mapped, urls, resumes, frames, host_passes, last_more);
    CHECK(resumes > 1);
    CHECK(!last_more);
    boxrig_text_recognizer_event event;
    CHECK(!boxrig_terminal_text_recognizer_event_peek(app.terminal, &event));
    CHECK(frames < resumes);
    if (!mapped) CHECK(frames == 0);

    boxrig_ui_view *view = WIDGET(view)(app.widget);
    size_t rows, columns, annotation_count = 0;
    boxrig_terminal_size(app.terminal, &rows, &columns);
    (void)columns;
    for (size_t row = 0; row < rows; row++) {
        size_t count = 0;
        boxrig_ui_status status = boxrig_ui_view_query_text_annotation_visual_ranges(view,
            row, NULL, 0, &count);
        CHECK(status == BOXRIG_UI_OK || status == BOXRIG_UI_LIMIT_EXCEEDED);
        annotation_count += count;
    }
    CHECK(annotation_count == (urls ? 1u : 0u));
    CHECK(!g_main_context_pending(context));

    g_source_destroy(app.dispatch_source);
    g_source_unref(app.dispatch_source);
    app.dispatch_source = NULL;
    g_main_context_unref(context);
    boxrig_term_text_policy_destroy(app.text_policy);
    CHECK(WIDGET(quiesce)(app.widget) == BOXRIG_UI_OK);
#if GTK_MAJOR_VERSION == 3
    gtk_widget_destroy(window);
#else
    gtk_window_destroy(GTK_WINDOW(window));
#endif
    g_object_unref(window);
    g_object_unref(app.widget);
    boxrig_session_destroy(app.session);
    boxrig_ui_model_destroy(app.model);
    boxrig_terminal_destroy(app.terminal);
}

int main(void)
{
#if GTK_MAJOR_VERSION == 3
    CHECK(gtk_init_check(NULL, NULL));
#else
    CHECK(gtk_init_check());
#endif
    run(true, true);
    run(true, false);
    run(false, true);
    printf("PASS GTK%d finite sample recognition drains without extra input\n", GTK_MAJOR_VERSION);
    return 0;
}
