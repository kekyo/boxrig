/* boxrig - Under MIT. */
#include "measurement.h"
#include "boxrig/gtk3.h"
#include "terminal_internal.h"
#include "../../examples/boxrig_term/common/text_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)
#define WIDGET(name) boxrig_gtk3_terminal_widget_##name

typedef struct replay {
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    BoxrigGtk3TerminalWidget *widget;
    GtkWidget *window;
    GtkWidget *activity;
    guint activity_timer;
    bool activity_lit;
    size_t activity_switches;
    size_t activity_paints[2];
    boxrig_term_text_policy *policy;
    bool frame_requested;
    uint64_t painted;
    size_t resumes;
    size_t work;
    size_t input_bytes;
    unsigned int cell_width;
    unsigned int cell_height;
} replay;

/* Keep the latest complete result count by logical identity, including zero.
 * The fixed diagnostic bound is independent of the recognizer's limits. */
static boxrig_text_recognizer_event results[8192];
static size_t result_count;
static size_t events;

boxrig_status __real_boxrig_terminal_text_recognizer_event_consume(boxrig_terminal *);
boxrig_status __wrap_boxrig_terminal_text_recognizer_event_consume(boxrig_terminal *terminal)
{
    boxrig_text_recognizer_event event;
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED ||
        event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_EVICTED);
    size_t index = 0;
    while (index < result_count && !(results[index].screen == event.screen &&
        results[index].line_id.epoch == event.line_id.epoch &&
        results[index].line_id.serial == event.line_id.serial)) index++;
    CHECK(index < G_N_ELEMENTS(results));
    if (index == result_count) result_count++;
    results[index] = event;
    events++;
    return __real_boxrig_terminal_text_recognizer_event_consume(terminal);
}

static void request_frame(void *context)
{
    ((replay *)context)->frame_requested = true;
}

static void painted(GdkFrameClock *clock, gpointer context)
{
    (void)clock;
    replay *test = context;
    test->painted = WIDGET(presentation_generation)(test->widget);
}

static void feed(replay *test, const char *bytes, size_t length)
{
    size_t consumed;
    BOXRIG_MEASURE_BEGIN(start);
    CHECK(boxrig_terminal_feed(test->terminal, (const uint8_t *)bytes,
        length, &consumed) == BOXRIG_STATUS_OK && consumed == length);
    BOXRIG_MEASURE_END(BOXRIG_MEASURE_FEED, start);
    test->frame_requested = true;
    test->input_bytes += length;
}

static bool resume(replay *test)
{
    size_t used = 0;
    bool more = false;
    BOXRIG_MEASURE_BEGIN(start);
    if (test->policy != NULL) {
        CHECK(boxrig_term_text_policy_resume(test->policy, 4096, &used, &more) == 0);
    } else {
        CHECK(boxrig_terminal_resume_text_recognizers(test->terminal, 4096,
            &used, &more) == BOXRIG_STATUS_OK);
        while (boxrig_terminal_text_recognizer_event_peek(test->terminal, NULL))
            CHECK(boxrig_terminal_text_recognizer_event_consume(test->terminal) == BOXRIG_STATUS_OK);
    }
    BOXRIG_MEASURE_END(BOXRIG_MEASURE_RECOGNITION, start);
    CHECK(used <= 4096);
    test->resumes++;
    test->work += used;
    return more;
}

typedef struct pending_update {
    replay *test;
    bool more;
    bool done;
    uint64_t previous_presentation;
} pending_update;

typedef struct update_source {
    GSource source;
    pending_update *pending;
} update_source;

static bool update_complete(const pending_update *pending)
{
    const replay *test = pending->test;
    return !pending->more && !test->frame_requested &&
        !boxrig_ui_view_frame_requested(WIDGET(view)(test->widget)) &&
        test->painted > pending->previous_presentation &&
        test->painted == WIDGET(presentation_generation)(test->widget);
}

static gboolean prepare_update(GSource *source, gint *timeout)
{
    const pending_update *pending = ((update_source *)source)->pending;
    *timeout = -1;
    return pending->more || pending->test->frame_requested || update_complete(pending);
}

static gboolean dispatch_update(GSource *source, GSourceFunc callback, gpointer data)
{
    (void)source;
    return callback(data);
}

static GSourceFuncs update_functions = {prepare_update, NULL, dispatch_update, NULL, NULL, NULL};

