/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_RENDER_OPENGL_RUNTIME_H
#define BOXRIG_RENDER_OPENGL_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** An opaque function pointer returned by a caller-owned OpenGL resolver. */
typedef void (*boxrig_gl_function)(void);

/** The current context and thread identity reported by the caller. */
typedef struct boxrig_gl_current_context {
    /** Nonzero identity of the context currently bound by the caller. */
    uintptr_t identity;
    /** Nonzero identity of the thread on which the context is current. */
    uint64_t thread_identity;
} boxrig_gl_current_context;

/**
 * Resolves an OpenGL entry point for the current context.
 *
 * @param context Caller-defined resolver context.
 * @param name NUL-terminated OpenGL entry-point name.
 * @return The resolved function, or NULL when unavailable.
 */
typedef boxrig_gl_function (*boxrig_gl_resolver)(
    void *context,
    const char *name);

/**
 * Reports the context and thread that are current at the call site.
 *
 * @param context Caller-defined query context.
 * @param current_out Receives nonzero context and thread identities.
 * @return true when a usable current context was reported.
 */
typedef bool (*boxrig_gl_current_context_query)(
    void *context,
    boxrig_gl_current_context *current_out);

/** Result codes for the borrowed-context OpenGL runtime contract. */
typedef enum boxrig_gl_status {
    /** The operation completed. */
    BOXRIG_GL_OK = 0,
    /** An argument or configuration value is invalid. */
    BOXRIG_GL_INVALID_ARGUMENT,
    /** OpenGL 3.3 core, OpenGL ES 3.0, or a required entry point is unavailable. */
    BOXRIG_GL_UNAVAILABLE,
    /** The current context or owner thread does not match the runtime. */
    BOXRIG_GL_CONTEXT_MISMATCH,
    /** A context or target generation is stale. */
    BOXRIG_GL_STALE_GENERATION,
    /** The caller-owned framebuffer is incomplete. */
    BOXRIG_GL_TARGET_INCOMPLETE,
    /** The target does not match the active frame transaction. */
    BOXRIG_GL_TARGET_MISMATCH,
    /** A required shader could not be compiled or linked. */
    BOXRIG_GL_SHADER_FAILURE,
    /** An OpenGL operation reported an error. */
    BOXRIG_GL_API_FAILURE,
    /** A target transaction is already active. */
    BOXRIG_GL_BUSY,
    /** Required memory or graphics storage could not be allocated. */
    BOXRIG_GL_OUT_OF_MEMORY
} boxrig_gl_status;

/** Color storage accepted for a borrowed OpenGL target. */
typedef enum boxrig_gl_target_format {
    /** Linear 8-bit RGBA storage. */
    BOXRIG_GL_TARGET_RGBA8 = 0,
    /** sRGB 8-bit color with linear 8-bit alpha storage. */
    BOXRIG_GL_TARGET_SRGB8_ALPHA8,
    /** Linear 8-bit RGB storage without alpha; requires opaque output and a nonzero framebuffer. */
    BOXRIG_GL_TARGET_RGB8
} boxrig_gl_target_format;

/** Alpha semantics requested by the caller. */
typedef enum boxrig_gl_alpha_mode {
    /** Every output pixel is treated as fully opaque. */
    BOXRIG_GL_ALPHA_OPAQUE = 0,
    /** RGB output is premultiplied by its alpha value. */
    BOXRIG_GL_ALPHA_PREMULTIPLIED
} boxrig_gl_alpha_mode;

/** Coordinate origin exposed by the target. */
typedef enum boxrig_gl_target_origin {
    /** OpenGL lower-left framebuffer origin. */
    BOXRIG_GL_TARGET_ORIGIN_LOWER_LEFT = 0
} boxrig_gl_target_origin;

/** Caller-owned framebuffer and its immutable frame properties. */
typedef struct boxrig_gl_target {
    /** Caller-owned draw framebuffer name; zero selects the default target. */
    uint32_t draw_framebuffer;
    /** Target width in physical pixels. */
    uint32_t width;
    /** Target height in physical pixels. */
    uint32_t height;
    /** Nonzero monotonic generation assigned by the caller. */
    uint64_t generation;
    /** Required color attachment storage. */
    boxrig_gl_target_format format;
    /** Alpha interpretation required for rendered output. */
    boxrig_gl_alpha_mode alpha_mode;
    /** Coordinate origin exposed by the framebuffer. */
    boxrig_gl_target_origin origin;
    /** Sample count; the initial renderer contract accepts zero only. */
    uint32_t samples;
} boxrig_gl_target;

