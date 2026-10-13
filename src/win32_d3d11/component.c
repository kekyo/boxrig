/* boxrig - Managed DirectComposition presentation for the shared Win32 component. */
#define COBJMACROS
#include "native.h"
#include "../win32/component_private.h"
#include "../win32/text_private.h"
#include "../d3d11/renderer_private.h"
#include <commctrl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct buffer_history {
    uint64_t complete_generation;
    RECT stale;
} buffer_history;

typedef struct surface {
    boxrig_dcomp_chain *chain;
    boxrig_d3d11_renderer *renderer;
    unsigned int width;
    unsigned int height;
    unsigned int buffer_index;
    uint64_t generation;
    uint64_t font_revision;
    uint64_t target_generation;
    uint64_t frame_generation;
    uint8_t opacity;
    bool ready;
    bool token_active;
    bool submitted;
    bool composition;
    bool full_raster;
    boxrig_renderer_frame_token token;
    buffer_history buffers[2];
} surface;

typedef struct presentation {
    HWND window;
    HWND host;
    DWORD thread;
    boxrig_win32_renderer *font;
    boxrig_dcomp_device *device;
    surface committed;
    surface candidate;
    surface retired;
    surface *waiting;
    boxrig_ui_view *view;
    boxrig_composition *composition;
    boxrig_win32_d3d11_diagnostics diagnostic;
    uint64_t generation;
    uint64_t target_generation;
    uint64_t composition_revision;
    uint64_t retired_commits;
    unsigned int width;
    unsigned int height;
    unsigned int preview_width;
    unsigned int preview_height;
    unsigned int scrollbar_width;
    int x;
    int y;
    bool alpha;
    bool validated;
    bool warp;
    bool invalid;
    bool dirty;
    bool tree_dirty;
    bool preview;
    bool visible;
    bool occluded;
    bool scrollbar;
    bool hovered;
    bool dragging;
} presentation;

static const boxrig_win32_component_presentation operations;
static void adapter_transparency(void *, boxrig_ui_transparency_capabilities *);

boxrig_dcomp_chain *boxrig_win32_d3d11_committed_chain(
    boxrig_win32_terminal_component *component)
{
    presentation *p;
    if (!boxrig_win32_terminal_component_has_presentation(component, &operations)) return NULL;
    p = boxrig_win32_terminal_component_presentation_context(component);
    return p->committed.chain;
}

IUnknown *boxrig_win32_d3d11_composition_device(boxrig_win32_terminal_component *component)
{
    presentation *p;
    if (!boxrig_win32_terminal_component_has_presentation(component, &operations)) return NULL;
    p = boxrig_win32_terminal_component_presentation_context(component);
    return p->device == NULL ? NULL : boxrig_dcomp_device_composition_get(p->device);
}

static void invalidate_view(presentation *p)
{
    const boxrig_ui_dirty dirty = {0, SIZE_MAX, 0, SIZE_MAX};
    p->dirty = true;
    if (p->view != NULL)
        (void)boxrig_ui_view_invalidate(p->view, BOXRIG_UI_DIRTY_PRESENT, dirty);
}

static int failed(presentation *p, HRESULT result)
{
    p->diagnostic.result = result;
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
        result == DXGI_ERROR_DEVICE_HUNG) {
        p->invalid = true;
        p->diagnostic.loss = BOXRIG_D3D11_LOSS_DEVICE;
    } else if (result == (HRESULT)D2DERR_RECREATE_TARGET) {
        p->diagnostic.loss = BOXRIG_D3D11_LOSS_TARGET;
    }
    invalidate_view(p);
    return -1;
}

static void abort_surface(surface *s)
{
    if (s->token_active)
        (void)boxrig_d3d11_renderer_abort_frame(s->renderer, &s->token);
    s->token_active = false;
    s->submitted = false;
}

static void release_surface(presentation *p, surface *s)
{
    if (p->waiting == s) p->waiting = NULL;
    abort_surface(s);
    (void)boxrig_d3d11_renderer_destroy(&s->renderer);
    boxrig_dcomp_chain_destroy(s->chain);
    memset(s, 0, sizeof(*s));
}

