/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_RENDERER_LIFECYCLE_H
#define BOXRIG_UI_RENDERER_LIFECYCLE_H

#include "boxrig/ui.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Result of a renderer lifecycle operation. */
typedef enum boxrig_renderer_status {
    /** The operation completed. */
    BOXRIG_RENDERER_OK = 0,
    /** An argument or configuration value is invalid. */
    BOXRIG_RENDERER_INVALID_ARGUMENT,
    /** Required memory could not be allocated. */
    BOXRIG_RENDERER_OUT_OF_MEMORY,
    /** The configured bounded-work limit was exceeded. */
    BOXRIG_RENDERER_LIMIT_EXCEEDED,
    /** The backend or target is unavailable. */
    BOXRIG_RENDERER_UNAVAILABLE,
    /** A frame or target transaction is already active. */
    BOXRIG_RENDERER_BUSY,
    /** A token or generation no longer identifies the active transaction. */
    BOXRIG_RENDERER_STALE
} boxrig_renderer_status;

/** Selects whether presentation is caller-managed or component-integrated. */
typedef enum boxrig_renderer_integration {
    /** The caller prepares and accepts renderer-only frames. */
    BOXRIG_RENDERER_INTEGRATION_RENDERER_ONLY = 0,
    /** A native component prepares, presents, and accepts frames. */
    BOXRIG_RENDERER_INTEGRATION_COMPONENT
} boxrig_renderer_integration;

/** Selects ownership of the graphics runtime bound to the lifecycle. */
typedef enum boxrig_renderer_runtime_ownership {
    /** The lifecycle borrows and does not destroy the graphics runtime. */
    BOXRIG_RENDERER_RUNTIME_BORROWED = 0,
    /** The lifecycle destroys the graphics runtime after resources. */
    BOXRIG_RENDERER_RUNTIME_MANAGED
} boxrig_renderer_runtime_ownership;

/** Selects which integration layer performs native presentation. */
typedef enum boxrig_renderer_presentation_ownership {
    /** The renderer-only caller presents after frame preparation. */
    BOXRIG_RENDERER_PRESENTATION_CALLER = 0,
    /** The native adapter presents before accepting the frame. */
    BOXRIG_RENDERER_PRESENTATION_ADAPTER
} boxrig_renderer_presentation_ownership;

/** Deterministic lifecycle failure point used by conformance tests. */
typedef enum boxrig_renderer_failure_point {
    /** Do not inject a failure. */
    BOXRIG_RENDERER_FAILURE_NONE = 0,
    /** Fail the next frame preparation. */
    BOXRIG_RENDERER_FAILURE_PREPARE_FRAME,
    /** Fail the next backend frame finalization. */
    BOXRIG_RENDERER_FAILURE_FINISH_FRAME,
    /** Fail the next adapter presentation. */
    BOXRIG_RENDERER_FAILURE_PRESENT_FRAME,
    /** Fail the next target preparation. */
    BOXRIG_RENDERER_FAILURE_PREPARE_TARGET,
    /** Fail the next target commit. */
    BOXRIG_RENDERER_FAILURE_COMMIT_TARGET
} boxrig_renderer_failure_point;

/** Immutable renderer target properties associated with one generation. */
typedef struct boxrig_renderer_target {
    /** Monotonic target generation selected by its owner. */
    uint64_t generation;
    /** Physical target width in pixels. */
    unsigned int width;
    /** Physical target height in pixels. */
    unsigned int height;
    /** Scale numerator used to convert logical coordinates to pixels. */
    unsigned int scale_numerator;
    /** Nonzero scale denominator. */
    unsigned int scale_denominator;
    /** Effective boxrig_ui_transparency_scope bit set. */
    uint32_t transparency_scopes;
    /** Whether target colors use premultiplied alpha. */
    bool premultiplied_alpha;
} boxrig_renderer_target;

/** Bounded work performed while preparing and finishing one frame. */
typedef struct boxrig_renderer_frame_work {
    /** Bytes uploaded during the frame. */
    size_t upload_bytes;
    /** Dirty rectangles consumed during the frame. */
    size_t dirty_rectangle_count;
    /** Draw operations submitted during the frame. */
    size_t draw_count;
    /** Backend submissions performed during the frame. */
    size_t submission_count;
    /** Live cache entries after frame preparation. */
    size_t cache_entry_count;
    /** Cache hits during the frame. */
    size_t cache_hit_count;
    /** Cache entries evicted during the frame. */
    size_t eviction_count;
    /** Resources rebuilt during the frame. */
    size_t rebuild_count;
} boxrig_renderer_frame_work;

