/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <windows.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Each fence role opens only the exact sample name with the access it needs. */
static HANDLE open_fence_event(const char *base, const wchar_t *suffix,
    DWORD access)
{
    static const char prefix[] = "Local\\boxrig-bench-fence-";
    wchar_t name[128];
    size_t length = strlen(base);
    size_t suffix_length = wcslen(suffix);
    if (length != sizeof(prefix) - 1 + 64 ||
        strncmp(base, prefix, sizeof(prefix) - 1) != 0 ||
        length + suffix_length >= sizeof(name) / sizeof(name[0])) return NULL;
    for (size_t index = 0; index < length; index++) {
        unsigned char value = (unsigned char)base[index];
        if (index >= sizeof(prefix) - 1 &&
            !((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) return NULL;
        name[index] = (wchar_t)value;
    }
    name[length] = L'\0';
    (void)wcscat(name, suffix);
    return OpenEventW(access, FALSE, name);
}

static int now(uint64_t frequency, uint64_t *result)
{
    LARGE_INTEGER value;
    if (!QueryPerformanceCounter(&value) || value.QuadPart < 0) return -1;
    uint64_t ticks = (uint64_t)value.QuadPart;
    *result = ticks / frequency * UINT64_C(1000000000) +
        ticks % frequency * UINT64_C(1000000000) / frequency;
    return 0;
}

/* Keep a failed or interrupted handshake from publishing a partial source. */
static int publish_fence(const char *path, uint64_t frequency,
    uint64_t arm_requested, uint64_t release_requested)
{
    size_t length = strlen(path);
    if (length > SIZE_MAX - 9) return -1;
    char *pending = malloc(length + 9);
    if (pending == NULL) return -1;
    memcpy(pending, path, length);
    memcpy(pending + length, ".pending", 9);
    FILE *file = fopen(pending, "wb");
    if (file == NULL) { free(pending); return -1; }
    int result = fprintf(file, "{\"frequency\":\"%" PRIu64
        "\",\"armRequestedNs\":\"%" PRIu64
        "\",\"releaseRequestedNs\":\"%" PRIu64 "\"}\n",
        frequency, arm_requested, release_requested) < 0 ? -1 : 0;
    if (fclose(file) != 0) result = -1;
    if (result == 0 && rename(pending, path) != 0) result = -1;
    if (result != 0) remove(pending);
    free(pending);
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    LARGE_INTEGER counter_frequency;
    if (!QueryPerformanceFrequency(&counter_frequency) ||
        counter_frequency.QuadPart <= 0 ||
        (uint64_t)counter_frequency.QuadPart >
            UINT64_MAX / UINT64_C(1000000000)) return 1;
    uint64_t frequency = (uint64_t)counter_frequency.QuadPart;
    HANDLE arm = open_fence_event(argv[1], L"-arm", EVENT_MODIFY_STATE);
    HANDLE acknowledgement = open_fence_event(argv[1], L"-ack", SYNCHRONIZE);
    HANDLE release = open_fence_event(argv[1], L"-release", EVENT_MODIFY_STATE);
    HANDLE timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    uint64_t arm_requested = 0, release_requested = 0;
    uint64_t write_start = 0, write_end = 0;
    int result = 1;
    if (arm == NULL || acknowledgement == NULL || release == NULL ||
        timer == NULL || output == NULL || output == INVALID_HANDLE_VALUE ||
        now(frequency, &arm_requested) != 0 || !SetEvent(arm) ||
        WaitForSingleObject(acknowledgement, 5000) != WAIT_OBJECT_0)
        goto cleanup;

    /* Retain three paced screen-sized writes. The first is fenced; the later
     * writes exercise continued input after the terminal resumes rendering. */
    for (unsigned int index = 0; index < 3; index++) {
        if (index != 0) {
            LARGE_INTEGER due;
            /* Relative waitable timers use negative 100 ns intervals. */
            due.QuadPart = -5000000;
            if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) ||
                WaitForSingleObject(timer, INFINITE) != WAIT_OBJECT_0)
                goto cleanup;
        }
        char frame[80 * 24];
        memset(frame, 'A' + index, sizeof(frame));
        DWORD written = 0;
        if (index == 0 && now(frequency, &write_start) != 0) goto cleanup;
        if (!WriteFile(output, frame, sizeof(frame), &written, NULL) ||
            written != sizeof(frame)) goto cleanup;
        if (index == 0 &&
            (now(frequency, &write_end) != 0 ||
             now(frequency, &release_requested) != 0 || !SetEvent(release)))
            goto cleanup;
    }

    FILE *log = fopen(argv[2], "wb");
    if (log == NULL) goto cleanup;
    int saved = fprintf(log, "{\"writes\":[{\"startNs\":\"%" PRIu64
        "\",\"endNs\":\"%" PRIu64 "\"}]}\n", write_start, write_end);
    if (fclose(log) != 0) saved = -1;
    if (saved < 0 || publish_fence(argv[3], frequency,
            arm_requested, release_requested) != 0) goto cleanup;
    result = 0;

cleanup:
    if (arm != NULL) (void)CloseHandle(arm);
    if (acknowledgement != NULL) (void)CloseHandle(acknowledgement);
    if (release != NULL) (void)CloseHandle(release);
    if (timer != NULL) (void)CloseHandle(timer);
    return result;
}