static void release_device(presentation *p)
{
    release_surface(p, &p->candidate);
    release_surface(p, &p->retired);
    release_surface(p, &p->committed);
    if (p->device != NULL) {
        boxrig_win32_d3d11_diagnostics native = {0};
        boxrig_dcomp_device_describe(p->device, &native);
        p->retired_commits += native.commits;
    }
    boxrig_dcomp_device_destroy(p->device);
    p->device = NULL;
    p->validated = false;
}

static bool ensure_device(presentation *p)
{
    HRESULT hr;
    if (p->device != NULL && !p->invalid) {
        hr = boxrig_dcomp_device_check(p->device);
        if (hr == S_OK) return true;
        (void)failed(p, hr);
        p->invalid = true;
        return false;
    }
    release_device(p);
    hr = boxrig_dcomp_device_create(p->window, p->warp, &p->device);
    if (FAILED(hr)) { (void)failed(p, hr); return false; }
    boxrig_dcomp_device_describe(p->device, &p->diagnostic);
    p->alpha = boxrig_dcomp_device_composition_get(p->device) != NULL;
    p->diagnostic.device_generation++;
    p->diagnostic.loss = BOXRIG_D3D11_LOSS_NONE;
    p->invalid = false;
    p->tree_dirty = true;
    invalidate_view(p);
    return true;
}

static bool create_surface(presentation *p, surface *s, unsigned int width,
    unsigned int height, uint8_t opacity, bool composition)
{
    HRESULT hr = composition ? boxrig_dcomp_chain_create(p->device, width, height,
        opacity < 255, &s->chain) :
        boxrig_dcomp_hwnd_chain_create(p->device, width, height, &s->chain);
    if (FAILED(hr)) { (void)failed(p, hr); return false; }
    {
        unsigned int retained = (p->committed.chain != NULL) +
            (p->candidate.chain != NULL) + (p->retired.chain != NULL);
        if (retained > p->diagnostic.peak_retained_swapchains)
            p->diagnostic.peak_retained_swapchains = retained;
    }
    s->width = width; s->height = height;
    s->generation = ++p->generation;
    s->opacity = opacity;
    s->composition = composition;
    s->buffers[0].stale = s->buffers[1].stale = (RECT){0, 0, (LONG)width, (LONG)height};
    s->ready = boxrig_dcomp_chain_activity(s->chain) == NULL;
    return true;
}

static void activity_ready(presentation *p)
{
    if (p->waiting != NULL) p->waiting->ready = true;
    p->waiting = NULL;
    /* The replacement's latency event allows the one retained old chain to
     * retire. DComp also retains its own references while processing Commit. */
    release_surface(p, &p->retired);
}

