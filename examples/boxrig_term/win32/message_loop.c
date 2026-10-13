/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "application_internal.h"

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static boxrig_session_status traced_session_dispatch(
    boxrig_term_win32_application *application,
    boxrig_session_dispatch_result *result)
{
    boxrig_term_win32_phase_scope scope = boxrig_term_win32_phase_trace_begin(
        &application->phase_trace);
    boxrig_session_status status = boxrig_win32_pty_session_dispatch(
        application->session, result);
    boxrig_term_win32_phase_trace_end(&application->phase_trace,
        BOXRIG_TERM_WIN32_PHASE_SESSION_DISPATCH, scope);
    return status;
}

static int traced_host_items(boxrig_term_win32_application *application)
{
    boxrig_term_win32_phase_scope scope = boxrig_term_win32_phase_trace_begin(
        &application->phase_trace);
    int result = boxrig_win32_host_adapter_process_items(application->host_adapter);
    boxrig_term_win32_phase_trace_end(&application->phase_trace,
        BOXRIG_TERM_WIN32_PHASE_HOST_ITEMS, scope);
    return result;
}

static boxrig_ui_status traced_model_synchronize(
    boxrig_term_win32_application *application)
{
    boxrig_term_win32_phase_scope scope = boxrig_term_win32_phase_trace_begin(
        &application->phase_trace);
    boxrig_ui_status status = boxrig_ui_model_synchronize(application->model);
    boxrig_term_win32_phase_trace_end(&application->phase_trace,
        BOXRIG_TERM_WIN32_PHASE_MODEL_RECOGNITION, scope);
    return status;
}

static int traced_text_policy_resume(
    boxrig_term_win32_application *application,
    size_t *work, bool *more)
{
    boxrig_term_win32_phase_scope scope = boxrig_term_win32_phase_trace_begin(
        &application->phase_trace);
    int result = boxrig_term_text_policy_resume(application->text_policy,
        BOXRIG_TERM_TEXT_POLICY_WORK_BUDGET_DEFAULT, work, more);
    boxrig_term_win32_phase_trace_end(&application->phase_trace,
        BOXRIG_TERM_WIN32_PHASE_MODEL_RECOGNITION, scope);
    return result;
}

static boxrig_ui_status traced_component_frame(
    boxrig_term_win32_application *application,
    boxrig_ui_component_frame_result *result)
{
    boxrig_term_win32_phase_scope scope = boxrig_term_win32_phase_trace_begin(
        &application->phase_trace);
    boxrig_ui_status status = boxrig_win32_terminal_component_frame(
        application->component, result);
    boxrig_term_win32_phase_trace_end(&application->phase_trace,
        BOXRIG_TERM_WIN32_PHASE_COMPONENT_FRAME, scope);
    return status;
}

static bool component_input_blocked(
    boxrig_term_win32_application *application)
{
    boxrig_ui_component_diagnostic diagnostic;

    return boxrig_win32_terminal_component_get_diagnostic(
               application->component,
               &diagnostic) == BOXRIG_UI_OK &&
           diagnostic.input_blocked;
}

static bool content_size_deferred(
    boxrig_term_win32_application *application)
{
    boxrig_ui_size_command_state state;

    return application->interactive_sizing &&
           boxrig_win32_terminal_component_get_size_command_state(
               application->component, &state) == BOXRIG_UI_OK &&
           state != BOXRIG_UI_SIZE_COMMAND_IDLE;
}

static int complete_pending_layout(boxrig_term_win32_application *application)
{
    size_t iteration;
    /* Completing a content-size command can queue its grid resize. Advance
     * both before consuming PTY replies against the newly allocated view. */
    for (iteration = 0; iteration < 64; iteration++) {
        int result = boxrig_term_win32_native_host_process_layout_action(application);
        if (result <= 0) return result;
    }
    return 1;
}

