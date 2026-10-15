/* boxrig - Under MIT. */
#ifdef BOXRIG_TEST_GTK3
#include "boxrig/gtk3.h"
#define Widget BoxrigGtk3TerminalWidget
#define widget_diagnostic boxrig_gtk3_terminal_widget_get_diagnostic
#define widget_visible boxrig_gtk3_terminal_widget_set_visible
#define widget_frame boxrig_gtk3_terminal_widget_frame
#define widget_composition boxrig_gtk3_terminal_widget_composition
#define widget_config boxrig_gtk3_terminal_widget_config
#define widget_config_default boxrig_gtk3_terminal_widget_config_default
#define widget_create boxrig_gtk3_terminal_widget_create
#define widget_ime_state boxrig_gtk3_terminal_widget_get_ime_state
#define widget_metrics boxrig_gtk3_terminal_widget_get_effective_metrics
#define widget_view boxrig_gtk3_terminal_widget_view
#define widget_take_layout boxrig_gtk3_terminal_widget_take_layout_action
#define widget_complete_resize boxrig_gtk3_terminal_widget_complete_grid_resize
#else
#include "boxrig/gtk4.h"
#define Widget BoxrigGtk4TerminalWidget
#define widget_diagnostic boxrig_gtk4_terminal_widget_get_diagnostic
#define widget_visible boxrig_gtk4_terminal_widget_set_visible
#define widget_frame boxrig_gtk4_terminal_widget_frame
#define widget_composition boxrig_gtk4_terminal_widget_composition
#define widget_config boxrig_gtk4_terminal_widget_config
#define widget_config_default boxrig_gtk4_terminal_widget_config_default
#define widget_create boxrig_gtk4_terminal_widget_create
#define widget_ime_state boxrig_gtk4_terminal_widget_get_ime_state
#define widget_metrics boxrig_gtk4_terminal_widget_get_effective_metrics
#define widget_view boxrig_gtk4_terminal_widget_view
#define widget_take_layout boxrig_gtk4_terminal_widget_take_layout_action
#define widget_complete_resize boxrig_gtk4_terminal_widget_complete_grid_resize
#endif
#include "ime_server.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL GTK protocol IME %s:%d: %s\n", __FILE__, __LINE__, #value); \
    abort(); } } while (0)

static GtkWidget *window;
static Widget *widget;
static boxrig_terminal *terminal;
static int channel;
static unsigned int last_frames;
static bool painted, dispatch_requested;
static char *probe_address;

static void request_frame(void *data)
{
    (void)data;
    dispatch_requested = true;
    g_main_context_wakeup(NULL);
}
static void iterate(void)
{
    gboolean block = !dispatch_requested;
    dispatch_requested = false;
    g_main_context_iteration(NULL, block);
}

