/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "first_frame_fence.h"

#include <stddef.h>
#include <string.h>
#include <wchar.h>

enum {
    BOXRIG_TERM_FENCE_WAITING_ARM = 0,
    BOXRIG_TERM_FENCE_HELD = 1,
    BOXRIG_TERM_FENCE_RELEASED = 2,
    BOXRIG_TERM_FENCE_FAILED = -1,
    BOXRIG_TERM_FENCE_RELEASE_TIMEOUT_MS = 5000
};

static int make_event_name(
    const char *base, const wchar_t *suffix, wchar_t *name, size_t capacity)
{
    static const char prefix[] = "Local\\boxrig-bench-fence-";
    size_t length;
    size_t suffix_length;
    size_t index;

    if (base == NULL) return -1;
    length = strlen(base);
    suffix_length = wcslen(suffix);
    if (length != sizeof(prefix) - 1 + 64 ||
        strncmp(base, prefix, sizeof(prefix) - 1) != 0 ||
        length + suffix_length >= capacity) return -1;
    for (index = 0; index < length; index++) {
        unsigned char value = (unsigned char)base[index];
        if (index >= sizeof(prefix) - 1 &&
            !((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) return -1;
        name[index] = (wchar_t)value;
    }
    name[length] = L'\0';
    (void)wcscat(name, suffix);
    return 0;
}

int boxrig_term_win32_first_frame_fence_initialize(
    boxrig_term_win32_first_frame_fence *fence, const char *base)
{
    static const wchar_t *suffixes[] = {L"-arm", L"-ack", L"-release"};
    HANDLE *events[3];
    wchar_t name[128];
    size_t index;

    if (fence == NULL) return -1;
    memset(fence, 0, sizeof(*fence));
    events[0] = &fence->arm;
    events[1] = &fence->acknowledgement;
    events[2] = &fence->release;
    if (!QueryPerformanceFrequency(&fence->frequency) ||
        fence->frequency.QuadPart <= 0) goto fail;
    for (index = 0; index < 3; index++) {
        if (make_event_name(base, suffixes[index], name,
                sizeof(name) / sizeof(name[0])) == -1) goto fail;
        /* The message-loop wait must leave release signaled for before_frame. */
        *events[index] = CreateEventW(NULL, TRUE, FALSE, name);
        if (*events[index] == NULL || GetLastError() == ERROR_ALREADY_EXISTS)
            goto fail;
    }
    fence->state = BOXRIG_TERM_FENCE_WAITING_ARM;
    return 0;

fail:
    boxrig_term_win32_first_frame_fence_destroy(fence);
    fence->state = BOXRIG_TERM_FENCE_FAILED;
    return -1;
}

int boxrig_term_win32_first_frame_fence_before_frame(
    boxrig_term_win32_first_frame_fence *fence)
{
    DWORD result;

    if (fence == NULL || fence->state == BOXRIG_TERM_FENCE_FAILED ||
        fence->arm == NULL || fence->release == NULL) return -1;
    if (fence->state == BOXRIG_TERM_FENCE_WAITING_ARM) {
        if (WaitForSingleObject(fence->release, 0) != WAIT_TIMEOUT) goto fail;
        result = WaitForSingleObject(fence->arm, 0);
        if (result == WAIT_TIMEOUT) return 0;
        if (result != WAIT_OBJECT_0 ||
            !QueryPerformanceCounter(&fence->acknowledgement_qpc) ||
            fence->acknowledgement_qpc.QuadPart < 0 ||
            !SetEvent(fence->acknowledgement)) goto fail;
        fence->held_since_milliseconds = GetTickCount();
        fence->state = BOXRIG_TERM_FENCE_HELD;
    }
    if (fence->state == BOXRIG_TERM_FENCE_HELD) {
        result = WaitForSingleObject(fence->release, 0);
        if (result == WAIT_TIMEOUT) {
            if ((DWORD)(GetTickCount() - fence->held_since_milliseconds) >=
                BOXRIG_TERM_FENCE_RELEASE_TIMEOUT_MS) goto fail;
            return 1;
        }
        if (result != WAIT_OBJECT_0 ||
            !QueryPerformanceCounter(&fence->release_qpc) ||
            fence->release_qpc.QuadPart < 0 ||
            !ResetEvent(fence->release)) goto fail;
        fence->state = BOXRIG_TERM_FENCE_RELEASED;
        return 0;
    }
    if (WaitForSingleObject(fence->release, 0) != WAIT_TIMEOUT) goto fail;
    return 0;

fail:
    fence->state = BOXRIG_TERM_FENCE_FAILED;
    return -1;
}

HANDLE boxrig_term_win32_first_frame_fence_wait_handle(
    const boxrig_term_win32_first_frame_fence *fence)
{
    if (fence == NULL) return NULL;
    if (fence->state == BOXRIG_TERM_FENCE_WAITING_ARM) return fence->arm;
    if (fence->state == BOXRIG_TERM_FENCE_HELD) return fence->release;
    return NULL;
}

int boxrig_term_win32_first_frame_fence_finish(
    const boxrig_term_win32_first_frame_fence *fence, FILE *output)
{
    if (fence == NULL || fence->state != BOXRIG_TERM_FENCE_RELEASED ||
        output == NULL ||
        WaitForSingleObject(fence->release, 0) != WAIT_TIMEOUT) return -1;
    if (fprintf(output,
            "boxrig_term: first-frame-fence-version=1\n"
            "boxrig_term: first-frame-fence qpc-frequency=%llu arm-ack-qpc=%llu release-seen-qpc=%llu\n"
            "boxrig_term: first-frame-fence-status=ok\n",
            (unsigned long long)fence->frequency.QuadPart,
            (unsigned long long)fence->acknowledgement_qpc.QuadPart,
            (unsigned long long)fence->release_qpc.QuadPart) < 0 ||
        fflush(output) != 0) return -1;
    return 0;
}

void boxrig_term_win32_first_frame_fence_destroy(
    boxrig_term_win32_first_frame_fence *fence)
{
    if (fence == NULL) return;
    if (fence->arm != NULL) (void)CloseHandle(fence->arm);
    if (fence->acknowledgement != NULL) (void)CloseHandle(fence->acknowledgement);
    if (fence->release != NULL) (void)CloseHandle(fence->release);
    fence->arm = NULL;
    fence->acknowledgement = NULL;
    fence->release = NULL;
}
