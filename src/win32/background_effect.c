/* boxrig - Under MIT. */
#include "background_effect_private.h"
#include <stdlib.h>
#include <string.h>

/* Documented SDK enum values, also usable with older MinGW SDK headers. */
#define SYSTEM_BACKDROP_ATTRIBUTE 38u
#define BACKDROP_NONE 1
#define BACKDROP_ACRYLIC 3
static const WCHAR owner_property[] = L"boxrig.background-effect.owner";
struct boxrig_win32_background_effect {
    HWND window;
    DWORD thread;
    HMODULE library;
    boxrig_win32_background_effect_system system;
    boxrig_ui_background_effect_state state;
    int original;
    bool have_original;
    bool changed;
    bool busy;
};

static bool valid_owner(const boxrig_win32_background_effect *effect)
{
    return effect != NULL && effect->thread == GetCurrentThreadId() &&
        GetPropW(effect->window, owner_property) == (HANDLE)effect;
}

boxrig_ui_status boxrig_win32_background_effect_create_with_system(HWND window,
    const boxrig_win32_background_effect_system *system, boxrig_win32_background_effect **out)
{
    if (out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *out = NULL;
    DWORD process = 0;
    if (system == NULL || !IsWindow(window) || GetAncestor(window, GA_ROOT) != window ||
        GetWindowThreadProcessId(window, &process) != GetCurrentThreadId() || process != GetCurrentProcessId())
        return BOXRIG_UI_INVALID_ARGUMENT;
    if (GetPropW(window, owner_property) != NULL) return BOXRIG_UI_BUSY;
    boxrig_win32_background_effect *effect = calloc(1, sizeof(*effect));
    if (effect == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    effect->window = window; effect->thread = GetCurrentThreadId(); effect->system = *system;
    HRESULT result = system->get != NULL && system->set != NULL ?
        system->get(window, SYSTEM_BACKDROP_ATTRIBUTE, &effect->original, sizeof(effect->original)) : E_NOTIMPL;
    effect->have_original = effect->state.supported = SUCCEEDED(result);
    effect->state.native_result = result;
    if (!SetPropW(window, owner_property, effect)) { free(effect); return BOXRIG_UI_UNAVAILABLE; }
    *out = effect;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_background_effect_create(HWND window, boxrig_win32_background_effect **out)
{
    boxrig_win32_background_effect_system system = {0};
    HMODULE library = LoadLibraryExW(L"dwmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (library != NULL) {
        FARPROC address = GetProcAddress(library, "DwmGetWindowAttribute");
        memcpy(&system.get, &address, sizeof(system.get));
        address = GetProcAddress(library, "DwmSetWindowAttribute");
        memcpy(&system.set, &address, sizeof(system.set));
    }
    boxrig_ui_status status = boxrig_win32_background_effect_create_with_system(window, &system, out);
    if (status == BOXRIG_UI_OK) (*out)->library = library;
    else if (library != NULL) FreeLibrary(library);
    return status;
}

boxrig_ui_status boxrig_win32_background_effect_set(boxrig_win32_background_effect *effect,
    boxrig_ui_background_effect_request request)
{
    if (!valid_owner(effect) || request < BOXRIG_UI_BACKGROUND_EFFECT_NONE ||
        request > BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE) return BOXRIG_UI_INVALID_ARGUMENT;
    if (effect->busy) return BOXRIG_UI_BUSY;
    effect->busy = true;
    if (!effect->have_original && effect->system.get != NULL && effect->system.set != NULL) {
        HRESULT result = effect->system.get(effect->window, SYSTEM_BACKDROP_ATTRIBUTE,
            &effect->original, sizeof(effect->original));
        effect->state.native_result = result;
        effect->have_original = effect->state.supported = SUCCEEDED(result);
    }
    if (!effect->state.supported) {
        effect->state.reason = request == BOXRIG_UI_BACKGROUND_EFFECT_NONE ?
            BOXRIG_UI_BACKGROUND_EFFECT_DISABLED : BOXRIG_UI_BACKGROUND_EFFECT_UNSUPPORTED;
        effect->busy = false;
        if (request == BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE) return BOXRIG_UI_UNAVAILABLE;
        effect->state.request = request;
        return BOXRIG_UI_OK;
    }
    int material = request == BOXRIG_UI_BACKGROUND_EFFECT_NONE ? BACKDROP_NONE : BACKDROP_ACRYLIC;
    HRESULT result = effect->system.set(effect->window, SYSTEM_BACKDROP_ATTRIBUTE, &material, sizeof(material));
    effect->state.native_result = result;
    effect->busy = false;
    if (FAILED(result)) {
        effect->state.reason = BOXRIG_UI_BACKGROUND_EFFECT_FAILED;
        return BOXRIG_UI_UNAVAILABLE;
    }
    effect->changed = true;
    effect->state.request = request;
    effect->state.accepted = request != BOXRIG_UI_BACKGROUND_EFFECT_NONE;
    effect->state.reason = effect->state.accepted ? BOXRIG_UI_BACKGROUND_EFFECT_ACCEPTED : BOXRIG_UI_BACKGROUND_EFFECT_DISABLED;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_background_effect_refresh(boxrig_win32_background_effect *effect)
{
    return effect == NULL ? BOXRIG_UI_INVALID_ARGUMENT : boxrig_win32_background_effect_set(effect, effect->state.request);
}

boxrig_ui_background_effect_state boxrig_win32_background_effect_get_state(const boxrig_win32_background_effect *effect)
{
    return effect == NULL ? (boxrig_ui_background_effect_state){0} : effect->state;
}

boxrig_ui_status boxrig_win32_background_effect_destroy(boxrig_win32_background_effect **address)
{
    if (address == NULL || *address == NULL) return BOXRIG_UI_OK;
    boxrig_win32_background_effect *effect = *address;
    if (effect->thread != GetCurrentThreadId()) return BOXRIG_UI_INVALID_ARGUMENT;
    if (effect->busy) return BOXRIG_UI_BUSY;
    if (valid_owner(effect)) {
        if (effect->changed) {
            effect->busy = true;
            HRESULT result = effect->system.set(effect->window, SYSTEM_BACKDROP_ATTRIBUTE,
                &effect->original, sizeof(effect->original));
            effect->busy = false;
            if (FAILED(result)) { effect->state.native_result = result; return BOXRIG_UI_UNAVAILABLE; }
        }
        RemovePropW(effect->window, owner_property);
    }
    if (effect->library != NULL) FreeLibrary(effect->library);
    free(effect); *address = NULL;
    return BOXRIG_UI_OK;
}
