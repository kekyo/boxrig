/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#include "boxrig/win32/opengl_component.h"
#include "component_private.h"
#include <commctrl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Win8 host style, also inspected when compiling the Win7-capable adapter. */
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

typedef struct framebuffer {
    GLuint framebuffer;
    GLuint texture;
    unsigned int width;
    unsigned int height;
} framebuffer;

typedef struct presentation {
    HWND parent;
    HWND host;
    HMONITOR monitor;
    DWORD thread;
    boxrig_win32_opengl_diagnostics diagnostics;
    boxrig_win32_renderer *font;
    boxrig_wgl_surface *surface;
    boxrig_ogl_renderer *renderer;
    framebuffer committed;
    framebuffer candidate;
    boxrig_renderer_lifecycle *lifecycle;
    boxrig_renderer_frame_token token;
    boxrig_ui_view *view;
    boxrig_composition *composition;
    uint64_t context_generation;
    uint64_t target_generation;
    uint64_t font_revision;
    uint64_t composition_revision;
    uint64_t swaps;
    unsigned int width;
    unsigned int height;
    unsigned int preview_width;
    unsigned int preview_height;
    int x;
    int y;
    unsigned int scrollbar_width;
    bool scrollbar;
    bool hovered;
    bool dragging;
    bool candidate_active;
    bool token_active;
    bool invalid;
    bool dirty;
    bool visible;
    bool swap_interval_set;
    bool direction_changed;
    bool desktop_only;
    boxrig_text_direction_policy direction;
    PFNGLGENFRAMEBUFFERSPROC gen_framebuffers;
    PFNGLDELETEFRAMEBUFFERSPROC delete_framebuffers;
    PFNGLBINDFRAMEBUFFERPROC bind_framebuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC framebuffer_texture;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC check_framebuffer;
    PFNGLBLITFRAMEBUFFERPROC blit_framebuffer;
    PFNGLGENTEXTURESPROC gen_textures;
    PFNGLDELETETEXTURESPROC delete_textures;
    PFNGLBINDTEXTUREPROC bind_texture;
    PFNGLTEXIMAGE2DPROC tex_image;
    PFNGLTEXPARAMETERIPROC tex_parameter;
    PFNGLDISABLEPROC disable;
    PFNGLREADBUFFERPROC read_buffer;
    PFNGLDRAWBUFFERSPROC draw_buffers;
    PFNGLGETERRORPROC get_error;
    PFNGLGETGRAPHICSRESETSTATUSARBPROC reset_status;
} presentation;

static void invalidate_view(presentation *p)
{
    boxrig_ui_dirty dirty = {0, SIZE_MAX, 0, SIZE_MAX};
    p->dirty = true;
    if (p->view != NULL)
        (void)boxrig_ui_view_invalidate(p->view, BOXRIG_UI_DIRTY_PRESENT, dirty);
}

static bool restore_context(HDC dc, HGLRC context, bool result)
{
    return wglMakeCurrent(dc, context) != FALSE && result;
}

static void delete_framebuffer(presentation *p, framebuffer *f)
{
    if (f->framebuffer != 0) p->delete_framebuffers(1, &f->framebuffer);
    if (f->texture != 0) p->delete_textures(1, &f->texture);
    memset(f, 0, sizeof(*f));
}

