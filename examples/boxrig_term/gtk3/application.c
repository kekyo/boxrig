/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "application_internal.h"
#include "../common/background_demo.h"


#include "event_source_policy.h"
#include "boxrig/gtk3/terminal_widget.h"
#include "../common/runtime_info.h"

#include <cairo/cairo.h>
#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#ifdef BOXRIG_TERM_ENABLE_BACKGROUND_EFFECT
static void update_background_effect(void *context)
{
    boxrig_term_gtk3_application *application = context;
    if (application->background_effect == NULL || application->window == NULL || application->widget == NULL ||
        !gtk_widget_get_mapped(GTK_WIDGET(application->window))) return;
    int x, y;
    if (!gtk_widget_translate_coordinates(GTK_WIDGET(application->widget), GTK_WIDGET(application->window), 0, 0, &x, &y)) return;
    boxrig_ui_background_effect_rectangle region = {x, y,
        gtk_widget_get_allocated_width(GTK_WIDGET(application->widget)),
        gtk_widget_get_allocated_height(GTK_WIDGET(application->widget))};
    if (region.width <= 0 || region.height <= 0) return;
    boxrig_ui_status status = boxrig_gtk3_background_effect_set(application->background_effect,
        application->background_request, &region, 1);
    boxrig_ui_background_effect_state state = boxrig_gtk3_background_effect_get_state(application->background_effect);
    if (!application->background_reported_valid || state.supported != application->background_reported.supported ||
        state.accepted != application->background_reported.accepted || state.reason != application->background_reported.reason) {
        fprintf(stderr, "boxrig_term: background-effect=gtk3 supported=%d accepted=%d reason=%d\n",
            state.supported, state.accepted, state.reason);
        application->background_reported = state; application->background_reported_valid = true;
    }
    if (status != BOXRIG_UI_OK && status != BOXRIG_UI_BUSY) {
        application->background_failed = true;
        boxrig_term_gtk3_request_dispatch(application);
    }
}
#endif

static int initialize_background_effect(boxrig_term_gtk3_application *application)
{
    boxrig_ui_background_effect_request request;
    if (boxrig_term_background_effect_from_environment(&request) == -1) return -1;
    if (request == BOXRIG_UI_BACKGROUND_EFFECT_NONE) return 0;
#ifdef BOXRIG_TERM_ENABLE_BACKGROUND_EFFECT
    application->background_request = request;
    return boxrig_gtk3_background_effect_create(application->window, update_background_effect, application,
        &application->background_effect) == BOXRIG_UI_OK ? 0 : -1;
#else
    (void)application;
    fprintf(stderr, "boxrig_term: background-effect requires the optional GTK3 effect adapter build\n");
    return request == BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE ? -1 : 0;
#endif
}

static boxrig_ui_status set_color_glyphs(void *context, bool enabled)
{
    boxrig_font_rendering_policy policy;
    boxrig_ui_status status = boxrig_gtk3_terminal_widget_get_font_rendering_policy(context, &policy);
    if (status != BOXRIG_UI_OK) return status;
    policy.color_glyphs = enabled;
    return boxrig_gtk3_terminal_widget_set_font_rendering_policy(context, &policy);
}

static boxrig_ui_status set_scrollbar_policy(void *context, boxrig_ui_scrollbar_visibility policy)
{
    return boxrig_gtk3_terminal_widget_set_scrollbar_visibility(context, policy);
}

static const char *window_system(GtkWidget *widget)
{
    GdkDisplay *display = gtk_widget_get_display(widget);
    const char *type_name;

    if (display == NULL) {
        return "unknown";
    }
    /* Preserve the sample's toolkit-only dependency boundary while using the
     * public backend display type to name the selected window system. */
    type_name = G_OBJECT_TYPE_NAME(display);
    if (strstr(type_name, "X11") != NULL) {
        return "x11";
    }
    if (strstr(type_name, "Wayland") != NULL) {
        return "wayland";
    }
    return type_name;
}

