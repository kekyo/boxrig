/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "surface_private.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct boxrig_wgl_binding {
    HGLRC context;
    HDC dc;
    DWORD thread;
    PROC (*resolve_procedure)(const char *);
};

static bool query_current(void *context, boxrig_gl_current_context *current)
{
    const boxrig_wgl_binding *binding = context;
    if (binding == NULL || current == NULL ||
        GetCurrentThreadId() != binding->thread ||
        wglGetCurrentContext() != binding->context ||
        wglGetCurrentDC() != binding->dc) return false;
    current->identity = (uintptr_t)binding->context;
    current->thread_identity = binding->thread;
    return true;
}

bool boxrig_wgl_copy_procedure(void *destination, size_t size, PROC procedure)
{
    uintptr_t bits = 0;
    if (destination == NULL || size != sizeof(procedure) ||
        sizeof(procedure) != sizeof(bits)) return false;
    memcpy(&bits, &procedure, sizeof(bits));
    if (bits == 0 || bits <= 3 || bits == UINTPTR_MAX) return false;
    memcpy(destination, &procedure, size);
    return true;
}

PROC boxrig_wgl_resolve_procedure(const char *name)
{
    HMODULE module;
    PROC procedure;
    if (name == NULL) return NULL;
    /* opengl32 exports the 1.1 entry points. Context-specific entries must
     * come from the ICD for this exact current context and pixel format. */
    module = GetModuleHandleW(L"opengl32.dll");
    procedure = module == NULL ? NULL : GetProcAddress(module, name);
    return procedure == NULL ? wglGetProcAddress(name) : procedure;
}

static boxrig_gl_function resolve(void *context, const char *name)
{
    const boxrig_wgl_binding *binding = context;
    boxrig_gl_current_context current;
    boxrig_gl_function result = NULL;
    if (name == NULL || !query_current(context, &current)) return NULL;
    /* Never call this opaque pointer: runtime.c copies it into APIENTRYP
     * typedefs from Khronos glcorearb.h, preserving i686 stdcall. */
    if (!boxrig_wgl_copy_procedure(&result, sizeof(result), binding->resolve_procedure(name)))
        return NULL;
    return result;
}

static boxrig_gl_status report_status(boxrig_gl_diagnostic *diagnostic,
    boxrig_gl_status status, const char *message)
{
    if (diagnostic != NULL) {
        memset(diagnostic, 0, sizeof(*diagnostic));
        diagnostic->status = status;
        snprintf(diagnostic->message, sizeof(diagnostic->message), "%s", message);
    }
    return status;
}

boxrig_gl_status boxrig_wgl_binding_create_with_resolver(
    uint64_t generation, uint64_t target_generation,
    PROC (*resolve_procedure)(const char *),
    boxrig_wgl_binding **binding_out, boxrig_gl_runtime_config *runtime_out,
    boxrig_gl_diagnostic *diagnostic)
{
    boxrig_wgl_binding *binding;
    if (binding_out != NULL) *binding_out = NULL;
    if (diagnostic != NULL) memset(diagnostic, 0, sizeof(*diagnostic));
    if (binding_out == NULL || runtime_out == NULL || generation == 0 ||
        target_generation == 0 || resolve_procedure == NULL)
        return report_status(diagnostic, BOXRIG_GL_INVALID_ARGUMENT, "invalid WGL binding configuration");
    memset(runtime_out, 0, sizeof(*runtime_out));
    if (wglGetCurrentContext() == NULL || wglGetCurrentDC() == NULL)
        return report_status(diagnostic, BOXRIG_GL_CONTEXT_MISMATCH, "a caller WGL context must be current");
    binding = calloc(1, sizeof(*binding));
    if (binding == NULL) return report_status(diagnostic, BOXRIG_GL_OUT_OF_MEMORY, "allocate WGL binding");
    binding->context = wglGetCurrentContext();
    binding->dc = wglGetCurrentDC();
    binding->thread = GetCurrentThreadId();
    binding->resolve_procedure = resolve_procedure;
    runtime_out->resolve = resolve;
    runtime_out->resolver_context = binding;
    runtime_out->query_current_context = query_current;
    runtime_out->current_context_query_context = binding;
    runtime_out->context_generation = generation;
    runtime_out->target_generation = target_generation;
    *binding_out = binding;
    return report_status(diagnostic, BOXRIG_GL_OK, "caller WGL context bound");
}

boxrig_gl_status boxrig_wgl_binding_create(uint64_t generation,
    uint64_t target_generation, boxrig_wgl_binding **out,
    boxrig_gl_runtime_config *runtime, boxrig_gl_diagnostic *diagnostic)
{
    return boxrig_wgl_binding_create_with_resolver(generation, target_generation,
        boxrig_wgl_resolve_procedure, out, runtime, diagnostic);
}

void boxrig_wgl_binding_destroy(boxrig_wgl_binding *binding)
{
    free(binding);
}
