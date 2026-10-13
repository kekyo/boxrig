/* Exercise owner policy changes in the actual CLI without adding product shortcuts. */
#include "../../examples/boxrig_term/win32/application_internal.h"
#include <assert.h>
#include <stdio.h>

static boxrig_input_key_router original_router;
static boxrig_term_win32_application *application;
static bool fail_frame;
static FILE *diagnostic_output;

int __real_boxrig_term_win32_run(const boxrig_term_win32_config *);

int __wrap_boxrig_term_win32_run(const boxrig_term_win32_config *config)
{
    /* Use the GUI entry point's explicitly preserved redirect, not CRT stdout. */
    diagnostic_output = config->output;
    return __real_boxrig_term_win32_run(config);
}

boxrig_ui_status __real_boxrig_win32_terminal_component_set_key_router(
    boxrig_win32_terminal_component *, boxrig_input_key_router, void *);
boxrig_ui_status __real_boxrig_win32_terminal_component_frame(
    boxrig_win32_terminal_component *, boxrig_ui_component_frame_result *);

static boxrig_input_key_route route(const boxrig_key_event *event, void *context)
{
    boxrig_term_win32_application *owner = context;
    if (event->key >= BOXRIG_KEY_F8 && event->key <= BOXRIG_KEY_F11) {
        if (event->action == BOXRIG_KEY_PRESS) {
            assert(owner->opacity_host != NULL);
            owner->opacity_config.mode = event->key == BOXRIG_KEY_F8
                ? BOXRIG_WIN32_OPACITY_BACKGROUND : event->key == BOXRIG_KEY_F10
                    ? BOXRIG_WIN32_OPACITY_DISABLED : BOXRIG_WIN32_OPACITY_WINDOW;
            owner->opacity_config.background.background_opacity = 128;
            owner->opacity_config.window_opacity = event->key == BOXRIG_KEY_F11 ? 64 : 128;
            fail_frame = event->key == BOXRIG_KEY_F11;
            owner->opacity_request_pending = true;
        }
        return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
    }
    return original_router(event, context);
}

boxrig_ui_status __wrap_boxrig_win32_terminal_component_set_key_router(
    boxrig_win32_terminal_component *component, boxrig_input_key_router router, void *context)
{
    assert(application == NULL && router != NULL && context != NULL);
    original_router = router;
    application = context;
    return __real_boxrig_win32_terminal_component_set_key_router(component, route, context);
}

boxrig_ui_status __wrap_boxrig_win32_terminal_component_frame(
    boxrig_win32_terminal_component *component, boxrig_ui_component_frame_result *frame)
{
    if (fail_frame && application != NULL && application->opacity_prepared) {
        fail_frame = false;
        assert(diagnostic_output != NULL);
        fprintf(diagnostic_output, "PROBE rejected candidate frame after owner preparation\n");
        fflush(diagnostic_output);
        return BOXRIG_UI_UNAVAILABLE;
    }
    return __real_boxrig_win32_terminal_component_frame(component, frame);
}