static void report_ime_state(boxrig_term_gtk3_application *application)
{
    boxrig_gtk_ime_state state;
    if (boxrig_gtk3_terminal_widget_get_ime_state(application->widget, &state) != BOXRIG_UI_OK) return;
    const boxrig_gtk_ime_state *previous = &application->reported_ime_state;
    if (application->ime_state_reported && previous->active_route == state.active_route &&
        previous->reason == state.reason && previous->ibus_state == state.ibus_state &&
        previous->registry_complete == state.registry_complete &&
        previous->candidate_valid == state.candidate_valid &&
        previous->candidate_owner == state.candidate_owner &&
        previous->candidate_reason == state.candidate_reason &&
        previous->candidate_revision == state.candidate_revision &&
        strcmp(previous->gtk_module, state.gtk_module) == 0) return;
    static const char *const routes[] = {"pending", "gtk", "ibus", "fcitx5"};
    static const char *const reasons[] = {"pending", "non-wayland", "explicit-gtk",
        "explicit-module", "explicit-ibus", "wayland-clauses", "legacy-ibus",
        "gtk-fallback", "ibus-unavailable", "display-closed", "x11-ibus", "client-candidates", "explicit-fcitx5", "fcitx5-unavailable"};
    static const char *const candidate_policies[] = {"auto", "system", "client"};
    static const char *const candidate_owners[] = {"none", "system", "client"};
    static const char *const candidate_reasons[] = {"pending", "ready", "system-request",
        "standard-input", "no-renderer", "ibus-unavailable"};
    application->reported_ime_state = state;
    application->ime_state_reported = true;
    fprintf(stdout, "boxrig_term: ime-policy=%d ime-route=%s ime-reason=%s gtk=%u.%u.%u display=%s module=%s registry=%d/%d text-input-version=%u ibus=%d contexts=%zu errors=%zu candidate-policy=%s candidate-owner=%s candidate-reason=%s candidate-revision=%" PRIu64 " candidate-count=%zu candidate-cursor=%zu candidate-visible=%d auxiliary-visible=%d\n",
        state.requested_policy, routes[state.active_route], reasons[state.reason],
        state.gtk_major, state.gtk_minor, state.gtk_micro, state.display_type, state.gtk_module,
        state.registry_queried, state.registry_complete, state.text_input_version, state.ibus_state,
        state.ibus_contexts, state.ibus_errors, candidate_policies[state.candidate_policy],
        candidate_owners[state.candidate_owner], candidate_reasons[state.candidate_reason],
        state.candidate_revision, state.candidate_count, state.candidate_cursor,
        state.candidate_visible, state.auxiliary_visible);
    fflush(stdout);
}

void boxrig_term_gtk3_report_render_state(boxrig_term_gtk3_application *application)
{
    report_ime_state(application);
    boxrig_gtk_render_state state;
    if (boxrig_gtk3_terminal_widget_get_render_state(application->widget, &state) != BOXRIG_UI_OK ||
        (state.active_backend == BOXRIG_GTK_RENDER_BACKEND_NONE && state.status == BOXRIG_UI_OK)) return;
    if (application->render_state_reported &&
        application->reported_render_state.requested_policy == state.requested_policy &&
        application->reported_render_state.active_backend == state.active_backend &&
        application->reported_render_state.pending == state.pending &&
        application->reported_render_state.is_gles == state.is_gles &&
        application->reported_render_state.reason == state.reason &&
        application->reported_render_state.status == state.status) return;
    application->reported_render_state = state;
    application->render_state_reported = true;
    bool opengl = state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL;
    boxrig_term_runtime_info info = {
        .frontend = "gtk3",
        .window_system = window_system(GTK_WIDGET(application->window)),
        .renderer = opengl ? "opengl" : state.active_backend == BOXRIG_GTK_RENDER_BACKEND_CAIRO ? "gtk3" : "unavailable",
        .graphics_api = opengl ? (state.is_gles ? "OpenGL ES" : "OpenGL") :
            state.active_backend == BOXRIG_GTK_RENDER_BACKEND_CAIRO ? "Cairo" : "unavailable",
        .graphics_api_version = state.active_backend == BOXRIG_GTK_RENDER_BACKEND_CAIRO ? cairo_version_string() : NULL};

    (void)boxrig_term_runtime_info_write(stdout, &info);
    fprintf(stdout, "boxrig_term: render-policy=%d render-backend=%d render-pending=%d render-reason=%d render-status=%d %s\n",
        state.requested_policy, state.active_backend, state.pending, state.reason, state.status, state.message);
    fflush(stdout);
}

