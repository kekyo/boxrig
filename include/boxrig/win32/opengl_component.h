/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#ifndef BOXRIG_WIN32_OPENGL_COMPONENT_H
#define BOXRIG_WIN32_OPENGL_COMPONENT_H
#include "component.h"
#include "wgl.h"

/** Bounded diagnostics for the last OpenGL component operation. */
typedef struct boxrig_win32_opengl_diagnostics {
    /** Most recent successful context probe, retained after recreation failure. */
    boxrig_wgl_surface_info surface;
    /** Generation of the most recently attempted framebuffer transaction. */
    uint64_t target_generation;
    /** Frames successfully presented and accepted by the renderer lifecycle.
     * This counter survives context recreation and excludes rejected swaps. */
    uint64_t accepted_frames;
    /** Whether graphics are currently usable without recreation. */
    bool available;
    /** Whether setting the optional swap interval actually succeeded. */
    bool swap_interval_set;
    /** Latest graphics operation status; one message of at most 511 bytes. */
    boxrig_gl_diagnostic last_operation;
} boxrig_win32_opengl_diagnostics;

/**
 * Copies graphics diagnostics without making any context current.
 * @param component OpenGL component on its creation thread.
 * @param diagnostics_out Receives the latest fixed-size snapshot.
 * @return UI status; a software component or wrong thread is rejected.
 * @remarks A failed frame retains dirty and returns unavailable. The application
 * may retry recreation or explicitly destroy and recreate using the software
 * factory. This query does not retry, wait, allocate, or invoke callbacks.
 */
boxrig_ui_status boxrig_win32_opengl_terminal_component_get_diagnostics(
    boxrig_win32_terminal_component *component,
    boxrig_win32_opengl_diagnostics *diagnostics_out);

/**
 * Creates a Win32 component with a managed OpenGL ES or desktop OpenGL child.
 * @param instance Application instance.
 * @param parent Borrowed parent on the calling thread.
 * @param model Borrowed terminal model which outlives the component.
 * @param config Common component settings; alpha is available in VIEW_SURFACE scope.
 * @param component_out Receives the component, or NULL on failure.
 * @return UI status; unsupported graphics or transparency are unavailable.
 * @remarks Use ordinary component operations and destruction. The input HWND,
 * IMM, clipboard and scrollbar use the common Win32 implementation. The parent
 * must have a normal redirection surface. The owner prepares DWM or whole-window
 * alpha on its ordinary root explicitly; the component never changes ancestors.
 * Select COMPOSITED scrollbar presentation when preparing background alpha:
 * native GDI child controls do not preserve alpha under the DWM-prepared root.
 * Actual pixel-format alpha determines view capability. Missing alpha permits
 * preferred opaque rendering; a required unavailable scope is rejected.
 * Initialization prefers driver-provided ES 3.0, falling back to desktop GL 3.3
 * core if ES cannot initialize. No implicit software switch occurs after context
 * loss. All calls use this thread.
 */
boxrig_ui_status boxrig_win32_opengl_terminal_component_create_child(
    HINSTANCE instance, HWND parent, boxrig_ui_model *model,
    const boxrig_win32_terminal_component_config *config,
    boxrig_win32_terminal_component **component_out);

/**
 * Attaches a GPU child inside a borrowed presentation allocation.
 * @param instance Application instance.
 * @param target Borrowed presentation/input HWNDs; the HDC presenter is unused.
 * @param model Borrowed terminal model which outlives the component.
 * @param config Common settings; use no scrollbar or the composited scrollbar.
 * @param component_out Receives the component, or NULL on failure.
 * @return UI status; unsupported graphics or transparency are unavailable.
 * @remarks The application retains geometry, input dispatch and both HWNDs.
 * Observe target extent before allocating, as with the software component.
 * The adapter owns only its GPU child and resources; an alpha framebuffer does
 * not establish desktop composition without explicit host preparation. Request
 * VIEW_SURFACE scope after preparing the owner host. All calls use the creation thread.
 */
boxrig_ui_status boxrig_win32_opengl_terminal_component_create_on_target(
    HINSTANCE instance, const boxrig_win32_terminal_borrowed_target *target,
    boxrig_ui_model *model,
    const boxrig_win32_terminal_component_config *config,
    boxrig_win32_terminal_component **component_out);
#endif
