/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_X11_RENDERER_PRIVATE_H
#define BOXRIG_X11_RENDERER_PRIVATE_H

#include "boxrig/x11/renderer.h"
#include "../ui/renderer_lifecycle.h"

#include <fontconfig/fontconfig.h>

typedef struct boxrig_x11_renderer_font_candidate
    boxrig_x11_renderer_font_candidate;

typedef struct boxrig_x11_renderer_font_selection {
    char *family_list;
    const char **resource_paths;
    size_t resource_path_count;
} boxrig_x11_renderer_font_selection;

boxrig_font_catalog_status boxrig_x11_renderer_prepare_font(
    boxrig_x11_renderer *renderer,
    const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy,
    boxrig_x11_renderer_font_candidate **candidate_out);

boxrig_font_catalog_status boxrig_x11_renderer_font_candidate_metrics(
    const boxrig_x11_renderer_font_candidate *candidate,
    boxrig_font_effective_metrics *metrics_out);

boxrig_font_catalog_status boxrig_x11_renderer_commit_font(
    boxrig_x11_renderer *renderer,
    boxrig_x11_renderer_font_candidate *candidate);

void boxrig_x11_renderer_font_candidate_destroy(
    boxrig_x11_renderer_font_candidate *candidate);

boxrig_font_catalog_status boxrig_x11_renderer_resolve_font_selection(
    const boxrig_x11_renderer *renderer,
    const boxrig_font_specification *specification,
    boxrig_x11_renderer_font_selection *selection_out);

void boxrig_x11_renderer_font_selection_destroy(
    boxrig_x11_renderer_font_selection *selection);

/* Returns a borrowed configuration that remains valid with the renderer. */
FcConfig *boxrig_x11_renderer_font_config(
    const boxrig_x11_renderer *renderer);

/* Selects whether the component-local frame includes a scrollbar. */
int boxrig_x11_renderer_set_composited_scrollbar(
    boxrig_x11_renderer *renderer,
    bool enabled,
    unsigned int width);

/* Updates visual interaction state without changing the scrollbar model. */
int boxrig_x11_renderer_set_composited_scrollbar_interaction(
    boxrig_x11_renderer *renderer,
    bool hovered,
    bool dragging);

/* Renders a complete resize generation without copying it to the Window. */
int boxrig_x11_renderer_prepare_surface_candidate(
    boxrig_x11_renderer *renderer,
    unsigned int width,
    unsigned int height);

/* Copies a completed candidate after matching native allocation confirmation. */
int boxrig_x11_renderer_commit_surface_candidate(
    boxrig_x11_renderer *renderer);

/* Restores the last Window-visible surface after constrained negotiation. */
void boxrig_x11_renderer_abort_surface_candidate(
    boxrig_x11_renderer *renderer);

/* Connects the X11 software target to the common component contract. */
boxrig_renderer_status boxrig_x11_renderer_lifecycle_create(
    boxrig_x11_renderer *renderer,
    const boxrig_renderer_target *target,
    boxrig_renderer_lifecycle **lifecycle_out);

#endif
