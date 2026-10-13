/* boxrig - Under MIT. */
#include "animation.h"

#include <windows.h>
#include <stdlib.h>
#include <wchar.h>

struct boxrig_win32_animation {
    DWORD thread;
    HWND window;
    HINSTANCE instance;
    wchar_t class_name[64];
    boxrig_ui_animation_runtime *runtime;
    UINT_PTR timer;
    uint64_t generation;
    size_t references;
    boxrig_win32_animation *next;
    void (*release)(boxrig_win32_animation *animation);
};

/* Only registry access and ID allocation cross thread boundaries. Native
 * dispatch, runtime operations and window destruction stay on the owner. */
static SRWLOCK registry_lock = SRWLOCK_INIT;
static boxrig_win32_animation *registry;
static UINT_PTR last_timer_id;

static void release_animation(boxrig_win32_animation *animation);

static boxrig_ui_status read_time(void *context, uint64_t *time_out)
{
    (void)context;
    *time_out = (uint64_t)GetTickCount64();
    return BOXRIG_UI_OK;
}

static void cancel_timer(void *context)
{
    boxrig_win32_animation *animation = context;
    UINT_PTR timer = animation->timer;
    animation->timer = 0;
    if (timer != 0) (void)KillTimer(animation->window, timer);
}

static boxrig_ui_status reserve_timer(
    void *context, uint64_t deadline, uint64_t generation)
{
    boxrig_win32_animation *animation = context;
    uint64_t now = (uint64_t)GetTickCount64();
    uint64_t difference = deadline > now ? deadline - now : 0;
    UINT delay = difference > USER_TIMER_MAXIMUM
                     ? USER_TIMER_MAXIMUM : (UINT)difference;
    if (delay < USER_TIMER_MINIMUM) delay = USER_TIMER_MINIMUM;
    AcquireSRWLockExclusive(&registry_lock);
    if (last_timer_id == UINTPTR_MAX) {
        ReleaseSRWLockExclusive(&registry_lock);
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    UINT_PTR timer = ++last_timer_id;
    ReleaseSRWLockExclusive(&registry_lock);
    /* KillTimer leaves queued WM_TIMER messages behind. Never reuse an ID,
     * including after a notification HWND is destroyed and its value reused.
     * Reserve the replacement first so failure preserves the old timer. */
    if (SetTimer(animation->window, timer, delay, NULL) == 0) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    cancel_timer(animation);
    animation->timer = timer;
    animation->generation = generation;
    return BOXRIG_UI_OK;
}

static LRESULT CALLBACK animation_window_proc(
    HWND window, UINT message, WPARAM word, LPARAM parameter)
{
    boxrig_win32_animation *animation =
        (boxrig_win32_animation *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW *creation = (const CREATESTRUCTW *)parameter;
        animation = creation->lpCreateParams;
        (void)SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)animation);
    } else if (message == WM_TIMER) {
        if (animation != NULL && animation->timer != 0 &&
            animation->timer == (UINT_PTR)word) {
            uint64_t generation = animation->generation;
            cancel_timer(animation);
            /* The runtime only requests a frame. Rendering remains in the
             * host's ordinary frame path, which also reports retained errors. */
            (void)boxrig_ui_animation_runtime_dispatch(animation->runtime, generation);
        }
        return 0;
    } else if (message == WM_NCDESTROY) {
        (void)SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(window, message, word, parameter);
}

boxrig_ui_status boxrig_win32_animation_acquire(
    boxrig_win32_animation **animation_out)
{
    if (animation_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *animation_out = NULL;
    DWORD thread = GetCurrentThreadId();
    AcquireSRWLockExclusive(&registry_lock);
    for (boxrig_win32_animation *entry = registry; entry != NULL; entry = entry->next) {
        if (entry->thread == thread) {
            if (entry->references == SIZE_MAX) {
                ReleaseSRWLockExclusive(&registry_lock);
                return BOXRIG_UI_LIMIT_EXCEEDED;
            }
            entry->references++;
            *animation_out = entry;
            ReleaseSRWLockExclusive(&registry_lock);
            return BOXRIG_UI_OK;
        }
    }
    ReleaseSRWLockExclusive(&registry_lock);
    boxrig_win32_animation *animation = calloc(1, sizeof(*animation));
    if (animation == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    animation->thread = thread;
    animation->references = 1;
    animation->release = release_animation;
    animation->instance = GetModuleHandleW(NULL);
    (void)swprintf(animation->class_name, 64, L"boxrig.animation.%p", (void *)animation);
    WNDCLASSW window_class = {0};
    window_class.lpfnWndProc = animation_window_proc;
    window_class.hInstance = animation->instance;
    window_class.lpszClassName = animation->class_name;
    if (RegisterClassW(&window_class) == 0) {
        free(animation);
        return BOXRIG_UI_UNAVAILABLE;
    }
    animation->window = CreateWindowExW(0, animation->class_name, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, animation->instance, animation);
    if (animation->window == NULL) {
        (void)UnregisterClassW(animation->class_name, animation->instance);
        free(animation);
        return BOXRIG_UI_UNAVAILABLE;
    }
    boxrig_ui_animation_scheduler scheduler = {
        animation, read_time, reserve_timer, cancel_timer
    };
    boxrig_ui_status status = boxrig_ui_animation_runtime_create(
        &scheduler, &animation->runtime);
    if (status != BOXRIG_UI_OK) {
        (void)SetWindowLongPtrW(animation->window, GWLP_USERDATA, 0);
        (void)DestroyWindow(animation->window);
        (void)UnregisterClassW(animation->class_name, animation->instance);
        free(animation);
        return status;
    }
    AcquireSRWLockExclusive(&registry_lock);
    animation->next = registry;
    registry = animation;
    ReleaseSRWLockExclusive(&registry_lock);
    *animation_out = animation;
    return BOXRIG_UI_OK;
}

static void release_animation(boxrig_win32_animation *animation)
{
    AcquireSRWLockExclusive(&registry_lock);
    if (--animation->references != 0) {
        ReleaseSRWLockExclusive(&registry_lock);
        return;
    }
    boxrig_win32_animation **entry = &registry;
    while (*entry != animation) entry = &(*entry)->next;
    *entry = animation->next;
    ReleaseSRWLockExclusive(&registry_lock);
    /* Disconnect dispatch before releasing the runtime or any view state. */
    (void)SetWindowLongPtrW(animation->window, GWLP_USERDATA, 0);
    cancel_timer(animation);
    (void)DestroyWindow(animation->window);
    boxrig_ui_animation_runtime_destroy(animation->runtime);
    (void)UnregisterClassW(animation->class_name, animation->instance);
    free(animation);
}

void boxrig_win32_animation_release(boxrig_win32_animation *animation)
{
    /* Optional renderer DLLs create components used through the executable's
     * static public API. Their private registries are distinct, so lifetime
     * operations must run in the module that owns this registration. */
    if (animation != NULL) animation->release(animation);
}

boxrig_ui_animation_runtime *boxrig_win32_animation_runtime(
    boxrig_win32_animation *animation)
{
    return animation == NULL ? NULL : animation->runtime;
}