static gboolean deadline(gpointer data)
{
    fprintf(stderr, "FAIL GTK protocol IME: no completion for %s\n", (const char *)data);
    boxrig_gtk_ime_state diagnostic;
    if (widget != NULL && widget_ime_state(widget, &diagnostic) == BOXRIG_UI_OK)
        fprintf(stderr, "route=%d bus=%d pending=%zu contexts=%zu errors=%zu\n",
            diagnostic.active_route, diagnostic.direct.state, diagnostic.direct.pending,
            diagnostic.direct.contexts, diagnostic.direct.errors);
    const uint8_t *bytes;
    if (terminal != NULL) fprintf(stderr, "output bytes=%zu\n", boxrig_terminal_output_peek(terminal, &bytes));
    boxrig_composition_view view;
    if (widget != NULL && boxrig_composition_get_view(widget_composition(widget), &view) == BOXRIG_COMPOSITION_OK)
        fprintf(stderr, "composition active=%d text=%.*s selection=%zu,%zu focus=%d composition-focus=%d candidate=%d\n",
            view.active, (int)view.text_length, view.text, view.selection_offset, view.selection_length,
            gtk_widget_has_focus(GTK_WIDGET(widget)), view.focused, view.candidate.visible);
    abort();
}
static void after_paint(GdkFrameClock *clock, gpointer data)
{ (void)clock; (void)data; painted = true; }
static void frame(void)
{
    boxrig_ui_component_frame_result result;
    dispatch_requested = false;
    do {
        CHECK(widget_frame(widget, &result) == BOXRIG_UI_OK);
        boxrig_ui_layout_action action;
        while (widget_take_layout(widget, &action)) {
            CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
            printf("Grid resize: %zu,%zu\n", action.value.grid_resize.rows, action.value.grid_resize.columns);
            CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows,
                action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
            CHECK(widget_complete_resize(widget, action.revision, true) == BOXRIG_UI_OK);
            result.additional_dispatch = true;
        }
    } while (result.additional_dispatch);
}
static boxrig_composition_view composition(void)
{
    boxrig_composition_view view;
    CHECK(boxrig_composition_get_view(widget_composition(widget), &view) == BOXRIG_COMPOSITION_OK);
    return view;
}
static ime_server_state state(void)
{
    /* The control socket is independent from GTK's Wayland connection. Wait
     * for its outgoing cursor/enable requests before reading server state. */
    gdk_display_sync(gtk_widget_get_display(window));
    return ime_server_exchange(channel, (ime_server_command){.operation = IME_SERVER_QUERY});
}

static size_t queued_input(void)
{
    boxrig_ui_component_diagnostic diagnostic;
    CHECK(widget_diagnostic(widget, &diagnostic) == BOXRIG_UI_OK);
    return diagnostic.native_input_queue_depth;
}

static void await_preedit(const char *text, size_t selection, size_t length)
{
    printf("Await preedit: %s selection=%zu,%zu\n", text, selection, length);
    guint timeout = g_timeout_add_seconds(60, deadline, "preedit");
    for (;;) {
        frame();
        const uint8_t *bytes;
        CHECK(boxrig_terminal_output_peek(terminal, &bytes) == 0);
        boxrig_composition_view view = composition();
        if (view.text_length == strlen(text) &&
            (view.text_length == 0 || memcmp(view.text, text, view.text_length) == 0) &&
            view.selection_offset == selection && view.selection_length == length && queued_input() == 0) break;
        iterate();
    }
    g_source_remove(timeout);
}

