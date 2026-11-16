/* Link-time observation of the existing GTK dispatch, parser and recognizer. */
#define _POSIX_C_SOURCE 200809L
#include "boxrig/session.h"
#include "terminal_internal.h"
#include "regex.h"
#include "interval_trace.h"
#include "../../src/gtk/gl_surface.h"

#include <errno.h>
#include <unistd.h>

static bool inside_session;

ssize_t __real_read(int descriptor, void *buffer, size_t capacity);
ssize_t __wrap_read(int descriptor, void *buffer, size_t capacity)
{
    if (!inside_session) return __real_read(descriptor, buffer, capacity);
    boxrig_interval_start start = boxrig_interval_begin();
    ssize_t result = __real_read(descriptor, buffer, capacity);
    int saved_errno = errno;
    boxrig_interval_end("read", start, result > 0 ? (size_t)result : 0u,
        0u, result < 0 ? saved_errno : 0, false);
    errno = saved_errno;
    return result;
}

boxrig_session_status __real_boxrig_session_dispatch(boxrig_session *session,
    uint32_t ready, boxrig_session_dispatch_result *result_out);
boxrig_session_status __wrap_boxrig_session_dispatch(boxrig_session *session,
    uint32_t ready, boxrig_session_dispatch_result *result_out)
{
    boxrig_interval_start start = boxrig_interval_begin();
    inside_session = true;
    boxrig_session_status status = __real_boxrig_session_dispatch(session, ready, result_out);
    inside_session = false;
    boxrig_interval_end("session", start, result_out->transport_bytes_read,
        0u, status, result_out->core_blocked);
    return status;
}

boxrig_status __real_boxrig_terminal_feed(boxrig_terminal *terminal,
    const uint8_t *bytes, size_t length, size_t *consumed_out);
boxrig_status __wrap_boxrig_terminal_feed(boxrig_terminal *terminal,
    const uint8_t *bytes, size_t length, size_t *consumed_out)
{
    boxrig_interval_start start = boxrig_interval_begin();
    boxrig_status status = __real_boxrig_terminal_feed(terminal, bytes, length, consumed_out);
    boxrig_interval_end("feed", start, *consumed_out,
        boxrig_terminal_text_revision(terminal), status, false);
    boxrig_interval_end("image_state", boxrig_interval_begin(),
        boxrig_terminal_image_count(terminal), terminal->graphics_protocol.storage_usage,
        0, boxrig_terminal_image_placement_count(terminal) != 0);
    return status;
}

boxrig_ui_status __real_boxrig_ui_model_synchronize(boxrig_ui_model *model);
boxrig_ui_status __wrap_boxrig_ui_model_synchronize(boxrig_ui_model *model)
{
    boxrig_interval_start start = boxrig_interval_begin();
    boxrig_ui_status status = __real_boxrig_ui_model_synchronize(model);
    boxrig_interval_end("model", start, 1, 0, status, false);
    return status;
}

boxrig_ui_status __real_boxrig_gtk_gl_surface_draw(boxrig_gtk_gl_surface *surface,
    boxrig_ui_view *view, boxrig_composition *composition, boxrig_gtk_render_state *state);
boxrig_ui_status __wrap_boxrig_gtk_gl_surface_draw(boxrig_gtk_gl_surface *surface,
    boxrig_ui_view *view, boxrig_composition *composition, boxrig_gtk_render_state *state)
{
    boxrig_interval_start start = boxrig_interval_begin();
    boxrig_ui_status status = __real_boxrig_gtk_gl_surface_draw(surface, view, composition, state);
    boxrig_interval_end("gl_draw", start, 1, 0, status, false);
    boxrig_ogl_render_statistics statistics;
    if (boxrig_gtk_gl_surface_get_statistics(surface, &statistics) == 0)
        boxrig_interval_end("gl_uploads", boxrig_interval_begin(),
            statistics.image_cache_upload_count, statistics.upload_bytes, 0, false);
    return status;
}

boxrig_ui_status __real_boxrig_gtk_gl_surface_present(boxrig_gtk_gl_surface *surface,
    const boxrig_gl_target *destination, boxrig_gtk_render_state *state);
boxrig_ui_status __wrap_boxrig_gtk_gl_surface_present(boxrig_gtk_gl_surface *surface,
    const boxrig_gl_target *destination, boxrig_gtk_render_state *state)
{
    boxrig_interval_start start = boxrig_interval_begin();
    boxrig_ui_status status = __real_boxrig_gtk_gl_surface_present(surface, destination, state);
    /* Submission is separate from the external probe's visible-pixel time. */
    boxrig_interval_end("gl_present", start, 1, 0, status, false);
    return status;
}

boxrig_status __real_boxrig_terminal_resume_text_recognizers(boxrig_terminal *terminal,
    size_t budget, size_t *used_out, bool *more_out);
boxrig_status __wrap_boxrig_terminal_resume_text_recognizers(boxrig_terminal *terminal,
    size_t budget, size_t *used_out, bool *more_out)
{
    boxrig_interval_start start = boxrig_interval_begin();
    boxrig_status status = __real_boxrig_terminal_resume_text_recognizers(
        terminal, budget, used_out, more_out);
    boxrig_interval_end("core_recognition", start, *used_out,
        boxrig_terminal_text_revision(terminal), status, *more_out);
    return status;
}

boxrig_status __real_boxrig_text_line_projection_cursor_resume(
    boxrig_text_line_projection_cursor *cursor, const boxrig_terminal *terminal,
    size_t budget, boxrig_text_snapshot **snapshot, size_t *used_out,
    bool *complete_out);
boxrig_status __wrap_boxrig_text_line_projection_cursor_resume(
    boxrig_text_line_projection_cursor *cursor, const boxrig_terminal *terminal,
    size_t budget, boxrig_text_snapshot **snapshot, size_t *used_out,
    bool *complete_out)
{
    boxrig_interval_start start = boxrig_interval_begin();
    boxrig_status status = __real_boxrig_text_line_projection_cursor_resume(
        cursor, terminal, budget, snapshot, used_out, complete_out);
    boxrig_interval_end("projection", start, *used_out,
        boxrig_terminal_text_revision(terminal), status, !*complete_out);
    return status;
}

boxrig_status __real_boxrig_regex_cursor_resume(boxrig_regex_cursor *cursor,
    size_t budget, size_t *used_out, boxrig_regex_progress *progress_out,
    boxrig_regex_match *match_out);
boxrig_status __wrap_boxrig_regex_cursor_resume(boxrig_regex_cursor *cursor,
    size_t budget, size_t *used_out, boxrig_regex_progress *progress_out,
    boxrig_regex_match *match_out)
{
    boxrig_interval_start start = boxrig_interval_begin();
    boxrig_status status = __real_boxrig_regex_cursor_resume(
        cursor, budget, used_out, progress_out, match_out);
    boxrig_interval_end("regex", start, *used_out, 0u, status, false);
    return status;
}

boxrig_status __real_boxrig_regex_cursor_create_result(const boxrig_regex_cursor *cursor,
    const uint8_t *bytes, const size_t *offsets, boxrig_regex_result **result_out);
boxrig_status __wrap_boxrig_regex_cursor_create_result(const boxrig_regex_cursor *cursor,
    const uint8_t *bytes, const size_t *offsets, boxrig_regex_result **result_out)
{
    boxrig_interval_start start = boxrig_interval_begin();
    boxrig_status status = __real_boxrig_regex_cursor_create_result(cursor, bytes, offsets, result_out);
    boxrig_interval_end("capture", start, 1u, 0u, status, false);
    return status;
}