static bool create_framebuffer(presentation *p, unsigned int width,
    unsigned int height, framebuffer *out)
{
    framebuffer f = {0};
    if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX) return false;
    p->gen_textures(1, &f.texture);
    p->bind_texture(GL_TEXTURE_2D, f.texture);
    p->tex_parameter(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p->tex_parameter(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p->tex_image(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height,
        0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p->gen_framebuffers(1, &f.framebuffer);
    p->bind_framebuffer(GL_FRAMEBUFFER, f.framebuffer);
    p->framebuffer_texture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, f.texture, 0);
    if (p->check_framebuffer(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        p->get_error() != GL_NO_ERROR || f.texture == 0 || f.framebuffer == 0) {
        delete_framebuffer(p, &f);
        return false;
    }
    f.width = width; f.height = height;
    *out = f;
    return true;
}

static void abort_frame(presentation *p)
{
    if (p->token_active)
        (void)boxrig_renderer_lifecycle_abort_frame(p->lifecycle, &p->token);
    p->token_active = false;
    boxrig_renderer_lifecycle_destroy(p->lifecycle);
    p->lifecycle = NULL;
}

static void release_graphics(presentation *p, bool current)
{
    abort_frame(p);
    if (p->renderer != NULL)
        (void)boxrig_ogl_renderer_destroy(&p->renderer,
            current ? p->context_generation : p->context_generation + 1, NULL);
    if (current && p->delete_framebuffers != NULL) {
        delete_framebuffer(p, &p->candidate);
        delete_framebuffer(p, &p->committed);
    } else {
        memset(&p->candidate, 0, sizeof(p->candidate));
        memset(&p->committed, 0, sizeof(p->committed));
    }
    p->font_revision = 0;
}

static LRESULT CALLBACK child_procedure(HWND window, UINT message,
    WPARAM word, LPARAM value, UINT_PTR id, DWORD_PTR data)
{
    presentation *p = (presentation *)data;
    (void)id;
    switch (message) {
    case WM_NCHITTEST:
        /* Route native hit testing back to the existing same-thread input HWND. */
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        {
            PAINTSTRUCT paint;
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            invalidate_view(p);
            return 0;
        }
    case WM_DISPLAYCHANGE:
        p->invalid = true;
        invalidate_view(p);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, child_procedure, 1);
        break;
    }
    return DefSubclassProc(window, message, word, value);
}

static LRESULT CALLBACK parent_procedure(HWND window, UINT message,
    WPARAM word, LPARAM value, UINT_PTR id, DWORD_PTR data)
{
    presentation *p = (presentation *)data;
    if (message == WM_DISPLAYCHANGE ||
        (message == WM_POWERBROADCAST &&
         (word == PBT_APMRESUMEAUTOMATIC || word == PBT_APMRESUMESUSPEND ||
          word == PBT_APMRESUMECRITICAL))) {
        p->invalid = true;
        invalidate_view(p);
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, parent_procedure, id);
    }
    return DefSubclassProc(window, message, word, value);
}

static bool extension_present(const char *extensions, const char *name)
{
    const char *position = extensions;
    size_t length = strlen(name);
    if (position == NULL) return false;
    while ((position = strstr(position, name)) != NULL) {
        if ((position == extensions || position[-1] == ' ') &&
            (position[length] == ' ' || position[length] == '\0')) return true;
        position += length;
    }
    return false;
}

static bool create_graphics(presentation *p)
{
    boxrig_wgl_surface_config config;
    PFNWGLGETEXTENSIONSSTRINGARBPROC extensions = NULL;
    PFNWGLSWAPINTERVALEXTPROC interval = NULL;
    const char *names = NULL;
    config.parent = p->parent;
    config.width = p->width;
    config.height = p->height;
    config.context_generation = ++p->context_generation;
    p->invalid = true;
    p->reset_status = NULL;
    p->diagnostics.last_operation.status = p->desktop_only
        ? boxrig_wgl_surface_create_desktop(&config, &p->surface, &p->diagnostics.last_operation)
        : boxrig_wgl_surface_create(&config, &p->surface, &p->diagnostics.last_operation);
    if (p->diagnostics.last_operation.status != BOXRIG_GL_OK ||
        boxrig_wgl_surface_make_current(p->surface, &p->diagnostics.last_operation) != BOXRIG_GL_OK)
        return false;
    if (boxrig_wgl_surface_get_info(p->surface, &p->diagnostics.surface) != BOXRIG_GL_OK)
        return false;
#define LOAD(field, name) \
    if (!boxrig_wgl_copy_procedure(&p->field, sizeof(p->field), \
            boxrig_wgl_resolve_procedure(name))) return false
    LOAD(gen_framebuffers, "glGenFramebuffers");
    LOAD(delete_framebuffers, "glDeleteFramebuffers");
    LOAD(bind_framebuffer, "glBindFramebuffer");
    LOAD(framebuffer_texture, "glFramebufferTexture2D");
    LOAD(check_framebuffer, "glCheckFramebufferStatus");
    LOAD(blit_framebuffer, "glBlitFramebuffer");
    LOAD(gen_textures, "glGenTextures");
    LOAD(delete_textures, "glDeleteTextures");
    LOAD(bind_texture, "glBindTexture");
    LOAD(tex_image, "glTexImage2D");
    LOAD(tex_parameter, "glTexParameteri");
    LOAD(disable, "glDisable");
    LOAD(read_buffer, "glReadBuffer");
    LOAD(draw_buffers, "glDrawBuffers");
    LOAD(get_error, "glGetError");
#undef LOAD
    if (p->diagnostics.surface.gl.robustness)
        (void)boxrig_wgl_copy_procedure(&p->reset_status, sizeof(p->reset_status),
            boxrig_wgl_resolve_procedure(p->diagnostics.surface.gl.reset_status_entry_point));
    p->swap_interval_set = false;
    if (boxrig_wgl_copy_procedure(&extensions, sizeof(extensions),
            boxrig_wgl_resolve_procedure("wglGetExtensionsStringARB")))
        names = extensions(wglGetCurrentDC());
    if (extension_present(names, "WGL_EXT_swap_control")) {
        /* An ICD can advertise the optional extension yet reject this window.
         * Preserve the usable core context and report the effective setting. */
        p->swap_interval_set = boxrig_wgl_copy_procedure(&interval, sizeof(interval),
            boxrig_wgl_resolve_procedure("wglSwapIntervalEXT")) && interval(1);
    }
    if (!SetWindowSubclass(boxrig_wgl_surface_window(p->surface),
            child_procedure, 1, (DWORD_PTR)p)) return false;
    p->invalid = false;
    p->monitor = MonitorFromWindow(p->host, MONITOR_DEFAULTTONEAREST);
    invalidate_view(p);
    return true;
}