static bool ready(presentation *p, surface *s)
{
    HANDLE event;
    DWORD result;
    if (p->waiting != NULL && p->waiting != s) {
        event = boxrig_dcomp_chain_activity(p->waiting->chain);
        result = WaitForSingleObject(event, 0);
        if (result == WAIT_TIMEOUT) return false;
        if (result != WAIT_OBJECT_0) { (void)failed(p, HRESULT_FROM_WIN32(GetLastError())); return false; }
        activity_ready(p);
    }
    if (s->ready) return true;
    event = boxrig_dcomp_chain_activity(s->chain);
    if (event == NULL) { s->ready = true; return true; }
    p->waiting = s;
    result = WaitForSingleObject(event, 0);
    if (result != WAIT_OBJECT_0) {
        if (result != WAIT_TIMEOUT) (void)failed(p, HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    activity_ready(p);
    return true;
}

static bool prepare_renderer(presentation *p, surface *s,
    const boxrig_font_effective_metrics *metrics)
{
    ID3D11Texture2D *texture = NULL;
    boxrig_d3d11_target target = {0};
    HRESULT hr = boxrig_dcomp_chain_buffer(s->chain, &texture, &s->buffer_index);
    if (FAILED(hr)) { (void)failed(p, hr); return false; }
    /* One full-extent rectangle bounds all dirty missed by each physical
     * buffer. Unknown age or a different completed frame requires full raster. */
    s->full_raster = s->renderer == NULL || s->buffer_index >= 2 ||
        s->font_revision != metrics->layout_revision ||
        s->buffers[s->buffer_index].complete_generation != s->frame_generation ||
        s->buffers[s->buffer_index].stale.right > s->buffers[s->buffer_index].stale.left ||
        s->buffers[s->buffer_index].stale.bottom > s->buffers[s->buffer_index].stale.top;
    target.texture = texture;
    target.width = s->width; target.height = s->height;
    target.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    target.alpha = s->opacity < 255 ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE;
    target.dpi_x = metrics->dpi_x; target.dpi_y = metrics->dpi_y;
    if (s->renderer != NULL && s->font_revision != metrics->layout_revision)
        (void)boxrig_d3d11_renderer_destroy(&s->renderer);
    if (s->renderer == NULL) {
        boxrig_d3d11_renderer_config config = boxrig_d3d11_renderer_config_default();
        target.generation = ++p->target_generation;
        config.device = boxrig_dcomp_device_get(p->device);
        config.device_generation = p->diagnostic.device_generation;
        config.target = target;
        config.background_opacity = s->opacity;
        hr = boxrig_d3d11_renderer_create_with_font(&config, p->font, &s->renderer);
    } else target.generation = s->target_generation;
    ID3D11Texture2D_Release(texture);
    if (FAILED(hr)) { (void)failed(p, hr); return false; }
    {
        boxrig_d3d11_diagnostic capability;
        if (boxrig_d3d11_renderer_get_diagnostic(s->renderer, &capability) != 0)
            return false;
        p->diagnostic.maximum_surface_extent = capability.maximum_target_extent;
    }
    s->font_revision = metrics->layout_revision;
    s->target_generation = target.generation;
    /* D3D11 rotates the resource behind buffer zero's view automatically.
     * Keep that view for the chain lifetime; stale pixels require redraw,
     * not a newly created view onto the same logical resource. */
    if (s->full_raster) boxrig_win32_text_invalidate(p->font);
    return boxrig_d3d11_renderer_set_scrollbar(s->renderer, p->scrollbar,
        p->scrollbar_width, p->hovered, p->dragging) == 0;
}

static bool validate_device(presentation *p, const boxrig_font_effective_metrics *metrics,
    const boxrig_ui_transparency_config *config)
{
    boxrig_ui_transparency_capabilities capabilities;
    boxrig_ui_transparency_state resolved;
    bool alpha_target = false;
    if (p->alpha) {
        alpha_target = create_surface(p, &p->candidate, 1, 1, 128, true) &&
            prepare_renderer(p, &p->candidate, metrics);
        release_surface(p, &p->candidate);
        if (!alpha_target) {
            if (p->invalid) return false;
            boxrig_dcomp_device_discard_composition(p->device, p->diagnostic.result);
            p->alpha = false;
        }
    }
    boxrig_dcomp_device_describe(p->device, &p->diagnostic);
    adapter_transparency(p, &capabilities);
    if (boxrig_ui_transparency_resolve(config, &capabilities, NULL, &resolved) != BOXRIG_UI_OK) {
        p->diagnostic.result = p->diagnostic.composition_result;
        return false;
    }
    if (resolved.effective != BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA) {
        const bool opaque_target = create_surface(p, &p->candidate, 1, 1, 255, false) &&
            prepare_renderer(p, &p->candidate, metrics);
        release_surface(p, &p->candidate);
        if (!opaque_target) return false;
    } else if (!alpha_target) return false;
    p->diagnostic.result = S_OK;
    p->diagnostic.loss = BOXRIG_D3D11_LOSS_NONE;
    p->validated = true;
    return true;
}

static int prepare_presentation(presentation *p, surface *s)
{
    const RECT entire = {0, 0, (LONG)s->width, (LONG)s->height};
    HRESULT hr;
    if (!s->token_active) return -1;
    if (s->submitted)
        return s == &p->candidate && !ready(p, s) ? 1 : 0;
    /* A partial raster uses ordinary presentation; a dirty full rectangle is
     * legal only when every pixel in it has just been rendered. */
    hr = boxrig_dcomp_chain_present(s->chain, s->full_raster ? &entire : NULL);
    p->diagnostic.dirty_present = boxrig_dcomp_chain_dirty_present(s->chain);
    if (hr != S_OK) {
        abort_surface(s);
        /* Occlusion is not acceptance. Wait for a window event to retry. */
        if (hr == DXGI_STATUS_OCCLUDED) {
            p->occluded = true;
            p->diagnostic.result = hr;
            invalidate_view(p);
            return 1;
        }
        return failed(p, hr);
    }
    s->ready = boxrig_dcomp_chain_activity(s->chain) == NULL;
    p->waiting = s->ready ? NULL : s;
    s->submitted = true;
    /* A successful Present only queues the new image. Keep the old visual
     * attached until the candidate's first presentation completes. The
     * activity handle wakes the host; DXGI 1.2 keeps its eventless path. */
    return s == &p->candidate && !ready(p, s) ? 1 : 0;
}

static int present_surface(presentation *p, surface *s)
{
    const bool exchange = s == &p->candidate;
    const RECT entire = {0, 0, (LONG)s->width, (LONG)s->height};
    int prepared = prepare_presentation(p, s);
    HRESULT hr;
    unsigned int i;
    if (prepared != 0) return prepared;
    if (exchange || p->tree_dirty) {
        hr = boxrig_dcomp_commit(p->device, s->chain, p->x, p->y, p->visible);
        if (FAILED(hr)) {
            abort_surface(s);
            if (exchange) release_surface(p, s);
            return failed(p, hr);
        }
    }
    if (boxrig_d3d11_renderer_accept_frame(s->renderer, &s->token) != BOXRIG_RENDERER_OK)
        return failed(p, E_FAIL);
    s->token_active = false;
    s->submitted = false;
    /* Keep only the latest complete generation and one stale rectangle per
     * buffer. Conservative full dirty includes gutters, overlays and scrollbar. */
    s->frame_generation++;
    for (i = 0; i < 2; i++) s->buffers[i].stale = entire;
    if (s->buffer_index < 2) {
        s->buffers[s->buffer_index].complete_generation = s->frame_generation;
        s->buffers[s->buffer_index].stale = (RECT){0, 0, 0, 0};
    }
    if (exchange) {
        release_surface(p, &p->retired);
        p->retired = p->committed;
        p->committed = p->candidate;
        memset(&p->candidate, 0, sizeof(p->candidate));
        if (p->waiting == &p->candidate) p->waiting = &p->committed;
        p->preview = false;
    }
    p->diagnostic.accepted_frames++;
    p->diagnostic.width = p->committed.width;
    p->diagnostic.height = p->committed.height;
    p->diagnostic.target_generation = p->committed.generation;
    p->diagnostic.composition_swapchain = p->committed.composition;
    p->diagnostic.alpha = p->committed.composition && p->alpha;
    p->diagnostic.result = S_OK;
    p->diagnostic.loss = BOXRIG_D3D11_LOSS_NONE;
    p->tree_dirty = false;
    p->dirty = false;
    return 0;
}

static int adapter_draw(void *context, boxrig_ui_view *view, boxrig_composition *composition)
{
    presentation *p = context;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_transparency_state alpha;
    boxrig_composition_view transient = {0};
    surface *s;
    unsigned int width, height;
    bool composition_surface;
    RECT client;
    p->view = view; p->composition = composition;
    if (p->thread != GetCurrentThreadId() || view == NULL) return -1;
    if (!GetClientRect(p->host, &client)) return -1;
    if (p->occluded || IsIconic(p->host) || client.right <= 0 || client.bottom <= 0 ||
        boxrig_terminal_synchronized_output(boxrig_ui_view_terminal(view))) return 1;
    if (!ensure_device(p)) return -1;
    if (composition != NULL && boxrig_composition_get_view(composition, &transient) != BOXRIG_COMPOSITION_OK)
        return -1;
    if (!p->preview && !p->dirty && !boxrig_ui_view_frame_requested(view) &&
        p->composition_revision == transient.revision) return 0;
    if (boxrig_win32_renderer_get_effective_metrics(p->font, &metrics) != BOXRIG_FONT_CATALOG_OK ||
        boxrig_ui_view_get_transparency(view, &alpha) != BOXRIG_UI_OK) return -1;
    if (!p->validated) {
        boxrig_ui_transparency_capabilities capabilities;
        if (!validate_device(p, &metrics, &alpha.config)) return -1;
        adapter_transparency(p, &capabilities);
        if (boxrig_ui_view_report_transparency_capabilities(view, &capabilities) != BOXRIG_UI_OK ||
            boxrig_ui_view_get_transparency(view, &alpha) != BOXRIG_UI_OK) return -1;
    }
    composition_surface = alpha.effective == BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA;
    width = p->preview ? p->preview_width : p->width;
    height = p->preview ? p->preview_height : p->height;
    if (p->candidate.chain != NULL && (p->candidate.width != width || p->candidate.height != height ||
        p->candidate.opacity != alpha.effective_background_opacity ||
        p->candidate.composition != composition_surface ||
        (p->candidate.token_active && p->candidate.font_revision != metrics.layout_revision)))
        release_surface(p, &p->candidate);
    if (p->preview || p->tree_dirty || p->candidate.chain != NULL || p->committed.chain == NULL ||
        p->committed.width != width || p->committed.height != height ||
        p->committed.composition != composition_surface ||
        p->committed.font_revision != metrics.layout_revision ||
        p->committed.opacity != alpha.effective_background_opacity ||
        p->diagnostic.loss == BOXRIG_D3D11_LOSS_TARGET) {
        s = &p->candidate;
        if (s->chain == NULL && !create_surface(p, s, width, height,
                alpha.effective_background_opacity, composition_surface)) return -1;
    } else s = &p->committed;
    {
        HRESULT hr = boxrig_dcomp_chain_prepare(s->chain, p->x, p->y, p->visible);
        if (FAILED(hr)) {
            if (s == &p->candidate) release_surface(p, s);
            return failed(p, hr);
        }
    }
    if (s->token_active)
        return p->preview ? prepare_presentation(p, s) : present_surface(p, s);
    if (!ready(p, s)) return 1;
    if (!prepare_renderer(p, s, &metrics)) return -1;
    /* Drawing may fail after touching pixels. Do not reuse that buffer's old
     * completion record unless the whole presentation transaction succeeds. */
    if (s->buffer_index < 2) {
        s->buffers[s->buffer_index].complete_generation = 0;
        s->buffers[s->buffer_index].stale = (RECT){0, 0, (LONG)s->width, (LONG)s->height};
    }
    if (boxrig_d3d11_renderer_prepare_frame(s->renderer, view, composition,
            p->diagnostic.device_generation, s->target_generation, &s->token) != BOXRIG_RENDERER_OK) {
        boxrig_d3d11_diagnostic diagnostic;
        (void)boxrig_d3d11_renderer_get_diagnostic(s->renderer, &diagnostic);
        return failed(p, FAILED(diagnostic.result) ? diagnostic.result : E_FAIL);
    }
    s->token_active = true;
    p->composition_revision = transient.revision;
    return p->preview ? prepare_presentation(p, s) : present_surface(p, s);
}

static int adapter_resize(void *context, unsigned int width, unsigned int height)
{
    presentation *p = context;
    if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX) return -1;
    if (p->width != width || p->height != height) {
        p->occluded = false;
        invalidate_view(p);
    }
    p->width = width; p->height = height;
    return 0;
}

static int adapter_size(void *context, SIZE *size)
{
    presentation *p = context;
    if (p->committed.chain == NULL) return -1;
    size->cx = (LONG)p->committed.width; size->cy = (LONG)p->committed.height;
    return 0;
}

static int adapter_prepare(void *context, unsigned int width, unsigned int height)
{
    presentation *p = context;
    if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX) return -1;
    p->preview = true;
    p->preview_width = width; p->preview_height = height;
    return 0;
}

