/* boxrig - Under MIT. */
#ifndef BOXRIG_UI_COLOR_PRIVATE_H
#define BOXRIG_UI_COLOR_PRIVATE_H

#include "boxrig/ui/view.h"

typedef struct boxrig_ui_color_statistics {
    uint64_t result_lookups;
    uint64_t result_hits;
    uint64_t callback_calls;
    uint64_t paint_lookups;
    uint64_t paint_hits;
    size_t result_cache_bytes;
    size_t paint_bytes;
} boxrig_ui_color_statistics;

typedef struct boxrig_ui_color_cache boxrig_ui_color_cache;

boxrig_ui_color_cache *boxrig_ui_color_cache_create(void);
void boxrig_ui_color_cache_destroy(boxrig_ui_color_cache *cache);
void boxrig_ui_color_cache_clear(boxrig_ui_color_cache *cache);
boxrig_ui_status boxrig_ui_color_cache_resolve(
    boxrig_ui_color_cache *cache,
    boxrig_ui_color_filter callback,
    void *context,
    const boxrig_ui_color_filter_input *input,
    boxrig_ui_color_filter_result *result);

typedef struct boxrig_ui_color_paints boxrig_ui_color_paints;

/* Allocation is a preparation transaction; failure leaves the output intact. */
boxrig_ui_status boxrig_ui_color_paints_create(
    size_t rows, size_t columns, size_t maximum_bytes,
    boxrig_ui_color_paints **paints_out);
void boxrig_ui_color_paints_destroy(boxrig_ui_color_paints *paints);
void boxrig_ui_color_paints_clear(boxrig_ui_color_paints *paints);
boxrig_ui_status boxrig_ui_color_paints_resolve(
    boxrig_ui_color_paints *paints, boxrig_ui_color_cache *cache,
    boxrig_ui_color_filter callback, void *context,
    size_t row, size_t column, const boxrig_ui_color_filter_input *input,
    boxrig_ui_color_filter_result *result);

/* Optional original-cell colors beneath a translucent selection. Allocated
 * only when used, and charged to the same retained-color memory bound. */
boxrig_ui_status boxrig_ui_color_paints_resolve_base(
    boxrig_ui_color_paints *paints, boxrig_ui_color_cache *cache,
    boxrig_ui_color_filter callback, void *context,
    size_t row, size_t column, const boxrig_ui_color_filter_input *input,
    boxrig_ui_color_filter_result *result);

/* Private diagnostics for workload measurements; not callback-count promises. */
void boxrig_ui_color_statistics_read(const boxrig_ui_color_cache *cache,
    const boxrig_ui_color_paints *paints, boxrig_ui_color_statistics *statistics);
void boxrig_ui_color_statistics_reset(boxrig_ui_color_cache *cache,
    boxrig_ui_color_paints *paints);

#endif