static int frame_component(
    boxrig_term_win32_application *application)
{
    boxrig_ui_component_frame_result frame;
    size_t recognition_work;
    bool recognition_more;
    int fence_result;
    int opacity_result;
    int layout_result;
    boxrig_ui_status frame_status;

    if (application->opacity_applying) return 0;

    layout_result = complete_pending_layout(application);
    if (layout_result != 0) return layout_result;
    if (traced_model_synchronize(application) != BOXRIG_UI_OK ||
        traced_text_policy_resume(
            application,
            &recognition_work,
            &recognition_more) == -1) {
        return -1;
    }
    if (application->first_frame_fence_enabled) {
        fence_result = boxrig_term_win32_first_frame_fence_before_frame(
            &application->first_frame_fence);
        if (fence_result != 0) return fence_result == 1 ? 0 : -1;
    }
    if (IsIconic(application->window)) {
        return 0;
    }
    if (boxrig_term_scrollbar_demo_apply(&application->scrollbar_demo) == -1) return -1;
    if (boxrig_term_win32_opacity_prepare(application) == -1) return -1;
    frame_status = traced_component_frame(application, &frame);
    opacity_result = boxrig_term_win32_opacity_finish(application, frame_status == BOXRIG_UI_OK);
    if (opacity_result != 0) return opacity_result;
    if (application->presentation_fallback_required) {
        application->presentation_fallback_required = false;
        if (boxrig_term_win32_native_host_synchronize_transparency(
                application) == -1 ||
            traced_model_synchronize(application) != BOXRIG_UI_OK ||
            traced_component_frame(application, &frame) != BOXRIG_UI_OK) {
            return -1;
        }
    }
    if (!application->external_presentation) {
        (void)UpdateWindow(
            boxrig_win32_terminal_component_owned_window(
                application->component));
    }
    int pointer_changed = boxrig_term_win32_native_host_update_pointer(application);
    if (pointer_changed < 0) return -1;
    /* A native modal sizing loop cannot execute content-size actions. Leave
     * only that revision dormant; allocation-originated grid actions and
     * presentation work must continue while the pointer remains captured. */
    return ((!content_size_deferred(application) &&
             frame.additional_dispatch) ||
            recognition_more || pointer_changed != 0)
               ? 1
               : 0;
}

int boxrig_term_win32_frame_component(boxrig_term_win32_application *application)
{
    int result;
    if (boxrig_term_win32_begin_presentation_work(application) == -1) return -1;
    result = frame_component(application);
    if (boxrig_term_win32_end_presentation_work(application) == -1) return -1;
    return result;
}

static int service_session(
    boxrig_term_win32_application *application)
{
    size_t iteration;
    int layout_result = complete_pending_layout(application);

    if (layout_result != 0) return layout_result;

    for (iteration = 0; iteration < 64; iteration++) {
        boxrig_session_dispatch_result dispatch;
        boxrig_host_item item;
        bool had_host_item;
        bool input_was_blocked;

        if (traced_session_dispatch(application, &dispatch) != BOXRIG_SESSION_OK) {
            return -1;
        }
        had_host_item = boxrig_terminal_host_item_peek(
            application->terminal,
            &item);
        input_was_blocked = component_input_blocked(application);
        if (traced_host_items(application) == -1) {
            return -1;
        }
        if (!had_host_item &&
            (!input_was_blocked ||
             dispatch.transport_bytes_written == 0)) {
            break;
        }
    }
    return boxrig_term_win32_frame_component(application);
}