static int adapter_commit(void *context)
{
    presentation *p = context;
    if (!p->preview || p->preview_width != p->width || p->preview_height != p->height)
        return -1;
    if (!p->candidate.token_active && adapter_draw(p, p->view, p->composition) != 0) return -1;
    return present_surface(p, &p->candidate) == 0 ? 0 : -1;
}

static void adapter_abort(void *context)
{
    presentation *p = context;
    release_surface(p, &p->candidate);
    p->preview = false;
    invalidate_view(p);
}

static int adapter_paint(void *context)
{
    presentation *p = context;
    p->occluded = false;
    invalidate_view(p);
    return 0;
}

static int adapter_geometry(void *context, int x, int y, unsigned int width, unsigned int height)
{
    presentation *p = context;
    if (p->x != x || p->y != y) { p->tree_dirty = true; invalidate_view(p); }
    p->x = x; p->y = y;
    return adapter_resize(p, width, height);
}

static void adapter_visible(void *context, bool visible)
{
    presentation *p = context;
    if (p->visible == visible) return;
    p->visible = visible;
    p->occluded = false;
    p->tree_dirty = true;
    invalidate_view(p);
    if (!visible && p->device != NULL && p->committed.chain != NULL) {
        HRESULT hr = boxrig_dcomp_commit(p->device, p->committed.chain, p->x, p->y, false);
        if (FAILED(hr)) (void)failed(p, hr);
    }
}

