/* Compile the same consumer as C99 and C++20, linking C-built archives. */
#include "boxrig/terminal.h"
#include "boxrig/ui.h"
#include "boxrig/session.h"

#if defined(BOXRIG_HEADER_GTK3)
#include "boxrig/gtk3.h"
#elif defined(BOXRIG_HEADER_GTK4)
#include "boxrig/gtk4.h"
#elif defined(_WIN32)
#include "boxrig/win32.h"
#else
#include "boxrig/posix.h"
#include "boxrig/x11.h"
#include "boxrig/wayland.h"
#include "boxrig/render/opengl.h"
#endif

#include <assert.h>

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    assert(terminal != NULL);
    boxrig_ui_view_config view = boxrig_ui_view_config_default();
    boxrig_session_config session = boxrig_session_config_default();
    assert(session.input_capacity > 0);
    (void)view;

#if defined(BOXRIG_HEADER_GTK3)
    boxrig_gtk3_terminal_widget_config widget =
        boxrig_gtk3_terminal_widget_config_default();
    assert(widget.paste_capacity > 0);
    boxrig_ui_input_state state;
    assert(boxrig_gtk3_terminal_widget_set_input_enabled(NULL, false) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(boxrig_gtk3_terminal_widget_get_input_state(NULL, &state) == BOXRIG_UI_INVALID_ARGUMENT);
#elif defined(BOXRIG_HEADER_GTK4)
    boxrig_gtk4_terminal_widget_config widget =
        boxrig_gtk4_terminal_widget_config_default();
    assert(widget.paste_capacity > 0);
    boxrig_ui_input_state state;
    assert(boxrig_gtk4_terminal_widget_set_input_enabled(NULL, false) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(boxrig_gtk4_terminal_widget_get_input_state(NULL, &state) == BOXRIG_UI_INVALID_ARGUMENT);
#elif defined(_WIN32)
    boxrig_win32_pty_config pty = boxrig_win32_pty_config_default();
    boxrig_win32_host_policy policy = boxrig_win32_host_policy_default();
    uint64_t now;
    assert(boxrig_win32_sample_monotonic_time(&now) == 0);
    boxrig_ui_input_state state;
    assert(boxrig_win32_input_adapter_set_input_enabled(NULL, false) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(boxrig_win32_input_adapter_get_input_state(NULL, &state) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(boxrig_win32_terminal_component_set_input_enabled(NULL, false) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(boxrig_win32_terminal_component_get_input_state(NULL, &state) == BOXRIG_UI_INVALID_ARGUMENT);
    (void)pty;
    (void)policy;
#else
    boxrig_posix_pty_input_batch batch;
    boxrig_posix_pty_input_batch_initialize(&batch);
    assert(!boxrig_posix_pty_input_batch_pending(&batch));
    boxrig_tty_erase unavailable;
    assert(boxrig_posix_get_tty_erase(-1, &unavailable) == -1);
    assert(!unavailable.available);
    boxrig_ui_input_state input_state;
    assert(boxrig_x11_input_adapter_set_input_enabled(NULL, false) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(boxrig_x11_input_adapter_get_input_state(NULL, &input_state) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(boxrig_x11_terminal_component_set_input_enabled(NULL, false) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(boxrig_x11_terminal_component_get_input_state(NULL, &input_state) == BOXRIG_UI_INVALID_ARGUMENT);
    boxrig_x11_host_policy policy = boxrig_x11_host_policy_default();
    boxrig_wayland_context_config wayland = boxrig_wayland_context_config_default();
    assert(boxrig_wayland_terminal_component_set_input_enabled(NULL, false) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(boxrig_wayland_terminal_component_get_input_state(NULL, &input_state) == BOXRIG_UI_INVALID_ARGUMENT);
    boxrig_ogl_renderer_config renderer = boxrig_ogl_renderer_config_default();
    (void)policy;
    (void)wayland;
    (void)renderer;
#endif

    boxrig_input_config input = boxrig_input_config_default();
    input.backspace_binding = BOXRIG_ERASE_ASCII_BACKSPACE;
    assert(boxrig_terminal_set_input_config(terminal, &input) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_get_input_config(terminal, &input) == BOXRIG_STATUS_OK);
    assert(input.backspace_binding == BOXRIG_ERASE_ASCII_BACKSPACE);
    boxrig_tty_erase erase = {true, 0};
    assert(boxrig_terminal_set_tty_erase(terminal, erase) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_get_tty_erase(terminal, &erase) == BOXRIG_STATUS_OK);
    assert(erase.available && erase.byte == 0);
    boxrig_terminal_destroy(terminal);
    boxrig_regex_limits limits = boxrig_regex_limits_default();
    boxrig_regex_program *program = NULL;
    boxrig_text_regex_diagnostic diagnostic;
    const uint8_t text[] = {'a'};
    assert(boxrig_regex_compile(text, sizeof(text), &limits, &diagnostic,
                                &program) == BOXRIG_STATUS_OK);
    boxrig_regex_matcher *matcher = NULL;
    size_t offset, used;
    assert(boxrig_regex_matcher_create(program, text, sizeof(text), 0,
                                       &offset, &matcher) == BOXRIG_STATUS_OK);
    boxrig_regex_program_destroy(program);
    boxrig_regex_progress progress;
    boxrig_regex_match match;
    assert(boxrig_regex_matcher_resume(matcher, 64, &used, &progress, &match) ==
           BOXRIG_STATUS_OK);
    assert(progress == BOXRIG_REGEX_MATCH && match.start == 0 && match.end == 1);
    boxrig_regex_result *result = NULL;
    assert(boxrig_regex_matcher_result(matcher, &result) == BOXRIG_STATUS_OK);
    boxrig_regex_matcher_destroy(matcher);
    boxrig_regex_capture capture;
    assert(boxrig_regex_result_get_capture(result, 0, &capture) == BOXRIG_STATUS_OK);
    assert(capture.participated && capture.end == 1);
    boxrig_regex_result_destroy(result);
    return 0;
}
