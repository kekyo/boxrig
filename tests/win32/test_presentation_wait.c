/* Modal sizing relays native activity without borrowing an event during work. */
#include "application_internal.h"
#include <assert.h>
#include <stdio.h>

static HANDLE component_event;
static unsigned int dispatched;

HANDLE boxrig_win32_terminal_component_activity_handle(boxrig_win32_terminal_component *component)
{
    (void)component;
    return component_event;
}

boxrig_ui_status boxrig_win32_terminal_component_dispatch_activity(boxrig_win32_terminal_component *component)
{
    (void)component;
    dispatched++;
    return BOXRIG_UI_OK;
}

int main(void)
{
    boxrig_term_win32_application app = {0};
    MSG message;
    app.window = CreateWindowExW(0, L"STATIC", L"presentation wait", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    assert(app.window != NULL);
    app.component = (boxrig_win32_terminal_component *)&app;
    component_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    assert(component_event != NULL);
    assert(boxrig_term_win32_resume_presentation_wait(&app) == 0);
    assert(app.presentation_wait == NULL);

    app.interactive_sizing = true;
    assert(boxrig_term_win32_resume_presentation_wait(&app) == 0);
    assert(app.presentation_wait != NULL);
    assert(boxrig_term_win32_begin_presentation_work(&app) == 0);
    assert(boxrig_term_win32_begin_presentation_work(&app) == 0);
    assert(app.presentation_wait == NULL && app.presentation_work_depth == 2);
    /* Resource replacement is safe even during a nested native callback. */
    assert(CloseHandle(component_event));
    component_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    assert(component_event != NULL);
    assert(boxrig_term_win32_end_presentation_work(&app) == 0);
    assert(app.presentation_wait == NULL);
    assert(boxrig_term_win32_end_presentation_work(&app) == 0);
    assert(app.presentation_wait != NULL);
    assert(SetEvent(component_event));
    assert(GetMessageW(&message, app.window, BOXRIG_TERM_PRESENTATION_READY,
        BOXRIG_TERM_PRESENTATION_READY) > 0);
    assert(dispatched == 0); /* Callback posts; only the owner dispatches. */
    assert(boxrig_term_win32_suspend_presentation_wait(&app) == 0);
    assert(app.presentation_wait == NULL && dispatched == 1);
    assert(boxrig_term_win32_suspend_presentation_wait(&app) == 0 && dispatched == 1);

    assert(boxrig_term_win32_resume_presentation_wait(&app) == 0);
    assert(app.presentation_wait != NULL);
    app.interactive_sizing = false;
    assert(boxrig_term_win32_suspend_presentation_wait(&app) == 0);
    assert(boxrig_term_win32_resume_presentation_wait(&app) == 0);
    assert(app.presentation_wait == NULL);
    assert(SetEvent(component_event));
    assert(WaitForSingleObject(component_event, 0) == WAIT_OBJECT_0);
    assert(dispatched == 1);
    assert(CloseHandle(component_event));
    assert(DestroyWindow(app.window));
    puts("PASS modal activity wakes the owner once and releases borrowed handles before nested work");
    return 0;
}