static void await_pixels(uint32_t selected)
{
    guint timeout = g_timeout_add_seconds(60, deadline, "rendered clause");
    boxrig_font_effective_metrics metrics;
    CHECK(widget_metrics(widget, &metrics) == BOXRIG_FONT_CATALOG_OK);
    unsigned int scale = (unsigned int)gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    double x, y;
#ifdef BOXRIG_TEST_GTK3
    gint tx, ty;
    CHECK(gtk_widget_translate_coordinates(GTK_WIDGET(widget), window, 0, 0, &tx, &ty));
    x = tx; y = ty;
#else
    graphene_point_t point;
    CHECK(gtk_widget_compute_point(GTK_WIDGET(widget), window, &GRAPHENE_POINT_INIT(0, 0), &point));
    double dx, dy;
    gtk_native_get_surface_transform(GTK_NATIVE(window), &dx, &dy);
    x = point.x + dx; y = point.y + dy;
#endif
    ime_server_command request = {.operation = IME_SERVER_PIXELS,
        .x = (int)(x * scale), .y = (int)(y * scale),
        .cell_width = (int)metrics.cell_width, .cell_height = (int)metrics.cell_height};
    printf("Await pixels: mask=%u origin=%d,%d cell=%d,%d\n", selected, request.x, request.y,
        request.cell_width, request.cell_height);
    unsigned int observed_frames = 0;
    painted = false;
    gtk_widget_queue_draw(GTK_WIDGET(widget));
    for (;;) {
        frame();
        if (painted) {
            gdk_display_sync(gtk_widget_get_display(window));
            ime_server_state observed = ime_server_exchange(channel, request);
            if (observed.frames != observed_frames) {
                printf("Observed pixels: frame=%u mask=%u\n", observed.frames, observed.selected_cells);
                observed_frames = observed.frames;
            }
            if (observed.frames > last_frames && observed.selected_cells == selected) {
                printf("GTK protocol pixels: frames=%u mask=%u origin=%d,%d cell=%d,%d\n",
                    observed.frames, observed.selected_cells, request.x, request.y, request.cell_width, request.cell_height);
                last_frames = observed.frames;
                break;
            }
        }
        iterate();
    }
    g_source_remove(timeout);
}
static void await_standard_cursor(void)
{
    guint timeout = g_timeout_add_seconds(60, deadline, "standard logical cursor");
    for (;;) {
        frame();
        boxrig_composition_view view = composition();
        int scale = gtk_widget_get_scale_factor(GTK_WIDGET(widget));
        double x, y;
#ifdef BOXRIG_TEST_GTK3
        gint tx, ty;
        CHECK(gtk_widget_translate_coordinates(GTK_WIDGET(widget), window, 0, 0, &tx, &ty));
        x = tx; y = ty;
#else
        graphene_point_t point;
        CHECK(gtk_widget_compute_point(GTK_WIDGET(widget), window, &GRAPHENE_POINT_INIT(0, 0), &point));
        double dx, dy;
        gtk_native_get_surface_transform(GTK_NATIVE(window), &dx, &dy);
        x = point.x + dx; y = point.y + dy;
#endif
        x += (double)view.candidate.exclude_x / scale;
        y += (double)view.candidate.exclude_y / scale;
        ime_server_state observed = state();
        if (view.candidate.visible && abs(observed.x - (int)x) <= 1 && abs(observed.y - (int)y) <= 1 &&
            observed.width == (int)((view.candidate.exclude_width + scale - 1) / scale) &&
            observed.height == (int)((view.candidate.exclude_height + scale - 1) / scale)) {
            printf("GTK protocol cursor: scale=%d logical=%d,%d,%d,%d\n", scale,
                observed.x, observed.y, observed.width, observed.height);
            break;
        }
        iterate();
    }
    g_source_remove(timeout);
}

static void send_preedit(const char *text, int begin, int end)
{
    ime_server_command command = {.operation = IME_SERVER_PREEDIT, .begin = begin, .end = end};
    CHECK(strlen(text) < sizeof(command.text));
    strcpy(command.text, text);
    (void)ime_server_exchange(channel, command);
}
static void send_key(int key)
{
    (void)ime_server_exchange(channel, (ime_server_command){.operation = IME_SERVER_KEY, .begin = key, .end = 1});
    (void)ime_server_exchange(channel, (ime_server_command){.operation = IME_SERVER_KEY, .begin = key, .end = 0});
}
static void await_output(const char *expected)
{
    guint timeout = g_timeout_add_seconds(60, deadline, "single ordered output");
    const uint8_t *bytes;
    while (boxrig_terminal_output_peek(terminal, &bytes) < strlen(expected)) {
        frame();
        if (boxrig_terminal_output_peek(terminal, &bytes) < strlen(expected))
            iterate();
    }
    CHECK(boxrig_terminal_output_peek(terminal, &bytes) == strlen(expected));
    CHECK(memcmp(bytes, expected, strlen(expected)) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, strlen(expected)) == BOXRIG_STATUS_OK);
    g_source_remove(timeout);
}

