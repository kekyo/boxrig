/* boxrig - Under MIT. */
#define _POSIX_C_SOURCE 200809L
#include "../../examples/boxrig_term/common/text_policy.h"
#include "../../include/boxrig/terminal.h"
#include "../../include/boxrig/ui/view.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct counted_line {
    boxrig_text_screen screen;
    boxrig_text_line_id line_id;
    size_t annotations;
} counted_line;

static counted_line lines[20000];
static size_t line_count;
static size_t annotation_count;
static boxrig_terminal *terminal;
static unsigned int settle_serial;
static size_t event_seen[4];
static size_t clear_count;

bool __real_boxrig_terminal_text_recognizer_event_peek(
    const boxrig_terminal *, boxrig_text_recognizer_event *);

bool __wrap_boxrig_terminal_text_recognizer_event_peek(
    const boxrig_terminal *core, boxrig_text_recognizer_event *event)
{
    bool present = __real_boxrig_terminal_text_recognizer_event_peek(core, event);
    if (present && event != NULL && event->kind <= BOXRIG_TEXT_RECOGNIZER_EVENT_SCOPE_INCOMPLETE) {
        event_seen[event->kind]++;
    }
    return present;
}

int __real_boxrig_term_text_policy_create(
    boxrig_terminal *, boxrig_ui_view *, boxrig_term_text_policy **);

int __wrap_boxrig_term_text_policy_create(
    boxrig_terminal *core, boxrig_ui_view *view, boxrig_term_text_policy **out)
{
    int result = __real_boxrig_term_text_policy_create(core, view, out);
    if (result == 0) terminal = core;
    return result;
}

boxrig_ui_status __real_boxrig_ui_view_replace_text_annotations(
    boxrig_ui_view *, uint64_t, boxrig_text_screen, boxrig_text_line_id,
    uint64_t, const boxrig_ui_text_annotation *, size_t);

boxrig_ui_status __wrap_boxrig_ui_view_replace_text_annotations(
    boxrig_ui_view *view, uint64_t source, boxrig_text_screen screen,
    boxrig_text_line_id line_id, uint64_t revision,
    const boxrig_ui_text_annotation *annotations, size_t count)
{
    boxrig_ui_status result = __real_boxrig_ui_view_replace_text_annotations(
        view, source, screen, line_id, revision, annotations, count);
    if (result != BOXRIG_UI_OK || source != BOXRIG_TERM_URL_ANNOTATION_SOURCE_ID) return result;
    size_t index = 0;
    while (index < line_count && (lines[index].screen != screen ||
        lines[index].line_id.epoch != line_id.epoch ||
        lines[index].line_id.serial != line_id.serial)) index++;
    if (index < line_count) {
        annotation_count -= lines[index].annotations;
    } else if (line_count < sizeof(lines) / sizeof(lines[0])) {
        index = line_count++;
        lines[index].screen = screen;
        lines[index].line_id = line_id;
    } else {
        return result;
    }
    lines[index].annotations = count;
    annotation_count += count;
    return result;
}

boxrig_ui_status __real_boxrig_ui_view_clear_text_annotations(boxrig_ui_view *, uint64_t);

boxrig_ui_status __wrap_boxrig_ui_view_clear_text_annotations(
    boxrig_ui_view *view, uint64_t source)
{
    boxrig_ui_status result = __real_boxrig_ui_view_clear_text_annotations(view, source);
    if (result == BOXRIG_UI_OK && source == BOXRIG_TERM_URL_ANNOTATION_SOURCE_ID) {
        line_count = 0;
        annotation_count = 0;
        clear_count++;
    }
    return result;
}

int __real_boxrig_term_text_policy_resume(
    boxrig_term_text_policy *, size_t, size_t *, bool *);

int __wrap_boxrig_term_text_policy_resume(
    boxrig_term_text_policy *policy, size_t budget, size_t *used, bool *more)
{
    int result = __real_boxrig_term_text_policy_resume(policy, budget, used, more);
    if (result == 0 && terminal != NULL && more != NULL && !*more) {
        const char *path = getenv("BOXRIG_ANNOTATION_COUNT_PATH");
        if (path != NULL && *path != '\0') {
            FILE *report = fopen(path, settle_serial++ == 0 ? "w" : "a");
            if (report != NULL) {
                struct timespec time;
                (void)clock_gettime(CLOCK_MONOTONIC, &time);
                fprintf(report, "%" PRIu64 "\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\n",
                    (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec,
                    boxrig_terminal_scrollback_rows(terminal), annotation_count, line_count,
                    event_seen[BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED],
                    event_seen[BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_EVICTED],
                    event_seen[BOXRIG_TEXT_RECOGNIZER_EVENT_RESCAN_REQUIRED],
                    event_seen[BOXRIG_TEXT_RECOGNIZER_EVENT_SCOPE_INCOMPLETE], clear_count);
                (void)fclose(report);
            }
        }
    }
    return result;
}