static gboolean advance(gpointer context)
{
    pending_update *pending = context;
    replay *test = pending->test;
    if (pending->more) pending->more = resume(test);
    CHECK(boxrig_ui_model_synchronize(test->model) == BOXRIG_UI_OK);
    if (test->frame_requested) {
        test->frame_requested = false;
        boxrig_ui_component_frame_result result;
        BOXRIG_MEASURE_BEGIN(start);
        CHECK(WIDGET(frame)(test->widget, &result) == BOXRIG_UI_OK);
        BOXRIG_MEASURE_END(BOXRIG_MEASURE_FRAME, start);
        test->frame_requested = result.additional_dispatch;
        boxrig_ui_layout_action action;
        while (WIDGET(take_layout_action)(test->widget, &action)) {
            CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
            CHECK(action.value.grid_resize.rows == 66 && action.value.grid_resize.columns == 190);
            CHECK(WIDGET(complete_grid_resize)(test->widget, action.revision, true) == BOXRIG_UI_OK);
        }
    }
    if (update_complete(pending)) {
        pending->done = true;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean expired(gpointer data)
{
    replay *test = data;
    boxrig_gtk_render_state state;
    CHECK(WIDGET(get_render_state)(test->widget, &state) == BOXRIG_UI_OK);
    fprintf(stderr, "replay timeout: requested=%d active=%d pending=%d message=%s\n",
        state.requested_policy, state.active_backend, state.pending, state.message);
    exit(1);
}

static void complete(replay *test)
{
    if (test->widget == NULL) {
        while (resume(test)) { }
    } else {
        pending_update pending = {test, true, false, test->painted};
        guint timeout = g_timeout_add_seconds(60, expired, test);
        GSource *source = g_source_new(&update_functions, sizeof(update_source));
        ((update_source *)source)->pending = &pending;
        g_source_set_callback(source, advance, &pending, NULL);
        g_source_set_priority(source, G_PRIORITY_DEFAULT_IDLE);
        CHECK(g_source_attach(source, NULL) != 0);
        while (!pending.done) g_main_context_iteration(NULL, true);
        g_source_destroy(source);
        g_source_unref(source);
        CHECK(g_source_remove(timeout));
    }
    CHECK(!boxrig_terminal_text_recognizer_event_peek(test->terminal, NULL));
}

static char *snapshot_hash(replay *test)
{
    boxrig_text_snapshot_options options = {0};
    options.screen = BOXRIG_TEXT_SCREEN_ALTERNATE;
    options.byte_capacity = 4 * 1024 * 1024;
    options.line_capacity = 65536;
    boxrig_text_snapshot *snapshot;
    CHECK(boxrig_terminal_create_text_snapshot(test->terminal, &options, &snapshot) == BOXRIG_STATUS_OK);
    size_t length;
    const uint8_t *bytes = boxrig_text_snapshot_data(snapshot, &length);
    char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes, length);
    boxrig_text_snapshot_destroy(snapshot);
    return hash;
}

static gboolean draw_activity(GtkWidget *widget, cairo_t *cairo, gpointer data)
{
    (void)widget;
    replay *test = data;
    test->activity_paints[test->activity_lit ? 1 : 0]++;
    cairo_set_source_rgb(cairo, 0, test->activity_lit ? 1 : 0, 0);
    cairo_paint(cairo);
    return FALSE;
}

static gboolean blink_activity(gpointer data)
{
    replay *test = data;
    test->activity_lit = !test->activity_lit;
    test->activity_switches++;
    gtk_widget_queue_draw(test->activity);
    return G_SOURCE_CONTINUE;
}

static void initialize(replay *test, const char *backend)
{
    memset(test, 0, sizeof(*test));
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 66;
    config.columns = 190;
    CHECK(boxrig_terminal_create(&config, &test->terminal) == BOXRIG_STATUS_OK);
    const char initial[] = "\033[?1049h\033[?25l";
    feed(test, initial, sizeof(initial) - 1);
    if (strcmp(backend, "core") == 0) {
        static const uint8_t pattern[] = "[Hh][Tt][Tt][Pp][Ss]?://"
            "([A-Za-z0-9]|[A-Za-z0-9][A-Za-z0-9._~:/?#@!$&+,;=%()-]*[A-Za-z0-9_~/#@%=/-])";
        boxrig_text_recognizer_options options = {0};
        options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH;
        options.limits = boxrig_text_recognizer_limits_default();
        options.pattern = pattern;
        options.pattern_length = sizeof(pattern) - 1;
        boxrig_text_regex_diagnostic diagnostic;
        uint64_t id;
        CHECK(boxrig_terminal_register_text_recognizer(test->terminal,
            &options, &diagnostic, &id) == BOXRIG_STATUS_OK);
        return;
    }
    CHECK(gtk_init_check(NULL, NULL));
    CHECK(boxrig_ui_model_create(test->terminal, &test->model) == BOXRIG_UI_OK);
    boxrig_gtk3_terminal_widget_config wc = WIDGET(config_default)();
    wc.frame_request_callback = request_frame;
    wc.frame_request_context = test;
    wc.render_policy = strcmp(backend, "opengl") == 0
        ? BOXRIG_GTK_RENDER_OPENGL_REQUIRED : BOXRIG_GTK_RENDER_CAIRO;
    CHECK(WIDGET(create)(test->model, &wc, &test->widget) == BOXRIG_UI_OK);
    boxrig_font_specification font = boxrig_font_specification_default();
    const boxrig_font_family families[] = {
        {"Noto Sans Mono", 14, BOXRIG_FONT_RESOURCE_NONE},
        {"Monospace", 9, BOXRIG_FONT_RESOURCE_NONE}};
    font.families = families;
    font.family_count = G_N_ELEMENTS(families);
    font.size_dip = 16;
    CHECK(WIDGET(set_font_specification)(test->widget, &font) == BOXRIG_UI_OK);
    boxrig_font_effective_metrics metrics;
    CHECK(WIDGET(get_effective_metrics)(test->widget, &metrics) == BOXRIG_FONT_CATALOG_OK);
    test->cell_width = metrics.cell_width;
    test->cell_height = metrics.cell_height;
    test->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_object_ref_sink(test->window);
    gtk_window_set_decorated(GTK_WINDOW(test->window), false);
    gtk_widget_set_size_request(GTK_WIDGET(test->widget), 190 * metrics.cell_width, 66 * metrics.cell_height);
    if (getenv("BOXRIG_MEASURE_BLINK") != NULL) {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_container_add(GTK_CONTAINER(test->window), box);
        gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(test->widget), FALSE, FALSE, 0);
        test->activity = gtk_drawing_area_new();
        gtk_widget_set_size_request(test->activity, 20, 24);
        gtk_box_pack_start(GTK_BOX(box), test->activity, FALSE, FALSE, 0);
        g_signal_connect(test->activity, "draw", G_CALLBACK(draw_activity), test);
        test->activity_timer = g_timeout_add(150, blink_activity, test);
    } else {
        gtk_container_add(GTK_CONTAINER(test->window), GTK_WIDGET(test->widget));
    }
    gtk_widget_show_all(test->window);
    CHECK(WIDGET(set_visible)(test->widget, true) == BOXRIG_UI_OK);
    GdkFrameClock *clock = gtk_widget_get_frame_clock(test->window);
    CHECK(clock != NULL);
    g_signal_connect_after(clock, "after-paint", G_CALLBACK(painted), test);
    CHECK(boxrig_term_text_policy_create(test->terminal, WIDGET(view)(test->widget),
        &test->policy) == 0);
    complete(test);
}