static bool retry_initial_desktop(presentation *p)
{
    if (p->desktop_only || p->diagnostics.accepted_frames != 0 ||
        !p->diagnostics.surface.gl.is_gles || p->surface == NULL) return false;
    /* Retry only initialization. Retain the common model, view and input HWND. */
    bool current = boxrig_wgl_surface_make_current(p->surface, NULL) == BOXRIG_GL_OK;
    release_graphics(p, current);
    if (boxrig_wgl_surface_destroy(&p->surface, NULL) != BOXRIG_GL_OK) return false;
    p->desktop_only = true;
    return create_graphics(p);
}

static bool ensure_current(presentation *p)
{
    HMONITOR monitor = MonitorFromWindow(p->host, MONITOR_DEFAULTTONEAREST);
    bool current = p->surface != NULL &&
        boxrig_wgl_surface_make_current(p->surface, &p->diagnostics.last_operation) == BOXRIG_GL_OK;
    if (monitor != p->monitor) p->invalid = true;
    if (current && p->reset_status != NULL && p->reset_status() != GL_NO_ERROR)
        p->invalid = true;
    if (current && !p->invalid) return true;
    /* Invalidate old tokens before releasing the context. No software fallback. */
    release_graphics(p, current && !p->invalid);
    if (p->surface != NULL &&
        boxrig_wgl_surface_destroy(&p->surface, NULL) != BOXRIG_GL_OK) return false;
    return create_graphics(p);
}

static boxrig_gl_target target_for(presentation *p, const framebuffer *f)
{
    boxrig_gl_target target;
    memset(&target, 0, sizeof(target));
    target.draw_framebuffer = f->framebuffer;
    target.width = f->width; target.height = f->height;
    target.generation = ++p->target_generation;
    target.format = BOXRIG_GL_TARGET_RGBA8;
    target.alpha_mode = p->diagnostics.surface.alpha_bits >= 8
        ? BOXRIG_GL_ALPHA_PREMULTIPLIED : BOXRIG_GL_ALPHA_OPAQUE;
    target.origin = BOXRIG_GL_TARGET_ORIGIN_LOWER_LEFT;
    return target;
}