static int adapter_scrollbar(void *context, bool enabled, unsigned int width)
{
    presentation *p = context;
    p->scrollbar = enabled; p->scrollbar_width = width;
    invalidate_view(p);
    return 0;
}

static int adapter_interaction(void *context, bool hovered, bool dragging)
{
    presentation *p = context;
    if (p->hovered != hovered || p->dragging != dragging) invalidate_view(p);
    p->hovered = hovered; p->dragging = dragging;
    return 0;
}

static void adapter_invalidate(void *context)
{
    presentation *p = context;
    p->occluded = false;
    p->invalid = true;
    invalidate_view(p);
}

static void adapter_direction(void *context, boxrig_text_direction_policy direction)
{
    presentation *p = context;
    (void)direction; /* The common component already updated its borrowed font state. */
    invalidate_view(p);
}

static LRESULT CALLBACK host_procedure(HWND window, UINT message,
    WPARAM word, LPARAM value, UINT_PTR id, DWORD_PTR data)
{
    presentation *p = (presentation *)data;
    if (message == WM_DISPLAYCHANGE || (message == WM_POWERBROADCAST &&
        (word == PBT_APMRESUMEAUTOMATIC || word == PBT_APMRESUMESUSPEND)))
        adapter_invalidate(p);
    else if (message == WM_SIZE || message == WM_SHOWWINDOW || message == WM_PAINT) {
        p->occluded = false;
        invalidate_view(p);
    }
    else if (message == WM_NCDESTROY) RemoveWindowSubclass(window, host_procedure, id);
    return DefSubclassProc(window, message, word, value);
}

