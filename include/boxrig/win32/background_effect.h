/* boxrig - Under MIT. */
#ifndef BOXRIG_WIN32_BACKGROUND_EFFECT_H
#define BOXRIG_WIN32_BACKGROUND_EFFECT_H
#include "boxrig/ui/background_effect.h"
#include <windows.h>

/** Exclusive owner of one toplevel's DWM system backdrop attribute. */
typedef struct boxrig_win32_background_effect boxrig_win32_background_effect;
/**
 * Borrows a same-thread toplevel and remembers its original backdrop.
 * @param window Host-owned toplevel; must outlive the provider or be destroyed first.
 * @param out Receives the owner, including on systems without Acrylic support.
 * @return OK, INVALID_ARGUMENT, BUSY for duplicate ownership, or OUT_OF_MEMORY.
 * @remarks Does not alter transparency, child HWNDs, or terminal pixels. All calls
 * use the creating thread. The owner delegates exclusive backdrop control until
 * destruction. Windows 11 build 22621 is the first supported Acrylic API version.
 */
boxrig_ui_status boxrig_win32_background_effect_create(HWND window, boxrig_win32_background_effect **out);
/**
 * Requests Desktop Acrylic through the documented system backdrop attribute.
 * @param effect Host provider.
 * @param request None, preferred, or required.
 * @return OK on acceptance or optional absence; UNAVAILABLE on native failure.
 * @remarks OS accessibility, battery, focus and transparency policy may substitute
 * an opaque material. Failed required requests preserve the previous setting.
 */
boxrig_ui_status boxrig_win32_background_effect_set(boxrig_win32_background_effect *effect,
    boxrig_ui_background_effect_request request);
/**
 * Reapplies the request after DWM or OS policy changes.
 * @param effect Provider on its creating thread.
 * @return Result of the native request, with details in get_state().
 */
boxrig_ui_status boxrig_win32_background_effect_refresh(boxrig_win32_background_effect *effect);
/**
 * Reads native support and acceptance without asserting visible blur.
 * @param effect Provider, or NULL for disabled state.
 * @return Copied state and native HRESULT of the last attempt.
 */
boxrig_ui_background_effect_state boxrig_win32_background_effect_get_state(const boxrig_win32_background_effect *effect);
/**
 * Restores the original attribute and releases the provider.
 * @param effect In/out provider; NULL on success. A destroyed HWND needs no restore.
 * @return OK, BUSY for reentrancy, or UNAVAILABLE if restoration failed.
 * @remarks On failure the provider remains live for retry; do not free its owner.
 */
boxrig_ui_status boxrig_win32_background_effect_destroy(boxrig_win32_background_effect **effect);
#endif