static bool initialize_renderer(presentation *p, framebuffer *f)
{
    boxrig_font_effective_metrics metrics;
    boxrig_gl_target target = target_for(p, f);
    if (boxrig_win32_renderer_get_effective_metrics(p->font, &metrics) !=
            BOXRIG_FONT_CATALOG_OK) return false;
    if (p->renderer != NULL && p->font_revision != metrics.layout_revision) {
        abort_frame(p);
        (void)boxrig_ogl_renderer_destroy(&p->renderer, p->context_generation, NULL);
    }
    if (p->renderer == NULL) {
        boxrig_ogl_renderer_config config = boxrig_ogl_renderer_config_default();
        config.initial_target = target;
        config.direction_policy = p->direction;
        config.effective_metrics = &metrics;
        if (boxrig_wgl_surface_get_runtime(p->surface, target.generation,
                &config.runtime) != BOXRIG_GL_OK ||
            boxrig_ogl_renderer_create_with_win32_font(&config, p->font,
                &p->renderer, NULL) != BOXRIG_GL_OK) return false;
        p->font_revision = metrics.layout_revision;
    } else if (boxrig_ogl_renderer_set_target(p->renderer, p->context_generation,
                   &target, NULL) != BOXRIG_GL_OK) return false;
    return true;
}

static bool prepare_renderer(presentation *p, framebuffer *f)
{
    boxrig_ui_transparency_state alpha;
    if (!initialize_renderer(p, f)) return false;
    if (boxrig_ui_view_get_transparency(p->view, &alpha) != BOXRIG_UI_OK ||
        alpha.effective == BOXRIG_UI_TRANSPARENCY_UNAVAILABLE ||
        boxrig_ogl_renderer_apply_transparency_state(p->renderer, &alpha) != 0)
        return false;
    return boxrig_ogl_renderer_set_composited_scrollbar(p->renderer,
               p->scrollbar, p->scrollbar_width) == 0 &&
        boxrig_ogl_renderer_set_composited_scrollbar_interaction(p->renderer,
            p->hovered, p->dragging) == 0;
}

static bool present_framebuffer(presentation *p, const framebuffer *f)
{
    HWND window = boxrig_wgl_surface_window(p->surface);
    RECT client;
    if (f->framebuffer == 0 || !GetClientRect(p->parent, &client) ||
        client.right <= 0 || client.bottom <= 0 || IsIconic(GetAncestor(p->parent, GA_ROOT))) return false;
    if (!SetWindowPos(window, HWND_TOP, p->x, p->y, (int)f->width,
            (int)f->height, SWP_NOACTIVATE)) return false;
    /* Both framebuffers are single-sample RGBA8; blitting never involves CPU pixels. */
    p->disable(GL_SCISSOR_TEST);
    /* ES linear windows need no optional sRGB write-control extension. */
    if (!p->diagnostics.surface.gl.is_gles || p->diagnostics.surface.srgb)
        p->disable(GL_FRAMEBUFFER_SRGB);
    p->bind_framebuffer(GL_READ_FRAMEBUFFER, f->framebuffer);
    p->read_buffer(GL_COLOR_ATTACHMENT0);
    p->bind_framebuffer(GL_DRAW_FRAMEBUFFER, 0);
    GLenum back = p->diagnostics.surface.gl.is_gles ? GL_BACK : GL_BACK_LEFT;
    p->draw_buffers(1, &back);
    p->blit_framebuffer(0, 0, (GLint)f->width, (GLint)f->height,
        0, 0, (GLint)f->width, (GLint)f->height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (p->get_error() != GL_NO_ERROR ||
        boxrig_wgl_surface_swap(p->surface, &p->diagnostics.last_operation) != BOXRIG_GL_OK) {
        p->invalid = true;
        invalidate_view(p);
        return false;
    }
    p->swaps++;
    ShowWindow(window, p->visible ? SW_SHOWNA : SW_HIDE);
    return true;
}

static bool commit_current_candidate(presentation *p)
{
    framebuffer previous;
    if (!p->token_active || p->candidate.width != p->width ||
        p->candidate.height != p->height ||
        !present_framebuffer(p, &p->candidate)) return false;
    if (boxrig_renderer_lifecycle_accept_frame(p->lifecycle, &p->token) !=
            BOXRIG_RENDERER_OK) {
        p->invalid = true;
        invalidate_view(p);
        return false;
    }
    p->token_active = false;
    p->diagnostics.accepted_frames++;
    boxrig_renderer_lifecycle_destroy(p->lifecycle);
    p->lifecycle = NULL;
    previous = p->committed;
    p->committed = p->candidate;
    memset(&p->candidate, 0, sizeof(p->candidate));
    delete_framebuffer(p, &previous);
    p->candidate_active = false;
    p->dirty = false;
    return true;
}

static int adapter_resize(void *context, unsigned int width, unsigned int height)
{
    presentation *p = context;
    if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX) return -1;
    if (p->width != width || p->height != height) invalidate_view(p);
    p->width = width; p->height = height;
    return 0;
}

