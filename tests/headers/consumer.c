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
#elif defined(BOXRIG_HEADER_GTK4)
    boxrig_gtk4_terminal_widget_config widget =
        boxrig_gtk4_terminal_widget_config_default();
    assert(widget.paste_capacity > 0);
#elif defined(_WIN32)
    boxrig_win32_pty_config pty = boxrig_win32_pty_config_default();
    boxrig_win32_host_policy policy = boxrig_win32_host_policy_default();
    uint64_t now;
    assert(boxrig_win32_sample_monotonic_time(&now) == 0);
    (void)pty;
    (void)policy;
#else
    boxrig_posix_pty_input_batch batch;
    boxrig_posix_pty_input_batch_initialize(&batch);
    assert(!boxrig_posix_pty_input_batch_pending(&batch));
    boxrig_x11_host_policy policy = boxrig_x11_host_policy_default();
    boxrig_wayland_context_config wayland = boxrig_wayland_context_config_default();
    boxrig_ogl_renderer_config renderer = boxrig_ogl_renderer_config_default();
    (void)policy;
    (void)wayland;
    (void)renderer;
#endif

    boxrig_terminal_destroy(terminal);
    return 0;
}
