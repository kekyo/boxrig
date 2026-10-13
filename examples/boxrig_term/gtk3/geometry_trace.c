/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "geometry_trace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <time.h>

struct boxrig_term_gtk3_geometry_trace {
    FILE *output;
    GtkWidget *window;
    GtkWidget *content_root;
    GtkWidget *terminal;
    uint64_t sequence;
    gint native_width;
    gint native_height;
};

static uint64_t monotonic_microseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1 || now.tv_sec < 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000);
}

static const char *window_state_name(boxrig_ui_window_state state)
{
    switch (state) {
    case BOXRIG_UI_WINDOW_NORMAL:
        return "normal";
    case BOXRIG_UI_WINDOW_MAXIMIZED:
        return "maximized";
    case BOXRIG_UI_WINDOW_FULLSCREEN:
        return "fullscreen";
    case BOXRIG_UI_WINDOW_TILED:
        return "tiled";
    default:
        return "unknown";
    }
}

int boxrig_term_gtk3_geometry_trace_create(
    GtkWidget *window,
    GtkWidget *content_root,
    GtkWidget *terminal,
    boxrig_term_gtk3_geometry_trace **trace_out)
{
    const char *path = getenv("BOXRIG_TERM_GTK3_GEOMETRY_TRACE");
    boxrig_term_gtk3_geometry_trace *trace;

    if (window == NULL || content_root == NULL || terminal == NULL ||
        trace_out == NULL) {
        return -1;
    }
    *trace_out = NULL;
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    trace = calloc(1, sizeof(*trace));
    if (trace == NULL) {
        return -1;
    }
    trace->output = fopen(path, "w");
    if (trace->output == NULL) {
        free(trace);
        return -1;
    }
    trace->window = window;
    trace->content_root = content_root;
    trace->terminal = terminal;
    if (fprintf(
            trace->output,
            "sequence,monotonic_us,event,state,native_width,native_height,"
            "window_width,window_height,content_width,content_height,"
            "terminal_width,terminal_height,rows,columns\n") < 0 ||
        fflush(trace->output) == EOF) {
        fclose(trace->output);
        free(trace);
        return -1;
    }
    *trace_out = trace;
    return 0;
}

void boxrig_term_gtk3_geometry_trace_record(
    boxrig_term_gtk3_geometry_trace *trace,
    const char *event,
    boxrig_ui_window_state state,
    gint native_width,
    gint native_height,
    size_t rows,
    size_t columns)
{
    if (trace == NULL || event == NULL) {
        return;
    }
    if (native_width > 0 && native_height > 0) {
        trace->native_width = native_width;
        trace->native_height = native_height;
    }
    trace->sequence++;
    (void)fprintf(
        trace->output,
        "%llu,%llu,%s,%s,%d,%d,%d,%d,%d,%d,%d,%d,%zu,%zu\n",
        (unsigned long long)trace->sequence,
        (unsigned long long)monotonic_microseconds(),
        event,
        window_state_name(state),
        trace->native_width,
        trace->native_height,
        gtk_widget_get_allocated_width(trace->window),
        gtk_widget_get_allocated_height(trace->window),
        gtk_widget_get_allocated_width(trace->content_root),
        gtk_widget_get_allocated_height(trace->content_root),
        gtk_widget_get_allocated_width(trace->terminal),
        gtk_widget_get_allocated_height(trace->terminal),
        rows,
        columns);
    (void)fflush(trace->output);
}

void boxrig_term_gtk3_geometry_trace_destroy(
    boxrig_term_gtk3_geometry_trace *trace)
{
    if (trace == NULL) {
        return;
    }
    (void)fclose(trace->output);
    free(trace);
}
