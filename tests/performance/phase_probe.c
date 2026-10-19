/* Test-only GTK3 phase probe for a workload marked by an OSC window title. */
#define _POSIX_C_SOURCE 200809L

#include "../../examples/boxrig_term/gtk3/application_internal.h"
#include "../../src/pangocairo/renderer.h"
#include "phase_state.h"
#ifdef BOXRIG_PHASE_TRACE
#include "interval_trace.h"
#endif

#include <inttypes.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static boxrig_terminal *observed_terminal;
static boxrig_ui_view *observed_view;
static boxrig_phase_state phase_state;
static bool recorded;

static uint64_t monotonic_nanoseconds(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
}

static void record_if_settled(boxrig_ui_view *view)
{
    const char *path;
    FILE *output;
    if (recorded || observed_terminal == NULL || view != observed_view ||
        !phase_state.active ||
        !boxrig_phase_try_settle(
            &phase_state,
            boxrig_terminal_text_revision(observed_terminal),
            boxrig_ui_view_frame_requested(view),
            monotonic_nanoseconds())) {
        return;
    }
    path = getenv("BOXRIG_PHASE_PROBE_PATH");
    if (path == NULL || *path == '\0') {
        phase_state.settled_ns = 0;
        return;
    }
    output = fopen(path, "w");
    if (output == NULL) {
        phase_state.settled_ns = 0;
        return;
    }
    int written = fprintf(output,
            "revision\tmarker_ns\trecognition_ns\tcairo_ns\tframe_ns\tsettled_ns\n"
            "%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%" PRIu64 "\t%" PRIu64 "\n",
            phase_state.target_revision, phase_state.marker_ns,
            phase_state.recognition_ns, phase_state.cairo_ns,
            phase_state.frame_ns, phase_state.settled_ns);
    int close_status = fclose(output);
    if (written > 0 && close_status == 0) {
        const char *signal_path = getenv("BOXRIG_PHASE_PROBE_FIFO");
        int signal_fd;

        recorded = true;
        if (signal_path != NULL && *signal_path != '\0') {
            signal_fd = open(signal_path, O_WRONLY);
            if (signal_fd >= 0) {
                if (write(signal_fd, "1", 1) != 1) {
                    fprintf(stderr, "phase probe signal failed\n");
                }
                (void)close(signal_fd);
            }
        }
    } else {
        phase_state.settled_ns = 0;
    }
}

int __real_boxrig_term_text_policy_create(
    boxrig_terminal *terminal, boxrig_ui_view *view,
    boxrig_term_text_policy **policy_out);

int __wrap_boxrig_term_text_policy_create(
    boxrig_terminal *terminal, boxrig_ui_view *view,
    boxrig_term_text_policy **policy_out)
{
#ifdef BOXRIG_PHASE_TRACE
    boxrig_interval_initialize("BOXRIG_PHASE_TRACE_PATH");
#endif
    int result = __real_boxrig_term_text_policy_create(
        terminal, view, policy_out);

    if (result == 0) {
        observed_terminal = terminal;
        observed_view = view;
    }
    return result;
}

void __real_boxrig_term_gtk3_set_window_title(
    boxrig_term_gtk3_application *application, const char *title);

void __wrap_boxrig_term_gtk3_set_window_title(
    boxrig_term_gtk3_application *application, const char *title)
{
    static const char marker[] = "boxrig-phase-final";
    static const char history_marker[] = "boxrig-phase-history";

    __real_boxrig_term_gtk3_set_window_title(application, title);
    if (application == NULL || application->terminal == NULL ||
        title == NULL || (strcmp(title, marker) != 0 &&
                          strcmp(title, history_marker) != 0)) {
        return;
    }
    if (application->terminal != observed_terminal) {
        return;
    }
    boxrig_phase_mark(
        &phase_state,
        boxrig_terminal_text_revision(observed_terminal),
        monotonic_nanoseconds());
    recorded = false;
#ifdef BOXRIG_PHASE_TRACE
    boxrig_interval_end(strcmp(title, marker) == 0 ? "final_marker" : "history_marker",
        boxrig_interval_begin(), 0u, phase_state.target_revision, 0, false);
#endif
    record_if_settled(observed_view);
}

int __real_boxrig_term_text_policy_resume(
    boxrig_term_text_policy *policy, size_t work_budget,
    size_t *work_used_out, bool *more_work_out);

int __wrap_boxrig_term_text_policy_resume(
    boxrig_term_text_policy *policy, size_t work_budget,
    size_t *work_used_out, bool *more_work_out)
{
#ifdef BOXRIG_PHASE_TRACE
    boxrig_interval_start start = boxrig_interval_begin();
#endif
    int result = __real_boxrig_term_text_policy_resume(
        policy, work_budget, work_used_out, more_work_out);
#ifdef BOXRIG_PHASE_TRACE
    boxrig_interval_end("policy", start, *work_used_out,
        boxrig_terminal_text_revision(observed_terminal), result, *more_work_out);
#endif

    if (result == 0 && observed_terminal != NULL &&
        more_work_out != NULL && !*more_work_out) {
        boxrig_phase_observe_recognition(
            &phase_state,
            boxrig_terminal_text_revision(observed_terminal),
            monotonic_nanoseconds());
        record_if_settled(observed_view);
    }
    return result;
}

int __real_boxrig_pango_cairo_renderer_draw_view_composition(
    boxrig_pango_cairo_renderer *renderer, boxrig_ui_view *view,
    boxrig_composition *composition);

int __wrap_boxrig_pango_cairo_renderer_draw_view_composition(
    boxrig_pango_cairo_renderer *renderer, boxrig_ui_view *view,
    boxrig_composition *composition)
{
#ifdef BOXRIG_PHASE_TRACE
    boxrig_interval_start start = boxrig_interval_begin();
#endif
    int result = __real_boxrig_pango_cairo_renderer_draw_view_composition(
        renderer, view, composition);
#ifdef BOXRIG_PHASE_TRACE
    boxrig_interval_end("cairo", start, 1u,
        boxrig_terminal_text_revision(observed_terminal), result, false);
#endif

    if (result == 0 && observed_terminal != NULL &&
        view == observed_view) {
        boxrig_phase_observe_cairo(
            &phase_state,
            boxrig_terminal_text_revision(observed_terminal),
            monotonic_nanoseconds());
    }
    return result;
}

boxrig_ui_status __real_boxrig_gtk3_terminal_widget_frame(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_component_frame_result *result_out);

boxrig_ui_status __wrap_boxrig_gtk3_terminal_widget_frame(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_component_frame_result *result_out)
{
#ifdef BOXRIG_PHASE_TRACE
    boxrig_interval_start start = boxrig_interval_begin();
#endif
    boxrig_ui_status status = __real_boxrig_gtk3_terminal_widget_frame(
        widget, result_out);
#ifdef BOXRIG_PHASE_TRACE
    boxrig_interval_end("frame", start, 1u,
        boxrig_terminal_text_revision(observed_terminal), status, false);
#endif
    boxrig_ui_view *view;

    if (status != BOXRIG_UI_OK || observed_terminal == NULL ||
        widget == NULL || result_out == NULL) {
        return status;
    }
    view = boxrig_gtk3_terminal_widget_view(widget);
    if (view == observed_view &&
        !result_out->frame_pending) {
        boxrig_phase_observe_frame(
            &phase_state,
            boxrig_terminal_text_revision(observed_terminal),
            monotonic_nanoseconds());
        record_if_settled(view);
    }
    return status;
}
