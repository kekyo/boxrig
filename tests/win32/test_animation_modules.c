/* boxrig - Under MIT. */
#include "../../src/win32/animation.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef BOXRIG_TEST_ANIMATION_DLL
__declspec(dllexport) boxrig_ui_status test_animation_acquire(
    boxrig_win32_animation **out)
{
    return boxrig_win32_animation_acquire(out);
}

__declspec(dllexport) void test_animation_release(boxrig_win32_animation *value)
{
    boxrig_win32_animation_release(value);
}
#else
__declspec(dllimport) boxrig_ui_status test_animation_acquire(
    boxrig_win32_animation **out);
__declspec(dllimport) void test_animation_release(boxrig_win32_animation *value);

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(EXIT_FAILURE); \
} } while (0)

int main(void)
{
    /* The optional renderer DLL creates a component whose public operations
     * are supplied by the executable's static Win32 library. Exercise both
     * release directions, including a reference retained by the creator. */
    for (unsigned iteration = 0; iteration < 3; iteration++) {
        boxrig_win32_animation *first = NULL, *second = NULL;
        CHECK(test_animation_acquire(&first) == BOXRIG_UI_OK);
        CHECK(test_animation_acquire(&second) == BOXRIG_UI_OK);
        CHECK(first == second);
        boxrig_win32_animation_release(first);
        CHECK(boxrig_ui_animation_runtime_status(
            boxrig_win32_animation_runtime(second)) == BOXRIG_UI_OK);
        boxrig_win32_animation_release(second);
        CHECK(boxrig_win32_animation_acquire(&first) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_animation_acquire(&second) == BOXRIG_UI_OK);
        CHECK(first == second);
        test_animation_release(first);
        CHECK(boxrig_ui_animation_runtime_status(
            boxrig_win32_animation_runtime(second)) == BOXRIG_UI_OK);
        test_animation_release(second);
    }
    puts("PASS animation lifetime across static and shared library boundaries");
    return 0;
}
#endif