/** Upper bounds enforced for one renderer lifecycle. */
typedef struct boxrig_renderer_limits {
    /** Maximum bytes uploaded by one frame. */
    size_t maximum_upload_bytes;
    /** Maximum dirty rectangles consumed by one frame. */
    size_t maximum_dirty_rectangle_count;
    /** Maximum draw operations submitted by one frame. */
    size_t maximum_draw_count;
    /** Maximum backend submissions performed by one frame. */
    size_t maximum_submission_count;
    /** Maximum live cache entries reported by a frame. */
    size_t maximum_cache_entry_count;
} boxrig_renderer_limits;

/** Cumulative lifecycle counters for diagnostics and conformance tests. */
typedef struct boxrig_renderer_counters {
    /** Frame preparation attempts. */
    size_t frame_prepare_count;
    /** Frames accepted after successful presentation. */
    size_t frame_accept_count;
    /** Frame transactions aborted before acceptance. */
    size_t frame_abort_count;
    /** Target preparation attempts. */
    size_t target_prepare_count;
    /** Target transactions committed. */
    size_t target_commit_count;
    /** Target transactions aborted. */
    size_t target_abort_count;
    /** Accepted target-loss notifications. */
    size_t target_loss_count;
    /** Backend or injected operation failures. */
    size_t failure_count;
    /** Cumulative uploaded bytes. */
    size_t upload_bytes;
    /** Cumulative consumed dirty rectangles. */
    size_t dirty_rectangle_count;
    /** Cumulative draw operations. */
    size_t draw_count;
    /** Cumulative backend submissions. */
    size_t submission_count;
    /** Cumulative cache hits. */
    size_t cache_hit_count;
    /** Cumulative cache evictions. */
    size_t eviction_count;
    /** Cumulative resource rebuilds. */
    size_t rebuild_count;
} boxrig_renderer_counters;

/** Backend callbacks implementing renderer work and target transitions. */
typedef struct boxrig_renderer_backend_operations {
    /** Prepares renderer work for the active target. */
    int (*prepare_frame)(
        void *context,
        boxrig_ui_view *view,
        boxrig_composition *composition,
        const boxrig_renderer_target *target,
        boxrig_renderer_frame_work *work_out);
    /** Finalizes renderer work before presentation. */
    int (*finish_frame)(
        void *context,
        boxrig_renderer_frame_work *work_in_out);
    /** Presents a component-owned frame. */
    int (*present_frame)(
        void *context,
        boxrig_renderer_frame_work *work_in_out);
    /** Rolls back the active frame transaction. */
    void (*abort_frame)(void *context);
    /** Prepares resources for a candidate target. */
    int (*prepare_target)(
        void *context,
        const boxrig_renderer_target *target);
    /** Commits the prepared target. */
    int (*commit_target)(void *context);
    /** Rolls back the prepared target. */
    void (*abort_target)(void *context);
    /** Invalidates resources belonging to the lost target. */
    void (*target_lost)(void *context);
    /** Releases renderer resources while the runtime remains valid. */
    void (*release_resources)(void *context);
    /** Destroys a managed graphics runtime. */
    void (*destroy_runtime)(void *context);
    /**
     * Optional analyzer owned by the backend renderer, borrowed by the lifecycle.
     * The renderer closes it before freeing its text-analysis context. Returning
     * a stable handle also supports temporary renderer-only lifecycle wrappers.
     * Omitting this operation preserves an analyzer registered by the host;
     * returning OK with NULL explicitly detaches it.
     */
    boxrig_ui_status (*fragment_layout_provider)(void *context,
        boxrig_ui_fragment_layout_provider **provider_out);
} boxrig_renderer_backend_operations;

/** Construction values for the common renderer transaction lifecycle. */
typedef struct boxrig_renderer_lifecycle_config {
    /** Renderer-only or component integration mode. */
    boxrig_renderer_integration integration;
    /** Borrowed or managed runtime ownership. */
    boxrig_renderer_runtime_ownership runtime_ownership;
    /** Caller or adapter presentation ownership. */
    boxrig_renderer_presentation_ownership presentation_ownership;
    /** Initially committed render target. */
    boxrig_renderer_target initial_target;
    /** Per-frame and cache work bounds. */
    boxrig_renderer_limits limits;
    /** Opaque context passed to every backend callback. */
    void *backend_context;
    /** Backend implementation callbacks copied during creation. */
    boxrig_renderer_backend_operations operations;
} boxrig_renderer_lifecycle_config;

/** Identity returned for one active frame transaction. */
typedef struct boxrig_renderer_frame_token {
    /** Monotonic frame serial assigned by the lifecycle. */
    uint64_t serial;
    /** Target generation against which the frame was prepared. */
    uint64_t target_generation;
} boxrig_renderer_frame_token;

/** Opaque renderer transaction coordinator. */
typedef struct boxrig_renderer_lifecycle boxrig_renderer_lifecycle;