static void queue_key_after_old_preedit(void)
{
    /* Establish the engine focus and drain its initial notifications. Then
     * retain a native key without dispatching it, and have the real daemon
     * deliver an older preedit notification before that key reaches IBus. */
    send_key(30);
    await_output("a");
    guint timeout = g_timeout_add_seconds(60, deadline, "old preedit before a retained key");
    while (queued_input() != 0) { frame(); if (queued_input() != 0) iterate(); }
    send_key(59);
    while (queued_input() < 2) iterate();
    size_t before = queued_input();
    GError *error = NULL;
    GDBusConnection *probe = g_dbus_connection_new_for_address_sync(probe_address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);
    CHECK(probe != NULL && error == NULL);
    GVariant *reply = g_dbus_connection_call_sync(probe, "org.boxrig.IMEProbe", "/org/boxrig/IMEProbe",
        "org.boxrig.IMEProbe", "ClearPreedit", NULL, G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE,
        -1, NULL, &error);
    CHECK(reply != NULL && error == NULL);
    g_variant_unref(reply);
    while (queued_input() == before) iterate();
    g_object_unref(probe);
    g_source_remove(timeout);
    printf("Old preedit received while %zu native key events were retained\n", before);
}

static boxrig_gtk_ime_state await_route(boxrig_gtk_ime_route route, boxrig_ime_state bus)
{
    guint timeout = g_timeout_add_seconds(60, deadline, "input route");
    boxrig_gtk_ime_state selected;
    for (;;) {
        frame();
        CHECK(widget_ime_state(widget, &selected) == BOXRIG_UI_OK);
        if (selected.active_route == route && selected.direct.state == bus &&
            (route != BOXRIG_GTK_IME_ROUTE_IBUS || bus != BOXRIG_IME_READY || selected.direct.contexts != 0)) break;
        iterate();
    }
    g_source_remove(timeout);
    printf("GTK protocol runtime: %u.%u.%u display=%s module=%s route=%d reason=%d advertised=%u bus=%d\n",
        selected.gtk_major, selected.gtk_minor, selected.gtk_micro, selected.display_type, selected.gtk_module,
        selected.active_route, selected.reason, selected.text_input_version, selected.direct.state);
    return selected;
}

