#include "system_library.h"

#include <string.h>
#include <wchar.h>

HMODULE boxrig_win32_load_system_library(
    const WCHAR *name,
    HMODULE(WINAPI *load_library)(LPCWSTR path))
{
    WCHAR path[MAX_PATH];
    UINT length = GetSystemDirectoryW(path, MAX_PATH);
    size_t name_length = wcslen(name);

    if (length == 0 || length >= MAX_PATH ||
        name_length >= MAX_PATH - length - 1) {
        return NULL;
    }
    /* An absolute path avoids requiring the Windows 7 loader-update flags. */
    path[length++] = L'\\';
    memcpy(path + length, name, (name_length + 1) * sizeof(WCHAR));
    return load_library(path);
}

boxrig_win32_library_system boxrig_win32_library_system_default(void)
{
    const boxrig_win32_library_system result = {LoadLibraryW, GetProcAddress, FreeLibrary};
    return result;
}
