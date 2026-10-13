/* boxrig - Under MIT. */
#include "uri_launcher.h"
#include <objbase.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>

struct boxrig_term_win32_uri_launcher {
    HWND owner;
    FILE *diagnostics;
    HRESULT apartment;
};

static int report_failure(boxrig_term_win32_uri_launcher *launcher, DWORD error)
{
    if (launcher != NULL && launcher->diagnostics != NULL) {
        fprintf(launcher->diagnostics, "boxrig_term: could not open URL (Win32 error=%lu).\n",
            (unsigned long)error);
        fflush(launcher->diagnostics);
    }
    SetLastError(error);
    return -1;
}

int boxrig_term_win32_uri_launcher_create(HWND owner, FILE *diagnostics,
    boxrig_term_win32_uri_launcher **launcher_out)
{
    if (launcher_out == NULL) return report_failure(NULL, ERROR_INVALID_PARAMETER);
    *launcher_out = NULL;
    boxrig_term_win32_uri_launcher *launcher = calloc(1, sizeof(*launcher));
    if (launcher == NULL) return report_failure(NULL, ERROR_NOT_ENOUGH_MEMORY);
    launcher->owner = owner;
    launcher->diagnostics = diagnostics;
    /* Keep the UI apartment alive while its message loop can service shell
     * extensions. Both S_OK and S_FALSE acquire one balanced COM reference. */
    launcher->apartment = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    *launcher_out = launcher;
    return 0;
}

int boxrig_term_win32_uri_launcher_open(boxrig_term_win32_uri_launcher *launcher,
    const char *uri, size_t length)
{
    if (launcher == NULL || uri == NULL || length == 0 || length > 8192 || strlen(uri) != length)
        return report_failure(launcher, ERROR_INVALID_PARAMETER);
    if (FAILED(launcher->apartment)) return report_failure(launcher, ERROR_NOT_SUPPORTED);
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, uri, (int)length, NULL, 0);
    if (count == 0) return report_failure(launcher, GetLastError());
    wchar_t *wide = malloc(((size_t)count + 1) * sizeof(*wide));
    if (wide == NULL) return report_failure(launcher, ERROR_NOT_ENOUGH_MEMORY);
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, uri, (int)length, wide, count) != count) {
        DWORD error = GetLastError();
        free(wide);
        return report_failure(launcher, error);
    }
    wide[count] = L'\0';
    SHELLEXECUTEINFOW request = {0};
    request.cbSize = sizeof(request);
    request.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_UNICODE;
    request.hwnd = launcher->owner;
    request.lpVerb = L"open";
    request.lpFile = wide;
    request.nShow = SW_SHOWNORMAL;
    BOOL accepted = ShellExecuteExW(&request);
    DWORD error = accepted ? ERROR_SUCCESS : GetLastError();
    free(wide);
    return accepted ? 0 : report_failure(launcher, error);
}

void boxrig_term_win32_uri_launcher_destroy(boxrig_term_win32_uri_launcher *launcher)
{
    if (launcher == NULL) return;
    if (SUCCEEDED(launcher->apartment)) CoUninitialize();
    free(launcher);
}