/** A rectangle expressed from the top-left of a boxrig view. */
typedef struct boxrig_gl_clip {
    /** Left edge in top-left-origin target pixels. */
    uint32_t x;
    /** Top edge in top-left-origin target pixels. */
    uint32_t y;
    /** Rectangle width in pixels. */
    uint32_t width;
    /** Rectangle height in pixels. */
    uint32_t height;
} boxrig_gl_clip;

/** Fixed-size diagnostics that never transfer allocation ownership. */
typedef struct boxrig_gl_diagnostic {
    /** Portable status corresponding to the diagnostic. */
    boxrig_gl_status status;
    /** OpenGL error code, or zero when no GL error was reported. */
    uint32_t gl_code;
    /** Graphics reset status, or zero when robustness is unavailable. */
    uint32_t reset_status;
    /** NUL-terminated bounded diagnostic text. */
    char message[512];
} boxrig_gl_diagnostic;

/** Capabilities established once while a context is current. */
typedef struct boxrig_gl_capabilities {
    /** Current OpenGL or OpenGL ES major version. */
    uint32_t major_version;
    /** Current OpenGL or OpenGL ES minor version. */
    uint32_t minor_version;
    /** Maximum sample count reported by the implementation. */
    uint32_t maximum_samples;
    /** Whether the current context uses OpenGL ES rather than desktop OpenGL. */
    bool is_gles;
    /** Whether the current desktop context has a core profile; false for ES. */
    bool core_profile;
    /** Whether sRGB framebuffer targets are supported. */
    bool srgb_target;
    /** Whether graphics reset status can be queried. */
    bool robustness;
    /** Whether core/KHR debug output and its log query are available; not enabled. */
    bool debug_output;
    /** NUL-terminated vendor text, truncated to fit; never used for admission. */
    char vendor[128];
    /** NUL-terminated renderer text, truncated to fit. */
    char renderer[256];
    /** NUL-terminated driver version text, truncated to fit. */
    char version[128];
    /** Supported reset query name, or an empty string when unavailable. */
    char reset_status_entry_point[32];
} boxrig_gl_capabilities;

/** State accounting for evaluating the cost of the borrowed-state contract. */
typedef struct boxrig_gl_state_counters {
    /** OpenGL state queries issued by the runtime. */
    size_t query_count;
    /** Caller state values temporarily changed by the runtime. */
    size_t change_count;
    /** Caller state values restored by the runtime. */
    size_t restore_count;
} boxrig_gl_state_counters;

/** Construction values supplied by a window-system adapter. */
typedef struct boxrig_gl_runtime_config {
    /** Opaque context passed to the OpenGL resolver. */
    void *resolver_context;
    /** Resolver for every required OpenGL entry point. */
    boxrig_gl_resolver resolve;
    /** Opaque context passed to the current-context query. */
    void *current_context_query_context;
    /** Query proving the caller made the same context current. */
    boxrig_gl_current_context_query query_current_context;
    /** Nonzero generation of the caller-owned current context. */
    uint64_t context_generation;
    /** Nonzero generation of the initial caller-owned target. */
    uint64_t target_generation;
} boxrig_gl_runtime_config;

/** A runtime bound to one context, one thread, and one context generation. */
typedef struct boxrig_gl_runtime boxrig_gl_runtime;

/** Renderer-owned texture storage that does not expose OpenGL typedefs. */
typedef enum boxrig_gl_texture_format {
    /** One-channel glyph coverage. */
    BOXRIG_GL_TEXTURE_COVERAGE8 = 0,
    /** Linear premultiplied 8-bit RGBA. */
    BOXRIG_GL_TEXTURE_PREMULTIPLIED_RGBA8
} boxrig_gl_texture_format;

/** One instanced quad in normalized target coordinates. */
typedef struct boxrig_gl_quad_instance {
    /** Normalized destination rectangle as left, top, right, bottom. */
    float destination[4];
    /** Normalized texture rectangle as left, top, right, bottom. */
    float texture_coordinates[4];
    /** Premultiplied RGBA tint. */
    float color[4];
    /** Numeric boxrig_gl_quad_source value consumed by the shader. */
    float source_kind;
} boxrig_gl_quad_instance;