static int adapter_size(void *context, SIZE *size)
{
    presentation *p = context;
    if (p->committed.framebuffer == 0) return -1;
    size->cx = (LONG)p->committed.width;
    size->cy = (LONG)p->committed.height;
    return 0;
}

static int adapter_prepare(void *context, unsigned int width, unsigned int height)
{
    presentation *p = context;
    HDC previous_dc = wglGetCurrentDC();
    HGLRC previous_context = wglGetCurrentContext();
    bool result = !p->candidate_active && ensure_current(p);
    if (result) {
        abort_frame(p);
        delete_framebuffer(p, &p->candidate);
        result = create_framebuffer(p, width, height, &p->candidate);
        if (!result && retry_initial_desktop(p))
            result = create_framebuffer(p, width, height, &p->candidate);
        p->candidate_active = result;
        p->preview_width = width;
        p->preview_height = height;
    }
    return restore_context(previous_dc, previous_context, result) ? 0 : -1;
}

/* The surface reports only its own framebuffer. The application's host
 * coordinator owns DWM and any whole-window alpha on the ordinary root. */
static void adapter_transparency(void *context,
    boxrig_ui_transparency_capabilities *out)
{
    presentation *p = context;
    memset(out, 0, sizeof(*out));
    out->premultiplied_argb32_target = p->diagnostics.surface.alpha_bits >= 8;
    if (out->premultiplied_argb32_target)
        out->supported_scopes = out->active_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
}

static int adapter_draw(void *context, boxrig_ui_view *view,
    boxrig_composition *composition);

static int adapter_commit(void *context)
{
    presentation *p = context;
    HDC previous_dc = wglGetCurrentDC();
    HGLRC previous_context = wglGetCurrentContext();
    bool result = ensure_current(p);
    /* A lost context invalidates GPU tokens, but not the pending size command.
     * Rebuild its preview using the common component's still-owned view/input. */
    if (result && p->candidate_active && !p->token_active)
        result = adapter_draw(p, p->view, p->composition) == 0;
    if (result) result = commit_current_candidate(p);
    return restore_context(previous_dc, previous_context, result) ? 0 : -1;
}

static void adapter_abort(void *context)
{
    presentation *p = context;
    HDC previous_dc = wglGetCurrentDC();
    HGLRC previous_context = wglGetCurrentContext();
    bool current = ensure_current(p);
    abort_frame(p);
    if (current) delete_framebuffer(p, &p->candidate);
    p->candidate_active = false;
    invalidate_view(p);
    (void)restore_context(previous_dc, previous_context, current);
}