static void verify_candidates(bool client)
{
    GError *error = NULL;
    GDBusConnection *probe = g_dbus_connection_new_for_address_sync(probe_address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);
    CHECK(probe != NULL && error == NULL);
    GVariant *reply = g_dbus_connection_call_sync(probe, "org.boxrig.IMEProbe", "/org/boxrig/IMEProbe",
        "org.boxrig.IMEProbe", "GetCandidates", NULL, G_VARIANT_TYPE("(uuu)"), G_DBUS_CALL_FLAGS_NONE,
        -1, NULL, &error);
    CHECK(reply != NULL && error == NULL);
    guint capabilities, lookup, auxiliary;
    g_variant_get(reply, "(uuu)", &capabilities, &lookup, &auxiliary);
    g_variant_unref(reply);
    CHECK((capabilities & 6U) == (client ? 6U : 0U));
    if (client) {
        for (guint action = 1; action <= 2; action++) {
            reply = g_dbus_connection_call_sync(probe, "org.boxrig.IMEProbe", "/org/boxrig/IMEProbe",
                "org.boxrig.IMEProbe", "Candidates", g_variant_new("(u)", action), G_VARIANT_TYPE("()"),
                G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
            CHECK(reply != NULL && error == NULL);
            g_variant_unref(reply);
            guint timeout = g_timeout_add_seconds(60, deadline, "candidate presentation");
            boxrig_ui_candidate_view candidates;
            for (;;) {
                frame();
                CHECK(boxrig_ui_view_get_candidates(widget_view(widget), &candidates) == BOXRIG_UI_OK);
                if (candidates.snapshot.visible == (action == 1) &&
                    candidates.snapshot.auxiliary_visible == (action == 1) &&
                    candidates.layout.revision == candidates.revision &&
                    candidates.snapshot.count == 5) break;
                iterate();
            }
            g_source_remove(timeout);
            CHECK(strcmp(candidates.snapshot.items[1], "汽車") == 0);
            CHECK(strcmp(candidates.snapshot.labels[1], "s") == 0);
            CHECK(strcmp(candidates.snapshot.auxiliary, "説明🙂\n次の行") == 0);
            CHECK(candidates.snapshot.cursor == 1);
            CHECK(candidates.layout.count == (action == 1 ? 3U : 0U));
            if (action == 1) CHECK(candidates.layout.bounds.width > 0 && candidates.layout.bounds.height > 0);
            else CHECK(candidates.layout.bounds.width == 0 && candidates.layout.bounds.height == 0);
        }
    }
    g_object_unref(probe);
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    if (argc == 2 && strcmp(argv[1], "--runtime") == 0) {
        printf("%u.%u.%u\n", gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version());
        return 0;
    }
    /* font, protocol version, scale, renderer, policy, expected route, module, candidates */
    CHECK(argc == 9);
    const unsigned int version = (unsigned int)atoi(argv[2]), scale = (unsigned int)atoi(argv[3]);
    const char *expected = argv[6];
    probe_address = g_strdup(getenv("IBUS_ADDRESS"));
    CHECK(probe_address != NULL);
    bool direct = strcmp(expected, "direct") == 0 || strcmp(expected, "reconnect") == 0 ||
        strcmp(expected, "discovered") == 0;
    bool standard = strcmp(expected, "standard") == 0;
    bool module_ibus = strcmp(expected, "gtk-ibus") == 0;
    bool unavailable = strcmp(expected, "unavailable") == 0;
    CHECK(setenv("GDK_BACKEND", "wayland", 1) == 0);
    CHECK(setenv("GSK_RENDERER", "cairo", 1) == 0);
    CHECK(setenv("NO_AT_BRIDGE", "1", 1) == 0);
    CHECK(setenv("GTK_A11Y", "none", 1) == 0);
    CHECK(setenv("IBUS_ENABLE_SYNC_MODE", "2", 1) == 0);
    if (strcmp(argv[7], "default") == 0) CHECK(unsetenv("GTK_IM_MODULE") == 0);
    else CHECK(setenv("GTK_IM_MODULE", argv[7], 1) == 0);
    if (unavailable) CHECK(setenv("IBUS_ADDRESS", "unix:path=/tmp/boxrig-no-such-ibus.sock", 1) == 0);
    if (strcmp(expected, "discovered") == 0) {
        CHECK(unsetenv("IBUS_ADDRESS") == 0);
        CHECK(unsetenv("XMODIFIERS") == 0);
    }
    const char *module_path = getenv("BOXRIG_TEST_GTK_MODULE_PATH");
    if (module_path != NULL) CHECK(setenv("GTK_PATH", module_path, 1) == 0);
    pid_t server;
    channel = ime_server_start(version, scale, &server);
#ifdef BOXRIG_TEST_GTK3
    gtk_init(NULL, NULL);
#else
    gtk_init();
#endif
    char *theme = NULL;
    g_object_get(gtk_settings_get_default(), "gtk-theme-name", &theme, NULL);
    printf("GTK theme: %s\n", theme);
    g_free(theme);
    const char *runtime = getenv("BOXRIG_TEST_GTK_RUNTIME");
    if (runtime != NULL) {
        unsigned int major, minor, micro;
        CHECK(sscanf(runtime, "%u.%u.%u", &major, &minor, &micro) == 3);
        CHECK(gtk_get_major_version() == major && gtk_get_minor_version() == minor && gtk_get_micro_version() == micro);
    }
    if (strcmp(expected, "discovered") == 0)
        g_object_set(gtk_settings_get_default(), "gtk-im-module", "gtk-im-context-simple", NULL);
    if (strcmp(expected, "reconnect") == 0)
        g_object_set(gtk_settings_get_default(), "gtk-im-module", "wayland", NULL);
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.columns = 32; config.rows = 10;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_ui_model *model;
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    widget_config wc = widget_config_default();
    wc.frame_request_callback = request_frame;
    wc.frame_request_context = &dispatch_requested;
    wc.render_policy = strcmp(argv[4], "opengl") == 0 ? BOXRIG_GTK_RENDER_OPENGL_REQUIRED : BOXRIG_GTK_RENDER_CAIRO;
    wc.ime_policy = strcmp(argv[5], "ibus") == 0 ? BOXRIG_GTK_IME_IBUS :
        strcmp(argv[5], "gtk") == 0 ? BOXRIG_GTK_IME_GTK : BOXRIG_GTK_IME_AUTO;
    wc.candidate_policy = strcmp(argv[8], "client") == 0 ? BOXRIG_UI_CANDIDATE_CLIENT :
        strcmp(argv[8], "system") == 0 ? BOXRIG_UI_CANDIDATE_SYSTEM : BOXRIG_UI_CANDIDATE_AUTO;
    CHECK(widget_create(model, &wc, &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    CHECK(widget_visible(widget, true) == BOXRIG_UI_OK);
    boxrig_gtk_ime_state selected;
    CHECK(widget_ime_state(widget, &selected) == BOXRIG_UI_OK);
    CHECK(selected.active_route == BOXRIG_GTK_IME_ROUTE_PENDING && !selected.registry_queried);
#ifdef BOXRIG_TEST_GTK3
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#else
    window = gtk_window_new();
#endif
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), 640, 400);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(GTK_WIDGET(widget), 23);
    gtk_widget_set_margin_top(GTK_WIDGET(widget), 17);
#ifdef BOXRIG_TEST_GTK3
    gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(widget), TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(window), box);
    gtk_widget_show_all(window);
#else
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(widget));
    gtk_window_set_child(GTK_WINDOW(window), box);
