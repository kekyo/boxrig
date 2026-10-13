/* Verify the installed static/shared public ABI without requiring a vendor ICD. */
#include <boxrig/render/opengl.h>
#include <boxrig/win32/wgl.h>
#include <boxrig/win32/opengl_component.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int count, char **arguments)
{
    boxrig_ogl_renderer_config renderer_config = boxrig_ogl_renderer_config_default();
    boxrig_ogl_renderer *renderer = NULL;
    boxrig_wgl_surface_config surface_config;
    boxrig_wgl_surface *surface = NULL;
    boxrig_wgl_binding *binding = NULL;
    boxrig_gl_runtime_config runtime;
    boxrig_gl_diagnostic diagnostic;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_win32_terminal_component_config component_config = boxrig_win32_terminal_component_config_default();
    boxrig_win32_terminal_component *component = NULL;
    bool shared;
    assert(count == 2);
    shared = strcmp(arguments[1], "shared") == 0;
    assert(shared || strcmp(arguments[1], "static") == 0);
    assert((GetModuleHandleW(L"boxrig-render-ogl.dll") != NULL) == shared);
    assert((GetModuleHandleW(L"boxrig-win32-ogl.dll") != NULL) == shared);
    assert(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(boxrig_win32_opengl_terminal_component_create_child(GetModuleHandleW(NULL),
        NULL, model, &component_config, &component) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(component == NULL);
    boxrig_win32_terminal_component_destroy(component);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    assert(renderer_config.limits.maximum_glyph_entries > 0);
    assert(boxrig_ogl_renderer_create(&renderer_config, &renderer, &diagnostic) == BOXRIG_GL_INVALID_ARGUMENT);
    assert(renderer == NULL && diagnostic.status == BOXRIG_GL_INVALID_ARGUMENT);
    assert(boxrig_wgl_binding_create(1, 1, &binding, &runtime, &diagnostic) == BOXRIG_GL_CONTEXT_MISMATCH);
    assert(binding == NULL && diagnostic.status == BOXRIG_GL_CONTEXT_MISMATCH);
    memset(&surface_config, 0, sizeof(surface_config));
    assert(boxrig_wgl_surface_create(&surface_config, &surface, &diagnostic) == BOXRIG_GL_INVALID_ARGUMENT);
    assert(surface == NULL && diagnostic.status == BOXRIG_GL_INVALID_ARGUMENT);
    puts("PASS installed Win32 OpenGL public ABI and context-free diagnostics");
    return 0;
}
