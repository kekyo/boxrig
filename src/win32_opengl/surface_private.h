/* Private injectable OS boundary; never part of the installed contract. */
#ifndef BOXRIG_WGL_SURFACE_PRIVATE_H
#define BOXRIG_WGL_SURFACE_PRIVATE_H
#include "boxrig/win32/wgl.h"
#include <GL/glcorearb.h>
#include <GL/wglext.h>

typedef struct boxrig_wgl_system {
    ATOM (WINAPI *register_class)(const WNDCLASSW *);
    BOOL (WINAPI *unregister_class)(LPCWSTR, HINSTANCE);
    HWND (WINAPI *create_window)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int,
        int, int, HWND, HMENU, HINSTANCE, LPVOID);
    BOOL (WINAPI *destroy_window)(HWND);
    HDC (WINAPI *get_dc)(HWND);
    int (WINAPI *choose_pixel_format)(HDC, const PIXELFORMATDESCRIPTOR *);
    int (WINAPI *describe_pixel_format)(HDC, int, UINT, LPPIXELFORMATDESCRIPTOR);
    BOOL (WINAPI *set_pixel_format)(HDC, int, const PIXELFORMATDESCRIPTOR *);
    HGLRC (WINAPI *create_context)(HDC);
    BOOL (WINAPI *delete_context)(HGLRC);
    BOOL (WINAPI *make_current)(HDC, HGLRC);
    BOOL (WINAPI *swap_buffers)(HDC);
    PROC (*resolve)(const char *);
    HGLRC (*create_profile_context)(PFNWGLCREATECONTEXTATTRIBSARBPROC, HDC, const int *);
} boxrig_wgl_system;

boxrig_wgl_system boxrig_wgl_system_default(void);
boxrig_gl_status boxrig_wgl_surface_create_with_system(
    const boxrig_wgl_surface_config *config, const boxrig_wgl_system *system,
    boxrig_wgl_surface **surface_out, boxrig_gl_diagnostic *diagnostic);

/* Used after an ES component fails to initialize its presentation resources. */
boxrig_gl_status boxrig_wgl_surface_create_desktop(const boxrig_wgl_surface_config *config,
    boxrig_wgl_surface **out, boxrig_gl_diagnostic *diagnostic);

/* Uses the production sentinel check before copying into a typed APIENTRYP. */
bool boxrig_wgl_copy_procedure(void *destination, size_t size, PROC procedure);
PROC boxrig_wgl_resolve_procedure(const char *name);
boxrig_gl_status boxrig_wgl_binding_create_with_resolver(uint64_t generation,
    uint64_t target_generation, PROC (*resolve)(const char *),
    boxrig_wgl_binding **out, boxrig_gl_runtime_config *runtime,
    boxrig_gl_diagnostic *diagnostic);
#endif
