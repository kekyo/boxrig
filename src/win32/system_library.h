/* Internal loading of optional libraries from the Windows system directory. */
#ifndef BOXRIG_WIN32_SYSTEM_LIBRARY_H
#define BOXRIG_WIN32_SYSTEM_LIBRARY_H

#include <windows.h>

/* Keep matching load/resolve/release operations for each library owner. */
typedef struct boxrig_win32_library_system {
    HMODULE (WINAPI *load_library)(LPCWSTR path);
    FARPROC (WINAPI *get_proc_address)(HMODULE module, LPCSTR name);
    BOOL (WINAPI *free_library)(HMODULE module);
} boxrig_win32_library_system;

boxrig_win32_library_system boxrig_win32_library_system_default(void);

/* The caller owns the returned reference and releases it after all DLL objects. */
HMODULE boxrig_win32_load_system_library(
    const WCHAR *name,
    HMODULE(WINAPI *load_library)(LPCWSTR path));

#endif
