/* Relay presentation activity into Win32's modal sizing message loop. */
#include "application_internal.h"

static VOID CALLBACK presentation_ready(void *context, BOOLEAN timed_out)
{
    boxrig_term_win32_application *application = context;
    (void)timed_out; /* The registered wait has no timeout. */
    InterlockedExchange(&application->presentation_ready, 1);
    /* Never call the component or synchronously send to its owning thread. */
    (void)PostMessageW(application->window, BOXRIG_TERM_PRESENTATION_READY, 0, 0);
}

int boxrig_term_win32_suspend_presentation_wait(boxrig_term_win32_application *application)
{
    if (application->presentation_work_depth != 0) return 0;
    if (application->presentation_wait != NULL) {
        /* Join only the short notification callback, never GPU work. This
         * must precede any operation that can destroy the borrowed event. */
        if (!UnregisterWaitEx(application->presentation_wait, INVALID_HANDLE_VALUE)) return -1;
        application->presentation_wait = NULL;
    }
    if (InterlockedExchange(&application->presentation_ready, 0) != 0 &&
        application->component != NULL &&
        boxrig_win32_terminal_component_dispatch_activity(application->component) != BOXRIG_UI_OK)
        return -1;
    return 0;
}

int boxrig_term_win32_resume_presentation_wait(boxrig_term_win32_application *application)
{
    HANDLE event;
    HANDLE wait = NULL;
    if (application->presentation_work_depth != 0 || !application->interactive_sizing ||
        application->window_closed || application->window == NULL ||
        application->component == NULL || application->presentation_wait != NULL) return 0;
    event = boxrig_win32_terminal_component_activity_handle(application->component);
    if (event == NULL) return 0;
    if (!RegisterWaitForSingleObject(&wait, event,
        presentation_ready, application, INFINITE, WT_EXECUTEONLYONCE)) return -1;
    application->presentation_wait = wait;
    return 0;
}

int boxrig_term_win32_begin_presentation_work(boxrig_term_win32_application *application)
{
    if (boxrig_term_win32_suspend_presentation_wait(application) == -1) return -1;
    application->presentation_work_depth++;
    return 0;
}

int boxrig_term_win32_end_presentation_work(boxrig_term_win32_application *application)
{
    application->presentation_work_depth--;
    return boxrig_term_win32_resume_presentation_wait(application);
}
