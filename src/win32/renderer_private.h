/* Internal system boundary used by Win32 renderer tests. */

#ifndef BOXRIG_WIN32_RENDERER_PRIVATE_H
#define BOXRIG_WIN32_RENDERER_PRIVATE_H

#include "boxrig/win32/renderer.h"

#include <dwrite_2.h>
#include <stdbool.h>

/** Identifies a non-exhaustive software renderer diagnostic interval. */
typedef enum boxrig_win32_renderer_phase {
    BOXRIG_WIN32_RENDERER_PHASE_UPDATE,
    BOXRIG_WIN32_RENDERER_PHASE_MODEL_ROW,
    BOXRIG_WIN32_RENDERER_PHASE_TEXT_RASTER,
    BOXRIG_WIN32_RENDERER_PHASE_IMAGE_COMPOSITE,
    BOXRIG_WIN32_RENDERER_PHASE_COPY_PRESENT,
    BOXRIG_WIN32_RENDERER_PHASE_COUNT
} boxrig_win32_renderer_phase;

/** Classifies an existing text-raster interval without adding clock reads. */
typedef enum boxrig_win32_raster_kind {
    BOXRIG_WIN32_RASTER_SHAPED_RUN,
    BOXRIG_WIN32_RASTER_DECORATION
} boxrig_win32_raster_kind;

/** Classifies one row-cache lookup without including terminal text. */
typedef enum boxrig_win32_row_cache_reason {
    BOXRIG_WIN32_ROW_CACHE_HIT,
    BOXRIG_WIN32_ROW_CACHE_INVALID,
    BOXRIG_WIN32_ROW_CACHE_LAYOUT,
    BOXRIG_WIN32_ROW_CACHE_DIRECTION,
    BOXRIG_WIN32_ROW_CACHE_CONTENT
} boxrig_win32_row_cache_reason;

/** Captures the signatures compared for one renderer row. */
typedef struct boxrig_win32_row_cache_comparison {
    /** Zero-based terminal row number. */
    size_t row;
    /** Signature retained by the cache before this comparison. */
    uint64_t cached_signature;
    /** Signature calculated from the current terminal row. */
    uint64_t content_signature;
    /** Match or first failing cache condition. */
    boxrig_win32_row_cache_reason reason;
} boxrig_win32_row_cache_comparison;

/** Borrows callbacks for intervals on the calling renderer thread. */
typedef struct boxrig_win32_renderer_phase_observer {
    /** Context owned by the caller for the lifetime of the binding. */
    void *context;
    /** Called immediately before a measured operation. */
    void (*begin)(void *context, boxrig_win32_renderer_phase phase);
    /** Called after the operation, including its failure path. */
    void (*end)(void *context, boxrig_win32_renderer_phase phase);
    /** Copies update-owned work counters before the update interval ends. */
    void (*work_snapshot)(
        void *context, const boxrig_win32_render_statistics *statistics);
    /** Supplies the next text-raster interval's kind and shaped glyph count. */
    void (*raster_kind)(
        void *context, boxrig_win32_raster_kind kind, size_t glyph_count);
    /** Borrows one row-cache decision before any cache replacement. */
    void (*row_cache_comparison)(
        void *context, const boxrig_win32_row_cache_comparison *comparison);
} boxrig_win32_renderer_phase_observer;

/** Binds or clears a borrowed observer without changing renderer ownership. */
void boxrig_win32_renderer_set_phase_observer(
    boxrig_win32_renderer *renderer,
    const boxrig_win32_renderer_phase_observer *observer);

/** Enables row signature and cache outcome counters for an explicit diagnostic. */
void boxrig_win32_renderer_enable_work_statistics(
    boxrig_win32_renderer *renderer, bool enabled);

typedef struct boxrig_win32_renderer_system {
    HRESULT (*create_bitmap_render_target)(
        IDWriteGdiInterop *interop,
        HDC destination_context,
        UINT32 width,
        UINT32 height,
        IDWriteBitmapRenderTarget **target_out);
    HRESULT (*query_bitmap_render_target1)(
        IDWriteBitmapRenderTarget *target,
        IDWriteBitmapRenderTarget1 **target_out);
    HBITMAP(WINAPI *create_dib_section)(
        HDC device_context,
        const BITMAPINFO *information,
        UINT usage,
        void **pixels_out,
        HANDLE section,
        DWORD offset);
    HMODULE(WINAPI *load_library)(LPCWSTR path);
    FARPROC(WINAPI *get_proc_address)(HMODULE module, LPCSTR name);
    BOOL(WINAPI *free_library)(HMODULE module);
    HRESULT (*query_factory2)(
        IDWriteFactory *factory,
        IDWriteFactory2 **factory_out);
} boxrig_win32_renderer_system;

/* Reflects the selected bitmap interface and accessible raster surface. */
bool boxrig_win32_renderer_supports_alpha(
    const boxrig_win32_renderer *renderer);

/* Returns the production operations so private fixture drivers can omit APIs. */
boxrig_win32_renderer_system boxrig_win32_renderer_system_default(void);

typedef struct boxrig_win32_renderer_font_candidate
    boxrig_win32_renderer_font_candidate;

int boxrig_win32_renderer_create_with_system(
    HDC reference_context,
    const boxrig_win32_renderer_system *system,
    boxrig_win32_renderer **renderer_out);

boxrig_font_catalog_status boxrig_win32_renderer_prepare_font(
    boxrig_win32_renderer *renderer,
    const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy,
    boxrig_win32_renderer_font_candidate **candidate_out);

boxrig_font_catalog_status boxrig_win32_renderer_font_candidate_metrics(
    const boxrig_win32_renderer_font_candidate *candidate,
    boxrig_font_effective_metrics *metrics_out);

boxrig_font_catalog_status boxrig_win32_renderer_commit_font(
    boxrig_win32_renderer *renderer,
    boxrig_win32_renderer_font_candidate *candidate);

void boxrig_win32_renderer_font_candidate_destroy(
    boxrig_win32_renderer_font_candidate *candidate);

/* Renders a complete resize generation without exposing it to WM_PAINT. */
int boxrig_win32_renderer_prepare_surface_candidate(
    boxrig_win32_renderer *renderer,
    unsigned int width,
    unsigned int height);

/* Makes a successfully rendered surface candidate paint-visible. */
int boxrig_win32_renderer_commit_surface_candidate(
    boxrig_win32_renderer *renderer);

/* Restores the last paint-visible surface after constrained negotiation. */
void boxrig_win32_renderer_abort_surface_candidate(
    boxrig_win32_renderer *renderer);

#endif