static int adapter_draw(void *context, boxrig_ui_view *view,
    boxrig_composition *composition)
{
    presentation *p = context;
    HDC previous_dc = wglGetCurrentDC();
    HGLRC previous_context = wglGetCurrentContext();
    boxrig_renderer_target target;
    boxrig_composition_view composition_view;
    RECT host_client;
    bool result = false;
    bool deferred = false;
    bool preview = p->candidate_active;
    p->view = view;
    p->composition = composition;
    if (boxrig_composition_get_view(composition, &composition_view) != BOXRIG_COMPOSITION_OK)
        return -1;
    if (!GetClientRect(p->host, &host_client)) return -1;
    /* Retain dirty while native messages finish restoring the host. Reporting
     * success here would request immediate frames and starve those messages. */
    if (IsIconic(p->host) || host_client.right <= 0 || host_client.bottom <= 0) return 1;
    if (!ensure_current(p)) goto done;
prepare:
    {
        boxrig_ui_transparency_capabilities capabilities;
        adapter_transparency(p, &capabilities);
        /* Context recreation can select a different pixel format. Publish its
         * real capability before resolving the retained view policy. */
        if (boxrig_ui_view_report_transparency_capabilities(view, &capabilities) != BOXRIG_UI_OK)
            goto done;
    }
    if (p->direction_changed) {
        abort_frame(p);
        if (p->renderer != NULL)
            (void)boxrig_ogl_renderer_destroy(&p->renderer, p->context_generation, NULL);
        p->direction_changed = false;
    }
    if (p->token_active) { result = true; goto done; }
    if (!preview && !p->dirty && !boxrig_ui_view_frame_requested(view) &&
        p->composition_revision == composition_view.revision) {
        result = true;
        goto done;
    }
    if (!preview && (p->candidate.width != p->width || p->candidate.height != p->height)) {
        delete_framebuffer(p, &p->candidate);
    }
    if (p->candidate.framebuffer == 0 &&
        !create_framebuffer(p, preview ? p->preview_width : p->width,
            preview ? p->preview_height : p->height, &p->candidate)) {
        if (retry_initial_desktop(p)) goto prepare;
        goto done;
    }
    if (!prepare_renderer(p, &p->candidate)) {
        if (retry_initial_desktop(p)) goto prepare;
        goto done;
    }
    memset(&target, 0, sizeof(target));
    target.generation = p->target_generation;
    target.width = p->candidate.width; target.height = p->candidate.height;
    target.scale_numerator = 1; target.scale_denominator = 1;
    target.premultiplied_alpha = p->diagnostics.surface.alpha_bits >= 8;
    if (boxrig_ogl_renderer_lifecycle_create(p->renderer, &target, &p->lifecycle) !=
            BOXRIG_RENDERER_OK ||
        boxrig_renderer_lifecycle_prepare_frame(p->lifecycle, view, composition,
            target.generation, &p->token) != BOXRIG_RENDERER_OK) goto done;
    p->token_active = true;
    result = preview || commit_current_candidate(p);
    if (result) p->composition_revision = composition_view.revision;
done:
    if (!result) {
        /* Native calls can minimize the host after the initial extent check.
         * Retain this unaccepted frame for restoration instead of failing the
         * component because presentation became temporarily unavailable. */
        deferred = GetClientRect(p->host, &host_client) &&
            (IsIconic(p->host) || host_client.right <= 0 || host_client.bottom <= 0);
        abort_frame(p);
        invalidate_view(p);
    }
    if (!restore_context(previous_dc, previous_context, result || deferred)) return -1;
    return deferred ? 1 : 0;
}

static int adapter_paint(void *context)
{
    presentation *p = context;
    /* A paint request schedules retained presentation through the frame loop. */
    invalidate_view(p);
    return 0;
}

static int adapter_geometry(void *context, int x, int y,
    unsigned int width, unsigned int height)
{
    presentation *p = context;
    p->x = x; p->y = y;
    return adapter_resize(p, width, height);
}

static void adapter_visible(void *context, bool visible)
{
    presentation *p = context;
    p->visible = visible;
    if (p->surface != NULL)
        ShowWindow(boxrig_wgl_surface_window(p->surface),
            visible && p->committed.framebuffer != 0 ? SW_SHOWNA : SW_HIDE);
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
    p->invalid = true;
    invalidate_view(p);
}

static void adapter_direction(void *context, boxrig_text_direction_policy direction)
{
    presentation *p = context;
    p->direction = direction;
    p->direction_changed = true;
    invalidate_view(p);
}

static void adapter_destroy(void *context)
{
    presentation *p = context;
    HDC previous_dc = wglGetCurrentDC();
    HGLRC previous_context = wglGetCurrentContext();
    bool current;
    if (p == NULL) return;
    RemoveWindowSubclass(p->host, parent_procedure, (UINT_PTR)p);
    current = p->surface != NULL &&
        boxrig_wgl_surface_make_current(p->surface, NULL) == BOXRIG_GL_OK;
    release_graphics(p, current);
    if (p->surface != NULL) (void)boxrig_wgl_surface_destroy(&p->surface, NULL);
    (void)restore_context(previous_dc, previous_context, true);
    free(p);
}

static const boxrig_win32_component_presentation operations = {
    adapter_resize, adapter_size, adapter_prepare, adapter_commit, adapter_abort,
    adapter_draw, adapter_paint, adapter_geometry, adapter_visible,
    adapter_scrollbar, adapter_interaction, adapter_invalidate, adapter_direction, adapter_destroy,
    adapter_transparency, NULL, NULL
};