#endif
    /* Subscribe a second widget and destroy it before running the main loop.
     * Completion of the shared registry query must never call its freed data. */
    boxrig_terminal *transient_terminal;
    boxrig_ui_model *transient_model;
    Widget *transient;
    CHECK(boxrig_terminal_create(&config, &transient_terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(transient_terminal, &transient_model) == BOXRIG_UI_OK);
    CHECK(widget_create(transient_model, &wc, &transient) == BOXRIG_UI_OK);
    g_object_ref_sink(transient);
#ifdef BOXRIG_TEST_GTK3
    gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(transient), TRUE, TRUE, 0);
    gtk_widget_realize(GTK_WIDGET(transient));
    gtk_widget_destroy(GTK_WIDGET(transient));
#else
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(transient));
    gtk_box_remove(GTK_BOX(box), GTK_WIDGET(transient));
#endif
    g_object_unref(transient);
    boxrig_ui_model_destroy(transient_model);
    boxrig_terminal_destroy(transient_terminal);
    gtk_window_present(GTK_WINDOW(window));
    guint timeout = g_timeout_add_seconds(60, deadline, "mapped focused window");
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget)) || !gtk_window_is_active(GTK_WINDOW(window)))
        iterate();
    g_source_remove(timeout);
    g_signal_connect(gtk_widget_get_frame_clock(window), "after-paint", G_CALLBACK(after_paint), NULL);
    gtk_widget_grab_focus(GTK_WIDGET(widget));
    selected = await_route(direct ? BOXRIG_GTK_IME_ROUTE_IBUS : BOXRIG_GTK_IME_ROUTE_GTK,
        direct ? BOXRIG_IME_READY : unavailable ? BOXRIG_IME_UNAVAILABLE : BOXRIG_IME_DISABLED);
    CHECK(strcmp(selected.display_type, "GdkWaylandDisplay") == 0);
    if (wc.ime_policy == BOXRIG_GTK_IME_AUTO && (strcmp(argv[7], "default") == 0 || argv[7][0] == '\0')) {
        CHECK(selected.registry_queried && selected.registry_complete && selected.text_input_version == version);
        CHECK(selected.reason == (standard ? BOXRIG_GTK_IME_REASON_WAYLAND_CLAUSES :
            direct ? (wc.candidate_policy == BOXRIG_UI_CANDIDATE_CLIENT ?
                BOXRIG_GTK_IME_REASON_CLIENT_CANDIDATES : BOXRIG_GTK_IME_REASON_LEGACY_DIRECT) :
            unavailable ? BOXRIG_GTK_IME_REASON_DIRECT_UNAVAILABLE : BOXRIG_GTK_IME_REASON_GTK_FALLBACK));
    } else {
        CHECK(!selected.registry_queried && !selected.registry_complete);
        CHECK(selected.reason == (wc.ime_policy == BOXRIG_GTK_IME_GTK ? BOXRIG_GTK_IME_REASON_EXPLICIT_GTK :
            wc.ime_policy == BOXRIG_GTK_IME_IBUS ? BOXRIG_GTK_IME_REASON_EXPLICIT_IBUS : BOXRIG_GTK_IME_REASON_EXPLICIT_MODULE));
    }
    CHECK(selected.candidate_policy == wc.candidate_policy);
    bool client_candidates = direct && wc.candidate_policy != BOXRIG_UI_CANDIDATE_SYSTEM;
    CHECK(selected.candidate_owner == (client_candidates ? BOXRIG_UI_CANDIDATE_OWNER_CLIENT :
        BOXRIG_UI_CANDIDATE_OWNER_SYSTEM));
    CHECK(selected.candidate_reason == (wc.candidate_policy == BOXRIG_UI_CANDIDATE_SYSTEM ?
        BOXRIG_UI_CANDIDATE_SYSTEM_REQUEST : unavailable ? BOXRIG_UI_CANDIDATE_IME_UNAVAILABLE :
        client_candidates ? BOXRIG_UI_CANDIDATE_READY : BOXRIG_UI_CANDIDATE_STANDARD_INPUT));
    if (direct || module_ibus) {
        CHECK(state().enabled == 0); /* No second input owner. */
        if (direct) queue_key_after_old_preedit();
        else send_key(59); /* F1 is consumed by the isolated IBus engine. */
        await_preedit("記者の汽車が記者で記者した", 0, 9);
        send_key(106);
        await_preedit("記者の汽車が記者で記者した", 9, 9);
        send_key(28);
        await_output("記者の汽車が記者で記者した");
        await_preedit("", 0, 0);
        send_key(59);
        await_preedit("記者の汽車が記者で記者した", 0, 9);
        send_key(1);
        await_preedit("", 0, 0);
        verify_candidates(client_candidates);
    } else if (standard) {
        CHECK(strcmp(selected.gtk_module, "wayland") == 0);
        timeout = g_timeout_add_seconds(60, deadline, "text-input enabled");
        while (!state().enabled) iterate();
        g_source_remove(timeout);
        CHECK(state().contexts == 1 && state().version == 2);
        const size_t ranges[][2] = {{0,3},{3,6},{6,7},{0,4},{0,3}};
        for (size_t index = 0; index < sizeof(ranges)/sizeof(ranges[0]); index++) {
            size_t begin = ranges[index][0], end = ranges[index][1];
            send_preedit("abcDEFg", (int)begin, (int)end);
            await_preedit("abcDEFg", begin, end - begin);
            await_pixels(((1U << end) - 1) & ~((1U << begin) - 1));
            await_standard_cursor();
        }
        (void)ime_server_exchange(channel, (ime_server_command){.operation = IME_SERVER_CANCEL});
        await_preedit("", 0, 0);
        await_pixels(0);
        /* Mixed UTF-8 verifies byte hints survive the real GTK/Pango bridge. */
        send_preedit("aé日本z", 3, 9);
        await_preedit("aé日本z", 3, 6);
        ime_server_command commit = {.operation = IME_SERVER_COMMIT};
        strcpy(commit.text, "aé日本z");
        (void)ime_server_exchange(channel, commit);
        await_output("aé日本z");
        await_preedit("", 0, 0);
    }
    if (strcmp(expected, "gtk-wayland") == 0) {
        CHECK(strcmp(selected.gtk_module, "wayland") == 0);
        timeout = g_timeout_add_seconds(60, deadline, "explicit Wayland enabled");
        while (!state().enabled) iterate();
        g_source_remove(timeout);
        CHECK(state().contexts == 1);
        send_preedit("read", 0, 0);
        await_preedit("read", 0, 0);
        (void)ime_server_exchange(channel, (ime_server_command){.operation = IME_SERVER_CANCEL});
        await_preedit("", 0, 0);
    }
    if (strcmp(expected, "reconnect") == 0) {
        send_key(59);
        await_preedit("記者の汽車が記者で記者した", 0, 9);
        CHECK(kill((pid_t)atoi(getenv("BOXRIG_TEST_IBUS_DAEMON_PID")), SIGTERM) == 0);
        (void)await_route(BOXRIG_GTK_IME_ROUTE_GTK, BOXRIG_IME_UNAVAILABLE);
        await_preedit("", 0, 0);
        send_key(30); send_key(28);
        await_output("a\r");
        timeout = g_timeout_add_seconds(60, deadline, "fallback Wayland enabled");
        while (!state().enabled) iterate();
        g_source_remove(timeout);
        send_preedit("hold", 0, 0);
        await_preedit("hold", 0, 0);
        CHECK(write(3, "restart\n", 8) == 8);
        char reply[128];
        CHECK(read(4, reply, sizeof(reply)) > 0);
        /* Reconnection may finish while the GTK owner is composing. Keep that
         * owner until its cancel has drained, then select the ready direct bus. */
        (void)await_route(BOXRIG_GTK_IME_ROUTE_GTK, BOXRIG_IME_READY);
        await_preedit("hold", 0, 0);
        (void)ime_server_exchange(channel, (ime_server_command){.operation = IME_SERVER_CANCEL});
        await_preedit("", 0, 0);
        (void)await_route(BOXRIG_GTK_IME_ROUTE_IBUS, BOXRIG_IME_READY);
        CHECK(state().enabled == 0);
        send_key(59);
        await_preedit("記者の汽車が記者で記者した", 0, 9);
        send_key(1);
        await_preedit("", 0, 0);
    }
    send_key(30);
    if (!direct && !module_ibus) send_key(28);
    await_output(direct || module_ibus ? "a" : "a\r");
    /* Unrooting must release the subscription and input focus without keeping
     * a stale delegate. The same widget can acquire this display again. */
