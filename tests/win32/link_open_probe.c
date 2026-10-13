/* boxrig - Under MIT. */
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

LRESULT WINAPI __real_DefSubclassProc(HWND window, UINT message, WPARAM word,
    LPARAM long_value);

/** Acknowledges native leave delivery after the sample handled cancellation.
 * @param window Native input window.
 * @param message Native event identifier.
 * @param word Native event word.
 * @param long_value Native event data.
 * @return Result from the remaining native subclass chain.
 */
LRESULT WINAPI __wrap_DefSubclassProc(HWND window, UINT message, WPARAM word,
    LPARAM long_value)
{
    LRESULT result = __real_DefSubclassProc(window, message, word, long_value);
    if (message == WM_MOUSELEAVE) {
        fputs("BOXRIG_TEST_LINK_LEAVE\n", stderr);
        fflush(stderr);
    }
    return result;
}

/** Records native shell requests without changing system associations.
 * @param request The real sample's prepared shell-open request.
 * @return FALSE for the controlled missing-handler URI; TRUE otherwise.
 */
BOOL WINAPI __wrap_ShellExecuteExW(SHELLEXECUTEINFOW *request)
{
    if (request == NULL || request->cbSize != sizeof(*request) ||
        request->hwnd == NULL ||
        request->fMask != (SEE_MASK_FLAG_NO_UI | SEE_MASK_UNICODE) ||
        request->lpVerb == NULL || wcscmp(request->lpVerb, L"open") != 0 ||
        request->lpFile == NULL || request->lpParameters != NULL ||
        request->lpDirectory != NULL || request->nShow != SW_SHOWNORMAL) {
        fputs("BOXRIG_TEST_URI_CONTRACT_FAILED\n", stderr);
        fflush(stderr);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    char uri[8193];
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, request->lpFile,
            -1, uri, sizeof(uri), NULL, NULL) == 0) {
        fputs("BOXRIG_TEST_URI_ENCODING_FAILED\n", stderr);
        fflush(stderr);
        return FALSE;
    }
    fprintf(stderr, "BOXRIG_TEST_URI_OPEN=%s\n", uri);
    fflush(stderr);
    BOOL accepted = strcmp(uri, "https://example.test/new") != 0;
    SetLastError(accepted ? ERROR_SUCCESS : ERROR_NO_ASSOCIATION);
    return accepted;
}
