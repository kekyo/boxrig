/* Public component transactions with real WGL resources and injected OS failures. */
#include "boxrig/win32/opengl_component.h"
#include "../../src/win32_opengl/component_private.h"
#include "../../src/win32/renderer_private.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "opengl_component_capture.h"

static bool base_directwrite;
static unsigned int omitted_bitmap_queries;
static HRESULT no_bitmap1(IDWriteBitmapRenderTarget *target, IDWriteBitmapRenderTarget1 **out)
{
    (void)target;
    *out = NULL;
    omitted_bitmap_queries++;
    return E_NOINTERFACE;
}
int __real_boxrig_win32_renderer_create(HDC, boxrig_win32_renderer **);
int __wrap_boxrig_win32_renderer_create(HDC dc, boxrig_win32_renderer **out)
{
    boxrig_win32_renderer_system system;
    if (!base_directwrite) return __real_boxrig_win32_renderer_create(dc, out);
    system = boxrig_win32_renderer_system_default();
    system.query_bitmap_render_target1 = no_bitmap1;
    return boxrig_win32_renderer_create_with_system(dc, &system, out);
}

static bool alpha_storage_absent;
boxrig_gl_status __real_boxrig_wgl_surface_get_info(const boxrig_wgl_surface *, boxrig_wgl_surface_info *);
boxrig_gl_status __wrap_boxrig_wgl_surface_get_info(const boxrig_wgl_surface *surface, boxrig_wgl_surface_info *out)
{
    boxrig_gl_status status = __real_boxrig_wgl_surface_get_info(surface, out);
    /* Surface format fallback is tested at the real WGL boundary separately.
     * Here reproduce a changed capability after managed context recreation. */
    if (status == BOXRIG_GL_OK && alpha_storage_absent) out->alpha_bits = 0;
    return status;
}

static bool fail_swap;
static bool fail_current;
static HWND minimize_during_current;
static bool fail_create;
static bool fail_after_create;
static bool fail_input;
static bool fail_frame;
static unsigned int creation_count;
static bool reject_swap_interval;
static unsigned int swap_interval_attempts;
static bool reject_es_renderer;
static unsigned int es_renderer_failures;

boxrig_gl_status __real_boxrig_ogl_renderer_create_with_win32_font(
    const boxrig_ogl_renderer_config *, boxrig_win32_renderer *,
    boxrig_ogl_renderer **, boxrig_gl_diagnostic *);
boxrig_gl_status __wrap_boxrig_ogl_renderer_create_with_win32_font(
    const boxrig_ogl_renderer_config *config, boxrig_win32_renderer *font,
    boxrig_ogl_renderer **out, boxrig_gl_diagnostic *diagnostic)
{
    PFNGLGETSTRINGPROC get_string = NULL;
    assert(boxrig_wgl_copy_procedure(&get_string, sizeof(get_string),
        boxrig_wgl_resolve_procedure("glGetString")));
    if (reject_es_renderer && strncmp((const char *)get_string(GL_VERSION),
            "OpenGL ES ", 10) == 0) {
        es_renderer_failures++;
        *out = NULL;
        return BOXRIG_GL_UNAVAILABLE;
    }
    return __real_boxrig_ogl_renderer_create_with_win32_font(config, font, out, diagnostic);
}

static BOOL WINAPI rejected_interval(int interval)
{
    assert(interval == 1);
    swap_interval_attempts++;
    return FALSE;
}

PROC __real_boxrig_wgl_resolve_procedure(const char *);
PROC __wrap_boxrig_wgl_resolve_procedure(const char *name)
{
    if (reject_swap_interval && strcmp(name, "wglSwapIntervalEXT") == 0) {
        PFNWGLSWAPINTERVALEXTPROC typed = rejected_interval;
        PROC result;
        memcpy(&result, &typed, sizeof(result));
        return result;
    }
    return __real_boxrig_wgl_resolve_procedure(name);
}

