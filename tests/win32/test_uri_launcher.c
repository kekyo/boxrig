/* boxrig - Under MIT. */
#include "../../examples/boxrig_term/win32/uri_launcher.h"
#include <shellapi.h>
#include <objbase.h>
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <wchar.h>

static HRESULT initialization = S_OK;
static size_t initialized, uninitialized, requests;
static bool reject;
static const HWND owner = (HWND)(UINT_PTR)42;
static const char uri[] = "https://example.test/\xe6\x97\xa5\xe6\x9c\xac?q=\"literal\"&s=$(nothing);value";

HRESULT WINAPI __wrap_CoInitializeEx(LPVOID reserved, DWORD flags)
{
    assert(reserved == NULL);
    assert(flags == (COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    initialized++;
    return initialization;
}
void WINAPI __wrap_CoUninitialize(void) { uninitialized++; }
BOOL WINAPI __wrap_ShellExecuteExW(SHELLEXECUTEINFOW *info)
{
    assert(info->cbSize == sizeof(*info));
    assert(info->hwnd == owner && info->fMask == (SEE_MASK_FLAG_NO_UI | SEE_MASK_UNICODE));
    assert(wcscmp(info->lpVerb, L"open") == 0 && info->lpParameters == NULL);
    assert(wcscmp(info->lpFile, L"https://example.test/\x65e5\x672c?q=\"literal\"&s=$(nothing);value") == 0);
    assert(info->lpDirectory == NULL && info->nShow == SW_SHOWNORMAL);
    assert(info->hProcess == NULL);
    requests++;
    SetLastError(reject ? ERROR_NO_ASSOCIATION : ERROR_SUCCESS);
    return !reject;
}

int main(void)
{
    FILE *errors = tmpfile();
    assert(errors != NULL);
    boxrig_term_win32_uri_launcher *launcher = NULL;
    assert(boxrig_term_win32_uri_launcher_create(owner, errors, &launcher) == 0);
    assert(boxrig_term_win32_uri_launcher_open(launcher, uri, sizeof(uri) - 1) == 0);
    assert(requests == 1 && initialized == 1 && uninitialized == 0 && ftell(errors) == 0);
    reject = true;
    assert(boxrig_term_win32_uri_launcher_open(launcher, uri, sizeof(uri) - 1) == -1);
    assert(GetLastError() == ERROR_NO_ASSOCIATION && requests == 2);
    long first_error = ftell(errors);
    assert(first_error > 0);
    reject = false;
    assert(boxrig_term_win32_uri_launcher_open(launcher, uri, sizeof(uri) - 1) == 0);
    assert(requests == 3 && ftell(errors) == first_error);
    assert(boxrig_term_win32_uri_launcher_open(launcher, "https://x/\xff", 11) == -1);
    assert(requests == 3 && ftell(errors) > first_error);
    assert(boxrig_term_win32_uri_launcher_open(launcher, "https://x/", 9) == -1);
    assert(requests == 3);
    boxrig_term_win32_uri_launcher_destroy(launcher);
    assert(uninitialized == 1);
    initialization = S_FALSE;
    assert(boxrig_term_win32_uri_launcher_create(owner, errors, &launcher) == 0);
    boxrig_term_win32_uri_launcher_destroy(launcher);
    assert(uninitialized == 2);
    initialization = RPC_E_CHANGED_MODE;
    assert(boxrig_term_win32_uri_launcher_create(owner, errors, &launcher) == 0);
    assert(boxrig_term_win32_uri_launcher_open(launcher, uri, sizeof(uri) - 1) == -1);
    assert(requests == 3);
    boxrig_term_win32_uri_launcher_destroy(launcher);
    assert(uninitialized == 2);
    fclose(errors);
    puts("PASS UTF-16 shell open, exact URI, failure recovery and COM ownership");
    return 0;
}