/** Source interpretation selected by the terminal composition batch. */
typedef enum boxrig_gl_quad_source {
    /** Ignore texture storage and emit the instance color. */
    BOXRIG_GL_QUAD_SOLID = 0,
    /** Multiply glyph coverage by the instance tint. */
    BOXRIG_GL_QUAD_COVERAGE,
    /** Multiply premultiplied RGBA texture data by the instance tint. */
    BOXRIG_GL_QUAD_PREMULTIPLIED_RGBA
} boxrig_gl_quad_source;

/** Opaque renderer-owned synchronization point. */
typedef struct boxrig_gl_fence {
    /** Private synchronization handle owned by the runtime. */
    void *handle;
} boxrig_gl_fence;

/**
 * Creates a runtime and validates the required OpenGL 3.3 / OpenGL ES 3.0 entry points.
 *
 * @param config Current-context callbacks and initial generations.
 * @param runtime_out Receives the created runtime.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return A borrowed-context OpenGL status.
 */
boxrig_gl_status boxrig_gl_runtime_create(
    const boxrig_gl_runtime_config *config,
    boxrig_gl_runtime **runtime_out,
    boxrig_gl_diagnostic *diagnostic_out);

/**
 * Returns the capabilities fixed at runtime creation.
 *
 * @param runtime Runtime to inspect.
 * @param capabilities_out Receives the immutable capability snapshot.
 * @return A borrowed-context OpenGL status.
 */
boxrig_gl_status boxrig_gl_runtime_get_capabilities(
    const boxrig_gl_runtime *runtime,
    boxrig_gl_capabilities *capabilities_out);

/**
 * Advances the accepted borrowed-target generation.
 *
 * @param runtime Runtime to update.
 * @param context_generation Current caller-owned context generation.
 * @param target_generation New nonzero monotonic target generation.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return A borrowed-context OpenGL status.
 */
boxrig_gl_status boxrig_gl_runtime_set_target_generation(
    boxrig_gl_runtime *runtime,
    uint64_t context_generation,
    uint64_t target_generation,
    boxrig_gl_diagnostic *diagnostic_out);

/**
 * Saves caller state and binds a validated borrowed target.
 *
 * The matching end call is required after success. Failure restores all state
 * before returning.
 *
 * @param runtime Runtime to activate.
 * @param context_generation Current caller-owned context generation.
 * @param target Complete caller-owned framebuffer description.
 * @param clip Top-left-origin clip, or NULL for the complete target.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return A borrowed-context OpenGL status.
 */
boxrig_gl_status boxrig_gl_runtime_begin_target(
    boxrig_gl_runtime *runtime,
    uint64_t context_generation,
    const boxrig_gl_target *target,
    const boxrig_gl_clip *clip,
    boxrig_gl_diagnostic *diagnostic_out);

/**
 * Restores the caller state saved by the matching begin call.
 *
 * @param runtime Runtime with an active target transaction.
 * @param context_generation Current caller-owned context generation.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return A borrowed-context OpenGL status.
 */
boxrig_gl_status boxrig_gl_runtime_end_target(
    boxrig_gl_runtime *runtime,
    uint64_t context_generation,
    boxrig_gl_diagnostic *diagnostic_out);

/**
 * Allocates immutable-size renderer texture storage while a target is active.
 *
 * @param runtime Runtime with an active target transaction.
 * @param width Texture width in pixels.
 * @param height Texture height in pixels.
 * @param format Renderer texture format.
 * @param texture_out Receives the renderer-owned texture name.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return A borrowed-context OpenGL status.
 */
boxrig_gl_status boxrig_gl_runtime_create_texture(
    boxrig_gl_runtime *runtime,
    uint32_t width,
    uint32_t height,
    boxrig_gl_texture_format format,
    uint32_t *texture_out,
    boxrig_gl_diagnostic *diagnostic_out);

/**
 * Uploads one bounded top-left-origin rectangle while a target is active.
 *
 * @param runtime Runtime with an active target transaction.
 * @param texture Renderer-owned destination texture.
 * @param format Destination texture format.
 * @param x Left destination coordinate.
 * @param y Top destination coordinate.
 * @param width Rectangle width in pixels.
 * @param height Rectangle height in pixels.
 * @param bytes Tightly packed source bytes.
 * @param byte_count Exact source byte count.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return A borrowed-context OpenGL status.
 */
boxrig_gl_status boxrig_gl_runtime_upload_texture(
    boxrig_gl_runtime *runtime,
    uint32_t texture,
    boxrig_gl_texture_format format,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    const uint8_t *bytes,
    size_t byte_count,
    boxrig_gl_diagnostic *diagnostic_out);

