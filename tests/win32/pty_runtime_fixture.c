/* boxrig - Under MIT. A loadable test package with ordinary exported APIs. */
#include <windows.h>

__declspec(dllexport) HRESULT WINAPI ConptyCreatePseudoConsole(
    COORD size, HANDLE input, HANDLE output, DWORD flags, void **console)
{
    (void)size; (void)input; (void)output; (void)flags; (void)console;
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI ConptyResizePseudoConsole(void *console, COORD size)
{
    (void)console; (void)size;
    return E_NOTIMPL;
}

#ifndef BOXRIG_TEST_MISSING_EXPORT
__declspec(dllexport) void WINAPI ConptyClosePseudoConsole(void *console)
{
    (void)console;
}
#endif