static void adapter_destroy(void *context)
{
    presentation *p = context;
    if (p == NULL) return;
    if (IsWindow(p->host)) RemoveWindowSubclass(p->host, host_procedure, (UINT_PTR)p);
    release_device(p);
    free(p);
}

static void adapter_transparency(void *context, boxrig_ui_transparency_capabilities *out)
{
    presentation *p = context;
    memset(out, 0, sizeof(*out));
    out->premultiplied_argb32_target = p->alpha;
    if (p->alpha) {
        const LONG_PTR styles = GetWindowLongPtrW(p->host, GWL_EXSTYLE);
        out->supported_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
        /* The ordinary owner prepares native alpha independently. Preserve
         * the existing explicit no-redirection contract for external hosts. */
        if ((styles & WS_EX_NOREDIRECTIONBITMAP) && !(styles & WS_EX_LAYERED))
            out->supported_scopes |= BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
        out->active_scopes = out->supported_scopes;
    }
    p->diagnostic.view_surface_alpha = p->alpha;
    p->diagnostic.native_toplevel_alpha =
        (out->supported_scopes & BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL) != 0;
}

static HANDLE adapter_activity(void *context)
{
    presentation *p = context;
    return p->waiting == NULL ? NULL : boxrig_dcomp_chain_activity(p->waiting->chain);
}

static void adapter_dispatch_activity(void *context)
{
    presentation *p = context;
    activity_ready(p);
}

static const boxrig_win32_component_presentation operations = {
    adapter_resize, adapter_size, adapter_prepare, adapter_commit, adapter_abort,
    adapter_draw, adapter_paint, adapter_geometry, adapter_visible,
    adapter_scrollbar, adapter_interaction, adapter_invalidate, adapter_direction,
    adapter_destroy, adapter_transparency, adapter_activity, adapter_dispatch_activity
};

typedef struct adapter_config {
    const boxrig_win32_d3d11_config *graphics;
    const boxrig_ui_transparency_config *transparency;
} adapter_config;

