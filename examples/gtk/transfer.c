/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/posix.h"
#ifdef BOXRIG_TRANSFER_GTK4
#include "boxrig/gtk4.h"
typedef BoxrigGtk4TerminalWidget transfer_widget;
typedef boxrig_gtk4_terminal_widget_config transfer_widget_config;
#define transfer_config_default boxrig_gtk4_terminal_widget_config_default
#define transfer_create boxrig_gtk4_terminal_widget_create
#define transfer_frame boxrig_gtk4_terminal_widget_frame
#define transfer_view boxrig_gtk4_terminal_widget_view
#define transfer_take_layout boxrig_gtk4_terminal_widget_take_layout_action
#define transfer_complete_grid boxrig_gtk4_terminal_widget_complete_grid_resize
#define transfer_complete_size boxrig_gtk4_terminal_widget_complete_size_command
#define transfer_set_key_router boxrig_gtk4_terminal_widget_set_key_router
#define transfer_has_presentation boxrig_gtk4_terminal_widget_has_presentation
#define transfer_get_render_state boxrig_gtk4_terminal_widget_get_render_state
#define transfer_hit_test boxrig_gtk4_terminal_widget_hit_test
#define transfer_get_font_policy boxrig_gtk4_terminal_widget_get_font_rendering_policy
#define transfer_set_font_policy boxrig_gtk4_terminal_widget_set_font_rendering_policy
#define transfer_get_metrics boxrig_gtk4_terminal_widget_get_effective_metrics
#define transfer_widget_width gtk_widget_get_width
#define transfer_widget_height gtk_widget_get_height
#else
#include "boxrig/gtk3.h"
typedef BoxrigGtk3TerminalWidget transfer_widget;
typedef boxrig_gtk3_terminal_widget_config transfer_widget_config;
#define transfer_config_default boxrig_gtk3_terminal_widget_config_default
#define transfer_create boxrig_gtk3_terminal_widget_create
#define transfer_frame boxrig_gtk3_terminal_widget_frame
#define transfer_view boxrig_gtk3_terminal_widget_view
#define transfer_take_layout boxrig_gtk3_terminal_widget_take_layout_action
#define transfer_complete_grid boxrig_gtk3_terminal_widget_complete_grid_resize
#define transfer_complete_size boxrig_gtk3_terminal_widget_complete_size_command
#define transfer_set_key_router boxrig_gtk3_terminal_widget_set_key_router
#define transfer_has_presentation boxrig_gtk3_terminal_widget_has_presentation
#define transfer_get_render_state boxrig_gtk3_terminal_widget_get_render_state
#define transfer_hit_test boxrig_gtk3_terminal_widget_hit_test
#define transfer_get_font_policy boxrig_gtk3_terminal_widget_get_font_rendering_policy
#define transfer_set_font_policy boxrig_gtk3_terminal_widget_set_font_rendering_policy
#define transfer_get_metrics boxrig_gtk3_terminal_widget_get_effective_metrics
#define transfer_widget_width gtk_widget_get_allocated_width
#define transfer_widget_height gtk_widget_get_allocated_height
#endif

#include <glib-unix.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct transfer_app transfer_app;

typedef struct transfer_source {
    GSource source;
    gpointer descriptor_tag;
    GIOCondition descriptor_events;
    transfer_app *app;
} transfer_source;

struct transfer_app {
    GtkWindow *windows[2];
    transfer_widget *widget;
    boxrig_ui_view *view;
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_posix_pty *pty;
    boxrig_session *session;
    boxrig_font_rendering_policy font_policy;
    boxrig_ui_selection selection;
    GMainLoop *loop;
    GSource *source;
    unsigned int parent_index;
    unsigned int transfers;
    unsigned int title_stage;
    bool frame_requested;
    bool transfer_requested;
    bool verify;
    bool gl;
    bool failed;
    bool peer_closed;
    bool selection_prepared;
};

static void request_dispatch(transfer_app *app)
{
    if (app->source != NULL) g_source_set_ready_time(app->source, 0);
}

static void frame_requested(void *context)
{
    transfer_app *app = context;
    app->frame_requested = true;
    request_dispatch(app);
}

static boxrig_input_key_route route_key(const boxrig_key_event *event,
    void *context)
{
    transfer_app *app = context;
    if (event->key != BOXRIG_KEY_F6) return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    if (event->action == BOXRIG_KEY_RELEASE) {
        app->transfer_requested = true;
        request_dispatch(app);
    }
    return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
}

