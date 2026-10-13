/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_PANGO_CAIRO_RENDERER_PRIVATE_H
#define BOXRIG_PANGO_CAIRO_RENDERER_PRIVATE_H

#include "renderer.h"
#include "../ui/renderer_lifecycle.h"

typedef struct boxrig_pango_cairo_renderer_font_candidate
    boxrig_pango_cairo_renderer_font_candidate;

typedef struct boxrig_pango_cairo_renderer_font_selection {
    char *family_list;
    const char **resource_paths;
    size_t resource_path_count;
} boxrig_pango_cairo_renderer_font_selection;

boxrig_font_catalog_status boxrig_pango_cairo_renderer_prepare_font(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy,
    boxrig_pango_cairo_renderer_font_candidate **candidate_out);

boxrig_font_catalog_status boxrig_pango_cairo_renderer_font_candidate_metrics(
    const boxrig_pango_cairo_renderer_font_candidate *candidate,
    boxrig_font_effective_metrics *metrics_out);

boxrig_font_catalog_status boxrig_pango_cairo_renderer_commit_font(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_pango_cairo_renderer_font_candidate *candidate);

void boxrig_pango_cairo_renderer_font_candidate_destroy(
    boxrig_pango_cairo_renderer_font_candidate *candidate);

boxrig_font_catalog_status boxrig_pango_cairo_renderer_resolve_font_selection(
    const boxrig_pango_cairo_renderer *renderer,
    const boxrig_font_specification *specification,
    boxrig_pango_cairo_renderer_font_selection *selection_out);

void boxrig_pango_cairo_renderer_font_selection_destroy(
    boxrig_pango_cairo_renderer_font_selection *selection);

/* Selects whether the component-local frame includes a scrollbar. */
int boxrig_pango_cairo_renderer_set_composited_scrollbar(
    boxrig_pango_cairo_renderer *renderer,
    bool enabled,
    unsigned int width);

/* Updates visual interaction state without changing the scrollbar model. */
int boxrig_pango_cairo_renderer_set_composited_scrollbar_interaction(
    boxrig_pango_cairo_renderer *renderer,
    bool hovered,
    bool dragging);

/* Renders a complete resize generation without replacing the retained frame. */
int boxrig_pango_cairo_renderer_prepare_surface_candidate(
    boxrig_pango_cairo_renderer *renderer,
    unsigned int width,
    unsigned int height);

/* Commits a completed candidate after frontend allocation confirmation. */
int boxrig_pango_cairo_renderer_commit_surface_candidate(
    boxrig_pango_cairo_renderer *renderer);

/* Restores the last retained surface after constrained negotiation. */
void boxrig_pango_cairo_renderer_abort_surface_candidate(
    boxrig_pango_cairo_renderer *renderer);

/* Connects the retained software target to the common renderer-only contract. */
boxrig_renderer_status boxrig_pango_cairo_renderer_lifecycle_create(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_renderer_target *target,
    boxrig_renderer_lifecycle **lifecycle_out);

#endif