/**
 * Returns bounded renderer-only lifecycle defaults.
 *
 * @return A configuration that still requires backend callbacks and a target.
 */
boxrig_renderer_lifecycle_config boxrig_renderer_lifecycle_config_default(
    void);

/**
 * Creates a lifecycle over a renderer backend.
 *
 * @param config Complete lifecycle and callback configuration.
 * @param lifecycle_out Receives the created lifecycle on success.
 * @return A renderer lifecycle status.
 */
boxrig_renderer_status boxrig_renderer_lifecycle_create(
    const boxrig_renderer_lifecycle_config *config,
    boxrig_renderer_lifecycle **lifecycle_out);

/**
 * Releases renderer resources and then a managed runtime, if configured.
 *
 * @param lifecycle Lifecycle to destroy, or NULL.
 */
void boxrig_renderer_lifecycle_destroy(
    boxrig_renderer_lifecycle *lifecycle);

/**
 * Prepares one frame transaction without accepting its pixels.
 *
 * @param lifecycle Lifecycle to use.
 * @param view View whose current dirty is captured transactionally.
 * @param composition Optional transient composition overlay.
 * @param target_generation Expected committed target generation.
 * @param token_out Receives the active transaction identity.
 * @return A renderer lifecycle status.
 */
boxrig_renderer_status boxrig_renderer_lifecycle_prepare_frame(
    boxrig_renderer_lifecycle *lifecycle,
    boxrig_ui_view *view,
    boxrig_composition *composition,
    uint64_t target_generation,
    boxrig_renderer_frame_token *token_out);

/**
 * Accepts prepared pixels after successful caller or adapter presentation.
 *
 * @param lifecycle Lifecycle with an active frame transaction.
 * @param token Token returned by the matching prepare operation.
 * @return A renderer lifecycle status.
 */
boxrig_renderer_status boxrig_renderer_lifecycle_accept_frame(
    boxrig_renderer_lifecycle *lifecycle,
    const boxrig_renderer_frame_token *token);

/**
 * Aborts a prepared frame and preserves its captured view dirty.
 *
 * @param lifecycle Lifecycle with an active frame transaction.
 * @param token Token returned by the matching prepare operation.
 * @return A renderer lifecycle status.
 */
boxrig_renderer_status boxrig_renderer_lifecycle_abort_frame(
    boxrig_renderer_lifecycle *lifecycle,
    const boxrig_renderer_frame_token *token);

/**
 * Prepares a replacement target without changing the committed target.
 *
 * @param lifecycle Lifecycle whose target will be replaced.
 * @param candidate Monotonically newer target to prepare.
 * @return A renderer lifecycle status.
 */
boxrig_renderer_status boxrig_renderer_lifecycle_prepare_target(
    boxrig_renderer_lifecycle *lifecycle,
    const boxrig_renderer_target *candidate);

/**
 * Commits the replacement target prepared by the preceding prepare call.
 *
 * @param lifecycle Lifecycle with an active target transaction.
 * @return A renderer lifecycle status.
 */
boxrig_renderer_status boxrig_renderer_lifecycle_commit_target(
    boxrig_renderer_lifecycle *lifecycle);

/**
 * Aborts the replacement target prepared by the preceding prepare call.
 *
 * @param lifecycle Lifecycle with an active target transaction.
 * @return A renderer lifecycle status.
 */
boxrig_renderer_status boxrig_renderer_lifecycle_abort_target(
    boxrig_renderer_lifecycle *lifecycle);

/**
 * Invalidates accepted pixels after asynchronous target loss.
 *
 * @param lifecycle Lifecycle to invalidate.
 * @param view View to mark for a complete redraw.
 * @param target_generation Generation that was lost.
 * @return A renderer lifecycle status.
 */
boxrig_renderer_status boxrig_renderer_lifecycle_target_lost(
    boxrig_renderer_lifecycle *lifecycle,
    boxrig_ui_view *view,
    uint64_t target_generation);

/**
 * Configures one deterministic failure consumed by the next matching call.
 *
 * @param lifecycle Lifecycle whose next matching operation will fail.
 * @param point Failure point to arm, or BOXRIG_RENDERER_FAILURE_NONE.
 */
void boxrig_renderer_lifecycle_fail_next(
    boxrig_renderer_lifecycle *lifecycle,
    boxrig_renderer_failure_point point);

/**
 * Copies cumulative transaction and bounded-work counters.
 *
 * @param lifecycle Lifecycle to inspect.
 * @param counters_out Receives the current cumulative counters.
 * @return A renderer lifecycle status.
 */
boxrig_renderer_status boxrig_renderer_lifecycle_get_counters(
    const boxrig_renderer_lifecycle *lifecycle,
    boxrig_renderer_counters *counters_out);

#ifdef __cplusplus
}
#endif

#endif