static bool text_contains(boxrig_terminal *terminal, const char *text)
{
    boxrig_text_snapshot_options options = {0};
    boxrig_text_snapshot *snapshot = NULL;
    size_t length = 0;
    size_t target = strlen(text);
    bool found = false;
    options.screen = BOXRIG_TEXT_SCREEN_PRIMARY;
    options.byte_capacity = 1024u * 1024u;
    options.line_capacity = 4096;
    if (boxrig_terminal_create_text_snapshot(terminal, &options,
            &snapshot) != BOXRIG_STATUS_OK) return false;
    const uint8_t *data = boxrig_text_snapshot_data(snapshot, &length);
    for (size_t i = 0; data != NULL && i + target <= length; i++) {
        if (memcmp(data + i, text, target) == 0) {
            found = true;
            break;
        }
    }
    boxrig_text_snapshot_destroy(snapshot);
    return found;
}

static bool visible_text_row(boxrig_terminal *terminal, const char *text,
    size_t *row_out)
{
    size_t rows, columns;
    size_t length = strlen(text);
    boxrig_terminal_size(terminal, &rows, &columns);
    for (size_t row = 0; row < rows; row++) {
        for (size_t column = 0; column + length <= columns; column++) {
            bool match = true;
            for (size_t offset = 0; offset < length; offset++) {
                boxrig_cell_view cell;
                if (boxrig_terminal_get_cell(terminal, row,
                        column + offset, &cell) != BOXRIG_STATUS_OK ||
                    cell.text_length != 1 || cell.text[0] != text[offset]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                *row_out = row;
                return true;
            }
        }
    }
    return false;
}

static void parent_set_child(GtkWindow *window, GtkWidget *child)
{
#ifdef BOXRIG_TRANSFER_GTK4
    gtk_window_set_child(window, child);
#else
    if (child != NULL) gtk_container_add(GTK_CONTAINER(window), child);
    else {
        GtkWidget *old = gtk_bin_get_child(GTK_BIN(window));
        if (old != NULL) gtk_container_remove(GTK_CONTAINER(window), old);
    }
#endif
}

static void parent_destroy(GtkWindow *window)
{
    if (window == NULL) return;
#ifdef BOXRIG_TRANSFER_GTK4
    gtk_window_destroy(window);
#else
    gtk_widget_destroy(GTK_WIDGET(window));
#endif
}

static bool transfer(transfer_app *app)
{
    unsigned int destination = 1u - app->parent_index;
    GtkWidget *native = GTK_WIDGET(app->widget);
    GtkWindow *old = app->windows[app->parent_index];
    GtkWindow *next = app->windows[destination];
    if (next == NULL || gtk_widget_get_parent(native) != GTK_WIDGET(old))
        return false;
    parent_set_child(old, NULL);
    if (gtk_widget_get_parent(native) != NULL) return false;
    parent_set_child(next, native);
    if (gtk_widget_get_parent(native) != GTK_WIDGET(next) ||
        transfer_view(app->widget) != app->view) return false;
    if (app->selection_prepared) {
        boxrig_ui_selection selection = {0};
        boxrig_font_rendering_policy policy = {0};
        if (!boxrig_ui_view_get_selection(app->view, &selection) ||
            !selection.range_valid ||
            selection.anchor.line_id.epoch != app->selection.anchor.line_id.epoch ||
            selection.anchor.line_id.serial != app->selection.anchor.line_id.serial ||
            transfer_get_font_policy(app->widget, &policy) != BOXRIG_UI_OK ||
            policy.color_glyphs != app->font_policy.color_glyphs)
            return false;
    }
#ifdef BOXRIG_TRANSFER_GTK4
    gtk_widget_set_visible(native, true);
#else
    gtk_widget_show(native);
#endif
    gtk_window_present(next);
    (void)gtk_widget_grab_focus(native);
    app->parent_index = destination;
    app->transfers++;
    app->transfer_requested = false;
    app->frame_requested = true;
    printf("TRANSFER %u\n", app->parent_index);
    if (app->verify && app->transfers == 2) {
        parent_destroy(app->windows[1]);
        app->windows[1] = NULL;
    }
    return true;
}

static bool service_layout(transfer_app *app)
{
    boxrig_ui_layout_action action;
    while (transfer_take_layout(app->widget, &action)) {
        if (action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
            if (boxrig_session_resize(app->session,
                    action.value.grid_resize.rows,
                    action.value.grid_resize.columns) != BOXRIG_SESSION_OK ||
                transfer_complete_grid(app->widget, action.revision, true) !=
                    BOXRIG_UI_OK) return false;
        } else if (action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE) {
            boxrig_ui_layout_result layout;
            bool applied;
            int width = transfer_widget_width(GTK_WIDGET(app->widget));
            int height = transfer_widget_height(GTK_WIDGET(app->widget));
            if (width <= 0 || height <= 0 ||
                transfer_complete_size(app->widget, action.revision,
                    BOXRIG_UI_SIZE_ACCEPTED,
                    (boxrig_ui_fixed)width * BOXRIG_UI_FIXED_ONE,
                    (boxrig_ui_fixed)height * BOXRIG_UI_FIXED_ONE,
                    &applied, &layout) != BOXRIG_UI_OK) return false;
        }
    }
    return true;
}

static bool service(transfer_app *app, uint32_t ready)
{
    boxrig_session_dispatch_result dispatch;
    boxrig_ui_component_frame_result frame = {0};
    boxrig_host_item item;
    if (boxrig_session_dispatch(app->session, ready, &dispatch) !=
            BOXRIG_SESSION_OK ||
        boxrig_ui_model_synchronize(app->model) != BOXRIG_UI_OK ||
        !service_layout(app)) return false;
    if (dispatch.peer_closed) app->peer_closed = true;
    if (gtk_widget_get_mapped(GTK_WIDGET(app->widget)) &&
        transfer_widget_width(GTK_WIDGET(app->widget)) > 0 &&
        transfer_widget_height(GTK_WIDGET(app->widget)) > 0) {
        app->frame_requested = false;
        if (transfer_frame(app->widget, &frame) != BOXRIG_UI_OK ||
            !service_layout(app)) return false;
        if (frame.additional_dispatch) app->frame_requested = true;
    }
    while (boxrig_terminal_host_item_peek(app->terminal, &item)) {
        if (item.type == BOXRIG_HOST_ITEM_TITLE) {
            if (app->title_stage == 0 && item.payload_length == 14 &&
                memcmp(item.payload, "transfer-ready", 14) == 0)
                app->title_stage = 1;
            else if (app->title_stage == 1 && item.payload_length == 12 &&
                memcmp(item.payload, "transfer-ack", 12) == 0)
                app->title_stage = 2;
            else if (app->title_stage == 2 && item.payload_length == 17 &&
                memcmp(item.payload, "transfer-verified", 17) == 0)
                app->title_stage = 3;
        }
        if (boxrig_terminal_host_item_consume(app->terminal) != BOXRIG_STATUS_OK)
            return false;
    }
    if (app->verify) {
        if (app->peer_closed && app->title_stage != 3) return false;
        if (app->title_stage == 1 && app->transfers == 0 &&
            transfer_has_presentation(app->widget) &&
            text_contains(app->terminal, "before-transfer")) {
            boxrig_ui_hit_test_result hit;
            boxrig_font_effective_metrics metrics;
            size_t text_row;
            if (!visible_text_row(app->terminal, "before-transfer", &text_row) ||
                transfer_get_metrics(app->widget, &metrics) != BOXRIG_FONT_CATALOG_OK)
                return false;
            if (transfer_hit_test(app->widget, BOXRIG_UI_FIXED_ONE,
                    ((boxrig_ui_fixed)text_row * metrics.cell_height + 1) *
                        BOXRIG_UI_FIXED_ONE, &hit) != BOXRIG_UI_OK ||
                !hit.text_position_valid ||
                boxrig_ui_view_begin_selection(app->view,
                    BOXRIG_UI_SELECTION_WORD, &hit) != BOXRIG_UI_OK ||
                !boxrig_ui_view_get_selection(app->view, &app->selection) ||
                !app->selection.range_valid) return false;
            app->selection_prepared = true;
            if (!transfer(app) ||
                boxrig_terminal_send_text(app->terminal, "first\n", 6) !=
                    BOXRIG_STATUS_OK) return false;
        } else if (app->title_stage == 2 && app->transfers == 1 &&
            transfer_has_presentation(app->widget) &&
            text_contains(app->terminal, "after-first-transfer")) {
            if (!transfer(app) ||
                boxrig_terminal_send_text(app->terminal, "second\n", 7) !=
                    BOXRIG_STATUS_OK) return false;
        } else if (app->title_stage == 3 && app->transfers == 2 &&
            text_contains(app->terminal, "after-second-transfer")) {
            boxrig_gtk_render_state render;
            if (transfer_get_render_state(app->widget, &render) != BOXRIG_UI_OK)
                return false;
            if (render.pending) return true;
            if (render.active_backend != (app->gl ?
                    BOXRIG_GTK_RENDER_BACKEND_OPENGL :
                    BOXRIG_GTK_RENDER_BACKEND_CAIRO)) return false;
            boxrig_ui_selection selection;
            boxrig_font_rendering_policy policy;
            if (!boxrig_ui_view_get_selection(app->view, &selection) ||
                !selection.range_valid ||
                selection.anchor.line_id.epoch != app->selection.anchor.line_id.epoch ||
                selection.anchor.line_id.serial != app->selection.anchor.line_id.serial ||
                transfer_get_font_policy(app->widget, &policy) != BOXRIG_UI_OK ||
                policy.color_glyphs != app->font_policy.color_glyphs)
                return false;
            puts("PASS GTK transfer retained widget, view, renderer and PTY");
            g_main_loop_quit(app->loop);
        }
    } else if (app->transfer_requested) {
        if (!transfer(app)) return false;
    }
    if (app->peer_closed && !app->verify) g_main_loop_quit(app->loop);
    return true;
}

static void update_descriptor(transfer_source *source)
{
    uint32_t interests = boxrig_session_interests(source->app->session);
    GIOCondition events = G_IO_HUP | G_IO_ERR | G_IO_NVAL;
    if (interests & BOXRIG_SESSION_INTEREST_READ) events |= G_IO_IN;
    if (interests & BOXRIG_SESSION_INTEREST_WRITE) events |= G_IO_OUT;
    if (events != source->descriptor_events) {
        g_source_modify_unix_fd((GSource *)source, source->descriptor_tag, events);
        source->descriptor_events = events;
    }
}

static gboolean source_prepare(GSource *base, gint *timeout)
{
    transfer_source *source = (transfer_source *)base;
    transfer_app *app = source->app;
    update_descriptor(source);
    *timeout = -1;
    uint32_t interests = boxrig_session_interests(app->session);
    return app->transfer_requested ||
        (app->frame_requested && gtk_widget_get_mapped(GTK_WIDGET(app->widget))) ||
        ((interests & BOXRIG_SESSION_INTEREST_DISPATCH) != 0 &&
         (interests & BOXRIG_SESSION_INTEREST_WRITE) == 0);
}

static gboolean source_check(GSource *base)
{
    transfer_source *source = (transfer_source *)base;
    return g_source_query_unix_fd(base, source->descriptor_tag) != 0;
}

static gboolean source_dispatch(GSource *base, GSourceFunc callback,
    gpointer context)
{
    transfer_source *source = (transfer_source *)base;
    GIOCondition native = g_source_query_unix_fd(base, source->descriptor_tag);
    uint32_t ready = BOXRIG_SESSION_READY_NONE;
    (void)callback;
    (void)context;
    g_source_set_ready_time(base, -1);
    if (native & (G_IO_ERR | G_IO_NVAL)) goto failure;
    if (native & (G_IO_IN | G_IO_HUP)) ready |= BOXRIG_SESSION_READY_READ;
    if (native & G_IO_OUT) ready |= BOXRIG_SESSION_READY_WRITE;
    if (!service(source->app, ready)) goto failure;
    return G_SOURCE_CONTINUE;
failure:
    source->app->failed = true;
    g_main_loop_quit(source->app->loop);
    return G_SOURCE_REMOVE;
}

static GSourceFuncs source_functions = {
    source_prepare, source_check, source_dispatch, NULL, NULL, NULL};

static gboolean timed_out(gpointer context)
{
    transfer_app *app = context;
    fprintf(stderr, "GTK transfer timeout: stage=%u moves=%u\n",
        app->title_stage, app->transfers);
    app->failed = true;
    g_main_loop_quit(app->loop);
    return G_SOURCE_REMOVE;
}

static void window_destroyed(GtkWidget *window, gpointer context)
{
    transfer_app *app = context;
    (void)window;
    if (!app->verify) g_main_loop_quit(app->loop);
}

int main(int argc, char **argv)
{
    transfer_app app = {0};
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_session_config session_config = boxrig_session_config_default();
    transfer_widget_config config = transfer_config_default();
    char *verify_args[] = {"/bin/sh", "-c",
        "printf 'before-transfer\\r\\n\\033]0;transfer-ready\\007'; "
        "read value; [ \"$value\" = first ] || exit 2; "
        "printf 'after-first-transfer\\r\\n\\033]0;transfer-ack\\007'; "
        "read value; [ \"$value\" = second ] || exit 3; "
        "printf 'after-second-transfer\\r\\n\\033]0;transfer-verified\\007'; "
        "exec cat", NULL};
    char *interactive_args[] = {"/bin/sh", NULL};
    int result = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verify") == 0) app.verify = true;
        else if (strcmp(argv[i], "--gl") == 0) app.gl = true;
        else return 2;
    }
#ifdef BOXRIG_TRANSFER_GTK4
    if (!gtk_init_check()) return 1;
#else
    if (!gtk_init_check(&argc, &argv)) return 1;
#endif
    app.loop = g_main_loop_new(NULL, false);
    if (app.loop == NULL) goto done;
    terminal_config.rows = 24;
    terminal_config.columns = 80;
    if (boxrig_terminal_create(&terminal_config, &app.terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(app.terminal, &app.model) != BOXRIG_UI_OK ||
        boxrig_posix_pty_spawn(app.verify ? verify_args : interactive_args,
            "xterm-256color", 24, 80, &app.pty) != 0 ||
        boxrig_posix_pty_session_create(app.terminal, app.pty,
            &session_config, &app.session) != BOXRIG_SESSION_OK) goto done;
    config.frame_request_callback = frame_requested;
    config.frame_request_context = &app;
    config.render_policy = app.gl ? BOXRIG_GTK_RENDER_OPENGL_REQUIRED :
        BOXRIG_GTK_RENDER_CAIRO;
    config.view.blink.text_enabled = false;
    config.view.blink.cursor_enabled = false;
    if (transfer_create(app.model, &config, &app.widget) != BOXRIG_UI_OK ||
        transfer_set_key_router(app.widget, route_key, &app) != BOXRIG_UI_OK)
        goto done;
    g_object_ref_sink(app.widget);
    app.view = transfer_view(app.widget);
    if (transfer_get_font_policy(app.widget, &app.font_policy) != BOXRIG_UI_OK)
        goto done;
    app.font_policy.color_glyphs = !app.font_policy.color_glyphs;
    if (transfer_set_font_policy(app.widget, &app.font_policy) != BOXRIG_UI_OK)
        goto done;
    for (unsigned int i = 0; i < 2; i++) {
#ifdef BOXRIG_TRANSFER_GTK4
        app.windows[i] = GTK_WINDOW(gtk_window_new());
#else
        app.windows[i] = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
#endif
        if (app.windows[i] == NULL) goto done;
        gtk_window_set_title(app.windows[i], i == 0 ?
            "boxrig GTK dock (F6)" : "boxrig GTK undock (F6)");
        gtk_window_set_default_size(app.windows[i], 640, 480);
        g_signal_connect(app.windows[i], "destroy", G_CALLBACK(window_destroyed), &app);
    }
    parent_set_child(app.windows[0], GTK_WIDGET(app.widget));
#ifdef BOXRIG_TRANSFER_GTK4
    gtk_widget_set_visible(GTK_WIDGET(app.widget), true);
#else
    gtk_widget_show_all(GTK_WIDGET(app.windows[0]));
    gtk_widget_show(GTK_WIDGET(app.windows[1]));
#endif
    gtk_window_present(app.windows[0]);
    gtk_window_present(app.windows[1]);
    (void)gtk_widget_grab_focus(GTK_WIDGET(app.widget));
    app.source = g_source_new(&source_functions, sizeof(transfer_source));
    if (app.source == NULL) goto done;
    transfer_source *source = (transfer_source *)app.source;
    source->app = &app;
    source->descriptor_events = G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL;
    source->descriptor_tag = g_source_add_unix_fd(app.source,
        boxrig_posix_pty_descriptor(app.pty), source->descriptor_events);
    g_source_set_priority(app.source, G_PRIORITY_DEFAULT_IDLE);
    if (g_source_attach(app.source, NULL) == 0) goto done;
    app.frame_requested = true;
    request_dispatch(&app);
    if (app.verify) g_timeout_add_seconds(60, timed_out, &app);
    g_main_loop_run(app.loop);
    result = app.failed ? 1 : 0;
done:
    if (result != 0) fprintf(stderr, "GTK transfer failed at stage=%u moves=%u\n",
        app.title_stage, app.transfers);
    if (app.source != NULL) {
        g_source_destroy(app.source);
        g_source_unref(app.source);
        app.source = NULL;
    }
    boxrig_session_destroy(app.session);
    boxrig_posix_pty_destroy(app.pty);
    for (unsigned int i = 0; i < 2; i++) parent_destroy(app.windows[i]);
    if (app.widget != NULL) g_object_unref(app.widget);
    boxrig_ui_model_destroy(app.model);
    boxrig_terminal_destroy(app.terminal);
    if (app.loop != NULL) g_main_loop_unref(app.loop);
    return result;
}
