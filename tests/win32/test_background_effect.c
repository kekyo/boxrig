/* boxrig - Under MIT. */
#include "background_effect_private.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int material = 2;
static HRESULT result = S_OK;
static HRESULT WINAPI get_attribute(HWND window, DWORD attribute, PVOID value, DWORD size)
{
    assert(window != NULL && attribute == 38 && size == sizeof(int));
    if (FAILED(result)) return result;
    *(int *)value = material;
    return S_OK;
}
static HRESULT WINAPI set_attribute(HWND window, DWORD attribute, LPCVOID value, DWORD size)
{
    assert(window != NULL && attribute == 38 && size == sizeof(int));
    if (FAILED(result)) return result;
    material = *(const int *)value;
    return S_OK;
}
int main(int argc, char **argv)
{
    HWND window = CreateWindowExW(0, L"STATIC", L"background effect", WS_OVERLAPPEDWINDOW,
        0, 0, 320, 200, NULL, NULL, GetModuleHandleW(NULL), NULL);
    assert(window != NULL);
    if (argc == 2 && strcmp(argv[1], "--physical") == 0) {
        boxrig_win32_background_effect *native;
        assert(boxrig_win32_background_effect_create(window, &native) == BOXRIG_UI_OK);
        boxrig_ui_status status = boxrig_win32_background_effect_set(native, BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE);
        boxrig_ui_background_effect_state state = boxrig_win32_background_effect_get_state(native);
        printf("physical Acrylic status=%d supported=%d accepted=%d reason=%d result=0x%08lx\n",
            status, state.supported, state.accepted, state.reason, (unsigned long)state.native_result);
        fflush(stdout);
        assert(status == BOXRIG_UI_OK && state.supported && state.accepted);
        assert(boxrig_win32_background_effect_set(native, BOXRIG_UI_BACKGROUND_EFFECT_NONE) == BOXRIG_UI_OK);
        assert(boxrig_win32_background_effect_destroy(&native) == BOXRIG_UI_OK && native == NULL);
        DestroyWindow(window);
        puts("PASS physical DWM background effect acceptance, removal and restoration");
        return 0;
    }
    boxrig_win32_background_effect *effect, *duplicate;
    boxrig_win32_background_effect_system system = {get_attribute, set_attribute};
    assert(boxrig_win32_background_effect_create_with_system(window, &system, &effect) == BOXRIG_UI_OK);
    assert(material == 2);
    assert(boxrig_win32_background_effect_create_with_system(window, &system, &duplicate) == BOXRIG_UI_BUSY);
    assert(boxrig_win32_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE) == BOXRIG_UI_OK);
    boxrig_ui_background_effect_state state = boxrig_win32_background_effect_get_state(effect);
    assert(state.supported && state.accepted && material == 3);
    material = 1;
    assert(boxrig_win32_background_effect_refresh(effect) == BOXRIG_UI_OK && material == 3);
    result = E_FAIL;
    assert(boxrig_win32_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_NONE) == BOXRIG_UI_UNAVAILABLE);
    state = boxrig_win32_background_effect_get_state(effect);
    assert(state.request == BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE && state.accepted && material == 3);
    result = S_OK;
    assert(boxrig_win32_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_NONE) == BOXRIG_UI_OK);
    assert(material == 1);
    result = E_FAIL;
    assert(boxrig_win32_background_effect_destroy(&effect) == BOXRIG_UI_UNAVAILABLE && effect != NULL);
    result = S_OK;
    assert(boxrig_win32_background_effect_destroy(&effect) == BOXRIG_UI_OK && effect == NULL);
    assert(material == 2);
    result = E_INVALIDARG;
    assert(boxrig_win32_background_effect_create_with_system(window, &system, &effect) == BOXRIG_UI_OK);
    assert(boxrig_win32_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_PREFER) == BOXRIG_UI_OK);
    state = boxrig_win32_background_effect_get_state(effect);
    assert(!state.supported && !state.accepted && state.reason == BOXRIG_UI_BACKGROUND_EFFECT_UNSUPPORTED);
    assert(boxrig_win32_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE) == BOXRIG_UI_UNAVAILABLE);
    assert(boxrig_win32_background_effect_get_state(effect).request == BOXRIG_UI_BACKGROUND_EFFECT_PREFER);
    assert(boxrig_win32_background_effect_destroy(&effect) == BOXRIG_UI_OK);
    DestroyWindow(window);
    puts("PASS Win32 Acrylic request, unavailable fallback, failure preservation and restoration");
    return 0;
}
