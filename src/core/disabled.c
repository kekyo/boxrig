/* boxrig - Under MIT. Empty results for optional services in reduced builds. */
#include "terminal_internal.h"
#include <stdlib.h>

#if !BOXRIG_FEATURE_TEXT_MONITOR
boxrig_status boxrig_terminal_register_text_monitor(
    boxrig_terminal *terminal,
    const boxrig_text_monitor_options *options,
    uint64_t *monitor_id_out)
{
    (void)terminal;
    (void)options;
    if (monitor_id_out) *monitor_id_out = 0;
    return BOXRIG_STATUS_LIMIT_EXCEEDED;
}

boxrig_status boxrig_terminal_unregister_text_monitor(
    boxrig_terminal *terminal,
    uint64_t monitor_id)
{
    (void)terminal;
    (void)monitor_id;
    return BOXRIG_STATUS_INVALID_ARGUMENT;
}

size_t boxrig_terminal_text_monitor_count(const boxrig_terminal *terminal)
{
    (void)terminal;
    return 0;
}

bool boxrig_terminal_text_monitor_event_peek(
    const boxrig_terminal *terminal,
    boxrig_text_monitor_event *event_out)
{
    (void)terminal;
    (void)event_out;
    return false;
}

boxrig_status boxrig_terminal_text_monitor_event_consume(
    boxrig_terminal *terminal)
{
    (void)terminal;
    return BOXRIG_STATUS_INVALID_ARGUMENT;
}

bool boxrig_terminal_text_monitor_needs_rescan(
    const boxrig_terminal *terminal,
    uint64_t monitor_id)
{
    (void)terminal;
    (void)monitor_id;
    return false;
}

boxrig_status boxrig_terminal_rescan_text_monitor(
    boxrig_terminal *terminal,
    uint64_t monitor_id)
{
    (void)terminal;
    (void)monitor_id;
    return BOXRIG_STATUS_INVALID_ARGUMENT;
}

boxrig_status boxrig_terminal_dispatch_text_monitors(
    boxrig_terminal *terminal,
    size_t work_budget,
    size_t *work_used_out,
    bool *more_work_out)
{
    (void)work_budget;
    if (!terminal || !work_used_out || !more_work_out) return BOXRIG_STATUS_INVALID_ARGUMENT;
    *work_used_out = 0;
    *more_work_out = false;
    return BOXRIG_STATUS_OK;
}
#endif

#if !BOXRIG_FEATURE_TEXT_RECOGNIZER
boxrig_text_recognizer_limits boxrig_text_recognizer_limits_default(void)
{
    return (boxrig_text_recognizer_limits){0};
}

boxrig_status boxrig_terminal_register_text_recognizer(
    boxrig_terminal *terminal,
    const boxrig_text_recognizer_options *options,
    boxrig_text_regex_diagnostic *diagnostic_out,
    uint64_t *recognizer_id_out)
{
    (void)terminal;
    (void)options;
    (void)diagnostic_out;
    if (recognizer_id_out) *recognizer_id_out = 0;
    return BOXRIG_STATUS_LIMIT_EXCEEDED;
}

boxrig_status boxrig_terminal_register_compiled_text_recognizer(
    boxrig_terminal *terminal, boxrig_regex_program *program,
    boxrig_text_recognizer_scope scope,
    const boxrig_text_recognizer_limits *limits, uint64_t *recognizer_id_out)
{
    (void)terminal;
    (void)program;
    (void)scope;
    (void)limits;
    if (recognizer_id_out) *recognizer_id_out = 0;
    return BOXRIG_STATUS_LIMIT_EXCEEDED;
}

boxrig_status boxrig_terminal_unregister_text_recognizer(
    boxrig_terminal *terminal,
    uint64_t recognizer_id)
{
    (void)terminal;
    (void)recognizer_id;
    return BOXRIG_STATUS_INVALID_ARGUMENT;
}

size_t boxrig_terminal_text_recognizer_count(
    const boxrig_terminal *terminal)
{
    (void)terminal;
    return 0;
}

boxrig_status boxrig_terminal_resume_text_recognizers(
    boxrig_terminal *terminal,
    size_t work_budget,
    size_t *work_used_out,
    bool *more_work_out)
{
    (void)work_budget;
    if (!terminal || !work_used_out || !more_work_out) return BOXRIG_STATUS_INVALID_ARGUMENT;
    *work_used_out = 0;
    *more_work_out = false;
    return BOXRIG_STATUS_OK;
}

bool boxrig_terminal_text_recognizer_event_peek(
    const boxrig_terminal *terminal,
    boxrig_text_recognizer_event *event_out)
{
    (void)terminal;
    (void)event_out;
    return false;
}

boxrig_status boxrig_terminal_text_recognizer_event_consume(
    boxrig_terminal *terminal)
{
    (void)terminal;
    return BOXRIG_STATUS_INVALID_ARGUMENT;
}

boxrig_status boxrig_terminal_query_text_recognizer_line_results(
    const boxrig_terminal *terminal,
    uint64_t recognizer_id,
    boxrig_text_screen screen,
    boxrig_text_line_id line_id,
    uint64_t line_revision,
    uint64_t result_set_revision,
    boxrig_text_recognition_result *results,
    size_t capacity,
    size_t *required_count_out)
{
    (void)terminal;
    (void)recognizer_id;
    (void)screen;
    (void)line_id;
    (void)line_revision;
    (void)result_set_revision;
    (void)results;
    (void)capacity;
    if (required_count_out) *required_count_out = 0;
    return BOXRIG_STATUS_INVALID_ARGUMENT;
}