static boxrig_ui_status create_adapter(HWND window, HWND input,
    boxrig_win32_renderer *font, void *config, void **out,
    const boxrig_win32_component_presentation **operations_out)
{
    const adapter_config *options = config;
    const boxrig_win32_d3d11_config *graphics = options->graphics;
    boxrig_font_effective_metrics metrics;
    presentation *p = calloc(1, sizeof(*p));
    (void)input;
    if (p == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    p->window = window; p->host = GetAncestor(window, GA_ROOT);
    p->thread = GetCurrentThreadId(); p->font = font;
    p->warp = graphics->warp_test_mode;
    p->width = 1; p->height = 1;
    p->scrollbar_width = 1;
    p->diagnostic.maximum_frame_latency = 1;
    p->diagnostic.warp_test_mode = p->warp;
    p->diagnostic.device_removal_recovery = true;
    /* Validate Direct2D and the initial target before reporting factory success,
     * so applications never receive a usable component from a device-only probe. */
    if (boxrig_win32_renderer_get_effective_metrics(font, &metrics) != BOXRIG_FONT_CATALOG_OK ||
        !ensure_device(p) || !validate_device(p, &metrics, options->transparency) ||
        !SetWindowSubclass(p->host, host_procedure, (UINT_PTR)p, (DWORD_PTR)p)) {
        if (graphics->creation_result != NULL) *graphics->creation_result = p->diagnostic.result;
        adapter_destroy(p);
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (graphics->creation_result != NULL) *graphics->creation_result = p->diagnostic.composition_result;
    *out = p; *operations_out = &operations;
    return BOXRIG_UI_OK;
}

boxrig_win32_d3d11_config boxrig_win32_d3d11_config_default(void)
{
    const boxrig_win32_d3d11_config result = {false, NULL};
    return result;
}

boxrig_ui_status boxrig_win32_d3d11_terminal_component_create_child(
    HINSTANCE instance, HWND parent, boxrig_ui_model *model,
    const boxrig_win32_terminal_component_config *config,
    const boxrig_win32_d3d11_config *graphics, boxrig_win32_terminal_component **out)
{
    adapter_config options;
    if (out != NULL) *out = NULL;
    if (graphics != NULL && graphics->creation_result != NULL) *graphics->creation_result = S_OK;
    if (config == NULL || graphics == NULL || out == NULL || !IsWindow(parent))
        return BOXRIG_UI_INVALID_ARGUMENT;
    options = (adapter_config){graphics, &config->transparency};
    /* Input remains on an ordinary persistent child. The presenter owns its
     * composition/opaque children without changing the application's root. */
    return boxrig_win32_terminal_component_create_child_with_renderer(instance, parent,
        model, config, 0, create_adapter, &options, out);
}

boxrig_ui_status boxrig_win32_d3d11_terminal_component_create_on_target(
    HINSTANCE instance, const boxrig_win32_terminal_borrowed_target *target,
    boxrig_ui_model *model, const boxrig_win32_terminal_component_config *config,
    const boxrig_win32_d3d11_config *graphics, boxrig_win32_terminal_component **out)
{
    adapter_config options;
    if (out != NULL) *out = NULL;
    if (graphics != NULL && graphics->creation_result != NULL) *graphics->creation_result = S_OK;
    if (config == NULL || graphics == NULL || target == NULL || out == NULL ||
        !IsWindow(target->presentation_window)) return BOXRIG_UI_INVALID_ARGUMENT;
    options = (adapter_config){graphics, &config->transparency};
    return boxrig_win32_terminal_component_create_on_target_with_renderer(instance, target,
        model, config, create_adapter, &options, out);
}

boxrig_ui_status boxrig_win32_d3d11_terminal_component_get_diagnostics(
    boxrig_win32_terminal_component *component, boxrig_win32_d3d11_diagnostics *out)
{
    presentation *p;
    if (out == NULL || !boxrig_win32_terminal_component_has_presentation(component, &operations))
        return BOXRIG_UI_INVALID_ARGUMENT;
    p = boxrig_win32_terminal_component_presentation_context(component);
    if (p->thread != GetCurrentThreadId()) return BOXRIG_UI_INVALID_ARGUMENT;
    *out = p->diagnostic;
    if (p->device != NULL) boxrig_dcomp_device_describe(p->device, out);
    out->commits += p->retired_commits;
    out->retained_swapchains = (p->committed.chain != NULL) +
        (p->candidate.chain != NULL) + (p->retired.chain != NULL);
    out->pending_presents = p->waiting != NULL;
    out->available = p->device != NULL && !p->invalid && p->validated;
    return BOXRIG_UI_OK;
}
