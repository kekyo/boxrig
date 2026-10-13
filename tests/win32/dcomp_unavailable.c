/* Exercise the actual CLI with only the optional DirectComposition DLL absent. */
#include "../../src/win32/system_library.h"
#include <wchar.h>

static HMODULE WINAPI load_library(LPCWSTR path)
{
    const WCHAR *name = wcsrchr(path, L'\\');
    name = name == NULL ? path : name + 1;
    if (wcscmp(name, L"dcomp.dll") == 0) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return NULL;
    }
    return LoadLibraryW(path);
}

boxrig_win32_library_system __wrap_boxrig_win32_library_system_default(void)
{
    const boxrig_win32_library_system result = {load_library, GetProcAddress, FreeLibrary};
    return result;
}