boxrig_status boxrig_terminal_text_recognizer_result(
    const boxrig_terminal *terminal, uint64_t recognizer_id, uint64_t match_id,
    uint64_t result_set_revision, boxrig_regex_result **result_out)
{
    (void)terminal;
    (void)recognizer_id;
    (void)match_id;
    (void)result_set_revision;
    (void)result_out;
    return BOXRIG_STATUS_INVALID_ARGUMENT;
}

boxrig_status boxrig_terminal_query_recognition_hits(
    const boxrig_terminal *terminal,
    const boxrig_text_range *grapheme_range,
    uint64_t recognizer_id_filter,
    boxrig_text_recognition_result *results,
    size_t capacity,
    size_t *required_count_out)
{
    (void)recognizer_id_filter;
    (void)results;
    (void)capacity;
    if (!terminal || !grapheme_range || !required_count_out) return BOXRIG_STATUS_INVALID_ARGUMENT;
    *required_count_out = 0;
    return BOXRIG_STATUS_OK;
}

bool boxrig_terminal_text_recognizer_needs_rescan(
    const boxrig_terminal *terminal,
    uint64_t recognizer_id)
{
    (void)terminal;
    (void)recognizer_id;
    return false;
}

boxrig_status boxrig_terminal_rescan_text_recognizer(
    boxrig_terminal *terminal,
    uint64_t recognizer_id)
{
    (void)terminal;
    (void)recognizer_id;
    return BOXRIG_STATUS_INVALID_ARGUMENT;
}
#endif

#if !BOXRIG_FEATURE_KITTY_GRAPHICS
uint64_t boxrig_terminal_image_revision(const boxrig_terminal *terminal)
{
    (void)terminal;
    return 0;
}

size_t boxrig_terminal_image_count(const boxrig_terminal *terminal)
{
    (void)terminal;
    return 0;
}

boxrig_status boxrig_terminal_image_handle_at(
    const boxrig_terminal *terminal,
    size_t index,
    boxrig_image_handle *handle_out)
{
    (void)terminal;
    (void)index;
    (void)handle_out;
    return BOXRIG_STATUS_INVALID_ARGUMENT;
}

boxrig_status boxrig_terminal_get_image(
    const boxrig_terminal *terminal,
    boxrig_image_handle handle,
    boxrig_image_view *image_out)
{
    (void)terminal;
    (void)handle;
    (void)image_out;
    return BOXRIG_STATUS_INVALID_ARGUMENT;
}

size_t boxrig_terminal_image_placement_count(
    const boxrig_terminal *terminal)
{
    (void)terminal;
    return 0;
}

boxrig_status boxrig_terminal_get_image_placement(
    const boxrig_terminal *terminal,
    size_t index,
    boxrig_image_placement_view *placement_out)
{
    (void)terminal;
    (void)index;
    (void)placement_out;
    return BOXRIG_STATUS_INVALID_ARGUMENT;
}

bool boxrig_terminal_take_image_dirty(
    boxrig_terminal *terminal,
    boxrig_dirty *dirty_out)
{
    (void)terminal;
    (void)dirty_out;
    return false;
}
#endif

#if !BOXRIG_FEATURE_CHECKPOINT
boxrig_status boxrig_terminal_export_checkpoint(const boxrig_terminal *terminal,
    size_t maximum_bytes, uint8_t **data_out, size_t *length_out)
{
    (void)terminal;
    (void)maximum_bytes;
    if (data_out) *data_out = 0;
    if (length_out) *length_out = 0;
    return BOXRIG_STATUS_LIMIT_EXCEEDED;
}

boxrig_status boxrig_terminal_import_checkpoint(const uint8_t *data, size_t length,
    size_t maximum_bytes, boxrig_terminal **out)
{
    (void)data;
    (void)length;
    (void)maximum_bytes;
    if (out) *out = 0;
    return BOXRIG_STATUS_LIMIT_EXCEEDED;
}

void boxrig_checkpoint_free(uint8_t *data)
{
    free(data);
}
#endif

#if !BOXRIG_FEATURE_KITTY_GRAPHICS
/* Clocks and cell geometry also serve synchronized text and window reports. */
boxrig_status boxrig_update_monotonic_time(boxrig_terminal *t, uint64_t milliseconds)
{
    if (!t || (t->graphics_protocol.monotonic_time_set &&
        milliseconds < t->graphics_protocol.monotonic_time_milliseconds)) return BOXRIG_STATUS_INVALID_ARGUMENT;
    t->graphics_protocol.monotonic_time_set = true;
    t->graphics_protocol.monotonic_time_milliseconds = milliseconds;
    boxrig_screen_update_synchronized_output_time(&t->screen, milliseconds);
    return BOXRIG_STATUS_OK;
}

bool boxrig_terminal_next_animation_deadline(const boxrig_terminal *t, uint64_t *deadline)
{
    if (!t || !deadline || !t->screen.synchronized_output_deadline_set || !t->screen.synchronized_output) return false;
    *deadline = t->screen.synchronized_output_deadline_milliseconds;
    return true;
}

boxrig_status boxrig_terminal_validate_cell_pixel_geometry(const boxrig_terminal *t, uint32_t width, uint32_t height)
{
    if (!t || !width || !height || t->screen.columns > SIZE_MAX / width ||
        t->screen.rows > SIZE_MAX / height) return BOXRIG_STATUS_INVALID_ARGUMENT;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_set_cell_pixel_geometry(boxrig_terminal *t, uint32_t width, uint32_t height)
{
    boxrig_status status = boxrig_terminal_validate_cell_pixel_geometry(t, width, height);
    if (status == BOXRIG_STATUS_OK) {
        t->graphics_protocol.cell_pixel_width = width;
        t->graphics_protocol.cell_pixel_height = height;
    }
    return status;
}
#endif
