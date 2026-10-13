/* Private presentation boundary. Input, fonts and geometry remain in the component. */
#ifndef BOXRIG_WIN32_COMPONENT_PRIVATE_H
#define BOXRIG_WIN32_COMPONENT_PRIVATE_H
#include "boxrig/win32/component.h"
#include "boxrig/win32/renderer.h"
#include "renderer_private.h"

typedef struct boxrig_win32_component_presentation {
    int (*resize)(void *, unsigned int, unsigned int);
    int (*size)(void *, SIZE *);
    int (*prepare)(void *, unsigned int, unsigned int);
    int (*commit)(void *);
    void (*abort)(void *);
    /* Returns 0 on completion, 1 when native activity must unblock retained
     * presentation, and -1 on failure. Deferred drawing preserves view dirty. */
    int (*draw)(void *, boxrig_ui_view *, boxrig_composition *);
    int (*paint)(void *);
    int (*geometry)(void *, int, int, unsigned int, unsigned int);
    void (*visible)(void *, bool);
    int (*scrollbar)(void *, bool, unsigned int);
    int (*interaction)(void *, bool, bool);
    void (*invalidate)(void *);
    void (*direction)(void *, boxrig_text_direction_policy);
    void (*destroy)(void *);
    void (*transparency)(void *, boxrig_ui_transparency_capabilities *);
    HANDLE (*activity)(void *);
    void (*dispatch_activity)(void *);
} boxrig_win32_component_presentation;

typedef boxrig_ui_status (*boxrig_win32_component_renderer_factory)(
    HWND, HWND, boxrig_win32_renderer *, void *, void **,
    const boxrig_win32_component_presentation **);

/* A renderer factory is part of the same rollback transaction as the input target. */
boxrig_ui_status boxrig_win32_terminal_component_create_child_with_renderer(
    HINSTANCE, HWND, boxrig_ui_model *,
    const boxrig_win32_terminal_component_config *, DWORD,
    boxrig_win32_component_renderer_factory, void *,
    boxrig_win32_terminal_component **);
boxrig_ui_status boxrig_win32_terminal_component_create_on_target_with_renderer(
    HINSTANCE, const boxrig_win32_terminal_borrowed_target *, boxrig_ui_model *,
    const boxrig_win32_terminal_component_config *,
    boxrig_win32_component_renderer_factory, void *,
    boxrig_win32_terminal_component **);
void *boxrig_win32_terminal_component_presentation_context(
    boxrig_win32_terminal_component *);
bool boxrig_win32_terminal_component_has_presentation(
    boxrig_win32_terminal_component *, const boxrig_win32_component_presentation *);
/* Accepts the borrowed observer only for a software-rendered component. */
bool boxrig_win32_terminal_component_set_renderer_phase_observer(
    boxrig_win32_terminal_component *,
    const boxrig_win32_renderer_phase_observer *);
/** Starts aggregate work counting for a software-rendered component. */
bool boxrig_win32_terminal_component_enable_renderer_work_statistics(
    boxrig_win32_terminal_component *);
/** Reads aggregate work counting for a software-rendered component. */
bool boxrig_win32_terminal_component_get_renderer_work_statistics(
    const boxrig_win32_terminal_component *,
    boxrig_win32_render_statistics *);
#endif