boxrig_gl_status __real_boxrig_wgl_surface_swap(boxrig_wgl_surface *, boxrig_gl_diagnostic *);
boxrig_gl_status __wrap_boxrig_wgl_surface_swap(boxrig_wgl_surface *s, boxrig_gl_diagnostic *d)
{
    if (fail_swap) { fail_swap = false; return BOXRIG_GL_API_FAILURE; }
    return __real_boxrig_wgl_surface_swap(s, d);
}
boxrig_gl_status __real_boxrig_wgl_surface_make_current(boxrig_wgl_surface *, boxrig_gl_diagnostic *);
boxrig_gl_status __wrap_boxrig_wgl_surface_make_current(boxrig_wgl_surface *s, boxrig_gl_diagnostic *d)
{
    if (fail_current) { fail_current = false; return BOXRIG_GL_API_FAILURE; }
    boxrig_gl_status status = __real_boxrig_wgl_surface_make_current(s, d);
    if (status == BOXRIG_GL_OK && minimize_during_current != NULL) {
        HWND host = minimize_during_current;
        minimize_during_current = NULL;
        ShowWindow(host, SW_MINIMIZE);
    }
    return status;
}
boxrig_gl_status __real_boxrig_wgl_surface_create(const boxrig_wgl_surface_config *,
    boxrig_wgl_surface **, boxrig_gl_diagnostic *);
boxrig_gl_status __wrap_boxrig_wgl_surface_create(const boxrig_wgl_surface_config *c,
    boxrig_wgl_surface **s, boxrig_gl_diagnostic *d)
{
    boxrig_gl_status status;
    creation_count++;
    if (fail_create) { *s = NULL; return BOXRIG_GL_UNAVAILABLE; }
    status = __real_boxrig_wgl_surface_create(c, s, d);
    return status == BOXRIG_GL_OK && fail_after_create ? BOXRIG_GL_UNAVAILABLE : status;
}
int __real_boxrig_win32_input_adapter_create(HWND, boxrig_terminal *, unsigned int,
    unsigned int, size_t, size_t, boxrig_win32_input_adapter **);
int __wrap_boxrig_win32_input_adapter_create(HWND w, boxrig_terminal *t,
    unsigned int cw, unsigned int ch, size_t capacity, size_t chunk,
    boxrig_win32_input_adapter **out)
{
    if (fail_input) { *out = NULL; return -1; }
    return __real_boxrig_win32_input_adapter_create(w, t, cw, ch, capacity, chunk, out);
}

boxrig_renderer_status __real_boxrig_ogl_renderer_lifecycle_create(boxrig_ogl_renderer *,
    const boxrig_renderer_target *, boxrig_renderer_lifecycle **);
boxrig_renderer_status __wrap_boxrig_ogl_renderer_lifecycle_create(boxrig_ogl_renderer *r,
    const boxrig_renderer_target *t, boxrig_renderer_lifecycle **out)
{
    if (fail_frame) { fail_frame = false; *out = NULL; return BOXRIG_RENDERER_UNAVAILABLE; }
    return __real_boxrig_ogl_renderer_lifecycle_create(r, t, out);
}

static boxrig_win32_ogl_observation observe(boxrig_win32_terminal_component *component)
{
    boxrig_win32_ogl_observation observation;
    HDC dc = wglGetCurrentDC();
    HGLRC context = wglGetCurrentContext();
    assert(boxrig_win32_ogl_observe(component, &observation));
    assert(wglMakeCurrent(dc, context));
    return observation;
}

static unsigned int child_count(HWND parent)
{
    unsigned int count = 0;
    HWND child;
    for (child = GetWindow(parent, GW_CHILD); child != NULL; child = GetWindow(child, GW_HWNDNEXT)) count++;
    return count;
}

