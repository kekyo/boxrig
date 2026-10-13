/* P2 driver: select an optional factory with an explicit per-monitor DPI host policy. */
#ifdef BOXRIG_TEST_D3D11_COMPONENT
#include "boxrig/win32/d3d11_component.h"
#else
#include "boxrig/win32/opengl_component.h"
#endif
#include "boxrig/win32/transparency.h"
#include "../../examples/boxrig_term/win32/application_internal.h"

#include <commctrl.h>
#include <limits.h>

/* The P2 host forwards real per-monitor notifications through the public
 * component policy API; the production sample's DPI policy is independent. */
static LRESULT CALLBACK dpi_host_procedure(HWND window, UINT message,
    WPARAM word, LPARAM value, UINT_PTR identifier, DWORD_PTR context)
{
    boxrig_term_win32_application *application =
        (boxrig_term_win32_application *)GetWindowLongPtrW(window, GWLP_USERDATA);
    (void)context;
    if (message == WM_NCDESTROY) {
        (void)RemoveWindowSubclass(window, dpi_host_procedure, identifier);
    } else if (message == WM_DPICHANGED && application != NULL &&
        application->component != NULL) {
        const RECT suggested = *(const RECT *)value;
        boxrig_font_rendering_policy policy;
        boxrig_ui_layout_action action;
        RECT outer;
        LONG width, height;
        const int scrollbar = GetSystemMetrics(SM_CXVSCROLL);
        if (boxrig_win32_terminal_component_get_font_rendering_policy(
                application->component, &policy) != BOXRIG_UI_OK) goto fail;
        policy.dpi_x = (float)LOWORD(word);
        policy.dpi_y = (float)HIWORD(word);
        if (boxrig_win32_terminal_component_set_font_rendering_policy(
                application->component, &policy) != BOXRIG_UI_OK) goto fail;
        /* Accept the component's matching content-size transaction during the
         * modal move. Integer cell metrics can differ from Windows' scaled
         * rectangle, so preserve its center to avoid crossing the DPI boundary
         * again solely because of the adjusted extent. */
        if (!boxrig_win32_terminal_component_take_layout_action(
                application->component, &action) ||
            action.kind != BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE ||
            action.value.content_size.width / BOXRIG_UI_FIXED_ONE > LONG_MAX - scrollbar ||
            action.value.content_size.height / BOXRIG_UI_FIXED_ONE > LONG_MAX ||
            scrollbar <= 0) goto fail;
        width = (LONG)(action.value.content_size.width / BOXRIG_UI_FIXED_ONE);
        height = (LONG)(action.value.content_size.height / BOXRIG_UI_FIXED_ONE);
        outer = (RECT){0, 0, width + scrollbar, height};
        if (!AdjustWindowRectExForDpi(&outer,
                (DWORD)GetWindowLongPtrW(window, GWL_STYLE), FALSE,
                (DWORD)GetWindowLongPtrW(window, GWL_EXSTYLE), LOWORD(word))) goto fail;
        application->pending_size = true;
        application->pending_size_revision = action.revision;
        application->pending_content_width = action.value.content_size.width;
        application->pending_content_height = action.value.content_size.height;
        if (!SetWindowPos(window, NULL,
                suggested.left + ((suggested.right - suggested.left) -
                    (outer.right - outer.left)) / 2,
                suggested.top + ((suggested.bottom - suggested.top) -
                    (outer.bottom - outer.top)) / 2,
                outer.right - outer.left, outer.bottom - outer.top,
                SWP_NOZORDER | SWP_NOACTIVATE)) goto fail;
        /* A pending font-size transaction can block WM_WINDOWPOSCHANGED before
         * DefWindowProc generates WM_SIZE. Complete allocation explicitly, as
         * the sample's normal content-size command handler does. */
        if ((application->pending_size &&
             boxrig_term_win32_native_host_resize_component(application) == -1) ||
            boxrig_term_win32_frame_component(application) == -1) goto fail;
        return 0;
fail:
        application->failed = true;
        (void)PostMessageW(window, WM_CLOSE, 0, 0);
        return 0;
    }
    return DefSubclassProc(window, message, word, value);
}

static boxrig_ui_status configure_dpi_host(HWND host,
    boxrig_win32_terminal_component **component)
{
    boxrig_font_rendering_policy policy;
    const UINT dpi = GetDpiForWindow(host);
    if (dpi == 0 ||
        boxrig_win32_terminal_component_get_font_rendering_policy(
            *component, &policy) != BOXRIG_UI_OK) goto fail;
    policy.dpi_x = (float)dpi;
    policy.dpi_y = (float)dpi;
    if (boxrig_win32_terminal_component_set_font_rendering_policy(
            *component, &policy) != BOXRIG_UI_OK ||
        !SetWindowSubclass(host, dpi_host_procedure, 1, 0)) goto fail;
    return BOXRIG_UI_OK;
fail:
    boxrig_win32_terminal_component_destroy(*component);
    *component = NULL;
    return BOXRIG_UI_UNAVAILABLE;
}

/* Select the same product host branch as --renderer. Replacing only the GDI
 * factory would leave the host preparing GDI resources for a GPU component. */
int __real_boxrig_term_win32_run(const boxrig_term_win32_config *config);
int __wrap_boxrig_term_win32_run(const boxrig_term_win32_config *config)
{
    boxrig_term_win32_config selected = *config;
    if (SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == NULL)
        return 1;
#ifdef BOXRIG_TEST_D3D11_COMPONENT
    selected.renderer = BOXRIG_TERM_WIN32_RENDERER_D3D11;
#else
    selected.renderer = BOXRIG_TERM_WIN32_RENDERER_OPENGL;
#endif
    return __real_boxrig_term_win32_run(&selected);
}

#ifdef BOXRIG_TEST_D3D11_COMPONENT
boxrig_ui_status __real_boxrig_win32_d3d11_terminal_component_create_child(
    HINSTANCE, HWND, boxrig_ui_model *, const boxrig_win32_terminal_component_config *,
    const boxrig_win32_d3d11_config *, boxrig_win32_terminal_component **);
boxrig_ui_status __wrap_boxrig_win32_d3d11_terminal_component_create_child(
    HINSTANCE instance, HWND parent, boxrig_ui_model *model,
    const boxrig_win32_terminal_component_config *config,
    const boxrig_win32_d3d11_config *graphics, boxrig_win32_terminal_component **out)
{
    const boxrig_ui_status status = __real_boxrig_win32_d3d11_terminal_component_create_child(
        instance, parent, model, config, graphics, out);
    return status == BOXRIG_UI_OK ? configure_dpi_host(parent, out) : status;
}
#else
boxrig_ui_status __real_boxrig_win32_opengl_terminal_component_create_child(
    HINSTANCE, HWND, boxrig_ui_model *, const boxrig_win32_terminal_component_config *,
    boxrig_win32_terminal_component **);
boxrig_ui_status __wrap_boxrig_win32_opengl_terminal_component_create_child(
    HINSTANCE instance, HWND parent, boxrig_ui_model *model,
    const boxrig_win32_terminal_component_config *config,
    boxrig_win32_terminal_component **out)
{
    const boxrig_ui_status status = __real_boxrig_win32_opengl_terminal_component_create_child(
        instance, parent, model, config, out);
    return status == BOXRIG_UI_OK ? configure_dpi_host(parent, out) : status;
}
#endif
