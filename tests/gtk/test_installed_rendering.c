/* boxrig - Under MIT. */
#include <gtk/gtk.h>
#if GTK_MAJOR_VERSION == 3
#include "boxrig/gtk3.h"
typedef BoxrigGtk3TerminalWidget terminal_widget;
typedef boxrig_gtk3_terminal_widget_config widget_config;
#define API(name) boxrig_gtk3_terminal_widget_##name
#else
#include "boxrig/gtk4.h"
typedef BoxrigGtk4TerminalWidget terminal_widget;
typedef boxrig_gtk4_terminal_widget_config widget_config;
#define API(name) boxrig_gtk4_terminal_widget_##name
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL GTK %d installed rendering line %d: %s\n", GTK_MAJOR_VERSION, __LINE__, #condition); \
    abort(); } } while (0)

static unsigned int attempts;
static bool fail_context;

static GdkGLContext *creating_context(GtkGLArea *area, gpointer data)
{
    (void)data;
    attempts++;
    if (fail_context) {
        GError *error = g_error_new_literal(GDK_GL_ERROR, GDK_GL_ERROR_NOT_AVAILABLE,
            "Injected installed-consumer context failure");
        gtk_gl_area_set_error(area, error);
        g_error_free(error);
        g_signal_stop_emission_by_name(area, "create-context");
    }
    return NULL;
}

/** @copydoc gtk_gl_area_new */
GtkWidget *__real_gtk_gl_area_new(void);
/** Observes context creation through GTK's public signal. */
GtkWidget *__wrap_gtk_gl_area_new(void)
{
    GtkWidget *area = __real_gtk_gl_area_new();
    g_signal_connect(area, "create-context", G_CALLBACK(creating_context), NULL);
    return area;
}

typedef struct fixture {
    terminal_widget *widget;
    boxrig_terminal *terminal;
    boxrig_gtk_render_backend expected;
    bool required_failure;
    bool done;
    uint64_t painted_generation;
} fixture;

static void painted(GdkFrameClock *clock, gpointer data)
{
    (void)clock;
    fixture *test = data;
    test->painted_generation = API(presentation_generation)(test->widget);
}