int boxrig_term_win32_run_message_loop(
    boxrig_term_win32_application *application)
{
    HANDLE wait_handles[4] = {
        boxrig_win32_pty_process_handle(application->pty),
        boxrig_win32_pty_session_activity_handle(application->session),
    };

    for (;;) {
        uint32_t interests;
        DWORD timeout_milliseconds = INFINITE;
        DWORD wait_result;
        DWORD wake_mask;
        int frame_result;
        bool input_blocked;
        DWORD wait_count;
        DWORD component_index;
        DWORD fence_index;
        HANDLE fence_wait;
        bool fence_held;

        frame_result = service_session(application);
        if (frame_result == -1) {
            return -1;
        }
        interests = boxrig_win32_pty_session_interests(
            application->session);
        input_blocked = component_input_blocked(application);
        if (input_blocked &&
            (interests & BOXRIG_SESSION_INTEREST_WRITE) == 0) {
            return -1;
        }
        if ((interests & BOXRIG_SESSION_INTEREST_DISPATCH) != 0 &&
            (interests & BOXRIG_SESSION_INTEREST_WRITE) == 0) {
            continue;
        }
        if (frame_result == 1) {
            continue;
        }
        /* Finish native transitions before resolving a single click. Inspect
         * queue status without dispatching synchronous messages: input received
         * here would otherwise remain unsent while MsgWait sleeps. Observed
         * messages no longer wake MsgWait, so service them after a zero-time wait. */
        bool messages_pending = !input_blocked &&
            HIWORD(GetQueueStatus(QS_ALLINPUT)) != 0;
        uint64_t now = GetTickCount64();
        if (!input_blocked && !messages_pending &&
            boxrig_term_link_gesture_dispatch(application->link_gesture, now) != 0 && application->error != NULL)
            fprintf(application->error, "boxrig_term: could not confirm URL click.\n");
        uint64_t link_deadline;
        if (!input_blocked && boxrig_term_link_gesture_deadline(application->link_gesture, &link_deadline))
            timeout_milliseconds = link_deadline <= now ? 0 : link_deadline - now >= INFINITE
                ? INFINITE - 1 : (DWORD)(link_deadline - now);
        if (messages_pending) timeout_milliseconds = 0;
        wake_mask = input_blocked ? QS_POSTMESSAGE | QS_TIMER : QS_ALLINPUT;
        fence_wait = application->first_frame_fence_enabled
            ? boxrig_term_win32_first_frame_fence_wait_handle(
                &application->first_frame_fence)
            : NULL;
        fence_held = application->first_frame_fence_enabled &&
            fence_wait == application->first_frame_fence.release;
        wait_count = 2;
        component_index = 4;
        fence_index = 4;
        if (!fence_held) {
            HANDLE component_wait =
                boxrig_win32_terminal_component_activity_handle(
                    application->component);
            if (component_wait != NULL) {
                component_index = wait_count;
                wait_handles[wait_count++] = component_wait;
            }
        }
        if (fence_wait != NULL) {
            fence_index = wait_count;
            wait_handles[wait_count++] = fence_wait;
        }
        if (fence_held &&
            (timeout_milliseconds == INFINITE || timeout_milliseconds > 5000)) {
            timeout_milliseconds = 5000;
        }
        wait_result = MsgWaitForMultipleObjects(
            wait_count,
            wait_handles,
            FALSE,
            timeout_milliseconds,
            wake_mask);
        if (wait_result == WAIT_OBJECT_0) {
            if (!GetExitCodeProcess(
                    wait_handles[0],
                    &application->child_exit_code)) {
                return -1;
            }
            application->child_finished = true;
            return 0;
        }
        if (wait_result == WAIT_OBJECT_0 + 1 ||
            (wait_result == WAIT_TIMEOUT && !messages_pending)) {
            continue;
        }
        if (wait_result == WAIT_OBJECT_0 + component_index) {
            if (boxrig_win32_terminal_component_dispatch_activity(application->component) != BOXRIG_UI_OK)
                return -1;
        } else if (wait_result == WAIT_OBJECT_0 + fence_index) {
            continue;
        } else if (wait_result != WAIT_OBJECT_0 + wait_count &&
                   !(wait_result == WAIT_TIMEOUT && messages_pending)) {
            return -1;
        }
        /* A presentation event must also service messages before rendering
         * again; a continuously ready swapchain must not starve input. Keep
         * the established PTY dispatch and synchronous-message batching order. */
        if (input_blocked) {
            MSG message;

            if (PeekMessageW(
                    &message,
                    NULL,
                    WM_QUIT,
                    WM_QUIT,
                    PM_REMOVE)) {
                return 0;
            }
            /* Animation delivery remains independent of terminal input
             * backpressure; the component requests the next ordinary frame. */
            while (PeekMessageW(&message, NULL, WM_TIMER, WM_TIMER, PM_REMOVE)) {
                (void)DispatchMessageW(&message);
            }
            continue;
        }
        {
            MSG message;

            while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
                boxrig_ui_component_event_result event_result;
                boxrig_term_win32_phase_scope scope;

                if (message.message == WM_QUIT) {
                    return 0;
                }
                scope = boxrig_term_win32_phase_trace_begin(
                    &application->phase_trace);
                event_result =
                    boxrig_win32_terminal_component_pretranslate_message(
                        application->component,
                        &message);
                if (event_result == BOXRIG_UI_COMPONENT_EVENT_FAILURE) {
                    boxrig_term_win32_phase_trace_end(&application->phase_trace,
                        BOXRIG_TERM_WIN32_PHASE_MESSAGE_DISPATCH, scope);
                    return -1;
                }
                if (event_result == BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
                    boxrig_term_win32_phase_trace_end(&application->phase_trace,
                        BOXRIG_TERM_WIN32_PHASE_MESSAGE_DISPATCH, scope);
                    continue;
                }
                (void)TranslateMessage(&message);
                (void)DispatchMessageW(&message);
                boxrig_term_win32_phase_trace_end(&application->phase_trace,
                    BOXRIG_TERM_WIN32_PHASE_MESSAGE_DISPATCH, scope);
                if (component_input_blocked(application)) {
                    break;
                }
            }
        }
    }
}