static boxrig_ui_transparency_config widget_transparency_config(
    const boxrig_ui_transparency_config *config)
{
    boxrig_ui_transparency_config widget_config = *config;

    widget_config.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    return widget_config;
}

void boxrig_term_gtk3_set_window_title(
    boxrig_term_gtk3_application *application,
    const char *title)
{
    if (application == NULL || application->window == NULL || title == NULL) {
        return;
    }
    gtk_window_set_title(application->window, title);
    if (application->titlebar != NULL) {
        gtk_header_bar_set_title(application->titlebar, title);
    }
}

static void configure_widget_accessibility(GtkWidget *widget)
{
    static const char accessible_id[] = "boxrig-terminal";
    AtkObject *accessible;

    gtk_widget_set_name(widget, accessible_id);
    accessible = gtk_widget_get_accessible(widget);
    if (accessible != NULL &&
        g_object_class_find_property(
            G_OBJECT_GET_CLASS(accessible),
            "accessible-id") != NULL) {
        /* The property is available with ATK 2.36 while the widget remains
         * buildable against the older ATK paired with GTK 3.24. */
        g_object_set(accessible, "accessible-id", accessible_id, NULL);
    }
}

static int configure_window_background(
    boxrig_term_gtk3_application *application,
    GtkWidget *window)
{
    static const char style[] =
        ".boxrig-terminal-window {"
        " background-color: transparent;"
        " background-image: none;"
        " }"
        "window.boxrig-terminal-window decoration {"
        " box-shadow: none;"
        " margin: 0;"
        " padding: 0;"
        " border: 0;"
        " border-radius: 0;"
        " }"
        ".boxrig-terminal-window scrollbar {"
        " background-color: @theme_bg_color;"
        " }";
    GdkScreen *screen;
    GdkVisual *visual;
    GtkCssProvider *provider;
    GtkStyleContext *context;
    GError *error = NULL;

    if (application == NULL || window == NULL) {
        return -1;
    }
    screen = gtk_widget_get_screen(window);
    provider = gtk_css_provider_new();
    if (screen == NULL || provider == NULL) {
        g_clear_object(&provider);
        return -1;
    }
    if (!gtk_css_provider_load_from_data(provider, style, -1, &error)) {
        g_clear_error(&error);
        g_object_unref(provider);
        return -1;
    }
    gtk_style_context_add_provider_for_screen(
        screen,
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    context = gtk_widget_get_style_context(window);
    gtk_style_context_add_class(context, "boxrig-terminal-window");
    gtk_widget_set_app_paintable(window, TRUE);
    visual = gdk_screen_get_rgba_visual(screen);
    if (visual != NULL) {
        gtk_widget_set_visual(window, visual);
    }
    application->window_style_provider = provider;
    application->window_style_screen = g_object_ref(screen);
    return 0;
}

static boxrig_ui_window_state current_window_state(
    const boxrig_term_gtk3_application *application)
{
    GdkWindowState state;

    if (application->surface == NULL) {
        return BOXRIG_UI_WINDOW_NORMAL;
    }
    state = gdk_window_get_state(application->surface);
    if ((state & GDK_WINDOW_STATE_FULLSCREEN) != 0) {
        return BOXRIG_UI_WINDOW_FULLSCREEN;
    }
    if ((state & GDK_WINDOW_STATE_MAXIMIZED) != 0) {
        return BOXRIG_UI_WINDOW_MAXIMIZED;
    }
    if ((state & GDK_WINDOW_STATE_TILED) != 0) {
        return BOXRIG_UI_WINDOW_TILED;
    }
    return BOXRIG_UI_WINDOW_NORMAL;
}

static gboolean surface_state_changed(
    GtkWidget *window,
    GdkEventWindowState *event,
    gpointer user_data)
{
    boxrig_term_gtk3_application *application = user_data;
    boxrig_ui_window_state window_state;
    bool defer_normal_state;
    bool begin_normal_state_defer;

    (void)window;
    (void)event;
    window_state = current_window_state(application);
    defer_normal_state =
        boxrig_term_gtk3_normal_state_remains_deferred(
            application->window_state_initialized,
            application->normal_state_allocation_pending,
            application->window_state,
            window_state);
    begin_normal_state_defer =
        defer_normal_state &&
        !application->normal_state_allocation_pending;
    if (begin_normal_state_defer) {
        /* GTK reports the state before allocating its remembered normal
         * extent. Keep boxrig constrained until that allocation cannot
         * supersede the restore action created by the NORMAL transition. */
        application->normal_state_allocation_pending = true;
        application->normal_state_transition_extent_generation =
            application->window_allocation_extent_generation;
    } else if (window_state != BOXRIG_UI_WINDOW_NORMAL) {
        application->normal_state_allocation_pending = false;
    }
    application->window_state = window_state;
    application->window_state_initialized = true;
    boxrig_term_gtk3_geometry_trace_record(
        application->geometry_trace,
        "state",
        window_state,
        0,
        0,
        0,
        0);
    if (!defer_normal_state && application->widget != NULL &&
        !application->failed &&
        !application->quitting &&
        boxrig_gtk3_terminal_widget_set_window_state(
            application->widget,
            window_state) != BOXRIG_UI_OK) {
        boxrig_term_gtk3_fail(application);
        return FALSE;
    }
    boxrig_term_gtk3_request_dispatch(application);
    return FALSE;
}

static void window_allocation_changed(
    GtkWidget *window,
    GtkAllocation *allocation,
    gpointer user_data)
{
    boxrig_term_gtk3_application *application = user_data;

    (void)window;
    /* The GtkWindow class handler has already allocated its children. Native
     * configure-event arrives earlier and can still expose their old size. */
    application->window_allocation_generation =
        application->window_allocation_generation == UINT64_MAX
            ? 1
            : application->window_allocation_generation + 1;
    application->window_allocation_extent_generation =
        boxrig_term_gtk3_allocation_extent_generation(
            application->window_allocation_extent_generation,
            application->window_allocation_width,
            application->window_allocation_height,
            allocation->width,
            allocation->height);
    application->window_allocation_width = allocation->width;
    application->window_allocation_height = allocation->height;
    boxrig_term_gtk3_geometry_trace_record(
        application->geometry_trace,
        "window-allocation",
        application->window_state,
        0,
        0,
        0,
        0);
    boxrig_term_gtk3_request_dispatch(application);
}

static gboolean native_configure_changed(
    GtkWidget *window,
    GdkEventConfigure *event,
    gpointer user_data)
{
    boxrig_term_gtk3_application *application = user_data;

    (void)window;
    boxrig_term_gtk3_geometry_trace_record(
        application->geometry_trace,
        "native-configure",
        application->window_state,
        event->width,
        event->height,
        0,
        0);
    return FALSE;
}

static void content_allocation_changed(
    GtkWidget *content,
    GtkAllocation *allocation,
    gpointer user_data)
{
    boxrig_term_gtk3_application *application = user_data;

    (void)content;
    (void)allocation;
    boxrig_term_gtk3_geometry_trace_record(
        application->geometry_trace,
        "content-allocation",
        application->window_state,
        0,
        0,
        0,
        0);
}

static void terminal_allocation_changed(
    GtkWidget *terminal,
    GtkAllocation *allocation,
    gpointer user_data)
{
    boxrig_term_gtk3_application *application = user_data;
#ifdef BOXRIG_TERM_ENABLE_BACKGROUND_EFFECT
    update_background_effect(application);
#endif


    (void)terminal;
    (void)allocation;
    boxrig_term_gtk3_geometry_trace_record(
        application->geometry_trace,
        "terminal-allocation",
        application->window_state,
        0,
        0,
        0,
        0);
}

static gboolean terminal_drawn(
    GtkWidget *terminal,
    cairo_t *cairo,
    gpointer user_data)
{
    boxrig_term_gtk3_application *application = user_data;

    (void)terminal;
    (void)cairo;
    boxrig_term_gtk3_geometry_trace_record(
        application->geometry_trace,
        "draw",
        application->window_state,
        0,
        0,
        0,
        0);
    return FALSE;
}

static void window_realized(GtkWidget *window, gpointer user_data)
{
    boxrig_term_gtk3_application *application = user_data;
    GdkWindow *surface = gtk_widget_get_window(window);

    g_set_object(&application->surface, surface);
    (void)surface_state_changed(window, NULL, application);
}

static void widget_mapped(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    boxrig_term_gtk3_request_dispatch(user_data);
}

void boxrig_term_gtk3_finish_if_ready(
    boxrig_term_gtk3_application *application)
{
    if (application == NULL || application->quitting ||
        !application->child_completed ||
        (!application->peer_closed && !application->frontend_closed &&
         !application->failed)) {
        return;
    }
    application->quitting = true;
    /* Quitting the main loop does not cancel sources already selected for
     * dispatch, including PTY errors that arrive with child exit. */
    if (application->dispatch_source != NULL) {
        g_source_destroy(application->dispatch_source);
    }
    /* GTK destroys child widget resources even while the application holds
     * references. End the policy's borrowed view lifetime before that cascade. */
    if (application->widget != NULL) {
        (void)boxrig_gtk3_terminal_widget_quiesce(application->widget);
    }
    boxrig_term_gtk3_stop_links(application);
    boxrig_term_text_policy_destroy(application->text_policy);
    application->text_policy = NULL;
    if (application->window != NULL) {
        gtk_widget_destroy(GTK_WIDGET(application->window));
    }
    if (application->gtk_application != NULL) {
        g_application_quit(G_APPLICATION(application->gtk_application));
    }
}

void boxrig_term_gtk3_fail(
    boxrig_term_gtk3_application *application)
{
    pid_t child;

    if (application == NULL || application->failed) {
        return;
    }
    application->failed = true;
    application->result = 1;
    application->frame_requested = false;
    fprintf(stderr, "boxrig_term: GTK 3 application failed\n");
    if (application->widget != NULL) {
        (void)boxrig_gtk3_terminal_widget_quiesce(application->widget);
    }
    if (application->pty != NULL && !application->child_completed) {
        child = boxrig_posix_pty_child(application->pty);
        if (kill(child, SIGTERM) == -1 && errno != ESRCH) {
            application->child_completed = true;
        }
    } else if (!application->runtime_started) {
        application->child_completed = true;
    }
    boxrig_term_gtk3_finish_if_ready(application);
}

static gboolean window_close_requested(
    GtkWidget *window,
    GdkEvent *event,
    gpointer user_data)
{
    boxrig_term_gtk3_application *application = user_data;

    (void)window;
    (void)event;
    if (!application->frontend_closed) {
        application->frontend_closed = true;
        if (application->widget != NULL) {
            (void)boxrig_gtk3_terminal_widget_set_visible(
                application->widget,
                false);
            gtk_widget_set_sensitive(GTK_WIDGET(application->widget), false);
        }
        if (application->terminal_container != NULL) {
            gtk_widget_set_sensitive(
                GTK_WIDGET(application->terminal_container),
                false);
        }
        if (application->pty == NULL ||
            (kill(boxrig_posix_pty_child(application->pty), SIGHUP) == -1 &&
             errno != ESRCH)) {
            boxrig_term_gtk3_fail(application);
        }
    }
    boxrig_term_gtk3_finish_if_ready(application);
    return TRUE;
}

static void activate_application(
    GtkApplication *gtk_application,
    gpointer user_data)
{
    boxrig_term_gtk3_application *application = user_data;
    boxrig_gtk3_terminal_widget_config widget_config;
    GtkWidget *titlebar;
    GtkWidget *window;

    if (application->activation_started) {
        if (application->window != NULL) {
            gtk_window_present(application->window);
        }
        return;
    }
    application->activation_started = true;
    window = gtk_application_window_new(gtk_application);
    if (window == NULL) {
        boxrig_term_gtk3_fail(application);
        return;
    }
    application->window = GTK_WINDOW(g_object_ref_sink(window));
    if (configure_window_background(application, window) == -1) {
        boxrig_term_gtk3_fail(application);
        return;
    }
    titlebar = gtk_header_bar_new();
    if (titlebar == NULL) {
        boxrig_term_gtk3_fail(application);
        return;
    }
    application->titlebar = GTK_HEADER_BAR(g_object_ref_sink(titlebar));
    gtk_header_bar_set_has_subtitle(application->titlebar, FALSE);
    gtk_header_bar_set_show_close_button(application->titlebar, TRUE);
    boxrig_term_gtk3_set_window_title(
        application,
        application->config->window_title);
    g_signal_connect(
        application->window,
        "delete-event",
        G_CALLBACK(window_close_requested),
        application);
    application->surface_state_handler = g_signal_connect(
        application->window,
        "window-state-event",
        G_CALLBACK(surface_state_changed),
        application);
    application->window_allocation_handler = g_signal_connect(
        application->window,
        "size-allocate",
        G_CALLBACK(window_allocation_changed),
        application);
    g_signal_connect(
        application->window,
        "configure-event",
        G_CALLBACK(native_configure_changed),
        application);
    g_signal_connect(
        application->window,
        "realize",
        G_CALLBACK(window_realized),
        application);

    widget_config = boxrig_gtk3_terminal_widget_config_default();
    const char *ime = getenv("BOXRIG_GTK_IME");
    if (ime != NULL && strcmp(ime, "auto") != 0) {
        if (strcmp(ime, "gtk") == 0) widget_config.ime_policy = BOXRIG_GTK_IME_GTK;
        else if (strcmp(ime, "ibus") == 0) widget_config.ime_policy = BOXRIG_GTK_IME_IBUS;
        else if (strcmp(ime, "fcitx5") == 0) widget_config.ime_policy = BOXRIG_GTK_IME_FCITX5;
        else {
            fprintf(stderr, "boxrig_term: BOXRIG_GTK_IME must be auto, gtk, ibus, or fcitx5\n");
            boxrig_term_gtk3_fail(application);
            return;
        }
    }
    const char *candidates = getenv("BOXRIG_IME_CANDIDATES");
    if (candidates != NULL && strcmp(candidates, "auto") != 0) {
        if (strcmp(candidates, "system") == 0) widget_config.candidate_policy = BOXRIG_UI_CANDIDATE_SYSTEM;
        else if (strcmp(candidates, "client") == 0) widget_config.candidate_policy = BOXRIG_UI_CANDIDATE_CLIENT;
        else {
            fprintf(stderr, "boxrig_term: BOXRIG_IME_CANDIDATES must be auto, system, or client\n");
            boxrig_term_gtk3_fail(application);
            return;
        }
    }
    widget_config.render_policy = application->config->renderer == BOXRIG_TERM_POSIX_RENDERER_AUTO
        ? BOXRIG_GTK_RENDER_OPENGL_PREFERRED
        : application->config->renderer == BOXRIG_TERM_POSIX_RENDERER_OPENGL
        ? BOXRIG_GTK_RENDER_OPENGL_REQUIRED : BOXRIG_GTK_RENDER_CAIRO;
    widget_config.paste_capacity =
        application->config->terminal.clipboard_capacity;
    widget_config.paste_chunk_size =
        application->config->terminal.output_capacity;
    widget_config.maximum_frame_items =
        BOXRIG_TERM_GTK3_ZOOM_SHORTCUT_BATCH_CAPACITY;
    widget_config.transparency =
        widget_transparency_config(&application->config->transparency);
    widget_config.frame_request_callback =
        boxrig_term_gtk3_request_dispatch;
    widget_config.frame_request_context = application;
    if (boxrig_gtk3_terminal_widget_create(
            application->model,
            &widget_config,
            &application->widget) != BOXRIG_UI_OK) {
        boxrig_term_gtk3_fail(application);
        return;
    }
    g_object_ref_sink(application->widget);
    configure_widget_accessibility(GTK_WIDGET(application->widget));
    gtk_widget_set_hexpand(GTK_WIDGET(application->widget), true);
    gtk_widget_set_vexpand(GTK_WIDGET(application->widget), true);
    g_signal_connect(
        application->widget,
        "map",
        G_CALLBACK(widget_mapped),
        application);
    if (boxrig_gtk3_terminal_widget_set_window_state(
            application->widget,
            BOXRIG_UI_WINDOW_NORMAL) != BOXRIG_UI_OK ||
        (boxrig_term_color_demo_initialize(
            &application->color_demo,
            boxrig_gtk3_terminal_widget_view(application->widget)) == -1 ||
            boxrig_term_font_demo_initialize(&application->font_demo,
            boxrig_gtk3_terminal_widget_view(application->widget),
            set_color_glyphs, application->widget) == -1 ||
        boxrig_term_scrollbar_demo_initialize(&application->scrollbar_demo,
            boxrig_gtk3_terminal_widget_view(application->widget),
            set_scrollbar_policy, application->widget) == -1 ||
            boxrig_term_decoration_demo_initialize(boxrig_gtk3_terminal_widget_view(application->widget)) == -1 ||
            boxrig_term_background_demo_initialize(boxrig_gtk3_terminal_widget_view(application->widget)) == -1) ||
        boxrig_term_text_policy_create(
            application->terminal,
            boxrig_gtk3_terminal_widget_view(application->widget),
            &application->text_policy) == -1 ||
        boxrig_term_gtk3_start_links(application) == -1 ||
        boxrig_gtk3_terminal_widget_set_key_router(
            application->widget,
            boxrig_term_gtk3_route_application_shortcut,
            application) != BOXRIG_UI_OK ||
        boxrig_gtk3_terminal_widget_set_mouse_router(
            application->widget,
            boxrig_term_gtk3_route_application_mouse,
            application) != BOXRIG_UI_OK ||
        boxrig_gtk3_terminal_widget_set_visible(
            application->widget,
            true) != BOXRIG_UI_OK) {
        boxrig_term_gtk3_fail(application);
        return;
    }
    boxrig_term_gtk3_observe_pointer(application);
    application->terminal_container =
        boxrig_gtk3_terminal_widget_create_scroll_container(application->widget);
    if (application->terminal_container == NULL) {
        boxrig_term_gtk3_fail(application);
        return;
    }
    g_object_ref_sink(application->terminal_container);
    if (boxrig_term_gtk3_resize_frame_create(
            application->window,
            GTK_WIDGET(application->titlebar),
            GTK_WIDGET(application->terminal_container),
            &application->resize_frame) == -1) {
        boxrig_term_gtk3_fail(application);
        return;
    }
    application->content_root =
        boxrig_term_gtk3_resize_frame_content_root(
            application->resize_frame);
    boxrig_term_gtk3_resize_frame_set_background_opacity(
        application->resize_frame,
        application->config->transparency.background_opacity);
    gtk_window_set_titlebar(
        application->window,
        boxrig_term_gtk3_resize_frame_titlebar_root(
            application->resize_frame));
    if (boxrig_term_gtk3_geometry_trace_create(
            GTK_WIDGET(application->window),
            application->content_root,
            GTK_WIDGET(application->widget),
            &application->geometry_trace) == -1) {
        boxrig_term_gtk3_fail(application);
        return;
    }
    g_signal_connect_after(
        application->content_root,
        "size-allocate",
        G_CALLBACK(content_allocation_changed),
        application);
    g_signal_connect_after(
        application->widget,
        "size-allocate",
        G_CALLBACK(terminal_allocation_changed),
        application);
    g_signal_connect_after(
        application->widget,
        "draw",
        G_CALLBACK(terminal_drawn),
        application);
    gtk_container_add(
        GTK_CONTAINER(application->window),
        application->content_root);
    if (boxrig_term_posix_runtime_start(
            application->runtime,
            application->config) == -1) {
        boxrig_term_gtk3_fail(application);
        return;
    }
    application->runtime_started = true;
    application->pty =
        boxrig_term_posix_runtime_pty(application->runtime);
    application->session =
        boxrig_term_posix_runtime_session(application->runtime);
    if (boxrig_term_gtk3_create_event_sources(application) == -1) {
        boxrig_term_gtk3_fail(application);
        return;
    }
    if (initialize_background_effect(application) == -1) {
        boxrig_term_gtk3_fail(application);
        return;
    }
    gtk_widget_show_all(GTK_WIDGET(application->window));
    gtk_window_present(application->window);
    (void)gtk_widget_grab_focus(GTK_WIDGET(application->widget));
    boxrig_term_gtk3_request_dispatch(application);
}

static void destroy_source(GSource **source)
{
    if (*source == NULL) {
        return;
    }
    g_source_destroy(*source);
    g_source_unref(*source);
    *source = NULL;
}

static void destroy_application(
    boxrig_term_gtk3_application *application)
{
#ifdef BOXRIG_TERM_ENABLE_BACKGROUND_EFFECT
    boxrig_gtk3_background_effect_destroy(application->background_effect);
    application->background_effect = NULL;
#endif
    destroy_source(&application->dispatch_source);
    destroy_source(&application->child_source);
    destroy_source(&application->notification_source);
    boxrig_freedesktop_notifications_destroy(application->notifications);
    application->notifications = NULL;
    if (application->widget != NULL) {
        (void)boxrig_gtk3_terminal_widget_quiesce(application->widget);
    }
    boxrig_term_gtk3_stop_links(application);
    boxrig_term_text_policy_destroy(application->text_policy);
    application->text_policy = NULL;
    boxrig_term_gtk3_native_host_destroy(application);
    boxrig_term_gtk3_resize_frame_destroy(application->resize_frame);
    application->resize_frame = NULL;
    application->content_root = NULL;
    if (application->terminal_container != NULL &&
        application->widget != NULL &&
        gtk_widget_get_parent(GTK_WIDGET(application->widget)) ==
            GTK_WIDGET(application->terminal_container)) {
        gtk_container_remove(
            GTK_CONTAINER(application->terminal_container),
            GTK_WIDGET(application->widget));
    }
    if (application->window != NULL &&
        application->terminal_container != NULL &&
        gtk_widget_get_parent(GTK_WIDGET(application->terminal_container)) ==
            GTK_WIDGET(application->window)) {
        gtk_container_remove(
            GTK_CONTAINER(application->window),
            GTK_WIDGET(application->terminal_container));
    }
    g_clear_object(&application->widget);
    g_clear_object(&application->terminal_container);
    g_clear_object(&application->titlebar);
    if (application->window_style_provider != NULL &&
        application->window_style_screen != NULL) {
        gtk_style_context_remove_provider_for_screen(
            application->window_style_screen,
            GTK_STYLE_PROVIDER(application->window_style_provider));
    }
    g_clear_object(&application->window_style_provider);
    g_clear_object(&application->window_style_screen);
    if (application->window != NULL) {
        gtk_widget_destroy(GTK_WIDGET(application->window));
    }
    boxrig_term_gtk3_geometry_trace_destroy(application->geometry_trace);
    application->geometry_trace = NULL;
    g_clear_object(&application->window);
    boxrig_term_posix_runtime_destroy(
        application->runtime,
        application->result);
    application->runtime = NULL;
    g_clear_object(&application->gtk_application);
}

int boxrig_term_gtk3_run(const boxrig_term_gtk3_config *config)
{
    boxrig_term_gtk3_application application;

    if (config == NULL || config->child_arguments == NULL ||
        config->child_arguments[0] == NULL || config->terminal_type == NULL ||
        config->terminal_type[0] == '\0' || config->window_title == NULL) {
        errno = EINVAL;
        return 1;
    }
    memset(&application, 0, sizeof(application));
    application.config = config;
    application.result = 1;
    application.remembered_background_opacity =
        config->transparency.background_opacity;
    application.enable_background_opacity =
        config->enable_background_opacity_shortcuts;
    (void)setlocale(LC_CTYPE, "");
    if (boxrig_term_posix_runtime_create(
            config,
            &application.runtime) == -1) {
        return 1;
    }
    application.terminal =
        boxrig_term_posix_runtime_terminal(application.runtime);
    application.model =
        boxrig_term_posix_runtime_model(application.runtime);
    application.host_consumer =
        boxrig_term_posix_runtime_host_consumer(application.runtime);
    application.gtk_application =
        gtk_application_new(NULL, G_APPLICATION_NON_UNIQUE);
    if (application.gtk_application == NULL) {
        destroy_application(&application);
        return 1;
    }
    g_signal_connect(
        application.gtk_application,
        "activate",
        G_CALLBACK(activate_application),
        &application);
    (void)g_application_run(
        G_APPLICATION(application.gtk_application),
        0,
        NULL);
    if (!application.child_completed) {
        application.result = 1;
    }
    destroy_application(&application);
    return application.result;
}
