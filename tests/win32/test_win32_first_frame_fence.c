/* Exercise the terminal's first-frame display fence with real named events. */
#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "../../examples/boxrig_term/win32/first_frame_fence.h"

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL first frame fence line %d: %s\n", __LINE__, #value); \
    return 1; \
} } while (0)

static int handshake(void)
{
    static const char base[] = "Local\\boxrig-bench-fence-"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    boxrig_term_win32_first_frame_fence fence = {0};
    HANDLE arm;
    HANDLE acknowledgement;
    HANDLE release;
    FILE *output;
    char bytes[512];
    size_t length;
    CHECK(boxrig_term_win32_first_frame_fence_initialize(&fence, base) == 0);
    arm = OpenEventW(EVENT_MODIFY_STATE, FALSE,
        L"Local\\boxrig-bench-fence-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef-arm");
    acknowledgement = OpenEventW(SYNCHRONIZE, FALSE,
        L"Local\\boxrig-bench-fence-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef-ack");
    release = OpenEventW(EVENT_MODIFY_STATE, FALSE,
        L"Local\\boxrig-bench-fence-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef-release");
    CHECK(arm != NULL && acknowledgement != NULL && release != NULL);
    CHECK(boxrig_term_win32_first_frame_fence_wait_handle(&fence) == fence.arm);
    CHECK(boxrig_term_win32_first_frame_fence_before_frame(&fence) == 0);
    CHECK(WaitForSingleObject(acknowledgement, 0) == WAIT_TIMEOUT);
    CHECK(SetEvent(arm));
    CHECK(boxrig_term_win32_first_frame_fence_before_frame(&fence) == 1);
    CHECK(WaitForSingleObject(acknowledgement, 0) == WAIT_OBJECT_0);
    CHECK(boxrig_term_win32_first_frame_fence_wait_handle(&fence) == fence.release);
    CHECK(boxrig_term_win32_first_frame_fence_before_frame(&fence) == 1);
    CHECK(SetEvent(release));
    /* The message loop waits on this handle before the next frame. */
    CHECK(WaitForSingleObject(
        boxrig_term_win32_first_frame_fence_wait_handle(&fence), 0) == WAIT_OBJECT_0);
    CHECK(boxrig_term_win32_first_frame_fence_before_frame(&fence) == 0);
    CHECK(boxrig_term_win32_first_frame_fence_wait_handle(&fence) == NULL);
    output = tmpfile();
    CHECK(output != NULL);
    CHECK(boxrig_term_win32_first_frame_fence_finish(&fence, output) == 0);
    CHECK(fflush(output) == 0 && fseek(output, 0, SEEK_SET) == 0);
    length = fread(bytes, 1, sizeof(bytes) - 1, output);
    bytes[length] = '\0';
    CHECK(strstr(bytes, "boxrig_term: first-frame-fence-version=1\n") != NULL);
    CHECK(strstr(bytes, "boxrig_term: first-frame-fence qpc-frequency=") != NULL);
    CHECK(strstr(bytes, " arm-ack-qpc=") != NULL);
    CHECK(strstr(bytes, " release-seen-qpc=") != NULL);
    CHECK(strstr(bytes, "boxrig_term: first-frame-fence-status=ok\n") != NULL);
    CHECK(fclose(output) == 0);
    CHECK(SetEvent(release));
    CHECK(boxrig_term_win32_first_frame_fence_before_frame(&fence) == -1);
    boxrig_term_win32_first_frame_fence_destroy(&fence);
    CHECK(CloseHandle(arm) && CloseHandle(acknowledgement) && CloseHandle(release));
    return 0;
}

static int missing_release(void)
{
    static const char base[] = "Local\\boxrig-bench-fence-"
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    boxrig_term_win32_first_frame_fence fence = {0};
    CHECK(boxrig_term_win32_first_frame_fence_initialize(&fence, base) == 0);
    CHECK(SetEvent(fence.arm));
    CHECK(boxrig_term_win32_first_frame_fence_before_frame(&fence) == 1);
    CHECK(boxrig_term_win32_first_frame_fence_finish(&fence, NULL) == -1);
    boxrig_term_win32_first_frame_fence_destroy(&fence);
    return 0;
}

static int premature_release(void)
{
    static const char base[] = "Local\\boxrig-bench-fence-"
        "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
    boxrig_term_win32_first_frame_fence fence = {0};
    CHECK(boxrig_term_win32_first_frame_fence_initialize(&fence, base) == 0);
    CHECK(SetEvent(fence.release));
    CHECK(boxrig_term_win32_first_frame_fence_before_frame(&fence) == -1);
    boxrig_term_win32_first_frame_fence_destroy(&fence);
    return 0;
}

static int invalid_base(void)
{
    boxrig_term_win32_first_frame_fence fence = {0};
    CHECK(boxrig_term_win32_first_frame_fence_initialize(
        &fence, "Local\\boxrig-bench-fence-invalid") == -1);
    CHECK(fence.arm == NULL && fence.acknowledgement == NULL &&
        fence.release == NULL);
    return 0;
}

int main(void)
{
    CHECK(handshake() == 0);
    CHECK(missing_release() == 0);
    CHECK(premature_release() == 0);
    CHECK(invalid_base() == 0);
    return 0;
}