static gboolean frame(gpointer data)
{
    fixture *test = data;
    boxrig_ui_layout_result layout;
    if (API(get_layout)(test->widget, &layout) != BOXRIG_UI_OK) return G_SOURCE_CONTINUE;
    boxrig_ui_component_frame_result result;
    boxrig_ui_status status = API(frame)(test->widget, &result);
    boxrig_gtk_render_state state;
    CHECK(API(get_render_state)(test->widget, &state) == BOXRIG_UI_OK);
    if (status != BOXRIG_UI_OK) {
        CHECK(test->required_failure && status == BOXRIG_UI_UNAVAILABLE);
        CHECK(!state.pending && state.active_backend == BOXRIG_GTK_RENDER_BACKEND_NONE);
        test->done = true;
        return G_SOURCE_REMOVE;
    }
    boxrig_ui_layout_action action;
    while (API(take_layout_action)(test->widget, &action)) {
        CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(boxrig_terminal_resize(test->terminal, action.value.grid_resize.rows,
            action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(API(complete_grid_resize)(test->widget, action.revision, true) == BOXRIG_UI_OK);
    }
    uint64_t generation = API(presentation_generation)(test->widget);
    if (!state.pending && state.active_backend == test->expected && generation > 0 &&
        test->painted_generation == generation && !result.frame_pending &&
        !boxrig_ui_view_frame_requested(API(view)(test->widget))) {
        test->done = true;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean expired(gpointer data)
{
    (void)data;
    CHECK(false && "installed consumer native frame deadline");
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    CHECK(argc == 4);
    bool built = atoi(argv[1]) != 0;
    boxrig_gtk_render_policy policy = (boxrig_gtk_render_policy)atoi(argv[2]);
    fail_context = atoi(argv[3]) != 0;
#if GTK_MAJOR_VERSION == 3
    CHECK(gtk_init_check(NULL, NULL));
#else
    CHECK(gtk_init_check());
#endif
    fixture test = {0};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 10;
    config.columns = 50;
    CHECK(boxrig_terminal_create(&config, &test.terminal) == BOXRIG_STATUS_OK);
    const char scene[] = "\033[?25l\033[48;2;40;80;120m\033[2JInstalled GTK rendering";
    size_t consumed;
    CHECK(boxrig_terminal_feed(test.terminal, (const uint8_t *)scene, sizeof(scene) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(scene) - 1);
    boxrig_ui_model *model = NULL;
    CHECK(boxrig_ui_model_create(test.terminal, &model) == BOXRIG_UI_OK);
    widget_config wc = API(config_default)();
    CHECK(wc.render_policy == BOXRIG_GTK_RENDER_CAIRO);
    wc.render_policy = policy;
    boxrig_gtk_render_state state;
    boxrig_ui_status created = API(create_with_render_state)(model, &wc, &test.widget, &state);
    if (!built && policy == BOXRIG_GTK_RENDER_OPENGL_REQUIRED) {
        CHECK(created == BOXRIG_UI_UNAVAILABLE && test.widget == NULL && !state.pending);
        CHECK(state.reason == BOXRIG_GTK_RENDER_REASON_NOT_BUILT);
    } else {
        CHECK(created == BOXRIG_UI_OK && test.widget != NULL && state.pending);
        test.required_failure = policy == BOXRIG_GTK_RENDER_OPENGL_REQUIRED && fail_context;
        test.expected = policy != BOXRIG_GTK_RENDER_CAIRO && built && !fail_context
            ? BOXRIG_GTK_RENDER_BACKEND_OPENGL : BOXRIG_GTK_RENDER_BACKEND_CAIRO;
#if GTK_MAJOR_VERSION == 3
        GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#else
        GtkWidget *window = gtk_window_new();
#endif
        g_object_ref_sink(window);
        gtk_widget_set_size_request(GTK_WIDGET(test.widget), 480, 160);
#if GTK_MAJOR_VERSION == 3
        gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(test.widget));
        gtk_widget_show_all(window);
#else
        gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(test.widget));
        gtk_window_present(GTK_WINDOW(window));
#endif
        CHECK(API(set_visible)(test.widget, true) == BOXRIG_UI_OK);
        GdkFrameClock *clock = gtk_widget_get_frame_clock(window);
        CHECK(clock != NULL);
        gulong handler = g_signal_connect_after(clock, "after-paint", G_CALLBACK(painted), &test);
        guint timeout = g_timeout_add_seconds(15, expired, &test);
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, frame, &test, NULL);
        while (!test.done) g_main_context_iteration(NULL, TRUE);
        g_source_remove(timeout);
        CHECK(API(get_render_state)(test.widget, &state) == BOXRIG_UI_OK);
        CHECK(state.requested_policy == policy && !state.pending);
        CHECK(state.active_backend == (test.required_failure ? BOXRIG_GTK_RENDER_BACKEND_NONE : test.expected));
        bool unavailable = policy != BOXRIG_GTK_RENDER_CAIRO && (!built || fail_context);
        CHECK(state.status == (unavailable ? BOXRIG_UI_UNAVAILABLE : BOXRIG_UI_OK));
        CHECK(state.reason == (policy == BOXRIG_GTK_RENDER_CAIRO ? BOXRIG_GTK_RENDER_REASON_REQUESTED :
            !built ? BOXRIG_GTK_RENDER_REASON_NOT_BUILT : fail_context ? BOXRIG_GTK_RENDER_REASON_CONTEXT :
            BOXRIG_GTK_RENDER_REASON_REQUESTED));
        if (!built && policy == BOXRIG_GTK_RENDER_CAIRO) {
            for (int selected = BOXRIG_GTK_RENDER_OPENGL_PREFERRED; selected <= BOXRIG_GTK_RENDER_OPENGL_REQUIRED; selected++) {
                CHECK(API(set_render_policy)(test.widget, (boxrig_gtk_render_policy)selected) == BOXRIG_UI_UNAVAILABLE);
                CHECK(API(get_render_state)(test.widget, &state) == BOXRIG_UI_OK);
                CHECK(!state.pending && state.active_backend == BOXRIG_GTK_RENDER_BACKEND_CAIRO &&
                    state.reason == BOXRIG_GTK_RENDER_REASON_NOT_BUILT);
            }
            CHECK(API(set_render_policy)(test.widget, BOXRIG_GTK_RENDER_CAIRO) == BOXRIG_UI_OK);
            CHECK(API(get_render_state)(test.widget, &state) == BOXRIG_UI_OK);
        }
        g_signal_handler_disconnect(clock, handler);
#if GTK_MAJOR_VERSION == 3
        gtk_widget_destroy(window);
#else
        gtk_window_destroy(GTK_WINDOW(window));
#endif
        g_object_unref(window);
    }
    CHECK(attempts == (built && policy != BOXRIG_GTK_RENDER_CAIRO ? 1u : 0u));
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(test.terminal);
    printf("PASS installed GTK %d built=%d policy=%d fault=%d backend=%d reason=%d attempts=%u\n",
        GTK_MAJOR_VERSION, built, policy, fail_context, state.active_backend, state.reason, attempts);
    return 0;
}