int main(void)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    HWND parent = CreateWindowExW(0, L"STATIC", L"WGL component transactions",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 400, NULL, NULL, instance, NULL);
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_win32_terminal_component *component = NULL;
    boxrig_win32_terminal_component_config config = boxrig_win32_terminal_component_config_default();
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result result;
    boxrig_font_effective_metrics metrics;
    boxrig_win32_ogl_observation before, after;
    boxrig_ui_dirty dirty = {0, 2, 0, 12};
    boxrig_ui_view *view;
    boxrig_win32_terminal_borrowed_target borrowed;
    boxrig_ui_transparency_state transparency;
    LRESULT event_result;
    unsigned int i;
    unsigned int low_dpi_width = 0, low_dpi_height = 0;
    bool native_es_used = false;
    assert(parent != NULL);
    tc.rows = 2; tc.columns = 12;
    assert(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED;

    /* View alpha uses the WGL target, including without GDI alpha support.
     * The caller's root attributes remain its own responsibility. */
    for (i = 0; i < 4; i++) {
        LONG_PTR original_style = GetWindowLongPtrW(parent, GWL_EXSTYLE);
        DIBSECTION bitmap;
        const uint8_t *pixels;
        size_t pixel, background = 0, glyph = 0, consumed;
        HDC capture;
        base_directwrite = i >= 2;
        config.transparency.request = i % 2 == 0 ? BOXRIG_UI_TRANSPARENCY_REQUIRED : BOXRIG_UI_TRANSPARENCY_PREFERRED;
        config.transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
        config.transparency.background_opacity = 128;
        assert(boxrig_win32_opengl_terminal_component_create_child(instance, parent,
            model, &config, &component) == BOXRIG_UI_OK);
        if (i == 0) {
            boxrig_win32_opengl_diagnostics diagnostics;
            assert(boxrig_win32_opengl_terminal_component_get_diagnostics(component, &diagnostics) == BOXRIG_UI_OK);
            native_es_used = diagnostics.surface.gl.is_gles;
            printf("PASS WGL component selected API=%s version=%s\n",
                native_es_used ? "OpenGL ES" : "OpenGL", diagnostics.surface.gl.version);
        }
        assert(GetWindowLongPtrW(parent, GWL_EXSTYLE) == original_style);
        assert(boxrig_win32_terminal_component_get_transparency(component, &transparency) == BOXRIG_UI_OK);
        assert(transparency.effective == BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA);
        assert(transparency.effective_background_opacity == 128);
        assert(transparency.capabilities.supported_scopes == BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE);
        assert(boxrig_win32_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
        allocation.width = metrics.cell_width * 12 + GetSystemMetrics(SM_CXVSCROLL);
        allocation.height = metrics.cell_height * 2;
        assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
        assert(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
        assert(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[HMMMMMMMM", 11, &consumed) == BOXRIG_STATUS_OK && consumed == 11);
        assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
        capture = capture_gpu_component(component);
        assert(capture != NULL);
        assert(GetObjectW(capture_bitmap, sizeof(bitmap), &bitmap) == sizeof(bitmap));
        pixels = bitmap.dsBm.bmBits;
        for (pixel = 0; pixel < (size_t)metrics.cell_width * 12 * metrics.cell_height; pixel++) {
            size_t row = pixel / (metrics.cell_width * 12);
            size_t column = pixel % (metrics.cell_width * 12);
            const uint8_t *value = pixels + ((size_t)bitmap.dsBm.bmHeight - row - 1) * bitmap.dsBm.bmWidthBytes + column * 4;
            if (value[3] == 128) background++;
            if (value[3] == 255 && value[0] > 100 && value[1] > 100 && value[2] > 100) glyph++;
            assert(value[0] <= value[3] && value[1] <= value[3] && value[2] <= value[3]);
        }
        assert(background > 20 && glyph > 10);
        /* Controls remain opaque even when the terminal background uses alpha. */
        for (pixel = 0; pixel < (size_t)bitmap.dsBm.bmHeight; pixel++) {
            const uint8_t *row = pixels + pixel * bitmap.dsBm.bmWidthBytes;
            unsigned int column;
            for (column = metrics.cell_width * 12; column < (unsigned int)bitmap.dsBm.bmWidth; column++)
                assert(row[column * 4 + 3] == 255);
        }
        assert(release_gpu_capture());
        if (base_directwrite) assert(omitted_bitmap_queries > 0);
        boxrig_win32_terminal_component_destroy(component);
        component = NULL;
        assert(child_count(parent) == 0 && GetWindowLongPtrW(parent, GWL_EXSTYLE) == original_style);
    }
    base_directwrite = false;
    config.transparency = boxrig_ui_transparency_config_default();
    /* Initial ES renderer failure retries GL without replacing view or input. */
    {
        boxrig_win32_opengl_diagnostics fallback;
        HWND input;
        reject_es_renderer = true;
        assert(boxrig_win32_opengl_terminal_component_create_child(instance, parent,
            model, &config, &component) == BOXRIG_UI_OK);
        assert(boxrig_win32_opengl_terminal_component_get_diagnostics(component, &fallback) == BOXRIG_UI_OK);
        assert(!fallback.surface.gl.is_gles);
        view = boxrig_win32_terminal_component_view(component);
        input = boxrig_win32_terminal_component_input_window(component);
        assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
        assert(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
        assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
        reject_es_renderer = false;
        assert(boxrig_win32_opengl_terminal_component_get_diagnostics(component, &fallback) == BOXRIG_UI_OK);
        assert(!fallback.surface.gl.is_gles && fallback.accepted_frames > 0);
        assert(es_renderer_failures == (native_es_used ? 1u : 0u));
        assert(boxrig_win32_terminal_component_view(component) == view);
        assert(boxrig_win32_terminal_component_input_window(component) == input);
        assert(capture_gpu_component(component) != NULL && release_gpu_capture());
        boxrig_win32_terminal_component_destroy(component);
        component = NULL;
        assert(child_count(parent) == 0 && wglGetCurrentContext() == NULL);
        puts(native_es_used ? "PASS ES renderer failure falls back to desktop GL" :
            "SKIP ES renderer failure: driver does not provide ES");
    }
    for (i = 0; i < 3; i++) {
        fail_create = i == 0; fail_after_create = i == 1; fail_input = i == 2;
        assert(boxrig_win32_opengl_terminal_component_create_child(instance, parent,
            model, &config, &component) == BOXRIG_UI_UNAVAILABLE);
        assert(component == NULL && child_count(parent) == 0);
        assert(wglGetCurrentContext() == NULL);
    }
    fail_create = fail_after_create = fail_input = false;
    /* Optional swap control failure must leave the opaque renderer usable. */
    reject_swap_interval = true;
    assert(boxrig_win32_opengl_terminal_component_create_child(instance, parent,
        model, &config, &component) == BOXRIG_UI_OK);
    assert(!observe(component).swap_interval_set);
    {
        boxrig_win32_opengl_diagnostics diagnostics;
        assert(boxrig_win32_opengl_terminal_component_get_diagnostics(component,
            &diagnostics) == BOXRIG_UI_OK);
        assert(diagnostics.available && !diagnostics.swap_interval_set);
        assert(diagnostics.surface.alpha_bits >= 8);
        if (diagnostics.surface.swap_control) assert(swap_interval_attempts > 0);
        assert(diagnostics.surface.gl.vendor[0] != '\0');
        assert(wglGetCurrentContext() == NULL);
    }
    reject_swap_interval = false;
    view = boxrig_win32_terminal_component_view(component);
    assert(boxrig_win32_terminal_component_capabilities(component) & BOXRIG_UI_COMPONENT_CAPABILITY_TARGET_ALPHA);
    assert(boxrig_win32_terminal_component_get_transparency(component, &transparency) == BOXRIG_UI_OK);
    assert(transparency.capabilities.supported_scopes == BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE);
    assert(boxrig_win32_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
    allocation.width = metrics.cell_width * 12 + GetSystemMetrics(SM_CXVSCROLL);
    allocation.height = metrics.cell_height * 2;
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
    before = observe(component);
    assert(before.swaps == 1 && before.framebuffer != 0);
    assert(before.window != boxrig_win32_terminal_component_input_window(component));
    assert(SendMessageW(before.window, WM_NCHITTEST, 0, 0) == HTTRANSPARENT);
    assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
    after = observe(component);
    assert(after.swaps == before.swaps && after.target_generation == before.target_generation);

    /* Losing optional alpha storage must keep the same input/model usable.
     * Capability recovery re-establishes the retained preferred view policy. */
    {
        HWND input = boxrig_win32_terminal_component_input_window(component);
        boxrig_ui_transparency_config alpha = boxrig_ui_transparency_config_default();
        boxrig_win32_opengl_diagnostics committed, changed;
        alpha.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
        alpha.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
        alpha.background_opacity = 128;
        assert(boxrig_win32_terminal_component_set_transparency(component, &alpha) == BOXRIG_UI_OK);
        assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
        assert(boxrig_win32_opengl_terminal_component_get_diagnostics(component, &committed) == BOXRIG_UI_OK);
        assert(boxrig_win32_terminal_component_get_transparency(component, &transparency) == BOXRIG_UI_OK);
        assert(transparency.effective_background_opacity == 128);
        alpha_storage_absent = true;
        fail_current = true;
        assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
        assert(boxrig_win32_opengl_terminal_component_get_diagnostics(component, &changed) == BOXRIG_UI_OK);
        assert(changed.accepted_frames > committed.accepted_frames && changed.surface.alpha_bits == 0);
        assert(boxrig_win32_terminal_component_get_transparency(component, &transparency) == BOXRIG_UI_OK);
        assert(transparency.effective_background_opacity == 255);
        assert(transparency.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
        assert(transparency.config.background_opacity == 128);
        assert(boxrig_win32_terminal_component_input_window(component) == input);
        alpha.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
        assert(boxrig_win32_terminal_component_set_transparency(component, &alpha) == BOXRIG_UI_UNAVAILABLE);
        alpha_storage_absent = false;
        fail_current = true;
        assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
        assert(boxrig_win32_terminal_component_get_transparency(component, &transparency) == BOXRIG_UI_OK);
        assert(transparency.effective_background_opacity == 128);
        assert(boxrig_win32_terminal_component_input_window(component) == input);
        alpha = boxrig_ui_transparency_config_default();
        assert(boxrig_win32_terminal_component_set_transparency(component, &alpha) == BOXRIG_UI_OK);
        assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
        before = observe(component);
    }

    /* Presentation-only changes must refresh cached native row metadata. */
    {
        static const uint8_t colors[] = "\033[48;2;31;79;127m\033[2J";
        size_t consumed;
        HDC capture;
        assert(boxrig_terminal_feed(terminal, colors, sizeof(colors) - 1, &consumed) == BOXRIG_STATUS_OK && consumed == sizeof(colors) - 1);
        assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
        capture = capture_gpu_component(component);
        assert(capture != NULL);
        assert(GetPixel(capture, (int)metrics.cell_width * 2, (int)metrics.cell_height / 2) == RGB(31, 79, 127));
        assert(release_gpu_capture());
        before = observe(component);
    }

    /* Direction changes after a cached frame must update native hit testing. */
    {
        boxrig_ui_hit_test_result hit;
        HDC dc = CreateCompatibleDC(NULL);
        boxrig_win32_renderer *reference = NULL;
        boxrig_text_layout *row = NULL;
        boxrig_win32_shape_statistics statistics;
        size_t expected;
        assert(boxrig_win32_renderer_create(dc, &reference) == 0);
        assert(boxrig_win32_renderer_set_direction_policy(reference, BOXRIG_TEXT_DIRECTION_FORCE_RTL) == 0);
        assert(boxrig_win32_renderer_layout_line(reference, terminal, 0, &row, &statistics) == 0);
        assert(boxrig_text_layout_visual_to_logical(row, 0, &expected) == BOXRIG_TEXT_LAYOUT_OK);
        assert(boxrig_win32_terminal_component_set_text_direction(component, BOXRIG_TEXT_DIRECTION_FORCE_RTL) == BOXRIG_UI_OK);
        assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
        assert(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
        assert(hit.logical_column == expected);
        boxrig_text_layout_destroy(row);
        boxrig_win32_renderer_destroy(reference);
        DeleteDC(dc);
        assert(boxrig_win32_terminal_component_set_text_direction(component, BOXRIG_TEXT_DIRECTION_AUTO) == BOXRIG_UI_OK);
        assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
        before = observe(component);
    }

    /* A failed render followed by a new extent must discard the obsolete candidate. */
    assert(boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
    fail_frame = true;
    assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_UNAVAILABLE);
    assert(observe(component).framebuffer == before.framebuffer);
    allocation.width++;
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
    before = observe(component);
    assert(before.width == allocation.width);

    /* Failed swap retains dirty and the last accepted texture. */
    assert(boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
    fail_swap = true;
    assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_UNAVAILABLE);
    after = observe(component);
    assert(after.swaps == before.swaps && after.framebuffer == before.framebuffer);
    assert(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_PRESENT, &dirty));
    assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
    after = observe(component);
    assert(after.context_generation > before.context_generation && after.swaps == before.swaps + 1);
    assert(!boxrig_ui_view_frame_requested(view));
    before = after;

    fail_current = true;
    assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
    after = observe(component);
    assert(after.context_generation > before.context_generation && after.swaps > before.swaps);
    before = after;
    assert(boxrig_win32_terminal_component_dispatch_message(component,
        boxrig_win32_terminal_component_input_window(component), WM_DISPLAYCHANGE,
        0, 0, &event_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    fail_create = true;
    assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_UNAVAILABLE);
    assert(boxrig_ui_view_frame_requested(view));
    {
        boxrig_win32_opengl_diagnostics diagnostics;
        assert(boxrig_win32_opengl_terminal_component_get_diagnostics(component,
            &diagnostics) == BOXRIG_UI_OK);
        assert(!diagnostics.available);
        assert(diagnostics.last_operation.status == BOXRIG_GL_UNAVAILABLE);
    }
    fail_create = false;
    assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
    after = observe(component);
    assert(after.context_generation > before.context_generation);
    before = after;

    /* WM_DISPLAYCHANGE is broadcast to the host, not to child HWNDs. */
    SendMessageW(parent, WM_DISPLAYCHANGE, 32, MAKELPARAM(1920, 1080));
    assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
    after = observe(component);
    assert(after.context_generation > before.context_generation);
    before = after;

    /* Normal allocation changes retain the child/context, including repeated resize. */
    {
        const WPARAM notifications[] = {PBT_APMRESUMEAUTOMATIC, PBT_APMRESUMESUSPEND};
        size_t notification;
        for (notification = 0; notification < 2; notification++) {
            SendMessageW(parent, WM_POWERBROADCAST, notifications[notification], 0);
            assert(boxrig_ui_view_frame_requested(view));
            assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
            after = observe(component);
            assert(after.context_generation > before.context_generation);
            assert(after.swaps == before.swaps + 1);
            before = after;
        }
    }
    for (i = 0; i < 12; i++) {
        allocation.width = metrics.cell_width * 12 + GetSystemMetrics(SM_CXVSCROLL) + i % 3;
        assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
        assert(observe(component).framebuffer == before.framebuffer);
        assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
        after = observe(component);
        assert(after.window == before.window && after.context_generation == before.context_generation);
        assert(after.width == allocation.width && after.height == allocation.height);
        before = after;
    }
    /* Native transitions can arrive after frame preparation has begun.
     * Keep its dirty pending until the same host becomes drawable again. */
    assert(boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
    minimize_during_current = parent;
    assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
    assert(minimize_during_current == NULL && IsIconic(parent));
    assert(!result.additional_dispatch);
    assert(boxrig_ui_view_frame_requested(view));
    after = observe(component);
    assert(after.swaps == before.swaps && after.framebuffer == before.framebuffer);
    ShowWindow(parent, SW_RESTORE);
    assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
    after = observe(component);
    assert(after.swaps == before.swaps + 1);
    assert(!boxrig_ui_view_frame_requested(view));
    before = after;
    {
        RECT bounds;
        RECT client;
        assert(GetWindowRect(parent, &bounds));
        assert(SetWindowPos(parent, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
        assert(!IsIconic(parent));
        assert(GetClientRect(parent, &client) && (client.right <= 0 || client.bottom <= 0));
        assert(boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
        assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
        /* Retain dirty without requesting another immediate frame. The host
         * must be able to dispatch the native transition that restores size. */
        assert(result.frame_pending && !result.additional_dispatch);
        assert(observe(component).swaps == before.swaps);
        assert(SetWindowPos(parent, NULL, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
    }
    ShowWindow(parent, SW_MINIMIZE);
    assert(IsIconic(parent));
    assert(boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
    assert(result.frame_pending && !result.additional_dispatch);
    assert(observe(component).swaps == before.swaps);
    ShowWindow(parent, SW_RESTORE);
    assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
    assert(!result.frame_pending && !result.additional_dispatch);
    assert(observe(component).swaps == before.swaps + 1);
    boxrig_win32_terminal_component_destroy(component);
    assert(child_count(parent) == 0 && IsWindow(parent));

    /* Borrowed input and presentation HWNDs survive destruction. */
    memset(&borrowed, 0, sizeof(borrowed));
    borrowed.presentation_window = parent;
    borrowed.input_window = parent;
    assert(boxrig_win32_opengl_terminal_component_create_on_target(instance, &borrowed,
        model, &config, &component) == BOXRIG_UI_OK);
    allocation.x = 7; allocation.y = 11;
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_input_window(component) == parent);
    assert(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
    assert(observe(component).width == allocation.width);
    for (i = 0; i < 5; i++) {
        boxrig_font_visual_step_policy zoom = boxrig_font_visual_step_policy_default();
        boxrig_ui_layout_action action;
        bool changed = false, applied = false;
        before = observe(component);
        zoom.minimum_size_dip = metrics.requested_size_dip;
        zoom.maximum_size_dip = metrics.requested_size_dip + 12;
        if (i >= 2) {
            boxrig_font_rendering_policy policy;
            boxrig_font_effective_metrics dpi_metrics;
            assert(boxrig_win32_terminal_component_get_font_rendering_policy(component, &policy) == BOXRIG_UI_OK);
            /* Establish the raw-font baseline after the visual zoom cases,
             * then exercise both directions of the same DPI transaction. */
            policy.dpi_x = i == 3 ? 144.0f : 96.0f;
            policy.dpi_y = policy.dpi_x;
            assert(boxrig_win32_terminal_component_set_font_rendering_policy(component, &policy) == BOXRIG_UI_OK);
            assert(boxrig_win32_terminal_component_get_effective_metrics(component, &dpi_metrics) == BOXRIG_FONT_CATALOG_OK);
            assert(dpi_metrics.dpi_x == policy.dpi_x && dpi_metrics.dpi_y == policy.dpi_y);
        } else {
            assert(boxrig_win32_terminal_component_visual_zoom(component, BOXRIG_FONT_ZOOM_INCREASE, zoom, &changed) == BOXRIG_UI_OK && changed);
        }
        assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
        after = observe(component);
        assert(after.swaps == before.swaps && after.framebuffer == before.framebuffer);
        assert(boxrig_win32_terminal_component_take_layout_action(component, &action));
        assert(action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE);
        if (i == 1) {
            assert(boxrig_win32_terminal_component_dispatch_message(component,
                boxrig_win32_terminal_component_input_window(component), WM_DISPLAYCHANGE,
                0, 0, &event_result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
        }
        assert(boxrig_win32_terminal_component_complete_size_command(component, action.revision, BOXRIG_UI_SIZE_ACCEPTED,
            action.value.content_size.width, action.value.content_size.height, &applied, &layout) == BOXRIG_UI_OK && applied);
        allocation.width = (unsigned int)(action.value.content_size.width / BOXRIG_UI_FIXED_ONE) + GetSystemMetrics(SM_CXVSCROLL);
        allocation.height = (unsigned int)(action.value.content_size.height / BOXRIG_UI_FIXED_ONE);
        assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
        if (i >= 2) {
            /* Output DPI changes defer drawing until the matching allocation;
             * the last complete frame remains committed in the meantime. */
            assert(observe(component).framebuffer == before.framebuffer);
            assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
        }
        after = observe(component);
        assert(after.swaps == before.swaps + 1);
        assert(i != 1 ? after.context_generation == before.context_generation : after.context_generation > before.context_generation);
        assert(after.width == allocation.width && after.height == allocation.height);
        if (i == 2) {
            low_dpi_width = after.width;
            low_dpi_height = after.height;
        } else if (i == 4) {
            assert(after.width == low_dpi_width && after.height == low_dpi_height);
        }
        assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
    }
    /* Exercise host-driven grid changes with a full, colored terminal frame. */
    assert(boxrig_win32_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
    for (i = 0; i < 6; i++) {
        boxrig_ui_layout_action action;
        RECT client;
        unsigned int row;
        allocation.width = metrics.cell_width * (75 + i % 3 * 4) + GetSystemMetrics(SM_CXVSCROLL);
        allocation.height = metrics.cell_height * (24 + i % 3 * 2);
        assert(SetWindowPos(parent, NULL, 0, 0, (int)allocation.width + 64, (int)allocation.height + 64,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
        assert(GetClientRect(parent, &client));
        assert(boxrig_win32_terminal_component_set_target_extent(component, (unsigned int)client.right, (unsigned int)client.bottom) == BOXRIG_UI_OK);
        assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
        assert(boxrig_win32_terminal_component_take_layout_action(component, &action));
        assert(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        assert(boxrig_terminal_resize(terminal, action.value.grid_resize.rows, action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        assert(boxrig_win32_terminal_component_complete_grid_resize(component, action.revision, true) == BOXRIG_UI_OK);
        for (row = 0; row < 24; row++) {
            char line[192];
            size_t consumed;
            int length = snprintf(line, sizeof(line),
                "\033[%u;1H\033[48;2;%u;%u;%um\033[38;2;232;238;248mROW %02u PRESENTATION 0123456789 abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                row + 1, 20 + row % 3 * 16, 42 + row % 4 * 12, 76 + row % 5 * 10, row + 1);
            assert(length > 0 && (size_t)length < sizeof(line));
            assert(boxrig_terminal_feed(terminal, (const uint8_t *)line, (size_t)length, &consumed) == BOXRIG_STATUS_OK && consumed == (size_t)length);
        }
        assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        assert(boxrig_win32_terminal_component_frame(component, &result) == BOXRIG_UI_OK);
        assert(observe(component).width == allocation.width);
    }
    before = observe(component);
    assert(boxrig_win32_terminal_component_quiesce(component) == BOXRIG_UI_OK);
    assert(!IsWindowVisible(before.window));
    boxrig_win32_terminal_component_destroy(component);
    assert(IsWindow(parent) && child_count(parent) == 0);
    {
        WCHAR class_name[96];
        WNDCLASSW window_class;
        assert(boxrig_win32_opengl_terminal_component_create_child(instance, parent, model, &config, &component) == BOXRIG_UI_OK);
        before = observe(component);
        assert(GetClassNameW(before.window, class_name, 96) != 0);
        assert(DestroyWindow(parent));
        boxrig_win32_terminal_component_destroy(component);
        assert(!GetClassInfoW(instance, class_name, &window_class));
    }
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS WGL component view alpha, base DirectWrite, opaque policy, rollback, retained swap, loss, resize and borrowed lifetime");
    return 0;
}