#ifdef BOXRIG_TEST_GTK3
    gtk_container_remove(GTK_CONTAINER(box), GTK_WIDGET(widget));
#else
    gtk_box_remove(GTK_BOX(box), GTK_WIDGET(widget));
#endif
    CHECK(widget_ime_state(widget, &selected) == BOXRIG_UI_OK);
    CHECK(selected.active_route == BOXRIG_GTK_IME_ROUTE_PENDING && !selected.candidate_valid);
#ifdef BOXRIG_TEST_GTK3
    gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(widget), TRUE, TRUE, 0);
    gtk_widget_show_all(window);
#else
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(widget));
#endif
    gtk_widget_grab_focus(GTK_WIDGET(widget));
    selected = await_route(direct ? BOXRIG_GTK_IME_ROUTE_IBUS : BOXRIG_GTK_IME_ROUTE_GTK,
        direct ? BOXRIG_IME_READY : unavailable ? BOXRIG_IME_UNAVAILABLE : BOXRIG_IME_DISABLED);
    if (direct || module_ibus) {
        send_key(59);
        await_preedit("記者の汽車が記者で記者した", 0, 9);
        send_key(1);
        await_preedit("", 0, 0);
    }
    send_key(30);
    if (!direct && !module_ibus) send_key(28);
    await_output(direct || module_ibus ? "a" : "a\r");
#ifdef BOXRIG_TEST_GTK3
    gtk_widget_destroy(window);
#else
    gtk_window_destroy(GTK_WINDOW(window));
#endif
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    gdk_display_close(gdk_display_get_default());
    ime_server_stop(channel, server);
    g_free(probe_address);
    puts("PASS real GTK Wayland routing, clauses, cancellation and single input ownership");
    return 0;
}