static boxrig_ui_status create_adapter(HWND parent, HWND input,
    boxrig_win32_renderer *font, void *context, void **out,
    const boxrig_win32_component_presentation **operations_out)
{
    presentation *p = calloc(1, sizeof(*p));
    HDC previous_dc = wglGetCurrentDC();
    HGLRC previous_context = wglGetCurrentContext();
    bool result;
    (void)context;
    (void)input;
    if (p == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    p->parent = parent; p->host = GetAncestor(parent, GA_ROOT); p->font = font;
    p->thread = GetCurrentThreadId();
    p->width = 1; p->height = 1;
    result = create_graphics(p);
    if (!result) result = retry_initial_desktop(p);
    /* Validate the actual font renderer before publishing the selected API or
     * returning success to a caller that may fall back to native rendering. */
    if (result) {
        result = create_framebuffer(p, 1, 1, &p->candidate) &&
            initialize_renderer(p, &p->candidate);
        if (!result && retry_initial_desktop(p))
            result = create_framebuffer(p, 1, 1, &p->candidate) &&
                initialize_renderer(p, &p->candidate);
    }
    result = result && SetWindowSubclass(p->host, parent_procedure, (UINT_PTR)p, (DWORD_PTR)p);
    result = restore_context(previous_dc, previous_context, result);
    if (!result) { adapter_destroy(p); return BOXRIG_UI_UNAVAILABLE; }
    *out = p; *operations_out = &operations;
    return BOXRIG_UI_OK;
}

static bool ordinary_parent(HWND parent)
{
    HWND root = GetAncestor(parent, GA_ROOT);
    return !(GetWindowLongPtrW(root, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP);
}

boxrig_ui_status boxrig_win32_opengl_terminal_component_create_child(
    HINSTANCE instance, HWND parent, boxrig_ui_model *model,
    const boxrig_win32_terminal_component_config *config,
    boxrig_win32_terminal_component **out)
{
    if (out != NULL) *out = NULL;
    if (config == NULL || out == NULL || parent == NULL || !IsWindow(parent))
        return BOXRIG_UI_INVALID_ARGUMENT;
    if (!ordinary_parent(parent)) return BOXRIG_UI_UNAVAILABLE;
    return boxrig_win32_terminal_component_create_child_with_renderer(
        instance, parent, model, config, 0, create_adapter, NULL, out);
}

boxrig_ui_status boxrig_win32_opengl_terminal_component_create_on_target(
    HINSTANCE instance, const boxrig_win32_terminal_borrowed_target *target,
    boxrig_ui_model *model, const boxrig_win32_terminal_component_config *config,
    boxrig_win32_terminal_component **out)
{
    if (out != NULL) *out = NULL;
    if (config == NULL || out == NULL || target == NULL ||
        target->presentation_window == NULL || !IsWindow(target->presentation_window))
        return BOXRIG_UI_INVALID_ARGUMENT;
    if (!ordinary_parent(target->presentation_window)) return BOXRIG_UI_UNAVAILABLE;
    return boxrig_win32_terminal_component_create_on_target_with_renderer(
        instance, target, model, config, create_adapter, NULL, out);
}

bool boxrig_win32_ogl_observe(boxrig_win32_terminal_component *component,
    boxrig_win32_ogl_observation *out)
{
    presentation *p = boxrig_win32_terminal_component_presentation_context(component);
    if (p == NULL || out == NULL || p->surface == NULL ||
        boxrig_wgl_surface_make_current(p->surface, NULL) != BOXRIG_GL_OK) return false;
    out->window = boxrig_wgl_surface_window(p->surface);
    out->framebuffer = p->committed.framebuffer;
    out->width = p->committed.width; out->height = p->committed.height;
    out->context_generation = p->context_generation;
    out->target_generation = p->target_generation;
    out->swaps = p->swaps;
    out->swap_interval_set = p->swap_interval_set;
    return true;
}

boxrig_ui_status boxrig_win32_opengl_terminal_component_get_diagnostics(
    boxrig_win32_terminal_component *component,
    boxrig_win32_opengl_diagnostics *out)
{
    presentation *p = boxrig_win32_terminal_component_presentation_context(component);
    if (p == NULL || out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (p->thread != GetCurrentThreadId()) return BOXRIG_UI_INVALID_ARGUMENT;
    *out = p->diagnostics;
    out->available = p->surface != NULL && !p->invalid;
    out->target_generation = p->target_generation;
    out->swap_interval_set = p->swap_interval_set;
    return BOXRIG_UI_OK;
}