/** Blend operation for a renderer batch. */
typedef enum boxrig_gl_blend {
    /** Source-over: source plus destination times inverse source alpha. */
    BOXRIG_GL_BLEND_OVER = 0,
    /** Replace every covered destination pixel. */
    BOXRIG_GL_BLEND_REPLACE = 1,
    /** Destination-over: source behind the already rendered terminal. */
    BOXRIG_GL_BLEND_BEHIND = 2
} boxrig_gl_blend;

/**
 * Draws one consecutive solid or texture batch into the active target.
 *
 * @param runtime Runtime with an active target transaction.
 * @param texture Renderer texture, or zero for a solid batch.
 * @param instances Consecutive normalized quad instances.
 * @param instance_count Number of entries in instances.
 * @param blend Premultiplied compositing operation.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return A borrowed-context OpenGL status.
 */
boxrig_gl_status boxrig_gl_runtime_draw_quads(
    boxrig_gl_runtime *runtime,
    uint32_t texture,
    const boxrig_gl_quad_instance *instances,
    size_t instance_count,
    boxrig_gl_blend blend,
    boxrig_gl_diagnostic *diagnostic_out);

/**
 * Deletes one renderer texture without changing caller-owned bindings.
 *
 * @param runtime Runtime that owns the texture.
 * @param context_generation Current caller-owned context generation.
 * @param texture Texture name to clear after deletion.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return A borrowed-context OpenGL status.
 */
boxrig_gl_status boxrig_gl_runtime_delete_texture(
    boxrig_gl_runtime *runtime,
    uint64_t context_generation,
    uint32_t *texture,
    boxrig_gl_diagnostic *diagnostic_out);

/**
 * Inserts a nonblocking resource-lifetime fence after queued renderer work.
 *
 * @param runtime Runtime owning the queued work.
 * @param context_generation Current caller-owned context generation.
 * @param fence_out Receives the opaque synchronization point.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return A borrowed-context OpenGL status.
 */
boxrig_gl_status boxrig_gl_runtime_insert_fence(
    boxrig_gl_runtime *runtime,
    uint64_t context_generation,
    boxrig_gl_fence *fence_out,
    boxrig_gl_diagnostic *diagnostic_out);

/**
 * Checks one fence once with a zero timeout and never waits or polls.
 *
 * @param runtime Runtime owning the fence.
 * @param context_generation Current caller-owned context generation.
 * @param fence Fence to query.
 * @param signaled_out Receives whether submitted work has completed.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return A borrowed-context OpenGL status.
 */
boxrig_gl_status boxrig_gl_runtime_poll_fence(
    boxrig_gl_runtime *runtime,
    uint64_t context_generation,
    boxrig_gl_fence *fence,
    bool *signaled_out,
    boxrig_gl_diagnostic *diagnostic_out);

/**
 * Releases a fence; NULL handles are accepted.
 *
 * @param runtime Runtime owning the fence.
 * @param context_generation Current caller-owned context generation.
 * @param fence Fence handle to clear after deletion.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return A borrowed-context OpenGL status.
 */
boxrig_gl_status boxrig_gl_runtime_delete_fence(
    boxrig_gl_runtime *runtime,
    uint64_t context_generation,
    boxrig_gl_fence *fence,
    boxrig_gl_diagnostic *diagnostic_out);

/**
 * Returns cumulative state query, change, and restoration counts.
 *
 * @param runtime Runtime to inspect.
 * @param counters_out Receives cumulative state accounting.
 * @return A borrowed-context OpenGL status.
 */
boxrig_gl_status boxrig_gl_runtime_get_state_counters(
    const boxrig_gl_runtime *runtime,
    boxrig_gl_state_counters *counters_out);

/**
 * Releases owned OpenGL objects and the runtime.
 *
 * If the bound context is no longer current, CPU state is released and the
 * context owner remains responsible for discarding its old share group.
 *
 * @param runtime Runtime pointer to clear after release.
 * @param context_generation Current caller-owned context generation.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return A borrowed-context OpenGL status.
 */
boxrig_gl_status boxrig_gl_runtime_destroy(
    boxrig_gl_runtime **runtime,
    uint64_t context_generation,
    boxrig_gl_diagnostic *diagnostic_out);

#ifdef __cplusplus
}
#endif

#endif