static size_t count_matches(void)
{
    size_t matches = 0;
    for (size_t index = 0; index < result_count; index++)
        if (results[index].kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED)
            matches += results[index].result_count;
    return matches;
}

static char *read_input(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL && fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    CHECK(size >= 0 && size <= 64 * 1024 * 1024 && fseek(file, 0, SEEK_SET) == 0);
    char *bytes = malloc((size_t)size + 1);
    CHECK(bytes != NULL && fread(bytes, 1, (size_t)size, file) == (size_t)size);
    CHECK(fclose(file) == 0 && memchr(bytes, 0, (size_t)size) == NULL);
    bytes[size] = 0;
    *length = (size_t)size;
    return bytes;
}

int main(int argc, char **argv)
{
    CHECK(argc == 4);
    CHECK(strcmp(argv[1], "core") == 0 || strcmp(argv[1], "cairo") == 0 || strcmp(argv[1], "opengl") == 0);
    CHECK(strcmp(argv[3], "detail") == 0 || strcmp(argv[3], "control") == 0);
    measurement_detail = strcmp(argv[3], "detail") == 0;
    const bool synthetic = strcmp(argv[2], "synthetic") == 0;
    size_t input_length = 0;
    char *input = synthetic ? NULL : read_input(argv[2], &input_length);
    const char *starts[4096];
    size_t lengths[4096], input_frames = 0;
    if (input != NULL) {
        const char *current = input;
        while ((current = strstr(current, "\033[?2026h")) != NULL) {
            const char *end = strstr(current + 8, "\033[?2026l");
            CHECK(end != NULL);
            if (end - current > 1000) {
                CHECK(input_frames < G_N_ELEMENTS(starts));
                starts[input_frames] = current;
                lengths[input_frames++] = (size_t)(end + 8 - current);
            }
            current = end + 8;
        }
        CHECK(input_frames != 0);
    }
    char *input_hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
        (const uint8_t *)(synthetic ? "synthetic-v1" : input), synthetic ? 12 : input_length);
    printf("{\"kind\":\"environment\",\"revision\":\"%s\",\"pid\":%ld,\"backend\":\"%s\","
        "\"detail\":%s,\"input_sha256\":\"%s\",\"input_frames\":%zu,\"gtk\":\"%u.%u.%u\"}\n",
        BOXRIG_MEASURE_REVISION, (long)getpid(), argv[1], measurement_detail ? "true" : "false",
        input_hash, input_frames, gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version());
    g_free(input_hash);
    printf("{\"kind\":\"presentation\",\"force_rgba\":%s,\"activity_blink\":%s}\n",
        getenv("BOXRIG_MEASURE_RGBA") != NULL ? "true" : "false",
        getenv("BOXRIG_MEASURE_BLINK") != NULL ? "true" : "false");
    replay test;
    initialize(&test, argv[1]);
    uint64_t process_start = measurement_clock(CLOCK_PROCESS_CPUTIME_ID);
    for (size_t update = 0; update < 41; update++) {
        measurement_reset();
#ifdef BOXRIG_MEASURE_TEXT_WORKLOADS
        boxrig_text_workload_counters_reset();
#endif
        test.resumes = test.work = test.input_bytes = events = 0;
        uint64_t elapsed_start = measurement_clock(CLOCK_MONOTONIC);
        uint64_t cpu_start = measurement_clock(CLOCK_THREAD_CPUTIME_ID);
        if (synthetic) {
            for (size_t row = 0; row < 66; row++) {
                char line[256];
                int count = snprintf(line, sizeof(line), "\033[%zu;1H\033[2Krow %zu state %zu %s",
                    row + 1, row, update, row == 65 ? "https://example.org/final" : "0123456789 abcdefghijklmnopqrstuvwxyz");
                CHECK(count > 0 && (size_t)count < sizeof(line));
                feed(&test, line, (size_t)count);
            }
        } else {
            size_t index = update % input_frames;
            feed(&test, starts[index], lengths[index]);
        }
        char marker[128];
        int marker_length = snprintf(marker, sizeof(marker),
            "\033[35;100H\033[0m\033[48;2;%zu;255;0m  \033[0m", 10 + update * 3);
        CHECK(marker_length > 0 && (size_t)marker_length < sizeof(marker));
        feed(&test, marker, (size_t)marker_length);
        complete(&test);
        uint64_t cpu = measurement_clock(CLOCK_THREAD_CPUTIME_ID) - cpu_start;
        uint64_t elapsed = measurement_clock(CLOCK_MONOTONIC) - elapsed_start;
        int processor = sched_getcpu();
        char frequency_path[128];
        snprintf(frequency_path, sizeof(frequency_path),
            "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", processor);
        FILE *frequency = fopen(frequency_path, "r");
        long khz = 0;
        if (frequency != NULL) {
            if (fscanf(frequency, "%ld", &khz) != 1) khz = 0;
            fclose(frequency);
        }
        boxrig_text_workload_counters counters = {0};
#ifdef BOXRIG_MEASURE_TEXT_WORKLOADS
        boxrig_text_workload_counters_read(&counters);
#endif
        size_t matches = count_matches();
        if (synthetic) CHECK(matches == 1);
        boxrig_gtk_render_state state = {0};
        if (test.widget != NULL) {
            CHECK(WIDGET(get_render_state)(test.widget, &state) == BOXRIG_UI_OK);
            CHECK(state.active_backend == (strcmp(argv[1], "opengl") == 0
                ? BOXRIG_GTK_RENDER_BACKEND_OPENGL : BOXRIG_GTK_RENDER_BACKEND_CAIRO));
            if (state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL) CHECK(measurement_gl_draws > 0);
            CHECK(measurement_gtk_compositions > 0);
            /* Validate the actual GTK image only after recording timings.
             * This diagnostic readback is never part of the renderer. */
            GdkPixbuf *pixel = gdk_pixbuf_get_from_window(
                gtk_widget_get_window(test.window),
                99 * test.cell_width + test.cell_width / 2,
                34 * test.cell_height + test.cell_height / 2, 1, 1);
            CHECK(pixel != NULL);
            const uint8_t *rgb = gdk_pixbuf_get_pixels(pixel);
            CHECK(rgb[0] == 10 + update * 3 && rgb[1] == 255 && rgb[2] == 0);
            g_object_unref(pixel);
        }
        printf("{\"kind\":\"update\",\"index\":%zu,\"warmup\":%s,\"elapsed_ms\":%.6f,\"cpu_ms\":%.6f,"
            "\"feed_to_compose_ms\":%.6f,\"active_backend\":%d,\"resumes\":%zu,\"work\":%zu,"
            "\"events\":%zu,\"matches\":%zu,\"input_bytes\":%zu,\"processor\":%d,\"khz\":%ld,"
            "\"more_work\":false,\"events_pending\":false,"
            "\"projection_cells\":%zu,\"projection_bytes\":%zu,\"position_comparisons\":%zu,"
            "\"prepare_steps\":%zu,\"decoded_scalars\":%zu,\"line_comparisons\":%zu,"
            "\"cached_empty_lines\":%zu,\"reused_lines\":%zu,"
            "\"text_sync_fragments\":%zu,\"text_sync_cells\":%zu,\"text_sync_bytes\":%zu,"
            "\"projection_metadata_fragments\":%zu,\"projection_cache_comparisons\":%zu,"
            "\"commit_steps\":%zu,\"released_results\":%zu,"
            "\"gl_draws\":%zu,\"signature_cells_total\":%zu,\"shape_rows_total\":%zu,",
            update, update == 0 ? "true" : "false", elapsed / 1e6, cpu / 1e6,
            measurement_last_composition > elapsed_start ? (measurement_last_composition - elapsed_start) / 1e6 : 0,
            state.active_backend, test.resumes, test.work, events, matches, test.input_bytes, processor, khz,
            counters.snapshot_projection_cells, counters.snapshot_projection_bytes,
            counters.snapshot_position_comparisons, counters.recognizer_prepare_steps,
            counters.recognizer_decoded_scalars, counters.recognizer_line_comparisons,
            counters.recognizer_cached_empty_lines, counters.recognizer_reused_lines,
            counters.text_sync_fragments, counters.text_sync_cells, counters.text_sync_bytes,
            counters.projection_metadata_fragments, counters.recognizer_projection_cache_comparisons,
            counters.recognizer_commit_steps, counters.recognizer_released_results,
            measurement_gl_draws, measurement_gl_statistics.row_signature_cell_count,
            measurement_gl_statistics.row_shape_count);
        measurement_print();
        char *hash = snapshot_hash(&test);
        printf(",\"text_sha256\":\"%s\"}\n", hash);
        g_free(hash);
    }
    printf("{\"kind\":\"complete\",\"updates\":41,\"process_cpu_ms\":%.6f,\"matches\":%zu,"
        "\"activity_switches\":%zu,\"activity_on_paints\":%zu,\"activity_off_paints\":%zu}\n",
        (measurement_clock(CLOCK_PROCESS_CPUTIME_ID) - process_start) / 1e6, count_matches(),
        test.activity_switches, test.activity_paints[1], test.activity_paints[0]);
    if (test.activity_timer != 0) {
        CHECK(test.activity_switches > 0 && test.activity_paints[0] > 0 && test.activity_paints[1] > 0);
        CHECK(g_source_remove(test.activity_timer));
    }
    boxrig_term_text_policy_destroy(test.policy);
    if (test.window != NULL) {
        CHECK(WIDGET(quiesce)(test.widget) == BOXRIG_UI_OK);
        gtk_widget_destroy(test.window);
        g_object_unref(test.window);
    }
    boxrig_ui_model_destroy(test.model);
    boxrig_terminal_destroy(test.terminal);
    free(input);
    return 0;
}
